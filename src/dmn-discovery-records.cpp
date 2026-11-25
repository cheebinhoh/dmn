/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-records.cpp
 * @brief Private DNS-SD wire parsing and source reconciliation.
 */

#include "dmn-discovery-records.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace dmn::detail {

namespace {

using SrvKey =
    std::tuple<std::uint16_t, std::uint16_t, std::uint16_t, std::string>;

/**
 * @brief Make an error that does not copy untrusted DNS payload bytes.
 */
DiscoveryError recordError(DiscoveryErrorKind kind, std::string message) {
  return DiscoveryError{kind, std::move(message)};
}

/**
 * @brief Compare SRV target names using DNS ASCII case and label semantics.
 */
SrvKey srvKey(const DiscoverySrv &record) {
  return {record.m_priority, record.m_weight, record.m_port,
          *canonicalizeDomain(record.m_target)};
}

/**
 * @brief Normalize the two permitted empty TXT encodings for comparison.
 */
void normalizeEmptyTxt(std::vector<std::uint8_t> &wire) {
  if (wire.empty()) {
    wire.push_back(0);
  }
}

struct TxtLess {
  bool operator()(const std::vector<std::uint8_t> &left,
                  const std::vector<std::uint8_t> &right) const {
    const auto count{std::min(left.size(), right.size())};
    for (std::size_t i{0}; i < count; ++i) {
      if (left[i] != right[i]) {
        return left[i] < right[i];
      }
    }

    return left.size() < right.size();
  }
};

/**
 * @brief Read one network-byte-order integer from a validated buffer.
 */
std::uint16_t readUint16(const std::vector<std::uint8_t> &bytes,
                         std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

/**
 * @brief Escape a DNS label byte in presentation form without losing bytes.
 */
void appendDnsByte(std::string &name, std::uint8_t value) {
  if (value >= 0x21 && value <= 0x7e && value != '.' && value != '\\') {
    name.push_back(static_cast<char>(value));

    return;
  }

  name.push_back('\\');
  name.push_back(static_cast<char>('0' + value / 100));
  name.push_back(static_cast<char>('0' + (value / 10) % 10));
  name.push_back(static_cast<char>('0' + value % 10));
}

/**
 * @brief Determine whether a typed address is usable as a candidate.
 */
bool isUsableAddress(const DiscoveryAddress &address) {
  if (address.m_interface && *address.m_interface == 0) {
    return false;
  }

  if (address.m_family == DiscoveryAddressFamily::kIpv4) {
    return address.m_bytes.size() == 4 &&
           !(address.m_bytes[0] == 0 && address.m_bytes[1] == 0 &&
             address.m_bytes[2] == 0 && address.m_bytes[3] == 0) &&
           address.m_bytes[0] < 224;
  }

  if (address.m_bytes.size() != 16 || address.m_bytes[0] == 0xff ||
      std::all_of(address.m_bytes.begin(), address.m_bytes.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    return false;
  }

  const bool linkLocal{address.m_bytes[0] == 0xfe &&
                       (address.m_bytes[1] & 0xc0) == 0x80};

  return !linkLocal || address.m_interface.has_value();
}

} // namespace

std::expected<DiscoverySrv, DiscoveryError>
parseSrvRecord(const std::vector<std::uint8_t> &rdata) {
  if (rdata.size() < 7 || rdata.size() > 261) {
    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid SRV record length")};
  }

  std::string target{};
  std::size_t offset{6};
  std::size_t nameLength{1};
  bool terminated{false};
  while (offset < rdata.size()) {
    const auto length{rdata[offset++]};
    if (length == 0) {
      terminated = true;
      break;
    }

    if (length > 63 || length > rdata.size() - offset ||
        nameLength + length + 1 > 255) {
      return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                         "invalid SRV target name")};
    }

    for (std::size_t index{0}; index < length; ++index) {
      appendDnsByte(target, rdata[offset++]);
    }

    target.push_back('.');
    nameLength += length + 1;
  }

  if (!terminated || offset != rdata.size()) {
    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid SRV target termination")};
  }

