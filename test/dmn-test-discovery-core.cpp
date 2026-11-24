/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-core.cpp
 * @brief Deterministic tests for the private DNS-SD discovery core.
 */

#include "dmn-discovery-core.hpp"
#include "dmn-discovery-records.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using dmn::detail::DiscoveryBackend;
using dmn::detail::DiscoveryBrowser;
using dmn::detail::DiscoveryContribution;
using dmn::detail::DiscoveryCore;
using dmn::detail::DiscoveryError;
using dmn::detail::DiscoveryErrorKind;
using dmn::detail::DiscoveryEvent;
using dmn::detail::DiscoveryEventKind;
using dmn::detail::DiscoveryOptions;
using dmn::detail::DiscoveryResolution;
using dmn::detail::DiscoveryResult;
using dmn::detail::DiscoveryServiceKey;
using dmn::detail::DiscoveryTxt;

/**
 * @brief Shared fake state, retained after backend destruction.
 */
struct BackendState {
  int m_initializations{0};
  int m_browses{0};
  int m_advertisements{0};
  int m_destructions{0};
  bool m_fail_initialization{false};
  bool m_fail_allocation{false};
};

/**
 * @brief Controlled backend used without a native daemon or network.
 */
class FakeBackend final : public DiscoveryBackend {
public:
  explicit FakeBackend(std::shared_ptr<BackendState> state)
      : m_state{std::move(state)} {}

  ~FakeBackend() override { ++m_state->m_destructions; }

  DiscoveryResult initialize() override {
    ++m_state->m_initializations;

    if (m_state->m_fail_allocation) {
      throw std::bad_alloc{};
    }

    if (m_state->m_fail_initialization) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "fake initialization failure"}};
    }

    return {};
  }

  DiscoveryResult browse() override {
    ++m_state->m_browses;

    return {};
  }

  DiscoveryResult advertise() override {
    ++m_state->m_advertisements;

    return {};
  }

private:
  std::shared_ptr<BackendState> m_state;
};

/**
 * @brief Monotonic fake clock for later retry and deadline tests.
 */
class FakeClock {
public:
  using duration = std::chrono::steady_clock::duration;
  using time_point = std::chrono::steady_clock::time_point;

  [[nodiscard]] time_point now() const { return m_now; }

  void advance(duration elapsed) { m_now += elapsed; }

private:
  time_point m_now{};
};

TEST(DmnDiscoveryCoreTest, ValidatesOptionsBeforeStartingBackend) {
  auto state{std::make_shared<BackendState>()};
  DiscoveryOptions options{};
  options.m_max_browsers = 0;

  auto client{
      DiscoveryCore::create(options, std::make_unique<FakeBackend>(state))};

  ASSERT_FALSE(client);
  EXPECT_EQ(state->m_initializations, 0);
  EXPECT_EQ(state->m_destructions, 1);
}

TEST(DmnDiscoveryCoreTest, ConstructionDoesNotBrowseOrAdvertise) {
  auto state{std::make_shared<BackendState>()};
  auto client{DiscoveryCore::create({}, std::make_unique<FakeBackend>(state))};

  ASSERT_TRUE(client);
  EXPECT_EQ(state->m_initializations, 1);
  EXPECT_EQ(state->m_browses, 0);
  EXPECT_EQ(state->m_advertisements, 0);

  client->reset();
  EXPECT_EQ(state->m_destructions, 1);
}

TEST(DmnDiscoveryCoreTest, FactoryFailureReleasesBackend) {
  auto state{std::make_shared<BackendState>()};
  state->m_fail_initialization = true;

  auto client{DiscoveryCore::create({}, std::make_unique<FakeBackend>(state))};

  ASSERT_FALSE(client);
  EXPECT_EQ(client.error().m_message, "fake initialization failure");
  EXPECT_EQ(state->m_initializations, 1);
  EXPECT_EQ(state->m_destructions, 1);
}

