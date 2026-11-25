/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-contribution.cpp
 * @brief Reconcile Avahi contribution operations with the record model.
 */
#include "dmn-discovery-avahi-contribution.hpp"
#include "dmn-discovery-records.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <utility>

namespace dmn::detail {
namespace {
constexpr std::array attempts{DiscoveryAttempt::kSrv, DiscoveryAttempt::kTxt,
                              DiscoveryAttempt::kIpv4, DiscoveryAttempt::kIpv6};
DiscoveryRecordKind kind(std::size_t i) {
  return i == 0 ? DiscoveryRecordKind::kSrv : DiscoveryRecordKind::kTxt;
}
DiscoveryAddressFamily family(std::size_t i) {
  return i == 2 ? DiscoveryAddressFamily::kIpv4 : DiscoveryAddressFamily::kIpv6;
}
DiscoveryError stateError(const char *message) {
  return {DiscoveryErrorKind::kInvalidArgument, message};
}
} // namespace

DiscoveryAvahiContribution::DiscoveryAvahiContribution(
    DiscoverySessionHandle session, std::uint64_t epoch,
    DiscoveryServiceKey key, DiscoveryContribution contribution,
    std::uint64_t source, DiscoveryOptions options, AvahiClient *client,
    std::shared_ptr<DiscoveryAvahiRecordApi> records,
    std::shared_ptr<DiscoveryAvahiResolverApi> resolvers,
    std::array<bool, 2> addressFamilies)
    : m_session{std::move(session)}, m_epoch{epoch}, m_key{std::move(key)},
      m_contribution{contribution}, m_source{source}, m_options{options},
      m_client{client}, m_record_api{std::move(records)},
      m_resolver_api{std::move(resolvers)},
      m_address_families{addressFamilies} {}

DiscoveryAvahiContribution::~DiscoveryAvahiContribution() {
  if (!m_cleaned) {
    static_cast<void>(cleanup());
  }
}

DiscoveryResult DiscoveryAvahiContribution::recordsUpdate(
    std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update) {
  return m_session.updateRecords(m_epoch, m_key, m_contribution, m_source,
                                 std::move(update));
}

void DiscoveryAvahiContribution::failed(std::size_t i, DiscoveryError error) {
  if (!m_operations[i].m_failure) {
    m_operations[i].m_failure = std::move(error);
  }
}

DiscoveryResult DiscoveryAvahiContribution::release(Operation &op,
                                                    bool modelActive) {
  op.m_watch.reset();
  op.m_resolver.reset();
  if (op.m_slot) {
    const auto slot{*op.m_slot};
    op.m_slot.reset();
    if (modelActive) {
      return m_session.finishNative(m_epoch, slot);
    }
  }
  return {};
}

DiscoveryResult DiscoveryAvahiContribution::startWatch(std::size_t i) {
  auto &op{m_operations[i]};
  auto slot{m_session.beginNative(m_epoch)};
  if (!slot) {
    return std::unexpected{slot.error()};
  }
  op.m_slot = *slot;
  auto activated{
      recordsUpdate([&](DiscoveryRecords &records, std::uint64_t source) {
        if (!records.setMonitoring(source, kind(i), true)) {
          return DiscoveryResult{std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                             "record watch generation exhausted"}}};
        }
        op.m_token = *records.monitorGeneration(source, kind(i));
        return DiscoveryResult{};
      })};
  if (!activated) {
    static_cast<void>(release(op));
    return activated;
  }
  op.m_watch = std::make_unique<DiscoveryAvahiRecordWatch>(
      m_record_api, m_client, m_key, m_contribution, kind(i), m_options,
      [this, i, token = op.m_token](DiscoveryAvahiRecordEvent event) {
        if (m_invalidated) {
          return DiscoveryResult{};
        }
        return recordsUpdate([&](DiscoveryRecords &records,
                                 std::uint64_t source) {
          if (records.monitorGeneration(source, kind(i)) != token) {
            return DiscoveryResult{};
          }
          if (i == 0) {
            const auto &srv{std::get<DiscoverySrv>(event.m_record)};
            if (event.m_added) {
              return records.addSrv(source, token, srv);
            }
            static_cast<void>(records.removeSrv(source, token, srv));
          } else {
            auto wire{
                std::get<std::vector<std::uint8_t>>(std::move(event.m_record))};
            if (event.m_added) {
              return records.addTxt(source, token, std::move(wire));
            }
            static_cast<void>(
                records.removeTxt(source, token, std::move(wire)));
          }
          return DiscoveryResult{};
        });
      },
      [this, i](DiscoveryError error) {
        if (!m_invalidated)
          failed(i, std::move(error));
      });
  auto result{op.m_watch->start()};
  if (!result) {
    failed(i, result.error());
  } else {
    op.m_success = true;
  }
  return result;
}

