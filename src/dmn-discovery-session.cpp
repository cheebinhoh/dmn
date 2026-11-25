/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-session.cpp
 * @brief Private integration of browser state and bounded worker lifetimes.
 */

#include "dmn-discovery-session.hpp"
#include "dmn-discovery-slots.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {

static_assert(std::is_nothrow_move_constructible_v<DiscoveryResult>);
static_assert(std::is_nothrow_move_assignable_v<DiscoveryResult>);

struct DiscoveryBarrierOperation {
  DiscoveryBarrierOperation() : m_completion{m_promise.get_future().share()} {}

  std::promise<DiscoveryResult> m_promise;
  std::shared_future<DiscoveryResult> m_completion;
  DiscoveryResult m_cancelled{std::unexpected{DiscoveryError{
      DiscoveryErrorKind::kCancelled, "barrier caller cancelled"}}};
  DiscoveryResult m_allocation_failure{std::unexpected{DiscoveryError{
      DiscoveryErrorKind::kResourceLimit, "barrier allocation failed"}}};
  DiscoveryResult m_failure{std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kBackend, "barrier failed"}}};
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
  std::atomic<bool> m_explicit_stop{false};
  bool m_released{false};
  bool m_overflow{false};
  bool m_backend_available{true};
  std::optional<DiscoveryError> m_cleanup_error;
  std::optional<DiscoveryError> m_stop_error;
  DiscoveryError m_cleanup_allocation_failure{
      DiscoveryErrorKind::kResourceLimit, "callback cleanup allocation failed"};
  std::unique_ptr<std::function<DiscoveryResult()>> m_native_cancellation;
  std::shared_ptr<std::function<DiscoveryResult()>> m_native_reset;
};

struct DiscoveryRegistrationSessionState {
  std::mutex m_mutex;
  std::mutex m_barrier;
  std::uint64_t m_id;
  std::shared_ptr<DiscoveryCommandWorker> m_commands;
  DiscoveryDeliveryHandle m_delivery;
  std::unique_ptr<DiscoveryRegistration> m_registration;
  std::atomic<bool> m_stop_requested{false};
  std::atomic<bool> m_explicit_stop{false};
  bool m_native_active{false};
  bool m_released{false};
  std::optional<DiscoveryError> m_callback_error;
  DiscoveryError m_callback_failure{DiscoveryErrorKind::kCallbackFailure,
                                    "registration callback failed"};
  DiscoveryError m_cleanup_allocation_failure{
      DiscoveryErrorKind::kResourceLimit, "callback cleanup allocation failed"};
  std::optional<DiscoveryError> m_delivery_error;
  DiscoverySessionClient::RegistrationFault m_fault;
  std::unique_ptr<std::function<void()>> m_native_cancellation;
};

namespace {

DiscoveryError invalidSession() {
  return {DiscoveryErrorKind::kInvalidArgument, "invalid discovery session"};
}

/** @brief Withdraw fake registration work without waiting for callbacks. */
DiscoveryResult stopRegistration(DiscoveryRegistrationSessionState &session) {
  session.m_stop_requested.store(true, std::memory_order_release);
  if (auto result{session.m_delivery.beginStop()}; !result) {
    return result;
  }

  session.m_native_active = false;

  return session.m_registration->stop();
}

DiscoveryResult
stopRegistrationWorker(DiscoveryRegistrationSessionState &session) {
  std::unique_ptr<std::function<void()>> cancellation;
  DiscoveryResult result;
  {
    std::lock_guard lock{session.m_mutex};
    result = stopRegistration(session);
    if (result) {
      cancellation = std::move(session.m_native_cancellation);
    }
  }

  if (cancellation) {
    (*cancellation)();
  }

  return result;
}

DiscoveryResult
finishRegistrationMutation(DiscoveryRegistrationSessionState &session,
                           DiscoveryResult result) {
  if (session.m_registration->overflowed()) {
    session.m_native_active = false;
    session.m_delivery.latchOverflow();

    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (session.m_delivery_error) {
    return std::unexpected{*session.m_delivery_error};
  }

  return result;
}

DiscoveryResult validateRegistrationAdmission(
    const DiscoveryRegistrationSessionState &session) {
  if (session.m_stop_requested.load(std::memory_order_acquire)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "registration stop pending"}};
  }

  return {};
}

DiscoveryResult
installNativeCancellation(DiscoveryRegistrationSessionState &session,
                          std::function<void()> cancellation) {
  std::lock_guard lock{session.m_mutex};
  if (auto result{validateRegistrationAdmission(session)}; !result) {
    return result;
  }

  if (session.m_native_cancellation) {
    return std::unexpected{DiscoveryError{
        DiscoveryErrorKind::kBackend, "native cancellation already installed"}};
  }

  session.m_native_cancellation =
      std::make_unique<std::function<void()>>(std::move(cancellation));

  return {};
}

/** @brief Free fake native objects and their quota leases synchronously. */
void cancelNative(DiscoverySessionState &session) {
  session.m_native_operations.clear();
  session.m_browse_slot.reset();
}

