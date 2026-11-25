/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-sessions.cpp
 * @brief Reconcile Bonjour browsers and registrations on the session worker.
 */

#include "dmn-discovery-bonjour-sessions.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"

#include <algorithm>
#include <list>
#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {

struct DiscoveryBonjourSessions::Browser {
  struct Source {
    DiscoveryServiceKey m_key;
    std::uint64_t m_generation;
    bool m_present{true};
    bool m_readd{false};
    std::unique_ptr<DiscoveryBonjourContribution> m_native;
  };

  DiscoveryBonjourQuery m_query;
  std::array<bool, 2> m_families;
  DiscoverySessionHandle m_handle;
  std::unique_ptr<DiscoveryBonjourBrowser> m_native;
  std::list<Source> m_sources;
  std::optional<DiscoveryError> m_error;
  DiscoveryBonjourPoller::Deadline m_retry_due;
  unsigned m_retry_seconds{1};
  std::uint64_t m_epoch{0};
  bool m_starting{false};
  std::vector<DiscoveryBonjourBrowseEvent> m_pending;
};

struct DiscoveryBonjourSessions::Registration {
  DiscoveryAdvertisement m_advertisement;
  DiscoveryRegistrationHandle m_handle;
  std::unique_ptr<DiscoveryBonjourRegistration> m_native;
  std::optional<DiscoveryError> m_error;
  DiscoveryBonjourPoller::Deadline m_retry_due;
  unsigned m_retry_seconds{1};
};

namespace {

bool sameKey(const DiscoveryServiceKey &left,
             const DiscoveryServiceKey &right) {
  const auto leftDomain{canonicalizeDomain(left.m_domain)};
  const auto rightDomain{canonicalizeDomain(right.m_domain)};

  return left.m_interface == right.m_interface &&
         discoveryBonjourSameLabel(left.m_instance, right.m_instance) &&
         discoveryBonjourSameLabel(left.m_type, right.m_type) && leftDomain &&
         rightDomain && *leftDomain == *rightDomain;
}

} // namespace

// The owner joins its worker before destroying the poller or browser state.
class DiscoveryBonjourSessions::DrivingPoller final
    : public DiscoveryCommandPoller {
public:
  DrivingPoller(DiscoveryBonjourSessions *owner,
                std::shared_ptr<DiscoveryBonjourPoller> poller)
      : m_owner{owner}, m_poller{std::move(poller)} {}

  DiscoveryResult start() override { return m_poller->start(); }

  DiscoveryResult wait(bool block) override {
    auto result{m_poller->wait(block)};
    if (!result) {
      m_owner->failAll(result.error());
    }

    return result;
  }

  void wake() noexcept override { m_poller->wake(); }

  DiscoveryResult stop() override {
    auto cleaned{m_owner->cleanupAll()};
    auto stopped{m_poller->stop()};

    return !cleaned ? cleaned : stopped;
  }

private:
  DiscoveryBonjourSessions *m_owner;
  std::shared_ptr<DiscoveryBonjourPoller> m_poller;
};

DiscoveryBonjourSessions::DiscoveryBonjourSessions(
    DiscoveryOptions options, DiscoveryBonjourPoller::Clock clock)
    : m_options{options}, m_clock{clock}, m_client{options, true, clock} {}

DiscoveryBonjourSessions::~DiscoveryBonjourSessions() {
  static_cast<void>(shutdown());
}

