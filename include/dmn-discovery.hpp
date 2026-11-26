/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery.hpp
 * @brief Platform-neutral DNS-SD discovery API declarations and owned values.
 *
 * API Boundary
 * ------------
 * This header defines the portable application-facing contract for explicit
 * service browsing and registration. It exposes no Avahi, Bonjour, D-Bus or
 * other native types. The optional Linux implementation uses Avahi; the
 * macOS implementation uses Bonjour.
 *
 * Ownership and Threading
 * -----------------------
 * Values crossing the API boundary own their strings and byte sequences.
 * Client, browser and registration operations are intended to be
 * thread-safe. A client owns its state worker and serialized callback worker;
 * native callbacks do not run application handlers. Handlers must not throw,
 * block indefinitely, destroy a handle/client, or call barrier operations
 * such as clearCache(), stop() or shutdown(). A callback may run before the
 * call that accepted it returns. See the discovery specification for complete
 * lifecycle and drain-barrier semantics.
 *
 * Discovery is a local observation, not an inventory, reachability check or
 * trusted identity. TXT data is opaque untrusted data.
 */

#ifndef DMN_DISCOVERY_HPP_
#define DMN_DISCOVERY_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dmn {

/**
 * @brief Stable Dmn category for a discovery operation failure.
 *
 * Callers should branch on this category rather than parse human-readable
 * detail strings. Backend-specific codes, when available, are carried
 * separately by Dmn_DiscoveryError.
 */
enum class Dmn_DiscoveryErrorKind {
  InvalidArgument,
  Unsupported,
  BackendUnavailable,
  Permission,
  ResourceLimit,
  Resolution,
  Collision,
  Cancelled,
  Internal,
  CallbackFailure,
  ReentrantCall,
  Stopped
};

/**
 * @brief Owned discovery failure with an optional native error identity.
 */
struct Dmn_DiscoveryError {
  Dmn_DiscoveryErrorKind kind{Dmn_DiscoveryErrorKind::Internal};
  std::string detail;
  std::optional<std::string> backend_domain;
  std::optional<std::int64_t> backend_code;
};

/** @brief Result of an operation that has no success value. */
using Dmn_DiscoveryResult = std::expected<void, Dmn_DiscoveryError>;

/**
 * @brief Result of an operation that returns an owned value.
 *
 * @tparam T Type of the success value.
 */
template <typename T>
using Dmn_DiscoveryValueResult = std::expected<T, Dmn_DiscoveryError>;

/** @brief Address family requested or observed by a discovery operation. */
enum class Dmn_DiscoveryAddressFamily { IPv4, IPv6 };

/**
 * @brief One owned IP address and its optional IPv6 scope interface.
 *
 * `bytes` contains exactly four bytes for IPv4 or sixteen for IPv6.
 * `interface_index` is the scope for a scoped address when known; an
 * IPv6 link-local address without a known scope is not a usable endpoint.
 */
struct Dmn_DiscoveryAddress {
  Dmn_DiscoveryAddressFamily family{Dmn_DiscoveryAddressFamily::IPv4};
  std::vector<std::uint8_t> bytes;
  std::optional<std::uint32_t> interface_index;
};

/**
 * @brief Portable identity of one DNS-SD instance.
 *
 * The display spelling is retained. Instance labels are not split on literal
 * dots. A missing interface index represents an interface-unknown result,
 * distinct from every known interface. Identity comparison is DNS-aware and
 * ASCII case-insensitive; ordinary Unicode normalization is not implied.
 */
struct Dmn_DiscoveryServiceKey {
  std::string instance;
  std::string service_type;
  std::string domain;
  std::optional<std::uint32_t> interface_index;
};

/**
 * @brief One SRV record observed for a service.
 *
 * `port` is in host byte order. The target is retained as an owned display
 * name; a target of "." or port zero does not describe a usable endpoint.
 */
struct Dmn_DiscoverySrvRecord {
  std::uint16_t priority{};
  std::uint16_t weight{};
  std::uint16_t port{};
  std::string target;
};

/** @brief Presence information for independently monitored TXT data. */
enum class Dmn_DiscoveryTxtState { Unknown, Absent, Present };

/**
 * @brief One opaque, length-delimited TXT entry.
 *
 * The bytes are not NUL-terminated text and must not be interpreted as
 * trusted configuration or credentials.
 */
using Dmn_DiscoveryTxtEntry = std::vector<std::uint8_t>;

/**
 * @brief Current status of SRV/TXT record monitoring for one service.
 */
enum class Dmn_DiscoveryRecordState {
  Unresolved,
  Monitoring,
  MonitoringIncomplete,
  MultipleRecordsUnsupported,
  RecordConflict = MultipleRecordsUnsupported,
  Unavailable
};