/** @brief Invalidate the whole fake browser after a resource overflow. */
void latchOverflow(DiscoverySessionState &session) {
  session.m_overflow = true;
  cancelNative(session);
  session.m_cached.m_candidates.clear();
  session.m_delivery.latchOverflow();
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

template <typename Mutation>
DiscoveryResult mutateSession(DiscoverySessionState &session,
                              Mutation &&mutation) {
  try {
    return finishMutation(session, std::forward<Mutation>(mutation)());
  } catch (const std::bad_alloc &) {
    latchOverflow(session);
    session.m_browser.reset();

    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }
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

DiscoveryResult stopSessionWorker(DiscoverySessionState &session) {
  std::unique_ptr<std::function<DiscoveryResult()>> cancellation;
  std::shared_ptr<std::function<DiscoveryResult()>> reset;
  DiscoveryResult result;
  {
    std::lock_guard lock{session.m_mutex};
    result = stopSession(session);
    if (result) {
      cancellation = std::move(session.m_native_cancellation);
      reset = std::move(session.m_native_reset);
    }
  }

  reset.reset();
  if (cancellation) {
    auto cancelled{(*cancellation)()};
    if (!cancelled) {
      std::lock_guard lock{session.m_mutex};
      session.m_cleanup_error = cancelled.error();
      session.m_stop_error = cancelled.error();

      return cancelled;
    }
  }

  return result;
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

#ifdef FIU_ENABLE
        if (fiu_fail("dmn/discovery/browser/event-allocation")) {
          throw std::bad_alloc{};
        }
#endif
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
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
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

DiscoveryRegistrationHandle::DiscoveryRegistrationHandle(
    std::shared_ptr<DiscoveryRegistrationSessionState> state)
    : m_state{std::move(state)} {}

DiscoveryResult
DiscoveryRegistrationHandle::established(std::uint64_t generation) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, generation]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateRegistrationAdmission(*state)}; !result) {
          return result;
        }

        return finishRegistrationMutation(
            *state, state->m_registration->established(generation));
      });
}

DiscoveryResult
DiscoveryRegistrationHandle::collision(std::uint64_t generation,
                                       std::string message) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, generation,
       message = std::move(message)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateRegistrationAdmission(*state)}; !result) {
          return result;
        }

        auto result{
            state->m_registration->collision(generation, std::move(message))};
        if (result) {
          state->m_native_active = false;
        }

        return finishRegistrationMutation(*state, std::move(result));
      });
}

DiscoveryResult DiscoveryRegistrationHandle::failed(std::uint64_t generation,
                                                    std::string message) const {
  return failed(generation, DiscoveryError{DiscoveryErrorKind::kBackend,
                                           std::move(message)});
}

DiscoveryResult
DiscoveryRegistrationHandle::failed(std::uint64_t generation,
                                    DiscoveryError error) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, generation,
       error = std::move(error)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateRegistrationAdmission(*state)}; !result) {
          return result;
        }

        auto result{
            state->m_registration->failed(generation, std::move(error))};
        if (result) {
          state->m_native_active = false;
        }

        return finishRegistrationMutation(*state, std::move(result));
      });
}

DiscoveryResult DiscoveryRegistrationHandle::backendLost() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute([state = m_state]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateRegistrationAdmission(*state)}; !result) {
      return result;
    }

    auto result{state->m_registration->backendLost()};
    if (result) {
      state->m_native_active = false;
    }

    return finishRegistrationMutation(*state, std::move(result));
  });
}

DiscoveryResult DiscoveryRegistrationHandle::backendAvailable() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute([state = m_state]() -> DiscoveryResult {
    std::lock_guard lock{state->m_mutex};
    if (auto result{validateRegistrationAdmission(*state)}; !result) {
      return result;
    }

    auto result{state->m_registration->backendAvailable()};
    if (result && state->m_registration->status().m_state ==
                      DiscoveryRegistrationState::kRegistering) {
      state->m_native_active = true;
    }

    return finishRegistrationMutation(*state, std::move(result));
  });
}

DiscoveryResult
DiscoveryRegistrationHandle::backendFailed(DiscoveryError error) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, error = std::move(error)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateRegistrationAdmission(*state)}; !result) {
          return result;
        }

        state->m_native_active = false;

        return finishRegistrationMutation(
            *state, state->m_registration->backendFailed(std::move(error)));
      });
}

void DiscoveryRegistrationHandle::failResourceLimit() const {
  std::lock_guard lock{m_state->m_mutex};
  if (m_state->m_stop_requested.load(std::memory_order_acquire)) {
    return;
  }

  m_state->m_native_active = false;
  m_state->m_registration->markOverflow();
  m_state->m_delivery.latchOverflow();
}

DiscoveryResult DiscoveryRegistrationHandle::stop() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  if (m_state->m_delivery.inCallbackContext() ||
      m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant stop barrier"}};
  }

  m_state->m_explicit_stop.store(true, std::memory_order_release);
  std::lock_guard barrier{m_state->m_barrier};
  m_state->m_stop_requested.store(true, std::memory_order_release);
  bool stopped{false};
  {
    std::lock_guard lock{m_state->m_mutex};
    stopped = m_state->m_registration->status().m_state ==
                  DiscoveryRegistrationState::kStopped &&
              !m_state->m_native_cancellation;
  }

  if (!stopped) {
    auto submitted{m_state->m_commands->submitControl(
        m_state->m_id, DiscoveryCommandControl::kStop,
        [state = m_state]() -> DiscoveryResult {
          return stopRegistrationWorker(*state);
        })};
    DiscoveryResult result{
        submitted ? submitted->get()
                  : DiscoveryResult{std::unexpected{submitted.error()}}};
    if (!result) {
      auto joined{m_state->m_commands->joinAfterPollFailure()};
      if (!joined) {
        return std::unexpected{joined.error()};
      }

      if (!*joined) {
        return result;
      }

      std::lock_guard lock{m_state->m_mutex};
      if (m_state->m_native_cancellation) {
        return result;
      }

      if (auto finalized{stopRegistration(*m_state)}; !finalized) {
        return finalized;
      }
    }
  }

  if (auto result{m_state->m_delivery.awaitStop()}; !result) {
    return result;
  }

  {
    std::lock_guard lock{m_state->m_mutex};
    m_state->m_released = true;
  }

  return {};
}

