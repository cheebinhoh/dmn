/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-command.cpp
 * @brief Bounded command admission and reserved-control tests.
 */

#include "dmn-discovery-command.hpp"
#include "dmn-discovery-session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace {

using namespace dmn::detail;

TEST(DmnDiscoveryCommandTest, PendingCancellationDoesNotInterruptCleanup) {
  DiscoveryCommandWorker worker{4, 1};
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult {
    EXPECT_EQ(pthread_cancel(pthread_self()), 0);
    return {};
  }));
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult { return {}; }));
  EXPECT_TRUE(worker.shutdown());
}

class TestPoller final : public DiscoveryCommandPoller {
public:
  DiscoveryResult start() override {
    if (m_on_start) {
      return m_on_start();
    }
    return {};
  }

  DiscoveryResult wait(bool block) override {
    std::unique_lock lock{m_mutex};
    if (block) {
      m_changed.wait(lock, [&] { return m_woken || m_fail; });
    } else {
      ++m_nonblocking_polls;
    }
    m_woken = false;
    if (m_fail) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kBackend, "poll failed"}};
    }
    return {};
  }

  void wake() noexcept override {
    std::lock_guard lock{m_mutex};
    m_woken = true;
    m_changed.notify_one();
  }

  DiscoveryResult stop() override {
    if (m_on_stop) {
      m_on_stop();
    }
    m_stopped.set_value();
    if (m_fail_stop) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kBackend, "cleanup failed"}};
    }
    return {};
  }

  void fail() {
    std::lock_guard lock{m_mutex};
    m_fail = true;
    m_changed.notify_one();
  }

  std::function<DiscoveryResult()> m_on_start;
  std::function<void()> m_on_stop;
  std::promise<void> m_stopped;
  std::atomic<unsigned> m_nonblocking_polls{0};
  bool m_fail_stop{false};

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_woken{false};
  bool m_fail{false};
};

TEST(DmnDiscoveryCommandTest, NativePollRunsAndStopsOnStateWorker) {
  DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  bool immediateCallback{false};
  bool stoppedOnWorker{false};
  poller->m_on_start = [&]() -> DiscoveryResult {
    EXPECT_TRUE(worker.inWorkerContext());
    return worker.execute([&]() -> DiscoveryResult {
      immediateCallback = true;
      return {};
    });
  };
  poller->m_on_stop = [&] { stoppedOnWorker = worker.inWorkerContext(); };
  ASSERT_TRUE(worker.attachPoller(poller));
  EXPECT_TRUE(immediateCallback);
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult { return {}; }));
  auto stopped{poller->m_stopped.get_future()};
  EXPECT_TRUE(worker.shutdown());
  EXPECT_TRUE(stoppedOnWorker);
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds{1}),
            std::future_status::ready);
}

TEST(DmnDiscoveryCommandTest, ClientAttachesPollerBeforeCreatingSessions) {
  DiscoverySessionClient client{{}};
  auto poller{std::make_shared<TestPoller>()};
  bool immediateCallback{false};
  poller->m_on_start = [&]() -> DiscoveryResult {
    return client.dispatchNative([&]() -> DiscoveryResult {
      immediateCallback = true;
      return {};
    });
  };
  ASSERT_TRUE(client.attachPoller(poller));
  EXPECT_TRUE(immediateCallback);
  EXPECT_TRUE(client.browse(nullptr));
  auto stopped{poller->m_stopped.get_future()};
  EXPECT_TRUE(client.shutdown());
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds{1}),
            std::future_status::ready);
  EXPECT_FALSE(client.attachPoller(std::make_shared<TestPoller>()));
}

