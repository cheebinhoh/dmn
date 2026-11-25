/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-discovery-worker-start-failure.cpp
 * @brief Verify discovery worker startup failures and rollback.
 */

#include "dmn-discovery-records.hpp"
#include "dmn-discovery-retry.hpp"
#include "dmn-discovery-session.hpp"

#include <fiu-control.h>
#include <fiu.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

constexpr auto failurePoint = "dmn/timer/pipe/proc/pthread_create";
std::atomic<int> attempts{};
std::promise<void> *callbackFailureReached{};
std::promise<void> *stateFailureReached{};
std::shared_future<void> stateFailureRelease;
std::promise<void> *shutdownFailureReached{};
std::shared_future<void> shutdownFailureRelease;

int failSecondStart(const char *, int *, void **, unsigned int *) {
  return ++attempts == 2;
}

int failCallbackWorker(const char *, int *, void **, unsigned int *) {
  callbackFailureReached->set_value();
  return 1;
}

int failStateWorker(const char *, int *, void **, unsigned int *) {
  stateFailureReached->set_value();
  stateFailureRelease.wait();
  return 1;
}

int failShutdown(const char *, int *, void **, unsigned int *) {
  shutdownFailureReached->set_value();
  shutdownFailureRelease.wait();
  return 1;
}

class TestPoller final : public dmn::detail::DiscoveryCommandPoller {
public:
  dmn::detail::DiscoveryResult start() override { return {}; }
  dmn::detail::DiscoveryResult wait(bool) override { return {}; }
  void wake() noexcept override {}
  dmn::detail::DiscoveryResult stop() override {
    stopped = true;
    return {};
  }
  std::atomic<bool> stopped{false};
};

TEST(DmnDiscoveryWorkerFaultInjection, FirstWorkerFailureCanBeRetried) {
  ASSERT_EQ(fiu_init(0), 0);
  ASSERT_EQ(fiu_disable(failurePoint), 0);
  ASSERT_EQ(fiu_enable(failurePoint, 1, nullptr, FIU_ONETIME), 0);

  dmn::detail::DiscoverySessionClient client{{}};
  EXPECT_FALSE(client.browse({}));
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection, PartialStartupRollsBackAndCanBeRetried) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  attempts = 0;
  ASSERT_EQ(fiu_enable_external(failurePoint, 1, nullptr, FIU_ONETIME,
                                failSecondStart),
            0);

  dmn::detail::DiscoverySessionClient client{{}};
  EXPECT_FALSE(client.browse({}));
  EXPECT_EQ(attempts.load(), 2);
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection,
     CancellationSetupFailuresRollBackAndCanBeRetried) {
  ASSERT_EQ(fiu_init(0), 0);
  for (const auto point : {"dmn/discovery/state-worker/cancel-setup",
                           "dmn/discovery/callback-worker/cancel-setup"}) {
    ASSERT_EQ(fiu_enable(point, 1, nullptr, FIU_ONETIME), 0);
    dmn::detail::DiscoverySessionClient client{{}};
    const auto failure{client.browse({})};
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().m_kind,
              dmn::detail::DiscoveryErrorKind::kBackend);
    auto browser{client.browse({})};
    ASSERT_TRUE(browser);
    EXPECT_TRUE(browser->stop());
    EXPECT_TRUE(client.shutdown());
  }
}