DiscoveryResult DiscoveryAvahiContribution::startResolver(
    std::size_t i, std::uint64_t resolution, std::uint64_t token) {
  auto &op{m_operations[i]};
  auto slot{m_session.beginNative(m_epoch)};
  if (!slot) {
    return std::unexpected{slot.error()};
  }
  op.m_slot = *slot;
  op.m_token = token;
  op.m_resolution = resolution;
  op.m_resolver = std::make_unique<DiscoveryAvahiServiceResolver>(
      m_resolver_api, m_client, m_key, m_contribution, family(i),
      [this, i, token, resolution](DiscoveryAvahiResolverEvent event) {
        if (m_invalidated) {
          return DiscoveryResult{};
        }
        bool accepted{false};
        auto result{
            recordsUpdate([&](DiscoveryRecords &records, std::uint64_t source) {
              const auto current{records.snapshot()};
              if (current.m_generation != resolution ||
                  records.addressGeneration(source, family(i)) != token ||
                  (current.m_state != DiscoveryRecordState::kResolving &&
                   current.m_state != DiscoveryRecordState::kReady)) {
                return DiscoveryResult{};
              }
              auto actual{canonicalizeDomain(event.m_host)};
              auto expected{canonicalizeDomain(current.m_srv->m_target)};
              if (!actual || !expected || *actual != *expected ||
                  event.m_port != current.m_srv->m_port) {
                return DiscoveryResult{std::unexpected{
                    DiscoveryError{DiscoveryErrorKind::kBackend,
                                   "resolver result does not match SRV"}}};
              }
              auto updated{records.replaceAddress(source, resolution, token,
                                                  event.m_host, event.m_port,
                                                  std::move(event.m_address))};
              accepted = updated.has_value();
              return updated;
            })};
        if (accepted) {
          m_operations[i].m_success = true;
        }
        return result;
      },
      [this, i](DiscoveryError error) {
        if (!m_invalidated)
          failed(i, std::move(error));
      });
  auto result{op.m_resolver->start()};
  if (!result) {
    failed(i, result.error());
  }
  return result;
}

DiscoveryResult DiscoveryAvahiContribution::start() {
  if (m_started || m_cleaned || !m_client || !m_record_api || !m_resolver_api ||
      !m_source) {
    return std::unexpected{stateError("invalid contribution start")};
  }
  m_started = true;
  return drive();
}

DiscoveryResult DiscoveryAvahiContribution::drive() try {
  if (!m_started || m_cleaned || m_driving) {
    return std::unexpected{stateError("invalid contribution drive")};
  }
  m_driving = true;
  struct Guard {
    bool &value;
    ~Guard() { value = false; }
  } guard{m_driving};
  std::optional<DiscoveryError> first;
  auto remember = [&](const DiscoveryResult &result) {
    if (!result &&
        (!first || (first->m_kind == DiscoveryErrorKind::kBackend &&
                    result.error().m_kind != DiscoveryErrorKind::kBackend))) {
      first = result.error();
    }
  };
  // Process failures only after the native callback/create stack has unwound.
  auto settle = [&]() {
    for (std::size_t i{0}; i < m_operations.size(); ++i) {
      auto &op{m_operations[i]};
      if (!op.m_failure && op.m_watch) {
        if (auto status{op.m_watch->status()}; !status) {
          failed(i, std::move(status.error()));
        }
      }
      if (!op.m_failure && op.m_resolver) {
        if (auto status{op.m_resolver->status()}; !status) {
          failed(i, std::move(status.error()));
        }
      }
      if (!op.m_failure && !op.m_success) {
        continue;
      }
      bool current{false};
      auto checked{recordsUpdate([&](DiscoveryRecords &records,
                                     std::uint64_t source) {
        if (i < 2) {
          current = records.monitorGeneration(source, kind(i)) == op.m_token;
        } else {
          current =
              records.addressGeneration(source, family(i)) == op.m_token &&
              records.snapshot().m_generation == op.m_resolution;
        }
        return DiscoveryResult{};
      })};
      if (!checked) {
        remember(checked);
        remember(release(op));
        op.m_failure.reset();
        op.m_success = false;
        continue;
      }
      if (!current) {
        // A superseded callback must neither fail nor recover the new lookup.
        // A non-backend error can also have invalidated its own model token.
        if (op.m_failure &&
            op.m_failure->m_kind != DiscoveryErrorKind::kBackend && !first) {
          first = *op.m_failure;
        }
        op.m_failure.reset();
        op.m_success = false;
        remember(release(op));
        continue;
      }
      if (op.m_failure) {
        auto error{std::move(*op.m_failure)};
        op.m_failure.reset();
        op.m_success = false;
        const bool recoverable{
            error.m_kind == DiscoveryErrorKind::kBackend &&
            (!error.m_native_category ||
             *error.m_native_category ==
                 DiscoveryNativeErrorCategory::kUnavailable)};
        auto result{m_session.failAttempt(
            m_epoch, m_key, m_contribution, m_source, attempts[i], op.m_token,
            op.m_resolution, recoverable, error.m_message, error)};
        op.m_blocked = true;
        remember(result);
        remember(release(op));
        if (!first) {
          first = std::move(error);
        }
      } else if (op.m_success) {
        op.m_success = false;
        remember(m_session.succeedAttempt(m_epoch, m_key, m_contribution,
                                          m_source, attempts[i], op.m_token,
                                          op.m_resolution));
      }
    }
  };
  settle();
  if (first && first->m_kind != DiscoveryErrorKind::kBackend) {
    return std::unexpected{*first};
  }
  for (std::size_t i{0}; i < 2; ++i) {
    auto &op{m_operations[i]};
    if (!op.m_watch && !op.m_blocked) {
      remember(startWatch(i));
      settle();
      if (first && first->m_kind != DiscoveryErrorKind::kBackend) {
        return std::unexpected{*first};
      }
    }
  }
  DiscoveryRecordSnapshot snapshot;
  std::array<std::optional<std::uint64_t>, 2> tokens;
  auto read{recordsUpdate([&](DiscoveryRecords &records, std::uint64_t source) {
    snapshot = records.snapshot();
    tokens[0] =
        records.addressGeneration(source, DiscoveryAddressFamily::kIpv4);
    tokens[1] =
        records.addressGeneration(source, DiscoveryAddressFamily::kIpv6);
    return DiscoveryResult{};
  })};
  if (!read) {
    return read;
  }
  const bool usable{snapshot.m_state == DiscoveryRecordState::kResolving ||
                    snapshot.m_state == DiscoveryRecordState::kReady};
  for (std::size_t i{2}; i < 4; ++i) {
    auto &op{m_operations[i]};
    if (!m_address_families[i - 2]) {
      continue;
    }
    if (op.m_blocked && op.m_resolution != snapshot.m_generation) {
      // Refresh cancels old address timers; a new generation is fresh work.
      op.m_blocked = false;
    }
    if (op.m_resolver && (!usable || op.m_resolution != snapshot.m_generation ||
                          tokens[i - 2] != op.m_token)) {
      remember(release(op));
    }
    if (usable && tokens[i - 2] && !op.m_resolver && !op.m_blocked) {
      remember(startResolver(i, snapshot.m_generation, *tokens[i - 2]));
      settle();
    }
    if (first && first->m_kind != DiscoveryErrorKind::kBackend) {
      return std::unexpected{*first};
    }
  }
  return first ? DiscoveryResult{std::unexpected{*first}} : DiscoveryResult{};
} catch (const std::bad_alloc &) {
  static_cast<void>(cleanup());
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "contribution allocation failed"}};
} catch (...) {
  static_cast<void>(cleanup());
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                        "contribution operation threw"}};
}

