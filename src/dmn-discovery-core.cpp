/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-core.cpp
 * @brief Private DNS-SD validation and backend construction.
 */

#include "dmn-discovery-core.hpp"
#include "dmn-discovery-records.hpp"
#include "dmn-discovery-retry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace dmn::detail {

namespace {

/**
 * @brief Return whether an ASCII byte is an alphabetic service-name byte.
 */
bool isAsciiLetter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

/**
 * @brief Compare observable record values, excluding the callback token.
 */
bool sameRecordView(const DiscoveryRecordSnapshot &left,
                    const DiscoveryRecordSnapshot &right) {
  return left.m_state == right.m_state && left.m_srv == right.m_srv &&
         left.m_txt_wire == right.m_txt_wire &&
         left.m_addresses == right.m_addresses;
}

/**
 * @brief Return whether an ASCII byte is a decimal digit.
 */
bool isAsciiDigit(char value) { return value >= '0' && value <= '9'; }

/**
 * @brief Fold an ASCII letter without applying locale or Unicode rules.
 */
char foldAscii(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value + ('a' - 'A'));
  }

  return value;
}

/**
 * @brief Check UTF-8 encoding without normalizing the instance label.
 *
 * Reject overlong forms, surrogate code points, values above U+10FFFF and
 * ASCII control characters. The caller retains the original byte sequence.
 */
bool isValidUtf8(std::string_view value) {
  std::size_t index{0};
  while (index < value.size()) {
    const auto first{static_cast<unsigned char>(value[index])};
    if (first < 0x20 || first == 0x7f) {
      return false;
    }

    if (first < 0x80) {
      ++index;
      continue;
    }

    std::size_t length{0};
    std::uint32_t codePoint{0};
    std::uint32_t minimum{0};
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      codePoint = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      codePoint = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      codePoint = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }

    if (length > value.size() - index) {
      return false;
    }

    for (std::size_t offset{1}; offset < length; ++offset) {
      const auto next{static_cast<unsigned char>(value[index + offset])};
      if ((next & 0xc0) != 0x80) {
        return false;
      }

      codePoint = (codePoint << 6) | (next & 0x3f);
    }

    if (codePoint < minimum || codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
      return false;
    }

    index += length;
  }

  return true;
}

/**
 * @brief Construct a typed validation error without exposing user payloads.
 */
DiscoveryResult invalidArgument(std::string message) {
  return std::unexpected{
      DiscoveryError{DiscoveryErrorKind::kInvalidArgument, std::move(message)}};
}

} // namespace

DiscoveryResult validateOptions(const DiscoveryOptions &options) {
  const std::array limits{
      options.m_max_browsers,       options.m_max_registrations,
      options.m_max_instances,      options.m_max_native_operations,
      options.m_max_addresses,      options.m_max_txt_entries,
      options.m_max_txt_bytes,      options.m_max_records_per_kind,
      options.m_max_data_events,    options.m_max_registration_events,
      options.m_max_control_events, options.m_max_commands,
  };

  for (const auto limit : limits) {
    if (limit == 0) {
      return invalidArgument("discovery resource limits must be nonzero");
    }
  }

  if (options.m_refresh_timeout <= std::chrono::milliseconds::zero()) {
    return invalidArgument("discovery refresh timeout must be positive");
  }

  return {};
}

