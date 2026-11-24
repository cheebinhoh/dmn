/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-records.hpp
 * @brief Private DNS-SD record and endpoint reconciliation model.
 *
 * The model preserves per-source ownership and rejects stale address
 * generations. It represents only records observed by its caller; it does not
 * claim a complete network inventory or perform native DNS operations.
 */

#ifndef DMN_DISCOVERY_RECORDS_HPP_
#define DMN_DISCOVERY_RECORDS_HPP_

#include "dmn-discovery-core.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmn::detail {

/**
 * @brief Decode bounded, uncompressed DNS SRV RDATA.
 *
 * Compression pointers are rejected because a standalone RDATA buffer does
 * not contain the DNS message needed to resolve them.
 *
 * @param rdata SRV priority, weight, port and wire-format target.
 * @return An owned record or a typed malformed-record error.
 */
[[nodiscard]] std::expected<DiscoverySrv, DiscoveryError>
parseSrvRecord(const std::vector<std::uint8_t> &rdata);

/**
 * @brief Decode DNS TXT RDATA into owned, length-delimited entries.
 *
 * @param rdata Raw wire bytes, including entry length octets.
 * @param maxEntries Maximum number of entries.
 * @param maxBytes Maximum total encoded bytes.
 * @return Entries or a typed framing/resource error.
 */
[[nodiscard]] std::expected<DiscoveryTxt, DiscoveryError>
parseTxtRecord(const std::vector<std::uint8_t> &rdata, std::size_t maxEntries,
               std::size_t maxBytes);

/**
 * @brief Caller-serialized record state for one interface-scoped service.
 *
 * Sources are identified by their live browse-contribution generation.
 * Address results from an old record generation cannot recreate an endpoint.
 * This private model has no native handles or background threads.
 */
class DiscoveryRecords {
public:
  /**
   * @brief Construct a record model with a per-kind distinct record limit.
   *
   * @param maxRecords Maximum distinct observed SRV or TXT records.
   * @param maxAddresses Maximum distinct addresses for the service.
   * @param maxTxtEntries Maximum aggregate entries across distinct records.
   * @param maxTxtBytes Maximum aggregate encoded bytes across distinct records.
   * @param interfaceIndex Known service interface, if any.
   */
  explicit DiscoveryRecords(std::size_t maxRecords,
                            std::size_t maxAddresses = 32,
                            std::size_t maxTxtEntries = 64,
                            std::size_t maxTxtBytes = 4096,
                            std::optional<std::uint32_t> interfaceIndex = {});

  /**
   * @brief Register one live browse contribution.
   *
   * @param source Opaque nonzero contribution generation.
   * @return Whether the source was accepted.
   */
  [[nodiscard]] bool addSource(std::uint64_t source);

  /**
   * @brief Remove one source and all of its record/address contributions.
   *
   * @param source Contribution generation.
   * @return Whether the source existed.
   */
  [[nodiscard]] bool removeSource(std::uint64_t source);

  /**
   * @brief Report whether one source has a functioning record subscription.
   *
   * @param source Contribution generation.
   * @param kind SRV or TXT subscription.
   * @param active Whether that subscription is functioning.
   * @return Whether the source existed.
   */
  [[nodiscard]] bool setMonitoring(std::uint64_t source,
                                   DiscoveryRecordKind kind, bool active);

  /**
   * @brief Obtain the lifetime token of a currently active record watch.
   *
   * A restarted watch receives a new token. Native callbacks must retain the
   * token associated with the watch that created them.
   *
   * @param source Contribution generation.
   * @param kind SRV or TXT subscription.
   * @return Active watch generation, or no value if inactive or absent.
   */
  [[nodiscard]] std::optional<std::uint64_t>
  monitorGeneration(std::uint64_t source, DiscoveryRecordKind kind) const;

  /**
   * @brief Add an observed SRV record for a source.
   *
   * @param source Contribution generation.
   * @param watch Active SRV watch generation.
   * @param record Owned record value.
   * @return Success or a typed invalid input/resource error.
   */
  [[nodiscard]] DiscoveryResult
  addSrv(std::uint64_t source, std::uint64_t watch, const DiscoverySrv &record);

  /**
   * @brief Remove an observed SRV record from one source.
   *
   * @param source Contribution generation.
   * @param watch Active SRV watch generation.
   * @param record Record previously observed.
   * @return Whether that source contained the record.
   */
  [[nodiscard]] bool removeSrv(std::uint64_t source, std::uint64_t watch,
                               const DiscoverySrv &record);

  /**
   * @brief Add a raw TXT record after validating its framing.
   *
   * @param source Contribution generation.
   * @param watch Active TXT watch generation.
   * @param wire Length-delimited TXT RDATA.
   * @return Success or a typed invalid input/resource error.
   */
  [[nodiscard]] DiscoveryResult addTxt(std::uint64_t source,
                                       std::uint64_t watch,
                                       std::vector<std::uint8_t> wire);

