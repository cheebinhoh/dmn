/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-discovery-bonjour.cpp
 * @brief Controlled Bonjour socket and ownership tests.
 */

#include "dmn-discovery-bonjour-address.hpp"
#include "dmn-discovery-bonjour-browser.hpp"
#include "dmn-discovery-bonjour-contribution.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"
#include "dmn-discovery-bonjour-poll.hpp"
#include "dmn-discovery-bonjour-record.hpp"
#include "dmn-discovery-bonjour-registration.hpp"
#include "dmn-discovery-bonjour-sessions.hpp"
#include "dmn-discovery-records.hpp"
#include "dmn-discovery.hpp"

#include <dns_sd.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <unistd.h>
#include <utility>

struct _DNSServiceRef_t {
  int m_fd;
  int m_result;
  int m_processed;
  int m_deallocated;
  DNSServiceBrowseReply m_callback{};
  void *m_context{};
  DNSServiceFlags m_flags{};
  uint32_t m_interface{};
  DNSServiceErrorType m_callback_error{};
  const char *m_instance{};
  const char *m_type{};
  const char *m_domain{};
  DNSServiceQueryRecordReply m_record_callback{};
  uint16_t m_rrtype{};
  uint16_t m_rrclass{};
  uint16_t m_rdlen{};
  const void *m_rdata{};
  DNSServiceGetAddrInfoReply m_address_callback{};
  const struct sockaddr *m_address{};
  DNSServiceRegisterReply m_register_callback{};
};

namespace {
DNSServiceRef g_next_browse_ref{};
std::vector<DNSServiceRef> g_browse_refs;
std::size_t g_browse_ref_index{};
std::atomic<DNSServiceErrorType> g_browse_error{};
std::atomic<unsigned> g_browse_calls{};
std::atomic<bool> g_browse_immediate{};
std::promise<void> *g_second_browse_call{};
std::function<void(DNSServiceRef)> g_after_browse_callback;
uint32_t g_browse_interface{};
std::string g_browse_type;
std::string g_browse_domain;
DNSServiceRef g_next_record_ref{};
DNSServiceErrorType g_record_error{};
std::vector<DNSServiceRef> g_record_refs;
std::size_t g_record_ref_index{};
std::promise<void> *g_record_started{};
uint32_t g_record_interface{};
std::string g_record_name;
uint16_t g_record_type{};
uint16_t g_record_class{};
DNSServiceRef g_next_address_ref{};
std::promise<void> *g_address_started{};
DNSServiceRef g_deallocation_ref{};
std::promise<void> *g_deallocation_event{};
DNSServiceProtocol g_address_protocol{};
uint32_t g_address_interface{};
std::string g_address_name;
DNSServiceRef g_next_register_ref{};
std::vector<DNSServiceRef> g_register_refs;
std::size_t g_register_ref_index{};
std::atomic<DNSServiceErrorType> g_register_error{};
std::atomic<unsigned> g_register_calls{};
std::atomic<bool> g_register_immediate{};
std::promise<void> *g_register_started{};
std::promise<void> *g_second_register_call{};
DNSServiceFlags g_register_flags{};
uint32_t g_register_interface{};
uint16_t g_register_port{};
std::vector<std::uint8_t> g_register_txt;
} // namespace

extern "C" {
DNSServiceErrorType DNSServiceBrowse(DNSServiceRef *ref, DNSServiceFlags,
                                     uint32_t interfaceIndex,
                                     const char *regtype, const char *domain,
                                     DNSServiceBrowseReply callback,
                                     void *context) {
  g_browse_interface = interfaceIndex;
  g_browse_type = regtype;
  g_browse_domain = domain;
  const auto count{++g_browse_calls};
  if (g_second_browse_call && count == 2) {
    g_second_browse_call->set_value();
  }
  if (const auto error{g_browse_error.load()}; error != 0) {
    return error;
  }

  *ref = g_browse_ref_index < g_browse_refs.size()
             ? g_browse_refs[g_browse_ref_index++]
             : g_next_browse_ref;
  (*ref)->m_callback = callback;
  (*ref)->m_context = context;
  if (g_browse_immediate.load()) {
    callback(*ref, kDNSServiceFlagsAdd, interfaceIndex, 0, "Printer", regtype,
             domain, context);
  }

  return 0;
}
DNSServiceErrorType DNSServiceQueryRecord(DNSServiceRef *ref, DNSServiceFlags,
                                          uint32_t interfaceIndex,
                                          const char *fullname, uint16_t rrtype,
                                          uint16_t rrclass,
                                          DNSServiceQueryRecordReply callback,
                                          void *context) {
  g_record_interface = interfaceIndex;
  g_record_name = fullname;
  g_record_type = rrtype;
  g_record_class = rrclass;
  if (g_record_error != 0) {
    return g_record_error;
  }

  *ref = g_record_ref_index < g_record_refs.size()
             ? g_record_refs[g_record_ref_index++]
             : g_next_record_ref;
  (*ref)->m_record_callback = callback;
  (*ref)->m_context = context;
  if (g_record_started && g_record_ref_index == 2) {
    g_record_started->set_value();
  }

  return 0;
}
DNSServiceErrorType DNSServiceConstructFullName(char *fullName,
                                                const char *service,
                                                const char *regtype,
                                                const char *domain) {
  std::string escaped;
  for (const char *current{service}; *current; ++current) {
    if (*current == '.' || *current == '\\') {
      escaped.push_back('\\');
    }

    escaped.push_back(*current);
  }

  const auto count{std::snprintf(fullName, kDNSServiceMaxDomainName, "%s.%s.%s",
                                 escaped.c_str(), regtype, domain)};

  return count < 0 || count >= kDNSServiceMaxDomainName ? -1 : 0;
}
DNSServiceErrorType DNSServiceGetAddrInfo(DNSServiceRef *ref, DNSServiceFlags,
                                          uint32_t interfaceIndex,
                                          DNSServiceProtocol protocol,
                                          const char *hostname,
                                          DNSServiceGetAddrInfoReply callback,
                                          void *context) {
  g_address_interface = interfaceIndex;
  g_address_protocol = protocol;
  g_address_name = hostname;
  *ref = g_next_address_ref;
  (*ref)->m_address_callback = callback;
  (*ref)->m_context = context;
  if (g_address_started) {
    g_address_started->set_value();
  }

  return 0;
}
DNSServiceErrorType
DNSServiceRegister(DNSServiceRef *ref, DNSServiceFlags flags,
                   uint32_t interfaceIndex, const char *name, const char *type,
                   const char *domain, const char *host, uint16_t port,
                   uint16_t size, const void *data,
                   DNSServiceRegisterReply callback, void *context) {
  g_register_flags = flags;
  g_register_interface = interfaceIndex;
  g_register_port = port;
  g_register_txt.clear();
  if (size != 0) {
    const auto *bytes{static_cast<const std::uint8_t *>(data)};
    g_register_txt.assign(bytes, bytes + size);
  }

  EXPECT_EQ(host, nullptr);
  const auto count{++g_register_calls};
  if (g_second_register_call && count == 2) {
    g_second_register_call->set_value();
  }

  if (const auto error{g_register_error.load()}; error != 0) {
    return error;
  }

  *ref = g_register_ref_index < g_register_refs.size()
             ? g_register_refs[g_register_ref_index++]
             : g_next_register_ref;
  (*ref)->m_register_callback = callback;
  (*ref)->m_context = context;
  if (g_register_immediate.load()) {
    callback(*ref, kDNSServiceFlagsAdd, 0, name, type, domain, context);
  }

  if (g_register_started) {
    g_register_started->set_value();
  }

  return 0;
}
int DNSServiceRefSockFD(DNSServiceRef ref) { return ref->m_fd; }
DNSServiceErrorType DNSServiceProcessResult(DNSServiceRef ref) {
  ++ref->m_processed;
  if (ref->m_callback) {
    char byte{};
    EXPECT_EQ(read(ref->m_fd, &byte, 1), 1);
    ref->m_callback(ref, ref->m_flags, ref->m_interface, ref->m_callback_error,
                    ref->m_instance, ref->m_type, ref->m_domain,
                    ref->m_context);
    if (g_after_browse_callback) {
      g_after_browse_callback(ref);
    }
  } else if (ref->m_record_callback) {
    char byte{};
    EXPECT_EQ(read(ref->m_fd, &byte, 1), 1);
    ref->m_record_callback(ref, ref->m_flags, ref->m_interface,
                           ref->m_callback_error, ref->m_instance,
                           ref->m_rrtype, ref->m_rrclass, ref->m_rdlen,
                           ref->m_rdata, 120, ref->m_context);
  } else if (ref->m_address_callback) {
    char byte{};
    EXPECT_EQ(read(ref->m_fd, &byte, 1), 1);
    ref->m_address_callback(ref, ref->m_flags, ref->m_interface,
                            ref->m_callback_error, ref->m_instance,
                            ref->m_address, 120, ref->m_context);
  } else if (ref->m_register_callback) {
    char byte{};
    EXPECT_EQ(read(ref->m_fd, &byte, 1), 1);
    ref->m_register_callback(ref, ref->m_flags, ref->m_callback_error,
                             ref->m_instance, ref->m_type, ref->m_domain,
                             ref->m_context);
  }

  return ref->m_result;
}
void DNSServiceRefDeallocate(DNSServiceRef ref) {
  ++ref->m_deallocated;
  if (ref == g_deallocation_ref && g_deallocation_event) {
    g_deallocation_event->set_value();
  }
}
}

namespace dmn::detail {
struct DiscoveryBonjourRef {
  int m_read;
  int m_write;
};
} // namespace dmn::detail

