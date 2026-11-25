/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-error.hpp
 * @brief Preserve Avahi error identities and stable public categories.
 */

#ifndef DMN_DISCOVERY_AVAHI_ERROR_HPP_
#define DMN_DISCOVERY_AVAHI_ERROR_HPP_

#include "dmn-discovery-core.hpp"

#include <avahi-common/error.h>

namespace dmn::detail {

inline DiscoveryError discoveryAvahiError(const char *prefix, int code,
                                          const char *message) {
  DiscoveryError result{DiscoveryErrorKind::kBackend,
                        std::string{prefix} + ": " +
                            (message ? message : "unknown error"),
                        "avahi",
                        code,
                        {}};
  switch (code) {
  case AVAHI_ERR_DISCONNECTED:
  case AVAHI_ERR_NO_DAEMON:
    result.m_native_category = DiscoveryNativeErrorCategory::kUnavailable;
    break;
  case AVAHI_ERR_ACCESS_DENIED:
    result.m_native_category = DiscoveryNativeErrorCategory::kPermission;
    break;
  case AVAHI_ERR_NO_MEMORY:
  case AVAHI_ERR_TOO_MANY_CLIENTS:
  case AVAHI_ERR_TOO_MANY_OBJECTS:
  case AVAHI_ERR_TOO_MANY_ENTRIES:
    result.m_native_category = DiscoveryNativeErrorCategory::kResourceLimit;
    break;
  case AVAHI_ERR_NOT_SUPPORTED:
    result.m_native_category = DiscoveryNativeErrorCategory::kUnsupported;
    break;
  case AVAHI_ERR_COLLISION:
    result.m_native_category = DiscoveryNativeErrorCategory::kCollision;
    break;
  default:
    break;
  }
  return result;
}

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_ERROR_HPP_