TEST(DmnDiscoveryCoreTest, FactoryAllocationFailureReleasesBackend) {
  auto state{std::make_shared<BackendState>()};
  state->m_fail_allocation = true;

  auto client{DiscoveryCore::create({}, std::make_unique<FakeBackend>(state))};

  ASSERT_FALSE(client);
  EXPECT_EQ(client.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(state->m_destructions, 1);
}

TEST(DmnDiscoveryCoreTest, RejectsInvalidOptionLimits) {
  DiscoveryOptions options{};
  EXPECT_TRUE(dmn::detail::validateOptions(options));

  options.m_refresh_timeout = 0ms;
  EXPECT_FALSE(dmn::detail::validateOptions(options));
  options = {};
  options.m_max_txt_bytes = 0;
  EXPECT_FALSE(dmn::detail::validateOptions(options));
  EXPECT_FALSE(DiscoveryCore::create({}, nullptr));
}

TEST(DmnDiscoveryCoreTest, ValidatesDnsSdServiceTypes) {
  EXPECT_TRUE(dmn::detail::validateServiceType("_http._tcp"));
  EXPECT_TRUE(dmn::detail::validateServiceType("_HTTP._TCP"));
  EXPECT_TRUE(dmn::detail::validateServiceType("_dmn-3._udp"));
  EXPECT_FALSE(dmn::detail::validateServiceType("http._tcp"));
  EXPECT_FALSE(dmn::detail::validateServiceType("_123._tcp"));
  EXPECT_FALSE(dmn::detail::validateServiceType("_bad--name._tcp"));
  EXPECT_FALSE(dmn::detail::validateServiceType("_http._sctp"));
  EXPECT_FALSE(dmn::detail::validateServiceType("_abcdefghijklmnop._tcp"));
}

TEST(DmnDiscoveryCoreTest, ValidatesInstanceNamesAndDomains) {
  EXPECT_TRUE(dmn::detail::validateInstanceName("My.Service"));
  EXPECT_TRUE(dmn::detail::validateInstanceName("Caf\xc3\xa9"));
  EXPECT_FALSE(dmn::detail::validateInstanceName(""));
  EXPECT_FALSE(dmn::detail::validateInstanceName(std::string(64, 'a')));
  EXPECT_FALSE(dmn::detail::validateInstanceName(std::string{"\xc0\x80", 2}));
  EXPECT_FALSE(dmn::detail::validateInstanceName(std::string{"a\0b", 3}));
  EXPECT_FALSE(dmn::detail::validateInstanceName("bad\nname"));
  EXPECT_FALSE(dmn::detail::validateInstanceName(std::string{"bad\x7f", 4}));

  EXPECT_TRUE(dmn::detail::validateDomain("local."));
  EXPECT_TRUE(dmn::detail::validateDomain("example.local."));
  EXPECT_TRUE(dmn::detail::validateDomain("."));
  EXPECT_TRUE(dmn::detail::validateDomain("literal\\.dot.local."));
  EXPECT_FALSE(dmn::detail::validateDomain(""));
  EXPECT_FALSE(dmn::detail::validateDomain("bad..local."));
  EXPECT_FALSE(dmn::detail::validateDomain("bad\\256.local."));
  EXPECT_FALSE(dmn::detail::validateDomain(std::string(64, 'a') + "."));
}

TEST(DmnDiscoveryCoreTest, PreservesBinaryTxtAndRejectsOversizeEntries) {
  DiscoveryTxt txt{{}, {0, 0xff, '=', 'x'}};
  ASSERT_TRUE(dmn::detail::validateTxt(txt, 2, 6));
  EXPECT_EQ(txt[1], (std::vector<std::uint8_t>{0, 0xff, '=', 'x'}));
  EXPECT_FALSE(dmn::detail::validateTxt(txt, 1, 6));
  EXPECT_FALSE(dmn::detail::validateTxt(txt, 2, 5));

  txt = {std::vector<std::uint8_t>(256, 1)};
  EXPECT_FALSE(dmn::detail::validateTxt(txt, 1, 1024));
}

TEST(DmnDiscoveryCoreTest, FakeClockAdvancesWithoutSleeping) {
  FakeClock clock{};
  const auto initial{clock.now()};
  clock.advance(5s);

  EXPECT_EQ(clock.now() - initial, 5s);
}

TEST(DmnDiscoveryCoreTest, MergesTransportContributionsAndRetainsOwnedKeys) {
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {}, [&](DiscoveryEvent event) { events.push_back(std::move(event)); }};
  DiscoveryServiceKey key{"Printer.1", "_http._tcp", "local.", 7};

  auto initial{browser.snapshot()};
  EXPECT_EQ(initial.m_sequence, 0);
  EXPECT_TRUE(initial.m_candidates.empty());

  auto ipv4{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(ipv4);
  auto duplicate{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(*duplicate, *ipv4);
  auto ipv6{browser.addObservation({"PRINTER.1", "_HTTP._TCP", "LOCAL", 7},
                                   DiscoveryContribution::kIpv6)};
  ASSERT_TRUE(ipv6);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].m_kind, DiscoveryEventKind::kAdded);

  EXPECT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *ipv4));
  EXPECT_EQ(browser.snapshot().m_candidates.size(), 1);
  EXPECT_EQ(events.size(), 1);
  EXPECT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv6, *ipv6));
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[1].m_kind, DiscoveryEventKind::kRemoved);
  EXPECT_EQ(events[0].m_candidate.m_key.m_instance, "Printer.1");
  EXPECT_EQ(events[1].m_sequence, 2);
  EXPECT_TRUE(browser.snapshot().m_candidates.empty());
}

