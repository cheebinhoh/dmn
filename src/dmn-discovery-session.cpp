/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-session.cpp
 * @brief Private integration of browser state and bounded worker lifetimes.
 */

#include "dmn-discovery-session.hpp"
#include "dmn-discovery-slots.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <utility>

namespace dmn::detail {

struct DiscoveryBarrierOperation {
  DiscoveryBarrierOperation() : m_completion{m_promise.get_future().share()} {}

  std::promise<DiscoveryResult> m_promise;
  std::shared_future<DiscoveryResult> m_completion;
  std::size_t m_joined{0};
};

struct DiscoverySessionState {
  explicit DiscoverySessionState(const DiscoveryOptions &options)
      : m_options{options}, m_slots{options.m_max_native_operations} {}

  std::mutex m_mutex;
  std::mutex m_barrier;
  std::shared_ptr<DiscoveryBarrierOperation> m_reset_operation;
  std::shared_ptr<DiscoveryBarrierOperation> m_stop_operation;
  std::uint64_t m_id;
  DiscoveryOptions m_options;
  DiscoveryBrowser::Clock m_clock;
  std::shared_ptr<DiscoveryCommandWorker> m_commands;
  DiscoveryDeliveryHandle m_delivery;
  std::unique_ptr<DiscoveryBrowser> m_browser;
  DiscoveryNativeSlots m_slots;
  std::optional<DiscoveryNativeSlots::Lease> m_browse_slot;
  std::map<std::uint64_t, DiscoveryNativeSlots::Lease> m_native_operations;
  std::uint64_t m_next_operation{1};
  DiscoverySnapshot m_cached{1, 0, {}};
  bool m_stopped{false};
  std::atomic<bool> m_stop_requested{false};
  bool m_released{false};
  bool m_overflow{false};
  bool m_backend_available{true};
  std::optional<DiscoveryError> m_cleanup_error;
};

namespace {

DiscoveryError invalidSession() {
  return {DiscoveryErrorKind::kInvalidArgument, "invalid discovery session"};
}

/** @brief Free fake native objects and their quota leases synchronously. */
void cancelNative(DiscoverySessionState &session) {
  session.m_native_operations.clear();
  session.m_browse_slot.reset();
}

/** @brief Invalidate the whole fake browser after a resource overflow. */
void latchOverflow(DiscoverySessionState &session) {
  static_cast<void>(session.m_delivery.failOverflow());
  session.m_overflow = true;
  cancelNative(session);
  session.m_cached.m_candidates.clear();
}

/** @brief Finalize a model mutation after all synchronous emissions finish. */
DiscoveryResult finishMutation(DiscoverySessionState &session,
                               DiscoveryResult result) {
  if (!result && result.error().m_kind == DiscoveryErrorKind::kResourceLimit) {
    latchOverflow(session);
  }
  if (session.m_overflow) {
    cancelNative(session);
    session.m_browser.reset();
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session overflow"}};
  }
  return result;
}

/** @brief Reserve the fake native browse reference before starting it. */
DiscoveryResult startBrowse(DiscoverySessionState &session) {
  auto lease{session.m_slots.reserve()};
  if (!lease) {
    latchOverflow(session);
    return std::unexpected{lease.error()};
  }
  session.m_browse_slot.emplace(std::move(*lease));
  return {};
}

/** @brief Stop fake native work without waiting for a running callback. */
DiscoveryResult stopSession(DiscoverySessionState &session) {
  session.m_stop_requested.store(true, std::memory_order_release);
  if (auto result{session.m_delivery.beginStop()}; !result) {
    return result;
  }
  session.m_stopped = true;
  cancelNative(session);
  session.m_browser.reset();
  session.m_cached.m_candidates.clear();
  return {};
}

/**
 * @brief Create the serialized model; its handler runs with the session lock.
 */
std::unique_ptr<DiscoveryBrowser>
makeBrowser(const std::shared_ptr<DiscoverySessionState> &session) {
  std::weak_ptr<DiscoverySessionState> weak{session};
  return std::make_unique<DiscoveryBrowser>(
      session->m_options,
      [weak](DiscoveryEvent event) {
        const auto state{weak.lock()};
        if (!state || !state->m_browser) {
          return;
        }

        auto snapshot{state->m_browser->snapshot()};
        const auto epoch{state->m_delivery.snapshot().m_epoch};
        const auto result{state->m_delivery.postData(epoch, std::move(event))};
        const auto delivery{state->m_delivery.snapshot()};
        snapshot.m_epoch = delivery.m_epoch;
        snapshot.m_sequence = delivery.m_sequence;
        if (!result) {
          state->m_overflow =
              delivery.m_state == DiscoveryDeliveryState::kOverflow;
          snapshot.m_candidates.clear();
        }
        state->m_cached = std::move(snapshot);
      },
      session->m_clock);
}

/**
 * @brief Reject work for stopped, overflowed or stale fake native sources.
 */
DiscoveryResult validateSession(const DiscoverySessionState &session,
                                std::optional<std::uint64_t> epoch = {},
                                bool requireBackend = true) {
  const auto delivery{session.m_delivery.snapshot()};
  if (session.m_stop_requested.load(std::memory_order_acquire) ||
      session.m_stopped ||
      delivery.m_state == DiscoveryDeliveryState::kStopped) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "discovery session stopped"}};
  }
  if (session.m_overflow ||
      delivery.m_state == DiscoveryDeliveryState::kOverflow) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session overflow"}};
  }
  if (epoch && *epoch != delivery.m_epoch) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale discovery epoch"}};
  }
  if (requireBackend && !session.m_backend_available) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "discovery backend unavailable"}};
  }

  return {};
}

} // namespace