namespace {
using namespace dmn::detail;

TEST(DmnDiscoveryBonjourTest, NativeAdapterUsesDnsServiceReferenceOperations) {
  _DNSServiceRef_t native{17, -42, 0, 0};
  auto api{makeDiscoveryBonjourApi()};
  auto *ref{reinterpret_cast<DiscoveryBonjourRef *>(&native)};
  EXPECT_EQ(api->socketFd(ref), 17);
  EXPECT_EQ(api->processResult(ref), -42);
  api->deallocate(ref);
  EXPECT_EQ(native.m_processed, 1);
  EXPECT_EQ(native.m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, NativeErrorsDistinguishPolicyAndRecovery) {
  const auto policy{
      discoveryBonjourError("browse failed", kDNSServiceErr_PolicyDenied)};
  EXPECT_EQ(policy.m_native_category,
            DiscoveryNativeErrorCategory::kPermission);
  const auto unavailable{
      discoveryBonjourError("browse failed", kDNSServiceErr_ServiceNotRunning)};
  EXPECT_EQ(unavailable.m_native_category,
            DiscoveryNativeErrorCategory::kUnavailable);
  const auto allocation{
      discoveryBonjourError("browse failed", kDNSServiceErr_NoMemory)};
  EXPECT_EQ(allocation.m_kind, DiscoveryErrorKind::kResourceLimit);
}

TEST(DmnDiscoveryBonjourTest, RegistrationPreservesWireValuesAndCollision) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  DiscoveryAdvertisement advertisement{
      {"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'a', 0, 'b'}, {}, {'x'}}};
  std::vector<DiscoveryBonjourRegistrationEvent> events;
  DiscoveryBonjourRegistration registration{
      poller, advertisement, DiscoveryOptions{},
      [&](DiscoveryBonjourRegistrationEvent event) -> DiscoveryResult {
        events.push_back(event);

        return {};
      }};
  ASSERT_TRUE(registration.start());
  EXPECT_NE(g_register_flags & kDNSServiceFlagsNoAutoRename, 0u);
  EXPECT_EQ(g_register_interface, 7u);
  EXPECT_EQ(g_register_port, htons(8080));
  EXPECT_EQ(g_register_txt,
            (std::vector<std::uint8_t>{3, 'a', 0, 'b', 0, 1, 'x'}));
  ref.m_flags = kDNSServiceFlagsAdd;
  ref.m_instance = "Printer";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ref.m_callback_error = kDNSServiceErr_NameConflict;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0], DiscoveryBonjourRegistrationEvent::kEstablished);
  EXPECT_EQ(events[1], DiscoveryBonjourRegistrationEvent::kCollision);
  ASSERT_TRUE(registration.stop());
  ASSERT_TRUE(registration.stop());
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationReportsImmediateCollision) {
  g_register_error = kDNSServiceErr_NameConflict;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  DiscoveryAdvertisement advertisement{
      {"Printer", "_ipp._tcp", "local.", 7}, 8080, {}};
  std::vector<DiscoveryBonjourRegistrationEvent> events;
  DiscoveryBonjourRegistration registration{
      poller, advertisement, DiscoveryOptions{},
      [&](DiscoveryBonjourRegistrationEvent event) -> DiscoveryResult {
        events.push_back(event);

        return {};
      }};
  ASSERT_TRUE(registration.start());
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0], DiscoveryBonjourRegistrationEvent::kCollision);
  ASSERT_TRUE(registration.stop());
  ASSERT_TRUE(poller->stop());
  g_register_error = 0;
}

TEST(DmnDiscoveryBonjourTest, RegistrationRejectsUnsupportedAndOversizedData) {
  DiscoveryOptions options;
  DiscoveryAdvertisement advertisement{
      {"Printer", "_ipp._tcp", "local.", 7}, 8080, {}};
  advertisement.m_key.m_domain = "example.com.";
  auto unsupported{
      validateDiscoveryBonjourAdvertisement(options, advertisement)};
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error().m_kind, DiscoveryErrorKind::kUnsupported);
  advertisement.m_key.m_domain = "local.";
  options.m_max_txt_entries = 300;
  options.m_max_txt_bytes = 100000;
  advertisement.m_txt.assign(257, std::vector<std::uint8_t>(255, 'x'));
  auto oversized{validateDiscoveryBonjourAdvertisement(options, advertisement)};
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  advertisement.m_txt.clear();
  advertisement.m_key.m_interface = 0xffffffffu;
  auto invalid{validateDiscoveryBonjourAdvertisement(options, advertisement)};
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().m_kind, DiscoveryErrorKind::kInvalidArgument);
}