TEST(DmnDiscoveryCoreTest, SeparatesInterfacesAndNormalizesDnsEscapes) {
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {}, [&](DiscoveryEvent event) { events.push_back(std::move(event)); }};
  const DiscoveryServiceKey first{"Printer", "_http._tcp",
                                  "office\\.one.local.", 4};
  const DiscoveryServiceKey same{"printer", "_http._tcp",
                                 "office\\046one.LOCAL", 4};
  const DiscoveryServiceKey second{"Printer", "_http._tcp",
                                   "office\\.one.local.", 5};
  const DiscoveryServiceKey unknown{"Printer", "_http._tcp",
                                    "office\\.one.local.", std::nullopt};

  ASSERT_TRUE(browser.addObservation(first, DiscoveryContribution::kIpv4));
  ASSERT_TRUE(browser.addObservation(same, DiscoveryContribution::kIpv4));
  ASSERT_TRUE(browser.addObservation(second, DiscoveryContribution::kIpv4));
  ASSERT_TRUE(browser.addObservation(unknown, DiscoveryContribution::kUnknown));
  EXPECT_EQ(events.size(), 3);
  EXPECT_EQ(browser.snapshot().m_candidates.size(), 3);
}

TEST(DmnDiscoveryCoreTest, IgnoresStaleSourceUpdatesAfterRemovalAndReadd) {
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {}, [&](DiscoveryEvent event) { events.push_back(std::move(event)); }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  auto old{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(old);
  ASSERT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *old));
  auto current{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(current);
  EXPECT_NE(*old, *current);

  EXPECT_FALSE(browser.updateResolution(key, DiscoveryContribution::kIpv4, *old,
                                        DiscoveryResolution::kResolving));
  EXPECT_FALSE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *old));
  EXPECT_TRUE(browser.updateResolution(key, DiscoveryContribution::kIpv4,
                                       *current,
                                       DiscoveryResolution::kResolving));
  ASSERT_EQ(events.size(), 4);
  EXPECT_EQ(events.back().m_kind, DiscoveryEventKind::kChanged);
  const auto snapshot{browser.snapshot()};
  ASSERT_EQ(snapshot.m_candidates.size(), 1);
  EXPECT_EQ(snapshot.m_sequence, events.back().m_sequence);
  EXPECT_EQ(snapshot.m_candidates[0].m_resolution,
            DiscoveryResolution::kResolving);
}

TEST(DmnDiscoveryCoreTest, RejectsInvalidKeysAndBoundsDistinctInstances) {
  DiscoveryOptions options{};
  options.m_max_instances = 1;
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{options, [&](DiscoveryEvent event) {
                             events.push_back(std::move(event));
                           }};
  const DiscoveryServiceKey first{"Printer", "_http._tcp", "local.", 2};
  const DiscoveryServiceKey second{"Other", "_http._tcp", "local.", 2};
  const DiscoveryServiceKey invalid{"Printer", "_http._tcp", "bad\\", 2};
  const DiscoveryServiceKey zeroInterface{"Printer", "_http._tcp", "local.", 0};

  EXPECT_FALSE(browser.addObservation(invalid, DiscoveryContribution::kIpv4));
  EXPECT_FALSE(
      browser.addObservation(zeroInterface, DiscoveryContribution::kIpv4));
  auto source{browser.addObservation(first, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  EXPECT_FALSE(browser.removeObservation(invalid, DiscoveryContribution::kIpv4,
                                         *source));
  EXPECT_FALSE(browser.updateResolution(invalid, DiscoveryContribution::kIpv4,
                                        *source,
                                        DiscoveryResolution::kResolving));
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(browser.snapshot().m_candidates.size(), 1);
  auto overflow{browser.addObservation(second, DiscoveryContribution::kIpv4)};
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().m_kind, DiscoveryErrorKind::kResourceLimit);
}

TEST(DmnDiscoveryCoreTest, SuppressesDuplicateResolutionChanges) {
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {}, [&](DiscoveryEvent event) { events.push_back(std::move(event)); }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateResolution(key, DiscoveryContribution::kIpv4,
                                       *source,
                                       DiscoveryResolution::kResolving));
  ASSERT_TRUE(browser.updateResolution(key, DiscoveryContribution::kIpv4,
                                       *source,
                                       DiscoveryResolution::kResolving));
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(browser.snapshot().m_sequence, 2);
}