DiscoverySessionHandle::DiscoverySessionHandle(
    std::shared_ptr<DiscoverySessionState> state)
    : m_state{std::move(state)} {}

std::expected<std::uint64_t, DiscoveryError>
DiscoverySessionHandle::observe(std::uint64_t epoch, DiscoveryServiceKey key,
                                DiscoveryContribution contribution) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  using Output = std::expected<std::uint64_t, DiscoveryError>;
  auto output{std::make_shared<std::optional<Output>>()};
  auto result{m_state->m_commands->execute([state = m_state, epoch,
                                            key = std::move(key), contribution,
                                            output]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateSession(*state, epoch)}; !result) {
      return result;
    }

    *output = state->m_browser->addObservation(key, contribution);
    return finishMutation(*state, **output ? DiscoveryResult{}
                                           : DiscoveryResult{std::unexpected{
                                                 (*output)->error()}});
  })};
  if (!result) {
    return std::unexpected{result.error()};
  }

  return std::move(**output);
}

DiscoveryResult DiscoverySessionHandle::removeObservation(
    std::uint64_t epoch, DiscoveryServiceKey key,
    DiscoveryContribution contribution, std::uint64_t generation) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute([state = m_state, epoch,
                                       key = std::move(key), contribution,
                                       generation]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateSession(*state, epoch)}; !result) {
      return result;
    }
    const auto removed{
        state->m_browser->removeObservation(key, contribution, generation)};
    if (!removed) {
      return finishMutation(*state, std::unexpected{DiscoveryError{
                                        DiscoveryErrorKind::kInvalidArgument,
                                        "stale browse contribution"}});
    }
    return finishMutation(*state, {});
  });
}

DiscoveryResult DiscoverySessionHandle::updateRecords(
    std::uint64_t epoch, DiscoveryServiceKey key,
    DiscoveryContribution contribution, std::uint64_t generation,
    std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update)
    const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  if (!update) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty record update"}};
  }
  return m_state->m_commands->execute(
      [state = m_state, epoch, key = std::move(key), contribution, generation,
       update = std::move(update)]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }
        return finishMutation(
            *state, state->m_browser->updateRecords(key, contribution,
                                                    generation, update));
      });
}

