/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-public-conversion.hpp
 * @brief Private conversion between owned public and discovery-core values.
 */

#ifndef DMN_DISCOVERY_PUBLIC_CONVERSION_HPP_
#define DMN_DISCOVERY_PUBLIC_CONVERSION_HPP_

#include "dmn-discovery-session.hpp"
#include "dmn-discovery.hpp"

#include <array>

namespace dmn::detail {

[[nodiscard]] DiscoveryOptions
discoveryOptions(const Dmn_DiscoveryOptions &options);
[[nodiscard]] DiscoveryServiceKey
discoveryKey(const Dmn_DiscoveryServiceKey &key);
[[nodiscard]] DiscoveryAdvertisement
discoveryAdvertisement(const Dmn_DiscoveryAdvertisement &advertisement);
[[nodiscard]] std::expected<std::array<bool, 2>, DiscoveryError>
discoveryQueryFamilies(const Dmn_DiscoveryQuery &query);
[[nodiscard]] Dmn_DiscoveryError
discoveryPublicError(const DiscoveryError &error,
                     Dmn_DiscoveryErrorKind backendKind);
[[nodiscard]] Dmn_DiscoveryValueResult<Dmn_DiscoveryCandidate>
discoveryPublicCandidate(const DiscoveryCandidate &candidate,
                         const DiscoveryOptions &options);
[[nodiscard]] Dmn_DiscoveryValueResult<Dmn_DiscoverySnapshot>
discoveryPublicSnapshot(const DiscoverySessionSnapshot &snapshot,
                        const DiscoveryOptions &options);
[[nodiscard]] Dmn_DiscoveryValueResult<Dmn_DiscoveryEvent>
discoveryPublicEvent(const DiscoveryDelivery &delivery,
                     const DiscoveryOptions &options);
[[nodiscard]] Dmn_DiscoveryRegistrationStatus
discoveryPublicRegistrationStatus(const DiscoveryRegistrationStatus &status);

} // namespace dmn::detail

#endif // DMN_DISCOVERY_PUBLIC_CONVERSION_HPP_