TEST(DmnDiscoveryCoreTest, PublishesOwnedRecordAndAddressChanges) {
  using namespace dmn::detail;
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {}, [&](DiscoveryEvent event) { events.push_back(std::move(event)); }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);

  auto configure{browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.setMonitoring(token, DiscoveryRecordKind::kSrv, true) ||
            !records.setMonitoring(token, DiscoveryRecordKind::kTxt, true)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
        }

        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      })};
  ASSERT_TRUE(configure);
  ASSERT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kResolving);
  const auto resolutionGeneration{
      events.back().m_candidate.m_records.m_generation};
  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  const DiscoveryAddress wrongScope{
      DiscoveryAddressFamily::kIpv6,
      {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
      3};
  EXPECT_FALSE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.addAddress(token, resolutionGeneration,
                                *records.addressGeneration(
                                    token, DiscoveryAddressFamily::kIpv6),
                                "host.local.", 42, wrongScope)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "scope mismatch"}};
        }

        return {};
      }));
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        if (!records.addAddress(token, resolutionGeneration,
                                *records.addressGeneration(
                                    token, DiscoveryAddressFamily::kIpv4),
                                "host.local.", 42, address)) {
          return std::unexpected{
              DiscoveryError{DiscoveryErrorKind::kBackend, "address rejected"}};
        }
        return {};
      }));
  const auto readyEvent{events.back()};
  EXPECT_EQ(readyEvent.m_candidate.m_records.m_state,
            DiscoveryRecordState::kReady);
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_records.m_addresses,
            (std::vector{address}));

  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) {
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 43, "other.local."});
      }));
  EXPECT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kMultipleRecordsUnsupported);
  EXPECT_TRUE(events.back().m_candidate.m_records.m_addresses.empty());
  EXPECT_EQ(readyEvent.m_candidate.m_records.m_addresses,
            (std::vector{address}));

  ASSERT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *source));
  EXPECT_FALSE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &, std::uint64_t) -> DiscoveryResult { return {}; }));
}

TEST(DmnDiscoveryCoreTest, RecordFailureAndContributionRemovalEmitChanges) {
  using namespace dmn::detail;
  std::vector<DiscoveryEvent> events{};
  DiscoveryOptions options{};
  options.m_max_txt_bytes = 3;
  DiscoveryBrowser browser{options, [&](DiscoveryEvent event) {
                             events.push_back(std::move(event));
                           }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto first{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(first);
  const auto second{browser.addObservation(key, DiscoveryContribution::kIpv6)};
  ASSERT_TRUE(second);
  for (const auto &[kind, token] :
       {std::pair{DiscoveryContribution::kIpv4, *first},
        std::pair{DiscoveryContribution::kIpv6, *second}}) {
    ASSERT_TRUE(browser.updateRecords(
        key, kind, token,
        [](DiscoveryRecords &records, std::uint64_t id) -> DiscoveryResult {
          if (!records.setMonitoring(id, DiscoveryRecordKind::kSrv, true) ||
              !records.setMonitoring(id, DiscoveryRecordKind::kTxt, true)) {
            return std::unexpected{
                DiscoveryError{DiscoveryErrorKind::kBackend, "missing source"}};
          }
          return records.addSrv(
              id, *records.monitorGeneration(id, DiscoveryRecordKind::kSrv),
              {0, 0, 42, "host.local."});
        }));
  }
  ASSERT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kResolving);

  const auto failure{browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *first,
      [](DiscoveryRecords &records, std::uint64_t id) {
        return records.addTxt(
            id, *records.monitorGeneration(id, DiscoveryRecordKind::kTxt),
            {3, 'a', 'b', 'c'});
      })};
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(events.back().m_kind, DiscoveryEventKind::kChanged);
  EXPECT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kMonitorIncomplete);

  ASSERT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *first));
  EXPECT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kResolving);
  EXPECT_EQ(browser.snapshot().m_sequence, events.back().m_sequence);
}