  if (target.empty()) {
    target = ".";
  }

  return DiscoverySrv{readUint16(rdata, 0), readUint16(rdata, 2),
                      readUint16(rdata, 4), std::move(target)};
}

std::expected<DiscoveryTxt, DiscoveryError>
parseTxtRecord(const std::vector<std::uint8_t> &rdata, std::size_t maxEntries,
               std::size_t maxBytes) {
  if (rdata.size() > maxBytes) {
    return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                       "TXT record byte limit exceeded")};
  }

  DiscoveryTxt result{};
  std::size_t offset{0};
  while (offset < rdata.size()) {
    if (result.size() >= maxEntries) {
      return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                         "TXT record entry limit exceeded")};
    }

    const auto length{rdata[offset++]};
    if (length > rdata.size() - offset) {
      return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                         "invalid TXT record framing")};
    }

    result.emplace_back(rdata.begin() + offset,
                        rdata.begin() + offset + length);
    offset += length;
  }

  return result;
}

DiscoveryRecords::DiscoveryRecords(std::size_t maxRecords,
                                   std::size_t maxAddresses,
                                   std::size_t maxTxtEntries,
                                   std::size_t maxTxtBytes,
                                   std::optional<std::uint32_t> interfaceIndex)
    : m_max_records{maxRecords}, m_max_addresses{maxAddresses},
      m_max_txt_entries{maxTxtEntries}, m_max_txt_bytes{maxTxtBytes},
      m_interface{interfaceIndex} {}

bool DiscoveryRecords::addSource(std::uint64_t source) {
  if (source == 0 || m_sources.contains(source) || m_next_lookup == 0 ||
      m_next_lookup == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }

  Source value{};
  value.m_ipv4_lookup = m_next_lookup++;
  value.m_ipv6_lookup = m_next_lookup++;

  return m_sources.emplace(source, std::move(value)).second;
}

bool DiscoveryRecords::removeSource(std::uint64_t source) {
  const auto before{snapshot()};
  if (m_sources.erase(source) == 0) {
    return false;
  }

  const auto after{snapshot()};
  if ((before.m_srv && !after.m_srv) || (!before.m_srv && after.m_srv) ||
      (before.m_srv && after.m_srv &&
       srvKey(*before.m_srv) != srvKey(*after.m_srv)) ||
      (before.m_state == DiscoveryRecordState::kMultipleRecordsUnsupported) !=
          (after.m_state ==
           DiscoveryRecordState::kMultipleRecordsUnsupported)) {
    invalidateAddresses();
  }

  return true;
}

bool DiscoveryRecords::setMonitoring(std::uint64_t source,
                                     DiscoveryRecordKind kind, bool active) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end()) {
    return false;
  }

  auto &monitor{kind == DiscoveryRecordKind::kSrv
                    ? entry->second.m_srv_monitor
                    : entry->second.m_txt_monitor};
  if (!active) {
    if (monitor == 0) {
      return true;
    }

    monitor = 0;
    if (kind == DiscoveryRecordKind::kSrv) {
      entry->second.m_srv.clear();
    } else {
      entry->second.m_txt.clear();
    }

    invalidateAddresses();
  } else if (monitor == 0) {
    if (m_next_watch == 0) {
      return false;
    }

    monitor = m_next_watch++;
  }

  return true;
}

std::optional<std::uint64_t>
DiscoveryRecords::monitorGeneration(std::uint64_t source,
                                    DiscoveryRecordKind kind) const {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end()) {
    return std::nullopt;
  }

  const auto token{kind == DiscoveryRecordKind::kSrv
                       ? entry->second.m_srv_monitor
                       : entry->second.m_txt_monitor};
  if (token == 0) {
    return std::nullopt;
  }

  return token;
}