bool validateServiceType(std::string_view type) {
  const auto separator{type.find('.')};
  if (separator == std::string_view::npos || separator < 2 || separator > 16 ||
      type.front() != '_') {
    return false;
  }

  const auto transport{type.substr(separator)};
  if (transport.size() != 5 || transport[1] != '_') {
    return false;
  }

  const auto firstTransport{foldAscii(transport[2])};
  const auto secondTransport{foldAscii(transport[3])};
  if (foldAscii(transport[4]) != 'p' ||
      !((firstTransport == 't' && secondTransport == 'c') ||
        (firstTransport == 'u' && secondTransport == 'd'))) {
    return false;
  }

  const auto name{type.substr(1, separator - 1)};
  if ((!isAsciiLetter(name.front()) && !isAsciiDigit(name.front())) ||
      (!isAsciiLetter(name.back()) && !isAsciiDigit(name.back()))) {
    return false;
  }

  bool hasLetter{false};
  bool previousHyphen{false};
  for (const auto value : name) {
    if (isAsciiLetter(value)) {
      hasLetter = true;
    } else if (!isAsciiDigit(value) && value != '-') {
      return false;
    }

    if (value == '-' && previousHyphen) {
      return false;
    }

    previousHyphen = value == '-';
  }

  return hasLetter;
}

bool validateInstanceName(std::string_view name) {
  return !name.empty() && name.size() <= 63 && isValidUtf8(name);
}

bool validateDomain(std::string_view domain) {
  if (domain.empty()) {
    return false;
  }

  if (domain == ".") {
    return true;
  }

  std::size_t labelLength{0};
  std::size_t wireLength{1};
  for (std::size_t index{0}; index < domain.size(); ++index) {
    const auto value{static_cast<unsigned char>(domain[index])};
    if (value == '.') {
      if (labelLength == 0) {
        return false;
      }

      wireLength += labelLength + 1;
      labelLength = 0;
      continue;
    }

    if (value == '\\') {
      if (++index >= domain.size()) {
        return false;
      }

      if (isAsciiDigit(domain[index])) {
        if (domain.size() - index < 3 || !isAsciiDigit(domain[index + 1]) ||
            !isAsciiDigit(domain[index + 2])) {
          return false;
        }

        const auto decoded{(domain[index] - '0') * 100 +
                           (domain[index + 1] - '0') * 10 +
                           (domain[index + 2] - '0')};
        if (decoded > 255) {
          return false;
        }

        index += 2;
      } else if (domain[index] == '\0') {
        return false;
      }
    } else if (value == 0 || value < 0x20 || value == 0x7f) {
      return false;
    }

    if (++labelLength > 63) {
      return false;
    }
  }

  if (labelLength != 0) {
    wireLength += labelLength + 1;
  }

  return wireLength <= 255;
}

bool validateTxt(const DiscoveryTxt &txt, std::size_t maxEntries,
                 std::size_t maxBytes) {
  if (txt.size() > maxEntries) {
    return false;
  }

  std::size_t used{0};
  for (const auto &entry : txt) {
    if (entry.size() > 255 || used >= maxBytes ||
        entry.size() > maxBytes - used - 1) {
      return false;
    }

    used += entry.size() + 1;
  }

  return true;
}

std::expected<std::string, DiscoveryError>
canonicalizeDomain(std::string_view domain) {
  if (!validateDomain(domain)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid DNS domain"}};
  }

  std::string result{};
  std::string label{};
  const auto appendLabel{[&] {
    if (!label.empty()) {
      result.push_back(static_cast<char>(label.size()));
      result += label;
      label.clear();
    }
  }};

  for (std::size_t index{0}; index < domain.size(); ++index) {
    char value{domain[index]};
    if (value == '.') {
      appendLabel();
      continue;
    }

    if (value == '\\') {
      value = domain[++index];
      if (isAsciiDigit(value)) {
        const auto decoded{(value - '0') * 100 +
                           (domain[index + 1] - '0') * 10 +
                           (domain[index + 2] - '0')};
        value = static_cast<char>(decoded);
        index += 2;
      }
    }

    label.push_back(foldAscii(value));
  }

  appendLabel();
  result.push_back('\0');

  return result;
}

DiscoveryBrowser::DiscoveryBrowser(const DiscoveryOptions &options,
                                   Handler handler, Clock clock)
    : m_max_instances{options.m_max_instances}, m_options{options},
      m_handler{std::move(handler)}, m_clock{std::move(clock)},
      m_retries{std::make_unique<DiscoveryRetrySchedule>(
          options.m_max_native_operations)} {}

