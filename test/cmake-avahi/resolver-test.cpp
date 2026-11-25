/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file resolver-test.cpp
 * @brief Controlled tests for Avahi resolver arguments and lifecycle.
 */

#include "dmn-discovery-avahi-resolver.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct AvahiClient {};
struct AvahiServiceResolver {};

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

class FakeResolverApi final : public DiscoveryAvahiResolverApi {
public:
  AvahiServiceResolver *
  create(AvahiClient *, AvahiIfIndex interfaceIndex, AvahiProtocol protocol,
         const char *name, const char *type, const char *domain,
         AvahiProtocol addressProtocol, AvahiLookupFlags flags,
         AvahiServiceResolverCallback callback, void *context) override {
    m_interface = interfaceIndex;
    m_protocol = protocol;
    m_name = name;
    m_type = type;
    m_domain = domain;
    m_address_protocol = addressProtocol;
    m_flags = flags;
    m_callback = callback;
    m_context = context;
    if (m_fail) {
      return nullptr;
    }
    if (m_immediate) {
      emit(AVAHI_RESOLVER_FOUND);
    }
    return &m_resolver;
  }
  void free(AvahiServiceResolver *) noexcept override { ++m_freed; }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "fake native error"; }

  void emit(AvahiResolverEvent event) {
    AvahiAddress address{};
    address.proto = m_address_protocol;
    const std::array<std::uint8_t, 16> bytes{0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                             0,    0,    0, 0, 0, 0, 0, 1};
    std::memcpy(address.data.ipv6.address, bytes.data(), bytes.size());
    m_callback(&m_resolver, m_interface, m_protocol, event, m_name.c_str(),
               m_type.c_str(), m_domain.c_str(), "Host.local.", &address, 8080,
               nullptr, AVAHI_LOOKUP_RESULT_MULTICAST, m_context);
  }

  bool m_fail{false};
  bool m_immediate{false};
  unsigned m_freed{0};
  AvahiIfIndex m_interface{};
  AvahiProtocol m_protocol{};
  AvahiProtocol m_address_protocol{};
  AvahiLookupFlags m_flags{};
  std::string m_name, m_type, m_domain;

private:
  AvahiServiceResolver m_resolver{};
  AvahiServiceResolverCallback m_callback{nullptr};
  void *m_context{nullptr};
};

void immediateAndReplacement() {
  AvahiClient client{};
  auto api{std::make_shared<FakeResolverApi>()};
  api->m_immediate = true;
  std::vector<DiscoveryAvahiResolverEvent> events;
  DiscoveryAvahiServiceResolver resolver{
      api,
      &client,
      {"Printer.Room", "_ipp._tcp", "local.", 7},
      DiscoveryContribution::kIpv4,
      DiscoveryAddressFamily::kIpv6,
      [&](DiscoveryAvahiResolverEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));
        return {};
      }};
  check(resolver.start().has_value(), "resolver startup failed");
  check(api->m_protocol == AVAHI_PROTO_INET &&
            api->m_address_protocol == AVAHI_PROTO_INET6 &&
            (api->m_flags & AVAHI_LOOKUP_NO_TXT) != 0,
        "resolver query and address family were conflated");
  check(events.size() == 1 && events[0].m_host == "Host.local." &&
            events[0].m_port == 8080 &&
            events[0].m_address.m_family == DiscoveryAddressFamily::kIpv6 &&
            events[0].m_address.m_interface == 7,
        "immediate address callback was not copied");
  api->emit(AVAHI_RESOLVER_FOUND);
  check(events.size() == 2, "subsequent resolver update missing");
  check(resolver.stop().has_value() && resolver.stop().has_value() &&
            api->m_freed == 1,
        "resolver was not freed exactly once");
  api->emit(AVAHI_RESOLVER_FOUND);
  check(events.size() == 2, "stopped resolver delivered callback");
}

void failuresAreReported() {
  AvahiClient client{};
  auto api{std::make_shared<FakeResolverApi>()};
  std::optional<DiscoveryError> failure;
  DiscoveryAvahiServiceResolver resolver{
      api,
      &client,
      {"Printer", "_ipp._tcp", "local.", 7},
      DiscoveryContribution::kIpv4,
      DiscoveryAddressFamily::kIpv6,
      nullptr,
      [&](DiscoveryError error) { failure = std::move(error); }};
  check(resolver.start().has_value(), "resolver startup failed");
  api->emit(AVAHI_RESOLVER_FAILURE);
  check(failure && !resolver.status() &&
            failure->m_message.find("fake native error") != std::string::npos,
        "resolver failure was hidden");
  check(resolver.stop().has_value(), "resolver stop failed");

  api = std::make_shared<FakeResolverApi>();
  failure.reset();
  DiscoveryAvahiServiceResolver allocationFailure{
      api,
      &client,
      {"Printer", "_ipp._tcp", "local.", 7},
      DiscoveryContribution::kIpv4,
      DiscoveryAddressFamily::kIpv6,
      [](DiscoveryAvahiResolverEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      },
      [&](DiscoveryError error) { failure = std::move(error); }};
  check(allocationFailure.start().has_value(), "resolver startup failed");
  api->emit(AVAHI_RESOLVER_FOUND);
  const auto allocationStatus{allocationFailure.status()};
  check(!allocationStatus &&
            allocationStatus.error().m_kind ==
                DiscoveryErrorKind::kResourceLimit &&
            failure && failure->m_kind == DiscoveryErrorKind::kResourceLimit,
        "resolver allocation failure classification was lost");
  check(allocationFailure.stop().has_value(), "resolver cleanup failed");
}
} // namespace

int main() {
  immediateAndReplacement();
  failuresAreReported();
}
