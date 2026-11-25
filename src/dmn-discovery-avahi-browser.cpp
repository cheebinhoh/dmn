/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-browser.cpp
 * @brief Owned Avahi service browse and callback conversion.
 */

#include "dmn-discovery-avahi-browser.hpp"
#include "dmn-discovery-avahi-error.hpp"

#include <avahi-common/error.h>

#include <limits>
#include <utility>

namespace dmn::detail {
namespace {

class NativeBrowserApi final : public DiscoveryAvahiBrowserApi {
public:
  AvahiServiceBrowser *create(AvahiClient *client, AvahiIfIndex interfaceIndex,
                              AvahiProtocol protocol, const char *type,
                              const char *domain, AvahiLookupFlags flags,
                              AvahiServiceBrowserCallback callback,
                              void *context) override {
    return avahi_service_browser_new(client, interfaceIndex, protocol, type,
                                     domain, flags, callback, context);
  }

  void free(AvahiServiceBrowser *browser) noexcept override {
    avahi_service_browser_free(browser);
  }

  int clientError(AvahiClient *client) override {
    return avahi_client_errno(client);
  }

  const char *errorString(int error) override { return avahi_strerror(error); }
};

} // namespace

std::shared_ptr<DiscoveryAvahiBrowserApi> makeDiscoveryAvahiBrowserApi() {
  return std::make_shared<NativeBrowserApi>();
}

void DiscoveryAvahiServiceBrowser::Deleter::operator()(
    AvahiServiceBrowser *browser) const noexcept {
  if (browser) {
    m_api->free(browser);
  }
}

DiscoveryAvahiServiceBrowser::DiscoveryAvahiServiceBrowser(
    std::shared_ptr<DiscoveryAvahiBrowserApi> api, AvahiClient *client,
    DiscoveryAvahiQuery query, Handler handler, FailureHandler failureHandler)
    : m_api{std::move(api)}, m_client{client}, m_query{std::move(query)},
      m_handler{std::move(handler)},
      m_failure_handler{std::move(failureHandler)},
      m_browser{nullptr, Deleter{m_api}} {}

DiscoveryAvahiServiceBrowser::~DiscoveryAvahiServiceBrowser() = default;

DiscoveryResult DiscoveryAvahiServiceBrowser::start() {
  if (!m_api || !m_client || m_browser || m_starting || m_callback_depth ||
      !validateServiceType(m_query.m_type) ||
      !validateDomain(m_query.m_domain) ||
      (m_query.m_interface &&
       (*m_query.m_interface == 0 ||
        *m_query.m_interface >
            static_cast<std::uint32_t>(
                std::numeric_limits<AvahiIfIndex>::max())))) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                       "invalid Avahi browser query or state"}};
  }

  m_error.reset();
  m_callback_failed = false;
  m_callback_allocation_failed = false;
  const auto interfaceIndex{
      m_query.m_interface ? static_cast<AvahiIfIndex>(*m_query.m_interface)
                          : AVAHI_IF_UNSPEC};
  m_starting = true;
  AvahiServiceBrowser *browser;
  try {
    browser = m_api->create(m_client, interfaceIndex, AVAHI_PROTO_UNSPEC,
                            m_query.m_type.c_str(), m_query.m_domain.c_str(),
                            static_cast<AvahiLookupFlags>(0), onBrowse, this);
  } catch (...) {
    m_starting = false;

    throw;
  }

  m_starting = false;
  m_browser.reset(browser);
  auto result{status()};
  if (!result) {
    m_browser.reset();

    return result;
  }

  if (!m_browser) {
    return std::unexpected{nativeError("Avahi browser creation failed")};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiServiceBrowser::status() const {
  if (m_callback_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi browser callback failed"}};
  }

  if (m_error) {
    return std::unexpected{*m_error};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiServiceBrowser::stop() {
  if (m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant Avahi browser stop"}};
  }

  m_browser.reset();

  return {};
}

DiscoveryError
DiscoveryAvahiServiceBrowser::nativeError(const char *prefix) const {
  const auto code{m_api->clientError(m_client)};

  return discoveryAvahiError(prefix, code, m_api->errorString(code));
}

void DiscoveryAvahiServiceBrowser::report(DiscoveryError error) {
  if (!m_error) {
    m_error = std::move(error);
    if (m_failure_handler) {
      m_failure_handler(*m_error);
    }
  }
}

void DiscoveryAvahiServiceBrowser::onBrowse(
    AvahiServiceBrowser *browser, AvahiIfIndex interfaceIndex,
    AvahiProtocol protocol, AvahiBrowserEvent event, const char *name,
    const char *type, const char *domain, AvahiLookupResultFlags flags,
    void *context) noexcept {
  auto &self{*static_cast<DiscoveryAvahiServiceBrowser *>(context)};
  if (!browser || (!self.m_starting &&
                   (!self.m_browser || self.m_browser.get() != browser))) {
    return;
  }

  ++self.m_callback_depth;
  try {
    if (!self.m_error && !self.m_callback_failed) {
      if (event == AVAHI_BROWSER_FAILURE) {
        self.report(self.nativeError("Avahi browser failed"));
      } else {
        auto mapped{mapAvahiBrowseEvent(event, interfaceIndex, protocol, name,
                                        type, domain, flags)};
        if (!mapped) {
          self.report(mapped.error());
        } else if (*mapped) {
          const auto &key{(**mapped).m_key};
          const auto typeName{canonicalizeDomain(key.m_type)};
          const auto queryType{canonicalizeDomain(self.m_query.m_type)};
          const auto domainName{canonicalizeDomain(key.m_domain)};
          const auto queryDomain{canonicalizeDomain(self.m_query.m_domain)};
          if (!typeName || !queryType || *typeName != *queryType ||
              !domainName || !queryDomain || *domainName != *queryDomain ||
              (self.m_query.m_interface &&
               key.m_interface != self.m_query.m_interface)) {
            self.report({DiscoveryErrorKind::kBackend,
                         "Avahi browse result does not match query"});
          } else if (self.m_handler) {
            auto result{self.m_handler(std::move(**mapped))};
            if (!result) {
              self.report(result.error());
            }
          }
        }
      }
    }
  } catch (const std::bad_alloc &) {
    self.m_callback_failed = true;
    self.m_callback_allocation_failed = true;
    try {
      if (self.m_failure_handler) {
        self.m_failure_handler({DiscoveryErrorKind::kResourceLimit, {}});
      }
    } catch (...) {
      // The allocation-failure latch remains authoritative.
    }
  } catch (...) {
    self.m_callback_failed = true;
    try {
      if (self.m_failure_handler) {
        self.m_failure_handler(
            {DiscoveryErrorKind::kBackend, "Avahi browser callback failed"});
      }
    } catch (...) {
      self.m_callback_failed = true;
    }
  }

  --self.m_callback_depth;
}

} // namespace dmn::detail