TEST(DmnDiscoveryWorkerFaultInjection, BrowserModelFailureDoesNotConsumeSlots) {
  ASSERT_EQ(fiu_init(0), 0);
  dmn::detail::DiscoveryOptions options;
  options.m_max_browsers = 1;
  options.m_max_registrations = 1;
  dmn::detail::DiscoverySessionClient client{options};
  constexpr auto point = "dmn/discovery/browser/model-allocation";
  ASSERT_EQ(fiu_enable(point, 1, nullptr, 0), 0);
  for (int i = 0; i != 3; ++i) {
    EXPECT_THROW(static_cast<void>(client.browse({})), std::bad_alloc);
  }
  ASSERT_EQ(fiu_disable(point), 0);
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection, RetryCollectionFailureKeepsAllTimers) {
  ASSERT_EQ(fiu_init(0), 0);
  using namespace std::chrono_literals;
  using namespace dmn::detail;
  DiscoveryRetrySchedule retries{2};
  const DiscoveryRetrySchedule::TimePoint now{};
  const DiscoveryAttemptId first{1, DiscoveryAttempt::kSrv};
  const DiscoveryAttemptId second{2, DiscoveryAttempt::kTxt};
  ASSERT_TRUE(retries.fail(first, true, "failed", now));
  ASSERT_TRUE(retries.fail(second, true, "failed", now));
  constexpr auto point = "dmn/discovery/retry/collect-allocation";
  ASSERT_EQ(fiu_enable(point, 1, nullptr, FIU_ONETIME), 0);
  EXPECT_THROW(static_cast<void>(retries.takeDue(now + 1s)), std::bad_alloc);
  EXPECT_EQ(retries.takeDue(now + 1s), (std::vector{first, second}));
}

TEST(DmnDiscoveryWorkerFaultInjection,
     BrowserEventAllocationFailureInvalidatesAndRecovers) {
  ASSERT_EQ(fiu_init(0), 0);
  using namespace dmn::detail;
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  constexpr auto point = "dmn/discovery/browser/event-allocation";
  for (const bool updateRecords : {false, true}) {
    std::promise<void> overflow;
    auto delivered{overflow.get_future()};
    DiscoverySessionClient client{{}};
    auto browser{client.browse([&](DiscoveryDelivery event) {
      if (event.m_control == DiscoveryControl::kOverflow) {
        overflow.set_value();
      }
    })};
    ASSERT_TRUE(browser);
    const auto epoch{browser->snapshot().m_delivery.m_epoch};
    ASSERT_TRUE(browser->beginNative(epoch));
    std::uint64_t generation{};
    if (updateRecords) {
      auto source{browser->observe(epoch, key, DiscoveryContribution::kIpv4)};
      ASSERT_TRUE(source);
      generation = *source;
      ASSERT_TRUE(browser->updateRecords(
          epoch, key, DiscoveryContribution::kIpv4, generation,
          [](DiscoveryRecords &records,
             std::uint64_t token) -> DiscoveryResult {
            if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv,
                                       true) ||
                !records.setMonitoring(token, DiscoveryRecordKind::kTxt,
                                       true)) {
              return std::unexpected{DiscoveryError{
                  DiscoveryErrorKind::kBackend, "missing source"}};
            }
            return {};
          }));
    }
    ASSERT_EQ(fiu_enable(point, 1, nullptr, FIU_ONETIME), 0);
    DiscoveryResult result;
    if (updateRecords) {
      result = browser->updateRecords(
          epoch, key, DiscoveryContribution::kIpv4, generation,
          [](DiscoveryRecords &records,
             std::uint64_t token) -> DiscoveryResult {
            return records.addTxt(
                token,
                *records.monitorGeneration(token, DiscoveryRecordKind::kTxt),
                {3, 'a', 'b', 'c'});
          });
    } else {
      auto observed{browser->observe(epoch, key, DiscoveryContribution::kIpv4)};
      ASSERT_FALSE(observed);
      result = std::unexpected{observed.error()};
    }
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
    const auto snapshot{browser->snapshot()};
    EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
    EXPECT_GT(snapshot.m_delivery.m_epoch, epoch);
    EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
    EXPECT_EQ(snapshot.m_native_operations, 0);
    ASSERT_EQ(delivered.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    ASSERT_TRUE(browser->clearCache());
    const auto resetEpoch{browser->snapshot().m_delivery.m_epoch};
    EXPECT_TRUE(
        browser->observe(resetEpoch, key, DiscoveryContribution::kIpv4));
    EXPECT_TRUE(browser->stop());
    EXPECT_TRUE(client.shutdown());
  }
}

