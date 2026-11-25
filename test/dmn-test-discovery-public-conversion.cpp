/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-public-conversion.cpp
 * @brief Owned public discovery value conversion tests.
 */

#include "dmn-discovery-public-conversion.hpp"

#include <gtest/gtest.h>

namespace {
using namespace dmn;
using namespace dmn::detail;

TEST(DmnDiscoveryPublicConversionTest, OwnsBinaryTxtAndScopedAddresses) {
  DiscoveryCandidate candidate;
  candidate.m_key = {"Printer.Room", "_ipp._tcp", "local.", 7};
  candidate.m_records.m_state = DiscoveryRecordState::kReady;
  candidate.m_records.m_srv = DiscoverySrv{0, 0, 8080, "host.local."};
  candidate.m_records.m_txt_wire =
      std::vector<std::uint8_t>{2, 0, 255, 0, 2, 0, 255};
  candidate.m_records.m_addresses = {
      {DiscoveryAddressFamily::kIpv6,
       {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
       7}};
  auto converted{discoveryPublicCandidate(candidate, {})};
  ASSERT_TRUE(converted);
  candidate.m_records.m_txt_wire->clear();
  candidate.m_key.m_instance.clear();
  EXPECT_EQ(converted->key.instance, "Printer.Room");
  EXPECT_EQ(converted->txt,
            (std::vector<Dmn_DiscoveryTxtEntry>{{0, 255}, {}, {0, 255}}));
  EXPECT_EQ(converted->txt_state, Dmn_DiscoveryTxtState::Present);
  ASSERT_EQ(converted->addresses.size(), 1);
  EXPECT_EQ(converted->addresses.front().interface_index, 7);
  EXPECT_EQ(converted->resolution, Dmn_DiscoveryResolutionState::Resolved);
}

TEST(DmnDiscoveryPublicConversionTest, DistinguishesUnknownTxtAndOverflow) {
  DiscoveryCandidate candidate;
  candidate.m_records.m_state = DiscoveryRecordState::kMonitorIncomplete;
  candidate.m_provenance.m_multicast = true;
  auto converted{discoveryPublicCandidate(candidate, {})};
  ASSERT_TRUE(converted);
  EXPECT_EQ(converted->provenance.multicast, true);
  EXPECT_FALSE(converted->provenance.local);
  EXPECT_EQ(converted->txt_state, Dmn_DiscoveryTxtState::Unknown);
  EXPECT_EQ(converted->record_state,
            Dmn_DiscoveryRecordState::MonitoringIncomplete);

  DiscoverySessionSnapshot snapshot{
      {2, 5, DiscoveryDeliveryState::kOverflow, false, true},
      {2, 5, {}},
      true,
      0,
      {},
      0,
      false};
  auto publicSnapshot{discoveryPublicSnapshot(snapshot, {})};
  ASSERT_TRUE(publicSnapshot);
  ASSERT_TRUE(publicSnapshot->error);
  EXPECT_EQ(publicSnapshot->state, Dmn_DiscoveryBrowserState::Overflow);
  EXPECT_EQ(publicSnapshot->error->kind, Dmn_DiscoveryErrorKind::ResourceLimit);
}

TEST(DmnDiscoveryPublicConversionTest, ControlEventsCarryEpochAndFailure) {
  DiscoveryDelivery delivery{4, 9, {}, DiscoveryControl::kOverflow, {}};
  auto event{discoveryPublicEvent(delivery, {})};
  ASSERT_TRUE(event);
  ASSERT_TRUE(event->error);
  EXPECT_EQ(event->epoch, 4);
  EXPECT_EQ(event->sequence, 9);
  EXPECT_EQ(event->kind, Dmn_DiscoveryEventKind::Error);
  EXPECT_EQ(event->error->kind, Dmn_DiscoveryErrorKind::ResourceLimit);
}

TEST(DmnDiscoveryPublicConversionTest, RetainsTerminalCallbackFailures) {
  DiscoverySessionSnapshot snapshot{
      {2, 5, DiscoveryDeliveryState::kStopped, true, false},
      {2, 5, {}},
      true,
      0,
      {},
      0,
      true};
  auto browser{discoveryPublicSnapshot(snapshot, {})};
  ASSERT_TRUE(browser);
  EXPECT_EQ(browser->state, Dmn_DiscoveryBrowserState::CallbackFailure);
  ASSERT_TRUE(browser->error);
  EXPECT_EQ(browser->error->kind, Dmn_DiscoveryErrorKind::CallbackFailure);
  snapshot.m_cleanup_error =
      DiscoveryError{DiscoveryErrorKind::kBackend, "conversion failed"};
  browser = discoveryPublicSnapshot(snapshot, {});
  ASSERT_TRUE(browser);
  EXPECT_EQ(browser->state, Dmn_DiscoveryBrowserState::Failed);
  EXPECT_EQ(browser->error->kind, Dmn_DiscoveryErrorKind::Internal);
  snapshot.m_explicit_stop = true;
  browser = discoveryPublicSnapshot(snapshot, {});
  ASSERT_TRUE(browser);
  EXPECT_EQ(browser->state, Dmn_DiscoveryBrowserState::Stopped);
  ASSERT_TRUE(browser->error);

  DiscoveryRegistrationStatus status{
      DiscoveryRegistrationState::kStopped,
      DiscoveryError{DiscoveryErrorKind::kCallbackFailure, "handler failed"},
      true, false};
  auto registration{discoveryPublicRegistrationStatus(status)};
  EXPECT_EQ(registration.state,
            Dmn_DiscoveryRegistrationState::CallbackFailure);
  ASSERT_TRUE(registration.error);
  EXPECT_EQ(registration.error->kind, Dmn_DiscoveryErrorKind::CallbackFailure);
  status.m_explicit_stop = true;
  registration = discoveryPublicRegistrationStatus(status);
  EXPECT_EQ(registration.state, Dmn_DiscoveryRegistrationState::Stopped);
  ASSERT_TRUE(registration.error);
}

TEST(DmnDiscoveryPublicConversionTest, QueryRequiresExplicitValidFamilies) {
  Dmn_DiscoveryQuery query;
  query.service_type = "_ipp._tcp";
  auto families{discoveryQueryFamilies(query)};
  ASSERT_TRUE(families);
  EXPECT_EQ(*families, (std::array<bool, 2>{true, true}));
  query.address_families = {Dmn_DiscoveryAddressFamily::IPv6};
  families = discoveryQueryFamilies(query);
  ASSERT_TRUE(families);
  EXPECT_EQ(*families, (std::array<bool, 2>{false, true}));
  query.address_families.clear();
  EXPECT_FALSE(discoveryQueryFamilies(query));
  query.address_families = {Dmn_DiscoveryAddressFamily::IPv4,
                            Dmn_DiscoveryAddressFamily::IPv4};
  EXPECT_FALSE(discoveryQueryFamilies(query));
  query.address_families = {Dmn_DiscoveryAddressFamily::IPv4};
  query.interface_index = 0;
  EXPECT_FALSE(discoveryQueryFamilies(query));
}

TEST(DmnDiscoveryPublicConversionTest, MapsLifecycleErrorsWithoutParsingText) {
  for (const auto &[internal, external] :
       {std::pair{DiscoveryErrorKind::kStopped,
                  Dmn_DiscoveryErrorKind::Stopped},
        std::pair{DiscoveryErrorKind::kReentrantCall,
                  Dmn_DiscoveryErrorKind::ReentrantCall},
        std::pair{DiscoveryErrorKind::kCancelled,
                  Dmn_DiscoveryErrorKind::Cancelled},
        std::pair{DiscoveryErrorKind::kCallbackFailure,
                  Dmn_DiscoveryErrorKind::CallbackFailure}}) {
    EXPECT_EQ(discoveryPublicError({internal, "same detail"},
                                   Dmn_DiscoveryErrorKind::Internal)
                  .kind,
              external);
  }
}

TEST(DmnDiscoveryPublicConversionTest, PreservesNativeAttemptError) {
  DiscoveryCandidate candidate;
  candidate.m_failures.push_back({{1, DiscoveryAttempt::kIpv4},
                                  {false, "denied", "avahi", -20,
                                   DiscoveryNativeErrorCategory::kPermission}});
  const auto converted{discoveryPublicCandidate(candidate, {})};
  ASSERT_TRUE(converted);
  ASSERT_EQ(converted->failures.size(), 1);
  const auto &error{converted->failures.front().error};
  EXPECT_EQ(error.kind, Dmn_DiscoveryErrorKind::Permission);
  EXPECT_EQ(error.backend_domain, "avahi");
  EXPECT_EQ(error.backend_code, -20);
  EXPECT_FALSE(converted->failures.front().recoverable);
}
} // namespace