std::expected<std::uint64_t, DiscoveryError>
DiscoverySessionHandle::refresh(DiscoveryServiceKey key) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  using Output = std::expected<std::uint64_t, DiscoveryError>;
  auto output{std::make_shared<std::optional<Output>>()};
  auto result{m_state->m_commands->execute(
      [state = m_state, key = std::move(key), output]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state)}; !result) {
          return result;
        }

        *output = state->m_browser->refresh(key);
        return finishMutation(
            *state, **output
                        ? DiscoveryResult{}
                        : DiscoveryResult{std::unexpected{(*output)->error()}});
      })};
  if (!result) {
    return std::unexpected{result.error()};
  }

  return std::move(**output);
}

DiscoveryResult
DiscoverySessionHandle::processTimers(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute([state = m_state,
                                       epoch]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateSession(*state, epoch)}; !result) {
      return result;
    }
    return finishMutation(*state, state->m_browser->processRefreshDeadlines());
  });
}

DiscoveryResult DiscoverySessionHandle::failAttempt(
    std::uint64_t epoch, DiscoveryServiceKey key,
    DiscoveryContribution contribution, std::uint64_t generation,
    DiscoveryAttempt attempt, std::uint64_t operationGeneration,
    std::uint64_t resolutionGeneration, bool recoverable,
    std::string message) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute(
      [state = m_state, epoch, key = std::move(key), contribution, generation,
       attempt, operationGeneration, resolutionGeneration, recoverable,
       message = std::move(message)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }
        return finishMutation(*state,
                              state->m_browser->failAttempt(
                                  key, contribution, generation, attempt,
                                  operationGeneration, resolutionGeneration,
                                  recoverable, std::move(message)));
      });
}

DiscoveryResult DiscoverySessionHandle::succeedAttempt(
    std::uint64_t epoch, DiscoveryServiceKey key,
    DiscoveryContribution contribution, std::uint64_t generation,
    DiscoveryAttempt attempt, std::uint64_t operationGeneration,
    std::uint64_t resolutionGeneration) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute(
      [state = m_state, epoch, key = std::move(key), contribution, generation,
       attempt, operationGeneration,
       resolutionGeneration]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }
        return finishMutation(*state,
                              state->m_browser->succeedAttempt(
                                  key, contribution, generation, attempt,
                                  operationGeneration, resolutionGeneration));
      });
}

std::expected<std::vector<DiscoveryAttemptId>, DiscoveryError>
DiscoverySessionHandle::takeDueRetries(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  using Output = std::expected<std::vector<DiscoveryAttemptId>, DiscoveryError>;
  auto output{std::make_shared<std::optional<Output>>()};
  auto result{m_state->m_commands->execute(
      [state = m_state, epoch, output]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto valid{validateSession(*state, epoch)}; !valid) {
          return valid;
        }
        *output = state->m_browser->takeDueRetries();
        return {};
      })};
  if (!result) {
    return std::unexpected{result.error()};
  }
  return std::move(**output);
}

std::expected<std::uint64_t, DiscoveryError>
DiscoverySessionHandle::beginNative(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  auto output{std::make_shared<std::optional<std::uint64_t>>()};
  auto result{m_state->m_commands->execute([state = m_state, epoch,
                                            output]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateSession(*state, epoch)}; !result) {
      return result;
    }
    if (state->m_next_operation == 0) {
      latchOverflow(*state);
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "native operation ID exhausted"}};
    }
    auto lease{state->m_slots.reserve()};
    if (!lease) {
      latchOverflow(*state);
      return std::unexpected{lease.error()};
    }
    const auto id{state->m_next_operation++};
    state->m_native_operations.emplace(id, std::move(*lease));
    *output = id;
    return {};
  })};
  if (!result) {
    return std::unexpected{result.error()};
  }
  return **output;
}

DiscoveryResult
DiscoverySessionHandle::finishNative(std::uint64_t epoch,
                                     std::uint64_t operation) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute([state = m_state, epoch,
                                       operation]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateSession(*state, epoch)}; !result) {
      return result;
    }
    if (state->m_native_operations.erase(operation) == 0) {
      return std::unexpected{DiscoveryError{
          DiscoveryErrorKind::kInvalidArgument, "unknown native operation"}};
    }
    return {};
  });
}