DiscoveryBrowser::~DiscoveryBrowser() = default;

DiscoveryBrowser::CanonicalKey
DiscoveryBrowser::canonicalize(const DiscoveryServiceKey &key) {
  CanonicalKey result{key.m_instance, key.m_type, {}, key.m_interface};
  for (auto &value : result.m_instance) {
    value = foldAscii(value);
  }

  for (auto &value : result.m_type) {
    value = foldAscii(value);
  }

  // The caller validated the name before entering this helper.
  result.m_domain = *canonicalizeDomain(key.m_domain);

  return result;
}

DiscoveryProvenance DiscoveryBrowser::provenance(const Entry &entry) {
  const auto merge = [&](auto member) -> std::optional<bool> {
    bool allFalse{!entry.m_sources.empty()};
    for (const auto &[contribution, source] : entry.m_sources) {
      static_cast<void>(contribution);
      const auto value{source.m_provenance.*member};
      if (value && *value) {
        return true;
      }

      if (!value) {
        allFalse = false;
      }
    }

    return allFalse ? std::optional<bool>{false} : std::nullopt;
  };

  return {merge(&DiscoveryProvenance::m_cached),
          merge(&DiscoveryProvenance::m_local),
          merge(&DiscoveryProvenance::m_wide_area),
          merge(&DiscoveryProvenance::m_multicast)};
}

DiscoveryBrowser::Entry *
DiscoveryBrowser::findLive(const DiscoveryServiceKey &key,
                           DiscoveryContribution contribution,
                           std::uint64_t generation) {
  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0)) {
    return nullptr;
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return nullptr;
  }

  const auto source{entry->second.m_sources.find(contribution)};
  if (source == entry->second.m_sources.end() ||
      source->second.m_generation != generation) {
    return nullptr;
  }

  return &entry->second;
}

const DiscoveryBrowser::Entry *
DiscoveryBrowser::findLive(const DiscoveryServiceKey &key,
                           DiscoveryContribution contribution,
                           std::uint64_t generation) const {
  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0)) {
    return nullptr;
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return nullptr;
  }

  const auto source{entry->second.m_sources.find(contribution)};
  if (source == entry->second.m_sources.end() ||
      source->second.m_generation != generation) {
    return nullptr;
  }

  return &entry->second;
}

void DiscoveryBrowser::emit(DiscoveryEventKind kind,
                            DiscoveryCandidate candidate) {
  DiscoveryEvent event{kind, m_epoch, ++m_sequence, std::move(candidate)};
  if (m_handler) {
    m_handler(std::move(event));
  }
}

