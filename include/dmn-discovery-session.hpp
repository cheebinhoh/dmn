/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-session.hpp
 * @brief Private browser, state-worker and callback-dispatch integration.
 *
 * The session itself has no daemon or network implementation. Private
 * adapters may attach worker-owned cancellation hooks and pollers. It is not
 * installed as a public API.
 */

#ifndef DMN_DISCOVERY_SESSION_HPP_
#define DMN_DISCOVERY_SESSION_HPP_

#include "dmn-discovery-command.hpp"
#include "dmn-discovery-dispatcher.hpp"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace dmn::detail {

/** @brief Internal value-conversion failure, distinct from a user callback. */
class DiscoveryDeliveryFailure final : public std::exception {
public:
  explicit DiscoveryDeliveryFailure(DiscoveryError error)
      : m_error{std::move(error)} {}
  [[nodiscard]] const DiscoveryError &error() const noexcept { return m_error; }

  [[nodiscard]] DiscoveryError takeError() noexcept {
    return std::move(m_error);
  }

  [[nodiscard]] const char *what() const noexcept override {
    return m_error.m_message.c_str();
  }

private:
  DiscoveryError m_error;
};

struct DiscoverySessionState;
struct DiscoveryRegistrationSessionState;

/** @brief Fault points in the private fake registration integration. */
enum class DiscoveryRegistrationFaultPoint {
  kAdmission,
  kInitialDelivery,
  kTransitionDelivery,
  kCallbackCleanup
};

/**
 * @brief Copyable private registration handle with synchronous withdrawal.
 *
 * Status remains available after client shutdown. The owner coordinates
 * destruction with concurrent calls; stop from callback context is rejected.
 */
class DiscoveryRegistrationHandle {
public:
  DiscoveryRegistrationHandle() = default;

  /**
   * @brief Apply a fake backend establishment callback.
   *
   * @param generation Native-operation generation.
   * @return Success or a stale, stopped, or overflow error.
   */
  [[nodiscard]] DiscoveryResult established(std::uint64_t generation) const;

  /**
   * @brief Apply a fake backend collision callback without renaming.
   *
   * @param generation Native-operation generation.
   * @param message Non-sensitive error detail.
   * @return Success or a stale, stopped, or overflow error.
   */
  [[nodiscard]] DiscoveryResult collision(std::uint64_t generation,
                                          std::string message) const;

  /**
   * @brief Apply a fake backend failure callback.
   *
   * @param generation Native-operation generation.
   * @param message Non-sensitive error detail.
   * @return Success or a stale, stopped, or overflow error.
   */
  [[nodiscard]] DiscoveryResult failed(std::uint64_t generation,
                                       std::string message) const;

  /** @brief Apply a typed backend failure for a current generation. */
  [[nodiscard]] DiscoveryResult failed(std::uint64_t generation,
                                       DiscoveryError error) const;

  /** @brief Invalidate fake native work after backend loss. */
  [[nodiscard]] DiscoveryResult backendLost() const;

  /** @brief Re-register after fake backend recovery. */
  [[nodiscard]] DiscoveryResult backendAvailable() const;

  /** @brief Report a fatal backend failure, including while unavailable. */
  [[nodiscard]] DiscoveryResult backendFailed(DiscoveryError error) const;

  /** @brief Withdraw and drain callbacks; repeated calls are safe. */
  [[nodiscard]] DiscoveryResult stop() const;

  /** @brief Copy the retained registration status. */
  [[nodiscard]] DiscoveryRegistrationStatus status() const;

  /** @brief Read the current fake native callback generation. */
  [[nodiscard]] std::uint64_t generation() const;

  /** @brief Whether fake native registration work is currently owned. */
  [[nodiscard]] bool nativeActive() const;

  /**
   * @brief Install a worker-owned withdrawal hook for a native adapter.
   *
   * The hook must not call back into this handle. It is invoked on the state
   * worker, without the registration mutex held, before stop completes.
   */
  [[nodiscard]] DiscoveryResult
  setNativeCancellation(std::function<void()> cancellation) const;

  /** @brief Disarm the hook after native cleanup on the state worker. */
  [[nodiscard]] DiscoveryResult nativeCleanupComplete() const;

  /** @brief Mark a resource failure without allocating. */
  void failResourceLimit() const;

private:
  friend class DiscoverySessionClient;
  explicit DiscoveryRegistrationHandle(
      std::shared_ptr<DiscoveryRegistrationSessionState> state);
  std::shared_ptr<DiscoveryRegistrationSessionState> m_state;
};