DiscoveryResult DiscoverySessionHandle::backendLost(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute(
      [state = m_state, epoch]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch, false)}; !result) {
          return result;
        }
        if (!state->m_backend_available) {
          return {};
        }
        auto replacement{makeBrowser(state)};
        auto result{state->m_delivery.backendLost(epoch)};
        cancelNative(*state);
        state->m_browser = std::move(replacement);
        state->m_cached = state->m_browser->snapshot();
        state->m_backend_available = false;
        if (!result) {
          state->m_overflow = state->m_delivery.snapshot().m_state ==
                              DiscoveryDeliveryState::kOverflow;
        }
        return result;
      });
}

DiscoveryResult
DiscoverySessionHandle::backendAvailable(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  return m_state->m_commands->execute(
      [state = m_state, epoch]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch, false)}; !result) {
          return result;
        }
        if (state->m_backend_available) {
          return {};
        }
        if (auto result{startBrowse(*state)}; !result) {
          return result;
        }
        auto result{state->m_delivery.backendAvailable(epoch)};
        if (!result) {
          state->m_overflow = state->m_delivery.snapshot().m_state ==
                              DiscoveryDeliveryState::kOverflow;
          cancelNative(*state);
          return result;
        }
        state->m_backend_available = true;
        return {};
      });
}

DiscoveryResult DiscoverySessionHandle::clearCache() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  if (m_state->m_delivery.inCallbackContext() ||
      m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant reset barrier"}};
  }

  std::shared_ptr<DiscoveryBarrierOperation> operation{};
  bool leader{false};
  {
    std::lock_guard lock{m_state->m_barrier};
    if (m_state->m_stop_operation) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kBackend, "session stop pending"}};
    }
    if (m_state->m_reset_operation) {
      operation = m_state->m_reset_operation;
      ++operation->m_joined;
    } else {
      operation = std::make_shared<DiscoveryBarrierOperation>();
      m_state->m_reset_operation = operation;
      leader = true;
    }
  }
  if (!leader) {
    return operation->m_completion.get();
  }

  DiscoveryResult result{};
  try {
    auto epoch{std::make_shared<std::optional<std::uint64_t>>()};
    auto submitted{m_state->m_commands->submitControl(
        m_state->m_id, DiscoveryCommandControl::kReset,
        [state = m_state, epoch]() -> DiscoveryResult {
          std::lock_guard lock{state->m_mutex};
          if (state->m_stopped) {
            return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                                  "session stopped"}};
          }

          auto replacement{makeBrowser(state)};
          cancelNative(*state);
          const auto newEpoch{state->m_delivery.beginReset()};
          if (!newEpoch) {
            return std::unexpected{newEpoch.error()};
          }
          state->m_browser = std::move(replacement);
          state->m_cached = state->m_browser->snapshot();
          const auto delivery{state->m_delivery.snapshot()};
          state->m_cached.m_epoch = delivery.m_epoch;
          state->m_cached.m_sequence = delivery.m_sequence;
          state->m_overflow = false;
          if (state->m_backend_available) {
            if (auto started{startBrowse(*state)}; !started) {
              return started;
            }
          }
          *epoch = *newEpoch;
          return {};
        })};
    if (!submitted) {
      result = std::unexpected{submitted.error()};
    } else {
      result = submitted->get();
      if (result) {
        result = m_state->m_delivery.awaitReset(**epoch);
      }
    }
  } catch (...) {
    result = std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "reset barrier failed"}};
  }
  {
    std::lock_guard lock{m_state->m_barrier};
    operation->m_promise.set_value(result);
    m_state->m_reset_operation.reset();
  }
  return result;
}