std::expected<std::uint64_t, DiscoveryError>
DiscoveryBrowser::addObservation(const DiscoveryServiceKey &key,
                                 DiscoveryContribution contribution,
                                 DiscoveryProvenance observation) {
  if (m_updating_records) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant browser update"}};
  }

  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid discovery service key"}};
  }

  auto canonical{canonicalize(key)};
  auto entry{m_entries.find(canonical)};
  if (entry != m_entries.end()) {
    const auto source{entry->second.m_sources.find(contribution)};
    if (source != entry->second.m_sources.end()) {
      const auto generation{source->second.m_generation};
      source->second.m_provenance = observation;
      auto current{provenance(entry->second)};
      if (current != entry->second.m_candidate.m_provenance) {
        entry->second.m_candidate.m_provenance = std::move(current);
        emit(DiscoveryEventKind::kChanged, entry->second.m_candidate);
      }

      return generation;
    }
  } else if (m_entries.size() >= m_max_instances) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery instance limit reached"}};
  }

  if (m_next_generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                       "discovery source generation exhausted"}};
  }

  const auto generation{m_next_generation++};
  if (entry == m_entries.end()) {
    auto records{std::make_unique<DiscoveryRecords>(
        m_options.m_max_records_per_kind, m_options.m_max_addresses,
        m_options.m_max_txt_entries, m_options.m_max_txt_bytes,
        key.m_interface)};
    if (!records->addSource(generation)) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "could not add record source"}};
    }

    Entry value{DiscoveryCandidate{key, DiscoveryResolution::kUnresolved,
                                   records->snapshot()},
                {},
                std::move(records),
                {}};
    value.m_sources.emplace(contribution,
                            Entry::Source{generation, observation});
    value.m_candidate.m_provenance = provenance(value);
    auto [inserted,
          created]{m_entries.emplace(std::move(canonical), std::move(value))};
    static_cast<void>(created);
    auto candidate{inserted->second.m_candidate};
    emit(DiscoveryEventKind::kAdded, std::move(candidate));
  } else {
    if (!entry->second.m_records->addSource(generation)) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                            "could not add record source"}};
    }

    entry->second.m_sources.emplace(contribution,
                                    Entry::Source{generation, observation});
    auto current{entry->second.m_records->snapshot()};
    auto currentProvenance{provenance(entry->second)};
    const bool changed{
        !sameRecordView(entry->second.m_candidate.m_records, current) ||
        entry->second.m_candidate.m_provenance != currentProvenance};
    entry->second.m_candidate.m_records = std::move(current);
    entry->second.m_candidate.m_provenance = std::move(currentProvenance);
    if (changed) {
      auto candidate{entry->second.m_candidate};
      emit(DiscoveryEventKind::kChanged, std::move(candidate));
    }
  }

  return generation;
}

bool DiscoveryBrowser::removeObservation(const DiscoveryServiceKey &key,
                                         DiscoveryContribution contribution,
                                         std::uint64_t generation) {
  if (m_updating_records) {
    return false;
  }

  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0)) {
    return false;
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return false;
  }

  const auto source{entry->second.m_sources.find(contribution)};
  if (source == entry->second.m_sources.end() ||
      source->second.m_generation != generation) {
    return false;
  }

  if (!entry->second.m_records->removeSource(generation)) {
    return false;
  }

  m_retries->removeSource(generation);
  const auto previousFailures{entry->second.m_candidate.m_failures.size()};
  std::erase_if(entry->second.m_candidate.m_failures,
                [generation](const DiscoveryFailure &failure) {
                  return failure.m_operation.m_source == generation;
                });
  const bool failuresChanged{previousFailures !=
                             entry->second.m_candidate.m_failures.size()};
  entry->second.m_sources.erase(source);
  if (entry->second.m_sources.empty()) {
    auto candidate{entry->second.m_candidate};
    m_entries.erase(entry);
    emit(DiscoveryEventKind::kRemoved, std::move(candidate));
  } else {
    auto current{entry->second.m_records->snapshot()};
    auto currentProvenance{provenance(entry->second)};
    const bool changed{
        !sameRecordView(entry->second.m_candidate.m_records, current) ||
        entry->second.m_candidate.m_provenance != currentProvenance};
    entry->second.m_candidate.m_records = std::move(current);
    entry->second.m_candidate.m_provenance = std::move(currentProvenance);
    if (changed || failuresChanged) {
      auto candidate{entry->second.m_candidate};
      emit(DiscoveryEventKind::kChanged, std::move(candidate));
    }
  }

  return true;
}

bool DiscoveryBrowser::updateResolution(const DiscoveryServiceKey &key,
                                        DiscoveryContribution contribution,
                                        std::uint64_t generation,
                                        DiscoveryResolution resolution) {
  if (m_updating_records) {
    return false;
  }

  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0)) {
    return false;
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return false;
  }

  const auto source{entry->second.m_sources.find(contribution)};
  if (source == entry->second.m_sources.end() ||
      source->second.m_generation != generation) {
    return false;
  }

  if (entry->second.m_candidate.m_resolution != resolution) {
    entry->second.m_candidate.m_resolution = resolution;
    auto candidate{entry->second.m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));
  }

  return true;
}