DiscoveryResult DiscoveryRecords::addSrv(std::uint64_t source,
                                         std::uint64_t watch,
                                         const DiscoverySrv &record) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || watch == 0 ||
      entry->second.m_srv_monitor != watch) {
    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid SRV source or target")};
  }

  if (!validateDomain(record.m_target)) {
    static_cast<void>(setMonitoring(source, DiscoveryRecordKind::kSrv, false));

    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid SRV source or target")};
  }

  const auto key{srvKey(record)};
  const auto same{
      [&](const DiscoverySrv &item) { return srvKey(item) == key; }};
  if (std::any_of(entry->second.m_srv.begin(), entry->second.m_srv.end(),
                  same)) {
    return {};
  }

  std::set<SrvKey> distinct{};
  for (const auto &[id, value] : m_sources) {
    static_cast<void>(id);
    for (const auto &item : value.m_srv) {
      distinct.insert(srvKey(item));
    }
  }

  if (!distinct.contains(key) && distinct.size() >= m_max_records) {
    static_cast<void>(setMonitoring(source, DiscoveryRecordKind::kSrv, false));

    return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                       "SRV record limit exceeded")};
  }

  const bool introducesRecord{distinct.insert(key).second};
  entry->second.m_srv.push_back(record);
  if (introducesRecord) {
    invalidateAddresses();
  }

  return {};
}

bool DiscoveryRecords::removeSrv(std::uint64_t source, std::uint64_t watch,
                                 const DiscoverySrv &record) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || watch == 0 ||
      entry->second.m_srv_monitor != watch ||
      !validateDomain(record.m_target)) {
    return false;
  }

  const auto key{srvKey(record)};
  auto &records{entry->second.m_srv};
  const auto item{
      std::find_if(records.begin(), records.end(),
                   [&](const auto &value) { return srvKey(value) == key; })};
  if (item == records.end()) {
    return false;
  }

  records.erase(item);
  bool remains{false};
  for (const auto &[id, value] : m_sources) {
    static_cast<void>(id);
    remains |= std::any_of(
        value.m_srv.begin(), value.m_srv.end(),
        [&](const auto &candidate) { return srvKey(candidate) == key; });
  }

  if (!remains) {
    invalidateAddresses();
  }

  return true;
}

DiscoveryResult DiscoveryRecords::addTxt(std::uint64_t source,
                                         std::uint64_t watch,
                                         std::vector<std::uint8_t> wire) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || watch == 0 ||
      entry->second.m_txt_monitor != watch) {
    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid TXT source")};
  }

  if (auto parsed{parseTxtRecord(wire, m_max_txt_entries, m_max_txt_bytes)};
      !parsed) {
    static_cast<void>(setMonitoring(source, DiscoveryRecordKind::kTxt, false));

    return std::unexpected{parsed.error()};
  }

  auto &records{entry->second.m_txt};
  if (std::find(records.begin(), records.end(), wire) != records.end()) {
    return {};
  }

  std::set<std::vector<std::uint8_t>, TxtLess> distinct{};
  for (const auto &[id, value] : m_sources) {
    static_cast<void>(id);
    for (auto item : value.m_txt) {
      normalizeEmptyTxt(item);
      distinct.insert(std::move(item));
    }
  }

  auto normalized{wire};
  normalizeEmptyTxt(normalized);
  if (!distinct.contains(normalized) && distinct.size() >= m_max_records) {
    static_cast<void>(setMonitoring(source, DiscoveryRecordKind::kTxt, false));

    return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                       "TXT record limit exceeded")};
  }

  const auto previousCount{distinct.size()};
  distinct.insert(std::move(normalized));
  std::size_t totalBytes{0};
  std::size_t totalEntries{0};
  for (const auto &item : distinct) {
    const auto parsed{parseTxtRecord(item, m_max_txt_entries, m_max_txt_bytes)};
    if (!parsed || item.size() > m_max_txt_bytes - totalBytes ||
        parsed->size() > m_max_txt_entries - totalEntries) {
      static_cast<void>(
          setMonitoring(source, DiscoveryRecordKind::kTxt, false));

      return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                         "aggregate TXT limit exceeded")};
    }

    totalBytes += item.size();
    totalEntries += parsed->size();
  }

  records.push_back(std::move(wire));
  if ((previousCount > 1) != (distinct.size() > 1)) {
    invalidateAddresses();
  }

  return {};
}

