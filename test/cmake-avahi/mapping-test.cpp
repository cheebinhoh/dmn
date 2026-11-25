/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file mapping-test.cpp
 * @brief Controlled tests for Avahi callback value conversion.
 */

#include "dmn-discovery-avahi-mapping.hpp"

#include <avahi-client/lookup.h>
#include <avahi-common/address.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

void browseEvents() {
  auto added{mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, 7, AVAHI_PROTO_INET6,
                                 "Kitchen.Printer", "_ipp._tcp", "local.",
                                 AVAHI_LOOKUP_RESULT_MULTICAST)};
  check(added && added->has_value(), "valid browse add was rejected");
  check((*added)->m_added && (*added)->m_key.m_instance == "Kitchen.Printer" &&
            (*added)->m_key.m_interface == 7 &&
            (*added)->m_contribution == DiscoveryContribution::kIpv6 &&
            (*added)->m_flags == AVAHI_LOOKUP_RESULT_MULTICAST,
        "browse add lost identity, transport or flags");
  auto provenance{mapAvahiBrowseProvenance(static_cast<AvahiLookupResultFlags>(
      AVAHI_LOOKUP_RESULT_CACHED | AVAHI_LOOKUP_RESULT_MULTICAST))};
  check(provenance.m_cached == true && provenance.m_multicast == true &&
            !provenance.m_local && !provenance.m_wide_area,
        "known or unknown browse provenance was changed");
  auto removed{mapAvahiBrowseEvent(AVAHI_BROWSER_REMOVE, AVAHI_IF_UNSPEC,
                                   AVAHI_PROTO_INET, "Kitchen.Printer",
                                   "_ipp._tcp", "local.", {})};
  check(removed && removed->has_value() && !(*removed)->m_added &&
            !(*removed)->m_key.m_interface &&
            (*removed)->m_contribution == DiscoveryContribution::kIpv4,
        "browse remove lost source identity");
  auto progress{mapAvahiBrowseEvent(AVAHI_BROWSER_ALL_FOR_NOW, AVAHI_IF_UNSPEC,
                                    AVAHI_PROTO_UNSPEC, nullptr, nullptr,
                                    nullptr, {})};
  check(progress && !progress->has_value(), "progress became a service");
  check(!mapAvahiBrowseEvent(AVAHI_BROWSER_FAILURE, AVAHI_IF_UNSPEC,
                             AVAHI_PROTO_UNSPEC, nullptr, nullptr, nullptr, {}),
        "browser failure was hidden");
  check(!mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, 1, AVAHI_PROTO_UNSPEC, "n",
                             "_ipp._tcp", "local.", {}),
        "unknown browse transport was accepted");
  check(!mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, 0, AVAHI_PROTO_INET, "n",
                             "_ipp._tcp", "local.", {}),
        "invalid interface was accepted");
  check(!mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, 1, AVAHI_PROTO_INET, nullptr,
                             "_ipp._tcp", "local.", {}),
        "null service name was accepted");
  const std::string oversized(64, 'x');
  check(!mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, 1, AVAHI_PROTO_INET,
                             oversized.c_str(), "_ipp._tcp", "local.", {}),
        "oversized service name was accepted");
}

void resolverArguments() {
  auto args{makeAvahiResolverArguments(7, DiscoveryContribution::kIpv4,
                                       DiscoveryAddressFamily::kIpv6)};
  check(args && args->m_interface == 7 &&
            args->m_query_protocol == AVAHI_PROTO_INET &&
            args->m_address_protocol == AVAHI_PROTO_INET6 &&
            (args->m_flags & AVAHI_LOOKUP_NO_TXT) != 0,
        "query transport and address family were conflated");
  check(!makeAvahiResolverArguments({}, DiscoveryContribution::kUnknown,
                                    DiscoveryAddressFamily::kIpv4),
        "unknown Avahi browse transport accepted");
}

void records() {
  const std::vector<std::uint8_t> srv{0,   1,   0,   2,   0x1f, 0x90,
                                      4,   'H', 'o', 's', 't',  5,
                                      'l', 'o', 'c', 'a', 'l',  0};
  const auto decoded{mapAvahiSrvRecord(srv.data(), srv.size())};
  check(decoded && decoded->m_priority == 1 && decoded->m_weight == 2 &&
            decoded->m_port == 8080 && decoded->m_target == "Host.local.",
        "SRV record decoding failed");
  check(!mapAvahiSrvRecord(nullptr, srv.size()), "null SRV data was accepted");
  const std::array<std::uint8_t, 4> txt{2, 0, 255, 0};
  const auto copied{mapAvahiTxtRecord(txt.data(), txt.size(), 2, 4)};
  check(copied && *copied == std::vector<std::uint8_t>(txt.begin(), txt.end()),
        "binary TXT bytes were not retained");
  check(!mapAvahiTxtRecord(txt.data(), txt.size(), 1, 4),
        "TXT entry bound was ignored");
  check(!mapAvahiTxtRecord(txt.data(), txt.size(), 2, 3),
        "TXT byte bound was ignored");
  const std::array<std::uint8_t, 2> invalid{2, 1};
  check(!mapAvahiTxtRecord(invalid.data(), invalid.size(), 2, 4),
        "malformed TXT data was accepted");
  check(mapAvahiTxtRecord(nullptr, 0, 2, 4).has_value(),
        "empty TXT record was rejected");
}

void addresses() {
  AvahiAddress address{};
  address.proto = AVAHI_PROTO_INET;
  const std::array<std::uint8_t, 4> ipv4{192, 0, 2, 1};
  std::memcpy(&address.data.ipv4.address, ipv4.data(), ipv4.size());
  const auto v4{mapAvahiAddress(&address, 4)};
  check(v4 && v4->m_family == DiscoveryAddressFamily::kIpv4 &&
            v4->m_bytes == std::vector<std::uint8_t>(ipv4.begin(), ipv4.end()),
        "IPv4 network bytes changed");
  address.proto = AVAHI_PROTO_INET6;
  address.data.ipv6.address[0] = 0xfe;
  address.data.ipv6.address[1] = 0x80;
  const auto v6{mapAvahiAddress(&address, 4)};
  check(v6 && v6->m_family == DiscoveryAddressFamily::kIpv6 &&
            v6->m_bytes.size() == 16 && v6->m_interface == 4,
        "IPv6 scope or family lost");
  check(!mapAvahiAddress(nullptr, 4), "null address was accepted");
  address.proto = AVAHI_PROTO_UNSPEC;
  check(!mapAvahiAddress(&address, 4), "unknown address family was accepted");
}
} // namespace

int main() {
  browseEvents();
  resolverArguments();
  records();
  addresses();
}