DiscoveryResult DiscoveryBrowser::updateRecords(
    const DiscoveryServiceKey &key, DiscoveryContribution contribution,
    std::uint64_t generation,
    const std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)>
        &update) {
  if (m_updating_records) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant browser update"}};
  }

  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0) || !update) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid record update"}};
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "unknown record source"}};
  }

  const auto source{entry->second.m_sources.find(contribution)};
  if (source == entry->second.m_sources.end() ||
      source->second.m_generation != generation) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale record source"}};
  }

  auto updated{*entry->second.m_records};
  DiscoveryResult result{};
  {
    struct UpdateGuard {
      ~UpdateGuard() { m_active = false; }

      bool &m_active;
    };
    m_updating_records = true;
    const UpdateGuard guard{m_updating_records};
    result = update(updated, generation);
  }

  auto current{updated.snapshot()};
  const bool changed{
      !sameRecordView(entry->second.m_candidate.m_records, current)};
  *entry->second.m_records = std::move(updated);
  entry->second.m_candidate.m_records = std::move(current);
  bool refreshChanged{false};
  auto &refresh{entry->second.m_candidate.m_refresh};
  if (refresh && refresh->m_state == DiscoveryRefreshState::kPending) {
    if (entry->second.m_refresh_deadline && m_clock &&
        m_clock() >= *entry->second.m_refresh_deadline) {
      refresh->m_state = DiscoveryRefreshState::kFailed;
      entry->second.m_refresh_deadline.reset();
      refreshChanged = true;
    } else if (entry->second.m_candidate.m_records.m_state ==
               DiscoveryRecordState::kReady) {
      refresh->m_state = DiscoveryRefreshState::kSucceeded;
      entry->second.m_refresh_deadline.reset();
      refreshChanged = true;
    }
  }

  if (changed || refreshChanged) {
    auto candidate{entry->second.m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));
  }

  return result;
}

std::expected<std::uint64_t, DiscoveryError>
DiscoveryBrowser::refresh(const DiscoveryServiceKey &key) {
  if (m_updating_records) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant browser update"}};
  }

  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0) || !m_clock ||
      m_options.m_refresh_timeout <= std::chrono::milliseconds::zero()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid refresh request"}};
  }

  const auto entry{m_entries.find(canonicalize(key))};
  if (entry == m_entries.end()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "unknown discovery service"}};
  }

  if (m_next_refresh == 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "refresh ID exhausted"}};
  }

  auto updated{*entry->second.m_records};
  const auto result{updated.refreshAddresses()};
  auto current{updated.snapshot()};
  const auto now{m_clock()};
  const auto timeout{m_options.m_refresh_timeout};
  const auto maxTimeout{std::chrono::duration_cast<std::chrono::milliseconds>(
      TimePoint::duration::max())};
  auto deadline{TimePoint::max()};
  if (timeout <= maxTimeout) {
    const auto duration{
        std::chrono::duration_cast<TimePoint::duration>(timeout)};
    if (now <= TimePoint::max() - duration) {
      deadline = now + duration;
    }
  }
  *entry->second.m_records = std::move(updated);
  entry->second.m_candidate.m_records = std::move(current);
  if (!result) {
    auto candidate{entry->second.m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));

    return std::unexpected{result.error()};
  }

  const auto id{m_next_refresh++};
  for (const auto &[contribution, source] : entry->second.m_sources) {
    static_cast<void>(contribution);
    m_retries->cancelTimer({source.m_generation, DiscoveryAttempt::kIpv4});
    m_retries->cancelTimer({source.m_generation, DiscoveryAttempt::kIpv6});
  }

  entry->second.m_candidate.m_refresh =
      DiscoveryRefreshStatus{id, DiscoveryRefreshState::kPending};
  entry->second.m_refresh_deadline = deadline;
  auto candidate{entry->second.m_candidate};
  emit(DiscoveryEventKind::kChanged, std::move(candidate));

  return id;
}

