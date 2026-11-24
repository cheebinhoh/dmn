/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-delivery.cpp
 * @brief Deterministic bounded discovery event-delivery tests.
 */

#include "dmn-discovery-delivery.hpp"

#include <gtest/gtest.h>

namespace {

using dmn::detail::DiscoveryCandidate;
using dmn::detail::DiscoveryControl;
using dmn::detail::DiscoveryDelivery;
using dmn::detail::DiscoveryDeliveryQueue;
using dmn::detail::DiscoveryDeliveryState;
using dmn::detail::DiscoveryEvent;
using dmn::detail::DiscoveryEventKind;

DiscoveryEvent added() {
  return DiscoveryEvent{
      DiscoveryEventKind::kAdded, 0, 0,
      DiscoveryCandidate{{"Printer", "_http._tcp", "local.", 2}}};
}

TEST(DmnDiscoveryDeliveryTest, MergesDataAndControlInSequenceOrder) {
  DiscoveryDeliveryQueue queue{2, 2};
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_TRUE(queue.pushControl(DiscoveryControl::kBackendUnavailable));
  ASSERT_TRUE(queue.pushData(added()));
  for (std::uint64_t sequence{1}; sequence <= 3; ++sequence) {
    const auto event{queue.pop()};
    ASSERT_TRUE(event);
    EXPECT_EQ(event->m_sequence, sequence);
    EXPECT_EQ(event->m_epoch, 1);
  }
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, OverflowDiscardsDeltasAndLatchesError) {
  DiscoveryDeliveryQueue queue{1, 2};
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_FALSE(queue.pushData(added()));
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kOverflow);
  EXPECT_EQ(queue.epoch(), 2);
  EXPECT_FALSE(queue.pushControl(DiscoveryControl::kBackendAvailable));
  const auto overflow{queue.pop()};
  ASSERT_TRUE(overflow);
  EXPECT_EQ(overflow->m_control, DiscoveryControl::kOverflow);
  EXPECT_EQ(overflow->m_epoch, 2);
  EXPECT_FALSE(queue.pop());
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kOverflow);
}

TEST(DmnDiscoveryDeliveryTest, ResetDropsQueuedEventsAndAllowsRestart) {
  DiscoveryDeliveryQueue queue{1, 1};
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_FALSE(queue.pushData(added()));
  ASSERT_TRUE(queue.reset());
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kRunning);
  const auto reset{queue.pop()};
  ASSERT_TRUE(reset);
  EXPECT_EQ(reset->m_control, DiscoveryControl::kReset);
  EXPECT_EQ(reset->m_epoch, 3);
  ASSERT_TRUE(queue.pushData(added()));
  const auto after{queue.pop()};
  ASSERT_TRUE(after);
  EXPECT_EQ(after->m_epoch, reset->m_epoch);
  EXPECT_GT(after->m_sequence, reset->m_sequence);
}

TEST(DmnDiscoveryDeliveryTest, FullControlQueueUsesEmergencyOverflowSlot) {
  DiscoveryDeliveryQueue queue{2, 1};
  ASSERT_TRUE(queue.pushControl(DiscoveryControl::kBackendUnavailable));
  ASSERT_FALSE(queue.pushControl(DiscoveryControl::kReset));
  const auto overflow{queue.pop()};
  ASSERT_TRUE(overflow);
  EXPECT_EQ(overflow->m_control, DiscoveryControl::kOverflow);
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, StopSuppressesQueuedAndEmergencyEvents) {
  DiscoveryDeliveryQueue queue{1, 1};
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_FALSE(queue.pushData(added()));
  queue.stop();
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kStopped);
  EXPECT_FALSE(queue.pop());
  EXPECT_FALSE(queue.reset());
}

TEST(DmnDiscoveryDeliveryTest, StopInvalidatesEpochExactlyOnce) {
  DiscoveryDeliveryQueue queue{1, 1};
  const auto epoch{queue.epoch()};
  queue.stop();
  EXPECT_GT(queue.epoch(), epoch);
  const auto stoppedEpoch{queue.epoch()};
  queue.stop();
  EXPECT_EQ(queue.epoch(), stoppedEpoch);
}

TEST(DmnDiscoveryDeliveryTest, BackendLossEmitsUnavailableThenReset) {
  DiscoveryDeliveryQueue queue{2, 2};
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_TRUE(queue.backendLost());
  const auto unavailable{queue.pop()};
  const auto reset{queue.pop()};
  ASSERT_TRUE(unavailable);
  ASSERT_TRUE(reset);
  EXPECT_EQ(unavailable->m_control, DiscoveryControl::kBackendUnavailable);
  EXPECT_EQ(reset->m_control, DiscoveryControl::kReset);
  EXPECT_EQ(unavailable->m_epoch, 2);
  EXPECT_EQ(reset->m_epoch, 2);
  EXPECT_LT(unavailable->m_sequence, reset->m_sequence);
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, BackendLossOverflowsIfControlsCannotFit) {
  DiscoveryDeliveryQueue queue{2, 1};
  EXPECT_FALSE(queue.backendLost());
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kOverflow);
  const auto event{queue.pop()};
  ASSERT_TRUE(event);
  EXPECT_EQ(event->m_control, DiscoveryControl::kOverflow);
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, ResetPreservesRequiredPendingControl) {
  DiscoveryDeliveryQueue queue{2, 2};
  ASSERT_TRUE(queue.pushControl(DiscoveryControl::kBackendUnavailable));
  ASSERT_TRUE(queue.pushData(added()));
  ASSERT_TRUE(queue.reset());
  const auto unavailable{queue.pop()};
  const auto reset{queue.pop()};
  ASSERT_TRUE(unavailable);
  ASSERT_TRUE(reset);
  EXPECT_EQ(unavailable->m_control, DiscoveryControl::kBackendUnavailable);
  EXPECT_EQ(reset->m_control, DiscoveryControl::kReset);
  EXPECT_LT(unavailable->m_sequence, reset->m_sequence);
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, ResetOverflowsRatherThanDroppingControl) {
  DiscoveryDeliveryQueue queue{2, 1};
  ASSERT_TRUE(queue.pushControl(DiscoveryControl::kBackendUnavailable));
  EXPECT_FALSE(queue.reset());
  EXPECT_EQ(queue.state(), DiscoveryDeliveryState::kOverflow);
  const auto overflow{queue.pop()};
  ASSERT_TRUE(overflow);
  EXPECT_EQ(overflow->m_control, DiscoveryControl::kOverflow);
  EXPECT_FALSE(queue.pop());
}

TEST(DmnDiscoveryDeliveryTest, LossPreservesPreviouslyQueuedControl) {
  DiscoveryDeliveryQueue queue{2, 3};
  ASSERT_TRUE(queue.pushControl(DiscoveryControl::kBackendAvailable));
  ASSERT_TRUE(queue.backendLost());
  const auto available{queue.pop()};
  const auto unavailable{queue.pop()};
  const auto reset{queue.pop()};
  ASSERT_TRUE(available);
  ASSERT_TRUE(unavailable);
  ASSERT_TRUE(reset);
  EXPECT_EQ(available->m_control, DiscoveryControl::kBackendAvailable);
  EXPECT_EQ(unavailable->m_control, DiscoveryControl::kBackendUnavailable);
  EXPECT_EQ(reset->m_control, DiscoveryControl::kReset);
}

} // namespace