DiscoveryRegistrationStatus DiscoveryRegistrationHandle::status() const {
  if (!m_state) {
    return {DiscoveryRegistrationState::kStopped, invalidSession()};
  }

  std::lock_guard lock{m_state->m_mutex};
  auto result{m_state->m_registration->status()};
  result.m_callback_failed = m_state->m_callback_error.has_value();
  result.m_explicit_stop =
      m_state->m_explicit_stop.load(std::memory_order_acquire);
  if (m_state->m_callback_error &&
      (!result.m_error ||
       result.m_error->m_kind != DiscoveryErrorKind::kResourceLimit)) {
    result.m_error = m_state->m_callback_error;
  }

  return result;
}

std::uint64_t DiscoveryRegistrationHandle::generation() const {
  if (!m_state) {
    return 0;
  }

  std::lock_guard lock{m_state->m_mutex};

  return m_state->m_registration->generation();
}

bool DiscoveryRegistrationHandle::nativeActive() const {
  if (!m_state) {
    return false;
  }

  std::lock_guard lock{m_state->m_mutex};

  return m_state->m_native_active;
}

DiscoveryResult DiscoveryRegistrationHandle::setNativeCancellation(
    std::function<void()> cancellation) const {
  if (!m_state || !cancellation) {
    return std::unexpected{invalidSession()};
  }

  if (m_state->m_commands->inWorkerContext()) {
    return installNativeCancellation(*m_state, std::move(cancellation));
  }

  return m_state->m_commands->execute(
      [state = m_state,
       cancellation = std::move(cancellation)]() mutable -> DiscoveryResult {
        return installNativeCancellation(*state, std::move(cancellation));
      });
}

DiscoveryResult DiscoveryRegistrationHandle::nativeCleanupComplete() const {
  if (!m_state || !m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "native cleanup requires worker"}};
  }

  std::unique_ptr<std::function<void()>> cancellation;
  {
    std::lock_guard lock{m_state->m_mutex};
    cancellation = std::move(m_state->m_native_cancellation);
    m_state->m_native_active = false;
  }

  return {};
}

DiscoverySessionHandle::DiscoverySessionHandle(
    std::shared_ptr<DiscoverySessionState> state)
    : m_state{std::move(state)} {}

DiscoveryResult DiscoverySessionHandle::nativeCleanupComplete() const {
  if (!m_state || !m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "native cleanup requires worker"}};
  }

  std::unique_ptr<std::function<DiscoveryResult()>> cancellation;
  std::shared_ptr<std::function<DiscoveryResult()>> reset;
  {
    std::lock_guard lock{m_state->m_mutex};
    cancellation = std::move(m_state->m_native_cancellation);
    reset = std::move(m_state->m_native_reset);
    cancelNative(*m_state);
  }

  return {};
}

std::expected<std::uint64_t, DiscoveryError>
DiscoverySessionHandle::observe(std::uint64_t epoch, DiscoveryServiceKey key,
                                DiscoveryContribution contribution,
                                DiscoveryProvenance provenance) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  using Output = std::expected<std::uint64_t, DiscoveryError>;
  auto output{std::make_shared<std::optional<Output>>()};
  auto result{m_state->m_commands->execute(
      [state = m_state, epoch, key = std::move(key), contribution, provenance,
       output]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }

        return mutateSession(*state, [&]() -> DiscoveryResult {
          *output =
              state->m_browser->addObservation(key, contribution, provenance);

          return **output
                     ? DiscoveryResult{}

                     : DiscoveryResult{std::unexpected{(*output)->error()}};
        });
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

    return mutateSession(*state, [&]() -> DiscoveryResult {
      if (!state->m_browser->removeObservation(key, contribution, generation)) {
        return std::unexpected{DiscoveryError{
            DiscoveryErrorKind::kInvalidArgument, "stale browse contribution"}};
      }

      return {};
    });
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

        return mutateSession(*state, [&] {
          return state->m_browser->updateRecords(key, contribution, generation,
                                                 update);
        });
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

        return mutateSession(*state, [&]() -> DiscoveryResult {
          *output = state->m_browser->refresh(key);

          return **output
                     ? DiscoveryResult{}

                     : DiscoveryResult{std::unexpected{(*output)->error()}};
        });
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

  return m_state->m_commands->execute(
      [state = m_state, epoch]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }

        return mutateSession(*state, [&] {
          return state->m_browser->processRefreshDeadlines();
        });
      });
}

DiscoveryResult DiscoverySessionHandle::failAttempt(
    std::uint64_t epoch, DiscoveryServiceKey key,
    DiscoveryContribution contribution, std::uint64_t generation,
    DiscoveryAttempt attempt, std::uint64_t operationGeneration,
    std::uint64_t resolutionGeneration, bool recoverable, std::string message,
    std::optional<DiscoveryError> native) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, epoch, key = std::move(key), contribution, generation,
       attempt, operationGeneration, resolutionGeneration, recoverable,
       message = std::move(message),
       native = std::move(native)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }

        return mutateSession(*state, [&] {
          return state->m_browser->failAttempt(
              key, contribution, generation, attempt, operationGeneration,
              resolutionGeneration, recoverable, std::move(message),
              std::move(native));
        });
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

        return mutateSession(*state, [&] {
          return state->m_browser->succeedAttempt(key, contribution, generation,
                                                  attempt, operationGeneration,
                                                  resolutionGeneration);
        });
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

