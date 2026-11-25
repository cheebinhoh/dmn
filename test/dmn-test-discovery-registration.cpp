/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-registration.cpp
 * @brief Deterministic registration contract tests.
 */

#include "dmn-discovery-registration.hpp"
#include "dmn-discovery-session.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace dmn::detail;

DiscoveryAdvertisement advertisement() {
  return {{"Printer", "_http._tcp", "local.", 2}, 8080, {{'a', 0, 'b'}, {}}};
}

TEST(DmnDiscoveryRegistrationTest, ValidatesAdvertisementBeforeAccepting) {
  DiscoveryOptions options{};
  auto valid{advertisement()};
  EXPECT_TRUE(DiscoveryRegistration::create(options, valid, true, {}));

  auto invalid{valid};
  invalid.m_port = 0;
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
  invalid = valid;
  invalid.m_key.m_instance.clear();
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
  invalid = valid;
  invalid.m_key.m_type = "_http._invalid";
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
  invalid = valid;
  invalid.m_key.m_domain = "invalid..domain";
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
  invalid = valid;
  invalid.m_key.m_interface = 0;
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
  invalid = valid;
  invalid.m_txt = {std::vector<std::uint8_t>(256)};
  EXPECT_FALSE(DiscoveryRegistration::create(options, invalid, true, {}));
}

TEST(DmnDiscoveryRegistrationTest, AcceptedIsNotEstablishedAndOwnsBinaryTxt) {
  auto input{advertisement()};
  std::vector<DiscoveryRegistrationState> events{};
  auto registration{DiscoveryRegistration::create(
      {}, input, true, [&](DiscoveryRegistrationStatus status) {
        events.push_back(status.m_state);
      })};
  ASSERT_TRUE(registration);
  input.m_txt[0][0] = 'z';
  EXPECT_EQ((*registration)->advertisement().m_txt[0][0], 'a');
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kRegistering);
  ASSERT_TRUE((*registration)->established((*registration)->generation()));
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kEstablished);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0], DiscoveryRegistrationState::kEstablished);
}

TEST(DmnDiscoveryRegistrationTest, CollisionAndFailureAreVisibleWithoutRename) {
  auto registration{
      DiscoveryRegistration::create({}, advertisement(), true, {})};
  ASSERT_TRUE(registration);
  const auto generation{(*registration)->generation()};
  ASSERT_TRUE((*registration)->collision(generation, "name collision"));
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kCollision);
  EXPECT_EQ((*registration)->advertisement().m_key.m_instance, "Printer");
  EXPECT_EQ((*registration)->status().m_error->m_message, "name collision");
}

TEST(DmnDiscoveryRegistrationTest, LossRecoveryRejectsLateNativeCallbacks) {
  auto registration{
      DiscoveryRegistration::create({}, advertisement(), true, {})};
  ASSERT_TRUE(registration);
  const auto old{(*registration)->generation()};
  ASSERT_TRUE((*registration)->backendLost());
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kUnavailable);
  ASSERT_TRUE((*registration)->backendAvailable());
  EXPECT_GT((*registration)->generation(), old);
  EXPECT_FALSE((*registration)->established(old));
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kRegistering);
  ASSERT_TRUE((*registration)->established((*registration)->generation()));
}

TEST(DmnDiscoveryRegistrationTest, NativeCancellationRunsOnlyOnce) {
  DiscoverySessionClient client{{}};
  std::atomic<unsigned> cancellations{0};
  auto registration{
      client.advertise(advertisement(), {}, [&] { ++cancellations; })};
  ASSERT_TRUE(registration);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(registration->stop());
  EXPECT_EQ(cancellations.load(), 1);
  ASSERT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryRegistrationTest, NotificationAllocationFailureIsTerminal) {
  auto registration{DiscoveryRegistration::create(
      {}, advertisement(), true,
      [](const DiscoveryRegistrationStatus &) { throw std::bad_alloc{}; })};
  ASSERT_TRUE(registration);
  const auto generation{(*registration)->generation()};
  const auto result{(*registration)->established(generation)};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  const auto status{(*registration)->status()};
  EXPECT_EQ(status.m_state, DiscoveryRegistrationState::kOverflow);
  ASSERT_TRUE(status.m_error);
  EXPECT_EQ(status.m_error->m_kind, DiscoveryErrorKind::kResourceLimit);
  (*registration)->markOverflow();
  EXPECT_EQ((*registration)->status().m_error->m_message,
            status.m_error->m_message);
  EXPECT_FALSE((*registration)->established(generation));
  EXPECT_FALSE((*registration)->backendAvailable());
}