TEST(DmnDiscoveryBonjourTest, RegistrationRejectsUnexpectedRename) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  DiscoveryBonjourRegistration registration{
      poller,
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}},
      DiscoveryOptions{},
      [](DiscoveryBonjourRegistrationEvent) -> DiscoveryResult { return {}; }};
  ASSERT_TRUE(registration.start());
  ref.m_flags = kDNSServiceFlagsAdd;
  ref.m_instance = "Printer (2)";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  auto status{registration.status()};
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().m_kind, DiscoveryErrorKind::kBackend);
  ASSERT_TRUE(registration.stop());
  ASSERT_TRUE(poller->stop());
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationSessionEstablishesAndWithdraws) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  std::promise<void> established;
  std::promise<void> collision;
  auto establishedFuture{established.get_future()};
  auto collisionFuture{collision.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          established.set_value();
        } else if (status.m_state == DiscoveryRegistrationState::kCollision) {
          collision.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ref.m_flags = kDNSServiceFlagsAdd;
  ref.m_instance = "Printer";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_EQ(establishedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ref.m_callback_error = kDNSServiceErr_NameConflict;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_EQ(collisionFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationSessionHandlesImmediateCallback) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  g_register_immediate = true;
  std::promise<void> established;
  auto establishedFuture{established.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          established.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(establishedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  g_register_immediate = false;
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, PublicClientUsesBonjourRegistration) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  g_register_immediate = true;
  std::promise<void> established;
  auto future{established.get_future()};
  auto client{dmn::Dmn_DiscoveryClient::create()};
  ASSERT_TRUE(client);
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](dmn::Dmn_DiscoveryRegistrationStatus status) {
        if (status.state == dmn::Dmn_DiscoveryRegistrationState::Established) {
          established.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(future.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE((*registration)->stop());
  ASSERT_TRUE((*client)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  g_register_immediate = false;
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, PublicClientUsesBonjourBrowser) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_browse_ref = &ref;
  g_browse_error = 0;
  auto client{dmn::Dmn_DiscoveryClient::create()};
  ASSERT_TRUE(client);
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7},
                                 [](dmn::Dmn_DiscoveryEvent) {})};
  ASSERT_TRUE(browser);
  ASSERT_TRUE((*browser)->snapshot());
  ASSERT_TRUE((*browser)->stop());
  ASSERT_TRUE((*client)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationSessionRecoversDaemonLoss) {
  int fds[2]{};
  int browserFds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  ASSERT_EQ(pipe(browserFds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_next_browse_ref = &browserRef;
  g_browse_error = 0;
  g_register_error = kDNSServiceErr_ServiceNotRunning;
  std::atomic<int> seconds{0};
  std::promise<void> unavailable;
  std::promise<void> started;
  std::promise<void> established;
  auto unavailableFuture{unavailable.get_future()};
  auto startedFuture{started.get_future()};
  auto establishedFuture{established.get_future()};
  g_register_started = &started;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto browser{
      (*owner)->browse({"_ipp._tcp", "local.", 7}, [](DiscoveryDelivery) {})};
  ASSERT_TRUE(browser);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kUnavailable) {
          g_register_error = 0;
          unavailable.set_value();
        } else if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          established.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  g_browse_error = kDNSServiceErr_PolicyDenied;
  seconds = 1;
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(startedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ref.m_flags = kDNSServiceFlagsAdd;
  ref.m_instance = "Printer";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_EQ(establishedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(browser->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  g_register_started = nullptr;
  g_register_error = 0;
  g_browse_error = 0;
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
  ASSERT_EQ(close(browserFds[0]), 0);
  ASSERT_EQ(close(browserFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, DaemonLossPropagatesBetweenSessionKinds) {
  for (const bool registrationFails : {false, true}) {
    int browserFds[2]{};
    int registrationFds[2]{};
    ASSERT_EQ(pipe(browserFds), 0);
    ASSERT_EQ(pipe(registrationFds), 0);
    _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
    _DNSServiceRef_t registrationRef{registrationFds[0], 0, 0, 0};
    g_next_browse_ref = &browserRef;
    g_next_register_ref = &registrationRef;
    g_browse_error = 0;
    g_register_error = 0;
    std::promise<void> browserLost;
    std::promise<void> registrationLost;
    auto browserFuture{browserLost.get_future()};
    auto registrationFuture{registrationLost.get_future()};
    auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [] {
      return std::chrono::steady_clock::time_point{};
    })};
    ASSERT_TRUE(owner);
    auto browser{(*owner)->browse(
        {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
          if (event.m_control == DiscoveryControl::kBackendUnavailable) {
            browserLost.set_value();
          }
        })};
    auto registration{(*owner)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
        [&](DiscoveryRegistrationStatus status) {
          if (status.m_state == DiscoveryRegistrationState::kUnavailable) {
            registrationLost.set_value();
          }
        })};
    ASSERT_TRUE(browser);
    ASSERT_TRUE(registration);
    auto &failed{registrationFails ? registrationRef : browserRef};
    failed.m_callback_error = kDNSServiceErr_ServiceNotRunning;
    ASSERT_EQ(
        write(registrationFails ? registrationFds[1] : browserFds[1], "x", 1),
        1);
    ASSERT_EQ(browserFuture.wait_for(std::chrono::seconds{3}),
              std::future_status::ready);
    ASSERT_EQ(registrationFuture.wait_for(std::chrono::seconds{3}),
              std::future_status::ready);
    ASSERT_TRUE(registration->stop());
    ASSERT_TRUE(browser->stop());
    ASSERT_TRUE((*owner)->shutdown());
    EXPECT_EQ(browserRef.m_deallocated, 1);
    EXPECT_EQ(registrationRef.m_deallocated, 1);
    ASSERT_EQ(close(browserFds[0]), 0);
    ASSERT_EQ(close(browserFds[1]), 0);
    ASSERT_EQ(close(registrationFds[0]), 0);
    ASSERT_EQ(close(registrationFds[1]), 0);
  }
}

TEST(DmnDiscoveryBonjourTest, RegistrationRetriesWithBoundedBackoff) {
  int browserFds[2]{};
  int registrationFds[2]{};
  ASSERT_EQ(pipe(browserFds), 0);
  ASSERT_EQ(pipe(registrationFds), 0);
  _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
  _DNSServiceRef_t registrationRef{registrationFds[0], 0, 0, 0};
  g_next_browse_ref = &browserRef;
  g_next_register_ref = &registrationRef;
  g_browse_error = 0;
  g_register_error = kDNSServiceErr_ServiceNotRunning;
  g_register_calls = 0;
  std::atomic<int> seconds{0};
  std::atomic<unsigned> losses{0};
  std::promise<void> firstLost;
  std::promise<void> secondAttempt;
  std::promise<void> recovered;
  auto firstFuture{firstLost.get_future()};
  auto secondFuture{secondAttempt.get_future()};
  auto recoveredFuture{recovered.get_future()};
  g_second_register_call = &secondAttempt;
  g_register_started = &recovered;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto browser{
      (*owner)->browse({"_ipp._tcp", "local.", 7}, [](DiscoveryDelivery) {})};
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kUnavailable &&
            ++losses == 1) {
          firstLost.set_value();
        }
      })};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration);
  ASSERT_EQ(firstFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  EXPECT_EQ(g_register_calls.load(), 1);
  seconds = 1;
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(secondFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  EXPECT_EQ(g_register_calls.load(), 2);
  g_register_error = 0;
  seconds = 3;
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(recoveredFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(browser->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(registrationRef.m_deallocated, 1);
  g_second_register_call = nullptr;
  g_register_started = nullptr;
  ASSERT_EQ(close(browserFds[0]), 0);
  ASSERT_EQ(close(browserFds[1]), 0);
  ASSERT_EQ(close(registrationFds[0]), 0);
  ASSERT_EQ(close(registrationFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, EstablishedRegistrationRecoversAfterDaemonLoss) {
  int browserFds[2]{};
  int firstFds[2]{};
  int secondFds[2]{};
  ASSERT_EQ(pipe(browserFds), 0);
  ASSERT_EQ(pipe(firstFds), 0);
  ASSERT_EQ(pipe(secondFds), 0);
  _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
  _DNSServiceRef_t first{firstFds[0], 0, 0, 0};
  _DNSServiceRef_t second{secondFds[0], 0, 0, 0};
  g_next_browse_ref = &browserRef;
  g_next_register_ref = &first;
  g_browse_error = 0;
  g_register_error = 0;
  std::atomic<int> seconds{0};
  std::atomic<unsigned> establishments{0};
  std::promise<void> initial;
  std::promise<void> unavailable;
  std::promise<void> restarted;
  std::promise<void> recovered;
  auto initialFuture{initial.get_future()};
  auto unavailableFuture{unavailable.get_future()};
  auto restartedFuture{restarted.get_future()};
  auto recoveredFuture{recovered.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto browser{
      (*owner)->browse({"_ipp._tcp", "local.", 7}, [](DiscoveryDelivery) {})};
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kUnavailable) {
          unavailable.set_value();
        } else if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          if (++establishments == 1) {
            initial.set_value();
          } else {
            recovered.set_value();
          }
        }
      })};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration);
  first.m_flags = kDNSServiceFlagsAdd;
  first.m_instance = "Printer";
  first.m_type = "_ipp._tcp";
  first.m_domain = "local.";
  ASSERT_EQ(write(firstFds[1], "x", 1), 1);
  ASSERT_EQ(initialFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  const auto oldGeneration{registration->generation()};
  first.m_callback_error = kDNSServiceErr_ServiceNotRunning;
  ASSERT_EQ(write(firstFds[1], "x", 1), 1);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  EXPECT_GT(registration->generation(), oldGeneration);
  g_next_register_ref = &second;
  g_register_started = &restarted;
  g_browse_error = kDNSServiceErr_PolicyDenied;
  seconds = 1;
  EXPECT_FALSE(browser->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(restartedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  second.m_flags = kDNSServiceFlagsAdd;
  second.m_instance = "Printer";
  second.m_type = "_ipp._tcp";
  second.m_domain = "local.";
  ASSERT_EQ(write(secondFds[1], "x", 1), 1);
  ASSERT_EQ(recoveredFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(browser->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(first.m_deallocated, 1);
  EXPECT_EQ(second.m_deallocated, 1);
  g_register_started = nullptr;
  g_browse_error = 0;
  ASSERT_EQ(close(browserFds[0]), 0);
  ASSERT_EQ(close(browserFds[1]), 0);
  ASSERT_EQ(close(firstFds[0]), 0);
  ASSERT_EQ(close(firstFds[1]), 0);
  ASSERT_EQ(close(secondFds[0]), 0);
  ASSERT_EQ(close(secondFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationPermissionFailureIsTerminal) {
  g_register_error = kDNSServiceErr_PolicyDenied;
  g_register_calls = 0;
  std::promise<void> failed;
  auto failedFuture{failed.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kFailed) {
          failed.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(failedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kFailed);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(g_register_calls.load(), 1);
  g_register_error = 0;
}

TEST(DmnDiscoveryBonjourTest, RegistrationReportsNativeSlotExhaustion) {
  int browserFds[2]{};
  int registrationFds[2]{};
  ASSERT_EQ(pipe(browserFds), 0);
  ASSERT_EQ(pipe(registrationFds), 0);
  _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
  _DNSServiceRef_t registrationRef{registrationFds[0], 0, 0, 0};
  g_next_browse_ref = &browserRef;
  g_next_register_ref = &registrationRef;
  g_browse_error = 0;
  g_register_error = 0;
  DiscoveryOptions options;
  options.m_max_native_operations = 1;
  std::promise<DiscoveryRegistrationStatus> failed;
  auto failedFuture{failed.get_future()};
  auto owner{DiscoveryBonjourSessions::create(options)};
  ASSERT_TRUE(owner);
  auto browser{
      (*owner)->browse({"_ipp._tcp", "local.", 7}, [](DiscoveryDelivery) {})};
  ASSERT_TRUE(browser);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kFailed) {
          failed.set_value(std::move(status));
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(failedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  const auto status{failedFuture.get()};
  ASSERT_TRUE(status.m_error);
  EXPECT_EQ(status.m_error->m_kind, DiscoveryErrorKind::kResourceLimit);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(browser->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(registrationRef.m_deallocated, 1);
  EXPECT_EQ(browserRef.m_deallocated, 1);
  ASSERT_EQ(close(browserFds[0]), 0);
  ASSERT_EQ(close(browserFds[1]), 0);
  ASSERT_EQ(close(registrationFds[0]), 0);
  ASSERT_EQ(close(registrationFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, StoppingOneRegistrationKeepsAnotherActive) {
  int firstFds[2]{};
  int secondFds[2]{};
  ASSERT_EQ(pipe(firstFds), 0);
  ASSERT_EQ(pipe(secondFds), 0);
  _DNSServiceRef_t first{firstFds[0], 0, 0, 0};
  _DNSServiceRef_t second{secondFds[0], 0, 0, 0};
  g_register_refs = {&first, &second};
  g_register_ref_index = 0;
  g_register_error = 0;
  std::promise<void> established;
  auto future{established.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto firstRegistration{
      (*owner)->advertise({{"First", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
                          [](DiscoveryRegistrationStatus) {})};
  auto secondRegistration{(*owner)->advertise(
      {{"Second", "_ipp._tcp", "local.", 7}, 8081, {{'y'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          established.set_value();
        }
      })};
  ASSERT_TRUE(firstRegistration);
  ASSERT_TRUE(secondRegistration);
  ASSERT_TRUE(firstRegistration->stop());
  EXPECT_EQ(first.m_deallocated, 1);
  second.m_flags = kDNSServiceFlagsAdd;
  second.m_instance = "Second";
  second.m_type = "_ipp._tcp";
  second.m_domain = "local.";
  ASSERT_EQ(write(secondFds[1], "x", 1), 1);
  ASSERT_EQ(future.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(secondRegistration->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(second.m_deallocated, 1);
  g_register_refs.clear();
  ASSERT_EQ(close(firstFds[0]), 0);
  ASSERT_EQ(close(firstFds[1]), 0);
  ASSERT_EQ(close(secondFds[0]), 0);
  ASSERT_EQ(close(secondFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RegistrationStopCancelsDaemonRetry) {
  g_register_error = kDNSServiceErr_ServiceNotRunning;
  g_register_calls = 0;
  std::promise<void> unavailable;
  auto unavailableFuture{unavailable.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kUnavailable) {
          unavailable.set_value();
        }
      })};
  ASSERT_TRUE(registration);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(g_register_calls.load(), 1);
  g_register_error = 0;
}

TEST(DmnDiscoveryBonjourTest, RegistrationStopDrainsInFlightCallback) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_register_ref = &ref;
  g_register_error = 0;
  std::promise<void> callbackStarted;
  std::promise<void> releaseCallback;
  std::promise<void> nativeStopped;
  auto callbackFuture{callbackStarted.get_future()};
  auto releaseFuture{releaseCallback.get_future().share()};
  auto nativeFuture{nativeStopped.get_future()};
  g_deallocation_ref = &ref;
  g_deallocation_event = &nativeStopped;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          callbackStarted.set_value();
          releaseFuture.wait();
        }
      })};
  ASSERT_TRUE(registration);
  ref.m_flags = kDNSServiceFlagsAdd;
  ref.m_instance = "Printer";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_EQ(callbackFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  auto stopped{
      std::async(std::launch::async, [&] { return registration->stop(); })};
  EXPECT_EQ(nativeFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds{0}),
            std::future_status::timeout);
  releaseCallback.set_value();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(stopped.get());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  g_deallocation_ref = nullptr;
  g_deallocation_event = nullptr;
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, DaemonLossDoesNotReviveCollision) {
  int browserFds[2]{};
  ASSERT_EQ(pipe(browserFds), 0);
  _DNSServiceRef_t browserRef{browserFds[0], 0, 0, 0};
  g_next_browse_ref = &browserRef;
  g_browse_error = 0;
  g_register_error = kDNSServiceErr_NameConflict;
  g_register_calls = 0;
  std::promise<void> collision;
  std::promise<void> browserLost;
  auto collisionFuture{collision.get_future()};
  auto browserFuture{browserLost.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [] {
    return std::chrono::steady_clock::time_point{};
  })};
  ASSERT_TRUE(owner);
  auto browser{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          browserLost.set_value();
        }
      })};
  auto registration{(*owner)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'x'}}},
      [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kCollision) {
          collision.set_value();
        }
      })};
  ASSERT_TRUE(browser);
  ASSERT_TRUE(registration);
  ASSERT_EQ(collisionFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  browserRef.m_callback_error = kDNSServiceErr_ServiceNotRunning;
  ASSERT_EQ(write(browserFds[1], "x", 1), 1);
  ASSERT_EQ(browserFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(registration->status().m_state,
            DiscoveryRegistrationState::kCollision);
  ASSERT_TRUE(registration->stop());
  ASSERT_TRUE(browser->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(g_register_calls.load(), 1);
  g_register_error = 0;
  ASSERT_EQ(close(browserFds[0]), 0);
  ASSERT_EQ(close(browserFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseMapsInterfaceAndAddRemove) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_browse_ref = &native;
  g_browse_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  std::vector<DiscoveryBonjourBrowseEvent> events;
  DiscoveryBonjourBrowser browser{
      poller,
      {"_example._tcp", "local.", 7},
      [&](DiscoveryBonjourBrowseEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));

        return {};
      }};
  ASSERT_TRUE(browser.start());
  EXPECT_EQ(g_browse_interface, 7);
  EXPECT_EQ(g_browse_type, "_example._tcp");
  EXPECT_EQ(g_browse_domain, "local.");
  native.m_instance = "literal.dot";
  native.m_type = "_example._tcp";
  native.m_domain = "local.";
  native.m_interface = 7;
  native.m_flags = kDNSServiceFlagsAdd | kDNSServiceFlagsMoreComing;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  native.m_flags = 0;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_EQ(events.size(), 2);
  EXPECT_TRUE(events[0].m_added);
  EXPECT_FALSE(events[1].m_added);
  EXPECT_EQ(events[0].m_key.m_instance, "literal.dot");
  EXPECT_EQ(events[0].m_key.m_interface, 7);
  EXPECT_EQ(events[0].m_contribution, DiscoveryContribution::kUnknown);
  ASSERT_TRUE(browser.stop());
  native.m_callback(&native, kDNSServiceFlagsAdd, 7, 0, "late", "_example._tcp",
                    "local.", native.m_context);
  EXPECT_EQ(events.size(), 2);
  ASSERT_TRUE(browser.stop());
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSupportsUnknownInterfaceAndTrailingDot) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_browse_ref = &native;
  g_browse_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  std::optional<DiscoveryBonjourBrowseEvent> event;
  DiscoveryBonjourBrowser browser{
      poller,
      {"_example._tcp", "local.", {}},
      [&](DiscoveryBonjourBrowseEvent next) -> DiscoveryResult {
        event = std::move(next);

        return {};
      }};
  ASSERT_TRUE(browser.start());
  EXPECT_EQ(g_browse_interface, 0);
  native.m_instance = "service";
  native.m_type = "_EXAMPLE._tcp.";
  native.m_domain = "LOCAL.";
  native.m_interface = 0;
  native.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_TRUE(event);
  EXPECT_FALSE(event->m_key.m_interface);
  EXPECT_EQ(event->m_key.m_type, "_EXAMPLE._tcp");
  ASSERT_TRUE(browser.stop());
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowsePollFailureRetiresReference) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], -42, 0, 0};
  g_next_browse_ref = &native;
  g_browse_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  std::optional<DiscoveryError> failure;
  DiscoveryBonjourBrowser browser{
      poller,
      {"_example._tcp", "local.", {}},
      [](DiscoveryBonjourBrowseEvent) -> DiscoveryResult { return {}; },
      [&](DiscoveryError error) { failure = std::move(error); }};
  ASSERT_TRUE(browser.start());
  native.m_instance = "service";
  native.m_type = "_example._tcp";
  native.m_domain = "local.";
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->m_backend_code, -42);
  EXPECT_FALSE(browser.status());
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_TRUE(browser.stop());
  ASSERT_TRUE(poller->stop());
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseAdmissionFailureReleasesNativeReference) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t occupied{fds[0], 0, 0, 0};
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_browse_ref = &native;
  g_browse_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  ASSERT_TRUE(
      poller->adopt(reinterpret_cast<DiscoveryBonjourRef *>(&occupied), {}));
  DiscoveryBonjourBrowser browser{
      poller,
      {"_example._tcp", "local.", {}},
      [](DiscoveryBonjourBrowseEvent) -> DiscoveryResult { return {}; }};
  auto result{browser.start()};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_TRUE(browser.stop());
  ASSERT_TRUE(poller->stop());
  EXPECT_EQ(occupied.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseRejectsUnexpectedInterface) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_browse_ref = &native;
  g_browse_error = 0;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  DiscoveryBonjourBrowser browser{
      poller,
      {"_example._tcp", "local.", 7},
      [](DiscoveryBonjourBrowseEvent) -> DiscoveryResult { return {}; }};
  ASSERT_TRUE(browser.start());
  native.m_instance = "service";
  native.m_type = "_example._tcp";
  native.m_domain = "local.";
  native.m_interface = 8;
  native.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  EXPECT_FALSE(browser.status());
  ASSERT_TRUE(browser.stop());
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, SrvWatchReportsScopedAddAndRemoveInWireOrder) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_record_ref = &native;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  std::vector<DiscoveryBonjourRecordEvent> events;
  DiscoveryBonjourRecordWatch watch{
      poller,
      "service._example._tcp.local.",
      7,
      DiscoveryRecordKind::kSrv,
      64,
      4096,
      [&](DiscoveryBonjourRecordEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));

        return {};
      }};
  ASSERT_TRUE(watch.start());
  EXPECT_EQ(g_record_interface, 7);
  EXPECT_EQ(g_record_type, kDNSServiceType_SRV);
  EXPECT_EQ(g_record_class, kDNSServiceClass_IN);
  EXPECT_EQ(g_record_name, "service._example._tcp.local.");
  const std::vector<std::uint8_t> srv{0,   0,   0,   0,   0x1f, 0x90,
                                      4,   'h', 'o', 's', 't',  5,
                                      'l', 'o', 'c', 'a', 'l',  0};
  native.m_instance = "SERVICE._example._tcp.local.";
  native.m_interface = 7;
  native.m_flags = kDNSServiceFlagsAdd;
  native.m_rrtype = kDNSServiceType_SRV;
  native.m_rrclass = kDNSServiceClass_IN;
  native.m_rdlen = srv.size();
  native.m_rdata = srv.data();
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  native.m_flags = 0;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_EQ(events.size(), 2);
  EXPECT_TRUE(events[0].m_added);
  EXPECT_FALSE(events[1].m_added);
  EXPECT_EQ(events[0].m_wire, srv);
  auto parsed{parseSrvRecord(events[0].m_wire)};
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->m_port, 8080);
  ASSERT_TRUE(watch.stop());
  ASSERT_TRUE(poller->stop());
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, TxtWatchRejectsMalformedWire) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_record_ref = &native;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  int updates{0};
  DiscoveryBonjourRecordWatch watch{
      poller,
      "service._example._tcp.local.",
      7,
      DiscoveryRecordKind::kTxt,
      64,
      4096,
      [&](DiscoveryBonjourRecordEvent) -> DiscoveryResult {
        ++updates;

        return {};
      }};
  ASSERT_TRUE(watch.start());
  const std::uint8_t malformed[]{2, 'x'};
  native.m_instance = "service._example._tcp.local.";
  native.m_interface = 7;
  native.m_flags = kDNSServiceFlagsAdd;
  native.m_rrtype = kDNSServiceType_TXT;
  native.m_rrclass = kDNSServiceClass_IN;
  native.m_rdlen = sizeof(malformed);
  native.m_rdata = malformed;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  EXPECT_FALSE(watch.status());
  EXPECT_EQ(updates, 0);
  ASSERT_TRUE(watch.stop());
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, AddressLookupUsesSrvTargetWithoutTxt) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_address_ref = &native;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  std::vector<DiscoveryBonjourAddressEvent> events;
  DiscoveryBonjourAddressLookup lookup{
      poller, "host.local.", 7, DiscoveryAddressFamily::kIpv4,
      [&](DiscoveryBonjourAddressEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));

        return {};
      }};
  ASSERT_TRUE(lookup.start());
  EXPECT_EQ(g_address_interface, 7);
  EXPECT_EQ(g_address_name, "host.local.");
  EXPECT_EQ(g_address_protocol, kDNSServiceProtocol_IPv4);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  const std::uint8_t ip[]{192, 0, 2, 1};
  std::memcpy(&address.sin_addr, ip, sizeof(ip));
  native.m_instance = "HOST.local.";
  native.m_interface = 7;
  native.m_flags = kDNSServiceFlagsAdd;
  native.m_address = reinterpret_cast<const sockaddr *>(&address);
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  native.m_flags = 0;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  ASSERT_EQ(events.size(), 2);
  EXPECT_TRUE(events[0].m_added);
  EXPECT_FALSE(events[1].m_added);
  EXPECT_EQ(events[0].m_address.m_bytes,
            std::vector<std::uint8_t>(ip, ip + sizeof(ip)));
  EXPECT_EQ(events[0].m_address.m_interface, 7);
  ASSERT_TRUE(lookup.stop());
  ASSERT_TRUE(poller->stop());
  EXPECT_EQ(native.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, AddressLookupRejectsWrongInterface) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t native{fds[0], 0, 0, 0};
  g_next_address_ref = &native;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 1)};
  ASSERT_TRUE(poller->start());
  DiscoveryBonjourAddressLookup lookup{
      poller, "host.local.", 7, DiscoveryAddressFamily::kIpv6,
      [](DiscoveryBonjourAddressEvent) -> DiscoveryResult { return {}; }};
  ASSERT_TRUE(lookup.start());
  EXPECT_EQ(g_address_protocol, kDNSServiceProtocol_IPv6);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  native.m_instance = "host.local.";
  native.m_interface = 8;
  native.m_flags = kDNSServiceFlagsAdd;
  native.m_address = reinterpret_cast<const sockaddr *>(&address);
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_TRUE(poller->wait(false));
  EXPECT_FALSE(lookup.status());
  ASSERT_TRUE(lookup.stop());
  ASSERT_TRUE(poller->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, ContributionSurfacesImmediateAllocationFailure) {
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 4)};
  DiscoveryOptions options;
  DiscoverySessionClient client{options, true};
  auto session{client.browse(nullptr)};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kUnknown)};
  ASSERT_TRUE(source);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    if (auto result{poller->start()}; !result) {
      return result;
    }

    DiscoveryBonjourContribution contribution{*session, epoch,   key,
                                              *source,  options, poller};
    g_record_error = kDNSServiceErr_NoMemory;
    auto result{contribution.start()};
    g_record_error = 0;
    EXPECT_FALSE(result);
    if (!result) {
      EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
    }

    EXPECT_TRUE(contribution.cleanup());

    return poller->stop();
  }));
  ASSERT_TRUE(session->stop());
  EXPECT_EQ(session->snapshot().m_native_operations, 0);
  ASSERT_TRUE(client.shutdown());
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionObservesAndWithdrawsService) {
  int browseFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  int addressFds[2]{};
  ASSERT_EQ(pipe(browseFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  ASSERT_EQ(pipe(addressFds), 0);
  _DNSServiceRef_t browseRef{browseFds[0], 0, 0, 0};
  _DNSServiceRef_t srvRef{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txtRef{txtFds[0], 0, 0, 0};
  _DNSServiceRef_t addressRef{addressFds[0], 0, 0, 0};
  g_next_browse_ref = &browseRef;
  g_browse_error = 0;
  g_record_refs = {&srvRef, &txtRef};
  g_record_ref_index = 0;
  g_next_address_ref = &addressRef;
  std::promise<void> added;
  std::promise<void> removed;
  std::promise<void> watchesStarted;
  std::promise<void> addressStarted;
  std::promise<void> resolved;
  std::atomic<bool> resolvedOnce{false};
  auto addedFuture{added.get_future()};
  auto removedFuture{removed.get_future()};
  auto watchesFuture{watchesStarted.get_future()};
  auto addressFuture{addressStarted.get_future()};
  auto resolvedFuture{resolved.get_future()};
  g_record_started = &watchesStarted;
  g_address_started = &addressStarted;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7},
      [&](DiscoveryDelivery event) {
        if (event.m_data &&
            event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          added.set_value();
        } else if (event.m_data &&
                   event.m_data->m_kind == DiscoveryEventKind::kRemoved) {
          removed.set_value();
        } else if (event.m_data &&
                   !event.m_data->m_candidate.m_records.m_addresses.empty() &&
                   !resolvedOnce.exchange(true)) {
          resolved.set_value();
        }
      },
      {true, false})};
  ASSERT_TRUE(session);
  browseRef.m_instance = "Printer";
  browseRef.m_type = "_ipp._tcp";
  browseRef.m_domain = "local.";
  browseRef.m_interface = 7;
  browseRef.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(addedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(watchesFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  const std::vector<std::uint8_t> srvWire{0,   0,   0,   0,   0x1f, 0x90,
                                          4,   'h', 'o', 's', 't',  5,
                                          'l', 'o', 'c', 'a', 'l',  0};
  srvRef.m_instance = "Printer._ipp._tcp.local.";
  srvRef.m_rrtype = kDNSServiceType_SRV;
  srvRef.m_rrclass = kDNSServiceClass_IN;
  srvRef.m_rdlen = srvWire.size();
  srvRef.m_rdata = srvWire.data();
  srvRef.m_interface = 7;
  srvRef.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(srvFds[1], "x", 1), 1);
  ASSERT_EQ(addressFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  const std::uint8_t ip[]{192, 0, 2, 1};
  std::memcpy(&address.sin_addr, ip, sizeof(ip));
  addressRef.m_instance = "host.local.";
  addressRef.m_interface = 7;
  addressRef.m_flags = kDNSServiceFlagsAdd;
  addressRef.m_address = reinterpret_cast<const sockaddr *>(&address);
  ASSERT_EQ(write(addressFds[1], "x", 1), 1);
  ASSERT_EQ(resolvedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  browseRef.m_flags = 0;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(removedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->stop());
  auto shutdown{(*owner)->shutdown()};
  ASSERT_TRUE(shutdown) << shutdown.error().m_message;
  EXPECT_EQ(browseRef.m_deallocated, 1);
  EXPECT_EQ(srvRef.m_deallocated, 1);
  EXPECT_EQ(txtRef.m_deallocated, 1);
  EXPECT_EQ(addressRef.m_deallocated, 1);
  g_record_started = nullptr;
  g_address_started = nullptr;
  g_record_refs.clear();
  ASSERT_EQ(close(browseFds[0]), 0);
  ASSERT_EQ(close(browseFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
  ASSERT_EQ(close(addressFds[0]), 0);
  ASSERT_EQ(close(addressFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionResetReplacesNativeReferences) {
  int firstFds[2]{};
  int secondFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  int newSrvFds[2]{};
  int newTxtFds[2]{};
  ASSERT_EQ(pipe(firstFds), 0);
  ASSERT_EQ(pipe(secondFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  ASSERT_EQ(pipe(newSrvFds), 0);
  ASSERT_EQ(pipe(newTxtFds), 0);
  _DNSServiceRef_t first{firstFds[0], 0, 0, 0};
  _DNSServiceRef_t second{secondFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  _DNSServiceRef_t newSrv{newSrvFds[0], 0, 0, 0};
  _DNSServiceRef_t newTxt{newTxtFds[0], 0, 0, 0};
  g_browse_refs = {&first, &second};
  g_browse_ref_index = 0;
  g_browse_error = 0;
  g_record_refs = {&srv, &txt, &newSrv, &newTxt};
  g_record_ref_index = 0;
  std::promise<void> watchesStarted;
  auto watchesFuture{watchesStarted.get_future()};
  g_record_started = &watchesStarted;
  std::promise<void> added;
  auto addedFuture{added.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse({"_ipp._tcp", "local.", 7},
                                [&](DiscoveryDelivery event) {
                                  if (event.m_data &&
                                      event.m_data->m_kind ==
                                          DiscoveryEventKind::kAdded) {
                                    added.set_value();
                                  }
                                },
                                {true, false})};
  ASSERT_TRUE(session);
  first.m_instance = "Printer";
  first.m_type = "_ipp._tcp";
  first.m_domain = "local.";
  first.m_interface = 7;
  first.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(firstFds[1], "x", 1), 1);
  ASSERT_EQ(addedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(watchesFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->clearCache());
  EXPECT_TRUE(session->snapshot().m_browser.m_candidates.empty());
  EXPECT_EQ(first.m_deallocated, 1);
  EXPECT_EQ(srv.m_deallocated, 1);
  EXPECT_EQ(txt.m_deallocated, 1);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(second.m_deallocated, 1);
  g_record_started = nullptr;
  g_browse_refs.clear();
  g_record_refs.clear();
  ASSERT_EQ(close(firstFds[0]), 0);
  ASSERT_EQ(close(firstFds[1]), 0);
  ASSERT_EQ(close(secondFds[0]), 0);
  ASSERT_EQ(close(secondFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
  ASSERT_EQ(close(newSrvFds[0]), 0);
  ASSERT_EQ(close(newSrvFds[1]), 0);
  ASSERT_EQ(close(newTxtFds[0]), 0);
  ASSERT_EQ(close(newTxtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionHandlesRemoveAndReaddInOneBatch) {
  int browseFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  int nextSrvFds[2]{};
  int nextTxtFds[2]{};
  ASSERT_EQ(pipe(browseFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  ASSERT_EQ(pipe(nextSrvFds), 0);
  ASSERT_EQ(pipe(nextTxtFds), 0);
  _DNSServiceRef_t browseRef{browseFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  _DNSServiceRef_t nextSrv{nextSrvFds[0], 0, 0, 0};
  _DNSServiceRef_t nextTxt{nextTxtFds[0], 0, 0, 0};
  g_next_browse_ref = &browseRef;
  g_browse_error = 0;
  g_record_refs = {&srv, &txt, &nextSrv, &nextTxt};
  g_record_ref_index = 0;
  std::atomic<bool> readd{false};
  g_after_browse_callback = [&](DNSServiceRef ref) {
    if (readd.exchange(false)) {
      ref->m_callback(ref, kDNSServiceFlagsAdd, ref->m_interface, 0,
                      ref->m_instance, ref->m_type, ref->m_domain,
                      ref->m_context);
    }
  };
  std::promise<void> initial;
  std::promise<void> again;
  std::promise<void> removed;
  std::promise<void> watchesStarted;
  auto initialFuture{initial.get_future()};
  auto againFuture{again.get_future()};
  auto removedFuture{removed.get_future()};
  auto watchesFuture{watchesStarted.get_future()};
  g_record_started = &watchesStarted;
  int addedCount{0};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_data &&
            event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          if (++addedCount == 1) {
            initial.set_value();
          } else if (addedCount == 2) {
            again.set_value();
          }
        } else if (event.m_data &&
                   event.m_data->m_kind == DiscoveryEventKind::kRemoved) {
          removed.set_value();
        }
      })};
  ASSERT_TRUE(session);
  browseRef.m_instance = "Printer";
  browseRef.m_type = "_ipp._tcp";
  browseRef.m_domain = "local.";
  browseRef.m_interface = 7;
  browseRef.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(initialFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(watchesFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  browseRef.m_flags = 0;
  readd = true;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(removedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(againFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(srv.m_deallocated, 1);
  EXPECT_EQ(txt.m_deallocated, 1);
  g_after_browse_callback = {};
  g_record_started = nullptr;
  g_record_refs.clear();
  ASSERT_EQ(close(browseFds[0]), 0);
  ASSERT_EQ(close(browseFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
  ASSERT_EQ(close(nextSrvFds[0]), 0);
  ASSERT_EQ(close(nextSrvFds[1]), 0);
  ASSERT_EQ(close(nextTxtFds[0]), 0);
  ASSERT_EQ(close(nextTxtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionRecoversUnavailableDaemon) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_browse_ref = &ref;
  g_browse_error = kDNSServiceErr_ServiceNotRunning;
  std::promise<void> unavailable;
  std::promise<void> available;
  std::atomic<int> seconds{0};
  auto unavailableFuture{unavailable.get_future()};
  auto availableFuture{available.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          g_browse_error = 0;
          unavailable.set_value();
        } else if (event.m_control == DiscoveryControl::kBackendAvailable) {
          available.set_value();
        }
      })};
  ASSERT_TRUE(session);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  seconds = 1;
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(availableFuture.wait_for(std::chrono::seconds{4}),
            std::future_status::ready);
  EXPECT_TRUE(session->snapshot().m_backend_available);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionDoesNotRetryPermissionFailure) {
  g_browse_error = kDNSServiceErr_PolicyDenied;
  g_browse_calls = 0;
  std::atomic<int> seconds{0};
  std::promise<void> unavailable;
  auto unavailableFuture{unavailable.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          unavailable.set_value();
        }
      })};
  ASSERT_TRUE(session);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  const auto status{session->snapshot()};
  ASSERT_TRUE(status.m_cleanup_error);
  EXPECT_EQ(status.m_cleanup_error->m_native_category,
            DiscoveryNativeErrorCategory::kPermission);
  seconds = 3600;
  ASSERT_TRUE(session->clearCache());
  EXPECT_EQ(g_browse_calls.load(), 1);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  g_browse_error = 0;
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionBacksOffRepeatedDaemonFailures) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_browse_ref = &ref;
  g_browse_error = kDNSServiceErr_ServiceNotRunning;
  g_browse_calls = 0;
  std::atomic<int> seconds{0};
  std::promise<void> firstLost;
  std::promise<void> secondAttempt;
  std::promise<void> recovered;
  auto firstFuture{firstLost.get_future()};
  auto secondFuture{secondAttempt.get_future()};
  auto recoveredFuture{recovered.get_future()};
  g_second_browse_call = &secondAttempt;
  std::atomic<int> recoveries{0};
  std::atomic<int> losses{0};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          if (++losses == 1) {
            firstLost.set_value();
          }
        } else if (event.m_control == DiscoveryControl::kBackendAvailable) {
          if (++recoveries == 1) {
            recovered.set_value();
          }
        }
      })};
  ASSERT_TRUE(session);
  ASSERT_EQ(firstFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(g_browse_calls.load(), 1);
  seconds = 1;
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(secondFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  EXPECT_EQ(g_browse_calls.load(), 2);
  EXPECT_EQ(recoveries.load(), 0);
  seconds = 2;
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  EXPECT_EQ(g_browse_calls.load(), 2);
  g_browse_error = 0;
  seconds = 3;
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(recoveredFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(g_browse_calls.load(), 3);
  EXPECT_EQ(recoveries.load(), 1);
  EXPECT_EQ(losses.load(), 1);
  g_second_browse_call = nullptr;
  EXPECT_EQ(ref.m_deallocated, 1);
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, RecoveryAcceptsImmediateNativeBrowseCallback) {
  int browseFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  ASSERT_EQ(pipe(browseFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  _DNSServiceRef_t browseRef{browseFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  g_next_browse_ref = &browseRef;
  g_record_refs = {&srv, &txt};
  g_record_ref_index = 0;
  g_browse_error = kDNSServiceErr_ServiceNotRunning;
  std::atomic<int> seconds{0};
  std::promise<void> unavailable;
  std::promise<void> added;
  std::promise<void> removed;
  auto unavailableFuture{unavailable.get_future()};
  auto addedFuture{added.get_future()};
  auto removedFuture{removed.get_future()};
  std::vector<DiscoveryControl> controls;
  bool availableBeforeAdded{false};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control) {
          controls.push_back(*event.m_control);
          if (*event.m_control == DiscoveryControl::kBackendUnavailable) {
            unavailable.set_value();
          }
        } else if (event.m_data &&
                   event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          availableBeforeAdded =
              std::find(controls.begin(), controls.end(),
                        DiscoveryControl::kBackendAvailable) != controls.end();
          added.set_value();
        } else if (event.m_data &&
                   event.m_data->m_kind == DiscoveryEventKind::kRemoved) {
          removed.set_value();
        }
      })};
  ASSERT_TRUE(session);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  g_browse_immediate = true;
  g_browse_error = 0;
  seconds = 1;
  EXPECT_FALSE(session->refresh({"Missing", "_ipp._tcp", "local.", 7}));
  ASSERT_EQ(addedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_TRUE(availableBeforeAdded);
  browseRef.m_instance = "Printer";
  browseRef.m_type = "_ipp._tcp";
  browseRef.m_domain = "local.";
  browseRef.m_interface = 7;
  browseRef.m_flags = 0;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(removedFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(browseRef.m_deallocated, 1);
  g_browse_immediate = false;
  g_record_refs.clear();
  ASSERT_EQ(close(browseFds[0]), 0);
  ASSERT_EQ(close(browseFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionStopCancelsDaemonRetry) {
  g_browse_error = kDNSServiceErr_ServiceNotRunning;
  g_browse_calls = 0;
  std::atomic<int> seconds{0};
  std::promise<void> unavailable;
  auto unavailableFuture{unavailable.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [&] {
    return std::chrono::steady_clock::time_point{} +
           std::chrono::seconds{seconds.load()};
  })};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          unavailable.set_value();
        }
      })};
  ASSERT_TRUE(session);
  ASSERT_EQ(unavailableFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(session->stop());
  seconds = 60;
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(g_browse_calls.load(), 1);
  g_browse_error = 0;
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionStopDrainsInFlightCallback) {
  int browseFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  ASSERT_EQ(pipe(browseFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  _DNSServiceRef_t browseRef{browseFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  g_next_browse_ref = &browseRef;
  g_browse_error = 0;
  g_record_refs = {&srv, &txt};
  g_record_ref_index = 0;
  std::promise<void> callbackStarted;
  std::promise<void> releaseCallback;
  std::promise<void> nativeStopped;
  auto callbackFuture{callbackStarted.get_future()};
  auto releaseFuture{releaseCallback.get_future().share()};
  auto nativeFuture{nativeStopped.get_future()};
  g_deallocation_ref = &browseRef;
  g_deallocation_event = &nativeStopped;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_data &&
            event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          callbackStarted.set_value();
          releaseFuture.wait();
        }
      })};
  ASSERT_TRUE(session);
  browseRef.m_instance = "Printer";
  browseRef.m_type = "_ipp._tcp";
  browseRef.m_domain = "local.";
  browseRef.m_interface = 7;
  browseRef.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(callbackFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  auto stopped{std::async(std::launch::async, [&] { return session->stop(); })};
  EXPECT_EQ(nativeFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds{0}),
            std::future_status::timeout);
  releaseCallback.set_value();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(stopped.get());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(browseRef.m_deallocated, 1);
  g_deallocation_ref = nullptr;
  g_deallocation_event = nullptr;
  g_record_refs.clear();
  ASSERT_EQ(close(browseFds[0]), 0);
  ASSERT_EQ(close(browseFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionResetDrainsOldCallback) {
  int firstFds[2]{};
  int secondFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  ASSERT_EQ(pipe(firstFds), 0);
  ASSERT_EQ(pipe(secondFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  _DNSServiceRef_t first{firstFds[0], 0, 0, 0};
  _DNSServiceRef_t second{secondFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  g_browse_refs = {&first, &second};
  g_browse_ref_index = 0;
  g_browse_error = 0;
  g_record_refs = {&srv, &txt};
  g_record_ref_index = 0;
  std::promise<void> callbackStarted;
  std::promise<void> releaseCallback;
  std::promise<void> nativeStopped;
  auto callbackFuture{callbackStarted.get_future()};
  auto releaseFuture{releaseCallback.get_future().share()};
  auto nativeFuture{nativeStopped.get_future()};
  g_deallocation_ref = &first;
  g_deallocation_event = &nativeStopped;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_data &&
            event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          callbackStarted.set_value();
          releaseFuture.wait();
        }
      })};
  ASSERT_TRUE(session);
  first.m_instance = "Printer";
  first.m_type = "_ipp._tcp";
  first.m_domain = "local.";
  first.m_interface = 7;
  first.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(firstFds[1], "x", 1), 1);
  ASSERT_EQ(callbackFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  auto reset{
      std::async(std::launch::async, [&] { return session->clearCache(); })};
  EXPECT_EQ(nativeFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(reset.wait_for(std::chrono::milliseconds{0}),
            std::future_status::timeout);
  releaseCallback.set_value();
  ASSERT_EQ(reset.wait_for(std::chrono::seconds{3}), std::future_status::ready);
  ASSERT_TRUE(reset.get());
  EXPECT_TRUE(session->snapshot().m_browser.m_candidates.empty());
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(first.m_deallocated, 1);
  EXPECT_EQ(second.m_deallocated, 1);
  g_deallocation_ref = nullptr;
  g_deallocation_event = nullptr;
  g_browse_refs.clear();
  g_record_refs.clear();
  ASSERT_EQ(close(firstFds[0]), 0);
  ASSERT_EQ(close(firstFds[1]), 0);
  ASSERT_EQ(close(secondFds[0]), 0);
  ASSERT_EQ(close(secondFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionOverflowReleasesNativeReferences) {
  int browseFds[2]{};
  int srvFds[2]{};
  int txtFds[2]{};
  ASSERT_EQ(pipe(browseFds), 0);
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  _DNSServiceRef_t browseRef{browseFds[0], 0, 0, 0};
  _DNSServiceRef_t srv{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txt{txtFds[0], 0, 0, 0};
  g_next_browse_ref = &browseRef;
  g_browse_error = 0;
  g_record_refs = {&srv, &txt};
  g_record_ref_index = 0;
  std::promise<void> initial;
  std::promise<void> watchesStarted;
  std::promise<void> overflow;
  auto initialFuture{initial.get_future()};
  auto watchesFuture{watchesStarted.get_future()};
  auto overflowFuture{overflow.get_future()};
  g_record_started = &watchesStarted;
  DiscoveryOptions options;
  options.m_max_instances = 1;
  auto owner{DiscoveryBonjourSessions::create(options)};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_data &&
            event.m_data->m_kind == DiscoveryEventKind::kAdded) {
          initial.set_value();
        } else if (event.m_control == DiscoveryControl::kOverflow) {
          overflow.set_value();
        }
      })};
  ASSERT_TRUE(session);
  browseRef.m_instance = "First";
  browseRef.m_type = "_ipp._tcp";
  browseRef.m_domain = "local.";
  browseRef.m_interface = 7;
  browseRef.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(initialFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(watchesFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  browseRef.m_instance = "Second";
  ASSERT_EQ(write(browseFds[1], "x", 1), 1);
  ASSERT_EQ(overflowFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(session->snapshot().m_delivery.m_state,
            DiscoveryDeliveryState::kOverflow);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(browseRef.m_deallocated, 1);
  EXPECT_EQ(srv.m_deallocated, 1);
  EXPECT_EQ(txt.m_deallocated, 1);
  g_record_started = nullptr;
  g_record_refs.clear();
  ASSERT_EQ(close(browseFds[0]), 0);
  ASSERT_EQ(close(browseFds[1]), 0);
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, BrowseSessionReportsNativeAllocationFailure) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_browse_ref = &ref;
  g_browse_error = 0;
  g_record_error = kDNSServiceErr_NoMemory;
  std::promise<void> overflow;
  auto overflowFuture{overflow.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kOverflow) {
          overflow.set_value();
        }
      })};
  ASSERT_TRUE(session);
  ref.m_instance = "Printer";
  ref.m_type = "_ipp._tcp";
  ref.m_domain = "local.";
  ref.m_interface = 7;
  ref.m_flags = kDNSServiceFlagsAdd;
  ASSERT_EQ(write(fds[1], "x", 1), 1);
  ASSERT_EQ(overflowFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  EXPECT_EQ(session->snapshot().m_delivery.m_state,
            DiscoveryDeliveryState::kOverflow);
  ASSERT_TRUE(session->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(ref.m_deallocated, 1);
  g_record_error = 0;
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, OwnerDestructionDrainsLiveBrowser) {
  int fds[2]{};
  ASSERT_EQ(pipe(fds), 0);
  _DNSServiceRef_t ref{fds[0], 0, 0, 0};
  g_next_browse_ref = &ref;
  g_browse_error = 0;
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{})};
  ASSERT_TRUE(owner);
  auto session{(*owner)->browse({"_ipp._tcp", "local.", 7}, nullptr)};
  ASSERT_TRUE(session);
  owner->reset();
  EXPECT_EQ(ref.m_deallocated, 1);
  EXPECT_EQ(session->snapshot().m_delivery.m_state,
            DiscoveryDeliveryState::kStopped);
  EXPECT_FALSE(session->snapshot().m_cleanup_error);
  ASSERT_TRUE(session->stop());
  ASSERT_EQ(close(fds[0]), 0);
  ASSERT_EQ(close(fds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest, DaemonLossInvalidatesAllBrowsers) {
  int firstFds[2]{};
  int secondFds[2]{};
  ASSERT_EQ(pipe(firstFds), 0);
  ASSERT_EQ(pipe(secondFds), 0);
  _DNSServiceRef_t first{firstFds[0], 0, 0, 0};
  _DNSServiceRef_t second{secondFds[0], 0, 0, 0};
  g_browse_refs = {&first, &second};
  g_browse_ref_index = 0;
  g_browse_error = 0;
  std::promise<void> firstLost;
  std::promise<void> secondLost;
  auto firstFuture{firstLost.get_future()};
  auto secondFuture{secondLost.get_future()};
  auto owner{DiscoveryBonjourSessions::create(DiscoveryOptions{}, [] {
    return std::chrono::steady_clock::time_point{};
  })};
  ASSERT_TRUE(owner);
  auto firstSession{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          firstLost.set_value();
        }
      })};
  auto secondSession{(*owner)->browse(
      {"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery event) {
        if (event.m_control == DiscoveryControl::kBackendUnavailable) {
          secondLost.set_value();
        }
      })};
  ASSERT_TRUE(firstSession);
  ASSERT_TRUE(secondSession);
  first.m_callback_error = kDNSServiceErr_ServiceNotRunning;
  ASSERT_EQ(write(firstFds[1], "x", 1), 1);
  ASSERT_EQ(firstFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_EQ(secondFuture.wait_for(std::chrono::seconds{3}),
            std::future_status::ready);
  ASSERT_TRUE(firstSession->stop());
  ASSERT_TRUE(secondSession->stop());
  ASSERT_TRUE((*owner)->shutdown());
  EXPECT_EQ(first.m_deallocated, 1);
  EXPECT_EQ(second.m_deallocated, 1);
  g_browse_refs.clear();
  ASSERT_EQ(close(firstFds[0]), 0);
  ASSERT_EQ(close(firstFds[1]), 0);
  ASSERT_EQ(close(secondFds[0]), 0);
  ASSERT_EQ(close(secondFds[1]), 0);
}

TEST(DmnDiscoveryBonjourTest,
     ContributionResolvesSrvWithoutTxtAndRejectsOldAddress) {
  int srvFds[2]{};
  int txtFds[2]{};
  int addressFds[2]{};
  int replacementFds[2]{};
  int targetFds[2]{};
  int refreshFds[2]{};
  int retryFds[2]{};
  ASSERT_EQ(pipe(srvFds), 0);
  ASSERT_EQ(pipe(txtFds), 0);
  ASSERT_EQ(pipe(addressFds), 0);
  ASSERT_EQ(pipe(replacementFds), 0);
  ASSERT_EQ(pipe(targetFds), 0);
  ASSERT_EQ(pipe(refreshFds), 0);
  ASSERT_EQ(pipe(retryFds), 0);
  _DNSServiceRef_t srvRef{srvFds[0], 0, 0, 0};
  _DNSServiceRef_t txtRef{txtFds[0], 0, 0, 0};
  _DNSServiceRef_t addressRef{addressFds[0], 0, 0, 0};
  _DNSServiceRef_t replacementRef{replacementFds[0], 0, 0, 0};
  _DNSServiceRef_t targetRef{targetFds[0], 0, 0, 0};
  _DNSServiceRef_t refreshRef{refreshFds[0], 0, 0, 0};
  _DNSServiceRef_t retryRef{retryFds[0], 0, 0, 0};
  g_record_refs = {&srvRef, &txtRef, &retryRef};
  g_record_ref_index = 0;
  g_next_address_ref = &addressRef;
  auto poller{
      std::make_shared<DiscoveryBonjourPoller>(makeDiscoveryBonjourApi(), 4)};
  DiscoveryOptions options;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoverySessionClient client{options, true, [&] { return now; }};
  auto session{client.browse(nullptr)};
  ASSERT_TRUE(session);
  const auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer.Office", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kUnknown)};
  ASSERT_TRUE(source);
  std::unique_ptr<DiscoveryBonjourContribution> contribution;
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    if (auto result{poller->start()}; !result) {
      return result;
    }

    contribution = std::make_unique<DiscoveryBonjourContribution>(
        *session, epoch, key, *source, options, poller,
        std::array<bool, 2>{true, false});

    return contribution->start();
  }));
  EXPECT_EQ(g_record_name, "Printer\\.Office._ipp._tcp.local.");
  const std::vector<std::uint8_t> srv{0,   0,   0,   0,   0x1f, 0x90,
                                      4,   'h', 'o', 's', 't',  5,
                                      'l', 'o', 'c', 'a', 'l',  0};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  const std::uint8_t ip[]{192, 0, 2, 1};
  std::memcpy(&address.sin_addr, ip, sizeof(ip));
  sockaddr_in secondAddress{address};
  const std::uint8_t secondIp[]{192, 0, 2, 2};
  std::memcpy(&secondAddress.sin_addr, secondIp, sizeof(secondIp));
  const std::vector<std::uint8_t> txt{3, 'a', 0, 'b'};
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    srvRef.m_record_callback(&srvRef, kDNSServiceFlagsAdd, 7, 0,
                             "Printer\\.Office._ipp._tcp.local.",
                             kDNSServiceType_SRV, kDNSServiceClass_IN,
                             srv.size(), srv.data(), 120, srvRef.m_context);
    auto result{contribution->drive()};
    if (!result) {
      return result;
    }

    EXPECT_EQ(g_address_name, "host.local.");
    EXPECT_NE(addressRef.m_address_callback, nullptr);
    addressRef.m_address_callback(&addressRef, kDNSServiceFlagsAdd, 7, 0,
                                  "host.local.",
                                  reinterpret_cast<const sockaddr *>(&address),
                                  120, addressRef.m_context);
    addressRef.m_address_callback(
        &addressRef, kDNSServiceFlagsAdd, 7, 0, "host.local.",
        reinterpret_cast<const sockaddr *>(&secondAddress), 120,
        addressRef.m_context);
    txtRef.m_record_callback(&txtRef, kDNSServiceFlagsAdd, 7, 0,
                             "Printer\\.Office._ipp._tcp.local.",
                             kDNSServiceType_TXT, kDNSServiceClass_IN,
                             txt.size(), txt.data(), 120, txtRef.m_context);

    return contribution->drive();
  }));
  auto snapshot{session->snapshot()};
  ASSERT_EQ(snapshot.m_browser.m_candidates.size(), 1);
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_addresses.size(), 2);
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_txt_wire, txt);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    addressRef.m_address_callback(&addressRef, 0, 7, 0, "host.local.",
                                  reinterpret_cast<const sockaddr *>(&address),
                                  120, addressRef.m_context);
    txtRef.m_record_callback(&txtRef, 0, 7, 0,
                             "Printer\\.Office._ipp._tcp.local.",
                             kDNSServiceType_TXT, kDNSServiceClass_IN,
                             txt.size(), txt.data(), 120, txtRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_addresses.size(), 1);
  EXPECT_FALSE(snapshot.m_browser.m_candidates[0].m_records.m_txt_wire);
  auto conflictingSrv{srv};
  conflictingSrv[5] = 0x91;
  conflictingSrv[7] = 'm';
  conflictingSrv[8] = 'o';
  conflictingSrv[9] = 'r';
  conflictingSrv[10] = 'e';
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    srvRef.m_record_callback(
        &srvRef, kDNSServiceFlagsAdd, 7, 0, "Printer\\.Office._ipp._tcp.local.",
        kDNSServiceType_SRV, kDNSServiceClass_IN, conflictingSrv.size(),
        conflictingSrv.data(), 120, srvRef.m_context);
    addressRef.m_address_callback(&addressRef, kDNSServiceFlagsAdd, 7, 0,
                                  "host.local.",
                                  reinterpret_cast<const sockaddr *>(&address),
                                  120, addressRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_state,
            DiscoveryRecordState::kMultipleRecordsUnsupported);
  EXPECT_TRUE(snapshot.m_browser.m_candidates[0].m_records.m_addresses.empty());
  EXPECT_EQ(addressRef.m_deallocated, 1);
  g_next_address_ref = &replacementRef;
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    srvRef.m_record_callback(
        &srvRef, 0, 7, 0, "Printer\\.Office._ipp._tcp.local.",
        kDNSServiceType_SRV, kDNSServiceClass_IN, conflictingSrv.size(),
        conflictingSrv.data(), 120, srvRef.m_context);
    auto result{contribution->drive()};
    if (!result) {
      return result;
    }

    EXPECT_NE(replacementRef.m_address_callback, nullptr);
    replacementRef.m_address_callback(
        &replacementRef, kDNSServiceFlagsAdd, 7, 0, "host.local.",
        reinterpret_cast<const sockaddr *>(&secondAddress), 120,
        replacementRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_addresses.size(), 1);
  g_next_address_ref = &targetRef;
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    srvRef.m_record_callback(&srvRef, 0, 7, 0,
                             "Printer\\.Office._ipp._tcp.local.",
                             kDNSServiceType_SRV, kDNSServiceClass_IN,
                             srv.size(), srv.data(), 120, srvRef.m_context);
    srvRef.m_record_callback(
        &srvRef, kDNSServiceFlagsAdd, 7, 0, "Printer\\.Office._ipp._tcp.local.",
        kDNSServiceType_SRV, kDNSServiceClass_IN, conflictingSrv.size(),
        conflictingSrv.data(), 120, srvRef.m_context);
    replacementRef.m_address_callback(
        &replacementRef, kDNSServiceFlagsAdd, 7, 0, "host.local.",
        reinterpret_cast<const sockaddr *>(&address), 120,
        replacementRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_TRUE(snapshot.m_browser.m_candidates[0].m_records.m_addresses.empty());
  EXPECT_EQ(replacementRef.m_deallocated, 1);
  EXPECT_EQ(g_address_name, "more.local.");
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    targetRef.m_address_callback(
        &targetRef, kDNSServiceFlagsAdd, 7, 0, "more.local.",
        reinterpret_cast<const sockaddr *>(&address), 120, targetRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_addresses.size(), 1);
  g_next_address_ref = &refreshRef;
  auto refresh{session->refresh(key)};
  ASSERT_TRUE(refresh);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    targetRef.m_address_callback(
        &targetRef, kDNSServiceFlagsAdd, 7, 0, "more.local.",
        reinterpret_cast<const sockaddr *>(&address), 120, targetRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_TRUE(snapshot.m_browser.m_candidates[0].m_records.m_addresses.empty());
  EXPECT_EQ(targetRef.m_deallocated, 1);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    refreshRef.m_address_callback(&refreshRef, kDNSServiceFlagsAdd, 7, 0,
                                  "more.local.",
                                  reinterpret_cast<const sockaddr *>(&address),
                                  120, refreshRef.m_context);

    return contribution->drive();
  }));
  snapshot = session->snapshot();
  EXPECT_EQ(snapshot.m_browser.m_candidates[0].m_records.m_addresses.size(), 1);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    srvRef.m_record_callback(&srvRef, 0, 7, kDNSServiceErr_ServiceNotRunning,
                             nullptr, 0, 0, 0, nullptr, 0, srvRef.m_context);
    auto failure{contribution->drive()};
    EXPECT_FALSE(failure);

    return {};
  }));
  snapshot = session->snapshot();
  ASSERT_EQ(snapshot.m_browser.m_candidates[0].m_failures.size(), 1);
  EXPECT_EQ(snapshot.m_browser.m_candidates[0]
                .m_failures[0]
                .m_error.m_native_category,
            DiscoveryNativeErrorCategory::kUnavailable);
  EXPECT_EQ(srvRef.m_deallocated, 1);
  now += std::chrono::seconds{1};
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    auto due{session->takeDueRetries(epoch)};
    if (!due) {
      return std::unexpected{due.error()};
    }

    EXPECT_EQ(due->size(), 1);
    if (due->size() != 1) {
      return std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kBackend, "retry not scheduled"}};
    }

    return contribution->retry(due->front());
  }));
  EXPECT_TRUE(session->snapshot().m_browser.m_candidates[0].m_failures.empty());
  EXPECT_NE(retryRef.m_record_callback, nullptr);
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    retryRef.m_record_callback(&retryRef, 0, 7, kDNSServiceErr_PolicyDenied,
                               nullptr, 0, 0, 0, nullptr, 0,
                               retryRef.m_context);
    auto failure{contribution->drive()};
    EXPECT_FALSE(failure);

    return {};
  }));
  snapshot = session->snapshot();
  ASSERT_EQ(snapshot.m_browser.m_candidates[0].m_failures.size(), 1);
  EXPECT_EQ(snapshot.m_browser.m_candidates[0]
                .m_failures[0]
                .m_error.m_native_category,
            DiscoveryNativeErrorCategory::kPermission);
  now += std::chrono::minutes{1};
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    auto due{session->takeDueRetries(epoch)};
    if (!due) {
      return std::unexpected{due.error()};
    }

    EXPECT_TRUE(due->empty());

    return {};
  }));
  ASSERT_TRUE(client.dispatchNative([&]() -> DiscoveryResult {
    auto result{contribution->cleanup()};
    contribution.reset();
    if (!result) {
      return result;
    }

    return poller->stop();
  }));
  EXPECT_EQ(session->snapshot().m_native_operations, 1);
  ASSERT_TRUE(session->stop());
  EXPECT_EQ(session->snapshot().m_native_operations, 0);
  ASSERT_TRUE(client.shutdown());
  g_record_refs.clear();
  ASSERT_EQ(close(srvFds[0]), 0);
  ASSERT_EQ(close(srvFds[1]), 0);
  ASSERT_EQ(close(txtFds[0]), 0);
  ASSERT_EQ(close(txtFds[1]), 0);
  ASSERT_EQ(close(addressFds[0]), 0);
  ASSERT_EQ(close(addressFds[1]), 0);
  ASSERT_EQ(close(replacementFds[0]), 0);
  ASSERT_EQ(close(replacementFds[1]), 0);
  ASSERT_EQ(close(targetFds[0]), 0);
  ASSERT_EQ(close(targetFds[1]), 0);
  ASSERT_EQ(close(refreshFds[0]), 0);
  ASSERT_EQ(close(refreshFds[1]), 0);
  ASSERT_EQ(close(retryFds[0]), 0);
  ASSERT_EQ(close(retryFds[1]), 0);
}

