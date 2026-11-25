/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-mapping.cpp
 * @brief Conversion of native Avahi callback values into owned discovery data.
 */

#include "dmn-discovery-avahi-mapping.hpp"
#include "dmn-discovery-records.hpp"

#include <cstring>
#include <limits>
#include <string_view>

namespace dmn::detail {
namespace {

DiscoveryError invalid(const char *message) {
  return {DiscoveryErrorKind::kInvalidArgument, message};
}

std::expected<std::optional<std::uint32_t>, DiscoveryError>
mapInterface(AvahiIfIndex value) {
  if (value == AVAHI_IF_UNSPEC) {
    return std::optional<std::uint32_t>{};
  }

  if (value <= 0) {
    return std::unexpected{invalid("invalid Avahi interface")};
  }

  return static_cast<std::uint32_t>(value);
}

std::expected<std::string_view, DiscoveryError>
boundedString(const char *value, std::size_t maximum) {
  if (!value) {
    return std::unexpected{invalid("missing Avahi string")};
  }

  const auto size{strnlen(value, maximum + 1)};
  if (size > maximum) {
    return std::unexpected{invalid("Avahi string exceeds its limit")};
  }

  return std::string_view{value, size};
}

std::expected<std::vector<std::uint8_t>, DiscoveryError>
copyRecord(const void *data, std::size_t size, std::size_t maximum) {
  if (size > maximum) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "Avahi record exceeds its limit"}};
  }

  if (size && !data) {
    return std::unexpected{invalid("missing Avahi record data")};
  }

  if (size == 0) {
    return std::vector<std::uint8_t>{};
  }

  const auto *bytes{static_cast<const std::uint8_t *>(data)};

  return std::vector<std::uint8_t>{bytes, bytes + size};
}

} // namespace

DiscoveryProvenance mapAvahiBrowseProvenance(AvahiLookupResultFlags flags) {
  const auto known =
      [flags](AvahiLookupResultFlags flag) -> std::optional<bool> {
    return (flags & flag) ? std::optional<bool>{true} : std::nullopt;
  };

  return {known(AVAHI_LOOKUP_RESULT_CACHED), known(AVAHI_LOOKUP_RESULT_LOCAL),
          known(AVAHI_LOOKUP_RESULT_WIDE_AREA),
          known(AVAHI_LOOKUP_RESULT_MULTICAST)};
}

std::expected<std::optional<DiscoveryAvahiBrowseEvent>, DiscoveryError>
mapAvahiBrowseEvent(AvahiBrowserEvent event, AvahiIfIndex interfaceIndex,
                    AvahiProtocol protocol, const char *name, const char *type,
                    const char *domain, AvahiLookupResultFlags flags) {
  if (event == AVAHI_BROWSER_CACHE_EXHAUSTED ||
      event == AVAHI_BROWSER_ALL_FOR_NOW) {
    return std::optional<DiscoveryAvahiBrowseEvent>{};
  }

  if (event == AVAHI_BROWSER_FAILURE) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "Avahi browser failed"}};
  }

  if (event != AVAHI_BROWSER_NEW && event != AVAHI_BROWSER_REMOVE) {
    return std::unexpected{invalid("unknown Avahi browse event")};
  }

  auto interface{mapInterface(interfaceIndex)};
  auto instance{boundedString(name, 63)};
  auto serviceType{boundedString(type, 21)};
  auto serviceDomain{boundedString(domain, 1024)};
  if (!interface || !instance || !serviceType || !serviceDomain) {
    return std::unexpected{invalid("invalid Avahi service identity")};
  }

  if (!validateInstanceName(*instance) || !validateServiceType(*serviceType) ||
      !validateDomain(*serviceDomain)) {
    return std::unexpected{invalid("invalid Avahi service identity")};
  }

  DiscoveryContribution contribution;
  if (protocol == AVAHI_PROTO_INET) {
    contribution = DiscoveryContribution::kIpv4;
  } else if (protocol == AVAHI_PROTO_INET6) {
    contribution = DiscoveryContribution::kIpv6;
  } else {
    return std::unexpected{invalid("invalid Avahi browse transport")};
  }

  return DiscoveryAvahiBrowseEvent{event == AVAHI_BROWSER_NEW,
                                   {std::string{*instance},
                                    std::string{*serviceType},
                                    std::string{*serviceDomain}, *interface},
                                   contribution,
                                   flags};
}

std::expected<DiscoveryAvahiResolverArguments, DiscoveryError>
makeAvahiResolverArguments(std::optional<std::uint32_t> interfaceIndex,
                           DiscoveryContribution contribution,
                           DiscoveryAddressFamily family) {
  if (interfaceIndex &&
      (*interfaceIndex == 0 ||
       *interfaceIndex > static_cast<std::uint32_t>(
                             std::numeric_limits<AvahiIfIndex>::max()))) {
    return std::unexpected{invalid("invalid resolver interface")};
  }

  AvahiProtocol queryProtocol;
  if (contribution == DiscoveryContribution::kIpv4) {
    queryProtocol = AVAHI_PROTO_INET;
  } else if (contribution == DiscoveryContribution::kIpv6) {
    queryProtocol = AVAHI_PROTO_INET6;
  } else {
    return std::unexpected{invalid("invalid resolver transport")};
  }

  AvahiProtocol addressProtocol;
  if (family == DiscoveryAddressFamily::kIpv4) {
    addressProtocol = AVAHI_PROTO_INET;
  } else if (family == DiscoveryAddressFamily::kIpv6) {
    addressProtocol = AVAHI_PROTO_INET6;
  } else {
    return std::unexpected{invalid("invalid resolver address family")};
  }

  return DiscoveryAvahiResolverArguments{
      interfaceIndex ? static_cast<AvahiIfIndex>(*interfaceIndex)
                     : AVAHI_IF_UNSPEC,
      queryProtocol, addressProtocol, AVAHI_LOOKUP_NO_TXT};
}

std::expected<DiscoverySrv, DiscoveryError>
mapAvahiSrvRecord(const void *data, std::size_t size) {
  auto bytes{copyRecord(data, size, 261)};
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }

  return parseSrvRecord(*bytes);
}

std::expected<std::vector<std::uint8_t>, DiscoveryError>
mapAvahiTxtRecord(const void *data, std::size_t size, std::size_t maxEntries,
                  std::size_t maxBytes) {
  auto bytes{copyRecord(data, size, maxBytes)};
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }

  if (auto parsed{parseTxtRecord(*bytes, maxEntries, maxBytes)}; !parsed) {
    return std::unexpected{parsed.error()};
  }

  return bytes;
}

std::expected<DiscoveryAddress, DiscoveryError>
mapAvahiAddress(const AvahiAddress *address, AvahiIfIndex interfaceIndex) {
  auto interface{mapInterface(interfaceIndex)};
  if (!address || !interface) {
    return std::unexpected{invalid("invalid Avahi address")};
  }

  if (address->proto == AVAHI_PROTO_INET) {
    const auto *bytes{
        reinterpret_cast<const std::uint8_t *>(&address->data.ipv4.address)};

    return DiscoveryAddress{
        DiscoveryAddressFamily::kIpv4, {bytes, bytes + 4}, *interface};
  }

  if (address->proto == AVAHI_PROTO_INET6) {
    const auto *bytes{address->data.ipv6.address};

    return DiscoveryAddress{
        DiscoveryAddressFamily::kIpv6, {bytes, bytes + 16}, *interface};
  }

  return std::unexpected{invalid("invalid Avahi address protocol")};
}

} // namespace dmn::detail