DiscoveryResult DiscoverySessionHandle::stop() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }
  if (m_state->m_delivery.inCallbackContext() ||
      m_state->m_commands->inWorkerContext()) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "reentrant stop barrier"}};
  }

  std::shared_ptr<DiscoveryBarrierOperation> operation{};
  std::shared_ptr<DiscoveryBarrierOperation> reset{};
  bool leader{false};
  {
    std::lock_guard lock{m_state->m_barrier};
    if (m_state->m_stop_operation) {
      operation = m_state->m_stop_operation;
    } else {
      operation = std::make_shared<DiscoveryBarrierOperation>();
      m_state->m_stop_operation = operation;
      m_state->m_stop_requested.store(true, std::memory_order_release);
      reset = m_state->m_reset_operation;
      leader = true;
    }
  }
  if (!leader) {
    return operation->m_completion.get();
  }

  DiscoveryResult result{};
  try {
    if (reset) {
      reset->m_completion.wait();
    }
    bool stopped{false};
    {
      std::lock_guard lock{m_state->m_mutex};
      stopped = m_state->m_stopped;
    }
    if (!stopped) {
      auto submitted{m_state->m_commands->submitControl(
          m_state->m_id, DiscoveryCommandControl::kStop,
          [state = m_state]() -> DiscoveryResult {
            std::lock_guard lock{state->m_mutex};
            return stopSession(*state);
          })};
      result = submitted ? submitted->get()
                         : DiscoveryResult{std::unexpected{submitted.error()}};
    }
    if (result) {
      result = m_state->m_delivery.awaitStop();
      if (result) {
        std::lock_guard lock{m_state->m_mutex};
        m_state->m_released = true;
      }
    }
  } catch (...) {
    result = std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "stop barrier failed"}};
  }
  {
    std::lock_guard lock{m_state->m_barrier};
    operation->m_promise.set_value(result);
    m_state->m_stop_operation.reset();
  }
  return result;
}

DiscoverySessionSnapshot DiscoverySessionHandle::snapshot() const {
  if (!m_state) {
    return {{0, 0, DiscoveryDeliveryState::kStopped, false},
            {0, 0, {}},
            false,
            0,
            {},
            0,
            false};
  }

  std::size_t joined{0};
  {
    std::lock_guard lock{m_state->m_barrier};
    if (m_state->m_reset_operation) {
      joined = m_state->m_reset_operation->m_joined;
    }
  }
  std::lock_guard lock{m_state->m_mutex};
  auto browser{m_state->m_cached};
  const auto delivery{m_state->m_delivery.snapshot()};
  browser.m_epoch = delivery.m_epoch;
  browser.m_sequence = delivery.m_sequence;
  if (delivery.m_state != DiscoveryDeliveryState::kRunning) {
    browser.m_candidates.clear();
  }

  return {delivery,
          std::move(browser),
          m_state->m_backend_available,
          m_state->m_slots.inUse(),
          m_state->m_cleanup_error,
          joined,
          m_state->m_stop_requested.load(std::memory_order_acquire)};
}

DiscoverySessionClient::DiscoverySessionClient(const DiscoveryOptions &options,
                                               bool backendAvailable,
                                               DiscoveryBrowser::Clock clock)
    : m_options{options}, m_initial_backend_available{backendAvailable},
      m_clock{std::move(clock)} {}

DiscoverySessionClient::~DiscoverySessionClient() {
  static_cast<void>(shutdown());
}

DiscoveryResult DiscoverySessionClient::dispatchNative(
    DiscoveryCommandWorker::Command callback) {
  std::shared_ptr<DiscoveryCommandWorker> commands{};
  {
    std::lock_guard lock{m_mutex};
    if (m_closing || !m_commands) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "discovery worker unavailable"}};
    }
    commands = m_commands;
  }
  return commands->execute(std::move(callback));
}