DiscoveryResult DiscoveryAvahiContribution::retry(DiscoveryAttemptId id) {
  if (!m_started || m_cleaned || m_driving || id.m_source != m_source) {
    return std::unexpected{stateError("invalid contribution retry")};
  }
  for (std::size_t i{0}; i < attempts.size(); ++i) {
    if (attempts[i] == id.m_attempt && m_operations[i].m_blocked) {
      m_operations[i].m_blocked = false;
      return drive();
    }
  }
  return std::unexpected{stateError("unknown contribution retry")};
}

void DiscoveryAvahiContribution::releaseNative() noexcept {
  m_invalidated = true;
  for (auto &op : m_operations) {
    op.m_watch.reset();
    op.m_resolver.reset();
  }
}

DiscoveryResult DiscoveryAvahiContribution::cleanup() try {
  if (m_cleaned) {
    if (m_cleanup_allocation_failed) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
    }
    return {};
  }
  if (m_driving) {
    return std::unexpected{stateError("reentrant contribution cleanup")};
  }
  m_cleaned = true;
  // Release native handles before snapshot or model updates can allocate.
  releaseNative();
  std::optional<DiscoveryError> first;
#ifdef FIU_ENABLE
  if (fiu_fail("dmn/discovery/avahi/contribution-cleanup-allocation")) {
    throw std::bad_alloc{};
  }
#endif
  const auto snapshot{m_session.snapshot()};
  const bool modelActive{
      snapshot.m_delivery.m_epoch == m_epoch && !snapshot.m_stop_pending &&
      snapshot.m_delivery.m_state == DiscoveryDeliveryState::kRunning &&
      snapshot.m_backend_available};
  for (auto &op : m_operations) {
    auto result{release(op, modelActive)};
    if (!result && !first) {
      first = result.error();
    }
  }
  if (m_started && modelActive) {
    auto result{
        recordsUpdate([](DiscoveryRecords &records, std::uint64_t source) {
          static_cast<void>(
              records.setMonitoring(source, DiscoveryRecordKind::kSrv, false));
          static_cast<void>(
              records.setMonitoring(source, DiscoveryRecordKind::kTxt, false));
          return DiscoveryResult{};
        })};
    if (!result && !first) {
      first = result.error();
    }
  }
  return first ? DiscoveryResult{std::unexpected{*first}} : DiscoveryResult{};
} catch (const std::bad_alloc &) {
  m_cleanup_allocation_failed = true;
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
}

} // namespace dmn::detail
