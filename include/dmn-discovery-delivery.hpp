/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-delivery.hpp
 * @brief Private bounded browser event-delivery state.
 *
 * Separate data and control queues are merged by sequence. Overflow clears
 * queued deltas and uses a reserved notification slot; it remains latched
 * until an explicit reset. The caller serializes access and owns any worker
 * or callback-drain synchronization.
 */

#ifndef DMN_DISCOVERY_DELIVERY_HPP_
#define DMN_DISCOVERY_DELIVERY_HPP_

#include "dmn-discovery-core.hpp"
#include "dmn-discovery-registration.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace dmn::detail {

/** @brief Non-data transition delivered by a browser. */
enum class DiscoveryControl {
  kReset,
  kBackendAvailable,
  kBackendUnavailable,
  kOverflow
};

/** @brief Persistent admission and lifecycle status. */
enum class DiscoveryDeliveryState { kRunning, kOverflow, kStopped };

/** @brief Owned data or control event in the common sequence space. */
struct DiscoveryDelivery {
  std::uint64_t m_epoch;
  std::uint64_t m_sequence;
  std::optional<DiscoveryEvent> m_data;
  std::optional<DiscoveryControl> m_control;
  std::optional<DiscoveryRegistrationStatus> m_registration{};
};

/** @brief Owned status independent of callback delivery. */
struct DiscoveryDeliverySnapshot {
  std::uint64_t m_epoch;
  std::uint64_t m_sequence;
  DiscoveryDeliveryState m_state;
  bool m_callback_failed;
  bool m_overflowed{false};
};

/**
 * @brief Caller-serialized bounded data/control queues with an emergency slot.
 *
 * The queue does not run handlers. Its owner must invalidate associated
 * backend work when admission fails and drain any callback already running.
 */
class DiscoveryDeliveryQueue {
public:
  /**
   * @brief Construct an initially empty running queue.
   *
   * @param maxData Maximum queued data events.
   * @param maxControl Maximum queued control events.
   */
  DiscoveryDeliveryQueue(std::size_t maxData, std::size_t maxControl);

  /**
   * @brief Assign sequence/epoch and enqueue an owned data event.
   *
   * @param event Browser delta; its input epoch/sequence are replaced.
   * @return Success or a typed overflow/stopped error.
   */
  [[nodiscard]] DiscoveryResult pushData(DiscoveryEvent event);

  /**
   * @brief Enqueue a registration transition in the bounded data queue.
   *
   * @param status Owned registration transition.
   * @return Success or a typed overflow/stopped error.
   */
  [[nodiscard]] DiscoveryResult
  pushRegistration(DiscoveryRegistrationStatus status);

  /**
   * @brief Queue a registration backend transition as a reserved control.
   *
   * Loss discards stale data; both transitions advance the delivery epoch.
   */
  [[nodiscard]] DiscoveryResult
  registrationBackendChange(DiscoveryRegistrationStatus status);

  /**
   * @brief Assign sequence/epoch and enqueue a required control transition.
   *
   * Overflow itself is reserved for the emergency slot.
   *
   * @param control Transition to enqueue.
   * @return Success or a typed overflow/stopped error.
   */
  [[nodiscard]] DiscoveryResult pushControl(DiscoveryControl control);

  /**
   * @brief Discard queued data and enqueue a new-epoch reset.
   *
   * Undelivered required controls remain ordered ahead of the reset.
   *
   * @return Success or a typed stopped/generation-exhaustion error.
   */
  [[nodiscard]] DiscoveryResult reset();

  /**
   * @brief Discard old-epoch deltas and report daemon loss followed by reset.
   *
   * @return Success or a typed stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendLost();

  /**
   * @brief Advance the backend generation and queue its availability.
   *
   * Pending loss controls remain ordered ahead of this transition.
   *
   * @return Success or a typed stopped/overflow error.
   */
  [[nodiscard]] DiscoveryResult backendAvailable();

  /**
   * @brief Latch overflow when another browser resource limit is exceeded.
   *
   * @return A typed overflow error.
   */
  [[nodiscard]] DiscoveryResult failOverflow();

  /** @brief Latch overflow without allocating or constructing an error. */
  void latchOverflow() noexcept;

  /**
   * @brief Stop admission and discard queued or emergency notifications.
   */
  void stop();

  /**
   * @brief Remove the next event in sequence order.
   *
   * @return The next owned event, or no value when the queue is empty.
   */
  [[nodiscard]] std::optional<DiscoveryDelivery> pop();

  /** @brief Read the current epoch. */
  [[nodiscard]] std::uint64_t epoch() const;

  /** @brief Read the latest assigned event sequence. */
  [[nodiscard]] std::uint64_t sequence() const;

  /** @brief Read the authoritative admission state. */
  [[nodiscard]] DiscoveryDeliveryState state() const;

  /** @brief Whether an overflow remains latched across stop. */
  [[nodiscard]] bool overflowed() const;

private:
  [[nodiscard]] DiscoveryResult overflow();
  [[nodiscard]] DiscoveryResult admit();

  std::size_t m_max_data;
  std::size_t m_max_control;
  std::uint64_t m_epoch{1};
  std::uint64_t m_sequence{0};
  DiscoveryDeliveryState m_state{DiscoveryDeliveryState::kRunning};
  bool m_overflowed{false};
  std::deque<DiscoveryDelivery> m_data;
  std::deque<DiscoveryDelivery> m_control;
  std::optional<DiscoveryDelivery> m_emergency;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_DELIVERY_HPP_