bool DiscoveryRecords::removeTxt(std::uint64_t source, std::uint64_t watch,
                                 std::vector<std::uint8_t> wire) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || watch == 0 ||
      entry->second.m_txt_monitor != watch) {
    return false;
  }

  auto &records{entry->second.m_txt};
  const auto item{std::find(records.begin(), records.end(), wire)};
  if (item == records.end()) {
    return false;
  }

  const auto wasConflict{snapshot().m_state ==
                         DiscoveryRecordState::kMultipleRecordsUnsupported};
  records.erase(item);
  const auto isConflict{snapshot().m_state ==
                        DiscoveryRecordState::kMultipleRecordsUnsupported};
  if (wasConflict != isConflict) {
    invalidateAddresses();
  }

  return true;
}

DiscoveryResult
DiscoveryRecords::replaceAddress(std::uint64_t source, std::uint64_t generation,
                                 std::uint64_t lookup, std::string_view host,
                                 std::uint16_t port, DiscoveryAddress address) {
  return storeAddress(source, generation, lookup, host, port,
                      std::move(address), true);
}

DiscoveryResult
DiscoveryRecords::addAddress(std::uint64_t source, std::uint64_t generation,
                             std::uint64_t lookup, std::string_view host,
                             std::uint16_t port, DiscoveryAddress address) {
  return storeAddress(source, generation, lookup, host, port,
                      std::move(address), false);
}

bool DiscoveryRecords::removeAddress(std::uint64_t source,
                                     std::uint64_t generation,
                                     std::uint64_t lookup,
                                     const DiscoveryAddress &address) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || m_generation == 0 ||
      generation != m_generation ||
      addressGeneration(source, address.m_family) != lookup) {
    return false;
  }

  auto &addresses{entry->second.m_addresses};
  const auto item{std::find(addresses.begin(), addresses.end(), address)};
  if (item == addresses.end()) {
    return false;
  }

  addresses.erase(item);

  return true;
}

std::optional<std::uint64_t>
DiscoveryRecords::addressGeneration(std::uint64_t source,
                                    DiscoveryAddressFamily family) const {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || m_generation == 0) {
    return std::nullopt;
  }

  const auto token{family == DiscoveryAddressFamily::kIpv4
                       ? entry->second.m_ipv4_lookup
                       : entry->second.m_ipv6_lookup};
  if (token == 0) {
    return std::nullopt;
  }

  return token;
}

bool DiscoveryRecords::failAddressLookup(std::uint64_t source,
                                         DiscoveryAddressFamily family,
                                         std::uint64_t lookup) {
  const auto entry{m_sources.find(source)};
  if (entry == m_sources.end() || m_generation == 0 ||
      addressGeneration(source, family) != lookup) {
    return false;
  }

  auto &token{family == DiscoveryAddressFamily::kIpv4
                  ? entry->second.m_ipv4_lookup
                  : entry->second.m_ipv6_lookup};
  if (m_next_lookup == 0) {
    m_generation = 0;
    for (auto &[id, value] : m_sources) {
      static_cast<void>(id);
      value.m_addresses.clear();
    }

    return false;
  }

  token = m_next_lookup++;
  std::erase_if(entry->second.m_addresses, [family](const auto &address) {
    return address.m_family == family;
  });

  return true;
}

DiscoveryResult DiscoveryRecords::refreshAddresses() {
  invalidateAddresses();
  if (m_generation == 0) {
    return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                       "address generation exhausted")};
  }

  return {};
}