std::expected<std::optional<DiscoveryBrowser::TimePoint>, DiscoveryError>
DiscoverySessionHandle::nextDeadline(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  auto output{std::make_shared<std::optional<DiscoveryBrowser::TimePoint>>()};
  auto result{m_state->m_commands->execute(
      [state = m_state, epoch, output]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto valid{validateSession(*state, epoch)}; !valid) {
          return valid;
        }
        *output = state->m_browser->nextDeadline();

        return {};
      })};
  if (!result) {
    return std::unexpected{result.error()};
  }

  return *output;
}

DiscoveryResult
DiscoverySessionHandle::backendFailed(DiscoveryError error) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  return m_state->m_commands->execute(
      [state = m_state, error = std::move(error)]() mutable -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto valid{validateSession(*state, {}, false)}; !valid) {
          return valid;
        }

        return mutateSession(*state, [&]() -> DiscoveryResult {
          state->m_cleanup_error = error;
          if (error.m_kind == DiscoveryErrorKind::kResourceLimit) {
            return std::unexpected{std::move(error)};
          }

          if (!state->m_backend_available) {
            return {};
          }

          auto replacement{makeBrowser(state)};
          auto result{state->m_delivery.backendLost(
              state->m_delivery.snapshot().m_epoch)};
          cancelNative(*state);
          state->m_browser = std::move(replacement);
          state->m_cached = state->m_browser->snapshot();
          state->m_backend_available = false;

          return result;
        });
      });
}

void DiscoverySessionHandle::failResourceLimit(bool backendUnavailable) const {
  std::lock_guard lock{m_state->m_mutex};
  if (m_state->m_stop_requested.load(std::memory_order_acquire)) {
    return;
  }

  m_state->m_cleanup_error =
      DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}};
  if (backendUnavailable) {
    m_state->m_backend_available = false;
  }

  latchOverflow(*m_state);
  m_state->m_browser.reset();
}

std::expected<std::uint64_t, DiscoveryError>
DiscoverySessionHandle::beginNative(std::uint64_t epoch) const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  auto output{std::make_shared<std::optional<std::uint64_t>>()};
  auto result{m_state->m_commands->execute(
      [state = m_state, epoch, output]() -> DiscoveryResult {
        std::lock_guard lock{state->m_mutex};
        if (auto result{validateSession(*state, epoch)}; !result) {
          return result;
        }

        return mutateSession(*state, [&]() -> DiscoveryResult {
          if (state->m_next_operation == 0) {
            return std::unexpected{
                DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                               "native operation ID exhausted"}};
          }

          auto lease{state->m_slots.reserve()};
          if (!lease) {
            return std::unexpected{lease.error()};
          }

          const auto id{state->m_next_operation++};
          state->m_native_operations.emplace(id, std::move(*lease));
          *output = id;

          return {};
        });
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

        return mutateSession(*state, [&]() -> DiscoveryResult {
          if (!state->m_backend_available) {
            return {};
          }

          auto replacement{makeBrowser(state)};
          auto result{state->m_delivery.backendLost(epoch)};
          cancelNative(*state);
          state->m_browser = std::move(replacement);
          state->m_cached = state->m_browser->snapshot();
          state->m_backend_available = false;

          return result;
        });
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

        return mutateSession(*state, [&]() -> DiscoveryResult {
          if (state->m_backend_available) {
            return {};
          }

          if (auto result{startBrowse(*state)}; !result) {
            return result;
          }

          auto result{state->m_delivery.backendAvailable(epoch)};
          if (!result) {
            cancelNative(*state);

            return result;
          }

          state->m_backend_available = true;

          return {};
        });
      });
}

DiscoveryResult DiscoverySessionHandle::clearCache() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  if (m_state->m_delivery.inCallbackContext() ||
      m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant reset barrier"}};
  }

  std::shared_ptr<DiscoveryBarrierOperation> operation{};
  bool leader{false};
  {
    std::lock_guard lock{m_state->m_barrier};
    if (m_state->m_stop_operation) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kStopped, "session stop pending"}};
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
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/reset-barrier/leader-allocation")) {
      throw std::bad_alloc{};
    }

    if (fiu_fail("dmn/discovery/reset-barrier/leader-failure")) {
      throw 1;
    }
#endif
    auto epoch{std::make_shared<std::optional<std::uint64_t>>()};
    auto submitted{m_state->m_commands->submitControl(
        m_state->m_id, DiscoveryCommandControl::kReset,
        [state = m_state, epoch]() -> DiscoveryResult {
          std::shared_ptr<std::function<DiscoveryResult()>> reset;
          {
            std::lock_guard lock{state->m_mutex};
            if (state->m_stopped) {
              return std::unexpected{DiscoveryError{
                  DiscoveryErrorKind::kStopped, "session stopped"}};
            }

            auto mutation{mutateSession(*state, [&]() -> DiscoveryResult {
              auto replacement{makeBrowser(state)};
              cancelNative(*state);
              const auto newEpoch{state->m_delivery.beginReset()};
              if (!newEpoch) {
                return std::unexpected{newEpoch.error()};
              }
              *epoch = *newEpoch;
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

              reset = state->m_native_reset;

              return {};
            })};
            if (!mutation) {
              *epoch = state->m_delivery.snapshot().m_epoch;

              return mutation;
            }
          }

          if (reset) {
            auto result{(*reset)()};
            *epoch = state->m_delivery.snapshot().m_epoch;
            if (!result) {
              return result;
            }
          }

          std::lock_guard lock{state->m_mutex};
          state->m_cleanup_error.reset();

          return {};
        })};
    if (!submitted) {
      result = std::unexpected{submitted.error()};
    } else {
      result = submitted->get();
      if (*epoch) {
        auto target{**epoch};
        while (true) {
          auto drained{m_state->m_delivery.awaitReset(target)};
          const auto current{m_state->m_delivery.snapshot().m_epoch};
          if (!drained && current > target) {
            target = current;
            continue;
          }

          if (result && !drained) {
            result = std::move(drained);
          }

          break;
        }
      }
    }
