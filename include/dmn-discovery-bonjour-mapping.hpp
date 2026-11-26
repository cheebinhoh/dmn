/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-mapping.hpp
 * @brief Private, bounded conversions for Bonjour callbacks.
 */

#ifndef DMN_DISCOVERY_BONJOUR_MAPPING_HPP_
#define DMN_DISCOVERY_BONJOUR_MAPPING_HPP_

#include "dmn-discovery-core.hpp"

#include <dns_sd.h>

#include <expected>
#include <optional>
#include <string_view>

namespace dmn::detail {

[[nodiscard]] std::expected<std::string_view, DiscoveryError>
discoveryBonjourString(const char *value, std::size_t maximum);
[[nodiscard]] std::optional<std::uint32_t>
discoveryBonjourInterface(std::uint32_t value);
[[nodiscard]] bool
discoveryBonjourValidInterface(std::optional<std::uint32_t> value);
[[nodiscard]] bool discoveryBonjourSameLabel(std::string_view left,
                                             std::string_view right);
[[nodiscard]] DiscoveryError discoveryBonjourError(const char *operation,
                                                   DNSServiceErrorType error);

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_MAPPING_HPP_
