/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-contribution.cpp
 * @brief Reconcile Bonjour operations with the shared record model.
 */

#include "dmn-discovery-bonjour-contribution.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"
#include "dmn-discovery-records.hpp"

#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {
namespace {

constexpr std::array attempts{DiscoveryAttempt::kSrv, DiscoveryAttempt::kTxt,
                              DiscoveryAttempt::kIpv4, DiscoveryAttempt::kIpv6};

DiscoveryRecordKind kind(std::size_t index) {
  return index == 0 ? DiscoveryRecordKind::kSrv : DiscoveryRecordKind::kTxt;
}

DiscoveryAddressFamily family(std::size_t index) {
  return index == 2 ? DiscoveryAddressFamily::kIpv4
                    : DiscoveryAddressFamily::kIpv6;
}

DiscoveryError stateError(const char *message) {
  return {DiscoveryErrorKind::kInvalidArgument, message};
}

} // namespace

DiscoveryBonjourContribution::DiscoveryBonjourContribution(
    DiscoverySessionHandle session, std::uint64_t epoch,
    DiscoveryServiceKey key, std::uint64_t source, DiscoveryOptions options,
    std::shared_ptr<DiscoveryBonjourPoller> poller,
    std::array<bool, 2> addressFamilies)
    : m_session{std::move(session)}, m_epoch{epoch}, m_key{std::move(key)},
      m_source{source}, m_options{options}, m_poller{std::move(poller)},
      m_address_families{addressFamilies} {}

DiscoveryBonjourContribution::~DiscoveryBonjourContribution() {
  if (!m_cleaned) {
    static_cast<void>(cleanup());
  }
}

DiscoveryResult DiscoveryBonjourContribution::recordsUpdate(
    std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update) {
  return m_session.updateRecords(m_epoch, m_key,
                                 DiscoveryContribution::kUnknown, m_source,
                                 std::move(update));
}

void DiscoveryBonjourContribution::failed(std::size_t index,
                                          DiscoveryError error) {
  if (!m_operations[index].m_failure) {
    m_operations[index].m_failure = std::move(error);
  }
}

DiscoveryResult DiscoveryBonjourContribution::release(Operation &operation,
                                                      bool modelActive) {
  operation.m_watch.reset();
  operation.m_lookup.reset();
  if (operation.m_slot) {
    const auto slot{*operation.m_slot};
    operation.m_slot.reset();
    if (modelActive) {
      return m_session.finishNative(m_epoch, slot);
    }
  }

  return {};
}

DiscoveryResult DiscoveryBonjourContribution::startWatch(std::size_t index) {
  auto &operation{m_operations[index]};
  auto slot{m_session.beginNative(m_epoch)};
  if (!slot) {
    return std::unexpected{slot.error()};
  }

  operation.m_slot = *slot;
  auto activated{
      recordsUpdate([&](DiscoveryRecords &records, std::uint64_t source) {
        if (!records.setMonitoring(source, kind(index), true)) {
          return DiscoveryResult{std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                             "record watch generation exhausted"}}};
        }

        operation.m_token = *records.monitorGeneration(source, kind(index));

        return DiscoveryResult{};
      })};
  if (!activated) {
    static_cast<void>(release(operation));

    return activated;
  }

  operation.m_watch = std::make_unique<DiscoveryBonjourRecordWatch>(
      m_poller, m_full_name, m_key.m_interface, kind(index),
      m_options.m_max_txt_entries, m_options.m_max_txt_bytes,
      [this, index,
       token = operation.m_token](DiscoveryBonjourRecordEvent event) {
        if (m_invalidated) {
          return DiscoveryResult{};
        }

        return recordsUpdate(
            [&](DiscoveryRecords &records, std::uint64_t source) {
              if (records.monitorGeneration(source, kind(index)) != token) {
                return DiscoveryResult{};
              }

              if (index == 0) {
                auto srv{parseSrvRecord(event.m_wire)};
                if (!srv) {
                  return DiscoveryResult{std::unexpected{srv.error()}};
                }

                if (event.m_added) {
                  return records.addSrv(source, token, *srv);
                }

                static_cast<void>(records.removeSrv(source, token, *srv));
              } else if (event.m_added) {
                return records.addTxt(source, token, std::move(event.m_wire));
              } else {
                static_cast<void>(
                    records.removeTxt(source, token, std::move(event.m_wire)));
              }

              return DiscoveryResult{};
            });
      },
      [this, index](DiscoveryError error) {
        if (!m_invalidated) {
          failed(index, std::move(error));
        }
      });
  auto result{operation.m_watch->start()};
  if (!result) {
    failed(index, result.error());
  } else {
    operation.m_success = true;
  }

  return result;
}

