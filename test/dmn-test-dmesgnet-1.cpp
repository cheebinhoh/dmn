/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-1.cpp
 * @brief Verify DMesgNet heartbeat lifecycle using bounded event-driven reads.
 *
 * A single node uses in-process pipes for its input and output. The test
 * observes a MasterPending heartbeat before Ready, checks that the node names
 * itself as master, and then observes a Destroyed heartbeat after teardown.
 *
 * Reads wait for one frame at a time instead of using fixed sleeps. The test
 * still depends on the real heartbeat timer and bounds both the wait per
 * frame and the number of frames examined; it does not exercise peer behavior.
 */

#include "dmn-dmesgnet.hpp"
#include "dmn-pipe.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

TEST(DmnDmesgNetLifecycleTest, PublishesReadyAndDestroyedHeartbeats) {
  auto input{std::make_shared<dmn::Dmn_Pipe<std::string>>("input")};
  auto output{std::make_shared<dmn::Dmn_Pipe<std::string>>("output")};
  auto node{std::make_unique<dmn::Dmn_DMesgNet>("dmesg1", input, output)};

  // Observe startup through self-election without feeding output back as input.
  bool pending{};
  bool ready{};
  for (int attempt{}; attempt < 10 && !ready; ++attempt) {
    const auto frames{output->read(1, 5000000L)};
    ASSERT_FALSE(frames.empty()) << "Timed out waiting for a heartbeat";

    dmn::DMesgPb message;
    ASSERT_TRUE(message.ParseFromString(frames.front()));
    ASSERT_EQ(message.type(), dmn::DMesgTypePb::sys);
    const auto &self{message.body().sys().self()};
    if (self.state() == dmn::DMesgStatePb::MasterPending) {
      pending = true;
    } else if (self.state() == dmn::DMesgStatePb::Ready) {
      EXPECT_TRUE(pending);
      EXPECT_EQ(self.masteridentifier(), "dmesg1");
      ready = true;
    }
  }
  ASSERT_TRUE(ready);

  // Earlier heartbeats may remain buffered when destruction sends the final
  // Destroyed heartbeat, so continue reading until it appears.
  node.reset();
  bool destroyed{};
  for (int attempt{}; attempt < 10 && !destroyed; ++attempt) {
    const auto frames{output->read(1, 5000000L)};
    ASSERT_FALSE(frames.empty()) << "Timed out waiting for shutdown heartbeat";

    dmn::DMesgPb message;
    ASSERT_TRUE(message.ParseFromString(frames.front()));
    ASSERT_EQ(message.type(), dmn::DMesgTypePb::sys);
    destroyed =
        message.body().sys().self().state() == dmn::DMesgStatePb::Destroyed;
  }
  EXPECT_TRUE(destroyed);
}
