/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-registration.cpp
 * @brief Owned Bonjour service registration and withdrawal.
 */

#include "dmn-discovery-bonjour-registration.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"

#include <arpa/inet.h>
#include <limits>
#include <utility>

namespace dmn::detail {
DiscoveryResult validateDiscoveryBonjourAdvertisement(
    const DiscoveryOptions &options,
    const DiscoveryAdvertisement &advertisement) {
  if (auto result{validateAdvertisement(options, advertisement)}; !result) {
    return result;
  }

  if (!discoveryBonjourValidInterface(advertisement.m_key.m_interface)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour interface"}};
  }

  const auto domain{canonicalizeDomain(advertisement.m_key.m_domain)};
  const auto local{canonicalizeDomain("local.")};
  if (!domain || !local || *domain != *local) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kUnsupported,
                       "non-local registration unsupported"}};
  }

  std::size_t length{0};
  for (const auto &entry : advertisement.m_txt) {
    const auto maximum{std::numeric_limits<std::uint16_t>::max()};
    if (length > maximum || entry.size() + 1 > maximum - length) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "Bonjour TXT record too long"}};
    }

    length += entry.size() + 1;
  }

  return {};
}

DiscoveryBonjourRegistration::DiscoveryBonjourRegistration(
    std::shared_ptr<DiscoveryBonjourPoller> poller,
    DiscoveryAdvertisement advertisement, DiscoveryOptions options,
    Handler handler, FailureHandler failureHandler)
    : m_operation{std::move(poller), std::move(failureHandler)},
      m_advertisement{std::move(advertisement)}, m_options{options},
      m_handler{std::move(handler)} {}

DiscoveryBonjourRegistration::~DiscoveryBonjourRegistration() {
  static_cast<void>(stop());
}

DiscoveryResult DiscoveryBonjourRegistration::start() {
  if (!m_handler || m_started) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid registration state"}};
  }

  if (auto result{
          validateDiscoveryBonjourAdvertisement(m_options, m_advertisement)};
      !result) {
    return result;
  }

  std::vector<std::uint8_t> wire;
  for (const auto &entry : m_advertisement.m_txt) {
    wire.push_back(static_cast<std::uint8_t>(entry.size()));
    wire.insert(wire.end(), entry.begin(), entry.end());
  }

  m_collision = false;
  const auto &key{m_advertisement.m_key};
  auto result{m_operation.start([&](DNSServiceRef *ref) {
    return DNSServiceRegister(
        ref, kDNSServiceFlagsNoAutoRename, key.m_interface.value_or(0),
        key.m_instance.c_str(), key.m_type.c_str(), key.m_domain.c_str(),
        nullptr, htons(m_advertisement.m_port),
        static_cast<std::uint16_t>(wire.size()),
        wire.empty() ? nullptr : wire.data(), onRegistration, this);
  })};
  if (!result && result.error().m_native_category ==
                     DiscoveryNativeErrorCategory::kCollision) {
    m_started = true;

    return notify(DiscoveryBonjourRegistrationEvent::kCollision);
  }

  if (result) {
    m_started = true;
  }

  return result;
}

DiscoveryResult DiscoveryBonjourRegistration::status() const {
  return m_operation.status();
}

DiscoveryResult DiscoveryBonjourRegistration::stop() {
  auto result{m_operation.stop()};
  if (result) {
    m_started = false;
  }

  return result;
}

DiscoveryResult
DiscoveryBonjourRegistration::notify(DiscoveryBonjourRegistrationEvent event) {
  if (m_collision) {
    return {};
  }

  if (event == DiscoveryBonjourRegistrationEvent::kCollision) {
    m_collision = true;
  }

  return m_handler(event);
}

void DNSSD_API DiscoveryBonjourRegistration::onRegistration(
    DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType error,
    const char *name, const char *type, const char *domain,
    void *context) noexcept {
  auto &self{*static_cast<DiscoveryBonjourRegistration *>(context)};
  self.m_operation.invoke(ref, [&] {
    if (error == kDNSServiceErr_NameConflict) {
      auto result{self.notify(DiscoveryBonjourRegistrationEvent::kCollision)};
      if (!result) {
        self.m_operation.report(std::move(result.error()));
      }

      return;
    }

    if (error != 0) {
      self.m_operation.report(
          discoveryBonjourError("Bonjour registration failed", error));

      return;
    }

    if (!(flags & kDNSServiceFlagsAdd)) {
      self.m_operation.report(
          {DiscoveryErrorKind::kBackend, "Bonjour registration was withdrawn"});

      return;
    }

    auto instance{discoveryBonjourString(name, 63)};
    auto serviceType{discoveryBonjourString(type, 22)};
    auto serviceDomain{discoveryBonjourString(domain, 1024)};
    if (!instance || !serviceType || !serviceDomain) {
      self.m_operation.report(!instance      ? instance.error()
                              : !serviceType ? serviceType.error()
                                             : serviceDomain.error());

      return;
    }

    if (serviceType->ends_with('.')) {
      serviceType->remove_suffix(1);
    }

    const auto receivedType{canonicalizeDomain(*serviceType)};
    const auto expectedType{
        canonicalizeDomain(self.m_advertisement.m_key.m_type)};
    const auto receivedDomain{canonicalizeDomain(*serviceDomain)};
    const auto expectedDomain{
        canonicalizeDomain(self.m_advertisement.m_key.m_domain)};
    if (!discoveryBonjourSameLabel(*instance,
                                   self.m_advertisement.m_key.m_instance) ||
        !receivedType || !expectedType || *receivedType != *expectedType ||
        !receivedDomain || !expectedDomain ||
        *receivedDomain != *expectedDomain) {
      self.m_operation.report({DiscoveryErrorKind::kBackend,
                               "Bonjour registration identity changed"});

      return;
    }

    auto result{self.notify(DiscoveryBonjourRegistrationEvent::kEstablished)};
    if (!result) {
      self.m_operation.report(std::move(result.error()));
    }
  });
}

} // namespace dmn::detail
