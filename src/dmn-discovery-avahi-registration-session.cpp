/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-registration-session.cpp
 * @brief Registration lifecycle integration on the Avahi poll worker.
 */

#include "dmn-discovery-avahi-registration-session.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <limits>
#include <list>
#include <stdexcept>
#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {

struct DiscoveryAvahiRegistrationSessions::Entry {
  DiscoveryAdvertisement m_advertisement;
  DiscoveryRegistrationHandle m_handle;
  std::unique_ptr<DiscoveryAvahiRegistration> m_native;
};

struct DiscoveryAvahiRegistrationSessions::BrowserEntry {
  struct Source {
    DiscoveryServiceKey m_key;
    DiscoveryContribution m_transport;
    std::uint64_t m_generation;
    bool m_present{true};
    bool m_readd{false};
    DiscoveryProvenance m_provenance{};
    std::unique_ptr<DiscoveryAvahiContribution> m_native;
  };
  DiscoveryAvahiQuery m_query;
  std::array<bool, 2> m_families;
  DiscoverySessionHandle m_handle;
  std::unique_ptr<DiscoveryAvahiServiceBrowser> m_native;
  std::list<Source> m_sources;
  std::optional<DiscoveryError> m_error;
};

namespace {
bool sameDnsName(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto fold = [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
    };
    if (fold(a[i]) != fold(b[i]))
      return false;
  }
  return true;
}
bool sameKey(const DiscoveryServiceKey &a, const DiscoveryServiceKey &b) {
  const auto leftDomain{canonicalizeDomain(a.m_domain)};
  const auto rightDomain{canonicalizeDomain(b.m_domain)};
  return a.m_interface == b.m_interface &&
         sameDnsName(a.m_instance, b.m_instance) &&
         sameDnsName(a.m_type, b.m_type) && leftDomain && rightDomain &&
         *leftDomain == *rightDomain;
}
} // namespace

class DiscoveryAvahiRegistrationSessions::DrivingPoller final
    : public DiscoveryCommandPoller {
public:
  DrivingPoller(std::weak_ptr<DiscoveryAvahiRegistrationSessions> owner,
                std::shared_ptr<DiscoveryAvahiPoller> poller)
      : m_owner{std::move(owner)}, m_poller{std::move(poller)} {}

  DiscoveryResult start() override {
    auto result{m_poller->start()};
    if (auto owner{m_owner.lock()}) {
      owner->reap();
    }
    return result;
  }
  DiscoveryResult wait(bool block) override {
    auto result{m_poller->wait(block)};
    if (auto owner{m_owner.lock()}) {
      if (!result) {
#ifdef FIU_ENABLE
        if (fiu_fail("dmn/discovery/avahi/poll-failure-allocation")) {
          throw std::bad_alloc{};
        }
#endif
        owner->failBackend(result.error());
      } else if (auto driven{owner->driveBrowsers()}; !driven) {
        owner->failBackend(driven.error());
        return std::unexpected{driven.error()};
      } else if (*driven &&
                 **driven == std::chrono::steady_clock::time_point::min()) {
        owner->m_force_poll_iteration = true;
      }
      owner->reap();
    }
    return result;
  }
  void wake() noexcept override { m_poller->wake(); }
  DiscoveryResult stop() override { return m_poller->stop(); }

private:
  std::weak_ptr<DiscoveryAvahiRegistrationSessions> m_owner;
  std::shared_ptr<DiscoveryAvahiPoller> m_poller;
};

DiscoveryAvahiRegistrationSessions::DiscoveryAvahiRegistrationSessions(
    DiscoveryOptions options,
    std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
    std::shared_ptr<DiscoveryAvahiBrowserApi> browserApi,
    std::shared_ptr<DiscoveryAvahiRecordApi> recordApi,
    std::shared_ptr<DiscoveryAvahiResolverApi> resolverApi,
    DiscoveryAvahiPoller::Clock clock,
    DiscoverySessionClient::RegistrationFault fault)
    : m_options{options}, m_api{std::move(registrationApi)},
      m_browser_api{std::move(browserApi)}, m_record_api{std::move(recordApi)},
      m_resolver_api{std::move(resolverApi)},
      m_client{options, false, std::move(clock), std::move(fault)} {}

DiscoveryAvahiRegistrationSessions::~DiscoveryAvahiRegistrationSessions() {
  static_cast<void>(shutdown());
}

std::expected<std::shared_ptr<DiscoveryAvahiRegistrationSessions>,
              DiscoveryError>