std::expected<DiscoverySessionHandle, DiscoveryError>
DiscoverySessionClient::browse(Handler handler) {
  if (auto result{validateOptions(m_options)}; !result) {
    return std::unexpected{result.error()};
  }
  if (!m_clock) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery clock"}};
  }
  std::lock_guard lock{m_mutex};
  if (m_closing) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "discovery client stopped"}};
  }
  if (m_next_session == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session ID exhausted"}};
  }

  if (!m_commands) {
    auto commands{std::make_shared<DiscoveryCommandWorker>(
        m_options.m_max_commands, m_options.m_max_browsers)};
    auto dispatcher{
        std::make_unique<DiscoveryDispatcher>(m_options.m_max_browsers)};
    m_commands = std::move(commands);
    m_dispatcher = std::move(dispatcher);
  }
  std::erase_if(m_sessions, [](const auto &session) {
    std::lock_guard sessionLock{session->m_mutex};
    return session->m_released;
  });
  auto session{std::make_shared<DiscoverySessionState>(m_options)};
  session->m_id = m_next_session++;
  session->m_commands = m_commands;
  session->m_clock = m_clock;
  session->m_backend_available = m_initial_backend_available;
  std::weak_ptr<DiscoverySessionState> weak{session};
  auto delivery{m_dispatcher->create(
      m_options.m_max_data_events, m_options.m_max_control_events,
      [weak, handler = std::move(handler)](DiscoveryDelivery event) {
        try {
          if (handler) {
            handler(std::move(event));
          }
        } catch (...) {
          if (auto state{weak.lock()}) {
            auto submitted{state->m_commands->submitControl(
                state->m_id, DiscoveryCommandControl::kStop,
                [state]() -> DiscoveryResult {
                  std::lock_guard lock{state->m_mutex};
                  return stopSession(*state);
                })};
            DiscoveryResult result{submitted ? submitted->get()
                                             : DiscoveryResult{std::unexpected{
                                                   submitted.error()}}};
            if (!result) {
              std::lock_guard lock{state->m_mutex};
              state->m_cleanup_error = result.error();
            }
          }
          throw;
        }
      })};
  if (!delivery) {
    return std::unexpected{delivery.error()};
  }
  session->m_delivery = std::move(*delivery);
  session->m_browser = makeBrowser(session);
  m_sessions.push_back(session);
  if (session->m_backend_available) {
    if (auto result{startBrowse(*session)}; !result) {
      return std::unexpected{result.error()};
    }
  }
  if (!session->m_backend_available) {
    auto result{
        session->m_delivery.postControl(session->m_delivery.snapshot().m_epoch,
                                        DiscoveryControl::kBackendUnavailable)};
    if (!result) {
      return std::unexpected{result.error()};
    }
  }

  return DiscoverySessionHandle{std::move(session)};
}

DiscoveryResult DiscoverySessionClient::shutdown() {
  std::vector<std::shared_ptr<DiscoverySessionState>> sessions{};
  std::shared_ptr<DiscoveryCommandWorker> commands{};
  DiscoveryDispatcher *dispatcher{nullptr};
  {
    std::unique_lock lock{m_mutex};
    if ((m_dispatcher && m_dispatcher->inCallbackContext()) ||
        (m_commands && m_commands->inWorkerContext())) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "reentrant shutdown barrier"}};
    }
    if (m_closing) {
      m_changed.wait(lock, [&] { return m_closed; });
      if (m_shutdown_error) {
        return std::unexpected{*m_shutdown_error};
      }
      return {};
    }
    m_closing = true;
    sessions = m_sessions;
    commands = m_commands;
    dispatcher = m_dispatcher.get();
  }

  std::optional<DiscoveryError> error{};
  for (const auto &session : sessions) {
    auto result{DiscoverySessionHandle{session}.stop()};
    if (!result && !error) {
      error = result.error();
    }
  }
  if (commands) {
    if (auto result{commands->shutdown()}; !result && !error) {
      error = result.error();
    }
  }
  if (dispatcher) {
    if (auto result{dispatcher->shutdown()}; !result && !error) {
      error = result.error();
    }
  }
  {
    std::lock_guard lock{m_mutex};
    m_shutdown_error = error;
    m_closed = true;
    m_changed.notify_all();
  }
  if (error) {
    return std::unexpected{*error};
  }

  return {};
}

} // namespace dmn::detail