DiscoveryResult DiscoveryBonjourContribution::startLookup(
    std::size_t index, std::uint64_t resolution, std::uint64_t token,
    const DiscoverySrv &srv) {
  auto &operation{m_operations[index]};
  auto slot{m_session.beginNative(m_epoch)};
  if (!slot) {
    return std::unexpected{slot.error()};
  }

  operation.m_slot = *slot;
  operation.m_token = token;
  operation.m_resolution = resolution;
  operation.m_lookup = std::make_unique<DiscoveryBonjourAddressLookup>(
      m_poller, srv.m_target, m_key.m_interface, family(index),
      [this, index, token, resolution, host = srv.m_target,
       port = srv.m_port](DiscoveryBonjourAddressEvent event) {
        if (m_invalidated) {
          return DiscoveryResult{};
        }

        bool accepted{false};
        auto result{recordsUpdate([&](DiscoveryRecords &records,
                                      std::uint64_t source) {
          const auto current{records.snapshot()};
          if (current.m_generation != resolution ||
              records.addressGeneration(source, family(index)) != token ||
              (current.m_state != DiscoveryRecordState::kResolving &&
               current.m_state != DiscoveryRecordState::kReady)) {
            return DiscoveryResult{};
          }

          if (!event.m_added) {
            static_cast<void>(records.removeAddress(source, resolution, token,
                                                    event.m_address));

            return DiscoveryResult{};
          }

          auto updated{records.addAddress(source, resolution, token, host, port,
                                          std::move(event.m_address))};
          accepted = updated.has_value();

          return updated;
        })};
        if (accepted) {
          m_operations[index].m_success = true;
        }

        return result;
      },
      [this, index](DiscoveryError error) {
        if (!m_invalidated) {
          failed(index, std::move(error));
        }
      });
  auto result{operation.m_lookup->start()};
  if (!result) {
    failed(index, result.error());
  }

  return result;
}

DiscoveryResult DiscoveryBonjourContribution::start() {
  if (m_started || m_cleaned || !m_poller || !m_source ||
      !validateInstanceName(m_key.m_instance) ||
      !validateServiceType(m_key.m_type) || !validateDomain(m_key.m_domain) ||
      !discoveryBonjourValidInterface(m_key.m_interface)) {
    return std::unexpected{stateError("invalid Bonjour contribution start")};
  }

  char fullName[kDNSServiceMaxDomainName]{};
  const auto error{DNSServiceConstructFullName(
      fullName, m_key.m_instance.c_str(), m_key.m_type.c_str(),
      m_key.m_domain.c_str())};
  if (error != 0) {
    return std::unexpected{discoveryBonjourError(
        "Bonjour service name construction failed", error)};
  }

  m_full_name = fullName;
  m_started = true;

  return drive();
}

