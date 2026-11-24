/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-command.hpp
 * @brief Private bounded state-worker command admission.
 *
 * Ordinary commands have bounded nonblocking admission. Reset and stop have
 * separately bounded, per-session control slots. Duplicate pending controls
 * share one completion; stop cannot be starved by the ordinary queue.
 */

#ifndef DMN_DISCOVERY_COMMAND_HPP_
#define DMN_DISCOVERY_COMMAND_HPP_

#include "dmn-discovery-core.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace dmn::detail {

/** @brief Reserved per-session control operation. */
enum class DiscoveryCommandControl { kReset, kStop };

struct DiscoveryCommandState;

/**
 * @brief One serialized worker with finite ordinary and control admission.
 *
 * Work runs outside the queue lock. A barrier command must not wait for user
 * callback completion on this worker; its caller coordinates that drain.
 * The owner externally coordinates destruction with method calls.
 */
class DiscoveryCommandWorker {
public:
  using Command = std::function<DiscoveryResult()>;
  using Completion = std::shared_future<DiscoveryResult>;

  /**
   * @brief Start the worker.
   *
   * @param maxCommands Maximum queued ordinary commands.
   * @param maxControlSessions Maximum sessions with pending controls.
   */
  DiscoveryCommandWorker(std::size_t maxCommands,
                         std::size_t maxControlSessions);
  ~DiscoveryCommandWorker();
  DiscoveryCommandWorker(const DiscoveryCommandWorker &) = delete;
  DiscoveryCommandWorker &operator=(const DiscoveryCommandWorker &) = delete;

  /** @brief Whether the caller is executing on this state worker. */
  [[nodiscard]] bool inWorkerContext() const;

  /**
   * @brief Admit an ordinary command without waiting for queue capacity.
   *
   * @param command Operation executed on the worker.
   * @return Shared completion or a typed admission error.
   */
  [[nodiscard]] std::expected<Completion, DiscoveryError>
  submit(Command command);

  /**
   * @brief Execute an operation on the state worker and wait for its result.
   *
   * A native callback already on this worker executes directly without
   * queueing or waiting for itself. Such callbacks must not hold locks that
   * the command acquires. Other threads use bounded ordinary admission.
   *
   * @param command Operation executed on the worker.
   * @return Operation result or a typed admission/exception error.
   */
  [[nodiscard]] DiscoveryResult execute(Command command);

  /**
   * @brief Admit or coalesce a reserved session control.
   *
   * A stop replaces a queued reset for the same session; its completion is
   * failed explicitly. A running reset finishes before stop. A reset cannot
   * supersede a pending stop.
   *
   * @param session Nonzero session identifier.
   * @param control Reset or stop.
   * @param command Operation executed on the worker.
   * @return Shared completion or a typed admission error.
   */
  [[nodiscard]] std::expected<Completion, DiscoveryError>
  submitControl(std::uint64_t session, DiscoveryCommandControl control,
                Command command);

  /**
   * @brief Cancel queued commands, drain the running command and join.
   *
   * @return Success or a typed reentrant-worker error.
   */
  [[nodiscard]] DiscoveryResult shutdown();

private:
  std::shared_ptr<DiscoveryCommandState> m_state;
  std::mutex m_shutdown_mutex;
  std::thread m_worker;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_COMMAND_HPP_