#if defined(__GLIBCXX__)
  } catch (const __cxxabiv1::__forced_unwind &) {
    std::lock_guard lock{m_state->m_barrier};
    operation->m_promise.set_value(std::move(operation->m_cancelled));
    m_state->m_reset_operation.reset();

    throw;
#endif
  } catch (const std::bad_alloc &) {
    result = std::move(operation->m_allocation_failure);
  } catch (...) {
    result = std::move(operation->m_failure);
  }

  {
    std::lock_guard lock{m_state->m_barrier};
    // Publish without copying an error string while callers share the barrier.
    operation->m_promise.set_value(std::move(result));
    m_state->m_reset_operation.reset();
  }

  return operation->m_completion.get();
}

DiscoveryResult DiscoverySessionHandle::stop() const {
  if (!m_state) {
    return std::unexpected{invalidSession()};
  }

  if (m_state->m_delivery.inCallbackContext() ||
      m_state->m_commands->inWorkerContext()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant stop barrier"}};
  }

  m_state->m_explicit_stop.store(true, std::memory_order_release);
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
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/stop-barrier/leader-allocation")) {
      throw std::bad_alloc{};
    }

    if (fiu_fail("dmn/discovery/stop-barrier/leader-failure")) {
      throw 1;
    }
#endif
    if (reset) {
      reset->m_completion.wait();
    }

    bool stopped{false};
    {
      std::lock_guard lock{m_state->m_mutex};
      stopped = m_state->m_stopped && !m_state->m_native_cancellation;
      if (stopped && m_state->m_stop_error) {
        result = std::unexpected{*m_state->m_stop_error};
      }
    }

    if (!stopped) {
      auto submitted{m_state->m_commands->submitControl(
          m_state->m_id, DiscoveryCommandControl::kStop,
          [state = m_state]() -> DiscoveryResult {
            return stopSessionWorker(*state);
          })};
      result = submitted ? submitted->get()
                         : DiscoveryResult{std::unexpected{submitted.error()}};
      if (!result) {
        auto joined{m_state->m_commands->joinAfterPollFailure()};
        if (!joined) {
          result = std::unexpected{joined.error()};
        } else if (*joined) {
          std::lock_guard lock{m_state->m_mutex};
          if (!m_state->m_native_cancellation && !m_state->m_native_reset) {
            result = stopSession(*m_state);
          }
        }
      }
    }

    bool finalized{false};
    {
      std::lock_guard lock{m_state->m_mutex};
      finalized = m_state->m_stopped && !m_state->m_native_cancellation;
    }

    if (finalized) {
      auto drained{m_state->m_delivery.awaitStop()};
      if (drained) {
        std::lock_guard lock{m_state->m_mutex};
        m_state->m_released = true;
      }

      if (result && !drained) {
        result = std::move(drained);
      }
    }
#if defined(__GLIBCXX__)
  } catch (const __cxxabiv1::__forced_unwind &) {
    std::lock_guard lock{m_state->m_barrier};
    operation->m_promise.set_value(std::move(operation->m_cancelled));
    m_state->m_stop_operation.reset();

    throw;
#endif
  } catch (const std::bad_alloc &) {
    result = std::move(operation->m_allocation_failure);
  } catch (...) {
    result = std::move(operation->m_failure);
  }

  {
    std::lock_guard lock{m_state->m_barrier};
    operation->m_promise.set_value(std::move(result));
    m_state->m_stop_operation.reset();
  }

  return operation->m_completion.get();
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
          m_state->m_stop_requested.load(std::memory_order_acquire),
          m_state->m_explicit_stop.load(std::memory_order_acquire)};
}

DiscoverySessionClient::DiscoverySessionClient(
    const DiscoveryOptions &options, bool backendAvailable,
    DiscoveryBrowser::Clock clock, RegistrationFault registrationFault)
    : m_options{options}, m_initial_backend_available{backendAvailable},
      m_clock{std::move(clock)},
      m_registration_fault{std::move(registrationFault)} {}

DiscoverySessionClient::~DiscoverySessionClient() {
  static_cast<void>(shutdown());
}

DiscoveryResult DiscoverySessionClient::ensureWorkers() {
  if (m_commands) {
    return {};
  }

  if (m_options.m_max_browsers >
      std::numeric_limits<std::size_t>::max() - m_options.m_max_registrations) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session limit overflow"}};
  }

  const auto maxSessions{m_options.m_max_browsers +
                         m_options.m_max_registrations};
  try {
    auto commands{std::make_shared<DiscoveryCommandWorker>(
        m_options.m_max_commands, maxSessions)};
    auto dispatcher{std::make_unique<DiscoveryDispatcher>(
        maxSessions, [this] { handleDispatcherFailure(); })};
    m_commands = std::move(commands);
    m_dispatcher = std::move(dispatcher);
  } catch (const std::system_error &error) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, error.what()}};
  } catch (const std::bad_alloc &) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                       "discovery worker allocation failed"}};
  }

  return {};
}