DiscoveryResult DiscoveryBonjourContribution::drive() try {
  if (!m_started || m_cleaned || m_driving) {
    return std::unexpected{stateError("invalid Bonjour contribution drive")};
  }

  m_driving = true;
  struct Guard {
    ~Guard() { m_value = false; }

    bool &m_value;
  } guard{m_driving};
  std::optional<DiscoveryError> first;
  auto remember = [&](const DiscoveryResult &result) {
    if (!result &&
        (!first || (first->m_kind == DiscoveryErrorKind::kBackend &&
                    result.error().m_kind != DiscoveryErrorKind::kBackend))) {
      first = result.error();
    }
  };
  auto settle = [&]() {
    for (std::size_t index{0}; index < m_operations.size(); ++index) {
      auto &operation{m_operations[index]};
      if (!operation.m_failure && operation.m_watch) {
        if (auto status{operation.m_watch->status()}; !status) {
          failed(index, std::move(status.error()));
        }
      }

      if (!operation.m_failure && operation.m_lookup) {
        if (auto status{operation.m_lookup->status()}; !status) {
          failed(index, std::move(status.error()));
        }
      }

      if (!operation.m_failure && !operation.m_success) {
        continue;
      }

      bool current{false};
      auto checked{recordsUpdate([&](DiscoveryRecords &records,
                                     std::uint64_t source) {
        if (index < 2) {
          current = records.monitorGeneration(source, kind(index)) ==
                    operation.m_token;
        } else {
          current = records.addressGeneration(source, family(index)) ==
                        operation.m_token &&
                    records.snapshot().m_generation == operation.m_resolution;
        }

        return DiscoveryResult{};
      })};
      if (!checked) {
        remember(checked);
        remember(release(operation));
        operation.m_failure.reset();
        operation.m_success = false;
        continue;
      }

      if (!current) {
        if (operation.m_failure &&
            operation.m_failure->m_kind != DiscoveryErrorKind::kBackend &&
            !first) {
          first = *operation.m_failure;
        }

        operation.m_failure.reset();
        operation.m_success = false;
        remember(release(operation));
        continue;
      }

      if (operation.m_failure) {
        auto error{std::move(*operation.m_failure)};
        operation.m_failure.reset();
        operation.m_success = false;
        const bool recoverable{
            error.m_kind == DiscoveryErrorKind::kBackend &&
            (!error.m_native_category ||
             *error.m_native_category ==
                 DiscoveryNativeErrorCategory::kUnavailable)};
        auto result{m_session.failAttempt(
            m_epoch, m_key, DiscoveryContribution::kUnknown, m_source,
            attempts[index], operation.m_token, operation.m_resolution,
            recoverable, error.m_message, error)};
        operation.m_blocked = true;
        remember(result);
        remember(release(operation));
        if (!first) {
          first = std::move(error);
        }
      } else {
        operation.m_success = false;
        remember(m_session.succeedAttempt(
            m_epoch, m_key, DiscoveryContribution::kUnknown, m_source,
            attempts[index], operation.m_token, operation.m_resolution));
      }
    }
  };
  settle();
  if (first && first->m_kind != DiscoveryErrorKind::kBackend) {
    return std::unexpected{*first};
  }

  for (std::size_t index{0}; index < 2; ++index) {
    auto &operation{m_operations[index]};
    if (!operation.m_watch && !operation.m_blocked) {
      remember(startWatch(index));
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
  for (std::size_t index{2}; index < 4; ++index) {
    auto &operation{m_operations[index]};
    if (!m_address_families[index - 2]) {
      continue;
    }

    if (operation.m_blocked &&
        operation.m_resolution != snapshot.m_generation) {
      operation.m_blocked = false;
    }

    if (operation.m_lookup &&
        (!usable || operation.m_resolution != snapshot.m_generation ||
         tokens[index - 2] != operation.m_token)) {
      remember(release(operation));
    }

    if (usable && tokens[index - 2] && !operation.m_lookup &&
        !operation.m_blocked) {
      remember(startLookup(index, snapshot.m_generation, *tokens[index - 2],
                           *snapshot.m_srv));
      settle();
    }

    if (first && first->m_kind != DiscoveryErrorKind::kBackend) {
      return std::unexpected{*first};
    }
  }

  return first ? DiscoveryResult{std::unexpected{*first}} : DiscoveryResult{};
} catch (const std::bad_alloc &) {
  static_cast<void>(cleanup());

  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Bonjour contribution allocation failed"}};
#if defined(__GLIBCXX__)
} catch (const __cxxabiv1::__forced_unwind &) {
  throw;
#endif
} catch (...) {
  static_cast<void>(cleanup());

  return std::unexpected{DiscoveryError{
      DiscoveryErrorKind::kBackend, "Bonjour contribution operation threw"}};
}

DiscoveryResult DiscoveryBonjourContribution::retry(DiscoveryAttemptId id) {
  if (!m_started || m_cleaned || m_driving || id.m_source != m_source) {
    return std::unexpected{stateError("invalid Bonjour contribution retry")};
  }

  for (std::size_t index{0}; index < attempts.size(); ++index) {
    if (attempts[index] == id.m_attempt && m_operations[index].m_blocked) {
      m_operations[index].m_blocked = false;

      return drive();
    }
  }

  return std::unexpected{stateError("unknown Bonjour contribution retry")};
}

void DiscoveryBonjourContribution::releaseNative() noexcept {
  m_invalidated = true;
  for (auto &operation : m_operations) {
    operation.m_watch.reset();
    operation.m_lookup.reset();
  }
}

DiscoveryResult DiscoveryBonjourContribution::cleanup() try {
  if (m_cleaned) {
    if (m_cleanup_allocation_failed) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
    }

    return {};
  }

  if (m_driving) {
    return std::unexpected{
        stateError("reentrant Bonjour contribution cleanup")};
  }

  m_cleaned = true;
  releaseNative();
  std::optional<DiscoveryError> first;
  const auto snapshot{m_session.snapshot()};
  const bool modelActive{
      snapshot.m_delivery.m_epoch == m_epoch && !snapshot.m_stop_pending &&
      snapshot.m_delivery.m_state == DiscoveryDeliveryState::kRunning &&
      snapshot.m_backend_available};
  for (auto &operation : m_operations) {
    auto result{release(operation, modelActive)};
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