DiscoveryAvahiRegistrationSessions::create(
    DiscoveryOptions options, std::shared_ptr<DiscoveryAvahiApi> pollApi,
    std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
    DiscoveryAvahiPoller::Clock clock,
    DiscoverySessionClient::RegistrationFault fault) {
  return create(options, std::move(pollApi), std::move(registrationApi), {}, {},
                {}, std::move(clock), std::move(fault));
}

std::expected<std::shared_ptr<DiscoveryAvahiRegistrationSessions>,
              DiscoveryError>
DiscoveryAvahiRegistrationSessions::create(
    DiscoveryOptions options, std::shared_ptr<DiscoveryAvahiApi> pollApi,
    std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
    std::shared_ptr<DiscoveryAvahiBrowserApi> browserApi,
    std::shared_ptr<DiscoveryAvahiRecordApi> recordApi,
    std::shared_ptr<DiscoveryAvahiResolverApi> resolverApi,
    DiscoveryAvahiPoller::Clock clock,
    DiscoverySessionClient::RegistrationFault fault) try {
  if (!pollApi || !registrationApi || !clock) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                       "invalid Avahi registration backend"}};
  }
  if ((browserApi || recordApi || resolverApi) &&
      (!browserApi || !recordApi || !resolverApi)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "incomplete Avahi browser backend"}};
  }
  auto owner{std::shared_ptr<DiscoveryAvahiRegistrationSessions>{
      new DiscoveryAvahiRegistrationSessions{
          options, std::move(registrationApi), std::move(browserApi),
          std::move(recordApi), std::move(resolverApi), clock,
          std::move(fault)}}};
  std::weak_ptr<DiscoveryAvahiRegistrationSessions> weak{owner};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      std::move(pollApi),
      [self = owner.get()](DiscoveryAvahiEvent event) {
        self->clientEvent(event);
      },
      std::move(clock), [self = owner.get()] { return self->cleanup(); },
      [self = owner.get()] { return self->driveBrowsers(); })};
  auto driving{std::make_shared<DrivingPoller>(weak, std::move(poller))};
  if (auto result{owner->m_client.attachPoller(std::move(driving))}; !result) {
    return std::unexpected{result.error()};
  }
  return owner;
} catch (const std::bad_alloc &) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "Avahi session allocation failed"}};
}

std::expected<DiscoveryRegistrationHandle, DiscoveryError>
DiscoveryAvahiRegistrationSessions::advertise(
    DiscoveryAdvertisement advertisement,
    DiscoverySessionClient::RegistrationHandler handler) try {
  if (auto result{
          validateDiscoveryAvahiAdvertisement(m_options, advertisement)};
      !result) {
    return std::unexpected{result.error()};
  }
  using Output = std::expected<DiscoveryRegistrationHandle, DiscoveryError>;
  auto output{std::make_shared<std::optional<Output>>()};
  auto result{m_client.dispatchNative([this,
                                       advertisement = std::move(advertisement),
                                       handler = std::move(handler),
                                       output]() mutable -> DiscoveryResult {
    if (m_next_id == 0) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "registration ID exhausted"}};
    }
    const auto id{m_next_id++};
    auto entry{std::make_unique<Entry>(Entry{advertisement, {}, {}})};
    std::weak_ptr<DiscoveryAvahiRegistrationSessions> weak{shared_from_this()};
    std::function<void()> cancellation{[weak, id] {
      if (auto self{weak.lock()}) {
        self->cancel(id);
      }
    }};
    m_entries.emplace(id, std::move(entry));
    auto handle{m_client.advertise(std::move(advertisement), std::move(handler),
                                   std::move(cancellation))};
    if (!handle) {
      m_entries.erase(id);
      return std::unexpected{handle.error()};
    }
    m_entries.at(id)->m_handle = *handle;
    *output = *handle;
    try {
#ifdef FIU_ENABLE
      if (fiu_fail("dmn/discovery/avahi/registration-post-admission") ||
          fiu_fail("dmn/discovery/avahi/registration-post-admission-nested")) {
        throw std::bad_alloc{};
      }
      if (fiu_fail("dmn/discovery/avahi/registration-post-admission-error")) {
        throw std::runtime_error{"injected registration startup failure"};
      }
#endif
      if (m_native_client) {
        auto &current{*m_entries.at(id)};
        auto available{current.m_handle.backendAvailable()};
        if (available) {
          start(current);
#ifdef FIU_ENABLE
          if (fiu_fail("dmn/discovery/avahi/registration-after-start") ||
              fiu_fail("dmn/discovery/avahi/registration-after-disconnect")) {
            throw std::bad_alloc{};
          }
          if (fiu_fail("dmn/discovery/avahi/registration-after-start-error")) {
            throw std::runtime_error{"injected registration startup failure"};
          }
#endif
        } else {
          const auto state{current.m_handle.status().m_state};
          if (state != DiscoveryRegistrationState::kStopped &&
              state != DiscoveryRegistrationState::kOverflow) {
            static_cast<void>(
                current.m_handle.backendFailed(available.error()));
          }
        }
        if (m_loss_pending) {
          try {
            loseClient();
          } catch (...) {
            failBackend({DiscoveryErrorKind::kBackend,
                         "Avahi registration backend transition failed"});
          }
        }
        reap();
      }
    } catch (const std::bad_alloc &) {
      if (m_loss_pending) {
        releaseClientNative();
      } else if (auto found{m_entries.find(id)}; found != m_entries.end()) {
        found->second->m_native.reset();
      }
      handle->failPostAdmissionAllocation();
#if defined(__GLIBCXX__)
    } catch (const __cxxabiv1::__forced_unwind &) {
      throw;
#endif
    } catch (...) {
      if (m_loss_pending) {
        releaseClientNative();
      } else if (auto found{m_entries.find(id)}; found != m_entries.end()) {
        found->second->m_native.reset();
      }
      try {
        auto failed{handle->backendFailed({DiscoveryErrorKind::kBackend, {}})};
        if (!failed &&
            failed.error().m_kind == DiscoveryErrorKind::kResourceLimit)
          handle->failPostAdmissionAllocation();
      } catch (const std::bad_alloc &) {
        handle->failPostAdmissionAllocation();
      }
    }
    return {};
  })};
  if (!result) {
    return std::unexpected{result.error()};
  }
  return std::move(**output);
} catch (const std::bad_alloc &) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Avahi advertisement allocation failed"}};
}

