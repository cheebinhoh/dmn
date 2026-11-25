/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-mapping.hpp
 * @brief Private, bounded conversion of Avahi discovery values.
 */

#ifndef DMN_DISCOVERY_AVAHI_MAPPING_HPP_
#define DMN_DISCOVERY_AVAHI_MAPPING_HPP_

#include "dmn-discovery-core.hpp"

#include <avahi-client/lookup.h>
#include <avahi-common/address.h>

namespace dmn::detail {

/** @brief An owned browse delta, including its native provenance flags. */
struct DiscoveryAvahiBrowseEvent {
  bool m_added;
  DiscoveryServiceKey m_key;
  DiscoveryContribution m_contribution;
  AvahiLookupResultFlags m_flags;
};

[[nodiscard]] DiscoveryProvenance
mapAvahiBrowseProvenance(AvahiLookupResultFlags flags);

/** @brief Map a native browse callback; progress events produce no delta. */
[[nodiscard]] std::expected<std::optional<DiscoveryAvahiBrowseEvent>,
                            DiscoveryError>
mapAvahiBrowseEvent(AvahiBrowserEvent event, AvahiIfIndex interfaceIndex,
                    AvahiProtocol protocol, const char *name, const char *type,
                    const char *domain, AvahiLookupResultFlags flags);

/** @brief Distinct browse transport and requested endpoint address family. */
struct DiscoveryAvahiResolverArguments {
  AvahiIfIndex m_interface;
  AvahiProtocol m_query_protocol;
  AvahiProtocol m_address_protocol;
  AvahiLookupFlags m_flags;
};

/** @brief Prepare a resolver that does not depend on TXT presence. */
[[nodiscard]] std::expected<DiscoveryAvahiResolverArguments, DiscoveryError>
makeAvahiResolverArguments(std::optional<std::uint32_t> interfaceIndex,
                           DiscoveryContribution contribution,
                           DiscoveryAddressFamily family);

/** @brief Copy and decode bounded uncompressed SRV RDATA. */
[[nodiscard]] std::expected<DiscoverySrv, DiscoveryError>
mapAvahiSrvRecord(const void *data, std::size_t size);

/** @brief Copy bounded TXT RDATA without interpreting its binary contents. */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, DiscoveryError>
mapAvahiTxtRecord(const void *data, std::size_t size, std::size_t maxEntries,
                  std::size_t maxBytes);

/** @brief Copy a native address without changing its network-order bytes. */
[[nodiscard]] std::expected<DiscoveryAddress, DiscoveryError>
mapAvahiAddress(const AvahiAddress *address, AvahiIfIndex interfaceIndex);

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_MAPPING_HPP_
