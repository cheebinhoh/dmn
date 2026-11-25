/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-command.cpp
 * @brief Bounded serialized command queue and reserved control slots.
 */

#include "dmn-discovery-command.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <map>
#include <new>
#include <optional>
#include <pthread.h>
#include <system_error>
#include <utility>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

namespace dmn::detail {

namespace {

using ControlKey = std::pair<std::uint64_t, DiscoveryCommandControl>;

struct CommandItem {
  explicit CommandItem(DiscoveryCommandWorker::Command command,
                       std::optional<ControlKey> control = {})
      : m_command{std::move(command)},
        m_completion{m_promise.get_future().share()}, m_control{control} {}

  DiscoveryCommandWorker::Command m_command;
  std::promise<DiscoveryResult> m_promise;
  DiscoveryCommandWorker::Completion m_completion;
  std::optional<ControlKey> m_control;
  DiscoveryError m_worker_failure{DiscoveryErrorKind::kBackend,
                                  "discovery worker failed"};
  DiscoveryError m_cancelled{DiscoveryErrorKind::kCancelled,
                             "discovery command cancelled"};
  bool m_running{false};
};

thread_local const DiscoveryCommandState *activeWorker{nullptr};

DiscoveryResult cancelled() {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kCancelled,
                                        "discovery command cancelled"}};
}

/** @brief Convert a command exception into an observable worker error. */
DiscoveryResult invokeCommand(const DiscoveryCommandWorker::Command &command) {
  try {
    return command();
#if defined(__GLIBCXX__)
  } catch (const __cxxabiv1::__forced_unwind &) {
    throw;
#endif
  } catch (const std::bad_alloc &) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                       "discovery command allocation failed"}};
  } catch (...) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "discovery command threw"}};
  }
}

} // namespace

struct DiscoveryCommandState {
  DiscoveryCommandState(std::size_t maxCommands, std::size_t maxControlSessions)
      : m_max_commands{maxCommands},
        m_max_control_sessions{maxControlSessions} {}

  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_startup_complete{false};
  int m_startup_error{0};
  std::deque<std::shared_ptr<CommandItem>> m_commands;
  std::deque<std::shared_ptr<CommandItem>> m_controls;
  std::map<ControlKey, std::shared_ptr<CommandItem>> m_control_slots;
  std::shared_ptr<CommandItem> m_running;
  std::size_t m_max_commands;
  std::size_t m_max_control_sessions;
  bool m_shutdown{false};
  std::shared_ptr<DiscoveryCommandPoller> m_poller;
  DiscoveryError m_worker_failure{DiscoveryErrorKind::kBackend,
                                  "discovery worker failed"};
  std::optional<DiscoveryError> m_poll_error;
  std::optional<DiscoveryError> m_cleanup_error;
};

namespace {

/**
 * @brief Run admitted controls before ordinary work on the state worker.
 */
void runCommands(const std::shared_ptr<DiscoveryCommandState> &state) {
  std::unique_lock lock{state->m_mutex};
  std::size_t commandsSincePoll{0};
  while (true) {
    const bool noCommands{state->m_controls.empty() &&
                          state->m_commands.empty()};
    if (!state->m_shutdown &&
        (noCommands || (state->m_poller && commandsSincePoll >= 16))) {
      if (state->m_poller) {
        const auto poller{state->m_poller};
        lock.unlock();
        activeWorker = state.get();
        const auto result{
            invokeCommand([&] { return poller->wait(noCommands); })};
        activeWorker = nullptr;
        lock.lock();
        commandsSincePoll = 0;
        if (!result) {
          state->m_poll_error = result.error();
          state->m_shutdown = true;
        }
      } else {
        state->m_changed.wait(lock, [&] {
          return state->m_shutdown || !state->m_controls.empty() ||
                 !state->m_commands.empty();
        });
      }
    }
    if (state->m_shutdown) {
      break;
    }
    if (state->m_controls.empty() && state->m_commands.empty()) {
      continue;
    }

    auto &queue{state->m_controls.empty() ? state->m_commands
                                          : state->m_controls};
    auto item{std::move(queue.front())};
    queue.pop_front();
    item->m_running = true;
    state->m_running = item;
    lock.unlock();
    activeWorker = state.get();
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/discovery/state-worker/fatal")) {
      throw std::runtime_error{"injected state worker failure"};
    }
#endif
    DiscoveryResult result{invokeCommand(item->m_command)};
    ++commandsSincePoll;
    activeWorker = nullptr;
    lock.lock();
    item->m_promise.set_value(std::move(result));
    state->m_running.reset();
    if (item->m_control) {
      state->m_control_slots.erase(*item->m_control);
    }
  }
  for (const auto &item : state->m_commands) {
    item->m_promise.set_value(std::unexpected{std::move(item->m_cancelled)});
  }
  for (const auto &item : state->m_controls) {
    item->m_promise.set_value(std::unexpected{std::move(item->m_cancelled)});
  }
  state->m_commands.clear();
  state->m_controls.clear();
  state->m_control_slots.clear();
  if (state->m_poller) {
    auto poller{std::move(state->m_poller)};
    lock.unlock();
    activeWorker = state.get();
    const auto result{invokeCommand([&] { return poller->stop(); })};
    activeWorker = nullptr;
    lock.lock();
    if (!result) {
      state->m_cleanup_error = result.error();
      if (state->m_poll_error) {
        state->m_poll_error->m_message +=
            "; poll cleanup failed: " + result.error().m_message;
      } else {
        state->m_poll_error = result.error();
      }
    }
  }
}

