/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-discovery.hpp
 * @brief Explicit, policy-gated discovery admission for DMesgNet sessions.
 *
 * Discovery is never connected to DMesgNet implicitly. The caller starts the
 * coordinator and supplies a session factory; the Phase 9.1
 * EnvironmentTrusted factory constructs an unauthenticated, framed TCP
 * session, while a future authenticated factory may perform TLS/mTLS or an
 * equivalent handshake. Every factory receives only validated, owned endpoint
 * data.
 *
 * Architecture
 * ------------
 * The integration is intentionally a separate layer from both libraries:
 *
 *   Dmn_DiscoveryClient --> Dmn_DMesgNetDiscovery --> TransportFactory
 *          |                         |                     |
 *          |                         |                     +--> Session
 *          |                         `--> bounded peer/retry state
 *          `--> Avahi or Bonjour
 *                                                  |
 *                                                  v
 *                                           Dmn_DMesgNet
 *                                                  |
 *                                                  v
 *                                      Dmn_Io<std::string>
 *
 * The coordinator validates and admits copied discovery candidates but does
 * not own an mDNS backend, parse application configuration or infer trust
 * from TXT data. The factory owns transport construction and authentication.
 * Phase 9.1 uses the environment-trusted TCP adapter; Phase 9.2 can replace
 * only that factory/session boundary without changing discovery or
 * Dmn_DMesgNet.
 */

#ifndef DMN_DMESGNET_DISCOVERY_HPP_
#define DMN_DMESGNET_DISCOVERY_HPP_

#include "dmn-discovery.hpp"
#include "dmn-dmesgnet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmn {

class Dmn_DMesgNetDiscovery {
public:
  class Session {
  public:
    virtual ~Session() = default;

    /** @brief Stop the owned transport and DMesgNet session. */
    virtual void shutdown() noexcept = 0;

    /**
     * @brief Open a handler on the owned DMesgNet session.
     *
     * The returned proxy is invalid when the session does not expose a
     * DMesgNet publisher. Implementations must not retain the arguments.
     */
    [[nodiscard]] virtual Dmn_DMesg::HandlerType
    openHandler(std::string_view name, std::string_view topic) {
      static_cast<void>(name);
      static_cast<void>(topic);
      return {};
    }

    /**
     * @brief Report whether the session completed transport setup.
     *
     * The default is ready for compatibility with simple injected sessions;
     * authenticated adapters should override this until their handshake
     * completes.
     */
    [[nodiscard]] virtual bool ready() const noexcept { return true; }

    /**
     * @brief Report whether the peer was authenticated by the factory.
     *
     * The environment-trusted adapter intentionally returns false through
     * its session implementation.
     */
    [[nodiscard]] virtual bool authenticated() const noexcept { return true; }
  };

  struct Endpoint {
    Dmn_DiscoveryServiceKey key;
    std::string node_identity;
    std::string protocol_version;
    std::string transport;
    std::string hostname;
    std::uint16_t port{};
    Dmn_DiscoveryAddress address;
    std::vector<std::uint8_t> credential;
  };

  struct Policy {
    std::string service_type;
    std::string domain{"local."};
    std::optional<std::uint32_t> interface_index;
    std::string node_identity;
    std::string protocol_version{"1"};
    std::string transport{"tls"};
    std::size_t max_peers{16};
    std::size_t max_txt_entries{32};
    std::size_t max_txt_bytes{4096};
    std::size_t max_field_bytes{256};
    std::size_t max_retries{3};
    std::chrono::milliseconds retry_delay{1000};
  };

  enum class StatusKind {
    Started,
    Stopped,
    Admitted,
    Duplicate,
    Removed,
    Rejected,
    Capacity,
    FactoryFailure,
    RetryScheduled,
    RetryExhausted,
    SessionLost,
    Ready,
    ConnectedUnverified,
  };

  struct Status {
    StatusKind kind{StatusKind::Stopped};
    std::size_t peer_count{};
    std::uint64_t generation{};
    std::string detail;
  };

  struct FactoryError {
    /// Sanitized diagnostic text; credentials and secrets are forbidden.
    std::string detail;
    bool transient{};
  };