TEST(DmnDiscoveryCoreTest, RecordUpdateCannotInvalidateItsOwnBrowserEntry) {
  using namespace dmn::detail;
  DiscoveryBrowser browser{{}, {}};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  EXPECT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &, std::uint64_t) -> DiscoveryResult {
        EXPECT_FALSE(browser.removeObservation(
            key, DiscoveryContribution::kIpv4, *source));
        return {};
      }));
  EXPECT_EQ(browser.snapshot().m_candidates.size(), 1);
}

TEST(DmnDiscoveryCoreTest, ThrowingRecordUpdateDoesNotPartiallyMutateState) {
  using namespace dmn::detail;
  DiscoveryBrowser browser{{}, {}};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  EXPECT_THROW(static_cast<void>(browser.updateRecords(
                   key, DiscoveryContribution::kIpv4, *source,
                   [](DiscoveryRecords &records,
                      std::uint64_t token) -> DiscoveryResult {
                     static_cast<void>(records.setMonitoring(
                         token, DiscoveryRecordKind::kSrv, true));
                     throw std::runtime_error{"failed update"};
                   })),
               std::runtime_error);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_FALSE(
            records.monitorGeneration(token, DiscoveryRecordKind::kSrv));
        return {};
      }));
}

TEST(DmnDiscoveryCoreTest, RefreshTimeoutIsNotChangedByLaterRecovery) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {},
      [&](DiscoveryEvent event) { events.push_back(std::move(event)); },
      [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kTxt, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  const auto oldGeneration{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  const auto refresh{browser.refresh(key)};
  ASSERT_TRUE(refresh);
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_refresh->m_state,
            DiscoveryRefreshState::kPending);
  const auto newGeneration{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  EXPECT_NE(oldGeneration, newGeneration);
  now += 5s;
  ASSERT_TRUE(browser.processRefreshDeadlines());
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_state,
            DiscoveryRefreshState::kFailed);

  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_FALSE(records.addAddress(
            token, oldGeneration,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42, address));
        EXPECT_TRUE(records.addAddress(
            token, newGeneration,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42, address));
        return {};
      }));
  EXPECT_EQ(events.back().m_candidate.m_records.m_state,
            DiscoveryRecordState::kReady);
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_state,
            DiscoveryRefreshState::kFailed);
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_id, *refresh);
}

TEST(DmnDiscoveryCoreTest, RefreshSupersedesAndSucceedsWithMatchingAddress) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {},
      [&](DiscoveryEvent event) { events.push_back(std::move(event)); },
      [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  EXPECT_FALSE(browser.refresh(key));
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kTxt, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  const auto first{browser.refresh(key)};
  ASSERT_TRUE(first);
  const auto oldGeneration{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  const auto second{browser.refresh(key)};
  ASSERT_TRUE(second);
  EXPECT_GT(*second, *first);
  const auto generation{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_FALSE(records.addAddress(
            token, oldGeneration,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42, address));
        EXPECT_TRUE(records.addAddress(
            token, generation,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42, address));
        return {};
      }));
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_id, *second);
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_state,
            DiscoveryRefreshState::kSucceeded);
  now += 10s;
  ASSERT_TRUE(browser.processRefreshDeadlines());
  EXPECT_EQ(events.back().m_candidate.m_refresh->m_state,
            DiscoveryRefreshState::kSucceeded);
}