DiscoveryResult DiscoveryBrowser::processRefreshDeadlines() {
  if (!m_clock || m_updating_records) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "invalid deadline context"}};
  }

  const auto now{m_clock()};
  std::vector<CanonicalKey> expired{};
  for (const auto &[key, entry] : m_entries) {
    if (entry.m_refresh_deadline && *entry.m_refresh_deadline <= now) {
      expired.push_back(key);
    }
  }

  for (const auto &key : expired) {
    const auto entry{m_entries.find(key)};
    if (entry == m_entries.end() || !entry->second.m_refresh_deadline ||
        *entry->second.m_refresh_deadline > now) {
      continue;
    }

    entry->second.m_refresh_deadline.reset();
    entry->second.m_candidate.m_refresh->m_state =
        DiscoveryRefreshState::kFailed;
    auto candidate{entry->second.m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));
  }

  return {};
}

std::optional<std::uint64_t> DiscoveryBrowser::attemptGeneration(
    const DiscoveryServiceKey &key, DiscoveryContribution contribution,
    std::uint64_t generation, DiscoveryAttempt attempt) const {
  const auto entry{findLive(key, contribution, generation)};
  if (!entry) {
    return std::nullopt;
  }

  switch (attempt) {
  case DiscoveryAttempt::kSrv:
    return entry->m_records->monitorGeneration(generation,
                                               DiscoveryRecordKind::kSrv);
  case DiscoveryAttempt::kTxt:
    return entry->m_records->monitorGeneration(generation,
                                               DiscoveryRecordKind::kTxt);
  case DiscoveryAttempt::kIpv4:
    return entry->m_records->addressGeneration(generation,
                                               DiscoveryAddressFamily::kIpv4);
  case DiscoveryAttempt::kIpv6:
    return entry->m_records->addressGeneration(generation,
                                               DiscoveryAddressFamily::kIpv6);
  }

  return std::nullopt;
}

DiscoveryResult DiscoveryBrowser::failAttempt(
    const DiscoveryServiceKey &key, DiscoveryContribution contribution,
    std::uint64_t generation, DiscoveryAttempt attempt,
    std::uint64_t operationGeneration, std::uint64_t resolutionGeneration,
    bool recoverable, std::string message,
    std::optional<DiscoveryError> native) {
  if (m_updating_records || !m_clock) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "invalid retry context"}};
  }

  auto entry{findLive(key, contribution, generation)};
  const auto token{attemptGeneration(key, contribution, generation, attempt)};
  const bool addressAttempt{attempt == DiscoveryAttempt::kIpv4 ||
                            attempt == DiscoveryAttempt::kIpv6};
  if (!entry || !token || *token != operationGeneration ||
      (addressAttempt &&
       entry->m_records->snapshot().m_generation != resolutionGeneration)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale failed operation"}};
  }

  auto updated{*entry->m_records};
  bool invalidated{false};
  switch (attempt) {
  case DiscoveryAttempt::kSrv:
    invalidated =
        updated.setMonitoring(generation, DiscoveryRecordKind::kSrv, false);
    break;
  case DiscoveryAttempt::kTxt:
    invalidated =
        updated.setMonitoring(generation, DiscoveryRecordKind::kTxt, false);
    break;
  case DiscoveryAttempt::kIpv4:
    invalidated = updated.failAddressLookup(
        generation, DiscoveryAddressFamily::kIpv4, operationGeneration);
    break;
  case DiscoveryAttempt::kIpv6:
    invalidated = updated.failAddressLookup(
        generation, DiscoveryAddressFamily::kIpv6, operationGeneration);
    break;
  }

  if (!invalidated) {
    entry->m_candidate.m_records = updated.snapshot();
    *entry->m_records = std::move(updated);
    auto candidate{entry->m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));

    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "operation generation exhausted"}};
  }

  const DiscoveryAttemptId id{generation, attempt};
  auto retries{*m_retries};
  auto result{retries.fail(id, recoverable, std::move(message), m_clock())};
  auto current{updated.snapshot()};
  *entry->m_records = std::move(updated);
  entry->m_candidate.m_records = std::move(current);
  if (result) {
    auto error{retries.error(id)};
    if (native) {
      error->m_backend_domain = std::move(native->m_backend_domain);
      error->m_backend_code = native->m_backend_code;
      error->m_native_category = native->m_native_category;
    }

    auto &failures{entry->m_candidate.m_failures};
    const auto existing{std::find_if(
        failures.begin(), failures.end(),
        [id](const auto &failure) { return failure.m_operation == id; })};
    if (existing == failures.end()) {
      failures.push_back(DiscoveryFailure{id, *error});
    } else {
      existing->m_error = *error;
    }
    *m_retries = std::move(retries);
  }

  auto candidate{entry->m_candidate};
  emit(DiscoveryEventKind::kChanged, std::move(candidate));

  return result;
}