TEST(DmnDiscoveryRegistrationTest, AdmissionAllocationFailureReleasesCapacity) {
  DiscoveryOptions options{};
  options.m_max_browsers = 1;
  options.m_max_registrations = 1;
  std::atomic<bool> fail{true};
  DiscoverySessionClient client{
      options, true, std::chrono::steady_clock::now,
      [&](DiscoveryRegistrationFaultPoint point) {
        if (point == DiscoveryRegistrationFaultPoint::kAdmission &&
            fail.exchange(false)) {
          throw std::bad_alloc{};
        }
      }};
  auto failed{client.advertise(advertisement(), {})};
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  auto browser{client.browse({})};
  auto retry{client.advertise(advertisement(), {})};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(retry);
  EXPECT_TRUE(retry->stop());
  EXPECT_TRUE(browser->stop());
}

TEST(DmnDiscoveryRegistrationTest, DeliveryAllocationFailureWithdraws) {
  for (const auto point :
       {DiscoveryRegistrationFaultPoint::kInitialDelivery,
        DiscoveryRegistrationFaultPoint::kTransitionDelivery}) {
    std::atomic<bool> fail{true};
    std::promise<DiscoveryRegistrationStatus> overflow{};
    DiscoverySessionClient client{{},
                                  true,
                                  std::chrono::steady_clock::now,
                                  [&](DiscoveryRegistrationFaultPoint current) {
                                    if (current == point &&
                                        fail.exchange(false)) {
                                      throw std::bad_alloc{};
                                    }
                                  }};
    auto registration{client.advertise(
        advertisement(), [&](DiscoveryRegistrationStatus status) {
          if (status.m_state == DiscoveryRegistrationState::kOverflow) {
            overflow.set_value(std::move(status));
          }
        })};
    ASSERT_TRUE(registration);
    if (point == DiscoveryRegistrationFaultPoint::kTransitionDelivery) {
      const auto result{registration->established(registration->generation())};
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
    }
    const auto reported{overflow.get_future().get()};
    EXPECT_EQ(reported.m_state, DiscoveryRegistrationState::kOverflow);
    ASSERT_TRUE(reported.m_error);
    EXPECT_EQ(reported.m_error->m_kind, DiscoveryErrorKind::kResourceLimit);
    EXPECT_FALSE(registration->nativeActive());
    EXPECT_TRUE(registration->stop());
    ASSERT_TRUE(registration->status().m_error);
    EXPECT_EQ(registration->status().m_error->m_kind,
              DiscoveryErrorKind::kResourceLimit);
  }
}

TEST(DmnDiscoveryRegistrationTest, StopIsIdempotentAndSuppressesEvents) {
  std::vector<DiscoveryRegistrationState> events{};
  auto registration{DiscoveryRegistration::create(
      {}, advertisement(), false, [&](DiscoveryRegistrationStatus status) {
        events.push_back(status.m_state);
      })};
  ASSERT_TRUE(registration);
  const auto generation{(*registration)->generation()};
  ASSERT_TRUE((*registration)->stop());
  ASSERT_TRUE((*registration)->stop());
  EXPECT_FALSE((*registration)->backendAvailable());
  EXPECT_FALSE((*registration)->established(generation));
  EXPECT_TRUE(events.empty());
  EXPECT_EQ((*registration)->status().m_state,
            DiscoveryRegistrationState::kStopped);
}

TEST(DmnDiscoveryRegistrationTest, SessionRetainsStatusAfterClientShutdown) {
  DiscoveryRegistrationHandle handle{};
  {
    DiscoverySessionClient client{{}};
    auto registration{client.advertise(advertisement(), {})};
    ASSERT_TRUE(registration);
    handle = *registration;
    EXPECT_EQ(handle.status().m_state,
              DiscoveryRegistrationState::kRegistering);
    EXPECT_TRUE(handle.nativeActive());
    ASSERT_TRUE(handle.established(handle.generation()));
    EXPECT_EQ(handle.status().m_state,
              DiscoveryRegistrationState::kEstablished);
    ASSERT_TRUE(client.shutdown());
  }
  EXPECT_EQ(handle.status().m_state, DiscoveryRegistrationState::kStopped);
  EXPECT_FALSE(handle.nativeActive());
  EXPECT_FALSE(handle.established(handle.generation()));
  EXPECT_TRUE(handle.stop());
}

TEST(DmnDiscoveryRegistrationTest, SessionCallbackFailureDoesNotStopBrowser) {
  DiscoverySessionClient client{{}};
  std::promise<void> called{};
  auto registration{client.advertise(
      advertisement(), [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          called.set_value();
          throw std::runtime_error{"callback failed"};
        }
      })};
  ASSERT_TRUE(registration);
  auto browser{client.browse({})};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration->established(registration->generation()));
  called.get_future().wait();
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(registration->status().m_error);
  EXPECT_EQ(registration->status().m_error->m_message,
            "registration callback failed");
  const DiscoveryServiceKey key{"Other", "_http._tcp", "local.", 2};
  EXPECT_TRUE(browser->observe(browser->snapshot().m_delivery.m_epoch, key,
                               DiscoveryContribution::kIpv4));
}

