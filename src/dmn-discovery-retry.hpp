/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-retry.hpp
 * @brief Private, caller-driven discovery retry scheduler.
 *
 * The scheduler owns no threads or native handles. Its caller supplies
 * monotonic time and starts the operations returned by takeDue(). Errors
 * remain visible while a retry is in flight, until that operation succeeds.
 */

#ifndef DMN_DISCOVERY_RETRY_HPP_
#define DMN_DISCOVERY_RETRY_HPP_

#include "dmn-discovery-core.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dmn::detail {

/**
 * @brief Bounded retry state driven by a caller-provided monotonic clock.
 *
 * At most one timer is outstanding for each admitted operation. Dispatching a
 * due timer leaves the error visible but does not reschedule: the owner must
 * report the resulting success or failure. Removing the source cancels all
 * its timers. The owner must serialize calls.
 */
class DiscoveryRetrySchedule {
public:
  using TimePoint = std::chrono::steady_clock::time_point;

  /**
   * @brief Construct with a bound on concurrently tracked failed operations.
   *
   * @param maxAttempts Maximum number of distinct tracked operations.
   */
  explicit DiscoveryRetrySchedule(std::size_t maxAttempts);

  /**
   * @brief Record a failure and schedule a retry when recoverable.
   *
   * Repeated reports while a timer is already pending do not postpone it.
   * Delays double from one second to a maximum of thirty seconds.
   *
   * @param id Live contribution and operation identity.
   * @param recoverable Whether automatic retry is permitted.
   * @param message Owned, non-sensitive failure detail.
   * @param now Current monotonic time.
   * @return Success or a typed invalid-input/resource error.
   */
  [[nodiscard]] DiscoveryResult fail(DiscoveryAttemptId id, bool recoverable,
                                     std::string message, TimePoint now);

  /**
   * @brief Clear the failure and retry state for a successful operation.
   *
   * @param id Live operation identity.
   * @return Whether a failure was present.
   */
  [[nodiscard]] bool succeed(DiscoveryAttemptId id);

  /**
   * @brief Cancel a pending timer while preserving the visible failure.
   *
   * @param id Operation whose lookup is being superseded.
   */
  void cancelTimer(DiscoveryAttemptId id);

  /**
   * @brief Remove all state belonging to a withdrawn contribution.
   *
   * @param source Live contribution generation.
   */
  void removeSource(std::uint64_t source);

  /**
   * @brief Collect due operations once, leaving their errors visible.
   *
   * @param now Current monotonic time.
   * @return Owned identifiers of operations ready to restart.
   */
  [[nodiscard]] std::vector<DiscoveryAttemptId> takeDue(TimePoint now);

  /**
   * @brief Copy the currently visible error for one operation.
   *
   * @param id Operation identity.
   * @return The last failure, or no value after success/removal.
   */
  [[nodiscard]] std::optional<DiscoveryAttemptError>
  error(DiscoveryAttemptId id) const;

private:
  struct Entry {
    DiscoveryAttemptError m_error;
    std::optional<TimePoint> m_due;
    unsigned int m_next_delay_seconds{1};
  };

  std::size_t m_max_attempts;
  std::map<DiscoveryAttemptId, Entry> m_entries;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_RETRY_HPP_
