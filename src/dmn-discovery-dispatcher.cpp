/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-dispatcher.cpp
 * @brief Multi-session callback delivery, failure isolation and drains.
 */

#include "dmn-discovery-dispatcher.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <cerrno>
#include <condition_variable>
#include <limits>
#include <map>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {

struct DiscoveryDeliverySession {
  DiscoveryDeliverySession(std::uint64_t id, std::size_t maxData,
                           std::size_t maxControl,
                           DiscoveryDispatcher::Handler handler)
      : m_id{id}, m_queue{maxData, maxControl}, m_handler{std::move(handler)} {}

  std::uint64_t m_id;
  DiscoveryDeliveryQueue m_queue;
  DiscoveryDispatcher::Handler m_handler;
  std::mutex m_barrier;
  bool m_active{false};
  bool m_callback_failed{false};
  std::uint64_t m_active_epoch{0};
};

struct DiscoveryDispatcherState {
  DiscoveryDispatcherState(std::size_t maxSessions,
                           std::function<void()> workerFailure)
      : m_max_sessions{maxSessions},
        m_worker_failure{std::move(workerFailure)} {}

  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_startup_complete{false};
  int m_startup_error{0};
  std::map<std::uint64_t, std::shared_ptr<DiscoveryDeliverySession>> m_sessions;
  std::size_t m_max_sessions;
  std::uint64_t m_next_id{1};
  std::uint64_t m_last_served{0};
  bool m_shutdown{false};
  bool m_wake{false};
  DiscoveryError m_fatal_error{DiscoveryErrorKind::kBackend,
                               "callback worker failed"};
  std::optional<DiscoveryError> m_worker_error;
  std::function<void()> m_worker_failure;
};

namespace {

thread_local const DiscoveryDispatcherState *activeDispatcher{nullptr};

/**
 * @brief Destroy captures outside the state lock while rejecting reentry.
 *
 * User-owned capture destructors may call back into discovery; treating their
 * retirement as callback context prevents a barrier from waiting on itself.
 */
void retireHandler(DiscoveryDispatcher::Handler &handler,
                   const DiscoveryDispatcherState *state) {
  const auto previous{activeDispatcher};
  activeDispatcher = state;
  handler = nullptr;
  activeDispatcher = previous;
}

/**
 * @brief Deliver one session at a time without holding shared state locked.
 */
void runDispatcher(const std::shared_ptr<DiscoveryDispatcherState> &state) {
  std::unique_lock lock{state->m_mutex};
  while (!state->m_shutdown) {
    std::shared_ptr<DiscoveryDeliverySession> session{};
    std::optional<DiscoveryDelivery> event{};
    auto it{state->m_sessions.upper_bound(state->m_last_served)};
    for (std::size_t checked{0}; checked < state->m_sessions.size();
         ++checked) {
      if (it == state->m_sessions.end()) {
        it = state->m_sessions.begin();
      }

      event = it->second->m_queue.pop();
      if (event) {
        session = it->second;
        state->m_last_served = it->first;
        break;
      }

      ++it;
    }

    if (!session) {
      state->m_changed.wait(lock,
                            [&] { return state->m_wake || state->m_shutdown; });
      state->m_wake = false;
      continue;
    }

    session->m_active = true;
    session->m_active_epoch = event->m_epoch;
    lock.unlock();
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/callback-worker/fatal")) {
      throw std::runtime_error{"injected callback worker failure"};
    }
#endif
    bool failed{false};
    activeDispatcher = state.get();
    try {
      if (session->m_handler) {
        session->m_handler(std::move(*event));
      }
#if defined(__GLIBCXX__)
    } catch (const __cxxabiv1::__forced_unwind &) {
      throw;
#endif
    } catch (...) {
      failed = true;
    }

    activeDispatcher = nullptr;
    lock.lock();
    session->m_active = false;
    if (failed) {
      session->m_callback_failed = true;
      session->m_queue.stop();
    }

    state->m_changed.notify_all();
  }
}

void runDispatcherSafely(
    const std::shared_ptr<DiscoveryDispatcherState> &state) {
  try {
#ifdef FIU_ENABLE
    const int error{
        fiu_fail("dmn/discovery/callback-worker/cancel-setup")
            ? EINVAL
            : pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr)};
#else
    const int error{pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr)};
#endif
    {
      std::lock_guard lock{state->m_mutex};
      state->m_startup_error = error;
      state->m_startup_complete = true;
      state->m_changed.notify_all();
    }

    if (error != 0) {
      throw std::system_error{error, std::generic_category()};
    }

    runDispatcher(state);

    return;
#if defined(__GLIBCXX__)
  } catch (const __cxxabiv1::__forced_unwind &) {
    throw;
#endif
  } catch (...) {
    activeDispatcher = nullptr;
  }

  {
    std::lock_guard lock{state->m_mutex};
    state->m_shutdown = true;
    state->m_worker_error = std::move(state->m_fatal_error);
    for (auto &[id, session] : state->m_sessions) {
      static_cast<void>(id);
      session->m_queue.stop();
      session->m_active = false;
    }

    state->m_changed.notify_all();
  }

  if (state->m_startup_error == 0 && state->m_worker_failure) {
    state->m_worker_failure();
  }
}