std::expected<DiscoverySessionHandle, DiscoveryError>
DiscoveryAvahiRegistrationSessions::browse(
    DiscoveryAvahiQuery query, DiscoverySessionClient::Handler handler,
    std::array<bool, 2> addressFamilies) try {
  if (!m_browser_api || !m_record_api || !m_resolver_api) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kUnsupported,
                                          "Avahi browsing not configured"}};
  }
  if (!validateServiceType(query.m_type) || !validateDomain(query.m_domain) ||
      (!addressFamilies[0] && !addressFamilies[1]) ||
      (query.m_interface &&
       (*query.m_interface == 0 ||
        *query.m_interface > static_cast<std::uint32_t>(
                                 std::numeric_limits<AvahiIfIndex>::max())))) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi browser query"}};
  }
  auto output{std::make_shared<std::optional<DiscoverySessionHandle>>()};
  auto result{m_client.dispatchNative([this, query = std::move(query),
                                       handler = std::move(handler),
                                       addressFamilies,
                                       output]() mutable -> DiscoveryResult {
    if (m_next_id == 0) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "discovery ID exhausted"}};
    }
    const auto id{m_next_id++};
    auto entry{std::make_unique<BrowserEntry>()};
    entry->m_query = std::move(query);
    entry->m_families = addressFamilies;
    std::weak_ptr<DiscoveryAvahiRegistrationSessions> weak{shared_from_this()};
    DiscoverySessionClient::BrowserLifecycle lifecycle{
        [weak, id]() -> DiscoveryResult {
          if (auto self{weak.lock()})
            return self->cancelBrowser(id);
          return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                                "Avahi owner unavailable"}};
        },
        [weak, id]() -> DiscoveryResult {
          if (auto self{weak.lock()})
            return self->resetBrowser(id);
          return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                                "Avahi owner unavailable"}};
        }};
    m_browsers.emplace(id, std::move(entry));
    auto handle{m_client.browse(std::move(handler), std::move(lifecycle))};
    if (!handle) {
      m_browsers.erase(id);
      return std::unexpected{handle.error()};
    }
    auto &current{*m_browsers.at(id)};
    current.m_handle = *handle;
    *output = *handle;
    try {
#ifdef FIU_ENABLE
      if (fiu_fail("dmn/discovery/avahi/browser-post-admission") ||
          fiu_fail("dmn/discovery/avahi/browser-post-admission-nested")) {
        throw std::bad_alloc{};
      }
      if (fiu_fail("dmn/discovery/avahi/browser-post-admission-error")) {
        throw std::runtime_error{"injected browser startup failure"};
      }
#endif
      if (m_native_client) {
        auto available{current.m_handle.backendAvailable(
            current.m_handle.snapshot().m_delivery.m_epoch)};
        if (available) {
          startBrowser(current);
#ifdef FIU_ENABLE
          if (fiu_fail("dmn/discovery/avahi/browser-after-start") ||
              fiu_fail("dmn/discovery/avahi/browser-after-disconnect")) {
            throw std::bad_alloc{};
          }
          if (fiu_fail("dmn/discovery/avahi/browser-after-start-error")) {
            throw std::runtime_error{"injected browser startup failure"};
          }
#endif
        } else if (current.m_handle.snapshot().m_delivery.m_state ==
                   DiscoveryDeliveryState::kRunning) {
          current.m_error = available.error();
        }
        if (m_loss_pending) {
          try {
            loseClient();
          } catch (...) {
            failBackend({DiscoveryErrorKind::kBackend,
                         "Avahi browser backend transition failed"});
          }
        }
        if (auto driven{driveBrowsers()}; !driven) {
          failBackend(driven.error());
        } else if (*driven &&
                   **driven == std::chrono::steady_clock::time_point::min()) {
          m_force_poll_iteration = true;
        }
      }
    } catch (const std::bad_alloc &) {
      if (m_loss_pending) {
        releaseClientNative();
      } else if (auto found{m_browsers.find(id)}; found != m_browsers.end()) {
        releaseBrowserNative(*found->second);
      }
      handle->failPostAdmissionAllocation();
#if defined(__GLIBCXX__)
    } catch (const __cxxabiv1::__forced_unwind &) {
      throw;
#endif
    } catch (...) {
      if (m_loss_pending) {
        releaseClientNative();
      } else if (auto found{m_browsers.find(id)}; found != m_browsers.end()) {
        releaseBrowserNative(*found->second);
      }
      try {
        auto failed{handle->backendFailed({DiscoveryErrorKind::kBackend, {}})};
        if (!failed &&
            failed.error().m_kind == DiscoveryErrorKind::kResourceLimit)
          handle->failPostAdmissionAllocation();
      } catch (const std::bad_alloc &) {
        handle->failPostAdmissionAllocation();
      }
    }
    return {};
  })};
  if (!result)
    return std::unexpected{result.error()};
  return std::move(**output);
} catch (const std::bad_alloc &) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "Avahi browser allocation failed"}};
}

