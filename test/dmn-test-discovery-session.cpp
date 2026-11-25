/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-session.cpp
 * @brief Browser, state-worker and callback-dispatch integration tests.
 */

#include "dmn-discovery-records.hpp"
#include "dmn-discovery-session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>

namespace {

using namespace dmn::detail;

TEST(DmnDiscoverySessionTest, InvalidOptionsRejectWorkBeforeWorkerStartup) {
  DiscoveryOptions options{};
  options.m_max_instances = 0;
  DiscoverySessionClient client{options};
  EXPECT_FALSE(client.dispatchNative([]() -> DiscoveryResult { return {}; }));
  EXPECT_FALSE(client.browse({}));
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, ResetRebuildsViewAndRejectsLateObservations) {
  std::promise<DiscoveryDelivery> added{};
  DiscoverySessionClient client{{}};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_data) {
      added.set_value(event);
    }
  })};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  const auto event{added.get_future().get()};
  EXPECT_EQ(event.m_data->m_candidate.m_key.m_instance, "Printer");
  EXPECT_EQ(session->snapshot().m_browser.m_candidates.size(), 1);
  ASSERT_TRUE(session->clearCache());
  const auto reset{session->snapshot()};
  EXPECT_GT(reset.m_delivery.m_epoch, epoch);
  EXPECT_TRUE(reset.m_browser.m_candidates.empty());
  EXPECT_FALSE(session->observe(epoch, key, DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, NativeLifecycleRunsUnlockedOnStateWorker) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle;
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  std::uint64_t priorEpoch{0};
  std::size_t resets{0};
  std::size_t cancellations{0};
  DiscoverySessionClient::BrowserLifecycle lifecycle{
      [&] {
        ++cancellations;
        EXPECT_EQ(handle.snapshot().m_delivery.m_state,
                  DiscoveryDeliveryState::kStopped);
        return DiscoveryResult{};
      },
      [&]() -> DiscoveryResult {
        ++resets;
        const auto snapshot{handle.snapshot()};
        EXPECT_GT(snapshot.m_delivery.m_epoch, priorEpoch);
        auto source{handle.observe(snapshot.m_delivery.m_epoch, key,
                                   DiscoveryContribution::kIpv4)};
        if (!source) {
          return std::unexpected{source.error()};
        }
        return {};
      }};
  auto session{client.browse({}, std::move(lifecycle))};
  ASSERT_TRUE(session);
  handle = *session;
  priorEpoch = handle.snapshot().m_delivery.m_epoch;
  ASSERT_TRUE(handle.clearCache());
  EXPECT_EQ(resets, 1);
  EXPECT_EQ(handle.snapshot().m_browser.m_candidates.size(), 1);
  ASSERT_TRUE(handle.stop());
  ASSERT_TRUE(handle.stop());
  EXPECT_EQ(cancellations, 1);
  ASSERT_TRUE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, NativeCleanupFailureStillDrainsCallback) {
  DiscoverySessionClient client{{}};
  std::promise<void> entered;
  std::promise<void> release;
  auto released{release.get_future().share()};
  std::promise<void> cancelled;
  auto session{client.browse(
      [&](DiscoveryDelivery event) {
        if (event.m_data) {
          entered.set_value();
          released.wait();
        }
      },
      {[&]() -> DiscoveryResult {
         cancelled.set_value();
         return std::unexpected{
             DiscoveryError{DiscoveryErrorKind::kBackend, "cleanup failed"}};
       },
       {}})};
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->observe(session->snapshot().m_delivery.m_epoch,
                               {"Printer", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  entered.get_future().wait();
  auto stopping{
      std::async(std::launch::async, [&] { return session->stop(); })};
  cancelled.get_future().wait();
  const bool returnedEarly{stopping.wait_for(std::chrono::milliseconds{20}) ==
                           std::future_status::ready};
  release.set_value();
  EXPECT_FALSE(returnedEarly);
  auto stopped{stopping.get()};
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().m_message, "cleanup failed");
  ASSERT_TRUE(session->snapshot().m_cleanup_error);
  EXPECT_EQ(session->snapshot().m_cleanup_error->m_message, "cleanup failed");
  EXPECT_FALSE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, NativeCleanupReleasesCapturesOutsideSessionLock) {
  struct Cleanup {
    std::function<void()> m_on_destroy;
    ~Cleanup() { m_on_destroy(); }
  };
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle;
  bool destroyed{false};
  auto owned{std::make_shared<Cleanup>([&] {
    static_cast<void>(handle.snapshot());
    destroyed = true;
  })};
  auto session{client.browse({}, {[owned] { return DiscoveryResult{}; }, {}})};
  ASSERT_TRUE(session);
  handle = *session;
  owned.reset();
  ASSERT_TRUE(handle.stop());
  EXPECT_TRUE(destroyed);
  ASSERT_TRUE(client.shutdown());
}

#if defined(__GLIBCXX__)
TEST(DmnDiscoverySessionTest, CancelledStopCallerDoesNotStrandBarrier) {
  DiscoverySessionClient client{{}};
  std::promise<void> entered;
  std::promise<void> release;
  auto released{release.get_future().share()};
  std::promise<void> cancelled;
  auto session{client.browse(
      [&](DiscoveryDelivery event) {
        if (event.m_data) {
          entered.set_value();
          released.wait();
        }
      },
      {[&]() -> DiscoveryResult {
         cancelled.set_value();
         return {};
       },
       {}})};
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->observe(session->snapshot().m_delivery.m_epoch,
                               {"Printer", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  entered.get_future().wait();

  pthread_t thread{};
  ASSERT_EQ(pthread_create(
                &thread, nullptr,
                [](void *arg) -> void * {
                  auto &handle{*static_cast<DiscoverySessionHandle *>(arg)};
                  static_cast<void>(handle.stop());
                  return nullptr;
                },
                &*session),
            0);
  cancelled.get_future().wait();
  EXPECT_EQ(pthread_cancel(thread), 0);
  void *result{};
  EXPECT_EQ(pthread_join(thread, &result), 0);
  EXPECT_EQ(result, PTHREAD_CANCELED);
  release.set_value();
  EXPECT_TRUE(session->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, CancelledResetLeaderReleasesJoinedCaller) {
  DiscoverySessionClient client{{}};
  std::promise<void> entered;
  std::promise<void> release;
  auto released{release.get_future().share()};
  std::promise<void> resetStarted;
  std::atomic<bool> firstReset{true};
  auto session{client.browse(
      [&](DiscoveryDelivery event) {
        if (event.m_data) {
          entered.set_value();
          released.wait();
        }
      },
      {{}, [&]() -> DiscoveryResult {
         if (firstReset.exchange(false)) {
           resetStarted.set_value();
         }
         return {};
       }})};
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->observe(session->snapshot().m_delivery.m_epoch,
                               {"Printer", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  entered.get_future().wait();
  pthread_t thread{};
  ASSERT_EQ(pthread_create(
                &thread, nullptr,
                [](void *arg) -> void * {
                  auto &handle{*static_cast<DiscoverySessionHandle *>(arg)};
                  static_cast<void>(handle.clearCache());
                  return nullptr;
                },
                &*session),
            0);
  resetStarted.get_future().wait();
  auto joined{
      std::async(std::launch::async, [&] { return session->clearCache(); })};
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{2}};
  while (session->snapshot().m_joined_reset_callers == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool joinedReset{session->snapshot().m_joined_reset_callers != 0};
  EXPECT_EQ(pthread_cancel(thread), 0);
  void *result{};
  EXPECT_EQ(pthread_join(thread, &result), 0);
  EXPECT_EQ(result, PTHREAD_CANCELED);
  const auto joinedResult{joined.get()};
  release.set_value();
  EXPECT_TRUE(joinedReset);
  ASSERT_FALSE(joinedResult);
  EXPECT_EQ(joinedResult.error().m_kind, DiscoveryErrorKind::kCancelled);
  EXPECT_TRUE(session->clearCache());
  EXPECT_TRUE(session->stop());
  EXPECT_TRUE(client.shutdown());
}
#endif

TEST(DmnDiscoverySessionTest, HandleSurvivesClientShutdown) {
  DiscoverySessionHandle handle{};
  {
    DiscoverySessionClient client{{}};
    auto session{client.browse({})};
    ASSERT_TRUE(session);
    handle = *session;
    ASSERT_TRUE(client.shutdown());
  }
  EXPECT_EQ(handle.snapshot().m_delivery.m_state,
            DiscoveryDeliveryState::kStopped);
  const auto refresh{handle.refresh({"Printer", "_http._tcp", "local.", 2})};
  ASSERT_FALSE(refresh);
  EXPECT_EQ(refresh.error().m_kind, DiscoveryErrorKind::kStopped);
  EXPECT_TRUE(handle.stop());
}

TEST(DmnDiscoverySessionTest, CallbackCanReadAndRefreshButNotWaitForBarriers) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle{};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  std::promise<bool> done{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_data && event.m_data->m_kind == DiscoveryEventKind::kAdded) {
      const auto snapshot{handle.snapshot()};
      const auto refresh{handle.refresh(key)};
      const auto reset{handle.clearCache()};
      const auto stop{handle.stop()};
      const auto shutdown{client.shutdown()};
      done.set_value(
          snapshot.m_browser.m_candidates.size() == 1 && refresh && !reset &&
          reset.error().m_kind == DiscoveryErrorKind::kReentrantCall && !stop &&
          stop.error().m_kind == DiscoveryErrorKind::kReentrantCall &&
          !shutdown &&
          shutdown.error().m_kind == DiscoveryErrorKind::kReentrantCall);
    }
  })};
  ASSERT_TRUE(session);
  handle = *session;
  ASSERT_TRUE(handle.observe(handle.snapshot().m_delivery.m_epoch, key,
                             DiscoveryContribution::kIpv4));
  EXPECT_TRUE(done.get_future().get());
  ASSERT_TRUE(handle.stop());
  ASSERT_TRUE(client.shutdown());
}

TEST(DmnDiscoverySessionTest, ResetDrainsOldCallbackWithoutBlockingSnapshot) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle{};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<bool> observed{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (!event.m_data) {
      return;
    }
    {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    observed.set_value(handle.snapshot().m_delivery.m_epoch > event.m_epoch);
  })};
  ASSERT_TRUE(session);
  handle = *session;
  const auto epoch{handle.snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(handle.observe(epoch, {"Printer", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto reset{
      std::async(std::launch::async, [&] { return handle.clearCache(); })};
  while (handle.snapshot().m_delivery.m_epoch == epoch) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_TRUE(observed.get_future().get());
  ASSERT_TRUE(reset.get());
  ASSERT_TRUE(handle.stop());
}

TEST(DmnDiscoverySessionTest, ConcurrentResetsShareOneBarrier) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle{};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (!event.m_data) {
      return;
    }
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  })};
  ASSERT_TRUE(session);
  handle = *session;
  const auto epoch{handle.snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(handle.observe(epoch, {"Printer", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto first{
      std::async(std::launch::async, [&] { return handle.clearCache(); })};
  while (handle.snapshot().m_delivery.m_epoch == epoch) {
    std::this_thread::yield();
  }
  const auto resetEpoch{handle.snapshot().m_delivery.m_epoch};
  auto second{
      std::async(std::launch::async, [&] { return handle.clearCache(); })};
  while (handle.snapshot().m_joined_reset_callers == 0) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  ASSERT_TRUE(first.get());
  ASSERT_TRUE(second.get());
  EXPECT_EQ(handle.snapshot().m_delivery.m_epoch, resetEpoch);
  ASSERT_TRUE(handle.stop());
}

TEST(DmnDiscoverySessionTest, InstanceOverflowInvalidatesViewUntilReset) {
  DiscoveryOptions options{};
  options.m_max_instances = 1;
  DiscoverySessionClient client{options};
  std::promise<void> overflow{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_control == DiscoveryControl::kOverflow) {
      overflow.set_value();
    }
  })};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->observe(epoch, {"First", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  EXPECT_FALSE(session->observe(epoch, {"Second", "_http._tcp", "local.", 2},
                                DiscoveryContribution::kIpv4));
  overflow.get_future().wait();
  auto snapshot{session->snapshot()};
  EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_GT(snapshot.m_delivery.m_epoch, epoch);
  EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
  ASSERT_TRUE(session->clearCache());
  EXPECT_TRUE(session->observe(session->snapshot().m_delivery.m_epoch,
                               {"Second", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, CallbackFailureIsIsolatedFromOtherSessions) {
  DiscoverySessionClient client{{}};
  std::promise<void> failingEntered{};
  std::promise<void> delivered{};
  auto failing{client.browse([&](DiscoveryDelivery) {
    failingEntered.set_value();
    throw std::runtime_error{"callback failed"};
  })};
  auto other{client.browse([&](DiscoveryDelivery) { delivered.set_value(); })};
  ASSERT_TRUE(failing);
  ASSERT_TRUE(other);
  ASSERT_TRUE(failing->observe(failing->snapshot().m_delivery.m_epoch,
                               {"First", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  failingEntered.get_future().wait();
  ASSERT_TRUE(other->observe(other->snapshot().m_delivery.m_epoch,
                             {"Second", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  delivered.get_future().wait();
  EXPECT_TRUE(failing->snapshot().m_delivery.m_callback_failed);
  EXPECT_TRUE(failing->snapshot().m_browser.m_candidates.empty());
  EXPECT_EQ(failing->snapshot().m_native_operations, 0);
  ASSERT_TRUE(failing->stop());
  ASSERT_TRUE(other->stop());
}

TEST(DmnDiscoverySessionTest, ConversionFailureRetainsItsError) {
  DiscoverySessionClient client{{}};
  std::promise<void> failingEntered;
  std::promise<void> delivered;
  auto failing{client.browse([&](DiscoveryDelivery) {
    failingEntered.set_value();
    throw DiscoveryDeliveryFailure{
        {DiscoveryErrorKind::kResourceLimit, "conversion failed"}};
  })};
  auto other{client.browse([&](DiscoveryDelivery) { delivered.set_value(); })};
  ASSERT_TRUE(failing);
  ASSERT_TRUE(other);
  ASSERT_TRUE(failing->observe(failing->snapshot().m_delivery.m_epoch,
                               {"First", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  failingEntered.get_future().wait();
  ASSERT_TRUE(other->observe(other->snapshot().m_delivery.m_epoch,
                             {"Other", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  delivered.get_future().wait();
  const auto snapshot{failing->snapshot()};
  ASSERT_TRUE(snapshot.m_cleanup_error);
  EXPECT_EQ(snapshot.m_cleanup_error->m_kind,
            DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(snapshot.m_cleanup_error->m_message, "conversion failed");
  EXPECT_TRUE(snapshot.m_delivery.m_callback_failed);
  EXPECT_TRUE(failing->stop());
  EXPECT_TRUE(other->stop());
}

TEST(DmnDiscoverySessionTest, BackendLossResetsViewAndRecoveryUsesFreshEpoch) {
  DiscoverySessionClient client{{}};
  std::mutex mutex{};
  std::vector<DiscoveryDelivery> events{};
  std::promise<void> recovered{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    std::lock_guard lock{mutex};
    events.push_back(event);
    if (event.m_control == DiscoveryControl::kBackendAvailable) {
      recovered.set_value();
    }
  })};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto oldEpoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->observe(oldEpoch, key, DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->backendLost(oldEpoch));
  auto lost{session->snapshot()};
  EXPECT_FALSE(lost.m_backend_available);
  EXPECT_GT(lost.m_delivery.m_epoch, oldEpoch);
  EXPECT_TRUE(lost.m_browser.m_candidates.empty());
  EXPECT_FALSE(session->observe(oldEpoch, key, DiscoveryContribution::kIpv4));
  EXPECT_FALSE(session->observe(lost.m_delivery.m_epoch, key,
                                DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->clearCache());
  const auto unavailableEpoch{session->snapshot().m_delivery.m_epoch};
  EXPECT_FALSE(session->snapshot().m_backend_available);
  ASSERT_TRUE(session->backendAvailable(unavailableEpoch));
  recovered.get_future().wait();
  const auto restored{session->snapshot()};
  EXPECT_TRUE(restored.m_backend_available);
  EXPECT_GT(restored.m_delivery.m_epoch, unavailableEpoch);
  ASSERT_TRUE(session->observe(restored.m_delivery.m_epoch, key,
                               DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->stop());
  std::lock_guard lock{mutex};
  for (const auto &event : events) {
    if (event.m_data) {
      EXPECT_NE(event.m_data->m_kind, DiscoveryEventKind::kRemoved);
    }
  }
}

TEST(DmnDiscoverySessionTest, BackendLossControlsRemainOrderedOnRecovery) {
  DiscoverySessionClient client{{}};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::vector<DiscoveryControl> controls{};
  std::promise<void> available{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_data) {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    } else {
      controls.push_back(*event.m_control);
      if (*event.m_control == DiscoveryControl::kBackendAvailable) {
        available.set_value();
      }
    }
  })};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->observe(epoch, {"Printer", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  ASSERT_TRUE(session->backendLost(epoch));
  ASSERT_TRUE(
      session->backendAvailable(session->snapshot().m_delivery.m_epoch));
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  available.get_future().wait();
  ASSERT_TRUE(session->stop());
  EXPECT_EQ(controls, (std::vector{DiscoveryControl::kBackendUnavailable,
                                   DiscoveryControl::kReset,
                                   DiscoveryControl::kBackendAvailable}));
}

TEST(DmnDiscoverySessionTest, InitiallyUnavailableCanRecoverWithoutLoss) {
  DiscoverySessionClient client{{}, false};
  std::promise<DiscoveryControl> initial{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_control == DiscoveryControl::kBackendUnavailable) {
      initial.set_value(*event.m_control);
    }
  })};
  ASSERT_TRUE(session);
  EXPECT_EQ(initial.get_future().get(), DiscoveryControl::kBackendUnavailable);
  const auto absent{session->snapshot()};
  EXPECT_FALSE(absent.m_backend_available);
  EXPECT_FALSE(session->observe(absent.m_delivery.m_epoch,
                                {"Printer", "_http._tcp", "local.", 2},
                                DiscoveryContribution::kIpv4));
  ASSERT_TRUE(session->backendAvailable(absent.m_delivery.m_epoch));
  EXPECT_TRUE(session->snapshot().m_backend_available);
  EXPECT_FALSE(session->backendLost(absent.m_delivery.m_epoch));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, StopDrainsCallbackWithoutBlockingItsSnapshot) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle{};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<bool> observed{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (!event.m_data) {
      return;
    }
    {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    observed.set_value(handle.snapshot().m_delivery.m_state ==
                       DiscoveryDeliveryState::kStopped);
  })};
  ASSERT_TRUE(session);
  handle = *session;
  ASSERT_TRUE(handle.observe(handle.snapshot().m_delivery.m_epoch,
                             {"Printer", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto stopped{std::async(std::launch::async, [&] { return handle.stop(); })};
  while (handle.snapshot().m_delivery.m_state !=
         DiscoveryDeliveryState::kStopped) {
    std::this_thread::yield();
  }
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_TRUE(observed.get_future().get());
  ASSERT_TRUE(stopped.get());
  EXPECT_TRUE(handle.stop());
}

TEST(DmnDiscoverySessionTest, ConcurrentResetStopAndShutdownDrainCallback) {
  DiscoverySessionClient client{{}};
  DiscoverySessionHandle handle{};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<void> callbackDone{};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (!event.m_data) {
      return;
    }
    {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    static_cast<void>(handle.snapshot());
    static_cast<void>(handle.refresh(key));
    callbackDone.set_value();
  })};
  ASSERT_TRUE(session);
  handle = *session;
  const auto epoch{handle.snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(handle.observe(epoch, key, DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto reset{
      std::async(std::launch::async, [&] { return handle.clearCache(); })};
  while (handle.snapshot().m_delivery.m_epoch == epoch) {
    std::this_thread::yield();
  }
  auto stopped{std::async(std::launch::async, [&] { return handle.stop(); })};
  while (!handle.snapshot().m_stop_pending) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(handle.observe(handle.snapshot().m_delivery.m_epoch, key,
                              DiscoveryContribution::kIpv4));
  auto shutdown{
      std::async(std::launch::async, [&] { return client.shutdown(); })};
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  callbackDone.get_future().wait();
  EXPECT_TRUE(reset.get());
  EXPECT_TRUE(stopped.get());
  EXPECT_TRUE(shutdown.get());
  EXPECT_EQ(handle.snapshot().m_delivery.m_state,
            DiscoveryDeliveryState::kStopped);
}

TEST(DmnDiscoverySessionTest, RepeatedBackendLossIsIdempotent) {
  DiscoverySessionClient client{{}};
  std::promise<void> initialReset{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_control == DiscoveryControl::kReset) {
      initialReset.set_value();
    }
  })};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->backendLost(epoch));
  initialReset.get_future().wait();
  const auto lost{session->snapshot()};
  ASSERT_TRUE(session->backendLost(lost.m_delivery.m_epoch));
  const auto repeated{session->snapshot()};
  EXPECT_EQ(repeated.m_delivery.m_epoch, lost.m_delivery.m_epoch);
  EXPECT_EQ(repeated.m_delivery.m_sequence, lost.m_delivery.m_sequence);
  EXPECT_FALSE(repeated.m_backend_available);
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, NativeQuotaOverflowCancelsOwnedOperations) {
  DiscoveryOptions options{};
  options.m_max_native_operations = 2;
  DiscoverySessionClient client{options};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  EXPECT_EQ(session->snapshot().m_native_operations, 1);
  auto operation{session->beginNative(epoch)};
  ASSERT_TRUE(operation);
  EXPECT_EQ(session->snapshot().m_native_operations, 2);
  EXPECT_FALSE(session->beginNative(epoch));
  auto overflow{session->snapshot()};
  EXPECT_EQ(overflow.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_EQ(overflow.m_native_operations, 0);
  EXPECT_FALSE(session->finishNative(epoch, *operation));
  ASSERT_TRUE(session->clearCache());
  EXPECT_EQ(session->snapshot().m_native_operations, 1);
  ASSERT_TRUE(session->stop());
  EXPECT_EQ(session->snapshot().m_native_operations, 0);
}

TEST(DmnDiscoverySessionTest, BackendLossCancelsAllFakeNativeReferences) {
  DiscoverySessionClient client{{}};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  auto operation{session->beginNative(epoch)};
  ASSERT_TRUE(operation);
  ASSERT_TRUE(session->backendLost(epoch));
  EXPECT_EQ(session->snapshot().m_native_operations, 0);
  EXPECT_FALSE(session->finishNative(epoch, *operation));
  ASSERT_TRUE(
      session->backendAvailable(session->snapshot().m_delivery.m_epoch));
  EXPECT_EQ(session->snapshot().m_native_operations, 1);
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, NativeCompletionAndResetReleaseOnlyOwnedSlots) {
  DiscoverySessionClient client{{}};
  auto first{client.browse({})};
  auto second{client.browse({})};
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const auto epoch{first->snapshot().m_delivery.m_epoch};
  auto finished{first->beginNative(epoch)};
  auto pending{first->beginNative(epoch)};
  ASSERT_TRUE(finished);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(first->finishNative(epoch, *finished));
  EXPECT_FALSE(first->finishNative(epoch, *finished));
  EXPECT_EQ(first->snapshot().m_native_operations, 2);
  ASSERT_TRUE(first->clearCache());
  EXPECT_FALSE(first->finishNative(epoch, *pending));
  EXPECT_EQ(first->snapshot().m_native_operations, 1);
  EXPECT_EQ(second->snapshot().m_native_operations, 1);
  ASSERT_TRUE(first->stop());
  EXPECT_EQ(second->snapshot().m_native_operations, 1);
  ASSERT_TRUE(second->stop());
}

TEST(DmnDiscoverySessionTest, DefaultLimitsSupportDualTransportAndFamilies) {
  DiscoverySessionClient client{{}};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  for (int instance{0}; instance < 64; ++instance) {
    const DiscoveryServiceKey key{"Service" + std::to_string(instance),
                                  "_http._tcp", "local.", 2};
    for (const auto contribution :
         {DiscoveryContribution::kIpv4, DiscoveryContribution::kIpv6}) {
      const auto source{session->observe(epoch, key, contribution)};
      ASSERT_TRUE(source);
      for (int operation{0}; operation < 4; ++operation) {
        ASSERT_TRUE(session->beginNative(epoch));
      }
      ASSERT_TRUE(session->updateRecords(
          epoch, key, contribution, *source,
          [contribution](DiscoveryRecords &records,
                         std::uint64_t token) -> DiscoveryResult {
            if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv,
                                       true) ||
                !records.setMonitoring(token, DiscoveryRecordKind::kTxt,
                                       true)) {
              return std::unexpected{DiscoveryError{
                  DiscoveryErrorKind::kBackend, "missing source"}};
            }
            auto srv{records.addSrv(
                token,
                *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
                {0, 0, 42, "host.local."})};
            if (!srv) {
              return srv;
            }
            const auto suffix{static_cast<std::uint8_t>(
                contribution == DiscoveryContribution::kIpv4 ? 1 : 2)};
            auto ipv4{records.addAddress(
                token, records.snapshot().m_generation,
                *records.addressGeneration(token,
                                           DiscoveryAddressFamily::kIpv4),
                "host.local.", 42,
                {DiscoveryAddressFamily::kIpv4, {10, 0, 0, suffix}, {}})};
            if (!ipv4) {
              return ipv4;
            }
            return records.addAddress(
                token, records.snapshot().m_generation,
                *records.addressGeneration(token,
                                           DiscoveryAddressFamily::kIpv6),
                "host.local.", 42,
                {DiscoveryAddressFamily::kIpv6,
                 {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, suffix},
                 {}});
          }));
    }
  }
  const auto snapshot{session->snapshot()};
  ASSERT_EQ(snapshot.m_browser.m_candidates.size(), 64);
  EXPECT_EQ(snapshot.m_native_operations, 513);
  for (const auto &candidate : snapshot.m_browser.m_candidates) {
    EXPECT_EQ(candidate.m_records.m_state, DiscoveryRecordState::kReady);
    EXPECT_EQ(candidate.m_records.m_addresses.size(), 4);
  }
  ASSERT_TRUE(session->stop());
  EXPECT_EQ(session->snapshot().m_native_operations, 0);
}

TEST(DmnDiscoverySessionTest, NativeCallbackMayMutateSessionOnWorker) {
  DiscoverySessionClient client{{}};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    auto operation{session->beginNative(epoch)};
    if (!operation) {
      return std::unexpected{operation.error()};
    }
    auto observed{session->observe(epoch,
                                   {"Printer", "_http._tcp", "local.", 2},
                                   DiscoveryContribution::kIpv4)};
    if (!observed) {
      return std::unexpected{observed.error()};
    }
    if (session->clearCache() || session->stop() || client.shutdown()) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "worker barrier was accepted"}};
    }
    return session->finishNative(epoch, *operation);
  }));
  EXPECT_EQ(session->snapshot().m_browser.m_candidates.size(), 1);
  EXPECT_EQ(session->snapshot().m_native_operations, 1);
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, RemovesOnlyCurrentSourceWithoutRevivingIt) {
  DiscoverySessionClient client{{}};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto ipv4{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  const auto ipv6{session->observe(epoch, key, DiscoveryContribution::kIpv6)};
  ASSERT_TRUE(ipv4);
  ASSERT_TRUE(ipv6);
  ASSERT_TRUE(session->removeObservation(epoch, key,
                                         DiscoveryContribution::kIpv4, *ipv4));
  EXPECT_EQ(session->snapshot().m_browser.m_candidates.size(), 1);
  EXPECT_FALSE(session->removeObservation(epoch, key,
                                          DiscoveryContribution::kIpv4, *ipv4));
  ASSERT_TRUE(session->removeObservation(epoch, key,
                                         DiscoveryContribution::kIpv6, *ipv6));
  EXPECT_TRUE(session->snapshot().m_browser.m_candidates.empty());
  ASSERT_TRUE(session->clearCache());
  EXPECT_FALSE(session->removeObservation(epoch, key,
                                          DiscoveryContribution::kIpv6, *ipv6));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, RefreshDeadlineIsProcessedOnStateWorker) {
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoverySessionClient client{{}, true, [&] { return now; }};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  const auto refresh{session->refresh(key)};
  ASSERT_TRUE(refresh);
  now += std::chrono::seconds{5};
  ASSERT_TRUE(session->processTimers(epoch));
  const auto snapshot{session->snapshot()};
  ASSERT_EQ(snapshot.m_browser.m_candidates.size(), 1);
  ASSERT_TRUE(snapshot.m_browser.m_candidates.front().m_refresh);
  EXPECT_EQ(snapshot.m_browser.m_candidates.front().m_refresh->m_id, *refresh);
  EXPECT_EQ(snapshot.m_browser.m_candidates.front().m_refresh->m_state,
            DiscoveryRefreshState::kFailed);
  ASSERT_TRUE(session->clearCache());
  EXPECT_FALSE(session->processTimers(epoch));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, RetryTimersAreScopedToLiveBackendEpoch) {
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoverySessionClient client{{}, true, [&] { return now; }};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  std::uint64_t watch{0};
  ASSERT_TRUE(session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv, true)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
        }
        watch = *records.monitorGeneration(token, DiscoveryRecordKind::kSrv);
        return {};
      }));
  ASSERT_TRUE(session->failAttempt(epoch, key, DiscoveryContribution::kIpv4,
                                   *source, DiscoveryAttempt::kSrv, watch, 0,
                                   true, "unavailable"));
  now += std::chrono::seconds{1};
  const auto due{session->takeDueRetries(epoch)};
  ASSERT_TRUE(due);
  EXPECT_EQ(*due,
            (std::vector{DiscoveryAttemptId{*source, DiscoveryAttempt::kSrv}}));
  std::uint64_t newWatch{0};
  ASSERT_TRUE(session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv, true)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
        }
        newWatch = *records.monitorGeneration(token, DiscoveryRecordKind::kSrv);
        return {};
      }));
  EXPECT_NE(watch, newWatch);
  EXPECT_FALSE(session->succeedAttempt(epoch, key, DiscoveryContribution::kIpv4,
                                       *source, DiscoveryAttempt::kSrv, watch,
                                       0));
  EXPECT_TRUE(session->succeedAttempt(epoch, key, DiscoveryContribution::kIpv4,
                                      *source, DiscoveryAttempt::kSrv, newWatch,
                                      0));
  EXPECT_TRUE(
      session->snapshot().m_browser.m_candidates.front().m_failures.empty());
  ASSERT_TRUE(session->backendLost(epoch));
  EXPECT_FALSE(session->takeDueRetries(epoch));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, RecordLimitOverflowCancelsSessionWork) {
  DiscoveryOptions options{};
  options.m_max_txt_bytes = 3;
  DiscoverySessionClient client{options};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(session->beginNative(epoch));
  ASSERT_TRUE(session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.setMonitoring(token, DiscoveryRecordKind::kTxt, true)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
        }
        return {};
      }));
  const auto failed{session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) {
        return records.addTxt(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kTxt),
            {3, 'a', 'b', 'c'});
      })};
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  const auto snapshot{session->snapshot()};
  EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
  EXPECT_EQ(snapshot.m_native_operations, 0);
  ASSERT_TRUE(session->clearCache());
  EXPECT_FALSE(session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &, std::uint64_t) -> DiscoveryResult { return {}; }));
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, AddressLimitOverflowCancelsSessionWork) {
  DiscoveryOptions options{};
  options.m_max_addresses = 1;
  DiscoverySessionClient client{options};
  auto session{client.browse({})};
  ASSERT_TRUE(session);
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  const auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv, true) ||
            !records.setMonitoring(token, DiscoveryRecordKind::kTxt, true)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
        }
        auto srv{records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."})};
        if (!srv) {
          return srv;
        }
        return records.addAddress(
            token, records.snapshot().m_generation,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42,
            {DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}});
      }));
  const auto failed{session->updateRecords(
      epoch, key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) {
        return records.addAddress(
            token, records.snapshot().m_generation,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42,
            {DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}});
      })};
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  const auto snapshot{session->snapshot()};
  EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
  EXPECT_EQ(snapshot.m_native_operations, 0);
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, DataQueueOverflowCancelsSessionWork) {
  DiscoveryOptions options{};
  options.m_max_data_events = 1;
  DiscoverySessionClient client{options};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<void> overflowDelivered{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_data) {
      std::unique_lock lock{mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    } else if (event.m_control == DiscoveryControl::kOverflow) {
      overflowDelivered.set_value();
    }
  })};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->beginNative(epoch));
  ASSERT_TRUE(session->observe(epoch, {"First", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  ASSERT_TRUE(session->observe(epoch, {"Second", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  EXPECT_FALSE(session->observe(epoch, {"Third", "_http._tcp", "local.", 2},
                                DiscoveryContribution::kIpv4));
  const auto snapshot{session->snapshot()};
  EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_EQ(snapshot.m_native_operations, 0);
  EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  overflowDelivered.get_future().wait();
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, LossControlOverflowCancelsSessionWork) {
  DiscoveryOptions options{};
  options.m_max_control_events = 1;
  DiscoverySessionClient client{options};
  std::promise<void> overflowDelivered{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_control == DiscoveryControl::kOverflow) {
      overflowDelivered.set_value();
    }
  })};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->beginNative(epoch));
  EXPECT_FALSE(session->backendLost(epoch));
  overflowDelivered.get_future().wait();
  const auto snapshot{session->snapshot()};
  EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
  EXPECT_EQ(snapshot.m_native_operations, 0);
  EXPECT_FALSE(snapshot.m_backend_available);
  ASSERT_TRUE(session->stop());
}

TEST(DmnDiscoverySessionTest, OverflowRemainsVisibleIfItsCallbackThrows) {
  DiscoveryOptions options{};
  options.m_max_instances = 1;
  DiscoverySessionClient client{options};
  std::promise<void> overflowEntered{};
  std::promise<void> otherDelivered{};
  auto session{client.browse([&](DiscoveryDelivery event) {
    if (event.m_control == DiscoveryControl::kOverflow) {
      overflowEntered.set_value();
      throw std::runtime_error{"overflow callback failed"};
    }
  })};
  auto other{
      client.browse([&](DiscoveryDelivery) { otherDelivered.set_value(); })};
  ASSERT_TRUE(session);
  ASSERT_TRUE(other);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(session->observe(epoch, {"First", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  EXPECT_FALSE(session->observe(epoch, {"Second", "_http._tcp", "local.", 2},
                                DiscoveryContribution::kIpv4));
  overflowEntered.get_future().wait();
  ASSERT_TRUE(other->observe(other->snapshot().m_delivery.m_epoch,
                             {"Other", "_http._tcp", "local.", 2},
                             DiscoveryContribution::kIpv4));
  otherDelivered.get_future().wait();
  const auto snapshot{session->snapshot()};
  EXPECT_TRUE(snapshot.m_delivery.m_callback_failed);
  EXPECT_TRUE(snapshot.m_delivery.m_overflowed);
  EXPECT_EQ(snapshot.m_native_operations, 0);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE(other->stop());
}

} // namespace