TEST(DmnDiscoveryWorkerFaultInjection,
     LifecycleAllocationFailureInvalidatesAndRecovers) {
  ASSERT_EQ(fiu_init(0), 0);
  using namespace dmn::detail;
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  for (const bool reset : {false, true}) {
    std::promise<void> overflow;
    auto delivered{overflow.get_future()};
    DiscoverySessionClient client{{}};
    auto browser{client.browse([&](DiscoveryDelivery event) {
      if (event.m_control == DiscoveryControl::kOverflow) {
        overflow.set_value();
      }
    })};
    ASSERT_TRUE(browser);
    const auto epoch{browser->snapshot().m_delivery.m_epoch};
    ASSERT_TRUE(browser->observe(epoch, key, DiscoveryContribution::kIpv4));
    ASSERT_TRUE(browser->beginNative(epoch));
    const auto point{reset ? "dmn/discovery/delivery/reset-allocation"
                           : "dmn/discovery/delivery/backend-lost-allocation"};
    ASSERT_EQ(fiu_enable(point, 1, nullptr, FIU_ONETIME), 0);
    const auto result{reset ? browser->clearCache()
                            : browser->backendLost(epoch)};
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
    const auto snapshot{browser->snapshot()};
    EXPECT_EQ(snapshot.m_delivery.m_state, DiscoveryDeliveryState::kOverflow);
    EXPECT_GT(snapshot.m_delivery.m_epoch, epoch);
    EXPECT_TRUE(snapshot.m_browser.m_candidates.empty());
    EXPECT_EQ(snapshot.m_native_operations, 0);
    ASSERT_EQ(delivered.wait_for(std::chrono::seconds{2}),
              std::future_status::ready);
    ASSERT_TRUE(browser->clearCache());
    EXPECT_TRUE(browser->observe(browser->snapshot().m_delivery.m_epoch, key,
                                 DiscoveryContribution::kIpv4));
    EXPECT_TRUE(browser->stop());
    EXPECT_TRUE(client.shutdown());
  }
}