/** @brief State of the most recently requested address-resolution attempt. */
enum class Dmn_DiscoveryRefreshState { Pending, Succeeded, Failed };

/** @brief Browser-local refresh identifier and its current status. */
struct Dmn_DiscoveryRefreshStatus {
  std::uint64_t id{};
  Dmn_DiscoveryRefreshState state{Dmn_DiscoveryRefreshState::Pending};
};

/** @brief Independently monitored operation that reported a failure. */
enum class Dmn_DiscoveryOperation {
  ServiceRecord,
  TextRecord,
  IPv4Address,
  IPv6Address
};

/** @brief Failure retained for one record watch or address lookup. */
struct Dmn_DiscoveryFailure {
  Dmn_DiscoveryOperation operation{Dmn_DiscoveryOperation::ServiceRecord};
  Dmn_DiscoveryError error;
  bool recoverable{};
};

/**
 * @brief Optional observation provenance supplied by a native backend.
 *
 * An unset member means unknown, not false. For multiple active sources,
 * a member is true if any source says true, false if all say false, and
 * unknown otherwise. Provenance is observation metadata, not authentication.
 */
struct Dmn_DiscoveryProvenance {
  std::optional<bool> cached;
  std::optional<bool> local;
  std::optional<bool> wide_area;
  std::optional<bool> multicast;
};

/** @brief Resolution state of an observed DNS-SD instance. */
enum class Dmn_DiscoveryResolutionState {
  Unresolved,
  Resolving,
  Partial,
  Resolved,
  Unavailable
};

/**
 * @brief Owned observation of one interface-scoped service instance.
 *
 * Candidate values are local observations. An absent address is not proof
 * that a service or host is down. `addresses` contains only currently
 * observed, valid results; no address is fabricated for an unresolved
 * service.
 */
struct Dmn_DiscoveryCandidate {
  Dmn_DiscoveryServiceKey key;
  Dmn_DiscoveryResolutionState resolution{
      Dmn_DiscoveryResolutionState::Unresolved};
  Dmn_DiscoveryRecordState record_state{Dmn_DiscoveryRecordState::Unresolved};
  std::optional<Dmn_DiscoverySrvRecord> srv;
  std::string hostname;
  std::vector<Dmn_DiscoveryAddress> addresses;
  Dmn_DiscoveryTxtState txt_state{Dmn_DiscoveryTxtState::Unknown};
  std::vector<Dmn_DiscoveryTxtEntry> txt;
  std::optional<Dmn_DiscoveryRefreshStatus> refresh;
  std::vector<Dmn_DiscoveryFailure> failures;
  Dmn_DiscoveryProvenance provenance;
};

/** @brief Persistent state of a browser's local view and delivery path. */
enum class Dmn_DiscoveryBrowserState {
  Running,
  BackendUnavailable,
  Overflow,
  CallbackFailure,
  Failed,
  Stopped
};

/**
 * @brief An owned point-in-time local browser view.
 *
 * `sequence` is the last event sequence assigned when this snapshot was
 * taken; taking a snapshot does not increment it. The snapshot is not a
 * network-wide inventory.
 */
struct Dmn_DiscoverySnapshot {
  Dmn_DiscoveryBrowserState state{Dmn_DiscoveryBrowserState::Running};
  std::uint64_t epoch{};
  std::uint64_t sequence{};
  std::vector<Dmn_DiscoveryCandidate> candidates;
  std::optional<Dmn_DiscoveryError> error;
};

/** @brief Data or lifecycle transition delivered to a browser handler. */
enum class Dmn_DiscoveryEventKind {
  Added,
  Changed,
  Removed,
  Reset,
  BackendAvailable,
  BackendUnavailable,
  Error
};

/**
 * @brief One ordered, owned browser event.
 *
 * `candidate` is populated for service deltas; `key` may identify an
 * operation-specific error. Control events need not carry either. Event
 * sequence numbers are browser-local and monotonic within the browser
 * lifetime; consumers should resnapshot after Reset or a detected gap.
 */
struct Dmn_DiscoveryEvent {
  Dmn_DiscoveryEventKind kind{Dmn_DiscoveryEventKind::Error};
  std::uint64_t epoch{};
  std::uint64_t sequence{};
  std::optional<Dmn_DiscoveryServiceKey> key;
  std::optional<Dmn_DiscoveryCandidate> candidate;
  std::optional<Dmn_DiscoveryError> error;
  Dmn_DiscoveryBrowserState browser_state{Dmn_DiscoveryBrowserState::Running};
};