  /**
   * @brief Remove the exact TXT wire representation observed by a source.
   *
   * @param source Contribution generation.
   * @param watch Active TXT watch generation.
   * @param wire Previously observed TXT RDATA.
   * @return Whether that source contained the record.
   */
  [[nodiscard]] bool removeTxt(std::uint64_t source, std::uint64_t watch,
                               std::vector<std::uint8_t> wire);

  /**
   * @brief Replace one source's address for the address's family.
   *
   * @param source Contribution generation.
   * @param generation Current SRV resolution generation.
   * @param lookup Address-lookup lifetime token for the address family.
   * @param host Resolved target hostname.
   * @param port Resolved host-order port.
   * @param address Owned IP address and scope.
   * @return Success or a typed stale/input/resource error.
   */
  [[nodiscard]] DiscoveryResult
  replaceAddress(std::uint64_t source, std::uint64_t generation,
                 std::uint64_t lookup, std::string_view host,
                 std::uint16_t port, DiscoveryAddress address);

  /**
   * @brief Add an address reported by a set-valued address lookup.
   *
   * The generation, hostname and port must match the current SRV endpoint.
   * The address is rejected if the configured bound would be exceeded.
   *
   * @param source Contribution generation.
   * @param generation Current SRV resolution generation.
   * @param lookup Address-lookup lifetime token for the address family.
   * @param host Resolved target hostname.
   * @param port Resolved host-order port.
   * @param address Owned IP address and scope.
   * @return Success or a typed stale/input/resource error.
   */
  [[nodiscard]] DiscoveryResult
  addAddress(std::uint64_t source, std::uint64_t generation,
             std::uint64_t lookup, std::string_view host, std::uint16_t port,
             DiscoveryAddress address);

  /**
   * @brief Remove an address from a still-current lookup source.
   *
   * @param source Contribution generation.
   * @param generation Current SRV resolution generation.
   * @param lookup Address-lookup lifetime token for the address family.
   * @param address Address previously reported by this source.
   * @return Whether the address was removed.
   */
  [[nodiscard]] bool removeAddress(std::uint64_t source,
                                   std::uint64_t generation,
                                   std::uint64_t lookup,
                                   const DiscoveryAddress &address);

  /**
   * @brief Obtain a source's current address-lookup lifetime token.
   *
   * @param source Live contribution generation.
   * @param family Resolved address family.
   * @return Token, or no value for an absent or exhausted source.
   */
  [[nodiscard]] std::optional<std::uint64_t>
  addressGeneration(std::uint64_t source, DiscoveryAddressFamily family) const;

  /**
   * @brief Remove one failed lookup's addresses and invalidate its callbacks.
   *
   * @param source Live contribution generation.
   * @param family Failed address family.
   * @param lookup Token of the failed lookup.
   * @return False if the source is missing or token space is exhausted.
   */
  [[nodiscard]] bool failAddressLookup(std::uint64_t source,
                                       DiscoveryAddressFamily family,
                                       std::uint64_t lookup);

  /**
   * @brief Discard current addresses and advance their resolution generation.
   *
   * Record subscriptions and their watch generations are not restarted.
   *
   * @return Success or a resource error if the generation is exhausted.
   */
  [[nodiscard]] DiscoveryResult refreshAddresses();

  /**
   * @brief Copy the currently observed endpoint and conflict state.
   *
   * @return An owned snapshot without an exhaustive-inventory guarantee.
   */
  [[nodiscard]] DiscoveryRecordSnapshot snapshot() const;

private:
  struct Source {
    std::uint64_t m_srv_monitor{0};
    std::uint64_t m_txt_monitor{0};
    std::uint64_t m_ipv4_lookup{0};
    std::uint64_t m_ipv6_lookup{0};
    std::vector<DiscoverySrv> m_srv;
    std::vector<std::vector<std::uint8_t>> m_txt;
    std::vector<DiscoveryAddress> m_addresses;
  };

  /**
   * @brief Invalidate previously resolved addresses after a record transition.
   */
  void invalidateAddresses();

  [[nodiscard]] DiscoveryResult
  storeAddress(std::uint64_t source, std::uint64_t generation,
               std::uint64_t lookup, std::string_view host, std::uint16_t port,
               DiscoveryAddress address, bool replace);

  std::size_t m_max_records;
  std::size_t m_max_addresses;
  std::size_t m_max_txt_entries;
  std::size_t m_max_txt_bytes;
  std::optional<std::uint32_t> m_interface;
  std::uint64_t m_generation{1};
  std::uint64_t m_next_watch{1};
  std::uint64_t m_next_lookup{1};
  std::map<std::uint64_t, Source> m_sources;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_RECORDS_HPP_