void DiscoveryAvahiRegistrationSessions::startBrowser(BrowserEntry &entry) {
  if (!m_native_client || entry.m_native)
    return;
  const auto epoch{entry.m_handle.snapshot().m_delivery.m_epoch};
  m_starting = true;
  struct Guard {
    bool &flag;
    ~Guard() { flag = false; }
  } guard{m_starting};
  try {
    entry.m_native = std::make_unique<DiscoveryAvahiServiceBrowser>(
        m_browser_api, m_native_client, entry.m_query,
        [this, &entry, epoch](DiscoveryAvahiBrowseEvent event) {
          const auto snapshot{entry.m_handle.snapshot()};
          if (snapshot.m_delivery.m_epoch != epoch || snapshot.m_stop_pending ||
              snapshot.m_delivery.m_state != DiscoveryDeliveryState::kRunning) {
            return DiscoveryResult{};
          }
          return onBrowse(entry, std::move(event));
        },
        [&entry](DiscoveryError error) { entry.m_error = std::move(error); });
    auto result{entry.m_native->start()};
    if (!result)
      entry.m_error = result.error();
  } catch (const std::bad_alloc &) {
    entry.m_error = DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                   "Avahi browser allocation failed"};
  } catch (...) {
    entry.m_error = DiscoveryError{DiscoveryErrorKind::kBackend,
                                   "Avahi browser startup failed"};
  }
}