  using FactoryResult = std::expected<std::unique_ptr<Session>, FactoryError>;
  /// The factory owns transport construction and, when selected,
  /// authentication.
  using TransportFactory = std::function<FactoryResult(const Endpoint &)>;
  /// The handler must not throw and must not call barrier operations.
  using StatusHandler = std::function<void(const Status &)>;

  /**
   * @brief Environment-trusted TCP adapter for isolated deployments.
   *
   * This adapter provides framing and DMesgNet wiring only. It deliberately
   * performs no cryptographic authentication; callers must use it only when
   * the network boundary is trusted. The future authenticated adapter can
   * reuse the same session and factory boundary.
   */
  class EnvironmentTrusted {
  public:
    struct Options {
      std::size_t max_frame_bytes{1024 * 1024};
      std::size_t max_pending_messages{1024};
      std::size_t max_pending_bytes{4 * 1024 * 1024};
    };

    /**
     * @brief Create a Phase 9.1 environment-trusted transport factory.
     *
     * @param local_node_identity Identity passed to the local DMesgNet node.
     * @return A factory that opens bounded framed TCP sessions.
     * @throws std::invalid_argument if the identity is empty.
     */
    [[nodiscard]] static TransportFactory
    factory(std::string local_node_identity);

    /**
     * @brief Create a Phase 9.1 factory with explicit resource limits.
     *
     * @param local_node_identity Identity passed to the local DMesgNet node.
     * @param options Frame and pending-message bounds.
     * @return A factory that opens bounded framed TCP sessions.
     * @throws std::invalid_argument if an identity or bound is invalid.
     */
    [[nodiscard]] static TransportFactory
    factory(std::string local_node_identity, Options options);
  };

  /**
   * @brief Construct an explicitly controlled discovery coordinator.
   *
   * @param policy Admission filters and resource/retry limits.
   * @param factory Transport/session construction boundary.
   * @param handler Optional serialized status callback.
   */
  Dmn_DMesgNetDiscovery(Policy policy, TransportFactory factory,
                        StatusHandler handler = {});
  ~Dmn_DMesgNetDiscovery();
  Dmn_DMesgNetDiscovery(const Dmn_DMesgNetDiscovery &) = delete;
  Dmn_DMesgNetDiscovery &operator=(const Dmn_DMesgNetDiscovery &) = delete;
  Dmn_DMesgNetDiscovery(Dmn_DMesgNetDiscovery &&) = delete;
  Dmn_DMesgNetDiscovery &operator=(Dmn_DMesgNetDiscovery &&) = delete;

  /** @brief Begin accepting discovery events; repeated calls are harmless. */
  void start();

  /** @brief Stop admission, close owned sessions and clear retry state. */
  void shutdown() noexcept;

  /** @brief Return whether the coordinator currently accepts events. */
  [[nodiscard]] bool running() const noexcept;

  /** @brief Return the number of currently admitted peers. */
  [[nodiscard]] std::size_t peerCount() const noexcept;

  /** @brief Copy the latest coordinator status. */
  [[nodiscard]] Status status() const;

  /**
   * @brief Open a handler on an admitted peer's DMesgNet session.
   *
   * @param key Service key identifying the admitted peer.
   * @param name Handler name.
   * @param topic Topic to receive and publish.
   * @return A handler proxy, or an error when the peer is not admitted or the
   *         session does not expose a DMesgNet publisher.
   */
  [[nodiscard]] std::expected<Dmn_DMesg::HandlerType, std::string>
  openHandler(const Dmn_DiscoveryServiceKey &key, std::string_view name,
              std::string_view topic);

  /** Process one owned browser event. Events before start are ignored. */
  void handle(Dmn_DiscoveryEvent event,
              std::chrono::steady_clock::time_point now =
                  std::chrono::steady_clock::now());

  /**
   * @brief Notify the coordinator that a session was lost.
   *
   * A transient loss is retried only when retry() is called after its due
   * time. The generation must match the currently admitted session.
   */
  void sessionLost(const Dmn_DiscoveryServiceKey &key, std::uint64_t generation,
                   bool transient, std::chrono::steady_clock::time_point now);

  /**
   * @brief Execute retries whose caller-supplied time has arrived.
   *
   * No background thread or wall-clock read is used. This makes retry
   * deterministic for library clients and tests.
   */
  void retry(std::chrono::steady_clock::time_point now);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DMESGNET_DISCOVERY_HPP_