std::expected<std::shared_ptr<DiscoveryBonjourSessions>, DiscoveryError>
DiscoveryBonjourSessions::create(DiscoveryOptions options,
                                 DiscoveryBonjourPoller::Clock clock) try {
  if (auto valid{validateOptions(options)}; !valid) {
    return std::unexpected{valid.error()};
  }

  if (!clock) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "missing Bonjour clock"}};
  }

  auto owner{std::shared_ptr<DiscoveryBonjourSessions>{
      new DiscoveryBonjourSessions{options, clock}}};
  owner->m_poller = std::make_shared<DiscoveryBonjourPoller>(
      makeDiscoveryBonjourApi(), options.m_max_native_operations, clock,
      [self = owner.get()] { return self->drive(); });
  auto driving{std::make_shared<DrivingPoller>(owner.get(), owner->m_poller)};
  if (auto attached{owner->m_client.attachPoller(std::move(driving))};
      !attached) {
    return std::unexpected{attached.error()};
  }

  return owner;
} catch (const std::bad_alloc &) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "Bonjour session allocation failed"}};
}

std::expected<DiscoverySessionHandle, DiscoveryError>
DiscoveryBonjourSessions::browse(DiscoveryBonjourQuery query,
                                 DiscoverySessionClient::Handler handler,
                                 std::array<bool, 2> addressFamilies) try {
  if (!validateServiceType(query.m_type) || !validateDomain(query.m_domain) ||
      !discoveryBonjourValidInterface(query.m_interface) ||
      (!addressFamilies[0] && !addressFamilies[1])) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour browser query"}};
  }

  auto output{std::make_shared<std::optional<DiscoverySessionHandle>>()};
  auto result{m_client.dispatchNative(
      [this, query = std::move(query), handler = std::move(handler),
       addressFamilies, output]() mutable -> DiscoveryResult {
        if (m_next_id == 0) {
          return std::unexpected{DiscoveryError{
              DiscoveryErrorKind::kResourceLimit, "discovery ID exhausted"}};
        }

        const auto id{m_next_id++};
        auto entry{std::make_unique<Browser>()};
        entry->m_query = std::move(query);
        entry->m_families = addressFamilies;
        DiscoverySessionClient::BrowserLifecycle lifecycle{
            [this, id]() { return cancel(id); },
            [this, id]() { return reset(id); }};
        m_browsers.emplace(id, std::move(entry));
        auto admitted{[&] {
          try {
            return m_client.browse(std::move(handler), std::move(lifecycle));
          } catch (...) {
            m_browsers.erase(id);

            throw;
          }
        }()};
        if (!admitted) {
          m_browsers.erase(id);

          return std::unexpected{admitted.error()};
        }

        auto &browser{*m_browsers.at(id)};
        browser.m_handle = *admitted;
        *output = *admitted;
        try {
          auto started{start(browser)};
          if (!started) {
            browser.m_error = started.error();
          }
        } catch (const std::bad_alloc &) {
          browser.m_native.reset();
          browser.m_handle.failResourceLimit();
#if defined(__GLIBCXX__)
        } catch (const __cxxabiv1::__forced_unwind &) {
          throw;
#endif
        } catch (...) {
          browser.m_native.reset();
          auto failed{browser.m_handle.backendFailed(
              {DiscoveryErrorKind::kBackend, "Bonjour browser startup threw"})};
          if (!failed &&
              failed.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
            browser.m_handle.failResourceLimit();
          }
        }

        return {};
      })};
  if (!result) {
    return std::unexpected{result.error()};
  }

  return std::move(**output);
} catch (const std::bad_alloc &) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "Bonjour browser allocation failed"}};
}