class FakeBonjour final : public DiscoveryBonjourApi {
public:
  int socketFd(DiscoveryBonjourRef *ref) override { return ref->m_read; }
  int processResult(DiscoveryBonjourRef *ref) override {
    char byte{};
    EXPECT_EQ(read(ref->m_read, &byte, 1), 1);
    ++m_processed;
    if (m_on_process) {
      m_on_process(ref);
    }

    return m_error;
  }
  void deallocate(DiscoveryBonjourRef *ref) noexcept override {
    ++m_deallocated;
    close(ref->m_read);
    if (ref->m_write != -1) {
      close(ref->m_write);
    }

    delete ref;
  }

  DiscoveryBonjourRef *create() {
    int fds[2]{};
    if (pipe(fds) != 0) {
      return nullptr;
    }

    return new DiscoveryBonjourRef{fds[0], fds[1]};
  }

  int m_error{0};
  unsigned m_processed{0};
  unsigned m_deallocated{0};
  std::function<void(DiscoveryBonjourRef *)> m_on_process;
};

TEST(DmnDiscoveryBonjourTest, ReadableReferenceIsProcessedAndReleasedOnce) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 2};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(poller.adopt(ref, {}));
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  ASSERT_TRUE(poller.wait(false));
  EXPECT_EQ(api->m_processed, 1);
  ASSERT_TRUE(poller.remove(ref));
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
  EXPECT_EQ(api->m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, RemovalDuringCallbackDefersDeallocation) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(poller.adopt(ref, {}));
  api->m_on_process = [&](DiscoveryBonjourRef *current) {
    EXPECT_TRUE(poller.remove(current));
    EXPECT_EQ(api->m_deallocated, 0);
  };
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  ASSERT_TRUE(poller.wait(false));
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
  EXPECT_EQ(api->m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, ProcessFailureIsReportedAndReferenceReleased) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  std::optional<DiscoveryError> observed;
  ASSERT_TRUE(poller.adopt(
      ref, [&](DiscoveryError error) { observed = std::move(error); }));
  api->m_error = -42;
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  ASSERT_TRUE(poller.wait(false));
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->m_backend_domain, "bonjour");
  EXPECT_EQ(observed->m_backend_code, -42);
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, ClosedNativeSocketReportsUnavailable) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  std::optional<DiscoveryError> observed;
  ASSERT_TRUE(poller.adopt(
      ref, [&](DiscoveryError error) { observed = std::move(error); }));
  ASSERT_EQ(close(ref->m_write), 0);
  ref->m_write = -1;
  ASSERT_TRUE(poller.wait(false));
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->m_native_category,
            DiscoveryNativeErrorCategory::kUnavailable);
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, ReferenceLimitRejectsWithoutTakingOwnership) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *first{api->create()};
  auto *second{api->create()};
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(poller.adopt(first, {}));
  const auto rejected{poller.adopt(second, {})};
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().m_kind, DiscoveryErrorKind::kResourceLimit);
  api->deallocate(second);
  ASSERT_TRUE(poller.stop());
  EXPECT_EQ(api->m_deallocated, 2);
}

