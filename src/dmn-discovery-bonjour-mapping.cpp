/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-mapping.cpp
 * @brief Bounded conversion of Bonjour callback values.
 */

#include "dmn-discovery-bonjour-mapping.hpp"

#include <cstring>

namespace dmn::detail {
namespace {
constexpr std::uint32_t kFirstReservedInterface{0xfffffff0u};
} // namespace

std::expected<std::string_view, DiscoveryError>
discoveryBonjourString(const char *value, std::size_t maximum) {
  if (!value) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "missing Bonjour value"}};
  }

  const auto size{strnlen(value, maximum + 1)};
  if (size > maximum) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "Bonjour value too long"}};
  }

  return std::string_view{value, size};
}

std::optional<std::uint32_t> discoveryBonjourInterface(std::uint32_t value) {
  if (value == 0 || value >= kFirstReservedInterface) {
    return std::nullopt;
  }

  return value;
}

bool discoveryBonjourValidInterface(std::optional<std::uint32_t> value) {
  return !value || discoveryBonjourInterface(*value).has_value();
}

bool discoveryBonjourSameLabel(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }

  const auto fold = [](unsigned char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
  };
  for (std::size_t index{0}; index < left.size(); ++index) {
    if (fold(left[index]) != fold(right[index])) {
      return false;
    }
  }

  return true;
}

DiscoveryError discoveryBonjourError(const char *operation,
                                     DNSServiceErrorType error) {
  DiscoveryError result{DiscoveryErrorKind::kBackend, operation, "bonjour",
                        error};
  switch (error) {
  case kDNSServiceErr_ServiceNotRunning:
  case kDNSServiceErr_DefunctConnection:
    result.m_native_category = DiscoveryNativeErrorCategory::kUnavailable;
    break;
  case kDNSServiceErr_PolicyDenied:
  case kDNSServiceErr_NotPermitted:
  case kDNSServiceErr_NoAuth:
  case kDNSServiceErr_Firewall:
  case kDNSServiceErr_Refused:
    result.m_native_category = DiscoveryNativeErrorCategory::kPermission;
    break;
  case kDNSServiceErr_NoMemory:
    result.m_kind = DiscoveryErrorKind::kResourceLimit;
    result.m_native_category = DiscoveryNativeErrorCategory::kResourceLimit;
    break;
  case kDNSServiceErr_Unsupported:
    result.m_native_category = DiscoveryNativeErrorCategory::kUnsupported;
    break;
  case kDNSServiceErr_NameConflict:
    result.m_native_category = DiscoveryNativeErrorCategory::kCollision;
    break;
  default:
    break;
  }

  return result;
}

} // namespace dmn::detail
