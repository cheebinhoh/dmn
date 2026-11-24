/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-records.cpp
 * @brief Deterministic DNS-SD record and address reconciliation tests.
 */

#include "dmn-discovery-records.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using dmn::detail::DiscoveryAddress;
using dmn::detail::DiscoveryAddressFamily;
using dmn::detail::DiscoveryRecordKind;
using dmn::detail::DiscoveryRecords;
using dmn::detail::DiscoveryRecordState;
using dmn::detail::DiscoverySrv;

TEST(DmnDiscoveryRecordsTest, ResolvesWithoutTxtAndReplacesSourceAddresses) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 3000, "host.local."}));

  const auto generation{records.snapshot().m_generation};
  DiscoveryAddress first{DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  DiscoveryAddress second{DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}};
  ASSERT_TRUE(records.replaceAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "HOST.local", 3000, first));
  ASSERT_EQ(records.snapshot().m_state, DiscoveryRecordState::kReady);
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{first}));

  ASSERT_TRUE(records.replaceAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 3000, second));
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{second}));
}

TEST(DmnDiscoveryRecordsTest, DeduplicatesRecordsAndRetainsOtherSources) {
  DiscoveryRecords records{4};
  for (const auto source : {1u, 2u}) {
    ASSERT_TRUE(records.addSource(source));
    ASSERT_TRUE(records.setMonitoring(source, DiscoveryRecordKind::kSrv, true));
    ASSERT_TRUE(records.setMonitoring(source, DiscoveryRecordKind::kTxt, true));
    ASSERT_TRUE(records.addSrv(
        source, *records.monitorGeneration(source, DiscoveryRecordKind::kSrv),
        {0, 0, 42, "Host.local."}));
  }

  auto generation{records.snapshot().m_generation};
  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {192, 168, 1, 3}, {}};
  ASSERT_TRUE(records.replaceAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, address));
  ASSERT_TRUE(records.replaceAddress(
      2, generation,
      *records.addressGeneration(2, DiscoveryAddressFamily::kIpv4),
      "HOST.LOCAL", 42, address));
  ASSERT_EQ(records.snapshot().m_addresses.size(), 1);
  ASSERT_TRUE(records.removeSource(1));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kReady);
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{address}));
}

TEST(DmnDiscoveryRecordsTest, ConflictsInvalidateOldResultsAndRecover) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  const DiscoverySrv original{0, 0, 1000, "one.local."};
  const DiscoverySrv replacement{0, 0, 2000, "two.local."};
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv), original));
  const auto oldGeneration{records.snapshot().m_generation};
  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {127, 0, 0, 1}, {}};
  ASSERT_TRUE(records.replaceAddress(
      1, oldGeneration,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "one.local.", 1000, address));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      replacement));
  EXPECT_EQ(records.snapshot().m_state,
            DiscoveryRecordState::kMultipleRecordsUnsupported);
  EXPECT_TRUE(records.snapshot().m_addresses.empty());
  ASSERT_TRUE(records.removeSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv), original));
  EXPECT_FALSE(records.replaceAddress(
      1, oldGeneration,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "one.local.", 1000, address));
  auto current{records.snapshot().m_generation};
  EXPECT_TRUE(records.replaceAddress(
      1, current, *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "two.local.", 2000, address));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kReady);
}

TEST(DmnDiscoveryRecordsTest, HandlesEmptyBinaryTxtAndMonitoringFailure) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 1, "host.local."}));
  ASSERT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt), {}));
  ASSERT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt), {0}));
  EXPECT_NE(records.snapshot().m_state,
            DiscoveryRecordState::kMultipleRecordsUnsupported);
  ASSERT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {3, 'x', '=', 0xff}));
  EXPECT_EQ(records.snapshot().m_state,
            DiscoveryRecordState::kMultipleRecordsUnsupported);
  ASSERT_TRUE(records.removeTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {3, 'x', '=', 0xff}));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, false));
  EXPECT_EQ(records.snapshot().m_state,
            DiscoveryRecordState::kMonitorIncomplete);
}