DiscoveryResult DiscoveryBrowser::succeedAttempt(
    const DiscoveryServiceKey &key, DiscoveryContribution contribution,
    std::uint64_t generation, DiscoveryAttempt attempt,
    std::uint64_t operationGeneration, std::uint64_t resolutionGeneration) {
  if (m_updating_records) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant browser update"}};
  }

  auto entry{findLive(key, contribution, generation)};
  const auto token{attemptGeneration(key, contribution, generation, attempt)};
  const bool addressAttempt{attempt == DiscoveryAttempt::kIpv4 ||
                            attempt == DiscoveryAttempt::kIpv6};
  if (!entry || !token || *token != operationGeneration ||
      (addressAttempt &&
       entry->m_records->snapshot().m_generation != resolutionGeneration)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "stale successful operation"}};
  }

  const DiscoveryAttemptId id{generation, attempt};
  if (m_retries->succeed(id)) {
    std::erase_if(entry->m_candidate.m_failures, [id](const auto &failure) {
      return failure.m_operation == id;
    });
    auto candidate{entry->m_candidate};
    emit(DiscoveryEventKind::kChanged, std::move(candidate));
  }

  return {};
}

std::expected<std::vector<DiscoveryAttemptId>, DiscoveryError>
DiscoveryBrowser::takeDueRetries() {
  if (!m_clock || m_updating_records) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "invalid retry context"}};
  }

  return m_retries->takeDue(m_clock());
}

std::optional<DiscoveryBrowser::TimePoint>
DiscoveryBrowser::nextDeadline() const {
  auto earliest{m_retries->nextDeadline()};
  for (const auto &[key, entry] : m_entries) {
    static_cast<void>(key);
    if (entry.m_refresh_deadline &&
        (!earliest || *entry.m_refresh_deadline < *earliest)) {
      earliest = entry.m_refresh_deadline;
    }
  }

  return earliest;
}

DiscoverySnapshot DiscoveryBrowser::snapshot() const {
  DiscoverySnapshot result{m_epoch, m_sequence, {}};
  result.m_candidates.reserve(m_entries.size());
  for (const auto &[key, entry] : m_entries) {
    static_cast<void>(key);
    result.m_candidates.push_back(entry.m_candidate);
  }

  return result;
}

DiscoveryCore::DiscoveryCore(std::unique_ptr<DiscoveryBackend> backend)
    : m_backend{std::move(backend)} {}

std::expected<std::unique_ptr<DiscoveryCore>, DiscoveryError>
DiscoveryCore::create(const DiscoveryOptions &options,
                      std::unique_ptr<DiscoveryBackend> backend) {
  if (auto result{validateOptions(options)}; !result) {
    return std::unexpected{result.error()};
  }

  if (!backend) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                       "discovery backend must not be null"}};
  }

  try {
    if (auto result{backend->initialize()}; !result) {
      return std::unexpected{result.error()};
    }

    return std::unique_ptr<DiscoveryCore>{
        new DiscoveryCore{std::move(backend)}};
  } catch (const std::bad_alloc &) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                       "discovery initialization allocation failed"}};
  }
}

} // namespace dmn::detail