DiscoveryResult DiscoveryAvahiRegistrationSessions::onBrowse(
    BrowserEntry &entry, DiscoveryAvahiBrowseEvent event) try {
  for (auto &source : entry.m_sources) {
    if (source.m_transport == event.m_contribution &&
        sameKey(source.m_key, event.m_key)) {
      if (event.m_added) {
        source.m_provenance = mapAvahiBrowseProvenance(event.m_flags);
        if (!source.m_present) {
          source.m_readd = true;
        } else {
          auto updated{entry.m_handle.observe(
              entry.m_handle.snapshot().m_delivery.m_epoch, source.m_key,
              source.m_transport, source.m_provenance)};
          if (!updated) {
            return std::unexpected{updated.error()};
          }
        }
      } else if (source.m_present) {
        source.m_present = false;
        if (source.m_native)
          source.m_native->invalidate();
      } else {
        source.m_readd = false;
      }
      return {};
    }
  }
  if (!event.m_added)
    return {};
  if (entry.m_sources.size() / 2 >= m_options.m_max_instances) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "browse contribution limit reached"}};
  }
  entry.m_sources.push_back({std::move(event.m_key),
                             event.m_contribution,
                             0,
                             true,
                             false,
                             mapAvahiBrowseProvenance(event.m_flags),
                             {}});
  auto &source{entry.m_sources.back()};
  auto generation{entry.m_handle.observe(
      entry.m_handle.snapshot().m_delivery.m_epoch, source.m_key,
      source.m_transport, source.m_provenance)};
  if (!generation) {
    entry.m_sources.pop_back();
    return std::unexpected{generation.error()};
  }
  source.m_generation = *generation;
  return {};
} catch (const std::bad_alloc &) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "browse contribution allocation failed"}};
}

void DiscoveryAvahiRegistrationSessions::releaseBrowserNative(
    BrowserEntry &entry) noexcept {
  entry.m_native.reset();
  for (auto &source : entry.m_sources) {
    if (source.m_native) {
      source.m_native->releaseNative();
    }
  }
}

DiscoveryResult
DiscoveryAvahiRegistrationSessions::cleanupBrowser(BrowserEntry &entry) {
  std::optional<DiscoveryError> first;
  releaseBrowserNative(entry);
  for (auto &source : entry.m_sources) {
    if (source.m_native) {
      auto result{source.m_native->cleanup()};
      if (!result && !first) {
        first = std::move(result.error());
      }
    }
    source.m_native.reset();
  }
  entry.m_sources.clear();
  return first ? DiscoveryResult{std::unexpected{std::move(*first)}}
               : DiscoveryResult{};
}

DiscoveryResult
DiscoveryAvahiRegistrationSessions::cancelBrowser(std::uint64_t id) {
  auto found{m_browsers.find(id)};
  if (found != m_browsers.end()) {
    auto result{cleanupBrowser(*found->second)};
    m_browsers.erase(found);
    return result;
  }
  return {};
}

DiscoveryResult
DiscoveryAvahiRegistrationSessions::resetBrowser(std::uint64_t id) {
  auto found{m_browsers.find(id)};
  if (found == m_browsers.end()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi browser unavailable"}};
  }
  auto &entry{*found->second};
  if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
    static_cast<void>(entry.m_handle.backendFailed(cleaned.error()));
    return cleaned;
  }
  entry.m_error.reset();
  if (m_native_client) {
    const auto snapshot{entry.m_handle.snapshot()};
    if (!snapshot.m_backend_available) {
      auto available{
          entry.m_handle.backendAvailable(snapshot.m_delivery.m_epoch)};
      if (!available)
        return available;
    }
    startBrowser(entry);
    if (m_loss_pending)
      loseClient();
    if (entry.m_error) {
      auto error{*entry.m_error};
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        error.m_message += "; cleanup failed: " + cleaned.error().m_message;
      }
      static_cast<void>(entry.m_handle.backendFailed(error));
      return std::unexpected{std::move(error)};
    }
  }
  return {};
}

