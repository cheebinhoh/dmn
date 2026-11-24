/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-retry.cpp
 * @brief Deterministic retry scheduling tests using caller-supplied time.
 */

#include "dmn-discovery-retry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono_literals;
using dmn::detail::DiscoveryAttempt;
using dmn::detail::DiscoveryAttemptId;
using dmn::detail::DiscoveryRetrySchedule;

TEST(DmnDiscoveryRetryTest, RetriesWithBoundedBackoffAndNoNewBrowse) {
  DiscoveryRetrySchedule retries{4};
  const DiscoveryAttemptId id{7, DiscoveryAttempt::kIpv4};
  const DiscoveryRetrySchedule::TimePoint start{};
  auto now{start};
  for (const auto delay : {1s, 2s, 4s, 8s, 16s, 30s, 30s}) {
    ASSERT_TRUE(retries.fail(id, true, "timeout", now));
    EXPECT_TRUE(retries.takeDue(now + delay - 1ms).empty());
    now += delay;
    EXPECT_EQ(retries.takeDue(now), (std::vector{id}));
    EXPECT_TRUE(retries.takeDue(now).empty());
    ASSERT_TRUE(retries.error(id));
  }
  ASSERT_TRUE(retries.succeed(id));
  EXPECT_FALSE(retries.error(id));
}

TEST(DmnDiscoveryRetryTest, PermanentFailureDoesNotRetryAndRemovalCancels) {
  DiscoveryRetrySchedule retries{2};
  const DiscoveryRetrySchedule::TimePoint now{};
  const DiscoveryAttemptId permanent{1, DiscoveryAttempt::kSrv};
  const DiscoveryAttemptId transient{1, DiscoveryAttempt::kTxt};
  ASSERT_TRUE(retries.fail(permanent, false, "denied", now));
  ASSERT_TRUE(retries.fail(transient, true, "unavailable", now));
  EXPECT_EQ(retries.takeDue(now + 31s), (std::vector{transient}));
  ASSERT_TRUE(retries.fail(transient, true, "unavailable", now + 31s));
  retries.removeSource(1);
  EXPECT_TRUE(retries.takeDue(now + 60s).empty());
  EXPECT_FALSE(retries.error(permanent));
}

TEST(DmnDiscoveryRetryTest, LimitsFailureStateAndKeepsOneTimerPerAttempt) {
  DiscoveryRetrySchedule retries{1};
  const DiscoveryRetrySchedule::TimePoint now{};
  const DiscoveryAttemptId first{1, DiscoveryAttempt::kIpv4};
  const DiscoveryAttemptId other{2, DiscoveryAttempt::kIpv4};
  ASSERT_TRUE(retries.fail(first, true, "timeout", now));
  ASSERT_TRUE(retries.fail(first, true, "timeout again", now));
  EXPECT_FALSE(retries.fail(other, true, "timeout", now));
  EXPECT_EQ(retries.takeDue(now + 1s), (std::vector{first}));
  EXPECT_TRUE(retries.takeDue(now + 10s).empty());
}

TEST(DmnDiscoveryRetryTest, CancellingTimerKeepsTheVisibleError) {
  DiscoveryRetrySchedule retries{2};
  const DiscoveryRetrySchedule::TimePoint now{};
  const DiscoveryAttemptId address{1, DiscoveryAttempt::kIpv4};
  const DiscoveryAttemptId watch{1, DiscoveryAttempt::kTxt};
  ASSERT_TRUE(retries.fail(address, true, "timeout", now));
  ASSERT_TRUE(retries.fail(watch, true, "disconnected", now));
  retries.cancelTimer(address);
  EXPECT_TRUE(retries.error(address));
  EXPECT_EQ(retries.takeDue(now + 1s), (std::vector{watch}));
}

} // namespace