void DiscoverySessionClient::handleDispatcherFailure() {
  // Native teardown remains on the state worker even if callbacks cannot run.
  // Select by monotonic ID so concurrent removal cannot skip another session.
  std::uint64_t lastId{0};
  while (true) {
    std::shared_ptr<DiscoverySessionState> session;
    {
      std::lock_guard lock{m_mutex};
      const auto it{std::find_if(m_sessions.begin(), m_sessions.end(),
                                 [lastId](const auto &candidate) {
                                   return candidate->m_id > lastId;
                                 })};
      if (it == m_sessions.end()) {
        break;
      }

      session = *it;
      lastId = session->m_id;
    }

    const auto fallback = [&](DiscoveryError error) {
      {
        std::lock_guard lock{session->m_mutex};
        session->m_cleanup_error = std::move(error);
        session->m_stop_requested.store(true, std::memory_order_release);
        session->m_stopped = true;
        cancelNative(*session);
        session->m_browser.reset();
        session->m_cached.m_candidates.clear();
      }

      session->m_commands->wakePoller();
    };
    try {
      {
        std::lock_guard lock{session->m_mutex};
        if (!session->m_cleanup_error) {
          session->m_cleanup_error = DiscoveryError{
              DiscoveryErrorKind::kBackend, "callback worker failed"};
        }
      }
#ifdef FIU_ENABLE
      if (fiu_fail("dmn/discovery/callback-worker/cleanup-allocation")) {
        throw std::bad_alloc{};
      }
#endif
      auto submitted{session->m_commands->submitControl(
          session->m_id, DiscoveryCommandControl::kStop,
          [session] { return stopSessionWorker(*session); })};
      auto result{submitted
                      ? submitted->get()
                      : DiscoveryResult{std::unexpected{submitted.error()}}};
      if (!result) {
        fallback(std::move(result.error()));
      }
    } catch (const std::bad_alloc &) {
      fallback(std::move(session->m_cleanup_allocation_failure));
    }
  }

  lastId = 0;
  while (true) {
    std::shared_ptr<DiscoveryRegistrationSessionState> session;
    {
      std::lock_guard lock{m_mutex};
      const auto it{std::find_if(m_registrations.begin(), m_registrations.end(),
                                 [lastId](const auto &candidate) {
                                   return candidate->m_id > lastId;
                                 })};
      if (it == m_registrations.end()) {
        break;
      }

      session = *it;
      lastId = session->m_id;
    }

    const auto fallback = [&](DiscoveryError error) {
      {
        std::lock_guard lock{session->m_mutex};
        session->m_callback_error = std::move(error);
        session->m_stop_requested.store(true, std::memory_order_release);
        session->m_native_active = false;
        static_cast<void>(session->m_registration->stop());
      }

      session->m_commands->wakePoller();
    };
    try {
      {
        std::lock_guard lock{session->m_mutex};
        if (!session->m_callback_error) {
          session->m_callback_error = DiscoveryError{
              DiscoveryErrorKind::kBackend, "callback worker failed"};
        }
      }
#ifdef FIU_ENABLE
      if (fiu_fail("dmn/discovery/callback-worker/cleanup-allocation")) {
        throw std::bad_alloc{};
      }
#endif
      auto submitted{session->m_commands->submitControl(
          session->m_id, DiscoveryCommandControl::kStop,
          [session] { return stopRegistrationWorker(*session); })};
      auto result{submitted
                      ? submitted->get()
                      : DiscoveryResult{std::unexpected{submitted.error()}}};
      if (!result) {
        fallback(std::move(result.error()));
      }
    } catch (const std::bad_alloc &) {
      fallback(std::move(session->m_cleanup_allocation_failure));
    }
  }
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

DiscoveryResult DiscoverySessionClient::attachPoller(
    std::shared_ptr<DiscoveryCommandPoller> poller) {
  if (!poller) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery poller"}};
  }

  if (auto result{validateOptions(m_options)}; !result) {
    return result;
  }

  if (!m_clock) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery clock"}};
  }

  std::shared_ptr<DiscoveryCommandWorker> commands;
  {
    std::lock_guard lock{m_mutex};
    if (m_closing) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                            "discovery client stopped"}};
    }

    if (auto result{ensureWorkers()}; !result) {
      return result;
    }

    commands = m_commands;
  }

  return commands->attachPoller(std::move(poller));
}

std::expected<DiscoverySessionHandle, DiscoveryError>
DiscoverySessionClient::browse(Handler handler, BrowserLifecycle lifecycle) {
  if (auto result{validateOptions(m_options)}; !result) {
    return std::unexpected{result.error()};
  }

  if (!m_clock) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery clock"}};
  }

  std::unique_lock lock{m_mutex};
  if (m_closing) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery client stopped"}};
  }

  if (m_next_session == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session ID exhausted"}};
  }

  if (auto result{ensureWorkers()}; !result) {
    return std::unexpected{result.error()};
  }

  std::erase_if(m_sessions, [](const auto &session) {
    std::lock_guard sessionLock{session->m_mutex};

    return session->m_released;
  });
  if (m_sessions.size() >= m_options.m_max_browsers) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "browser limit reached"}};
  }

  auto session{std::make_shared<DiscoverySessionState>(m_options)};
  session->m_id = m_next_session++;
  session->m_commands = m_commands;
  session->m_clock = m_clock;
  session->m_backend_available = m_initial_backend_available;
  if (lifecycle.m_cancel) {
    session->m_native_cancellation =
        std::make_unique<std::function<DiscoveryResult()>>(
            std::move(lifecycle.m_cancel));
  }
  if (lifecycle.m_reset) {
    session->m_native_reset =
        std::make_shared<std::function<DiscoveryResult()>>(
            std::move(lifecycle.m_reset));
  }