TEST(DmnDiscoveryBonjourTest, WorkerWakeAndShutdownReleaseReferences) {
  auto api{std::make_shared<FakeBonjour>()};
  auto poller{std::make_shared<DiscoveryBonjourPoller>(api, 1)};
  DiscoveryCommandWorker worker{4, 1};
  ASSERT_TRUE(worker.attachPoller(poller));
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(worker.execute([&] { return poller->adopt(ref, {}); }));
  ASSERT_TRUE(worker.execute([] { return DiscoveryResult{}; }));
  ASSERT_TRUE(worker.shutdown());
  EXPECT_EQ(api->m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, NativeErrorHandlerRunsOnStateWorker) {
  auto api{std::make_shared<FakeBonjour>()};
  api->m_error = -42;
  auto poller{std::make_shared<DiscoveryBonjourPoller>(api, 1)};
  std::promise<bool> notified;
  auto notification{notified.get_future()};
  DiscoveryCommandWorker worker{4, 1};
  ASSERT_TRUE(worker.attachPoller(poller));
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(worker.execute([&] {
    return poller->adopt(ref, [&](DiscoveryError error) {
      notified.set_value(error.m_backend_code == -42 &&
                         worker.inWorkerContext());
    });
  }));
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  ASSERT_EQ(notification.wait_for(std::chrono::seconds{5}),
            std::future_status::ready);
  EXPECT_TRUE(notification.get());
  ASSERT_TRUE(worker.shutdown());
  EXPECT_EQ(api->m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, ClosedSocketReportsFailureAndCancelsReference) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  std::optional<DiscoveryError> observed;
  ASSERT_TRUE(poller.adopt(
      ref, [&](DiscoveryError error) { observed = std::move(error); }));
  ASSERT_EQ(close(ref->m_write), 0);
  ref->m_write = -1;
  ASSERT_TRUE(poller.wait(false));
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->m_backend_domain, "posix");
  EXPECT_EQ(observed->m_backend_code, EIO);
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.wait(false));
  ASSERT_TRUE(poller.stop());
  EXPECT_EQ(api->m_deallocated, 1);
}

