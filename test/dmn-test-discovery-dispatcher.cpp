/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-dispatcher.cpp
 * @brief Multi-session callback ownership and lifetime tests.
 */

#include "dmn-discovery-command.hpp"
#include "dmn-discovery-dispatcher.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace dmn::detail;

DiscoveryEvent added() {
  return DiscoveryEvent{
      DiscoveryEventKind::kAdded, 0, 0,
      DiscoveryCandidate{{"Printer", "_http._tcp", "local.", 2}}};
}

TEST(DmnDiscoveryDispatcherTest, PendingCancellationDoesNotInterruptDrain) {
  DiscoveryDispatcher dispatcher{1};
  std::promise<void> delivered;
  unsigned count{0};
  auto handle{dispatcher.create(2, 1, [&](DiscoveryDelivery) {
    if (++count == 1) {
      EXPECT_EQ(pthread_cancel(pthread_self()), 0);
    } else {
      delivered.set_value();
    }
  })};
  ASSERT_TRUE(handle);
  const auto epoch{handle->snapshot().m_epoch};
  ASSERT_TRUE(handle->postData(epoch, added()));
  ASSERT_TRUE(handle->postData(epoch, added()));
  EXPECT_EQ(delivered.get_future().wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_TRUE(handle->stop());
  EXPECT_TRUE(dispatcher.shutdown());
}

TEST(DmnDiscoveryDispatcherTest, FailureOfOneSessionDoesNotStopAnother) {
  DiscoveryDispatcher dispatcher{2};
  std::promise<void> failingEntered{};
  std::promise<void> otherDelivered{};
  auto failing{dispatcher.create(2, 2, [&](DiscoveryDelivery) {
    failingEntered.set_value();
    throw std::runtime_error{"bad callback"};
  })};
  auto other{dispatcher.create(
      2, 2, [&](DiscoveryDelivery) { otherDelivered.set_value(); })};
  ASSERT_TRUE(failing);
  ASSERT_TRUE(other);
  ASSERT_TRUE(failing->postData(failing->snapshot().m_epoch, added()));
  failingEntered.get_future().wait();
  ASSERT_TRUE(other->postData(other->snapshot().m_epoch, added()));
  otherDelivered.get_future().wait();
  ASSERT_TRUE(failing->stop());
  EXPECT_TRUE(failing->snapshot().m_callback_failed);
  ASSERT_TRUE(other->stop());
  ASSERT_TRUE(dispatcher.shutdown());
}

TEST(DmnDiscoveryDispatcherTest, HandleOutlivesShutdownAndCannotRestart) {
  DiscoveryDeliveryHandle handle{};
  {
    DiscoveryDispatcher dispatcher{1};
    auto created{dispatcher.create(1, 1, {})};
    ASSERT_TRUE(created);
    handle = *created;
    ASSERT_TRUE(dispatcher.shutdown());
  }
  EXPECT_EQ(handle.snapshot().m_state, DiscoveryDeliveryState::kStopped);
  EXPECT_FALSE(handle.postData(handle.snapshot().m_epoch, added()));
  EXPECT_FALSE(handle.reset());
  EXPECT_TRUE(handle.stop());
}

TEST(DmnDiscoveryDispatcherTest, StopReleasesHandlerWhileHandleSurvives) {
  DiscoveryDispatcher dispatcher{1};
  auto owned{std::make_shared<int>(42)};
  std::weak_ptr<int> weak{owned};
  auto handle{dispatcher.create(1, 1, [owned](DiscoveryDelivery) {})};
  ASSERT_TRUE(handle);
  owned.reset();
  EXPECT_FALSE(weak.expired());
  ASSERT_TRUE(handle->stop());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(handle->snapshot().m_state, DiscoveryDeliveryState::kStopped);
}

TEST(DmnDiscoveryDispatcherTest, ShutdownReleasesHandlersOfRetainedHandles) {
  DiscoveryDispatcher dispatcher{1};
  auto owned{std::make_shared<int>(42)};
  std::weak_ptr<int> weak{owned};
  auto handle{dispatcher.create(1, 1, [owned](DiscoveryDelivery) {})};
  ASSERT_TRUE(handle);
  owned.reset();
  ASSERT_TRUE(dispatcher.shutdown());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(handle->snapshot().m_state, DiscoveryDeliveryState::kStopped);
  ASSERT_TRUE(handle->stop());
}

TEST(DmnDiscoveryDispatcherTest, HandlerCleanupRejectsReentrantStop) {
  struct Cleanup {
    std::function<void()> m_on_destroy;
    ~Cleanup() { m_on_destroy(); }
  };
  DiscoveryDispatcher dispatcher{1};
  DiscoveryDeliveryHandle handle{};
  bool rejected{false};
  auto owned{std::make_shared<Cleanup>([&] { rejected = !handle.stop(); })};
  auto created{dispatcher.create(1, 1, [owned](DiscoveryDelivery) {})};
  ASSERT_TRUE(created);
  handle = *created;
  owned.reset();
  ASSERT_TRUE(handle.stop());
  EXPECT_TRUE(rejected);
  ASSERT_TRUE(dispatcher.shutdown());
}

TEST(DmnDiscoveryDispatcherTest, SessionLimitIsExplicitAndReusable) {
  DiscoveryDispatcher dispatcher{1};
  auto first{dispatcher.create(1, 1, {})};
  ASSERT_TRUE(first);
  EXPECT_FALSE(dispatcher.create(1, 1, {}));
  ASSERT_TRUE(first->stop());
  EXPECT_TRUE(dispatcher.create(1, 1, {}));
  ASSERT_TRUE(dispatcher.shutdown());
}

TEST(DmnDiscoveryDispatcherTest, ResetDrainsCallbacksAndRejectsOldEpoch) {
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  bool snapshotRead{false};
  std::vector<DiscoveryDelivery> events{};
  DiscoveryDeliveryHandle handle{};
  DiscoveryDispatcher dispatcher{1};
  auto created{dispatcher.create(4, 4, [&](DiscoveryDelivery event) {
    std::unique_lock lock{mutex};
    events.push_back(event);
    if (event.m_data) {
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
      snapshotRead = handle.snapshot().m_epoch > event.m_epoch;
    }
  })};
  ASSERT_TRUE(created);
  handle = *created;
  const auto epoch{handle.snapshot().m_epoch};
  ASSERT_TRUE(handle.postData(epoch, added()));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  ASSERT_TRUE(handle.postData(epoch, added()));
  auto reset{std::async(std::launch::async, [&] { return handle.reset(); })};
  while (handle.snapshot().m_epoch == epoch) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  ASSERT_TRUE(reset.get());
  EXPECT_FALSE(handle.postData(epoch, added()));
  ASSERT_TRUE(handle.stop());
  std::lock_guard lock{mutex};
  EXPECT_TRUE(snapshotRead);
  EXPECT_LE(events.size(), 2);
}

TEST(DmnDiscoveryDispatcherTest, CallbackMaySnapshotButNotCallBarriers) {
  std::promise<bool> done{};
  DiscoveryDeliveryHandle handle{};
  DiscoveryDispatcher dispatcher{1};
  auto created{dispatcher.create(2, 2, [&](DiscoveryDelivery) {
    const auto snapshot{handle.snapshot()};
    const auto reset{handle.reset()};
    const auto stop{handle.stop()};
    const auto shutdown{dispatcher.shutdown()};
    done.set_value(snapshot.m_epoch == 1 && !reset && !stop && !shutdown);
  })};
  ASSERT_TRUE(created);
  handle = *created;
  ASSERT_TRUE(handle.postData(handle.snapshot().m_epoch, added()));
  EXPECT_TRUE(done.get_future().get());
  ASSERT_TRUE(handle.stop());
  ASSERT_TRUE(dispatcher.shutdown());
}

TEST(DmnDiscoveryDispatcherTest, OverflowNotificationUsesReservedSlot) {
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<DiscoveryDelivery> notification{};
  DiscoveryDispatcher dispatcher{1};
  auto handle{dispatcher.create(1, 1, [&](DiscoveryDelivery event) {
    if (event.m_data) {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    } else if (event.m_control == DiscoveryControl::kOverflow) {
      notification.set_value(event);
    }
  })};
  ASSERT_TRUE(handle);
  const auto epoch{handle->snapshot().m_epoch};
  ASSERT_TRUE(handle->postData(epoch, added()));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  ASSERT_TRUE(handle->postData(epoch, added()));
  EXPECT_FALSE(handle->postData(epoch, added()));
  EXPECT_EQ(handle->snapshot().m_state, DiscoveryDeliveryState::kOverflow);
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_EQ(notification.get_future().get().m_control,
            DiscoveryControl::kOverflow);
  ASSERT_TRUE(handle->reset());
  ASSERT_TRUE(handle->stop());
}

TEST(DmnDiscoveryDispatcherTest, LossDropsQueuedDataAndOrdersControlEvents) {
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::vector<DiscoveryControl> controls{};
  std::promise<void> resetDelivered{};
  DiscoveryDispatcher dispatcher{1};
  auto handle{dispatcher.create(2, 2, [&](DiscoveryDelivery event) {
    if (event.m_data) {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    } else {
      controls.push_back(*event.m_control);
      if (*event.m_control == DiscoveryControl::kReset) {
        resetDelivered.set_value();
      }
    }
  })};
  ASSERT_TRUE(handle);
  const auto epoch{handle->snapshot().m_epoch};
  ASSERT_TRUE(handle->postData(epoch, added()));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  ASSERT_TRUE(handle->postData(epoch, added()));
  ASSERT_TRUE(handle->backendLost(epoch));
  EXPECT_FALSE(handle->postData(epoch, added()));
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  resetDelivered.get_future().wait();
  ASSERT_TRUE(handle->stop());
  EXPECT_EQ(controls, (std::vector{DiscoveryControl::kBackendUnavailable,
                                   DiscoveryControl::kReset}));
}

TEST(DmnDiscoveryDispatcherTest, SplitResetDoesNotBlockStateWorkerForCallback) {
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool proceed{false};
  DiscoveryCommandWorker commands{2, 1};
  DiscoveryDispatcher dispatcher{1};
  auto handle{dispatcher.create(2, 2, [&](DiscoveryDelivery event) {
    if (!event.m_data) {
      return;
    }

    {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return proceed; });
    }
    const auto snapshotCommand{
        commands.submit([]() -> DiscoveryResult { return {}; })};
    EXPECT_TRUE(snapshotCommand);
    if (snapshotCommand) {
      EXPECT_TRUE(snapshotCommand->get());
    }
  })};
  ASSERT_TRUE(handle);
  const auto oldEpoch{handle->snapshot().m_epoch};
  ASSERT_TRUE(handle->postData(oldEpoch, added()));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto begin{commands.submitControl(1, DiscoveryCommandControl::kReset,
                                    [&]() -> DiscoveryResult {
                                      const auto epoch{handle->beginReset()};
                                      if (!epoch) {
                                        return std::unexpected{epoch.error()};
                                      }
                                      return {};
                                    })};
  ASSERT_TRUE(begin);
  ASSERT_TRUE(begin->get());
  const auto newEpoch{handle->snapshot().m_epoch};
  auto drain{std::async(std::launch::async,
                        [&] { return handle->awaitReset(newEpoch); })};
  {
    std::lock_guard lock{mutex};
    proceed = true;
  }
  changed.notify_all();
  EXPECT_TRUE(drain.get());
  ASSERT_TRUE(handle->stop());
  ASSERT_TRUE(commands.shutdown());
}

} // namespace