/** @brief Owned browser view and authoritative delivery lifecycle status. */
struct DiscoverySessionSnapshot {
  DiscoveryDeliverySnapshot m_delivery;
  DiscoverySnapshot m_browser;
  bool m_backend_available;
  std::size_t m_native_operations;
  std::optional<DiscoveryError> m_cleanup_error;
  std::size_t m_joined_reset_callers; ///< Callers sharing the active reset.
  bool m_stop_pending; ///< Stop has been requested, including after stop.
  bool m_explicit_stop{false};
};

/** @brief Copyable private browser handle with synchronous barriers. */
class DiscoverySessionHandle {
public:
  DiscoverySessionHandle() = default;

  /** @brief Disarm the hooks after native cleanup on the state worker. */
  [[nodiscard]] DiscoveryResult nativeCleanupComplete() const;

  /** @brief Mark a resource failure without allocating. */
  void failResourceLimit(bool backendUnavailable = false) const;

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
          DiscoveryContribution contribution,
          DiscoveryProvenance provenance = {}) const;

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
              std::string message,
              std::optional<DiscoveryError> native = {}) const;

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

  /** @brief Read the next worker retry or refresh deadline. */
  [[nodiscard]] std::expected<std::optional<DiscoveryBrowser::TimePoint>,
                              DiscoveryError>
  nextDeadline(std::uint64_t epoch) const;

  /** @brief Invalidate a browser after a terminal native backend failure. */
  [[nodiscard]] DiscoveryResult backendFailed(DiscoveryError error) const;

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
 * This session starts no native operation unless a private adapter attaches
 * one; the adapter owns backend state and callback-to-worker integration.
 */
class DiscoverySessionClient {
public:
  using Handler = DiscoveryDispatcher::Handler;
  using RegistrationHandler = std::function<void(DiscoveryRegistrationStatus)>;
  using RegistrationFault =
      std::function<void(DiscoveryRegistrationFaultPoint)>;
  struct BrowserLifecycle {
    std::function<DiscoveryResult()> m_cancel;
    std::function<DiscoveryResult()> m_reset;
  };

  /**
   * @brief Configure a client with a simulated initial backend state.
   *
   * Workers start only after options and the clock are validated by browse().
   *
   * @param options Finite resource limits.
   * @param backendAvailable Whether the fake backend is initially available.
   * @param clock Monotonic clock used for deterministic deadline tests.
   * @param registrationFault Optional deterministic fake fault injection.
   */
  explicit DiscoverySessionClient(
      const DiscoveryOptions &options, bool backendAvailable = true,
      DiscoveryBrowser::Clock clock = std::chrono::steady_clock::now,
      RegistrationFault registrationFault = {});
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
  browse(Handler handler, BrowserLifecycle lifecycle = {});

  /**
   * @brief Accept an explicitly requested fake advertisement.
   *
   * @param advertisement Owned service description.
   * @param handler User callback for registration changes.
   * @param nativeCancellation Optional worker-owned native withdrawal hook,
   * installed before the initial event can be delivered.
   * @return Registration handle or a typed validation/resource error.
   */
  [[nodiscard]] std::expected<DiscoveryRegistrationHandle, DiscoveryError>
  advertise(DiscoveryAdvertisement advertisement, RegistrationHandler handler,
            std::function<void()> nativeCancellation = {});

  /**
   * @brief Start a native poller on the same worker used by session mutations.
   *
   * The poller may issue immediate worker-context callbacks during startup.
   * Only one poller may be attached; shutdown stops it after session barriers.
   */
  [[nodiscard]] DiscoveryResult
  attachPoller(std::shared_ptr<DiscoveryCommandPoller> poller);

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
  [[nodiscard]] DiscoveryResult ensureWorkers();
  void handleDispatcherFailure();

  DiscoveryOptions m_options;
  bool m_initial_backend_available;
  DiscoveryBrowser::Clock m_clock;
  RegistrationFault m_registration_fault;
  std::shared_ptr<DiscoveryCommandWorker> m_commands;
  std::unique_ptr<DiscoveryDispatcher> m_dispatcher;
  std::mutex m_mutex;
  std::condition_variable m_changed;
  std::vector<std::shared_ptr<DiscoverySessionState>> m_sessions;
  std::vector<std::shared_ptr<DiscoveryRegistrationSessionState>>
      m_registrations;
  std::uint64_t m_next_session{1};
  bool m_closing{false};
  bool m_closed{false};
  std::optional<DiscoveryError> m_shutdown_error;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_SESSION_HPP_