std::expected<DiscoveryRegistrationHandle, DiscoveryError>
DiscoveryBonjourSessions::advertise(
    DiscoveryAdvertisement advertisement,
    DiscoverySessionClient::RegistrationHandler handler) try {
  if (auto result{
          validateDiscoveryBonjourAdvertisement(m_options, advertisement)};
      !result) {
    return std::unexpected{result.error()};
  }

  auto output{std::make_shared<std::optional<DiscoveryRegistrationHandle>>()};
  auto result{m_client.dispatchNative(
      [this, advertisement = std::move(advertisement),
       handler = std::move(handler), output]() mutable -> DiscoveryResult {
        if (m_next_id == 0) {
          return std::unexpected{DiscoveryError{
              DiscoveryErrorKind::kResourceLimit, "discovery ID exhausted"}};
        }

        const auto id{m_next_id++};
        auto entry{std::make_unique<Registration>()};
        entry->m_advertisement = advertisement;
        m_registrations.emplace(id, std::move(entry));
        auto admitted{[&] {
          try {
            return m_client.advertise(std::move(advertisement),
                                      std::move(handler),
                                      [this, id] { cancelRegistration(id); });
          } catch (...) {
            m_registrations.erase(id);

            throw;
          }
        }()};
        if (!admitted) {
          m_registrations.erase(id);

          return std::unexpected{admitted.error()};
        }

        auto &registration{*m_registrations.at(id)};
        registration.m_handle = *admitted;
        *output = *admitted;
        start(registration);

        return {};
      })};
  if (!result) {
    return std::unexpected{result.error()};
  }

  return std::move(**output);
} catch (const std::bad_alloc &) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Bonjour registration admission allocation failed"}};
}

void DiscoveryBonjourSessions::start(Registration &registration) try {
  const auto generation{registration.m_handle.generation()};
  auto handle{registration.m_handle};
  registration.m_native = std::make_unique<DiscoveryBonjourRegistration>(
      m_poller, registration.m_advertisement, m_options,
      [handle, generation](DiscoveryBonjourRegistrationEvent event) {
        return event == DiscoveryBonjourRegistrationEvent::kEstablished
                   ? handle.established(generation)
                   : handle.collision(generation, "Bonjour name collision");
      },
      [&registration](DiscoveryError error) {
        registration.m_error = std::move(error);
      });
  auto result{registration.m_native->start()};
  if (!result) {
    registration.m_error = result.error();
  }
} catch (const std::bad_alloc &) {
  registration.m_handle.failResourceLimit();
#if defined(__GLIBCXX__)
} catch (const __cxxabiv1::__forced_unwind &) {
  throw;
#endif
} catch (...) {
  registration.m_error = DiscoveryError{DiscoveryErrorKind::kBackend,
                                        "Bonjour registration startup threw"};
}

DiscoveryResult DiscoveryBonjourSessions::start(Browser &browser) {
  browser.m_epoch = browser.m_handle.snapshot().m_delivery.m_epoch;
  browser.m_starting = true;
  struct Guard {
    ~Guard() { m_starting = false; }

    bool &m_starting;
  } guard{browser.m_starting};
  browser.m_native = std::make_unique<DiscoveryBonjourBrowser>(
      m_poller, browser.m_query,
      [this, &browser](DiscoveryBonjourBrowseEvent event) {
        const auto state{browser.m_handle.snapshot()};
        if (browser.m_starting && !state.m_backend_available) {
          if (browser.m_pending.size() >= m_options.m_max_data_events) {
            return DiscoveryResult{std::unexpected{
                DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                               "Bonjour startup event limit reached"}}};
          }

          browser.m_pending.push_back(std::move(event));

          return DiscoveryResult{};
        }

        if (state.m_delivery.m_epoch != browser.m_epoch ||
            state.m_stop_pending ||
            state.m_delivery.m_state != DiscoveryDeliveryState::kRunning) {
          return DiscoveryResult{};
        }

        return onBrowse(browser, std::move(event));
      },
      [&browser](DiscoveryError error) { browser.m_error = std::move(error); });

  return browser.m_native->start();
}

DiscoveryResult
DiscoveryBonjourSessions::onBrowse(Browser &browser,
                                   DiscoveryBonjourBrowseEvent event) {
  for (auto &source : browser.m_sources) {
    if (!sameKey(source.m_key, event.m_key)) {
      continue;
    }

    if (event.m_added) {
      if (!source.m_present) {
        source.m_readd = true;
      }
    } else if (source.m_present) {
      source.m_present = false;
      if (source.m_native) {
        source.m_native->invalidate();
      }
    } else {
      source.m_readd = false;
    }

    return {};
  }

  if (!event.m_added) {
    return {};
  }

  if (browser.m_sources.size() >= m_options.m_max_instances) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "browse contribution limit reached"}};
  }

  auto generation{
      browser.m_handle.observe(browser.m_handle.snapshot().m_delivery.m_epoch,
                               event.m_key, DiscoveryContribution::kUnknown)};
  if (!generation) {
    return std::unexpected{generation.error()};
  }

  browser.m_sources.push_back(
      {std::move(event.m_key), *generation, true, false, {}});

  return {};
}

