/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-session.hpp
 * @brief Private browser, state-worker and callback-dispatch integration.
 *
 * This deterministic seam has no native daemon or network implementation.
 * It is not installed as a public API and must not be used to claim discovery
 * support before the platform adapters are implemented and qualified.
 */

#ifndef DMN_DISCOVERY_SESSION_HPP_
#define DMN_DISCOVERY_SESSION_HPP_

#include "dmn-discovery-command.hpp"
#include "dmn-discovery-dispatcher.hpp"

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <vector>

namespace dmn::detail {

struct DiscoverySessionState;

/** @brief Owned browser view and authoritative delivery lifecycle status. */
struct DiscoverySessionSnapshot {
  DiscoveryDeliverySnapshot m_delivery;
  DiscoverySnapshot m_browser;
  bool m_backend_available;
  std::size_t m_native_operations;
  std::optional<DiscoveryError> m_cleanup_error;
  std::size_t m_joined_reset_callers; ///< Callers sharing the active reset.
  bool m_stop_pending; ///< Stop has been requested, including after stop.
};

/** @brief Copyable private browser handle with synchronous barriers. */
class DiscoverySessionHandle {
public:
  DiscoverySessionHandle() = default;

  /**
   * @brief Apply one fake native browse contribution on the state worker.
   *
   * @param epoch Backend epoch captured by the source.
   * @param key Owned service identity.
   * @param contribution Native browse transport.
   * @return Source generation or a typed stale/limit error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError>
  observe(std::uint64_t epoch, DiscoveryServiceKey key,
          DiscoveryContribution contribution) const;

  /**
   * @brief Remove one still-current fake native browse contribution.
   *
   * @param epoch Backend generation captured by the source.
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Source generation returned by observe().
   * @return Success or a typed stale, backend or overflow error.
   */
  [[nodiscard]] DiscoveryResult
  removeObservation(std::uint64_t epoch, DiscoveryServiceKey key,
                    DiscoveryContribution contribution,
                    std::uint64_t generation) const;

  /**
   * @brief Apply one current source's owned record update.
   *
   * The update runs on the state worker and must not re-enter this session.
   * Resource failures invalidate the session and cancel its fake native work.
   *
   * @param epoch Backend generation captured by the source.
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Source generation returned by observe().
   * @param update Synchronous record-model operation.
   * @return Success or a typed stale, backend or overflow error.
   */
  [[nodiscard]] DiscoveryResult updateRecords(
      std::uint64_t epoch, DiscoveryServiceKey key,
      DiscoveryContribution contribution, std::uint64_t generation,
      std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update)
      const;

  /**
   * @brief Submit an explicit local resolution refresh.
   *
   * @param key Currently observed service identity.
   * @return Refresh ID or a typed error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError>
  refresh(DiscoveryServiceKey key) const;

  /**
   * @brief Process fake-clock refresh deadlines on the state worker.
   *
   * @param epoch Backend generation captured by the timer.
   * @return Success or a typed stale, stopped or overflow error.
   */
  [[nodiscard]] DiscoveryResult processTimers(std::uint64_t epoch) const;

  /**
   * @brief Record a current native attempt failure and schedule its retry.
   *
   * @param epoch Backend generation captured by the attempt.
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Source generation returned by observe().
   * @param attempt Failed record or address operation.
   * @param operationGeneration Failed operation lifetime token.
   * @param resolutionGeneration Address resolution lifetime token.
   * @param recoverable Whether a bounded retry should be scheduled.
   * @param message Non-sensitive error detail.
   * @return Success or a typed stale, backend or overflow error.
   */
  [[nodiscard]] DiscoveryResult
  failAttempt(std::uint64_t epoch, DiscoveryServiceKey key,
              DiscoveryContribution contribution, std::uint64_t generation,
              DiscoveryAttempt attempt, std::uint64_t operationGeneration,
              std::uint64_t resolutionGeneration, bool recoverable,
              std::string message) const;

  /**
   * @brief Clear a current attempt's failure after successful recovery.
   *
   * @param epoch Backend generation captured by the attempt.
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Source generation returned by observe().
   * @param attempt Recovered record or address operation.
   * @param operationGeneration Current operation lifetime token.
   * @param resolutionGeneration Address resolution lifetime token.
   * @return Success or a typed stale, backend or overflow error.
   */
  [[nodiscard]] DiscoveryResult
  succeedAttempt(std::uint64_t epoch, DiscoveryServiceKey key,
                 DiscoveryContribution contribution, std::uint64_t generation,
                 DiscoveryAttempt attempt, std::uint64_t operationGeneration,
                 std::uint64_t resolutionGeneration) const;