TEST(DmnDiscoveryCoreTest, RetryFailureRemovesOnlyFailedSourceAndRecovers) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {},
      [&](DiscoveryEvent event) { events.push_back(std::move(event)); },
      [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto first{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  const auto second{browser.addObservation(key, DiscoveryContribution::kIpv6)};
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  for (const auto &[contribution, token] :
       {std::pair{DiscoveryContribution::kIpv4, *first},
        std::pair{DiscoveryContribution::kIpv6, *second}}) {
    ASSERT_TRUE(browser.updateRecords(
        key, contribution, token,
        [](DiscoveryRecords &records, std::uint64_t id) -> DiscoveryResult {
          EXPECT_TRUE(
              records.setMonitoring(id, DiscoveryRecordKind::kSrv, true));
          EXPECT_TRUE(
              records.setMonitoring(id, DiscoveryRecordKind::kTxt, true));
          return records.addSrv(
              id, *records.monitorGeneration(id, DiscoveryRecordKind::kSrv),
              {0, 0, 42, "host.local."});
        }));
  }
  const auto resolution{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  const auto firstLookup{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv4, *first, DiscoveryAttempt::kIpv4)};
  const auto secondLookup{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv6, *second, DiscoveryAttempt::kIpv4)};
  ASSERT_TRUE(firstLookup);
  ASSERT_TRUE(secondLookup);
  const DiscoveryAddress firstAddress{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  const DiscoveryAddress secondAddress{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}};
  for (const auto &[contribution, token, lookup, address] :
       {std::tuple{DiscoveryContribution::kIpv4, *first, *firstLookup,
                   firstAddress},
        std::tuple{DiscoveryContribution::kIpv6, *second, *secondLookup,
                   secondAddress}}) {
    ASSERT_TRUE(browser.updateRecords(
        key, contribution, token,
        [&](DiscoveryRecords &records, std::uint64_t id) -> DiscoveryResult {
          EXPECT_TRUE(records.addAddress(id, resolution, lookup, "host.local.",
                                         42, address));
          return {};
        }));
  }
  ASSERT_TRUE(browser.failAttempt(key, DiscoveryContribution::kIpv4, *first,
                                  DiscoveryAttempt::kIpv4, *firstLookup,
                                  resolution, true, "timeout"));
  const auto failed{browser.snapshot().m_candidates.front()};
  EXPECT_EQ(failed.m_records.m_addresses, (std::vector{secondAddress}));
  ASSERT_EQ(failed.m_failures.size(), 1);
  const auto earlyRetries{browser.takeDueRetries()};
  ASSERT_TRUE(earlyRetries);
  EXPECT_TRUE(earlyRetries->empty());
  now += 1s;
  const auto retries{browser.takeDueRetries()};
  ASSERT_TRUE(retries);
  EXPECT_EQ(*retries,
            (std::vector{DiscoveryAttemptId{*first, DiscoveryAttempt::kIpv4}}));
  const auto retryLookup{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv4, *first, DiscoveryAttempt::kIpv4)};
  ASSERT_TRUE(retryLookup);
  ASSERT_NE(*retryLookup, *firstLookup);
  EXPECT_FALSE(browser.failAttempt(key, DiscoveryContribution::kIpv4, *first,
                                   DiscoveryAttempt::kIpv4, *firstLookup,
                                   resolution, true, "late failure"));
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *first,
      [&](DiscoveryRecords &records, std::uint64_t id) -> DiscoveryResult {
        EXPECT_FALSE(records.addAddress(id, resolution, *firstLookup,
                                        "host.local.", 42, firstAddress));
        EXPECT_TRUE(records.addAddress(id, resolution, *retryLookup,
                                       "host.local.", 42, firstAddress));
        return {};
      }));
  ASSERT_TRUE(browser.succeedAttempt(key, DiscoveryContribution::kIpv4, *first,
                                     DiscoveryAttempt::kIpv4, *retryLookup,
                                     resolution));
  EXPECT_TRUE(browser.snapshot().m_candidates.front().m_failures.empty());
}

TEST(DmnDiscoveryCoreTest, RefreshFromEventCallbackAndRemovalCancelsDeadline) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoveryBrowser *browserPtr{nullptr};
  std::optional<std::uint64_t> refreshId{};
  std::vector<DiscoveryEvent> events{};
  DiscoveryBrowser browser{
      {},
      [&](DiscoveryEvent event) {
        events.push_back(event);
        if (event.m_kind == DiscoveryEventKind::kAdded) {
          const auto refresh{browserPtr->refresh(event.m_candidate.m_key)};
          EXPECT_TRUE(refresh);
          if (refresh) {
            refreshId = *refresh;
          }
        }
      },
      [&] { return now; }};
  browserPtr = &browser;
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(refreshId);
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_refresh->m_id,
            *refreshId);
  ASSERT_TRUE(
      browser.removeObservation(key, DiscoveryContribution::kIpv4, *source));
  const auto count{events.size()};
  now += 10s;
  ASSERT_TRUE(browser.processRefreshDeadlines());
  EXPECT_EQ(events.size(), count);
}

