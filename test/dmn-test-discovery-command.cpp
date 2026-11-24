/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-command.cpp
 * @brief Bounded command admission and reserved-control tests.
 */

#include "dmn-discovery-command.hpp"

#include <gtest/gtest.h>

#include <future>
#include <mutex>
#include <vector>

namespace {

using namespace dmn::detail;

TEST(DmnDiscoveryCommandTest, FullOrdinaryQueueDoesNotStarveControl) {
  DiscoveryCommandWorker worker{1, 1};
  std::promise<void> started{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  std::mutex mutex{};
  std::vector<int> order{};
  const auto running{worker.submit([&]() -> DiscoveryResult {
    started.set_value();
    released.wait();
    return {};
  })};
  ASSERT_TRUE(running);
  started.get_future().wait();
  const auto ordinary{worker.submit([&]() -> DiscoveryResult {
    std::lock_guard lock{mutex};
    order.push_back(2);
    return {};
  })};
  ASSERT_TRUE(ordinary);
  EXPECT_FALSE(worker.submit([]() -> DiscoveryResult { return {}; }));
  const auto control{worker.submitControl(1, DiscoveryCommandControl::kReset,
                                          [&]() -> DiscoveryResult {
                                            std::lock_guard lock{mutex};
                                            order.push_back(1);
                                            return {};
                                          })};
  ASSERT_TRUE(control);
  release.set_value();
  EXPECT_TRUE(running->get());
  EXPECT_TRUE(control->get());
  EXPECT_TRUE(ordinary->get());
  EXPECT_EQ(order, (std::vector{1, 2}));
  ASSERT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, DuplicateControlsShareOneBoundedOperation) {
  DiscoveryCommandWorker worker{1, 1};
  std::promise<void> started{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  int executions{0};
  const auto first{worker.submitControl(1, DiscoveryCommandControl::kReset,
                                        [&]() -> DiscoveryResult {
                                          started.set_value();
                                          released.wait();
                                          ++executions;
                                          return {};
                                        })};
  ASSERT_TRUE(first);
  started.get_future().wait();
  const auto duplicate{
      worker.submitControl(1, DiscoveryCommandControl::kReset,
                           []() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(duplicate);
  const auto stop{worker.submitControl(1, DiscoveryCommandControl::kStop,
                                       []() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(stop);
  EXPECT_FALSE(worker.submitControl(2, DiscoveryCommandControl::kReset,
                                    []() -> DiscoveryResult { return {}; }));
  release.set_value();
  EXPECT_TRUE(first->get());
  EXPECT_TRUE(duplicate->get());
  EXPECT_TRUE(stop->get());
  EXPECT_EQ(executions, 1);
  ASSERT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, ExceptionsAreReportedAndWorkerContinues) {
  DiscoveryCommandWorker worker{2, 1};
  const auto failure{worker.submit(
      []() -> DiscoveryResult { throw std::runtime_error{"failed command"}; })};
  ASSERT_TRUE(failure);
  EXPECT_FALSE(failure->get());
  const auto success{worker.submit([]() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(success);
  EXPECT_TRUE(success->get());
  ASSERT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, StopCancelsQueuedResetWithoutDroppingCompletion) {
  DiscoveryCommandWorker worker{1, 1};
  std::promise<void> started{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  const auto running{worker.submit([&]() -> DiscoveryResult {
    started.set_value();
    released.wait();
    return {};
  })};
  ASSERT_TRUE(running);
  started.get_future().wait();
  const auto reset{
      worker.submitControl(1, DiscoveryCommandControl::kReset,
                           []() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(reset);
  const auto stop{worker.submitControl(1, DiscoveryCommandControl::kStop,
                                       []() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(stop);
  EXPECT_FALSE(reset->get());
  release.set_value();
  EXPECT_TRUE(running->get());
  EXPECT_TRUE(stop->get());
  ASSERT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, ShutdownCancelsQueuedWorkAndDrainsActiveWork) {
  DiscoveryCommandWorker worker{1, 1};
  std::promise<void> started{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  const auto running{worker.submit([&]() -> DiscoveryResult {
    started.set_value();
    released.wait();
    return {};
  })};
  ASSERT_TRUE(running);
  started.get_future().wait();
  const auto queued{worker.submit([]() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(queued);
  auto shutdown{
      std::async(std::launch::async, [&] { return worker.shutdown(); })};
  EXPECT_FALSE(queued->get());
  release.set_value();
  EXPECT_TRUE(running->get());
  EXPECT_TRUE(shutdown.get());
  EXPECT_FALSE(worker.submit([]() -> DiscoveryResult { return {}; }));
}

TEST(DmnDiscoveryCommandTest, NativeCallbackExecutesDirectlyOnStateWorker) {
  DiscoveryCommandWorker worker{1, 1};
  std::promise<void> entered{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  int calls{0};
  auto running{worker.submit([&]() -> DiscoveryResult {
    entered.set_value();
    released.wait();
    return worker.execute([&]() -> DiscoveryResult {
      ++calls;
      return {};
    });
  })};
  ASSERT_TRUE(running);
  entered.get_future().wait();
  auto queued{worker.submit([]() -> DiscoveryResult { return {}; })};
  ASSERT_TRUE(queued);
  release.set_value();
  EXPECT_TRUE(running->get());
  EXPECT_TRUE(queued->get());
  EXPECT_EQ(calls, 1);
  ASSERT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, DirectCommandExceptionIsExplicit) {
  DiscoveryCommandWorker worker{1, 1};
  auto running{worker.submit([&]() -> DiscoveryResult {
    return worker.execute([]() -> DiscoveryResult {
      throw std::runtime_error{"native failure"};
    });
  })};
  ASSERT_TRUE(running);
  EXPECT_FALSE(running->get());
  ASSERT_TRUE(worker.shutdown());
}

} // namespace