TEST(DmnDiscoveryCommandTest, NativePollStartupFailureRollsBack) {
  DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  poller->m_on_start = []() -> DiscoveryResult {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "startup failed"}};
  };
  auto stopped{poller->m_stopped.get_future()};
  const auto result{worker.attachPoller(poller)};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_message, "startup failed");
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds{1}),
            std::future_status::ready);
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult { return {}; }));
  EXPECT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, NativePollStartupExceptionRollsBack) {
  DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  poller->m_on_start = []() -> DiscoveryResult {
    throw std::runtime_error{"startup threw"};
  };
  auto stopped{poller->m_stopped.get_future()};
  EXPECT_FALSE(worker.attachPoller(poller));
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds{1}),
            std::future_status::ready);
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult { return {}; }));
  EXPECT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, NativePollReportsStartupAndCleanupFailures) {
  DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  poller->m_fail_stop = true;
  poller->m_on_start = []() -> DiscoveryResult {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "startup failed"}};
  };
  const auto result{worker.attachPoller(poller)};
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().m_message.find("startup failed"), std::string::npos);
  EXPECT_NE(result.error().m_message.find("cleanup failed"), std::string::npos);
  EXPECT_TRUE(worker.shutdown());
}

TEST(DmnDiscoveryCommandTest, NativePollFailureCancelsWorker) {
  DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  ASSERT_TRUE(worker.attachPoller(poller));
  auto stopped{poller->m_stopped.get_future()};
  poller->fail();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  const auto result{worker.shutdown()};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_message, "poll failed");
  EXPECT_FALSE(worker.submit([]() -> DiscoveryResult { return {}; }));
}

TEST(DmnDiscoveryCommandTest, FailedWorkerJoinReportsCleanupSeparately) {
  for (const bool cleanupFails : {false, true}) {
    DiscoveryCommandWorker worker{4, 1};
    auto poller{std::make_shared<TestPoller>()};
    poller->m_fail_stop = cleanupFails;
    ASSERT_TRUE(worker.attachPoller(poller));
    const auto running{worker.joinAfterPollFailure()};
    ASSERT_TRUE(running);
    EXPECT_FALSE(*running);
    auto stopped{poller->m_stopped.get_future()};
    poller->fail();
    ASSERT_EQ(stopped.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    const auto joined{worker.joinAfterPollFailure()};
    if (cleanupFails) {
      ASSERT_FALSE(joined);
      EXPECT_EQ(joined.error().m_message, "cleanup failed");
    } else {
      ASSERT_TRUE(joined);
      EXPECT_TRUE(*joined);
    }
    const auto shutdown{worker.shutdown()};
    ASSERT_FALSE(shutdown);
    EXPECT_NE(shutdown.error().m_message.find("poll failed"),
              std::string::npos);
  }
}

TEST(DmnDiscoveryCommandTest, BusyCommandsDoNotStarveNativePoll) {
  DiscoveryCommandWorker worker{64, 1};
  auto poller{std::make_shared<TestPoller>()};
  ASSERT_TRUE(worker.attachPoller(poller));
  std::promise<void> entered{};
  std::promise<void> release{};
  auto released{release.get_future().share()};
  auto running{worker.submit([&]() -> DiscoveryResult {
    entered.set_value();
    released.wait();
    return {};
  })};
  ASSERT_TRUE(running);
  entered.get_future().wait();
  std::vector<DiscoveryCommandWorker::Completion> queued{};
  for (int i{0}; i < 32; ++i) {
    auto submitted{worker.submit([]() -> DiscoveryResult { return {}; })};
    ASSERT_TRUE(submitted);
    queued.push_back(*submitted);
  }
  release.set_value();
  ASSERT_TRUE(running->get());
  for (auto &completion : queued) {
    ASSERT_TRUE(completion.get());
  }
  EXPECT_GT(poller->m_nonblocking_polls.load(), 0U);
  EXPECT_TRUE(worker.shutdown());
}

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

TEST(DmnDiscoveryCommandTest, AllocationFailureIsReportedAsResourceLimit) {
  DiscoveryCommandWorker worker{2, 1};
  auto failure{
      worker.execute([]() -> DiscoveryResult { throw std::bad_alloc{}; })};
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  auto nested{worker.execute([&]() -> DiscoveryResult {
    return worker.execute([]() -> DiscoveryResult { throw std::bad_alloc{}; });
  })};
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_TRUE(worker.execute([]() -> DiscoveryResult { return {}; }));
  EXPECT_TRUE(worker.shutdown());
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
