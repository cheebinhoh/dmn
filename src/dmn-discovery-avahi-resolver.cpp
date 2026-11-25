/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-resolver.cpp
 * @brief Owned Avahi service resolution and result conversion.
 */

#include "dmn-discovery-avahi-resolver.hpp"
#include "dmn-discovery-avahi-error.hpp"

#include <avahi-common/domain.h>
#include <avahi-common/error.h>

#include <cstring>
#include <utility>

namespace dmn::detail {
namespace {

class NativeResolverApi final : public DiscoveryAvahiResolverApi {
public:
  AvahiServiceResolver *create(AvahiClient *client, AvahiIfIndex interfaceIndex,
                               AvahiProtocol protocol, const char *name,
                               const char *type, const char *domain,
                               AvahiProtocol addressProtocol,
                               AvahiLookupFlags flags,
                               AvahiServiceResolverCallback callback,
                               void *context) override {
    return avahi_service_resolver_new(client, interfaceIndex, protocol, name,
                                      type, domain, addressProtocol, flags,
                                      callback, context);
  }

  void free(AvahiServiceResolver *resolver) noexcept override {
    avahi_service_resolver_free(resolver);
  }

  int clientError(AvahiClient *client) override {
    return avahi_client_errno(client);
  }

  const char *errorString(int error) override { return avahi_strerror(error); }
};

bool sameInstance(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }

  const auto fold = [](char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
  };
  for (std::size_t index{0}; index < left.size(); ++index) {
    if (fold(left[index]) != fold(right[index])) {
      return false;
    }
  }

  return true;
}

bool sameDomain(const std::string &left, const std::string &right) {
  const auto first{canonicalizeDomain(left)};
  const auto second{canonicalizeDomain(right)};

  return first && second && *first == *second;
}

} // namespace

std::shared_ptr<DiscoveryAvahiResolverApi> makeDiscoveryAvahiResolverApi() {
  return std::make_shared<NativeResolverApi>();
}

void DiscoveryAvahiServiceResolver::Deleter::operator()(
    AvahiServiceResolver *resolver) const noexcept {
  if (resolver) {
    m_api->free(resolver);
  }
}

DiscoveryAvahiServiceResolver::DiscoveryAvahiServiceResolver(
    std::shared_ptr<DiscoveryAvahiResolverApi> api, AvahiClient *client,
    DiscoveryServiceKey key, DiscoveryContribution contribution,
    DiscoveryAddressFamily family, Handler handler,
    FailureHandler failureHandler)
    : m_api{std::move(api)}, m_client{client}, m_key{std::move(key)},
      m_contribution{contribution}, m_family{family},
      m_handler{std::move(handler)},
      m_failure_handler{std::move(failureHandler)},
      m_resolver{nullptr, Deleter{m_api}} {}

DiscoveryAvahiServiceResolver::~DiscoveryAvahiServiceResolver() = default;

DiscoveryResult DiscoveryAvahiServiceResolver::start() {
  if (!m_api || !m_client || m_resolver || m_starting || m_callback_depth ||
      !validateInstanceName(m_key.m_instance) ||
      !validateServiceType(m_key.m_type) || !validateDomain(m_key.m_domain)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi resolver"}};
  }

  auto args{
      makeAvahiResolverArguments(m_key.m_interface, m_contribution, m_family)};
  if (!args) {
    return std::unexpected{args.error()};
  }

  m_arguments = *args;
  m_error.reset();
  m_callback_failed = false;
  m_callback_allocation_failed = false;
  m_starting = true;
  AvahiServiceResolver *resolver;
  try {
    resolver = m_api->create(
        m_client, args->m_interface, args->m_query_protocol,
        m_key.m_instance.c_str(), m_key.m_type.c_str(), m_key.m_domain.c_str(),
        args->m_address_protocol, args->m_flags, onResolved, this);
  } catch (...) {
    m_starting = false;

    throw;
  }

  m_starting = false;
  m_resolver.reset(resolver);
  auto result{status()};
  if (!result) {
    m_resolver.reset();

    return result;
  }

  if (!m_resolver) {
    return std::unexpected{nativeError("Avahi resolver creation failed")};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiServiceResolver::status() const {
  if (m_callback_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi resolver callback failed"}};
  }

  if (m_error) {
    return std::unexpected{*m_error};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiServiceResolver::stop() {
  if (m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant Avahi resolver stop"}};
  }

  m_resolver.reset();

  return {};
}

DiscoveryError
DiscoveryAvahiServiceResolver::nativeError(const char *prefix) const {
  const auto code{m_api->clientError(m_client)};

  return discoveryAvahiError(prefix, code, m_api->errorString(code));
}

void DiscoveryAvahiServiceResolver::report(DiscoveryError error) {
  if (!m_error) {
    m_error = std::move(error);
    if (m_failure_handler) {
      m_failure_handler(*m_error);
    }
  }
}

void DiscoveryAvahiServiceResolver::onResolved(
    AvahiServiceResolver *resolver, AvahiIfIndex interfaceIndex,
    AvahiProtocol protocol, AvahiResolverEvent event, const char *name,
    const char *type, const char *domain, const char *host,
    const AvahiAddress *address, std::uint16_t port, AvahiStringList *,
    AvahiLookupResultFlags flags, void *context) noexcept {
  auto &self{*static_cast<DiscoveryAvahiServiceResolver *>(context)};
  if (!resolver || (!self.m_starting &&
                    (!self.m_resolver || self.m_resolver.get() != resolver))) {
    return;
  }

  ++self.m_callback_depth;
  try {
    if (!self.m_error && !self.m_callback_failed) {
      if (event == AVAHI_RESOLVER_FAILURE) {
        self.report(self.nativeError("Avahi resolver failed"));
      } else {
        auto source{mapAvahiBrowseEvent(AVAHI_BROWSER_NEW, interfaceIndex,
                                        protocol, name, type, domain, flags)};
        const auto hostSize{host ? strnlen(host, AVAHI_DOMAIN_NAME_MAX) : 0};
        const bool validHost{host && hostSize < AVAHI_DOMAIN_NAME_MAX &&
                             validateDomain(std::string_view{host, hostSize})};
        auto mapped{mapAvahiAddress(address, interfaceIndex)};
        if (event != AVAHI_RESOLVER_FOUND || !source || !*source ||
            interfaceIndex != self.m_arguments.m_interface ||
            protocol != self.m_arguments.m_query_protocol ||
            !sameInstance((*source)->m_key.m_instance, self.m_key.m_instance) ||
            !sameDomain((*source)->m_key.m_type, self.m_key.m_type) ||
            !sameDomain((*source)->m_key.m_domain, self.m_key.m_domain) ||
            !validHost || port == 0 || !mapped ||
            mapped->m_family != self.m_family) {
          self.report(
              {DiscoveryErrorKind::kBackend, "invalid Avahi resolver result"});
        } else if (self.m_handler) {
          auto result{self.m_handler(
              {std::string{host, hostSize}, port, std::move(*mapped), flags})};
          if (!result) {
            self.report(result.error());
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
            {DiscoveryErrorKind::kBackend, "Avahi resolver callback failed"});
      }
    } catch (...) {
      self.m_callback_failed = true;
    }
  }

  --self.m_callback_depth;
}

} // namespace dmn::detail