/**
 * @brief Reject calls on default-constructed or stale handle objects.
 */
DiscoveryResult invalidHandle() {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                        "invalid discovery delivery handle"}};
}

} // namespace

DiscoveryDeliveryHandle::DiscoveryDeliveryHandle(
    std::shared_ptr<DiscoveryDispatcherState> state,
    std::shared_ptr<DiscoveryDeliverySession> session)
    : m_state{std::move(state)}, m_session{std::move(session)} {}

DiscoveryResult DiscoveryDeliveryHandle::postData(std::uint64_t epoch,
                                                  DiscoveryEvent event) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale delivery epoch"}};
  }

  auto result{m_session->m_queue.pushData(std::move(event))};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult DiscoveryDeliveryHandle::postRegistration(
    std::uint64_t epoch, DiscoveryRegistrationStatus status) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale delivery epoch"}};
  }

  auto result{m_session->m_queue.pushRegistration(std::move(status))};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult DiscoveryDeliveryHandle::registrationBackendChange(
    DiscoveryRegistrationStatus status) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  auto result{m_session->m_queue.registrationBackendChange(std::move(status))};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult
DiscoveryDeliveryHandle::postControl(std::uint64_t epoch,
                                     DiscoveryControl control) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale delivery epoch"}};
  }

  auto result{m_session->m_queue.pushControl(control)};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult
DiscoveryDeliveryHandle::backendLost(std::uint64_t epoch) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale delivery epoch"}};
  }

  auto result{m_session->m_queue.backendLost()};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult
DiscoveryDeliveryHandle::backendAvailable(std::uint64_t epoch) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale delivery epoch"}};
  }

  auto result{m_session->m_queue.backendAvailable()};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

DiscoveryResult DiscoveryDeliveryHandle::failOverflow() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::lock_guard lock{m_state->m_mutex};
  auto result{m_session->m_queue.failOverflow()};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return result;
}

void DiscoveryDeliveryHandle::latchOverflow() const {
  if (!m_state || !m_session) {
    throw std::logic_error{"invalid discovery delivery handle"};
  }

  std::lock_guard lock{m_state->m_mutex};
  m_session->m_queue.latchOverflow();
  m_state->m_wake = true;
  m_state->m_changed.notify_all();
}

bool DiscoveryDeliveryHandle::inCallbackContext() const {
  return m_state && activeDispatcher == m_state.get();
}

std::expected<std::uint64_t, DiscoveryError>
DiscoveryDeliveryHandle::beginReset() const {
  if (!m_state || !m_session) {
    return std::unexpected{invalidHandle().error()};
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant reset barrier"}};
  }

  std::lock_guard lock{m_state->m_mutex};
  auto result{m_session->m_queue.reset()};
  m_state->m_wake = true;
  m_state->m_changed.notify_all();
  if (!result) {
    return std::unexpected{result.error()};
  }

  return m_session->m_queue.epoch();
}

DiscoveryResult DiscoveryDeliveryHandle::awaitReset(std::uint64_t epoch) const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant reset barrier"}};
  }

  std::unique_lock lock{m_state->m_mutex};
  if (epoch != m_session->m_queue.epoch()) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kCancelled, "reset was superseded"}};
  }

  m_state->m_changed.wait(lock, [&] {
    return !m_session->m_active || m_session->m_active_epoch >= epoch;
  });
  if (m_session->m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kCallbackFailure,
                                          "discovery callback failed"}};
  }

  if (m_session->m_queue.state() != DiscoveryDeliveryState::kRunning) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery delivery unavailable"}};
  }

  if (m_session->m_queue.epoch() != epoch) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kCancelled, "reset was superseded"}};
  }

  return {};
}

DiscoveryResult DiscoveryDeliveryHandle::beginStop() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant stop barrier"}};
  }

  std::lock_guard lock{m_state->m_mutex};
  m_session->m_queue.stop();
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return {};
}

DiscoveryResult DiscoveryDeliveryHandle::cancelFromCallback() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher != m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "not in callback context"}};
  }

  std::lock_guard lock{m_state->m_mutex};
  m_session->m_queue.stop();
  m_state->m_wake = true;
  m_state->m_changed.notify_all();

  return {};
}

DiscoveryResult DiscoveryDeliveryHandle::abandonUnpublished() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  std::unique_lock lock{m_state->m_mutex};
  if (m_session->m_active) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "unpublished callback is active"}};
  }

  m_session->m_queue.stop();
  m_state->m_sessions.erase(m_session->m_id);
  DiscoveryDispatcher::Handler retired{std::move(m_session->m_handler)};
  lock.unlock();
  retireHandler(retired, m_state.get());

  return {};
}

DiscoveryResult DiscoveryDeliveryHandle::awaitStop() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant stop barrier"}};
  }

  std::unique_lock lock{m_state->m_mutex};
  if (m_session->m_queue.state() != DiscoveryDeliveryState::kStopped) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stop not started"}};
  }

  m_state->m_changed.wait(lock, [&] { return !m_session->m_active; });
  m_state->m_sessions.erase(m_session->m_id);
  DiscoveryDispatcher::Handler retired{std::move(m_session->m_handler)};
  lock.unlock();
  retireHandler(retired, m_state.get());

  return {};
}

