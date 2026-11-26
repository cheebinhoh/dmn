/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-address.hpp
 * @brief Private, interface-scoped Bonjour address subscriptions.
 */

#ifndef DMN_DISCOVERY_BONJOUR_ADDRESS_HPP_
#define DMN_DISCOVERY_BONJOUR_ADDRESS_HPP_

#include "dmn-discovery-bonjour-operation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dmn::detail {

/** @brief Owned address addition or withdrawal from one lookup. */
struct DiscoveryBonjourAddressEvent {
  bool m_added;
  DiscoveryAddress m_address;
};

/** @brief One worker-owned Bonjour address lookup. */
class DiscoveryBonjourAddressLookup {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryBonjourAddressEvent)>;
  using FailureHandler = DiscoveryBonjourOperation::FailureHandler;

  DiscoveryBonjourAddressLookup(std::shared_ptr<DiscoveryBonjourPoller> poller,
                                std::string target,
                                std::optional<std::uint32_t> interfaceIndex,
                                DiscoveryAddressFamily family, Handler handler,
                                FailureHandler failureHandler = {});
  ~DiscoveryBonjourAddressLookup();
  DiscoveryBonjourAddressLookup(const DiscoveryBonjourAddressLookup &) = delete;
  DiscoveryBonjourAddressLookup &
  operator=(const DiscoveryBonjourAddressLookup &) = delete;
  DiscoveryBonjourAddressLookup(DiscoveryBonjourAddressLookup &&) = delete;
  DiscoveryBonjourAddressLookup &
  operator=(DiscoveryBonjourAddressLookup &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  static void DNSSD_API onAddress(DNSServiceRef ref, DNSServiceFlags flags,
                                  std::uint32_t interfaceIndex,
                                  DNSServiceErrorType error,
                                  const char *hostname,
                                  const struct sockaddr *address,
                                  std::uint32_t ttl, void *context) noexcept;

  DiscoveryBonjourOperation m_operation;
  std::string m_target;
  std::optional<std::uint32_t> m_interface;
  DiscoveryAddressFamily m_family;
  Handler m_handler;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_ADDRESS_HPP_