DiscoveryResult DiscoveryBonjourSessions::cleanup(Browser &browser) {
  browser.m_native.reset();
  browser.m_pending.clear();
  std::optional<DiscoveryError> first;
  for (auto &source : browser.m_sources) {
    if (source.m_native) {
      auto result{source.m_native->cleanup()};
      if (!result && !first) {
        first = result.error();
      }

      source.m_native.reset();
    }
  }

  browser.m_sources.clear();

  return first ? DiscoveryResult{std::unexpected{std::move(*first)}}
               : DiscoveryResult{};
}

DiscoveryResult DiscoveryBonjourSessions::cancel(std::uint64_t id) {
  auto found{m_browsers.find(id)};
  if (found == m_browsers.end()) {
    return {};
  }

  auto result{cleanup(*found->second)};
  m_browsers.erase(found);

  return result;
}

void DiscoveryBonjourSessions::cancelRegistration(std::uint64_t id) {
  m_registrations.erase(id);
}

DiscoveryResult DiscoveryBonjourSessions::reset(std::uint64_t id) {
  auto found{m_browsers.find(id)};
  if (found == m_browsers.end()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Bonjour browser unavailable"}};
  }

  auto &browser{*found->second};
  if (auto result{cleanup(browser)}; !result) {
    return result;
  }

  browser.m_error.reset();
  if (!browser.m_handle.snapshot().m_backend_available) {
    return {};
  }

  auto result{start(browser)};
  if (!result) {
    browser.m_error = result.error();
  }

  return result;
}

void DiscoveryBonjourSessions::propagateUnavailable(
    const DiscoveryError &error) {
  for (auto &[id, registration] : m_registrations) {
    static_cast<void>(id);
    const auto state{registration->m_handle.status().m_state};
    if (!registration->m_error &&
        (state == DiscoveryRegistrationState::kRegistering ||
         state == DiscoveryRegistrationState::kEstablished)) {
      registration->m_error = error;
    }
  }

  for (auto &[id, browser] : m_browsers) {
    static_cast<void>(id);
    const auto state{browser->m_handle.snapshot()};
    if (!browser->m_error && !state.m_stop_pending &&
        state.m_backend_available &&
        state.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
      browser->m_error = error;
    }
  }
}