#ifdef FIU_ENABLE
  if (fiu_fail("dmn/discovery/browser/model-allocation")) {
    throw std::bad_alloc{};
  }
#endif
  session->m_browser = makeBrowser(session);
  m_sessions.reserve(m_sessions.size() + 1);
  std::weak_ptr<DiscoverySessionState> weak{session};
  auto delivery{m_dispatcher->create(
      m_options.m_max_data_events, m_options.m_max_control_events,
      [weak, handler = std::move(handler)](DiscoveryDelivery event) {
        try {
          if (handler) {
            try {
              handler(std::move(event));
            } catch (DiscoveryDeliveryFailure &failure) {
              if (auto state{weak.lock()}) {
                std::lock_guard lock{state->m_mutex};
                state->m_cleanup_error = failure.takeError();
              }

              throw;
            }
          }
        } catch (...) {
          if (auto state{weak.lock()}) {
            const auto fallback = [&](DiscoveryError error) {
              {
                std::lock_guard lock{state->m_mutex};
                state->m_cleanup_error = std::move(error);
                state->m_stop_requested.store(true, std::memory_order_release);
                static_cast<void>(state->m_delivery.cancelFromCallback());
                state->m_stopped = true;
                cancelNative(*state);
                state->m_browser.reset();
                state->m_cached.m_candidates.clear();
              }

              state->m_commands->wakePoller();
            };
            try {
              auto submitted{state->m_commands->submitControl(
                  state->m_id, DiscoveryCommandControl::kStop,
                  [state]() -> DiscoveryResult {
                    return stopSessionWorker(*state);
                  })};
              DiscoveryResult result{submitted
                                         ? submitted->get()
                                         : DiscoveryResult{std::unexpected{
                                               std::move(submitted.error())}}};
              if (!result) {
                fallback(std::move(result.error()));
              }
            } catch (const std::bad_alloc &) {
              fallback(std::move(state->m_cleanup_allocation_failure));
            }
          }

          throw;
        }
      })};
  if (!delivery) {
    return std::unexpected{delivery.error()};
  }

  session->m_delivery = std::move(*delivery);
  m_sessions.push_back(session);
  if (session->m_backend_available) {
    if (auto result{startBrowse(*session)}; !result) {
      return std::unexpected{result.error()};
    }
  }

  if (!session->m_backend_available) {
    DiscoveryResult result;
    try {
#ifdef FIU_ENABLE
      if (fiu_fail("dmn/discovery/browser/initial-delivery")) {
        throw std::bad_alloc{};
      }
#endif
      result = session->m_delivery.postControl(
          session->m_delivery.snapshot().m_epoch,
          DiscoveryControl::kBackendUnavailable);
    } catch (const std::bad_alloc &) {
      m_sessions.pop_back();
      lock.unlock();
      auto abandoned{session->m_delivery.abandonUnpublished()};
      if (!abandoned) {
        return std::unexpected{abandoned.error()};
      }

      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                         "initial delivery allocation failed"}};
    }

    if (!result) {
      m_sessions.pop_back();
      lock.unlock();
      auto abandoned{session->m_delivery.abandonUnpublished()};
      if (!abandoned) {
        return std::unexpected{abandoned.error()};
      }

      return std::unexpected{result.error()};
    }
  }

  return DiscoverySessionHandle{std::move(session)};
}