/** @brief Callback receiving an owned browser event by value. */
using Dmn_DiscoveryEventHandler = std::function<void(Dmn_DiscoveryEvent)>;

/**
 * @brief Explicit query for one DNS-SD service type.
 *
 * An empty interface selection requests all available interfaces. The
 * default domain is the explicit mDNS domain `local.`; other domains require
 * an explicit caller choice. Browsing is never started implicitly.
 */
struct Dmn_DiscoveryQuery {
  std::string service_type;
  std::string domain{"local."};
  std::optional<std::uint32_t> interface_index;
  std::vector<Dmn_DiscoveryAddressFamily> address_families{
      Dmn_DiscoveryAddressFamily::IPv4, Dmn_DiscoveryAddressFamily::IPv6};
};

/**
 * @brief Caller-owned description of an explicitly advertised service.
 *
 * The backend chooses the host target. Advertising does not open a socket,
 * and a successful local acceptance does not mean that registration has
 * completed or that another host has discovered the service.
 */
struct Dmn_DiscoveryAdvertisement {
  Dmn_DiscoveryServiceKey key;
  std::uint16_t port{};
  std::vector<Dmn_DiscoveryTxtEntry> txt;
};

/** @brief Current locally observed state of one requested registration. */
enum class Dmn_DiscoveryRegistrationState {
  Registering,
  Established,
  Collision,
  Unavailable,
  Failed,
  Overflow,
  CallbackFailure,
  Stopped
};

/**
 * @brief Owned registration state, including its last reported failure.
 */
struct Dmn_DiscoveryRegistrationStatus {
  Dmn_DiscoveryRegistrationState state{
      Dmn_DiscoveryRegistrationState::Registering};
  std::optional<Dmn_DiscoveryError> error;
};

/** @brief Callback receiving an owned registration transition by value. */
using Dmn_DiscoveryRegistrationHandler =
    std::function<void(Dmn_DiscoveryRegistrationStatus)>;

/**
 * @brief Finite resource limits and refresh-attempt deadline for a client.
 *
 * These bound Dmn-owned resources and do not reserve daemon, operating
 * system, or network resources. Every count and the deadline must be
 * positive. Native-library and operating-system allocations are outside
 * these limits.
 */
struct Dmn_DiscoveryOptions {
  std::size_t max_browsers{16};
  std::size_t max_registrations{16};
  std::size_t max_instances_per_browser{64};
  std::size_t max_native_operations_per_browser{640};
  std::size_t max_addresses_per_instance{32};
  std::size_t max_txt_entries_per_instance{64};
  std::size_t max_txt_bytes_per_instance{4096};
  std::size_t max_records_per_kind_per_instance{8};
  std::size_t max_data_events_per_browser{1024};
  std::size_t max_registration_events{32};
  std::size_t max_control_events_per_browser{8};
  std::size_t max_queued_commands{256};
  std::chrono::milliseconds refresh_timeout{std::chrono::seconds{5}};
};

class Dmn_DiscoveryClient;

/**
 * @brief Explicit browse session with an owned snapshot and event stream.
 *
 * The browser owns its local view only. clearCache() resets this browser's
 * view and work; it does not flush a system daemon cache or affect another
 * browser. stop() is an idempotent callback-drain barrier. Snapshot state
 * remains inspectable after stop or client shutdown.
 */
class Dmn_DiscoveryBrowser {
public:
  /** @brief Attempt to stop an initialized browser and release its handle. */
  ~Dmn_DiscoveryBrowser();
  Dmn_DiscoveryBrowser(const Dmn_DiscoveryBrowser &) = delete;
  Dmn_DiscoveryBrowser &operator=(const Dmn_DiscoveryBrowser &) = delete;
  Dmn_DiscoveryBrowser(Dmn_DiscoveryBrowser &&) = delete;
  Dmn_DiscoveryBrowser &operator=(Dmn_DiscoveryBrowser &&) = delete;

  /**
   * @brief Copy the current owned browser view and lifecycle status.
   *
   * @return The snapshot or an error.
   */
  [[nodiscard]] Dmn_DiscoveryValueResult<Dmn_DiscoverySnapshot>
  snapshot() const;

  /**
   * @brief Request a new bounded resolution attempt for a present key.
   *
   * The returned identifier means the request was accepted locally, not that
   * an address or successful network query is guaranteed.
   *
   * @param key Service to refresh.
   * @return The accepted attempt identifier or an error.
   */
  [[nodiscard]] Dmn_DiscoveryValueResult<std::uint64_t>
  refresh(const Dmn_DiscoveryServiceKey &key);