DiscoveryBonjourPoller::DriveResult DiscoveryBonjourSessions::drive() try {
  DiscoveryBonjourPoller::Deadline next;
  for (auto it{m_registrations.begin()}; it != m_registrations.end();) {
    auto &registration{*it->second};
    auto state{registration.m_handle.status().m_state};
    if (state == DiscoveryRegistrationState::kUnavailable &&
        registration.m_retry_due && *registration.m_retry_due <= m_clock()) {
      registration.m_retry_due.reset();
      auto available{registration.m_handle.backendAvailable()};
      if (!available) {
        registration.m_error = available.error();
      } else {
        start(registration);
      }

      state = registration.m_handle.status().m_state;
    }

    if (registration.m_native && !registration.m_error) {
      if (auto status{registration.m_native->status()}; !status) {
        registration.m_error = status.error();
      }
    }

    if (registration.m_error) {
      auto error{std::move(*registration.m_error)};
      registration.m_error.reset();
      registration.m_native.reset();
      if (state == DiscoveryRegistrationState::kRegistering ||
          state == DiscoveryRegistrationState::kEstablished ||
          state == DiscoveryRegistrationState::kUnavailable) {
        if (error.m_native_category ==
            DiscoveryNativeErrorCategory::kUnavailable) {
          propagateUnavailable(error);
          registration.m_error.reset();
          auto lost{registration.m_handle.backendLost()};
          if (!lost) {
            if (lost.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
              registration.m_handle.failResourceLimit();
            } else if (lost.error().m_kind != DiscoveryErrorKind::kStopped) {
              return std::unexpected{lost.error()};
            }
          } else {
            const auto now{m_clock()};
            const auto delay{
                std::chrono::seconds{registration.m_retry_seconds}};
            registration.m_retry_due =
                now > std::chrono::steady_clock::time_point::max() - delay
                    ? std::chrono::steady_clock::time_point::max()
                    : now + delay;
            registration.m_retry_seconds =
                std::min(registration.m_retry_seconds * 2, 30u);
            next = std::chrono::steady_clock::time_point::min();
          }
        } else {
          auto failed{registration.m_handle.backendFailed(std::move(error))};
          if (!failed &&
              failed.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
            registration.m_handle.failResourceLimit();
          } else if (!failed &&
                     failed.error().m_kind != DiscoveryErrorKind::kStopped) {
            return std::unexpected{failed.error()};
          }
        }
      }

      state = registration.m_handle.status().m_state;
    }

    if (state == DiscoveryRegistrationState::kEstablished) {
      registration.m_retry_seconds = 1;
    }

    if (state == DiscoveryRegistrationState::kUnavailable &&
        registration.m_retry_due &&
        (!next || *registration.m_retry_due < *next)) {
      next = registration.m_retry_due;
    }

    if (state == DiscoveryRegistrationState::kStopped ||
        state == DiscoveryRegistrationState::kOverflow ||
        state == DiscoveryRegistrationState::kFailed ||
        state == DiscoveryRegistrationState::kCollision) {
      registration.m_native.reset();
    }

    if (state == DiscoveryRegistrationState::kStopped) {
      it = m_registrations.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it{m_browsers.begin()}; it != m_browsers.end();) {
    auto &browser{*it->second};
    auto state{browser.m_handle.snapshot()};
    if (state.m_stop_pending ||
        state.m_delivery.m_state != DiscoveryDeliveryState::kRunning) {
      if (auto result{cleanup(browser)}; !result) {
        return std::unexpected{result.error()};
      }

      if (state.m_stop_pending) {
        it = m_browsers.erase(it);
      } else {
        ++it;
      }

      continue;
    }

    if (!state.m_backend_available) {
      if (browser.m_retry_due && *browser.m_retry_due <= m_clock()) {
        browser.m_retry_due.reset();
        auto started{start(browser)};
        if (!started) {
          browser.m_error = started.error();
        } else {
          auto available{
              browser.m_handle.backendAvailable(state.m_delivery.m_epoch)};
          if (!available) {
            browser.m_error = available.error();
          } else {
            browser.m_epoch = browser.m_handle.snapshot().m_delivery.m_epoch;
            browser.m_retry_seconds = 1;
            auto pending{std::move(browser.m_pending)};
            for (auto &event : pending) {
              auto result{onBrowse(browser, std::move(event))};
              if (!result) {
                browser.m_error = result.error();
                break;
              }
            }
          }
        }
      } else {
        if (browser.m_retry_due && (!next || *browser.m_retry_due < *next)) {
          next = browser.m_retry_due;
        }

        ++it;
        continue;
      }

      state = browser.m_handle.snapshot();
    }

    const auto epoch{state.m_delivery.m_epoch};
    if (browser.m_native && !browser.m_error) {
      if (auto status{browser.m_native->status()}; !status) {
        browser.m_error = status.error();
      }
    }

    if (!browser.m_error) {
      auto timers{browser.m_handle.processTimers(epoch)};
      auto due{browser.m_handle.takeDueRetries(epoch)};
      if (!timers) {
        browser.m_error = timers.error();
      } else if (!due) {
        browser.m_error = due.error();
      } else {
        for (auto source{browser.m_sources.begin()};
             source != browser.m_sources.end();) {
          if (!source->m_present) {
            if (source->m_native) {
              auto result{source->m_native->cleanup()};
              source->m_native.reset();
              if (!result) {
                browser.m_error = result.error();
                break;
              }
            }

            auto removed{browser.m_handle.removeObservation(
                epoch, source->m_key, DiscoveryContribution::kUnknown,
                source->m_generation)};
            if (!removed) {
              browser.m_error = removed.error();
              break;
            }

            if (!source->m_readd) {
              source = browser.m_sources.erase(source);
              continue;
            }

            auto added{browser.m_handle.observe(
                epoch, source->m_key, DiscoveryContribution::kUnknown)};
            if (!added) {
              browser.m_error = added.error();
              break;
            }

            source->m_generation = *added;
            source->m_present = true;
            source->m_readd = false;
          }

          if (!source->m_native) {
            source->m_native = std::make_unique<DiscoveryBonjourContribution>(
                browser.m_handle, epoch, source->m_key, source->m_generation,
                m_options, m_poller, browser.m_families);
            auto result{source->m_native->start()};
            if (!result &&
                (result.error().m_kind != DiscoveryErrorKind::kBackend ||
                 result.error().m_native_category ==
                     DiscoveryNativeErrorCategory::kUnavailable ||
                 source->m_native->cleaned())) {
              browser.m_error = result.error();
              break;
            }
          }

          ++source;
        }

        for (const auto &retry : *due) {
          for (auto &source : browser.m_sources) {
            if (source.m_generation == retry.m_source && source.m_native) {
              auto result{source.m_native->retry(retry)};
              if (!result &&
                  (result.error().m_kind != DiscoveryErrorKind::kBackend ||
                   result.error().m_native_category ==
                       DiscoveryNativeErrorCategory::kUnavailable ||
                   source.m_native->cleaned())) {
                browser.m_error = result.error();
              }

              break;
            }
          }
        }

        for (auto &source : browser.m_sources) {
          if (browser.m_error || !source.m_native) {
            continue;
          }

          auto result{source.m_native->drive()};
          if (!result &&
              (result.error().m_kind != DiscoveryErrorKind::kBackend ||
               result.error().m_native_category ==
                   DiscoveryNativeErrorCategory::kUnavailable ||
               source.m_native->cleaned())) {
            browser.m_error = result.error();
          }
        }
      }
    }

    if (browser.m_error) {
      auto error{std::move(*browser.m_error)};
      browser.m_error.reset();
      if (auto result{cleanup(browser)}; !result) {
        error.m_message += "; cleanup failed: " + result.error().m_message;
      }

      const auto current{browser.m_handle.snapshot()};
      if (!current.m_stop_pending &&
          current.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
        const bool recoverable{error.m_native_category ==
                               DiscoveryNativeErrorCategory::kUnavailable};
        if (recoverable) {
          propagateUnavailable(error);
          browser.m_error.reset();
          next = std::chrono::steady_clock::time_point::min();
        }

        auto reported{browser.m_handle.backendFailed(std::move(error))};
        if (!reported) {
          const auto after{browser.m_handle.snapshot()};
          if (!after.m_stop_pending &&
              after.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
            return std::unexpected{reported.error()};
          }
        } else if (recoverable) {
          const auto now{m_clock()};
          const auto delay{std::chrono::seconds{browser.m_retry_seconds}};
          browser.m_retry_due =
              now > std::chrono::steady_clock::time_point::max() - delay
                  ? std::chrono::steady_clock::time_point::max()
                  : now + delay;
          browser.m_retry_seconds = std::min(browser.m_retry_seconds * 2, 30u);
          if (!next || *browser.m_retry_due < *next) {
            next = browser.m_retry_due;
          }
        }
      }
    } else {
      auto deadline{browser.m_handle.nextDeadline(epoch)};
      if (!deadline) {
        const auto current{browser.m_handle.snapshot()};
        if (!current.m_stop_pending &&
            current.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
          browser.m_error = deadline.error();
          next = std::chrono::steady_clock::time_point::min();
        }
      } else if (*deadline && (!next || **deadline < *next)) {
        next = *deadline;
      }
    }

    ++it;
  }

  return next;
} catch (const std::bad_alloc &) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                     "Bonjour browser drive allocation failed"}};
}

