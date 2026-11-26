/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-browser.hpp
 * @brief Private Bonjour service-browser adapter.
 *
 * This implementation is private and is not installed. It is composed by the
 * Bonjour session owner selected by the public factory on macOS.
 */

#ifndef DMN_DISCOVERY_BONJOUR_BROWSER_HPP_
#define DMN_DISCOVERY_BONJOUR_BROWSER_HPP_

#include "dmn-discovery-bonjour-operation.hpp"

#include <dns_sd.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dmn::detail {

/** @brief Explicit interface-scoped Bonjour service query. */
struct DiscoveryBonjourQuery {
  std::string m_type;
  std::string m_domain;
  std::optional<std::uint32_t> m_interface;
};

/** @brief Owned native browse update without a fabricated query transport. */
struct DiscoveryBonjourBrowseEvent {
  bool m_added;
  DiscoveryServiceKey m_key;
  DiscoveryContribution m_contribution{DiscoveryContribution::kUnknown};
};

/**
 * @brief Worker-owned browser and callback context.
 *
 * The owner must stop the browser before stopping its poller, and must not
 * destroy or stop this object from one of its native callbacks or handlers.
 */
class DiscoveryBonjourBrowser {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryBonjourBrowseEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;

  DiscoveryBonjourBrowser(std::shared_ptr<DiscoveryBonjourPoller> poller,
                          DiscoveryBonjourQuery query, Handler handler,
                          FailureHandler failureHandler = {});
  ~DiscoveryBonjourBrowser();
  DiscoveryBonjourBrowser(const DiscoveryBonjourBrowser &) = delete;
  DiscoveryBonjourBrowser &operator=(const DiscoveryBonjourBrowser &) = delete;
  DiscoveryBonjourBrowser(DiscoveryBonjourBrowser &&) = delete;
  DiscoveryBonjourBrowser &operator=(DiscoveryBonjourBrowser &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  static void DNSSD_API onBrowse(DNSServiceRef ref, DNSServiceFlags flags,
                                 std::uint32_t interfaceIndex,
                                 DNSServiceErrorType error, const char *name,
                                 const char *type, const char *domain,
                                 void *context) noexcept;
  DiscoveryBonjourOperation m_operation;
  DiscoveryBonjourQuery m_query;
  Handler m_handler;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_BROWSER_HPP_