TEST(DmnDiscoveryCoreTest, DeadlineWinsOverLateAddressWithoutTimerPolling) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoveryBrowser browser{{}, {}, [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kTxt, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  ASSERT_TRUE(browser.refresh(key));
  const auto generation{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  now += 5s;
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [&](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        const DiscoveryAddress address{
            DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
        EXPECT_TRUE(records.addAddress(
            token, generation,
            *records.addressGeneration(token, DiscoveryAddressFamily::kIpv4),
            "host.local.", 42, address));
        return {};
      }));
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_refresh->m_state,
            DiscoveryRefreshState::kFailed);
}

TEST(DmnDiscoveryCoreTest, RecordWatchFailureRetriesWithoutAnotherBrowse) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoveryBrowser browser{{}, {}, [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kTxt, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  const auto oldWatch{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv4, *source, DiscoveryAttempt::kSrv)};
  ASSERT_TRUE(oldWatch);
  ASSERT_TRUE(browser.failAttempt(key, DiscoveryContribution::kIpv4, *source,
                                  DiscoveryAttempt::kSrv, *oldWatch, 0, true,
                                  "disconnected"));
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_records.m_state,
            DiscoveryRecordState::kMonitorIncomplete);
  now += 1s;
  const auto retries{browser.takeDueRetries()};
  ASSERT_TRUE(retries);
  EXPECT_EQ(*retries,
            (std::vector{DiscoveryAttemptId{*source, DiscoveryAttempt::kSrv}}));
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  const auto newWatch{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv4, *source, DiscoveryAttempt::kSrv)};
  ASSERT_TRUE(newWatch);
  ASSERT_NE(*newWatch, *oldWatch);
  EXPECT_FALSE(browser.succeedAttempt(key, DiscoveryContribution::kIpv4,
                                      *source, DiscoveryAttempt::kSrv,
                                      *oldWatch, 0));
  ASSERT_TRUE(browser.succeedAttempt(key, DiscoveryContribution::kIpv4, *source,
                                     DiscoveryAttempt::kSrv, *newWatch, 0));
  EXPECT_TRUE(browser.snapshot().m_candidates.front().m_failures.empty());
}

TEST(DmnDiscoveryCoreTest, RefreshCancelsAddressRetryButNotRecordRetry) {
  using namespace dmn::detail;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoveryBrowser browser{{}, {}, [&] { return now; }};
  const DiscoveryServiceKey key{"Printer", "_http._tcp", "local.", 2};
  const auto source{browser.addObservation(key, DiscoveryContribution::kIpv4)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(browser.updateRecords(
      key, DiscoveryContribution::kIpv4, *source,
      [](DiscoveryRecords &records, std::uint64_t token) -> DiscoveryResult {
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kSrv, true));
        EXPECT_TRUE(
            records.setMonitoring(token, DiscoveryRecordKind::kTxt, true));
        return records.addSrv(
            token, *records.monitorGeneration(token, DiscoveryRecordKind::kSrv),
            {0, 0, 42, "host.local."});
      }));
  const auto generation{
      browser.snapshot().m_candidates.front().m_records.m_generation};
  const auto address{browser.attemptGeneration(
      key, DiscoveryContribution::kIpv4, *source, DiscoveryAttempt::kIpv4)};
  const auto txt{browser.attemptGeneration(key, DiscoveryContribution::kIpv4,
                                           *source, DiscoveryAttempt::kTxt)};
  ASSERT_TRUE(address);
  ASSERT_TRUE(txt);
  ASSERT_TRUE(browser.failAttempt(key, DiscoveryContribution::kIpv4, *source,
                                  DiscoveryAttempt::kIpv4, *address, generation,
                                  true, "address timeout"));
  ASSERT_TRUE(browser.failAttempt(key, DiscoveryContribution::kIpv4, *source,
                                  DiscoveryAttempt::kTxt, *txt, generation,
                                  true, "TXT watch failure"));
  ASSERT_TRUE(browser.refresh(key));
  now += 1s;
  const auto retries{browser.takeDueRetries()};
  ASSERT_TRUE(retries);
  EXPECT_EQ(*retries,
            (std::vector{DiscoveryAttemptId{*source, DiscoveryAttempt::kTxt}}));
  EXPECT_EQ(browser.snapshot().m_candidates.front().m_failures.size(), 2);
}

} // namespace
