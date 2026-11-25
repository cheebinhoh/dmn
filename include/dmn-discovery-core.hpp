/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-core.hpp
 * @brief Private, backend-neutral DNS-SD validation and lifecycle seam.
 *
 * This backend-neutral foundation owns no native discovery implementation.
 * Its interfaces are internal and must not be installed as a public API.
 */

#ifndef DMN_DISCOVERY_CORE_HPP_
#define DMN_DISCOVERY_CORE_HPP_

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmn::detail {

/**
 * @brief Private error categories for validation and backend initialization.
 */
enum class DiscoveryErrorKind {
  kInvalidArgument,
  kUnsupported,
  kBackend,
  kResourceLimit,
  kStopped,
  kReentrantCall,
  kCancelled,
  kCallbackFailure
};

enum class DiscoveryNativeErrorCategory {
  kUnavailable,
  kPermission,
  kResourceLimit,
  kUnsupported,
  kCollision
};

/**
 * @brief Owned error without references to native or caller-owned storage.
 */
struct DiscoveryError {
  DiscoveryErrorKind m_kind{DiscoveryErrorKind::kBackend};
  std::string m_message{};
  std::optional<std::string> m_backend_domain{};
  std::optional<std::int64_t> m_backend_code{};
  std::optional<DiscoveryNativeErrorCategory> m_native_category{};
};

/**
 * @brief Success or an owned discovery error.
 */
using DiscoveryResult = std::expected<void, DiscoveryError>;

/**
 * @brief Length-delimited TXT entries; bytes are not interpreted as text.
 */
using DiscoveryTxt = std::vector<std::vector<std::uint8_t>>;

/**
 * @brief An owned, interface-scoped DNS-SD service identity.
 *
 * A missing interface is distinct from every known interface. Instance names
 * retain their display spelling and are not split at literal dots.
 */
struct DiscoveryServiceKey {
  std::string m_instance;
  std::string m_type;
  std::string m_domain;
  std::optional<std::uint32_t> m_interface;
};

/**
 * @brief Native browse contribution, separate from an address family.
 */
enum class DiscoveryContribution { kUnknown, kIpv4, kIpv6 };

/**
 * @brief Initial resolution states used by the private browser model.
 */
enum class DiscoveryResolution { kUnresolved, kResolving };

/**
 * @brief An owned SRV value with a host-order port.
 */
struct DiscoverySrv {
  bool operator==(const DiscoverySrv &) const = default;

  std::uint16_t m_priority;
  std::uint16_t m_weight;
  std::uint16_t m_port;
  std::string m_target;
};

/** @brief Address family of an observed endpoint. */
enum class DiscoveryAddressFamily { kIpv4, kIpv6 };

/** @brief Owned IP address and optional scope interface. */
struct DiscoveryAddress {
  auto operator<=>(const DiscoveryAddress &) const = default;

  DiscoveryAddressFamily m_family;
  std::vector<std::uint8_t> m_bytes;
  std::optional<std::uint32_t> m_interface;
};

/** @brief Kind of independently monitored DNS-SD record. */
enum class DiscoveryRecordKind { kSrv, kTxt };

/** @brief Availability based on currently observed records. */
enum class DiscoveryRecordState {
  kUnresolved,
  kResolving,
  kReady,
  kUnavailable,
  kMonitorIncomplete,
  kMultipleRecordsUnsupported,
  kGenerationExhausted
};

/** @brief Owned record view and address-resolution generation. */
struct DiscoveryRecordSnapshot {
  DiscoveryRecordState m_state{DiscoveryRecordState::kUnresolved};
  std::uint64_t m_generation{1};
  std::optional<DiscoverySrv> m_srv;
  std::optional<std::vector<std::uint8_t>> m_txt_wire;
  std::vector<DiscoveryAddress> m_addresses;
};

/** @brief Result of the latest explicitly requested resolution attempt. */
enum class DiscoveryRefreshState { kPending, kSucceeded, kFailed };

/** @brief Browser-local refresh identifier and its current status. */
struct DiscoveryRefreshStatus {
  bool operator==(const DiscoveryRefreshStatus &) const = default;

  std::uint64_t m_id;
  DiscoveryRefreshState m_state;
};

/** @brief Independent native operation subject to retry. */
enum class DiscoveryAttempt { kSrv, kTxt, kIpv4, kIpv6 };

/** @brief Live contribution and operation identity. */
struct DiscoveryAttemptId {
  auto operator<=>(const DiscoveryAttemptId &) const = default;

  std::uint64_t m_source;
  DiscoveryAttempt m_attempt;
};

/** @brief Last failure of a retryable or permanent operation. */
struct DiscoveryAttemptError {
  bool operator==(const DiscoveryAttemptError &) const = default;

  bool m_recoverable;
  std::string m_message;
  std::optional<std::string> m_backend_domain{};
  std::optional<std::int64_t> m_backend_code{};
  std::optional<DiscoveryNativeErrorCategory> m_native_category{};
};

/** @brief An operation's currently visible failure. */
struct DiscoveryFailure {
  bool operator==(const DiscoveryFailure &) const = default;

  DiscoveryAttemptId m_operation;
  DiscoveryAttemptError m_error;
};

/** @brief Known provenance of a browse observation; unset means unknown. */
struct DiscoveryProvenance {
  bool operator==(const DiscoveryProvenance &) const = default;

  std::optional<bool> m_cached;
  std::optional<bool> m_local;
  std::optional<bool> m_wide_area;
  std::optional<bool> m_multicast;
};

/**
 * @brief Owned service observation, independent of callback buffer lifetime.
 */
struct DiscoveryCandidate {
  DiscoveryServiceKey m_key;
  DiscoveryResolution m_resolution{DiscoveryResolution::kUnresolved};
  DiscoveryRecordSnapshot m_records{};
  std::optional<DiscoveryRefreshStatus> m_refresh{};
  std::vector<DiscoveryFailure> m_failures{};
  DiscoveryProvenance m_provenance{};
};

/**
 * @brief Portable changes produced by the initial browser model.
 */
enum class DiscoveryEventKind { kAdded, kChanged, kRemoved };

/**
 * @brief One ordered, owned browser event.
 */
struct DiscoveryEvent {
  DiscoveryEventKind m_kind;
  std::uint64_t m_epoch;
  std::uint64_t m_sequence;
  DiscoveryCandidate m_candidate;
};

/**
 * @brief Owned point-in-time browser view and its latest assigned sequence.
 */
struct DiscoverySnapshot {
  std::uint64_t m_epoch;
  std::uint64_t m_sequence;
  std::vector<DiscoveryCandidate> m_candidates;
};

/**
 * @brief Finite limits for one discovery client.
 *
 * Limits constrain Dmn-owned resources and do not reserve daemon resources.
 * Every count and the refresh deadline must be greater than zero.
 */
struct DiscoveryOptions {
  std::size_t m_max_browsers{16};
  std::size_t m_max_registrations{16};
  std::size_t m_max_instances{64};
  std::size_t m_max_native_operations{640};
  std::size_t m_max_addresses{32};
  std::size_t m_max_txt_entries{64};
  std::size_t m_max_txt_bytes{4096};
  std::size_t m_max_records_per_kind{8};
  std::size_t m_max_data_events{1024};
  std::size_t m_max_registration_events{32};
  std::size_t m_max_control_events{8};
  std::size_t m_max_commands{256};
  std::chrono::milliseconds m_refresh_timeout{std::chrono::seconds{5}};
};

/**
 * @brief Validate that every configured resource bound is nonzero.
 *
 * @param options Limits owned by the caller.
 * @return Success or an invalid-argument error.
 */
[[nodiscard]] DiscoveryResult validateOptions(const DiscoveryOptions &options);

/**
 * @brief Validate a DNS-SD service type such as `_http._tcp`.
 *
 * @param type Textual service type without a domain suffix.
 * @return Whether the type satisfies DNS-SD service-name constraints.
 */
[[nodiscard]] bool validateServiceType(std::string_view type);

/**
 * @brief Validate one UTF-8 service instance label.
 *
 * @param name Label bytes, not an escaped full DNS name.
 * @return Whether the label is nonempty, valid UTF-8, contains no ASCII
 * controls and is at most 63 bytes.
 */
[[nodiscard]] bool validateInstanceName(std::string_view name);

/**
 * @brief Validate a textual DNS domain, including escaped label bytes.
 *
 * @param domain Domain name with an optional final root dot.
 * @return Whether all labels and the full encoded name fit DNS limits.
 */
[[nodiscard]] bool validateDomain(std::string_view domain);

/**
 * @brief Encode a validated textual DNS name as case-folded wire labels.
 *
 * @param domain Textual DNS domain with optional escaped label bytes.
 * @return Canonical owned wire name or a typed invalid-argument error.
 */
[[nodiscard]] std::expected<std::string, DiscoveryError>
canonicalizeDomain(std::string_view domain);

/**
 * @brief Validate a complete collection of length-delimited TXT entries.
 *
 * The byte limit includes one DNS length octet per entry. Empty entries and
 * binary bytes are allowed; no entry may exceed 255 bytes.
 *
 * @param txt Owned entries to inspect without modification.
 * @param maxEntries Maximum number of entries.
 * @param maxBytes Maximum total encoded TXT bytes.
 * @return Whether all framing and limits are valid.
 */
[[nodiscard]] bool validateTxt(const DiscoveryTxt &txt, std::size_t maxEntries,
                               std::size_t maxBytes);

class DiscoveryRecords;
class DiscoveryRetrySchedule;

/**
 * @brief Private, caller-serialized browser state used by deterministic tests.
 *
 * Contributions merge by DNS-aware key and are independently removed. Each
 * contribution receives an opaque generation, allowing a caller to reject
 * callbacks that arrive after removal and re-addition. This initial model
 * delivers callbacks synchronously; it is not the final threaded public API.
 */
class DiscoveryBrowser {
public:
  using Handler = std::function<void(DiscoveryEvent)>;
  using TimePoint = std::chrono::steady_clock::time_point;
  using Clock = std::function<TimePoint()>;

  /**
   * @brief Construct an empty private browser.
   *
   * @param options Resource limits, validated by the owning core.
   * @param handler Receives owned events synchronously, in sequence order.
   * @param clock Monotonic clock; defaults to the system steady clock.
   */
  DiscoveryBrowser(const DiscoveryOptions &options, Handler handler,
                   Clock clock = std::chrono::steady_clock::now);
  ~DiscoveryBrowser();
  DiscoveryBrowser(const DiscoveryBrowser &) = delete;
  DiscoveryBrowser &operator=(const DiscoveryBrowser &) = delete;

  /**
   * @brief Add one transport contribution to an interface-scoped service.
   *
   * Repeated adds of the same contribution return its existing generation.
   *
   * @param key Service identity, copied into the browser.
   * @param contribution Native observation source.
   * @return Opaque source generation or a typed validation/limit error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError>
  addObservation(const DiscoveryServiceKey &key,
                 DiscoveryContribution contribution,
                 DiscoveryProvenance provenance = {});

  /**
   * @brief Remove a matching live contribution.
   *
   * @param key Service identity.
   * @param contribution Native observation source.
   * @param generation Generation returned when the source was added.
   * @return False for a missing or stale source; true when removed.
   */
  bool removeObservation(const DiscoveryServiceKey &key,
                         DiscoveryContribution contribution,
                         std::uint64_t generation);

  /**
   * @brief Apply a resolution state only while its source is still live.
   *
   * Duplicate states do not generate events.
   *
   * @param key Service identity.
   * @param contribution Native observation source.
   * @param generation Generation returned when the source was added.
   * @param resolution New resolution state.
   * @return False for a missing or stale source; true for a live source.
   */
  bool updateResolution(const DiscoveryServiceKey &key,
                        DiscoveryContribution contribution,
                        std::uint64_t generation,
                        DiscoveryResolution resolution);

  /**
   * @brief Apply one record-model update from a current browse source.
   *
   * The update runs synchronously on the caller-serialized browser. The
   * callback receives the live source token and must not retain the model
   * reference, alter browse-source membership, or re-enter browser mutation
   * methods. An exception from the update leaves the prior state unchanged.
   * Observable changes are published after the update, including changes
   * caused by an unsuccessful update; the session handles publication failures.
   *
   * @param key Service identity.
   * @param contribution Native observation source.
   * @param generation Generation returned when the source was added.
   * @param update Operation on the candidate's record model.
   * @return Update result, or an error if the source is missing or stale.
   */
  [[nodiscard]] DiscoveryResult updateRecords(
      const DiscoveryServiceKey &key, DiscoveryContribution contribution,
      std::uint64_t generation,
      const std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)>
          &update);

  /**
   * @brief Start a new resolution generation for a currently observed key.
   *
   * Existing record watches remain active. Previous addresses are invalidated,
   * and a later refresh supersedes a pending one. The caller schedules native
   * lookups using the generation in the updated candidate.
   *
   * @param key Service identity.
   * @return Browser-local refresh ID or a typed error.
   */
  [[nodiscard]] std::expected<std::uint64_t, DiscoveryError>
  refresh(const DiscoveryServiceKey &key);

  /**
   * @brief Fail pending attempts whose monotonic deadline has passed.
   *
   * The caller drives this from its timer loop; no sleep or thread is owned by
   * this private browser. Background resolution may continue after timeout.
   *
   * @return Success or a typed invalid-context error.
   */
  [[nodiscard]] DiscoveryResult processRefreshDeadlines();

  /**
   * @brief Obtain the active lifetime token for one native operation.
   *
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Browse contribution generation.
   * @param attempt Record watch or address family.
   * @return Operation token, or no value if absent or inactive.
   */
  [[nodiscard]] std::optional<std::uint64_t>
  attemptGeneration(const DiscoveryServiceKey &key,
                    DiscoveryContribution contribution,
                    std::uint64_t generation, DiscoveryAttempt attempt) const;

  /**
   * @brief Record an operation failure and invalidate its usable data.
   *
   * Stale source, operation or address-resolution tokens are rejected.
   * Recoverable errors schedule one bounded-backoff retry; permanent failures
   * remain visible without a timer.
   *
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Browse contribution generation.
   * @param attempt Failed operation.
   * @param operationGeneration Failed operation's lifetime token.
   * @param resolutionGeneration Address resolution generation.
   * @param recoverable Whether automatic retry is allowed.
   * @param message Non-sensitive error detail.
   * @return Success or a typed stale-source/resource error.
   */
  [[nodiscard]] DiscoveryResult
  failAttempt(const DiscoveryServiceKey &key,
              DiscoveryContribution contribution, std::uint64_t generation,
              DiscoveryAttempt attempt, std::uint64_t operationGeneration,
              std::uint64_t resolutionGeneration, bool recoverable,
              std::string message, std::optional<DiscoveryError> native = {});

  /**
   * @brief Clear a current operation's previously reported failure.
   *
   * @param key Service identity.
   * @param contribution Native browse source.
   * @param generation Browse contribution generation.
   * @param attempt Recovered operation.
   * @param operationGeneration Current operation lifetime token.
   * @param resolutionGeneration Current address resolution generation.
   * @return Success or a typed stale-source error.
   */
  [[nodiscard]] DiscoveryResult
  succeedAttempt(const DiscoveryServiceKey &key,
                 DiscoveryContribution contribution, std::uint64_t generation,
                 DiscoveryAttempt attempt, std::uint64_t operationGeneration,
                 std::uint64_t resolutionGeneration);

  /**
   * @brief Collect scheduled retries due at the injected clock's current time.
   *
   * The caller starts each returned operation and reports its result.
   * @return Due operation identities or a typed invalid-context error.
   */
  [[nodiscard]] std::expected<std::vector<DiscoveryAttemptId>, DiscoveryError>
  takeDueRetries();

  /** @brief Earliest retry or refresh deadline for the owning poll loop. */
  [[nodiscard]] std::optional<TimePoint> nextDeadline() const;

  /**
   * @brief Copy the current candidate set and last assigned event sequence.
   *
   * @return An owned point-in-time snapshot.
   */
  [[nodiscard]] DiscoverySnapshot snapshot() const;

private:
  struct CanonicalKey {
    auto operator<=>(const CanonicalKey &) const = default;

    std::string m_instance;
    std::string m_type;
    std::string m_domain;
    std::optional<std::uint32_t> m_interface;
  };

  struct Entry {
    struct Source {
      std::uint64_t m_generation;
      DiscoveryProvenance m_provenance;
    };
    DiscoveryCandidate m_candidate;
    std::map<DiscoveryContribution, Source> m_sources;
    std::unique_ptr<DiscoveryRecords> m_records;
    std::optional<TimePoint> m_refresh_deadline;
  };

  /**
   * @brief Canonicalize a previously validated service identity.
   */
  [[nodiscard]] static CanonicalKey
  canonicalize(const DiscoveryServiceKey &key);
  [[nodiscard]] static DiscoveryProvenance provenance(const Entry &entry);

  [[nodiscard]] Entry *findLive(const DiscoveryServiceKey &key,
                                DiscoveryContribution contribution,
                                std::uint64_t generation);
  [[nodiscard]] const Entry *findLive(const DiscoveryServiceKey &key,
                                      DiscoveryContribution contribution,
                                      std::uint64_t generation) const;

  /**
   * @brief Publish an owned event after committing its corresponding state.
   */
  void emit(DiscoveryEventKind kind, DiscoveryCandidate candidate);

  std::size_t m_max_instances;
  DiscoveryOptions m_options;
  Handler m_handler;
  Clock m_clock;
  std::unique_ptr<DiscoveryRetrySchedule> m_retries;
  std::map<CanonicalKey, Entry> m_entries;
  std::uint64_t m_epoch{1};
  std::uint64_t m_sequence{0};
  std::uint64_t m_next_generation{1};
  std::uint64_t m_next_refresh{1};
  bool m_updating_records{false};
};