DiscoveryResult DiscoveryDeliveryHandle::reset() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant reset barrier"}};
  }

  std::lock_guard barrier{m_session->m_barrier};
  const auto epoch{beginReset()};
  if (!epoch) {
    return std::unexpected{epoch.error()};
  }

  return awaitReset(*epoch);
}

DiscoveryResult DiscoveryDeliveryHandle::stop() const {
  if (!m_state || !m_session) {
    return invalidHandle();
  }

  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant stop barrier"}};
  }

  std::lock_guard barrier{m_session->m_barrier};
  if (auto result{beginStop()}; !result) {
    return result;
  }

  return awaitStop();
}

DiscoveryDeliverySnapshot DiscoveryDeliveryHandle::snapshot() const {
  if (!m_state || !m_session) {
    return {0, 0, DiscoveryDeliveryState::kStopped, false};
  }

  std::lock_guard lock{m_state->m_mutex};

  return {m_session->m_queue.epoch(), m_session->m_queue.sequence(),
          m_session->m_queue.state(), m_session->m_callback_failed,
          m_session->m_queue.overflowed()};
}

DiscoveryDispatcher::DiscoveryDispatcher(std::size_t maxSessions,
                                         std::function<void()> workerFailure)
    : m_state{std::make_shared<DiscoveryDispatcherState>(
          maxSessions, std::move(workerFailure))},
      m_worker{"discovery-callback",
               [state = m_state] { runDispatcherSafely(state); },
               Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait} {
  if (!m_worker.exec()) {
    throw std::system_error{EAGAIN, std::generic_category(),
                            "starting discovery callback worker"};
  }

  {
    std::unique_lock lock{m_state->m_mutex};
    m_state->m_changed.wait(lock,
                            [this] { return m_state->m_startup_complete; });
    const int error{m_state->m_startup_error};
    if (error != 0) {
      lock.unlock();
      static_cast<void>(m_worker.wait());

      throw std::system_error{error, std::generic_category(),
                              "starting discovery callback worker"};
    }
  }

  m_worker_started = true;
}

DiscoveryDispatcher::~DiscoveryDispatcher() { static_cast<void>(shutdown()); }

bool DiscoveryDispatcher::inCallbackContext() const {
  return activeDispatcher == m_state.get();
}

std::expected<DiscoveryDeliveryHandle, DiscoveryError>
DiscoveryDispatcher::create(std::size_t maxData, std::size_t maxControl,
                            Handler handler) {
  std::lock_guard lock{m_state->m_mutex};
  if (maxData == 0 || maxControl == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "delivery limits must be nonzero"}};
  }

  if (m_state->m_shutdown) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery dispatcher stopped"}};
  }

  if (m_state->m_sessions.size() >= m_state->m_max_sessions ||
      m_state->m_next_id == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "delivery session limit reached"}};
  }

  const auto id{m_state->m_next_id++};
  auto session{std::make_shared<DiscoveryDeliverySession>(
      id, maxData, maxControl, std::move(handler))};
  m_state->m_sessions.emplace(id, session);

  return DiscoveryDeliveryHandle{m_state, std::move(session)};
}

DiscoveryResult DiscoveryDispatcher::shutdown() {
  if (activeDispatcher == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant shutdown barrier"}};
  }

  std::lock_guard barrier{m_shutdown_mutex};
  {
    std::lock_guard lock{m_state->m_mutex};
    m_state->m_shutdown = true;
    for (auto &[id, session] : m_state->m_sessions) {
      static_cast<void>(id);
      session->m_queue.stop();
    }

    m_state->m_wake = true;
    m_state->m_changed.notify_all();
  }

  if (m_worker_started) {
    try {
      static_cast<void>(m_worker.wait());
    } catch (const std::exception &error) {
      m_worker_error =
          DiscoveryError{DiscoveryErrorKind::kBackend, error.what()};
#if defined(__GLIBCXX__)
    } catch (const __cxxabiv1::__forced_unwind &) {
      throw;
#endif
    } catch (...) {
      m_worker_error = DiscoveryError{DiscoveryErrorKind::kBackend,
                                      "discovery callback worker failed"};
    }

    if (m_worker.isJoinable()) {
      return std::unexpected{*m_worker_error};
    }

    m_worker_started = false;
  }

  {
    std::lock_guard lock{m_state->m_mutex};
    if (m_state->m_worker_error) {
      m_worker_error = m_state->m_worker_error;
    }
  }

  while (true) {
    Handler retired{};
    {
      std::lock_guard lock{m_state->m_mutex};
      if (m_state->m_sessions.empty()) {
        break;
      }

      const auto session{m_state->m_sessions.begin()->second};
      retired = std::move(session->m_handler);
      m_state->m_sessions.erase(m_state->m_sessions.begin());
    }

    retireHandler(retired, m_state.get());
  }

  if (m_worker_error) {
    return std::unexpected{*m_worker_error};
  }

  return {};
}

} // namespace dmn::detail