void DiscoveryBonjourSessions::failAll(const DiscoveryError &error) try {
  for (auto &[id, registration] : m_registrations) {
    static_cast<void>(id);
    registration->m_native.reset();
    const auto state{registration->m_handle.status().m_state};
    if (state == DiscoveryRegistrationState::kRegistering ||
        state == DiscoveryRegistrationState::kEstablished ||
        state == DiscoveryRegistrationState::kUnavailable) {
      auto result{registration->m_handle.backendFailed(error)};
      if (!result &&
          result.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
        registration->m_handle.failResourceLimit();
      }
    }
  }

  for (auto &[id, browser] : m_browsers) {
    static_cast<void>(id);
    auto reported{error};
    if (auto result{cleanup(*browser)}; !result) {
      reported.m_message += "; cleanup failed: " + result.error().m_message;
    }

    const auto state{browser->m_handle.snapshot()};
    if (!state.m_stop_pending &&
        state.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
      auto result{browser->m_handle.backendFailed(std::move(reported))};
      if (!result &&
          result.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
        browser->m_handle.failResourceLimit(true);
      }
    }
  }
} catch (const std::bad_alloc &) {
  for (auto &[id, registration] : m_registrations) {
    static_cast<void>(id);
    registration->m_native.reset();
    registration->m_handle.failResourceLimit();
  }

  for (auto &[id, browser] : m_browsers) {
    static_cast<void>(id);
    browser->m_native.reset();
    for (auto &source : browser->m_sources) {
      if (source.m_native) {
        source.m_native->releaseNative();
      }
    }

    browser->m_handle.failResourceLimit(true);
  }
}