std::expected<DiscoveryRegistrationHandle, DiscoveryError>
DiscoverySessionClient::advertise(
    DiscoveryAdvertisement advertisement, RegistrationHandler handler,
    std::function<void()> nativeCancellation) try {
  auto registration{DiscoveryRegistration::create(
      m_options, std::move(advertisement), m_initial_backend_available, {})};
  if (!registration) {
    return std::unexpected{registration.error()};
  }

  if (!m_clock) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery clock"}};
  }

  std::lock_guard lock{m_mutex};
  if (m_closing) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery client stopped"}};
  }

  if (m_next_session == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery session ID exhausted"}};
  }

  std::erase_if(m_registrations, [](const auto &session) {
    std::lock_guard sessionLock{session->m_mutex};

    return session->m_released;
  });
  if (m_registrations.size() >= m_options.m_max_registrations) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "registration limit reached"}};
  }

  if (auto result{ensureWorkers()}; !result) {
    return std::unexpected{result.error()};
  }

  auto session{std::make_shared<DiscoveryRegistrationSessionState>()};
  session->m_id = m_next_session++;
  session->m_commands = m_commands;
  session->m_fault = m_registration_fault;
  if (nativeCancellation) {
    session->m_native_cancellation =
        std::make_unique<std::function<void()>>(std::move(nativeCancellation));
  }
  std::weak_ptr<DiscoveryRegistrationSessionState> weak{session};
  session->m_registration = std::move(*registration);
  session->m_registration->setHandler(
      [weak](const DiscoveryRegistrationStatus &status) {
        if (const auto state{weak.lock()}) {
          if (state->m_fault) {
            state->m_fault(
                DiscoveryRegistrationFaultPoint::kTransitionDelivery);
          }

          const bool control{
              status.m_state == DiscoveryRegistrationState::kUnavailable ||
              status.m_state == DiscoveryRegistrationState::kRegistering};
          const auto epoch{state->m_delivery.snapshot().m_epoch};
          auto result{control
                          ? state->m_delivery.registrationBackendChange(status)
                          : state->m_delivery.postRegistration(epoch, status)};
          if (!result) {
            state->m_native_active = false;
            if (state->m_delivery.snapshot().m_state ==
                DiscoveryDeliveryState::kOverflow) {
              state->m_registration->markOverflow();
            } else {
              state->m_delivery_error = std::move(result.error());
              state->m_registration->markDeliveryFailure(
                  *state->m_delivery_error);
            }
          }
        }
      });
  m_registrations.reserve(m_registrations.size() + 1);
  if (m_registration_fault) {
    m_registration_fault(DiscoveryRegistrationFaultPoint::kAdmission);
  }

  auto delivery{m_dispatcher->create(
      m_options.m_max_registration_events, m_options.m_max_control_events,
      [weak, handler = std::move(handler)](DiscoveryDelivery event) {
        try {
          try {
            if (handler && event.m_registration) {
              handler(std::move(*event.m_registration));
            } else if (handler &&
                       event.m_control == DiscoveryControl::kOverflow) {
              if (const auto state{weak.lock()}) {
                DiscoveryRegistrationStatus status{};
                {
                  std::lock_guard stateLock{state->m_mutex};
                  status = state->m_registration->status();
                }

                handler(std::move(status));
              }
            }
          } catch (DiscoveryDeliveryFailure &failure) {
            if (auto state{weak.lock()}) {
              std::lock_guard stateLock{state->m_mutex};
              state->m_callback_error = failure.takeError();
            }

            throw;
          }
        } catch (...) {
          if (auto state{weak.lock()}) {
            {
              std::lock_guard stateLock{state->m_mutex};
              if (!state->m_callback_error) {
                state->m_callback_error = std::move(state->m_callback_failure);
              }
            }

            const auto markCallbackCleanupFailure = [&state](
                                                        DiscoveryError error) {
              std::lock_guard stateLock{state->m_mutex};
              state->m_callback_error = std::move(error);
              state->m_stop_requested.store(true, std::memory_order_release);
              state->m_native_active = false;
              static_cast<void>(state->m_delivery.cancelFromCallback());
              static_cast<void>(state->m_registration->stop());
              // An adapter with native work reaps stopped sessions after
              // the poll returns; never destroy native work on this thread.
              state->m_commands->wakePoller();
            };
            try {
              if (state->m_fault) {
                state->m_fault(
                    DiscoveryRegistrationFaultPoint::kCallbackCleanup);
              }

              auto submitted{state->m_commands->submitControl(
                  state->m_id, DiscoveryCommandControl::kStop,
                  [state]() -> DiscoveryResult {
                    return stopRegistrationWorker(*state);
                  })};
              DiscoveryResult result{submitted
                                         ? submitted->get()
                                         : DiscoveryResult{std::unexpected{
                                               std::move(submitted.error())}}};
              if (!result) {
                markCallbackCleanupFailure(std::move(result.error()));
              }
            } catch (const std::bad_alloc &) {
              markCallbackCleanupFailure(
                  std::move(state->m_cleanup_allocation_failure));
            }
          }

          throw;
        }
      })};
  if (!delivery) {
    return std::unexpected{delivery.error()};
  }

  session->m_delivery = std::move(*delivery);
  session->m_native_active = m_initial_backend_available;
  m_registrations.push_back(session);
  {
    std::lock_guard sessionLock{session->m_mutex};
    const auto epoch{session->m_delivery.snapshot().m_epoch};
    try {
      if (session->m_fault) {
        session->m_fault(DiscoveryRegistrationFaultPoint::kInitialDelivery);
      }

      if (auto result{session->m_delivery.postRegistration(
              epoch, session->m_registration->status())};
          !result) {
        session->m_native_active = false;
        if (session->m_delivery.snapshot().m_state ==
            DiscoveryDeliveryState::kOverflow) {
          session->m_registration->markOverflow();
        } else {
          session->m_delivery_error = std::move(result.error());
          session->m_registration->markDeliveryFailure(
              *session->m_delivery_error);
        }
      }
    } catch (const std::bad_alloc &) {
      session->m_registration->markOverflow();
      session->m_native_active = false;
      session->m_delivery.latchOverflow();
    }
  }

  return DiscoveryRegistrationHandle{std::move(session)};
} catch (const std::bad_alloc &) {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "registration allocation failed"}};
}

DiscoveryResult DiscoverySessionClient::shutdown() {
  std::vector<std::shared_ptr<DiscoverySessionState>> sessions{};
  std::vector<std::shared_ptr<DiscoveryRegistrationSessionState>>
      registrations{};
  std::shared_ptr<DiscoveryCommandWorker> commands{};
  DiscoveryDispatcher *dispatcher{nullptr};
  {
    std::unique_lock lock{m_mutex};
    if ((m_dispatcher && m_dispatcher->inCallbackContext()) ||
        (m_commands && m_commands->inWorkerContext())) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                            "reentrant shutdown barrier"}};
    }

    while (m_closing && !m_closed) {
      m_changed.wait(lock, [&] { return m_closed || !m_closing; });
    }

    if (m_closed) {
      if (m_shutdown_error) {
        return std::unexpected{*m_shutdown_error};
      }

      return {};
    }

    sessions = m_sessions;
    registrations = m_registrations;
    commands = m_commands;
    dispatcher = m_dispatcher.get();
    m_closing = true;
  }

  try {
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/shutdown/allocation")) {
      throw std::bad_alloc{};
    }
#endif
    std::optional<DiscoveryError> error{};
    for (const auto &session : sessions) {
      auto result{DiscoverySessionHandle{session}.stop()};
      if (!result && !error) {
        error = result.error();
      }
    }

    for (const auto &session : registrations) {
      auto result{DiscoveryRegistrationHandle{session}.stop()};
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
  } catch (...) {
    std::lock_guard lock{m_mutex};
    m_closing = false;
    m_changed.notify_all();

    throw;
  }
}

} // namespace dmn::detail