TEST(DmnDiscoveryBonjourTest, UnhandledNativeErrorFailsPoller) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(poller.adopt(ref, {}));
  api->m_error = -42;
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  const auto result{poller.wait(false)};
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kBackend);
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, CallbackCannotReAdoptRetiredReference) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_TRUE(poller.adopt(ref, {}));
  api->m_on_process = [&](DiscoveryBonjourRef *current) {
    EXPECT_TRUE(poller.remove(current));
    EXPECT_FALSE(poller.adopt(current, {}));
  };
  ASSERT_EQ(write(ref->m_write, "x", 1), 1);
  ASSERT_TRUE(poller.wait(false));
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, CancellingReferenceKeepsItsSlotUntilReleased) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *first{api->create()};
  auto *second{api->create()};
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(poller.adopt(first, {}));
  api->m_on_process = [&](DiscoveryBonjourRef *current) {
    EXPECT_TRUE(poller.remove(current));
    auto result{poller.adopt(second, {})};
    EXPECT_FALSE(result);
    if (!result) {
      EXPECT_EQ(result.error().m_kind, DiscoveryErrorKind::kResourceLimit);
    }
  };
  ASSERT_EQ(write(first->m_write, "x", 1), 1);
  ASSERT_TRUE(poller.wait(false));
  EXPECT_EQ(api->m_deallocated, 1);
  ASSERT_TRUE(poller.adopt(second, {}));
  ASSERT_TRUE(poller.stop());
  EXPECT_EQ(api->m_deallocated, 2);
}

