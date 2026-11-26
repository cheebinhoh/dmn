/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-browser.cpp
 * @brief Worker-owned Bonjour browse and callback conversion.
 */

#include "dmn-discovery-bonjour-browser.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"

#include <utility>

namespace dmn::detail {

DiscoveryBonjourBrowser::DiscoveryBonjourBrowser(
    std::shared_ptr<DiscoveryBonjourPoller> poller, DiscoveryBonjourQuery query,
    Handler handler, FailureHandler failureHandler)
    : m_operation{std::move(poller), std::move(failureHandler)},
      m_query{std::move(query)}, m_handler{std::move(handler)} {}

DiscoveryBonjourBrowser::~DiscoveryBonjourBrowser() {
  static_cast<void>(stop());
}

DiscoveryResult DiscoveryBonjourBrowser::start() {
  if (!m_handler || !validateServiceType(m_query.m_type) ||
      !validateDomain(m_query.m_domain) ||
      !discoveryBonjourValidInterface(m_query.m_interface)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour browser query"}};
  }

  return m_operation.start([this](DNSServiceRef *ref) {
    return DNSServiceBrowse(ref, 0, m_query.m_interface.value_or(0),
                            m_query.m_type.c_str(), m_query.m_domain.c_str(),
                            onBrowse, this);
  });
}

DiscoveryResult DiscoveryBonjourBrowser::status() const {
  return m_operation.status();
}

DiscoveryResult DiscoveryBonjourBrowser::stop() { return m_operation.stop(); }

void DNSSD_API DiscoveryBonjourBrowser::onBrowse(
    DNSServiceRef ref, DNSServiceFlags flags, std::uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *name, const char *type,
    const char *domain, void *context) noexcept {
  auto &self{*static_cast<DiscoveryBonjourBrowser *>(context)};
  self.m_operation.invoke(ref, [&] {
    if (error != 0) {
      self.m_operation.report(
          discoveryBonjourError("Bonjour browse failed", error));
    } else {
      auto instance{discoveryBonjourString(name, 63)};
      auto serviceType{discoveryBonjourString(type, 22)};
      auto serviceDomain{discoveryBonjourString(domain, 1024)};
      if (!instance || !serviceType || !serviceDomain) {
        const auto &failure{!instance      ? instance.error()
                            : !serviceType ? serviceType.error()
                                           : serviceDomain.error()};
        self.m_operation.report(failure);
      } else {
        if (serviceType->ends_with('.')) {
          serviceType->remove_suffix(1);
        }

        auto canonicalType{canonicalizeDomain(*serviceType)};
        auto queryType{canonicalizeDomain(self.m_query.m_type)};
        auto canonicalDomain{canonicalizeDomain(*serviceDomain)};
        auto queryDomain{canonicalizeDomain(self.m_query.m_domain)};
        const auto interface{discoveryBonjourInterface(interfaceIndex)};
        if (!validateInstanceName(*instance) ||
            !validateServiceType(*serviceType) || !canonicalType ||
            !queryType || *canonicalType != *queryType || !canonicalDomain ||
            !queryDomain || *canonicalDomain != *queryDomain ||
            (self.m_query.m_interface &&
             self.m_query.m_interface != interface)) {
          self.m_operation.report(
              {DiscoveryErrorKind::kBackend,
               "Bonjour browse result does not match query"});
        } else {
          auto result{self.m_handler(
              {(flags & kDNSServiceFlagsAdd) != 0,
               {std::string{*instance}, std::string{*serviceType},
                std::string{*serviceDomain}, interface}})};
          if (!result) {
            self.m_operation.report(result.error());
          }
        }
      }
    }
  });
}

} // namespace dmn::detail