TEST(DmnDiscoveryWorkerFaultInjection, FailedResetDrainsOldCallback) {
  ASSERT_EQ(fiu_init(0), 0);
  using namespace std::chrono_literals;
  using namespace dmn::detail;
  std::promise<void> entered;
  auto enteredFuture{entered.get_future()};
  std::promise<void> release;
  auto releaseFuture{release.get_future().share()};
  DiscoverySessionClient client{{}};
  auto browser{client.browse([&](DiscoveryDelivery event) {
    if (event.m_data) {
      entered.set_value();
      releaseFuture.wait();
    }
  })};
  ASSERT_TRUE(browser);
  const auto epoch{browser->snapshot().m_delivery.m_epoch};
  ASSERT_TRUE(browser->observe(epoch, {"Printer", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  ASSERT_EQ(enteredFuture.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(fiu_enable("dmn/discovery/delivery/reset-allocation", 1, nullptr,
                       FIU_ONETIME),
            0);
  auto reset{
      std::async(std::launch::async, [&] { return browser->clearCache(); })};
  const auto deadline{std::chrono::steady_clock::now() + 2s};
  while (browser->snapshot().m_delivery.m_state !=
             DiscoveryDeliveryState::kOverflow &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto state{browser->snapshot().m_delivery.m_state};
  const auto blocked{reset.wait_for(20ms) == std::future_status::timeout};
  release.set_value();
  ASSERT_EQ(reset.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(state, DiscoveryDeliveryState::kOverflow);
  EXPECT_TRUE(blocked);
  const auto result{reset.get()};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_TRUE(browser->clearCache());
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection,
     BarrierAllocationFailureReleasesCallers) {
  using namespace std::chrono_literals;
  ASSERT_EQ(fiu_init(0), 0);
  dmn::detail::DiscoverySessionClient client{{}};
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  std::promise<void> release;
  stateFailureReached = &reached;
  stateFailureRelease = release.get_future().share();
  ASSERT_EQ(fiu_enable_external("dmn/discovery/reset-barrier/leader-allocation",
                                1, nullptr, FIU_ONETIME, failStateWorker),
            0);
  auto leader{
      std::async(std::launch::async, [&] { return browser->clearCache(); })};
  const bool reachedFailure{reachedFuture.wait_for(2s) ==
                            std::future_status::ready};
  auto follower{
      std::async(std::launch::async, [&] { return browser->clearCache(); })};
  const auto deadline{std::chrono::steady_clock::now() + 2s};
  while (reachedFailure && browser->snapshot().m_joined_reset_callers == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool joined{browser->snapshot().m_joined_reset_callers != 0};
  release.set_value();
  ASSERT_TRUE(reachedFailure);
  ASSERT_TRUE(joined);
  ASSERT_EQ(leader.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(follower.wait_for(2s), std::future_status::ready);
  const auto first{leader.get()};
  const auto second{follower.get()};
  ASSERT_FALSE(first);
  ASSERT_FALSE(second);
  EXPECT_EQ(first.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(second.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kResourceLimit);
  stateFailureReached = nullptr;
  EXPECT_TRUE(browser->clearCache());

  ASSERT_EQ(fiu_enable("dmn/discovery/reset-barrier/leader-failure", 1, nullptr,
                       FIU_ONETIME),
            0);
  const auto failedReset{browser->clearCache()};
  ASSERT_FALSE(failedReset);
  EXPECT_EQ(failedReset.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kBackend);
  EXPECT_TRUE(browser->clearCache());

  ASSERT_EQ(fiu_enable("dmn/discovery/stop-barrier/leader-allocation", 1,
                       nullptr, FIU_ONETIME),
            0);
  const auto failedStop{browser->stop()};
  ASSERT_FALSE(failedStop);
  EXPECT_EQ(failedStop.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kResourceLimit);
  ASSERT_EQ(fiu_enable("dmn/discovery/stop-barrier/leader-failure", 1, nullptr,
                       FIU_ONETIME),
            0);
  const auto failedStopBackend{browser->stop()};
  ASSERT_FALSE(failedStopBackend);
  EXPECT_EQ(failedStopBackend.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kBackend);
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection,
     InitialBrowserDeliveryFailureReleasesSlots) {
  ASSERT_EQ(fiu_init(0), 0);
  dmn::detail::DiscoveryOptions options;
  options.m_max_browsers = 1;
  options.m_max_registrations = 1;
  dmn::detail::DiscoverySessionClient client{options, false};
  constexpr auto point = "dmn/discovery/browser/initial-delivery";
  ASSERT_EQ(fiu_enable(point, 1, nullptr, 0), 0);
  for (int i = 0; i != 3; ++i) {
    auto failure{client.browse({})};
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().m_kind,
              dmn::detail::DiscoveryErrorKind::kResourceLimit);
  }
  ASSERT_EQ(fiu_disable(point), 0);
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  EXPECT_TRUE(browser->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection,
     InitialBrowserDeliveryFailureCanRollbackInsideCallback) {
  ASSERT_EQ(fiu_init(0), 0);
  dmn::detail::DiscoveryOptions options;
  options.m_max_browsers = 2;
  options.m_max_registrations = 1;
  dmn::detail::DiscoverySessionClient client{options, false};
  std::promise<bool> finished;
  auto result{finished.get_future()};
  auto first{client.browse([&](auto) {
    constexpr auto point = "dmn/discovery/browser/initial-delivery";
    bool ok{fiu_enable(point, 1, nullptr, 0) == 0};
    for (int i = 0; i != 4; ++i) {
      auto failure{client.browse({})};
      ok = ok && !failure &&
           failure.error().m_kind ==
               dmn::detail::DiscoveryErrorKind::kResourceLimit;
    }
    ok = fiu_disable(point) == 0 && ok;
    finished.set_value(ok);
  })};
  ASSERT_TRUE(first);
  ASSERT_EQ(result.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_TRUE(result.get());
  auto second{client.browse({})};
  ASSERT_TRUE(second);
  EXPECT_TRUE(second->stop());
  EXPECT_TRUE(first->stop());
  EXPECT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryWorkerFaultInjection, ShutdownFailureCanBeRetried) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoverySessionClient client{{}};
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  std::promise<void> release;
  shutdownFailureReached = &reached;
  shutdownFailureRelease = release.get_future().share();
  ASSERT_EQ(fiu_enable_external("dmn/discovery/shutdown/allocation", 1, nullptr,
                                FIU_ONETIME, failShutdown),
            0);
  auto failing{std::async(std::launch::async, [&] {
    try {
      static_cast<void>(client.shutdown());
      return false;
    } catch (const std::bad_alloc &) {
      return true;
    }
  })};
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  std::promise<void> secondStarted;
  auto secondStartedFuture{secondStarted.get_future()};
  auto retry{std::async(std::launch::async, [&] {
    secondStarted.set_value();
    return client.shutdown();
  })};
  ASSERT_EQ(secondStartedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  release.set_value();
  ASSERT_EQ(failing.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_TRUE(failing.get());
  ASSERT_EQ(retry.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  EXPECT_TRUE(retry.get());
  shutdownFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection, StateFailureCompletesWaitersAndJoins) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoveryCommandWorker worker{4, 1};
  auto poller{std::make_shared<TestPoller>()};
  ASSERT_TRUE(worker.attachPoller(poller));
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  std::promise<void> release;
  stateFailureReached = &reached;
  stateFailureRelease = release.get_future().share();
  ASSERT_EQ(fiu_enable_external("dmn/discovery/state-worker/fatal", 1, nullptr,
                                FIU_ONETIME, failStateWorker),
            0);
  auto completion{worker.submit([] { return dmn::detail::DiscoveryResult{}; })};
  ASSERT_TRUE(completion);
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  auto queued{worker.submit([] { return dmn::detail::DiscoveryResult{}; })};
  ASSERT_TRUE(queued);
  release.set_value();
  ASSERT_EQ(completion->wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  ASSERT_EQ(queued->wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_FALSE(completion->get());
  EXPECT_FALSE(queued->get());
  EXPECT_FALSE(worker.shutdown());
  EXPECT_TRUE(poller->stopped.load());
  EXPECT_FALSE(worker.shutdown());
  stateFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection, CallbackFailureStopsAndDrains) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoveryDispatcher dispatcher{1};
  auto handle{dispatcher.create(1, 1, [](dmn::detail::DiscoveryDelivery) {})};
  ASSERT_TRUE(handle);

  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  callbackFailureReached = &reached;
  ASSERT_EQ(fiu_enable_external("dmn/discovery/callback-worker/fatal", 1,
                                nullptr, FIU_ONETIME, failCallbackWorker),
            0);
  ASSERT_TRUE(handle->postControl(handle->snapshot().m_epoch,
                                  dmn::detail::DiscoveryControl::kReset));
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_FALSE(dispatcher.shutdown());
  EXPECT_EQ(handle->snapshot().m_state,
            dmn::detail::DiscoveryDeliveryState::kStopped);
  EXPECT_TRUE(handle->stop());
  EXPECT_FALSE(dispatcher.shutdown());
  callbackFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection, CallbackFailureCancelsNativeBrowser) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoverySessionClient client{{}};
  std::promise<void> cancelled;
  auto cancellation{cancelled.get_future()};
  auto browser{client.browse({}, {[&] {
                                    cancelled.set_value();
                                    return dmn::detail::DiscoveryResult{};
                                  },
                                  {}})};
  ASSERT_TRUE(browser);
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  callbackFailureReached = &reached;
  ASSERT_EQ(fiu_enable_external("dmn/discovery/callback-worker/fatal", 1,
                                nullptr, FIU_ONETIME, failCallbackWorker),
            0);
  ASSERT_TRUE(browser->backendLost(browser->snapshot().m_delivery.m_epoch));
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_EQ(cancellation.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  ASSERT_TRUE(browser->snapshot().m_cleanup_error);
  EXPECT_EQ(browser->snapshot().m_cleanup_error->m_kind,
            dmn::detail::DiscoveryErrorKind::kBackend);
  EXPECT_FALSE(client.shutdown());
  callbackFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection, CallbackFailureWithdrawsRegistration) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoverySessionClient client{{}};
  std::promise<void> cancelled;
  auto cancellation{cancelled.get_future()};
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  callbackFailureReached = &reached;
  ASSERT_EQ(fiu_enable_external("dmn/discovery/callback-worker/fatal", 1,
                                nullptr, FIU_ONETIME, failCallbackWorker),
            0);
  auto registration{
      client.advertise({{"Printer", "_http._tcp", "local.", 2}, 8080, {}}, {},
                       [&] { cancelled.set_value(); })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_EQ(cancellation.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  ASSERT_TRUE(registration->status().m_error);
  EXPECT_EQ(registration->status().m_error->m_kind,
            dmn::detail::DiscoveryErrorKind::kBackend);
  EXPECT_FALSE(client.shutdown());
  callbackFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection, CallbackFailureCleansEverySession) {
  ASSERT_EQ(fiu_init(0), 0);
  static_cast<void>(fiu_disable(failurePoint));
  dmn::detail::DiscoverySessionClient client{{}};
  std::atomic<int> cancelled{0};
  auto first{client.browse({}, {[&]() -> dmn::detail::DiscoveryResult {
                                  ++cancelled;
                                  return {};
                                },
                                {}})};
  auto second{client.browse({}, {[&]() -> dmn::detail::DiscoveryResult {
                                   ++cancelled;
                                   return {};
                                 },
                                 {}})};
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  std::promise<void> initial;
  auto initialFuture{initial.get_future()};
  auto registration{client.advertise(
      {{"Printer", "_http._tcp", "local.", 2}, 8080, {}},
      [&](auto) { initial.set_value(); }, [&] { ++cancelled; })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(initialFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  callbackFailureReached = &reached;
  ASSERT_EQ(fiu_enable_external("dmn/discovery/callback-worker/fatal", 1,
                                nullptr, FIU_ONETIME, failCallbackWorker),
            0);
  ASSERT_TRUE(first->backendLost(first->snapshot().m_delivery.m_epoch));
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  EXPECT_FALSE(client.shutdown());
  EXPECT_EQ(cancelled.load(), 3);
  callbackFailureReached = nullptr;
}

TEST(DmnDiscoveryWorkerFaultInjection,
     CallbackCleanupAllocationFailureIsVisible) {
  ASSERT_EQ(fiu_init(0), 0);
  dmn::detail::DiscoverySessionClient client{{}};
  std::atomic<int> cancelled{0};
  auto browser{client.browse({}, {[&]() -> dmn::detail::DiscoveryResult {
                                    ++cancelled;
                                    return {};
                                  },
                                  {}})};
  ASSERT_TRUE(browser);
  std::promise<void> initial;
  auto initialFuture{initial.get_future()};
  auto registration{client.advertise(
      {{"Printer", "_http._tcp", "local.", 2}, 8080, {}},
      [&](auto) { initial.set_value(); }, [&] { ++cancelled; })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(initialFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  ASSERT_EQ(fiu_enable("dmn/discovery/callback-worker/cleanup-allocation", 1,
                       nullptr, 0),
            0);
  std::promise<void> reached;
  auto reachedFuture{reached.get_future()};
  callbackFailureReached = &reached;
  ASSERT_EQ(fiu_enable_external("dmn/discovery/callback-worker/fatal", 1,
                                nullptr, FIU_ONETIME, failCallbackWorker),
            0);
  ASSERT_TRUE(browser->backendLost(browser->snapshot().m_delivery.m_epoch));
  ASSERT_EQ(reachedFuture.wait_for(std::chrono::seconds{2}),
            std::future_status::ready);
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{2}};
  while (std::chrono::steady_clock::now() < deadline) {
    auto b{browser->snapshot().m_cleanup_error};
    auto r{registration->status().m_error};
    if (b && r &&
        b->m_kind == dmn::detail::DiscoveryErrorKind::kResourceLimit &&
        r->m_kind == dmn::detail::DiscoveryErrorKind::kResourceLimit) {
      break;
    }
    std::this_thread::yield();
  }
  const auto browserError{browser->snapshot().m_cleanup_error};
  const auto registrationError{registration->status().m_error};
  EXPECT_TRUE(browserError &&
              browserError->m_kind ==
                  dmn::detail::DiscoveryErrorKind::kResourceLimit);
  EXPECT_TRUE(registrationError &&
              registrationError->m_kind ==
                  dmn::detail::DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(fiu_disable("dmn/discovery/callback-worker/cleanup-allocation"), 0);
  EXPECT_FALSE(client.shutdown());
  EXPECT_EQ(cancelled.load(), 2);
  callbackFailureReached = nullptr;
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