TEST(DmnDiscoveryBonjourTest, WakeInterruptsBlockingWait) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  std::promise<void> entered;
  auto ready{entered.get_future()};
  auto result{std::async(std::launch::async, [&] {
    entered.set_value();

    return poller.wait(true);
  })};
  ready.wait();
  poller.wake();
  EXPECT_EQ(result.wait_for(std::chrono::milliseconds{500}),
            std::future_status::ready);
  EXPECT_TRUE(result.get());
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, FullWakePipeDoesNotBlockOrFail) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  for (int count{0}; count < 100000; ++count) {
    poller.wake();
  }

  ASSERT_TRUE(poller.wait(false));
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, RejectsInvalidSocketWithoutTakingOwnership) {
  auto api{std::make_shared<FakeBonjour>()};
  DiscoveryBonjourPoller poller{api, 1};
  ASSERT_TRUE(poller.start());
  auto *ref{api->create()};
  ASSERT_NE(ref, nullptr);
  ASSERT_EQ(close(ref->m_read), 0);
  ref->m_read = -1;
  EXPECT_FALSE(poller.adopt(ref, {}));
  EXPECT_EQ(api->m_deallocated, 0);
  close(ref->m_write);
  delete ref;
  ASSERT_TRUE(poller.stop());
}

TEST(DmnDiscoveryBonjourTest, DueRetryDoesNotBlockPoll) {
  auto api{std::make_shared<FakeBonjour>()};
  using Poller = DiscoveryBonjourPoller;
  auto poller{std::make_shared<Poller>(
      api, 1, std::chrono::steady_clock::now, []() -> Poller::DriveResult {
        return Poller::Deadline{std::chrono::steady_clock::time_point::min()};
      })};
  ASSERT_TRUE(poller->start());
  auto result{
      std::async(std::launch::async, [&] { return poller->wait(true); })};
  EXPECT_EQ(result.wait_for(std::chrono::milliseconds{500}),
            std::future_status::ready);
  EXPECT_TRUE(result.get());
  ASSERT_TRUE(poller->stop());
}
} // namespace