void runCommandsSafely(const std::shared_ptr<DiscoveryCommandState> &state) {
  try {
    // Native callbacks cannot propagate pthread cancellation through C APIs.
#ifdef FIU_ENABLE
    const int error{
        fiu_fail("dmn/discovery/state-worker/cancel-setup")
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
    runCommands(state);
    return;
#if defined(__GLIBCXX__)
  } catch (const __cxxabiv1::__forced_unwind &) {
    throw;
#endif
  } catch (...) {
    activeWorker = nullptr;
  }

  std::shared_ptr<DiscoveryCommandPoller> poller;
  // Complete the in-flight and queued commands before releasing native state.
  {
    std::lock_guard lock{state->m_mutex};
    state->m_shutdown = true;
    state->m_poll_error = std::move(state->m_worker_failure);
    if (state->m_running) {
      state->m_running->m_promise.set_value(
          std::unexpected{std::move(state->m_running->m_worker_failure)});
      state->m_running.reset();
    }
    for (const auto &item : state->m_commands) {
      item->m_promise.set_value(
          std::unexpected{std::move(item->m_worker_failure)});
    }
    for (const auto &item : state->m_controls) {
      item->m_promise.set_value(
          std::unexpected{std::move(item->m_worker_failure)});
    }
    state->m_commands.clear();
    state->m_controls.clear();
    state->m_control_slots.clear();
    poller = std::move(state->m_poller);
    state->m_changed.notify_all();
  }
  if (poller) {
    activeWorker = state.get();
    const auto stopped{invokeCommand([&] { return poller->stop(); })};
    activeWorker = nullptr;
    if (!stopped) {
      std::lock_guard lock{state->m_mutex};
      state->m_cleanup_error = stopped.error();
      state->m_poll_error->m_message +=
          "; poll cleanup failed: " + stopped.error().m_message;
    }
  }
}

} // namespace

DiscoveryCommandWorker::DiscoveryCommandWorker(std::size_t maxCommands,
                                               std::size_t maxControlSessions)
    : m_state{std::make_shared<DiscoveryCommandState>(maxCommands,
                                                      maxControlSessions)},
      m_worker{"discovery-state",
               [state = m_state] { runCommandsSafely(state); },
               Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait} {
  if (!m_worker.exec()) {
    throw std::system_error{EAGAIN, std::generic_category(),
                            "starting discovery state worker"};
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
                              "starting discovery state worker"};
    }
  }
  m_worker_started = true;
}

DiscoveryCommandWorker::~DiscoveryCommandWorker() {
  static_cast<void>(shutdown());
}

bool DiscoveryCommandWorker::inWorkerContext() const {
  return activeWorker == m_state.get();
}

void DiscoveryCommandWorker::wakePoller() noexcept {
  std::lock_guard lock{m_state->m_mutex};
  if (m_state->m_poller) {
    m_state->m_poller->wake();
  }
}

std::expected<DiscoveryCommandWorker::Completion, DiscoveryError>
DiscoveryCommandWorker::submit(Command command) {
  std::lock_guard lock{m_state->m_mutex};
  if (!command) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery command"}};
  }
  if (m_state->m_shutdown) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery worker stopped"}};
  }
  if (m_state->m_commands.size() >= m_state->m_max_commands) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery command queue full"}};
  }

  auto item{std::make_shared<CommandItem>(std::move(command))};
  m_state->m_commands.push_back(item);
  if (m_state->m_poller) {
    m_state->m_poller->wake();
  }
  m_state->m_changed.notify_one();
  return item->m_completion;
}

DiscoveryResult DiscoveryCommandWorker::execute(Command command) {
  if (!command) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery command"}};
  }
  if (activeWorker == m_state.get()) {
    return invokeCommand(command);
  }

  auto submitted{submit(std::move(command))};
  if (!submitted) {
    return std::unexpected{submitted.error()};
  }
  return submitted->get();
}