/**
 * @brief Private backend seam for deterministic construction tests.
 *
 * A real backend must eventually provide an owned polling context and native
 * operations. This initial interface is not a public abstraction or a
 * success-shaped substitute for an unimplemented native backend.
 */
class DiscoveryBackend {
public:
  virtual ~DiscoveryBackend() = default;

  /**
   * @brief Initialize local backend resources.
   *
   * @return Success or a backend initialization error.
   */
  virtual DiscoveryResult initialize() = 0;

  /**
   * @brief Start an explicitly requested browse.
   *
   * @return Success or a backend error.
   */
  virtual DiscoveryResult browse() = 0;

  /**
   * @brief Start an explicitly requested advertisement.
   *
   * @return Success or a backend error.
   */
  virtual DiscoveryResult advertise() = 0;
};

/**
 * @brief Private owner of one validated, explicitly supplied backend.
 */
class DiscoveryCore {
public:
  /**
   * @brief Validate limits and initialize the supplied backend.
   *
   * No browse or advertisement is started during construction. On failure,
   * the backend is destroyed before the error is returned.
   *
   * @param options Finite resource limits.
   * @param backend Unique backend ownership, which must not be null.
   * @return An owned core or a validation/backend initialization error.
   */
  [[nodiscard]] static std::expected<std::unique_ptr<DiscoveryCore>,
                                     DiscoveryError>
  create(const DiscoveryOptions &options,
         std::unique_ptr<DiscoveryBackend> backend);

  DiscoveryCore(const DiscoveryCore &) = delete;
  DiscoveryCore &operator=(const DiscoveryCore &) = delete;
  DiscoveryCore(DiscoveryCore &&) = delete;
  DiscoveryCore &operator=(DiscoveryCore &&) = delete;
  ~DiscoveryCore() = default;

private:
  explicit DiscoveryCore(std::unique_ptr<DiscoveryBackend> backend);

  std::unique_ptr<DiscoveryBackend> m_backend;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_CORE_HPP_