DiscoveryResult
DiscoveryRecords::storeAddress(std::uint64_t source, std::uint64_t generation,
                               std::uint64_t lookup, std::string_view host,
                               std::uint16_t port, DiscoveryAddress address,
                               bool replace) {
  const auto entry{m_sources.find(source)};
  const auto current{snapshot()};
  const auto normalizedHost{canonicalizeDomain(host)};
  if (entry == m_sources.end() || m_generation == 0 ||
      generation != m_generation ||
      addressGeneration(source, address.m_family) != lookup ||
      (current.m_state != DiscoveryRecordState::kResolving &&
       current.m_state != DiscoveryRecordState::kReady) ||
      !current.m_srv || port != current.m_srv->m_port || !normalizedHost ||
      *normalizedHost != *canonicalizeDomain(current.m_srv->m_target) ||
      (m_interface && address.m_interface &&
       *m_interface != *address.m_interface) ||
      !isUsableAddress(address)) {
    return std::unexpected{recordError(DiscoveryErrorKind::kInvalidArgument,
                                       "invalid or stale address result")};
  }

  auto updated{entry->second.m_addresses};
  if (replace) {
    std::erase_if(updated, [&](const auto &item) {
      return item.m_family == address.m_family;
    });
  }

  if (std::find(updated.begin(), updated.end(), address) == updated.end()) {
    updated.push_back(std::move(address));
  }

  std::set<DiscoveryAddress> distinct{updated.begin(), updated.end()};
  for (const auto &[id, value] : m_sources) {
    if (id != source) {
      distinct.insert(value.m_addresses.begin(), value.m_addresses.end());
    }
  }

  if (distinct.size() > m_max_addresses) {
    return std::unexpected{recordError(DiscoveryErrorKind::kResourceLimit,
                                       "address limit exceeded")};
  }

  entry->second.m_addresses = std::move(updated);

  return {};
}

void DiscoveryRecords::invalidateAddresses() {
  if (m_generation == std::numeric_limits<std::uint64_t>::max()) {
    m_generation = 0;
  } else if (m_generation != 0) {
    ++m_generation;
  }

  for (auto &[id, source] : m_sources) {
    static_cast<void>(id);
    source.m_addresses.clear();
  }
}

DiscoveryRecordSnapshot DiscoveryRecords::snapshot() const {
  DiscoveryRecordSnapshot result{
      DiscoveryRecordState::kUnresolved, m_generation, {}, {}, {}};
  std::map<SrvKey, DiscoverySrv> srv{};
  std::set<std::vector<std::uint8_t>, TxtLess> txt{};
  std::set<DiscoveryAddress> addresses{};
  bool complete{!m_sources.empty()};
  for (const auto &[id, source] : m_sources) {
    static_cast<void>(id);
    complete &= source.m_srv_monitor && source.m_txt_monitor;
    for (const auto &record : source.m_srv) {
      srv.try_emplace(srvKey(record), record);
    }

    for (auto item : source.m_txt) {
      normalizeEmptyTxt(item);
      txt.insert(std::move(item));
    }

    addresses.insert(source.m_addresses.begin(), source.m_addresses.end());
  }

  if (m_generation == 0) {
    result.m_state = DiscoveryRecordState::kGenerationExhausted;

    return result;
  }

  if (!complete) {
    result.m_state = m_sources.empty()
                         ? DiscoveryRecordState::kUnresolved
                         : DiscoveryRecordState::kMonitorIncomplete;

    return result;
  }

  if (srv.size() > 1 || txt.size() > 1) {
    result.m_state = DiscoveryRecordState::kMultipleRecordsUnsupported;

    return result;
  }

  if (!txt.empty()) {
    for (const auto &[id, source] : m_sources) {
      static_cast<void>(id);
      if (!source.m_txt.empty()) {
        result.m_txt_wire = source.m_txt.front();
        break;
      }
    }
  }

  if (srv.empty()) {
    return result;
  }

  result.m_srv = srv.begin()->second;
  if (result.m_srv->m_target == "." || result.m_srv->m_port == 0) {
    result.m_state = DiscoveryRecordState::kUnavailable;

    return result;
  }

  result.m_addresses.assign(addresses.begin(), addresses.end());
  result.m_state = result.m_addresses.empty() ? DiscoveryRecordState::kResolving
                                              : DiscoveryRecordState::kReady;

  return result;
}

} // namespace dmn::detail