  /**
   * @brief Collect retries due at the fake clock's current time.
   *
   * @param epoch Backend generation captured by the timer.
   * @return Due operation IDs or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] std::expected<std::vector<DiscoveryAttemptId>, DiscoveryError>
  takeDueRetries(std::uint64_t epoch) const;

  /**
   * @brief Reserve a fake native operation in the current backend generation.
   *
   * @param epoch Generation captured by the fake backend.
   * @return Operation ID or a typed stale/overflow error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError>
  beginNative(std::uint64_t epoch) const;

  /**
   * @brief Free a previously admitted fake native operation.
   *
   * @param epoch Generation captured when the operation was started.
   * @param operation ID returned by beginNative().
   * @return Success or a typed stale/unknown-operation error.
   */
  [[nodiscard]] DiscoveryResult finishNative(std::uint64_t epoch,
                                             std::uint64_t operation) const;

  /**
   * @brief Apply a fake backend disconnect and discard the old view.
   *
   * @param epoch Generation captured by the disconnected backend.
   * @return Success or a typed stale/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendLost(std::uint64_t epoch) const;

  /**
   * @brief Apply a fake reconnect and create a fresh backend generation.
   *
   * @param epoch Generation captured by the reconnect attempt.
   * @return Success or a typed stale/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendAvailable(std::uint64_t epoch) const;

  /**
   * @brief Reset the local view and drain callbacks from its previous epoch.
   *
   * @return Success or a typed reentrant/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult clearCache() const;

  /**
   * @brief Stop this browser, invalidate work and drain callbacks.
   *
   * @return Success or a typed reentrant error.
   */
  [[nodiscard]] DiscoveryResult stop() const;

  /**
   * @brief Copy the browser's latest committed local observation and status.
   *
   * @return Owned snapshot; it remains available after client shutdown.
   */
  [[nodiscard]] DiscoverySessionSnapshot snapshot() const;

private:
  friend class DiscoverySessionClient;
  explicit DiscoverySessionHandle(std::shared_ptr<DiscoverySessionState> state);
  std::shared_ptr<DiscoverySessionState> m_state;
};

/**
 * @brief Private client owning a state worker and one callback dispatcher.
 *
 * The owner must coordinate client destruction with concurrent methods.
 * This fake seam starts no native browser or network operation.
 */
class DiscoverySessionClient {
public:
  using Handler = DiscoveryDispatcher::Handler;

  /**
   * @brief Configure a client with a simulated initial backend state.
   *
   * Workers start only after options and the clock are validated by browse().
   *
   * @param options Finite resource limits.
   * @param backendAvailable Whether the fake backend is initially available.
   * @param clock Monotonic clock used for deterministic deadline tests.
   */
  explicit DiscoverySessionClient(
      const DiscoveryOptions &options, bool backendAvailable = true,
      DiscoveryBrowser::Clock clock = std::chrono::steady_clock::now);
  ~DiscoverySessionClient();
  DiscoverySessionClient(const DiscoverySessionClient &) = delete;
  DiscoverySessionClient &operator=(const DiscoverySessionClient &) = delete;

  /**
   * @brief Create an empty, explicitly requested fake browser.
   *
   * @param handler User callback for owned events.
   * @return A browser handle or a typed validation/resource error.
   */
  [[nodiscard]] std::expected<DiscoverySessionHandle, DiscoveryError>
  browse(Handler handler);

  /**
   * @brief Drive one fake native callback on the serialized state worker.
   *
   * @param callback Nonblocking fake backend callback.
   * @return Callback result or a typed admission/exception error.
   */
  [[nodiscard]] DiscoveryResult
  dispatchNative(DiscoveryCommandWorker::Command callback);

  /**
   * @brief Stop all browsers, then join state and callback workers.
   *
   * @return Success or a typed reentrant/cleanup error.
   */
  [[nodiscard]] DiscoveryResult shutdown();

private:
  DiscoveryOptions m_options;
  bool m_initial_backend_available;
  DiscoveryBrowser::Clock m_clock;
  std::shared_ptr<DiscoveryCommandWorker> m_commands;
  std::unique_ptr<DiscoveryDispatcher> m_dispatcher;
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::vector<std::shared_ptr<DiscoverySessionState>> m_sessions;
  std::uint64_t m_next_session{1};
  bool m_closing{false};
  bool m_closed{false};
  std::optional<DiscoveryError> m_shutdown_error;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_SESSION_HPP_