  /**
   * @brief Reset only this browser's local view and resubmit its query.
   *
   * Returns after local invalidation and restart submission, not after
   * network responses or callback delivery. Not permitted from this
   * browser's callback context.
   *
   * @return Success or an error.
   */
  [[nodiscard]] Dmn_DiscoveryResult clearCache();

  /**
   * @brief Stop this browser, invalidate work and drain started callbacks.
   *
   * Repeated calls are safe. Not permitted from this browser's callback
   * context.
   *
   * @return Success or an error.
   */
  [[nodiscard]] Dmn_DiscoveryResult stop();

private:
  struct Impl;
  explicit Dmn_DiscoveryBrowser(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> m_impl;

  friend class Dmn_DiscoveryClient;
};

/**
 * @brief Explicitly requested service registration.
 *
 * stop() withdraws only this registration and drains any callback already
 * started. It does not emit a stopped callback. Status remains inspectable
 * after stop or client shutdown.
 */
class Dmn_DiscoveryRegistration {
public:
  /** @brief Attempt to stop an initialized registration and release its handle.
   */
  ~Dmn_DiscoveryRegistration();
  Dmn_DiscoveryRegistration(const Dmn_DiscoveryRegistration &) = delete;
  Dmn_DiscoveryRegistration &
  operator=(const Dmn_DiscoveryRegistration &) = delete;
  Dmn_DiscoveryRegistration(Dmn_DiscoveryRegistration &&) = delete;
  Dmn_DiscoveryRegistration &
  operator=(Dmn_DiscoveryRegistration &&) = delete;

  /**
   * @brief Copy the current owned registration state and last error.
   *
   * @return The registration status or an error.
   */
  [[nodiscard]]
  Dmn_DiscoveryValueResult<Dmn_DiscoveryRegistrationStatus> status() const;

  /**
   * @brief Withdraw this registration and drain started callbacks.
   *
   * @return Success or an error.
   */
  [[nodiscard]] Dmn_DiscoveryResult stop();

private:
  struct Impl;
  explicit Dmn_DiscoveryRegistration(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> m_impl;

  friend class Dmn_DiscoveryClient;
};

/**
 * @brief Owner of explicitly created browsers, registrations and workers.
 *
 * Client construction configures local resources only; daemon absence alone
 * need not make construction fail. Backend availability and operation
 * outcomes are reported through typed errors and events.
 */
class Dmn_DiscoveryClient {
public:
  /**
   * @brief Create a client with finite, validated resource limits.
   *
   * No browse or registration is started by this factory.
   *
   * @param options Resource limits and refresh deadline.
   * @return The client or an error.
   */
  [[nodiscard]] static Dmn_DiscoveryValueResult<
      std::unique_ptr<Dmn_DiscoveryClient>>
  create(Dmn_DiscoveryOptions options = {});

  /** @brief Attempt to shut down the client and release its workers. */
  ~Dmn_DiscoveryClient();
  Dmn_DiscoveryClient(const Dmn_DiscoveryClient &) = delete;
  Dmn_DiscoveryClient &operator=(const Dmn_DiscoveryClient &) = delete;
  Dmn_DiscoveryClient(Dmn_DiscoveryClient &&) = delete;
  Dmn_DiscoveryClient &operator=(Dmn_DiscoveryClient &&) = delete;

  /**
   * @brief Accept an explicit browse query and owned event handler.
   *
   * Acceptance is local submission, not a promise of backend availability
   * or discovered services. The handler may be called before this function
   * returns and is serialized with this client's other callbacks.
   *
   * @param query Service query to submit.
   * @param handler Callback receiving owned events.
   * @return The browser or an error.
   */
  [[nodiscard]] Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryBrowser>>
  browse(Dmn_DiscoveryQuery query, Dmn_DiscoveryEventHandler handler);

  /**
   * @brief Accept an explicit advertisement and owned status handler.
   *
   * Acceptance is not registration completion. A name collision is reported
   * and the instance is not silently renamed.
   *
   * @param advertisement Service to advertise.
   * @param handler Callback receiving registration status.
   * @return The registration or an error.
   */
  [[nodiscard]] Dmn_DiscoveryValueResult<
      std::unique_ptr<Dmn_DiscoveryRegistration>>
  advertise(Dmn_DiscoveryAdvertisement advertisement,
            Dmn_DiscoveryRegistrationHandler handler);

  /**
   * @brief Stop all owned handles and drain/join the client's workers.
   *
   * Repeated calls are safe. Not permitted from this client's callback
   * context.
   *
   * @return Success or an error.
   */
  [[nodiscard]] Dmn_DiscoveryResult shutdown();

private:
  struct Impl;
  explicit Dmn_DiscoveryClient(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DISCOVERY_HPP_