DiscoveryResult DiscoveryBonjourSessions::cleanupAll() {
  std::optional<DiscoveryError> first;
  for (auto &[id, registration] : m_registrations) {
    static_cast<void>(id);
    registration->m_native.reset();
    const auto state{registration->m_handle.status().m_state};
    if (state == DiscoveryRegistrationState::kRegistering ||
        state == DiscoveryRegistrationState::kEstablished ||
        state == DiscoveryRegistrationState::kUnavailable) {
      auto result{registration->m_handle.backendFailed(
          {DiscoveryErrorKind::kBackend, "Bonjour worker stopped"})};
      if (!result && !first) {
        first = result.error();
      }
    }

    if (auto result{registration->m_handle.nativeCleanupComplete()};
        !result && !first) {
      first = result.error();
    }
  }

  for (auto &[id, browser] : m_browsers) {
    static_cast<void>(id);
    if (auto result{cleanup(*browser)}; !result && !first) {
      first = result.error();
    }

    const auto state{browser->m_handle.snapshot()};
    if (!state.m_stop_pending &&
        state.m_delivery.m_state == DiscoveryDeliveryState::kRunning) {
      auto result{browser->m_handle.backendFailed(
          {DiscoveryErrorKind::kBackend, "Bonjour worker stopped"})};
      if (!result && !first) {
        first = result.error();
      }
    }

    if (auto result{browser->m_handle.nativeCleanupComplete()};
        !result && !first) {
      first = result.error();
    }
  }

  return first ? DiscoveryResult{std::unexpected{std::move(*first)}}
               : DiscoveryResult{};
}

DiscoveryResult DiscoveryBonjourSessions::shutdown() {
  return m_client.shutdown();
}

} // namespace dmn::detail
