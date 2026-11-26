/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-address.cpp
 * @brief Worker-owned Bonjour address lookups.
 */

#include "dmn-discovery-bonjour-address.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <utility>

namespace dmn::detail {

DiscoveryBonjourAddressLookup::DiscoveryBonjourAddressLookup(
    std::shared_ptr<DiscoveryBonjourPoller> poller, std::string target,
    std::optional<std::uint32_t> interfaceIndex, DiscoveryAddressFamily family,
    Handler handler, FailureHandler failureHandler)
    : m_operation{std::move(poller), std::move(failureHandler)},
      m_target{std::move(target)}, m_interface{interfaceIndex},
      m_family{family}, m_handler{std::move(handler)} {}

DiscoveryBonjourAddressLookup::~DiscoveryBonjourAddressLookup() {
  static_cast<void>(stop());
}

DiscoveryResult DiscoveryBonjourAddressLookup::start() {
  if (!m_handler || !validateDomain(m_target) || m_target == "." ||
      !discoveryBonjourValidInterface(m_interface) ||
      (m_family != DiscoveryAddressFamily::kIpv4 &&
       m_family != DiscoveryAddressFamily::kIpv6)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour address lookup"}};
  }

  return m_operation.start([this](DNSServiceRef *ref) {
    return DNSServiceGetAddrInfo(ref, 0, m_interface.value_or(0),
                                 m_family == DiscoveryAddressFamily::kIpv4
                                     ? kDNSServiceProtocol_IPv4
                                     : kDNSServiceProtocol_IPv6,
                                 m_target.c_str(), onAddress, this);
  });
}

DiscoveryResult DiscoveryBonjourAddressLookup::status() const {
  return m_operation.status();
}

DiscoveryResult DiscoveryBonjourAddressLookup::stop() {
  return m_operation.stop();
}

void DNSSD_API DiscoveryBonjourAddressLookup::onAddress(
    DNSServiceRef ref, DNSServiceFlags flags, std::uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *hostname,
    const struct sockaddr *address, std::uint32_t, void *context) noexcept {
  auto &self{*static_cast<DiscoveryBonjourAddressLookup *>(context)};
  self.m_operation.invoke(ref, [&] {
    if (error != 0) {
      self.m_operation.report(
          discoveryBonjourError("Bonjour address lookup failed", error));

      return;
    }

    auto name{discoveryBonjourString(hostname, 1024)};
    if (!name) {
      self.m_operation.report(name.error());

      return;
    }

    auto received{canonicalizeDomain(*name)};
    auto expected{canonicalizeDomain(self.m_target)};
    auto interface{discoveryBonjourInterface(interfaceIndex)};
    const auto expectedFamily{
        self.m_family == DiscoveryAddressFamily::kIpv4 ? AF_INET : AF_INET6};
    if (!address || !received || !expected || *received != *expected ||
        interface != self.m_interface || address->sa_family != expectedFamily) {
      self.m_operation.report(
          {DiscoveryErrorKind::kBackend, "invalid Bonjour address update"});

      return;
    }

    DiscoveryAddress value{self.m_family, {}, interface};
    if (self.m_family == DiscoveryAddressFamily::kIpv4) {
      const auto &native{*reinterpret_cast<const sockaddr_in *>(address)};
      const auto *bytes{
          reinterpret_cast<const std::uint8_t *>(&native.sin_addr)};
      value.m_bytes.assign(bytes, bytes + 4);
    } else {
      const auto &native{*reinterpret_cast<const sockaddr_in6 *>(address)};
      if (native.sin6_scope_id != 0) {
        auto scope{discoveryBonjourInterface(native.sin6_scope_id)};
        if (!scope || (interface && scope != interface)) {
          self.m_operation.report({DiscoveryErrorKind::kBackend,
                                   "invalid Bonjour IPv6 address scope"});

          return;
        }

        value.m_interface = scope;
      }

      const auto *bytes{
          reinterpret_cast<const std::uint8_t *>(&native.sin6_addr)};
      value.m_bytes.assign(bytes, bytes + 16);
    }

    auto result{
        self.m_handler({(flags & kDNSServiceFlagsAdd) != 0, std::move(value)})};
    if (!result) {
      self.m_operation.report(std::move(result.error()));
    }
  });
}

} // namespace dmn::detail