TEST(DmnDiscoveryRecordsTest, RejectsInvalidTargetsAndUnscopedLinkLocal) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 1, "."}));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kUnavailable);
  ASSERT_TRUE(records.removeSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 1, "."}));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 0, "host.local."}));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kUnavailable);
  ASSERT_TRUE(records.removeSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 0, "host.local."}));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 1, "host.local."}));

  const auto generation{records.snapshot().m_generation};
  DiscoveryAddress linkLocal{
      DiscoveryAddressFamily::kIpv6,
      {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
      std::nullopt};
  EXPECT_FALSE(records.replaceAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv6),
      "host.local.", 1, linkLocal));
  linkLocal.m_interface = 2;
  EXPECT_TRUE(records.replaceAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv6),
      "host.local.", 1, linkLocal));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kReady);
}

TEST(DmnDiscoveryRecordsTest, ParsesBoundedSrvAndTxtWireData) {
  const std::vector<std::uint8_t> srv{0,   1,   0,   2,   0x1f, 0x90,
                                      4,   'h', 'o', 's', 't',  5,
                                      'l', 'o', 'c', 'a', 'l',  0};
  auto parsed{dmn::detail::parseSrvRecord(srv)};
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->m_port, 8080);
  EXPECT_EQ(parsed->m_priority, 1);
  EXPECT_EQ(parsed->m_weight, 2);
  EXPECT_EQ(parsed->m_target, "host.local.");
  EXPECT_FALSE(dmn::detail::parseSrvRecord({0, 0, 0, 0, 0, 1, 0xc0, 0}));
  EXPECT_FALSE(dmn::detail::parseSrvRecord({0, 0, 0, 0, 0, 1, 4, 'x'}));
  EXPECT_FALSE(dmn::detail::parseTxtRecord({4, 'x', 'y'}, 4, 20));
  auto txt{dmn::detail::parseTxtRecord({0, 2, 0, 0xff}, 4, 20)};
  ASSERT_TRUE(txt);
  EXPECT_EQ(txt->size(), 2);
  EXPECT_EQ((*txt)[1], (std::vector<std::uint8_t>{0, 0xff}));
}

TEST(DmnDiscoveryRecordsTest, RemovesIndividualBonjourAddresses) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 42, "host.local."}));
  const auto generation{records.snapshot().m_generation};
  const DiscoveryAddress first{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  const DiscoveryAddress second{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}};

  ASSERT_TRUE(records.addAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, first));
  ASSERT_TRUE(records.addAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, second));
  ASSERT_EQ(records.snapshot().m_addresses.size(), 2);
  EXPECT_TRUE(records.removeAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4), first));
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{second}));
}

TEST(DmnDiscoveryRecordsTest, MonitoringFailureDiscardsThatSourcesRecords) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 42, "host.local."}));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, false));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  EXPECT_EQ(records.snapshot().m_state, DiscoveryRecordState::kUnresolved);
}

TEST(DmnDiscoveryRecordsTest, PreservesTheObservedEmptyTxtEncoding) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt), {}));
  ASSERT_TRUE(records.snapshot().m_txt_wire.has_value());
  EXPECT_TRUE(records.snapshot().m_txt_wire->empty());
}

TEST(DmnDiscoveryRecordsTest, EnforcesConfiguredAddressAndTxtLimits) {
  DiscoveryRecords records{4, 1, 1, 3};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 42, "host.local."}));
  const auto generation{records.snapshot().m_generation};
  const DiscoveryAddress first{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  const DiscoveryAddress second{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}};
  ASSERT_TRUE(records.addAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, first));
  const auto overflow{records.addAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, second)};
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().m_kind,
            dmn::detail::DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{first}));
  EXPECT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {2, 'a', 'b'}));
  EXPECT_FALSE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {1, 'a', 1, 'b'}));
}

TEST(DmnDiscoveryRecordsTest, MalformedRecordSuppressesEndpoint) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addSrv(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kSrv),
      {0, 0, 42, "host.local."}));
  const auto generation{records.snapshot().m_generation};
  const DiscoveryAddress address{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  ASSERT_TRUE(records.addAddress(
      1, generation,
      *records.addressGeneration(1, DiscoveryAddressFamily::kIpv4),
      "host.local.", 42, address));
  EXPECT_FALSE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt), {3, 'a'}));
  EXPECT_EQ(records.snapshot().m_state,
            DiscoveryRecordState::kMonitorIncomplete);
  EXPECT_TRUE(records.snapshot().m_addresses.empty());
}

