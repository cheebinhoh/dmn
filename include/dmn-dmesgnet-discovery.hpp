/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-discovery.hpp
 * @brief Explicit, policy-gated discovery admission for DMesgNet sessions.
 *
 * Discovery is never connected to DMesgNet implicitly. The caller starts the
 * coordinator and supplies a factory that performs authenticated session
 * construction; the factory receives only validated, owned endpoint data.
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
#include <vector>

namespace dmn {

class Dmn_DMesgNetDiscovery {
public:
  class Session {
  public:
    virtual ~Session() = default;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual bool ready() const noexcept { return true; }
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
  /// The factory owns authentication and transport/session construction.
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

    [[nodiscard]] static TransportFactory
    factory(std::string local_node_identity);

    [[nodiscard]] static TransportFactory
    factory(std::string local_node_identity, Options options);
  };

  Dmn_DMesgNetDiscovery(Policy policy, TransportFactory factory,
                        StatusHandler handler = {});
  ~Dmn_DMesgNetDiscovery();
  Dmn_DMesgNetDiscovery(const Dmn_DMesgNetDiscovery &) = delete;
  Dmn_DMesgNetDiscovery &operator=(const Dmn_DMesgNetDiscovery &) = delete;

  void start();
  void shutdown() noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::size_t peerCount() const noexcept;
  [[nodiscard]] Status status() const;

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
