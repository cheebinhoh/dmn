/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-slots.cpp
 * @brief Native-operation quota and owned lease tests.
 */

#include "dmn-discovery-slots.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace {

using dmn::detail::DiscoveryNativeSlots;

TEST(DmnDiscoverySlotsTest, SlotIsHeldUntilItsNativeOwnerReleasesIt) {
  DiscoveryNativeSlots slots{1};
  auto lease{slots.reserve()};
  ASSERT_TRUE(lease);
  EXPECT_EQ(slots.inUse(), 1);
  EXPECT_FALSE(slots.reserve());

  auto moved{std::move(*lease)};
  EXPECT_EQ(slots.inUse(), 1);
  moved.release();
  moved.release();
  EXPECT_EQ(slots.inUse(), 0);
  EXPECT_TRUE(slots.reserve());
}

TEST(DmnDiscoverySlotsTest, LeasesCanOutliveTheirAllocator) {
  DiscoveryNativeSlots::Lease lease{};
  {
    DiscoveryNativeSlots slots{1};
    auto reserved{slots.reserve()};
    ASSERT_TRUE(reserved);
    lease = std::move(*reserved);
  }
  lease.release();
}

TEST(DmnDiscoverySlotsTest, MoveAssignmentReleasesPreviousLease) {
  DiscoveryNativeSlots slots{2};
  auto first{slots.reserve()};
  auto second{slots.reserve()};
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  *first = std::move(*second);
  EXPECT_EQ(slots.inUse(), 1);
  first->release();
  EXPECT_EQ(slots.inUse(), 0);
}

} // namespace
