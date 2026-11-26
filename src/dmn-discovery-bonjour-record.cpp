/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-record.cpp
 * @brief Worker-owned Bonjour SRV and TXT record watches.
 */

#include "dmn-discovery-bonjour-record.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"
#include "dmn-discovery-records.hpp"

#include <utility>

namespace dmn::detail {

DiscoveryBonjourRecordWatch::DiscoveryBonjourRecordWatch(
    std::shared_ptr<DiscoveryBonjourPoller> poller, std::string fullName,
    std::optional<std::uint32_t> interfaceIndex, DiscoveryRecordKind kind,
    std::size_t maxTxtEntries, std::size_t maxTxtBytes, Handler handler,
    FailureHandler failureHandler)
    : m_operation{std::move(poller), std::move(failureHandler)},
      m_name{std::move(fullName)}, m_interface{interfaceIndex}, m_kind{kind},
      m_max_txt_entries{maxTxtEntries}, m_max_txt_bytes{maxTxtBytes},
      m_handler{std::move(handler)} {}

DiscoveryBonjourRecordWatch::~DiscoveryBonjourRecordWatch() {
  static_cast<void>(stop());
}

DiscoveryResult DiscoveryBonjourRecordWatch::start() {
  if (!m_handler || !validateDomain(m_name) ||
      !discoveryBonjourValidInterface(m_interface) ||
      (m_kind != DiscoveryRecordKind::kSrv &&
       m_kind != DiscoveryRecordKind::kTxt) ||
      m_max_txt_entries == 0 || m_max_txt_bytes == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour record watch"}};
  }

  return m_operation.start([this](DNSServiceRef *ref) {
    return DNSServiceQueryRecord(
        ref, 0, m_interface.value_or(0), m_name.c_str(),
        m_kind == DiscoveryRecordKind::kSrv ? kDNSServiceType_SRV
                                            : kDNSServiceType_TXT,
        kDNSServiceClass_IN, onRecord, this);
  });
}

DiscoveryResult DiscoveryBonjourRecordWatch::status() const {
  return m_operation.status();
}

DiscoveryResult DiscoveryBonjourRecordWatch::stop() {
  return m_operation.stop();
}

void DNSSD_API DiscoveryBonjourRecordWatch::onRecord(
    DNSServiceRef ref, DNSServiceFlags flags, std::uint32_t interfaceIndex,
    DNSServiceErrorType error, const char *fullName, std::uint16_t type,
    std::uint16_t recordClass, std::uint16_t size, const void *data,
    std::uint32_t, void *context) noexcept {
  auto &self{*static_cast<DiscoveryBonjourRecordWatch *>(context)};
  self.m_operation.invoke(ref, [&] {
    if (error != 0) {
      self.m_operation.report(
          discoveryBonjourError("Bonjour record watch failed", error));

      return;
    }

    auto name{discoveryBonjourString(fullName, 1024)};
    if (!name) {
      self.m_operation.report(name.error());

      return;
    }

    auto received{canonicalizeDomain(*name)};
    auto expected{canonicalizeDomain(self.m_name)};
    const auto expectedType{self.m_kind == DiscoveryRecordKind::kSrv
                                ? kDNSServiceType_SRV
                                : kDNSServiceType_TXT};
    if (!received || !expected || *received != *expected ||
        discoveryBonjourInterface(interfaceIndex) != self.m_interface ||
        type != expectedType || recordClass != kDNSServiceClass_IN ||
        (size != 0 && !data)) {
      self.m_operation.report(
          {DiscoveryErrorKind::kBackend, "invalid Bonjour record update"});

      return;
    }

    const auto maximum{self.m_kind == DiscoveryRecordKind::kSrv
                           ? std::size_t{261}
                           : self.m_max_txt_bytes};
    if (size > maximum) {
      self.m_operation.report({DiscoveryErrorKind::kResourceLimit,
                               "Bonjour record exceeds its limit"});

      return;
    }

    std::vector<std::uint8_t> wire;
    if (size != 0) {
      const auto *bytes{static_cast<const std::uint8_t *>(data)};
      wire.assign(bytes, bytes + size);
    }

    DiscoveryResult parsed;
    if (self.m_kind == DiscoveryRecordKind::kSrv) {
      auto record{parseSrvRecord(wire)};
      if (!record) {
        parsed = std::unexpected{record.error()};
      }
    } else {
      auto record{
          parseTxtRecord(wire, self.m_max_txt_entries, self.m_max_txt_bytes)};
      if (!record) {
        parsed = std::unexpected{record.error()};
      }
    }

    if (!parsed) {
      auto failure{std::move(parsed.error())};
      if (failure.m_kind == DiscoveryErrorKind::kInvalidArgument) {
        failure.m_kind = DiscoveryErrorKind::kBackend;
      }

      self.m_operation.report(std::move(failure));

      return;
    }

    auto result{self.m_handler(
        {(flags & kDNSServiceFlagsAdd) != 0, self.m_kind, std::move(wire)})};
    if (!result) {
      self.m_operation.report(std::move(result.error()));
    }
  });
}

} // namespace dmn::detail