std::expected<DiscoveryAvahiPoller::Deadline, DiscoveryError>
DiscoveryAvahiRegistrationSessions::driveBrowsers() try {
  if (m_loss_pending) {
    loseClient();
    return DiscoveryAvahiPoller::Deadline{
        std::chrono::steady_clock::time_point::min()};
  }
  DiscoveryAvahiPoller::Deadline next;
  if (m_force_poll_iteration) {
    m_force_poll_iteration = false;
    next = std::chrono::steady_clock::time_point::min();
  }
  for (auto it{m_browsers.begin()}; it != m_browsers.end();) {
    auto &entry{*it->second};
    auto snapshot{entry.m_handle.snapshot()};
    if (snapshot.m_stop_pending ||
        snapshot.m_delivery.m_state == DiscoveryDeliveryState::kStopped) {
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        return std::unexpected{cleaned.error()};
      }
      it = m_browsers.erase(it);
      continue;
    }
    if (snapshot.m_delivery.m_state == DiscoveryDeliveryState::kOverflow) {
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        return std::unexpected{cleaned.error()};
      }
      ++it;
      continue;
    }
    if (entry.m_native) {
      auto status{entry.m_native->status()};
      if (!status && !entry.m_error)
        entry.m_error = status.error();
    }
    if (entry.m_error) {
      auto error{std::move(*entry.m_error)};
      entry.m_error.reset();
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        error.m_message += "; cleanup failed: " + cleaned.error().m_message;
      }
      static_cast<void>(entry.m_handle.backendFailed(std::move(error)));
      ++it;
      continue;
    }
    if (!m_native_client || !snapshot.m_backend_available || !entry.m_native) {
      ++it;
      continue;
    }
    const auto epoch{snapshot.m_delivery.m_epoch};
    auto timers{entry.m_handle.processTimers(epoch)};
    if (!timers) {
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        return std::unexpected{cleaned.error()};
      }
      static_cast<void>(entry.m_handle.backendFailed(timers.error()));
      ++it;
      continue;
    }
    auto due{entry.m_handle.takeDueRetries(epoch)};
    if (!due) {
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        return std::unexpected{cleaned.error()};
      }
      static_cast<void>(entry.m_handle.backendFailed(due.error()));
      ++it;
      continue;
    }
    m_starting = true;
    {
      struct Guard {
        bool &flag;
        ~Guard() { flag = false; }
      } guard{m_starting};
      for (auto source{entry.m_sources.begin()};
           source != entry.m_sources.end();) {
        if (!source->m_present) {
          if (source->m_native) {
            auto cleaned{source->m_native->cleanup()};
            if (!cleaned && !entry.m_error) {
              entry.m_error = cleaned.error();
            }
          }
          source->m_native.reset();
          auto removed{entry.m_handle.removeObservation(
              epoch, source->m_key, source->m_transport, source->m_generation)};
          if (!removed && !entry.m_error) {
            entry.m_error = removed.error();
          }
          if (!source->m_readd || entry.m_error) {
            source = entry.m_sources.erase(source);
            continue;
          }
          auto added{entry.m_handle.observe(
              epoch, source->m_key, source->m_transport, source->m_provenance)};
          if (!added) {
            entry.m_error = added.error();
            source = entry.m_sources.erase(source);
            continue;
          }
          source->m_generation = *added;
          source->m_present = true;
          source->m_readd = false;
        }
        if (!source->m_native) {
          try {
            source->m_native = std::make_unique<DiscoveryAvahiContribution>(
                entry.m_handle, epoch, source->m_key, source->m_transport,
                source->m_generation, m_options, m_native_client, m_record_api,
                m_resolver_api, entry.m_families);
          } catch (const std::bad_alloc &) {
            entry.m_error = DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                           "contribution allocation failed"};
            break;
          }
          auto started{source->m_native->start()};
          if (!started &&
              (started.error().m_kind != DiscoveryErrorKind::kBackend ||
               source->m_native->cleaned())) {
            entry.m_error = started.error();
          }
        }
        if (m_loss_pending || entry.m_error)
          break;
        ++source;
      }
      if (!m_loss_pending && !entry.m_error) {
        for (const auto &retry : *due) {
          for (auto &source : entry.m_sources) {
            if (source.m_generation == retry.m_source && source.m_native) {
              auto result{source.m_native->retry(retry)};
              if (!result &&
                  (result.error().m_kind != DiscoveryErrorKind::kBackend ||
                   source.m_native->cleaned())) {
                entry.m_error = result.error();
              }
              break;
            }
          }
          if (m_loss_pending || entry.m_error)
            break;
        }
      }
      for (int pass = 0; pass < 2 && !m_loss_pending && !entry.m_error;
           ++pass) {
        for (auto &source : entry.m_sources) {
          if (!source.m_native)
            continue;
          auto result{source.m_native->drive()};
          if (!result &&
              (result.error().m_kind != DiscoveryErrorKind::kBackend ||
               source.m_native->cleaned())) {
            entry.m_error = result.error();
            break;
          }
          if (m_loss_pending)
            break;
        }
      }
    }
    if (m_loss_pending) {
      loseClient();
      m_force_poll_iteration = false;
      return DiscoveryAvahiPoller::Deadline{
          std::chrono::steady_clock::time_point::min()};
    }
    if (entry.m_error) {
      auto error{std::move(*entry.m_error)};
      entry.m_error.reset();
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        error.m_message += "; cleanup failed: " + cleaned.error().m_message;
      }
      static_cast<void>(entry.m_handle.backendFailed(std::move(error)));
      ++it;
      continue;
    }
    auto deadline{entry.m_handle.nextDeadline(epoch)};
    if (!deadline) {
      if (auto cleaned{cleanupBrowser(entry)}; !cleaned) {
        return std::unexpected{cleaned.error()};
      }
      static_cast<void>(entry.m_handle.backendFailed(deadline.error()));
    } else if (*deadline && (!next || **deadline < *next)) {
      next = *deadline;
    }
    ++it;
  }
  return next;
} catch (const std::bad_alloc &) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Avahi browser drive allocation failed"}};
} catch (...) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                        "Avahi browser drive failed"}};
}