TEST(DmnDiscoveryRecordsTest, BoundsAggregateTxtAcrossDistinctRecords) {
  DiscoveryRecords records{4, 4, 4, 5};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  ASSERT_TRUE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {2, 'a', 'b'}));
  EXPECT_FALSE(records.addTxt(
      1, *records.monitorGeneration(1, DiscoveryRecordKind::kTxt),
      {2, 'c', 'd'}));
  EXPECT_EQ(records.snapshot().m_state,
            DiscoveryRecordState::kMonitorIncomplete);
}

TEST(DmnDiscoveryRecordsTest, RejectsCallbacksFromPreviousRecordWatch) {
  DiscoveryRecords records{4};
  ASSERT_TRUE(records.addSource(1));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kTxt, true));
  const auto oldWatch{records.monitorGeneration(1, DiscoveryRecordKind::kSrv)};
  ASSERT_TRUE(oldWatch);
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, false));
  const auto stoppedGeneration{records.snapshot().m_generation};
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, false));
  EXPECT_EQ(records.snapshot().m_generation, stoppedGeneration);
  ASSERT_TRUE(records.setMonitoring(1, DiscoveryRecordKind::kSrv, true));
  const auto newWatch{records.monitorGeneration(1, DiscoveryRecordKind::kSrv)};
  ASSERT_TRUE(newWatch);
  ASSERT_NE(*oldWatch, *newWatch);
  EXPECT_FALSE(records.addSrv(1, *oldWatch, {0, 0, 42, "old.local."}));
  EXPECT_TRUE(records.addSrv(1, *newWatch, {0, 0, 43, "new.local."}));
  EXPECT_EQ(records.snapshot().m_srv->m_target, "new.local.");
}

TEST(DmnDiscoveryRecordsTest, FailedLookupInvalidatesOnlyItsOwnAddressToken) {
  DiscoveryRecords records{4};
  for (const auto source : {1u, 2u}) {
    ASSERT_TRUE(records.addSource(source));
    ASSERT_TRUE(records.setMonitoring(source, DiscoveryRecordKind::kSrv, true));
    ASSERT_TRUE(records.setMonitoring(source, DiscoveryRecordKind::kTxt, true));
    ASSERT_TRUE(records.addSrv(
        source, *records.monitorGeneration(source, DiscoveryRecordKind::kSrv),
        {0, 0, 42, "host.local."}));
  }
  const auto generation{records.snapshot().m_generation};
  const auto oldLookup{
      records.addressGeneration(1, DiscoveryAddressFamily::kIpv4)};
  const auto otherLookup{
      records.addressGeneration(2, DiscoveryAddressFamily::kIpv4)};
  ASSERT_TRUE(oldLookup);
  ASSERT_TRUE(otherLookup);
  const DiscoveryAddress first{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 1}, {}};
  const DiscoveryAddress second{
      DiscoveryAddressFamily::kIpv4, {10, 0, 0, 2}, {}};
  ASSERT_TRUE(
      records.addAddress(1, generation, *oldLookup, "host.local.", 42, first));
  ASSERT_TRUE(records.addAddress(2, generation, *otherLookup, "host.local.", 42,
                                 second));
  ASSERT_TRUE(
      records.failAddressLookup(1, DiscoveryAddressFamily::kIpv4, *oldLookup));
  EXPECT_EQ(records.snapshot().m_addresses, (std::vector{second}));
  EXPECT_FALSE(
      records.addAddress(1, generation, *oldLookup, "host.local.", 42, first));
  const auto retryLookup{
      records.addressGeneration(1, DiscoveryAddressFamily::kIpv4)};
  ASSERT_TRUE(retryLookup);
  EXPECT_NE(*retryLookup, *oldLookup);
  EXPECT_TRUE(records.addAddress(1, generation, *retryLookup, "host.local.", 42,
                                 first));
}

} // namespace
