/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-dispatcher.hpp
 * @brief Private multi-session callback dispatcher and drain barriers.
 *
 * One callback thread serves all sessions. Handles retain their stopped
 * snapshot after client shutdown, but cannot create new work. This component
 * does not yet own the state worker or native polling handles.
 */

#ifndef DMN_DISCOVERY_DISPATCHER_HPP_
#define DMN_DISCOVERY_DISPATCHER_HPP_

#include "dmn-discovery-delivery.hpp"
#include "dmn-proc.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace dmn::detail {

struct DiscoveryDispatcherState;
struct DiscoveryDeliverySession;

/**
 * @brief Copyable session handle with an independently retained status.
 *
 * The owner must coordinate destruction with concurrent calls. A stopped
 * handle remains safe to inspect even after its dispatcher is destroyed.
 */
class DiscoveryDeliveryHandle {
public:
  DiscoveryDeliveryHandle() = default;

  /**
   * @brief Submit an owned data event for a current epoch.
   *
   * @param epoch Epoch captured when the native work was started.
   * @param event Owned browser delta.
   * @return Success or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult postData(std::uint64_t epoch,
                                         DiscoveryEvent event) const;

  /**
   * @brief Submit an owned registration transition for a current epoch.
   *
   * @param epoch Current delivery epoch.
   * @param status Owned registration status.
   * @return Success or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult
  postRegistration(std::uint64_t epoch,
                   DiscoveryRegistrationStatus status) const;

  /** @brief Queue an epoch-changing registration backend transition. */
  [[nodiscard]] DiscoveryResult
  registrationBackendChange(DiscoveryRegistrationStatus status) const;

  /**
   * @brief Submit a required control event for a current epoch.
   *
   * @param epoch Epoch captured when the work was started.
   * @param control Transition to deliver.
   * @return Success or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult postControl(std::uint64_t epoch,
                                            DiscoveryControl control) const;

  /**
   * @brief Invalidate an epoch after backend loss and queue ordered controls.
   *
   * @param epoch Epoch captured by the backend client.
   * @return Success or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendLost(std::uint64_t epoch) const;

  /**
   * @brief Start a fresh backend generation and queue its availability.
   *
   * @param epoch Epoch captured by the reconnect attempt.
   * @return Success or a typed stale/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendAvailable(std::uint64_t epoch) const;

  /**
   * @brief Latch an overflow caused by another browser resource.
   *
   * @return A typed overflow error.
   */
  [[nodiscard]] DiscoveryResult failOverflow() const;

  /** @brief Latch overflow and wake delivery without allocating an error. */
  void latchOverflow() const;

  /** @brief Whether this thread is executing this client's callback. */
  [[nodiscard]] bool inCallbackContext() const;

  /**
   * @brief Begin a reset without waiting for a callback drain.
   *
   * This internal split barrier is intended for the state worker. Its caller
   * must serialize reset/stop operations and later call awaitReset() from
   * outside the state and callback workers.
   *
   * @return New epoch or a typed reentrant/stopped/overflow error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError> beginReset() const;

  /**
   * @brief Wait until callbacks older than the supplied reset epoch finish.
   *
   * @param epoch Epoch returned by beginReset().
   * @return Success or a typed terminal/reentrant error.
   */
  [[nodiscard]] DiscoveryResult awaitReset(std::uint64_t epoch) const;

  /**
   * @brief Invalidate queued delivery without waiting for in-flight callbacks.
   *
   * @return Success or a typed reentrant error.
   */
  [[nodiscard]] DiscoveryResult beginStop() const;

  /** @brief Invalidate delivery from its callback without waiting for drain. */
  [[nodiscard]] DiscoveryResult cancelFromCallback() const;

  /** @brief Discard an unpublished session whose initial event was not queued.
   */
  [[nodiscard]] DiscoveryResult abandonUnpublished() const;

  /**
   * @brief Drain callbacks after beginStop() and release the session slot.
   *
   * @return Success or a typed reentrant error.
   */
  [[nodiscard]] DiscoveryResult awaitStop() const;

  /**
   * @brief Invalidate queued events and drain any old-epoch callback.
   *
   * @return Success or a typed reentrant/stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult reset() const;

  /**
   * @brief Stop this session and drain any callback already in flight.
   *
   * @return Success or a typed reentrant error.
   */
  [[nodiscard]] DiscoveryResult stop() const;

  /**
   * @brief Read the owned status without waiting for user callbacks.
   *
   * @return Current epoch, sequence and persistent terminal state.
   */
  [[nodiscard]] DiscoveryDeliverySnapshot snapshot() const;

private:
  friend class DiscoveryDispatcher;
  DiscoveryDeliveryHandle(std::shared_ptr<DiscoveryDispatcherState> state,
                          std::shared_ptr<DiscoveryDeliverySession> session);

  std::shared_ptr<DiscoveryDispatcherState> m_state;
  std::shared_ptr<DiscoveryDeliverySession> m_session;
};

/**
 * @brief Serve bounded sessions on one owned callback worker.
 *
 * Barrier calls from this dispatcher's callback or callback-capture cleanup
 * context are rejected. An exception from one handler latches only that
 * session's failure.
 */
class DiscoveryDispatcher {
public:
  using Handler = std::function<void(DiscoveryDelivery)>;

  /**
   * @brief Start one callback thread with a bounded session count.
   *
   * @param maxSessions Maximum concurrently admitted sessions.
   */
  explicit DiscoveryDispatcher(std::size_t maxSessions,
                               std::function<void()> workerFailure = {});
  ~DiscoveryDispatcher();
  DiscoveryDispatcher(const DiscoveryDispatcher &) = delete;
  DiscoveryDispatcher &operator=(const DiscoveryDispatcher &) = delete;

  /** @brief Whether this thread is executing this dispatcher's callback. */
  [[nodiscard]] bool inCallbackContext() const;

  /**
   * @brief Admit one session without starting a browser or registration.
   *
   * @param maxData Maximum queued data events.
   * @param maxControl Maximum queued control events.
   * @param handler User callback, invoked without the state lock.
   * @return Owned handle or a typed invalid/stopped/resource error.
   */
  [[nodiscard]] std::expected<DiscoveryDeliveryHandle, DiscoveryError>
  create(std::size_t maxData, std::size_t maxControl, Handler handler);

  /**
   * @brief Stop all sessions and join the callback worker.
   *
   * @return Success or a typed reentrant error.
   */
  [[nodiscard]] DiscoveryResult shutdown();

private:
  std::shared_ptr<DiscoveryDispatcherState> m_state;
  std::mutex m_shutdown_mutex;
  Dmn_Proc m_worker;
  bool m_worker_started{false};
  std::optional<DiscoveryError> m_worker_error;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_DISPATCHER_HPP_