void DiscoveryAvahiRegistrationSessions::start(Entry &entry) try {
  struct StartGuard {
    bool &m_starting;
    ~StartGuard() { m_starting = false; }
  };
  m_starting = true;
  StartGuard guard{m_starting};
  const auto generation{entry.m_handle.generation()};
  auto handle{entry.m_handle};
  entry.m_native = std::make_unique<DiscoveryAvahiRegistration>(
      m_api, m_native_client, entry.m_advertisement, m_options,
      [handle, generation](DiscoveryAvahiRegistrationEvent event) {
        return event == DiscoveryAvahiRegistrationEvent::kEstablished
                   ? handle.established(generation)
                   : handle.collision(generation, "Avahi name collision");
      },
      [handle, generation](DiscoveryError error) {
        static_cast<void>(handle.failed(generation, std::move(error)));
      });
  auto result{entry.m_native->start()};
  if (!result) {
    static_cast<void>(entry.m_handle.failed(generation, result.error()));
  }
} catch (const std::bad_alloc &) {
  static_cast<void>(entry.m_handle.failed(
      entry.m_handle.generation(),
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Avahi registration allocation failed"}));
} catch (...) {
  static_cast<void>(entry.m_handle.failed(entry.m_handle.generation(),
                                          "Avahi registration startup failed"));
}

void DiscoveryAvahiRegistrationSessions::clientEvent(
    DiscoveryAvahiEvent event) {
  if (event.m_state == AVAHI_CLIENT_FAILURE) {
    m_force_poll_iteration = true;
  }
  if (event.m_state == AVAHI_CLIENT_S_RUNNING && event.m_client) {
    if (m_native_client == event.m_client || m_starting || m_loss_pending) {
      return;
    }
    m_native_client = event.m_client;
    for (auto &[id, entry] : m_entries) {
      static_cast<void>(id);
      auto available{entry->m_handle.backendAvailable()};
      if (available) {
        start(*entry);
      } else {
        const auto state{entry->m_handle.status().m_state};
        if (state != DiscoveryRegistrationState::kStopped &&
            state != DiscoveryRegistrationState::kOverflow) {
          throw std::runtime_error{available.error().m_message};
        }
      }
      if (m_loss_pending) {
        loseClient();
        return;
      }
    }
    for (auto &[id, entry] : m_browsers) {
      static_cast<void>(id);
      auto state{entry->m_handle.snapshot()};
      if (state.m_stop_pending ||
          state.m_delivery.m_state != DiscoveryDeliveryState::kRunning) {
        continue;
      }
      auto available{
          entry->m_handle.backendAvailable(state.m_delivery.m_epoch)};
      if (available) {
        startBrowser(*entry);
      } else {
        entry->m_error = available.error();
      }
      if (m_loss_pending) {
        loseClient();
        return;
      }
    }
    return;
  }
  m_native_client = nullptr;
  if (m_starting) {
    m_loss_pending = true;
    return;
  }
  loseClient();
}

void DiscoveryAvahiRegistrationSessions::loseClient() {
  m_native_client = nullptr;
  m_loss_pending = false;
  std::optional<DiscoveryError> cleanupError;
  releaseClientNative();
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
    if (auto result{cleanupBrowser(*entry)}; !result && !cleanupError) {
      cleanupError = std::move(result.error());
    }
    entry->m_error.reset();
  }
  if (cleanupError) {
    throw std::runtime_error{cleanupError->m_message};
  }
  for (auto &[id, entry] : m_entries) {
    static_cast<void>(id);
    auto lost{entry->m_handle.backendLost()};
    if (!lost) {
      const auto state{entry->m_handle.status().m_state};
      if (state != DiscoveryRegistrationState::kStopped &&
          state != DiscoveryRegistrationState::kOverflow) {
        throw std::runtime_error{lost.error().m_message};
      }
    }
  }
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
    const auto state{entry->m_handle.snapshot()};
    if (!state.m_stop_pending &&
        state.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
      auto lost{entry->m_handle.backendLost(state.m_delivery.m_epoch)};
      if (!lost && entry->m_handle.snapshot().m_delivery.m_state ==
                       DiscoveryDeliveryState::kRunning) {
        throw std::runtime_error{lost.error().m_message};
      }
    }
  }
}