TEST(DmnDiscoveryRegistrationTest, ConversionFailureRetainsItsError) {
  DiscoverySessionClient client{{}};
  std::promise<void> failingEntered;
  std::promise<void> delivered;
  auto registration{client.advertise(
      advertisement(), [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          failingEntered.set_value();
          throw DiscoveryDeliveryFailure{
              {DiscoveryErrorKind::kResourceLimit, "conversion failed"}};
        }
      })};
  auto browser{
      client.browse([&](DiscoveryDelivery) { delivered.set_value(); })};
  ASSERT_TRUE(registration);
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration->established(registration->generation()));
  failingEntered.get_future().wait();
  ASSERT_TRUE(browser->observe(browser->snapshot().m_delivery.m_epoch,
                               {"Other", "_http._tcp", "local.", 2},
                               DiscoveryContribution::kIpv4));
  delivered.get_future().wait();
  const auto status{registration->status()};
  ASSERT_TRUE(status.m_error);
  EXPECT_EQ(status.m_error->m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(status.m_error->m_message, "conversion failed");
  EXPECT_TRUE(status.m_callback_failed);
  EXPECT_TRUE(registration->stop());
  EXPECT_TRUE(browser->stop());
}

TEST(DmnDiscoveryRegistrationTest, CallbackCleanupAllocationFailureWithdraws) {
  std::promise<void> cleanupEntered{};
  DiscoverySessionClient client{
      {},
      true,
      std::chrono::steady_clock::now,
      [&](DiscoveryRegistrationFaultPoint point) {
        if (point == DiscoveryRegistrationFaultPoint::kCallbackCleanup) {
          cleanupEntered.set_value();
          throw std::bad_alloc{};
        }
      }};
  auto registration{
      client.advertise(advertisement(), [](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          throw std::runtime_error{"callback failed"};
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_TRUE(registration->established(registration->generation()));
  cleanupEntered.get_future().wait();
  ASSERT_TRUE(registration->stop());
  EXPECT_FALSE(registration->nativeActive());
  const auto status{registration->status()};
  EXPECT_EQ(status.m_state, DiscoveryRegistrationState::kStopped);
  ASSERT_TRUE(status.m_error);
  EXPECT_EQ(status.m_error->m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(status.m_error->m_message, "callback cleanup allocation failed");
}

TEST(DmnDiscoveryRegistrationTest, SessionRecoveryRejectsOldCallbacks) {
  DiscoverySessionClient client{{}};
  auto registration{client.advertise(advertisement(), {})};
  ASSERT_TRUE(registration);
  const auto old{registration->generation()};
  ASSERT_TRUE(registration->established(old));
  ASSERT_TRUE(registration->backendLost());
  EXPECT_FALSE(registration->nativeActive());
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kUnavailable);
  ASSERT_TRUE(registration->backendAvailable());
  EXPECT_TRUE(registration->nativeActive());
  EXPECT_FALSE(registration->established(old));
  ASSERT_TRUE(registration->established(registration->generation()));
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kEstablished);
}

TEST(DmnDiscoveryRegistrationTest, FailureIsVisibleAndDoesNotRename) {
  DiscoverySessionClient client{{}};
  auto registration{client.advertise(advertisement(), {})};
  ASSERT_TRUE(registration);
  ASSERT_TRUE(registration->failed(registration->generation(), "native error"));
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kFailed);
  ASSERT_TRUE(registration->status().m_error);
  EXPECT_EQ(registration->status().m_error->m_message, "native error");
  EXPECT_FALSE(registration->nativeActive());
}

TEST(DmnDiscoveryRegistrationTest,
     InitialAbsenceIsReportedWithoutRegistration) {
  DiscoverySessionClient client{{}, false};
  std::promise<DiscoveryRegistrationState> initial{};
  auto registration{client.advertise(advertisement(),
                                     [&](DiscoveryRegistrationStatus status) {
                                       initial.set_value(status.m_state);
                                     })};
  ASSERT_TRUE(registration);
  EXPECT_EQ(initial.get_future().get(),
            DiscoveryRegistrationState::kUnavailable);
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kUnavailable);
  EXPECT_TRUE(registration->stop());
}