DiscoveryResult DiscoveryCommandWorker::attachPoller(
    std::shared_ptr<DiscoveryCommandPoller> poller) {
  if (!poller) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery poller"}};
  }
  return execute([state = m_state, poller = std::move(poller)]() {
    {
      std::lock_guard lock{state->m_mutex};
      if (state->m_poller || state->m_shutdown) {
        return DiscoveryResult{std::unexpected{DiscoveryError{
            DiscoveryErrorKind::kBackend, "discovery poller unavailable"}}};
      }
    }
    auto result{invokeCommand([&] { return poller->start(); })};
    if (!result) {
      auto stopped{invokeCommand([&] { return poller->stop(); })};
      if (!stopped) {
        return DiscoveryResult{std::unexpected{DiscoveryError{
            result.error().m_kind,
            result.error().m_message +
                "; poll cleanup failed: " + stopped.error().m_message}}};
      }
      return result;
    }
    {
      std::lock_guard lock{state->m_mutex};
      if (!state->m_shutdown) {
        state->m_poller = poller;
        return DiscoveryResult{};
      }
    }
    auto stopped{invokeCommand([&] { return poller->stop(); })};
    if (!stopped) {
      return stopped;
    }
    return cancelled();
  });
}

std::expected<DiscoveryCommandWorker::Completion, DiscoveryError>
DiscoveryCommandWorker::submitControl(std::uint64_t session,
                                      DiscoveryCommandControl control,
                                      Command command) {
  std::lock_guard lock{m_state->m_mutex};
  if (session == 0 || !command) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid discovery control"}};
  }
  if (m_state->m_shutdown) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery worker stopped"}};
  }

  const ControlKey key{session, control};
  if (const auto existing{m_state->m_control_slots.find(key)};
      existing != m_state->m_control_slots.end()) {
    return existing->second->m_completion;
  }
  const ControlKey stopKey{session, DiscoveryCommandControl::kStop};
  if (control == DiscoveryCommandControl::kReset &&
      m_state->m_control_slots.contains(stopKey)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "session stop already pending"}};
  }

  const bool sessionPresent{std::any_of(
      m_state->m_control_slots.begin(), m_state->m_control_slots.end(),
      [session](const auto &item) { return item.first.first == session; })};
  std::size_t sessionCount{0};
  std::uint64_t previous{0};
  for (const auto &[slot, item] : m_state->m_control_slots) {
    static_cast<void>(item);
    if (slot.first != previous) {
      previous = slot.first;
      ++sessionCount;
    }
  }
  if (!sessionPresent && sessionCount >= m_state->m_max_control_sessions) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery control slots full"}};
  }

  if (control == DiscoveryCommandControl::kStop) {
    const ControlKey resetKey{session, DiscoveryCommandControl::kReset};
    const auto reset{m_state->m_control_slots.find(resetKey)};
    if (reset != m_state->m_control_slots.end() && !reset->second->m_running) {
      auto item{reset->second};
      std::erase(m_state->m_controls, item);
      m_state->m_control_slots.erase(reset);
      item->m_promise.set_value(std::unexpected{std::move(item->m_cancelled)});
    }
  }

  auto item{std::make_shared<CommandItem>(std::move(command), key)};
  m_state->m_control_slots.emplace(key, item);
  try {
    if (control == DiscoveryCommandControl::kStop) {
      m_state->m_controls.push_front(item);
    } else {
      m_state->m_controls.push_back(item);
    }
  } catch (...) {
    m_state->m_control_slots.erase(key);
    throw;
  }
  if (m_state->m_poller) {
    m_state->m_poller->wake();
  }
  m_state->m_changed.notify_one();
  return item->m_completion;
}

DiscoveryResult DiscoveryCommandWorker::shutdown() {
  if (activeWorker == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant worker shutdown"}};
  }

  std::lock_guard barrier{m_shutdown_mutex};
  {
    std::lock_guard lock{m_state->m_mutex};
    m_state->m_shutdown = true;
    for (const auto &item : m_state->m_commands) {
      item->m_promise.set_value(std::unexpected{std::move(item->m_cancelled)});
    }
    for (const auto &item : m_state->m_controls) {
      item->m_promise.set_value(std::unexpected{std::move(item->m_cancelled)});
      m_state->m_control_slots.erase(*item->m_control);
    }
    m_state->m_commands.clear();
    m_state->m_controls.clear();
    if (m_state->m_poller) {
      m_state->m_poller->wake();
    }
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
                                      "discovery worker failed"};
    }
    if (m_worker.isJoinable()) {
      return std::unexpected{*m_worker_error};
    }
    m_worker_started = false;
  }
  if (m_worker_error) {
    return std::unexpected{*m_worker_error};
  }
  if (m_state->m_poll_error) {
    return std::unexpected{*m_state->m_poll_error};
  }
  return {};
}

std::expected<bool, DiscoveryError>
DiscoveryCommandWorker::joinAfterPollFailure() {
  {
    std::lock_guard lock{m_state->m_mutex};
    if (!m_state->m_poll_error) {
      return false;
    }
  }
  static_cast<void>(shutdown());
  std::lock_guard lock{m_state->m_mutex};
  if (m_state->m_cleanup_error) {
    return std::unexpected{*m_state->m_cleanup_error};
  }
  return true;
}

} // namespace dmn::detail