void DiscoveryAvahiRegistrationSessions::reap() {
  for (auto it{m_entries.begin()}; it != m_entries.end();) {
    auto &entry{*it->second};
    auto state{entry.m_handle.status().m_state};
    if (entry.m_native) {
      auto nativeStatus{entry.m_native->status()};
      if (!nativeStatus) {
        if (state == DiscoveryRegistrationState::kRegistering ||
            state == DiscoveryRegistrationState::kEstablished) {
          static_cast<void>(entry.m_handle.failed(entry.m_handle.generation(),
                                                  nativeStatus.error()));
          state = entry.m_handle.status().m_state;
        }
        entry.m_native.reset();
      }
    }
    if (state == DiscoveryRegistrationState::kCollision ||
        state == DiscoveryRegistrationState::kFailed ||
        state == DiscoveryRegistrationState::kOverflow ||
        state == DiscoveryRegistrationState::kStopped) {
      entry.m_native.reset();
    }
    if (state == DiscoveryRegistrationState::kStopped) {
      it = m_entries.erase(it);
    } else {
      ++it;
    }
  }
}

void DiscoveryAvahiRegistrationSessions::cancel(std::uint64_t id) {
  m_entries.erase(id);
}

void DiscoveryAvahiRegistrationSessions::releaseClientNative() noexcept {
  for (auto &[id, entry] : m_entries) {
    static_cast<void>(id);
    entry->m_native.reset();
  }
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
    releaseBrowserNative(*entry);
  }
}

void DiscoveryAvahiRegistrationSessions::failBackend(
    const DiscoveryError &error) {
  m_native_client = nullptr;
  releaseClientNative();
  auto reported{error};
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
    if (auto result{cleanupBrowser(*entry)}; !result) {
      reported.m_message += "; cleanup failed: " + result.error().m_message;
    }
  }
  for (auto &[id, entry] : m_entries) {
    static_cast<void>(id);
    static_cast<void>(entry->m_handle.backendFailed(reported));
  }
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
    static_cast<void>(entry->m_handle.backendFailed(reported));
  }
}

DiscoveryResult DiscoveryAvahiRegistrationSessions::cleanup() {
  m_native_client = nullptr;
  std::optional<DiscoveryError> cleanupError;
  releaseClientNative();
  for (auto &[id, entry] : m_browsers) {
    static_cast<void>(id);
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/avahi/browser-cleanup-allocation")) {
      throw std::bad_alloc{};
    }
#endif
    if (auto result{cleanupBrowser(*entry)}; !result && !cleanupError) {
      cleanupError = std::move(result.error());
    }
  }
  if (cleanupError) {
    return std::unexpected{std::move(*cleanupError)};
  }
  try {
    for (auto &[id, entry] : m_entries) {
      static_cast<void>(id);
      const auto state{entry->m_handle.status().m_state};
      if (state == DiscoveryRegistrationState::kRegistering ||
          state == DiscoveryRegistrationState::kEstablished ||
          state == DiscoveryRegistrationState::kUnavailable) {
        static_cast<void>(entry->m_handle.backendFailed(
            {DiscoveryErrorKind::kBackend,
             "Avahi registration worker stopped"}));
      }
    }
    for (auto &[id, entry] : m_browsers) {
      static_cast<void>(id);
      const auto state{entry->m_handle.snapshot()};
      if (!state.m_stop_pending &&
          state.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
        static_cast<void>(entry->m_handle.backendFailed(
            {DiscoveryErrorKind::kBackend, "Avahi discovery worker stopped"}));
      }
    }
    for (auto &[id, entry] : m_entries) {
      static_cast<void>(id);
      if (auto result{entry->m_handle.nativeCleanupComplete()}; !result) {
        return result;
      }
    }
    for (auto &[id, entry] : m_browsers) {
      static_cast<void>(id);
      if (auto result{entry->m_handle.nativeCleanupComplete()}; !result) {
        return result;
      }
    }
  } catch (...) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi registration cleanup failed"}};
  }
  return {};
}

DiscoveryResult DiscoveryAvahiRegistrationSessions::shutdown() {
  return m_client.shutdown();
}

} // namespace dmn::detail
