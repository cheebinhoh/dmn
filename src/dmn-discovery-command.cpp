/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-command.cpp
 * @brief Bounded serialized command queue and reserved control slots.
 */

#include "dmn-discovery-command.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <utility>

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
  bool m_running{false};
};

thread_local const DiscoveryCommandState *activeWorker{nullptr};

DiscoveryResult cancelled() {
  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                        "discovery command cancelled"}};
}

/** @brief Convert a command exception into an observable worker error. */
DiscoveryResult invokeCommand(const DiscoveryCommandWorker::Command &command) {
  try {
    return command();
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
  std::deque<std::shared_ptr<CommandItem>> m_commands;
  std::deque<std::shared_ptr<CommandItem>> m_controls;
  std::map<ControlKey, std::shared_ptr<CommandItem>> m_control_slots;
  std::size_t m_max_commands;
  std::size_t m_max_control_sessions;
  bool m_shutdown{false};
};

namespace {

/**
 * @brief Run admitted controls before ordinary work on the state worker.
 */
void runCommands(const std::shared_ptr<DiscoveryCommandState> &state) {
  std::unique_lock lock{state->m_mutex};
  while (true) {
    state->m_changed.wait(lock, [&] {
      return state->m_shutdown || !state->m_controls.empty() ||
             !state->m_commands.empty();
    });
    if (state->m_shutdown) {
      return;
    }

    auto &queue{state->m_controls.empty() ? state->m_commands
                                          : state->m_controls};
    auto item{std::move(queue.front())};
    queue.pop_front();
    item->m_running = true;
    lock.unlock();
    activeWorker = state.get();
    DiscoveryResult result{invokeCommand(item->m_command)};
    activeWorker = nullptr;
    lock.lock();
    item->m_promise.set_value(std::move(result));
    if (item->m_control) {
      state->m_control_slots.erase(*item->m_control);
    }
  }
}

} // namespace

DiscoveryCommandWorker::DiscoveryCommandWorker(std::size_t maxCommands,
                                               std::size_t maxControlSessions)
    : m_state{std::make_shared<DiscoveryCommandState>(maxCommands,
                                                      maxControlSessions)},
      m_worker{[state = m_state] { runCommands(state); }} {}

DiscoveryCommandWorker::~DiscoveryCommandWorker() {
  static_cast<void>(shutdown());
}

bool DiscoveryCommandWorker::inWorkerContext() const {
  return activeWorker == m_state.get();
}

std::expected<DiscoveryCommandWorker::Completion, DiscoveryError>
DiscoveryCommandWorker::submit(Command command) {
  std::lock_guard lock{m_state->m_mutex};
  if (!command) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "empty discovery command"}};
  }
  if (m_state->m_shutdown) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "discovery worker stopped"}};
  }
  if (m_state->m_commands.size() >= m_state->m_max_commands) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery command queue full"}};
  }

  auto item{std::make_shared<CommandItem>(std::move(command))};
  m_state->m_commands.push_back(item);
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
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
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
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
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
      item->m_promise.set_value(cancelled());
    }
  }

  auto item{std::make_shared<CommandItem>(std::move(command), key)};
  m_state->m_control_slots.emplace(key, item);
  if (control == DiscoveryCommandControl::kStop) {
    m_state->m_controls.push_front(item);
  } else {
    m_state->m_controls.push_back(item);
  }
  m_state->m_changed.notify_one();
  return item->m_completion;
}

DiscoveryResult DiscoveryCommandWorker::shutdown() {
  if (activeWorker == m_state.get()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant worker shutdown"}};
  }

  std::lock_guard barrier{m_shutdown_mutex};
  {
    std::lock_guard lock{m_state->m_mutex};
    m_state->m_shutdown = true;
    for (const auto &item : m_state->m_commands) {
      item->m_promise.set_value(cancelled());
    }
    for (const auto &item : m_state->m_controls) {
      item->m_promise.set_value(cancelled());
      m_state->m_control_slots.erase(*item->m_control);
    }
    m_state->m_commands.clear();
    m_state->m_controls.clear();
    m_state->m_changed.notify_all();
  }
  if (m_worker.joinable()) {
    m_worker.join();
  }

  return {};
}

} // namespace dmn::detail
