/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-record.hpp
 * @brief Private Bonjour SRV and TXT record subscriptions.
 */

#ifndef DMN_DISCOVERY_BONJOUR_RECORD_HPP_
#define DMN_DISCOVERY_BONJOUR_RECORD_HPP_

#include "dmn-discovery-bonjour-operation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dmn::detail {

/** @brief Owned record update from one interface-scoped subscription. */
struct DiscoveryBonjourRecordEvent {
  bool m_added;
  DiscoveryRecordKind m_kind;
  std::vector<std::uint8_t> m_wire;
};

/** @brief One worker-owned native SRV or TXT record watch. */
class DiscoveryBonjourRecordWatch {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryBonjourRecordEvent)>;
  using FailureHandler = DiscoveryBonjourOperation::FailureHandler;

  DiscoveryBonjourRecordWatch(std::shared_ptr<DiscoveryBonjourPoller> poller,
                              std::string fullName,
                              std::optional<std::uint32_t> interfaceIndex,
                              DiscoveryRecordKind kind,
                              std::size_t maxTxtEntries,
                              std::size_t maxTxtBytes, Handler handler,
                              FailureHandler failureHandler = {});
  ~DiscoveryBonjourRecordWatch();
  DiscoveryBonjourRecordWatch(const DiscoveryBonjourRecordWatch &) = delete;
  DiscoveryBonjourRecordWatch &
  operator=(const DiscoveryBonjourRecordWatch &) = delete;
  DiscoveryBonjourRecordWatch(DiscoveryBonjourRecordWatch &&) = delete;
  DiscoveryBonjourRecordWatch &
  operator=(DiscoveryBonjourRecordWatch &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  static void DNSSD_API onRecord(DNSServiceRef ref, DNSServiceFlags flags,
                                 std::uint32_t interfaceIndex,
                                 DNSServiceErrorType error,
                                 const char *fullName, std::uint16_t type,
                                 std::uint16_t recordClass, std::uint16_t size,
                                 const void *data, std::uint32_t ttl,
                                 void *context) noexcept;

  DiscoveryBonjourOperation m_operation;
  std::string m_name;
  std::optional<std::uint32_t> m_interface;
  DiscoveryRecordKind m_kind;
  std::size_t m_max_txt_entries;
  std::size_t m_max_txt_bytes;
  Handler m_handler;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_RECORD_HPP_
