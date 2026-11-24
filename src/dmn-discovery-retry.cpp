/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-retry.cpp
 * @brief Monotonic, bounded retry scheduling for discovery operations.
 */

#include "dmn-discovery-retry.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace dmn::detail {

DiscoveryRetrySchedule::DiscoveryRetrySchedule(std::size_t maxAttempts)
    : m_max_attempts{maxAttempts} {}

DiscoveryResult DiscoveryRetrySchedule::fail(DiscoveryAttemptId id,
                                             bool recoverable,
                                             std::string message,
                                             TimePoint now) {
  if (id.m_source == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid retry source"}};
  }

  auto entry{m_entries.find(id)};
  if (entry == m_entries.end()) {
    if (m_entries.size() >= m_max_attempts) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "retry operation limit reached"}};
    }

    entry =
        m_entries
            .emplace(id,
                     Entry{DiscoveryAttemptError{recoverable, message}, {}, 1})
            .first;
  }

  auto &value{entry->second};
  value.m_error = DiscoveryAttemptError{recoverable, std::move(message)};
  if (!recoverable) {
    value.m_due.reset();
  } else if (!value.m_due) {
    const auto delay{std::chrono::seconds{value.m_next_delay_seconds}};
    value.m_due =
        now > TimePoint::max() - delay ? TimePoint::max() : now + delay;
    value.m_next_delay_seconds = std::min(value.m_next_delay_seconds * 2, 30u);
  }

  return {};
}

bool DiscoveryRetrySchedule::succeed(DiscoveryAttemptId id) {
  return m_entries.erase(id) != 0;
}

void DiscoveryRetrySchedule::cancelTimer(DiscoveryAttemptId id) {
  const auto entry{m_entries.find(id)};
  if (entry != m_entries.end()) {
    entry->second.m_due.reset();
  }
}

void DiscoveryRetrySchedule::removeSource(std::uint64_t source) {
  std::erase_if(m_entries, [source](const auto &entry) {
    return entry.first.m_source == source;
  });
}

std::vector<DiscoveryAttemptId> DiscoveryRetrySchedule::takeDue(TimePoint now) {
  std::vector<DiscoveryAttemptId> result{};
  for (auto &[id, entry] : m_entries) {
    if (entry.m_due && *entry.m_due <= now) {
      result.push_back(id);
      entry.m_due.reset();
    }
  }

  return result;
}

std::optional<DiscoveryAttemptError>
DiscoveryRetrySchedule::error(DiscoveryAttemptId id) const {
  const auto entry{m_entries.find(id)};
  if (entry == m_entries.end()) {
    return std::nullopt;
  }

  return entry->second.m_error;
}

} // namespace dmn::detail