TEST(DmnDiscoveryRegistrationTest, StopDrainsCallbackWithoutStoppedEvent) {
  DiscoverySessionClient client{{}};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::vector<DiscoveryRegistrationState> states{};
  auto registration{client.advertise(
      advertisement(), [&](DiscoveryRegistrationStatus status) {
        if (status.m_state != DiscoveryRegistrationState::kEstablished) {
          return;
        }
        std::unique_lock lock{mutex};
        states.push_back(status.m_state);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
      })};
  ASSERT_TRUE(registration);
  ASSERT_TRUE(registration->established(registration->generation()));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  auto stopped{
      std::async(std::launch::async, [&] { return registration->stop(); })};
  while (registration->status().m_state !=
         DiscoveryRegistrationState::kStopped) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(registration->backendAvailable());
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_TRUE(stopped.get());
  ASSERT_EQ(states.size(), 1);
  EXPECT_EQ(states.front(), DiscoveryRegistrationState::kEstablished);
}

TEST(DmnDiscoveryRegistrationTest, RegistrationLimitIsIndependentOfBrowsers) {
  DiscoveryOptions options{};
  options.m_max_browsers = 1;
  options.m_max_registrations = 1;
  DiscoverySessionClient client{options};
  auto browser{client.browse({})};
  auto registration{client.advertise(advertisement(), {})};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration);
  EXPECT_FALSE(client.advertise(advertisement(), {}));
  EXPECT_FALSE(client.browse({}));
  ASSERT_TRUE(registration->stop());
  EXPECT_TRUE(client.advertise(advertisement(), {}));
}

TEST(DmnDiscoveryRegistrationTest, StoppingOneRegistrationDoesNotStopAnother) {
  DiscoverySessionClient client{{}};
  auto first{client.advertise(advertisement(), {})};
  auto second{client.advertise(advertisement(), {})};
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(first->stop());
  EXPECT_FALSE(first->nativeActive());
  EXPECT_TRUE(second->nativeActive());
  EXPECT_TRUE(second->established(second->generation()));
  EXPECT_EQ(second->status().m_state, DiscoveryRegistrationState::kEstablished);
}

TEST(DmnDiscoveryRegistrationTest,
     SaturatedRegistrationQueueWithdrawsAndReports) {
  DiscoveryOptions options{};
  options.m_max_registration_events = 1;
  DiscoverySessionClient client{options};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  std::promise<void> initial{};
  std::promise<DiscoveryRegistrationStatus> overflow{};
  auto browser{client.browse([&](DiscoveryDelivery) {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  })};
  ASSERT_TRUE(browser);
  auto registration{client.advertise(
      advertisement(), [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kRegistering) {
          initial.set_value();
        }
        if (status.m_state == DiscoveryRegistrationState::kOverflow) {
          overflow.set_value(std::move(status));
        }
      })};
  ASSERT_TRUE(registration);
  initial.get_future().wait();
  const DiscoveryServiceKey key{"Other", "_http._tcp", "local.", 2};
  ASSERT_TRUE(browser->observe(browser->snapshot().m_delivery.m_epoch, key,
                               DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  const auto generation{registration->generation()};
  EXPECT_TRUE(registration->established(generation));
  EXPECT_FALSE(registration->collision(generation, "collision"));
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kOverflow);
  EXPECT_FALSE(registration->nativeActive());
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_EQ(overflow.get_future().get().m_state,
            DiscoveryRegistrationState::kOverflow);
  EXPECT_FALSE(registration->backendAvailable());
  EXPECT_TRUE(registration->stop());
  ASSERT_TRUE(registration->status().m_error);
  EXPECT_EQ(registration->status().m_error->m_kind,
            DiscoveryErrorKind::kResourceLimit);
}

TEST(DmnDiscoveryRegistrationTest, SaturatedRegistrationControlQueueOverflows) {
  DiscoveryOptions options{};
  options.m_max_control_events = 1;
  DiscoverySessionClient client{options};
  std::mutex mutex{};
  std::condition_variable changed{};
  bool entered{false};
  bool release{false};
  auto browser{client.browse([&](DiscoveryDelivery) {
    std::unique_lock lock{mutex};
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  })};
  ASSERT_TRUE(browser);
  auto registration{client.advertise(advertisement(), {})};
  ASSERT_TRUE(registration);
  const DiscoveryServiceKey key{"Other", "_http._tcp", "local.", 2};
  ASSERT_TRUE(browser->observe(browser->snapshot().m_delivery.m_epoch, key,
                               DiscoveryContribution::kIpv4));
  {
    std::unique_lock lock{mutex};
    changed.wait(lock, [&] { return entered; });
  }
  EXPECT_TRUE(registration->backendLost());
  EXPECT_FALSE(registration->backendAvailable());
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kOverflow);
  EXPECT_FALSE(registration->nativeActive());
  {
    std::lock_guard lock{mutex};
    release = true;
  }
  changed.notify_all();
  EXPECT_TRUE(registration->stop());
}

} // namespace
