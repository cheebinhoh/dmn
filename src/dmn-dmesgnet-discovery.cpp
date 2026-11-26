/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-discovery.cpp
 * @brief Discovery admission and environment-trusted DMesgNet transport.
 *
 * Architecture
 * ------------
 * `Dmn_DMesgNetDiscovery` owns only validated peer/session lifecycle. Discovery
 * backends remain caller-owned and feed copied events into this coordinator.
 * The injected factory is the transport boundary: the Phase 9.1 factory
 * connects a TCP stream, applies bounded length-prefix framing, and supplies
 * one `Dmn_Io<std::string>` to `Dmn_DMesgNet`. `Dmn_DMesgNet` owns its
 * existing `Dmn_Proc` input worker; this adapter does not add another worker.
 *
 * The environment-trusted profile intentionally performs no authentication.
 * A future authenticated factory can reuse the same coordinator and session
 * contract without changing discovery values or the DMesgNet core.
 */

#include "dmn-dmesgnet-discovery.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace dmn {
namespace {

using Key = Dmn_DiscoveryServiceKey;
using Clock = std::chrono::steady_clock;

struct KeyLess {
  bool operator()(const Key &left, const Key &right) const {
    return std::tie(left.instance, left.service_type, left.domain,
                    left.interface_index) <
           std::tie(right.instance, right.service_type, right.domain,
                    right.interface_index);
  }
};

std::string fingerprint(const Dmn_DiscoveryCandidate &candidate) {
  std::ostringstream stream;
  stream << candidate.key.instance << '\n' << candidate.hostname << '\n';
  if (candidate.srv) {
    stream << candidate.srv->target << ':' << candidate.srv->port;
  }
  for (const auto &entry : candidate.txt) {
    stream << '\n' << std::string(entry.begin(), entry.end());
  }
  for (const auto &address : candidate.addresses) {
    stream << '\n'
           << static_cast<int>(address.family) << ':'
           << std::string(address.bytes.begin(), address.bytes.end());
  }
  return stream.str();
}

// Dmn_Pipe is intentionally not used here: it models an in-memory queue, not
// a byte-stream socket. Dmn_DMesgNet already owns the Dmn_Proc reader, so this
// adapter only supplies the transport-specific Dmn_Io implementation.
class TcpIo final : public Dmn_Io<std::string> {
public:
  using Dmn_Io<std::string>::write;

  TcpIo(int fd, std::size_t max_frame_bytes)
      : m_fd{fd}, m_max_frame_bytes{max_frame_bytes} {}

  ~TcpIo() noexcept override { shutdown(); }

  TcpIo(const TcpIo &) = delete;
  TcpIo &operator=(const TcpIo &) = delete;

  auto read() -> std::optional<std::string> override {
    std::uint32_t network_size{};
    if (!readExact(&network_size, sizeof(network_size))) {
      return std::nullopt;
    }

    const auto size = ntohl(network_size);
    if (size > m_max_frame_bytes) {
      throw std::system_error(std::make_error_code(std::errc::message_size),
                              "Dmn_DMesgNet environment transport frame");
    }

    std::string value(size, '\0');
    if (size != 0 && !readExact(value.data(), value.size())) {
      return std::nullopt;
    }
    return value;
  }

  void write(const std::string &value) override {
    if (value.size() > m_max_frame_bytes ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::system_error(std::make_error_code(std::errc::message_size),
                              "Dmn_DMesgNet environment transport frame");
    }

    std::lock_guard lock{m_write_mutex};
    const auto fd = m_fd.load(std::memory_order_acquire);
    if (fd < 0) {
      throw std::system_error(std::make_error_code(std::errc::not_connected),
                              "Dmn_DMesgNet environment transport write");
    }

    const auto network_size = htonl(static_cast<std::uint32_t>(value.size()));
    writeExact(fd, &network_size, sizeof(network_size));
    if (!value.empty()) {
      writeExact(fd, value.data(), value.size());
    }
  }

  void write(std::string &&value) override { write(std::as_const(value)); }

  void shutdown() noexcept override {
    const auto fd = m_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
    }
  }

private:
  static void writeExact(int fd, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    while (size != 0) {
      int flags{};
#ifdef MSG_NOSIGNAL
      flags = MSG_NOSIGNAL;
#endif
      const auto count = ::send(fd, bytes, size, flags);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throw std::system_error(
            count == 0 ? std::make_error_code(std::errc::connection_reset)
                       : std::error_code(errno, std::system_category()),
            "Dmn_DMesgNet environment transport write");
      }
      bytes += count;
      size -= static_cast<std::size_t>(count);
    }
  }

  bool readExact(void *data, std::size_t size) {
    auto *bytes = static_cast<char *>(data);
    while (size != 0) {
      const auto fd = m_fd.load(std::memory_order_acquire);
      if (fd < 0) {
        return false;
      }
      const auto count = ::recv(fd, bytes, size, 0);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count == 0) {
        return false;
      }
      if (count < 0) {
        throw std::system_error(errno, std::system_category(),
                                "Dmn_DMesgNet environment transport read");
      }
      bytes += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  std::atomic<int> m_fd;
  std::size_t m_max_frame_bytes;
  std::mutex m_write_mutex;
};

int connectEndpoint(const Dmn_DMesgNetDiscovery::Endpoint &endpoint) {
  sockaddr_storage address{};
  socklen_t address_size{};
  int family{};

  if (endpoint.address.family == Dmn_DiscoveryAddressFamily::IPv4 &&
      endpoint.address.bytes.size() == sizeof(in_addr)) {
    auto *ipv4 = reinterpret_cast<sockaddr_in *>(&address);
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(endpoint.port);
    std::memcpy(&ipv4->sin_addr, endpoint.address.bytes.data(),
                endpoint.address.bytes.size());
    address_size = sizeof(*ipv4);
    family = AF_INET;
  } else if (endpoint.address.family == Dmn_DiscoveryAddressFamily::IPv6 &&
             endpoint.address.bytes.size() == sizeof(in6_addr)) {
    auto *ipv6 = reinterpret_cast<sockaddr_in6 *>(&address);
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(endpoint.port);
    std::memcpy(&ipv6->sin6_addr, endpoint.address.bytes.data(),
                endpoint.address.bytes.size());
    ipv6->sin6_scope_id = endpoint.address.interface_index.value_or(0);
    address_size = sizeof(*ipv6);
    family = AF_INET6;
  } else {
    throw std::invalid_argument(
        "Dmn_DMesgNet environment transport received an invalid address");
  }

  const auto fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::system_category(),
                            "Dmn_DMesgNet environment transport socket");
  }
  if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                address_size) < 0) {
    const auto error = std::error_code(errno, std::system_category());
    ::close(fd);
    throw std::system_error(error,
                            "Dmn_DMesgNet environment transport connect");
  }
  return fd;
}

class EnvironmentTrustedSession final : public Dmn_DMesgNetDiscovery::Session {
public:
  EnvironmentTrustedSession(
      std::string local_node_identity,
      const Dmn_DMesgNetDiscovery::EnvironmentTrusted::Options &options, int fd)
      : m_io{std::make_shared<TcpIo>(fd, options.max_frame_bytes)},
        m_network{std::make_unique<Dmn_DMesgNet>(
            local_node_identity, m_io, m_io,
            Dmn_DMesgNet::Options{options.max_frame_bytes,
                                  options.max_pending_messages,
                                  options.max_pending_bytes})} {}

  void shutdown() noexcept override {
    if (m_network) {
      m_network.reset();
    }
    m_io->shutdown();
  }

  [[nodiscard]] bool authenticated() const noexcept override { return false; }

private:
  std::shared_ptr<TcpIo> m_io;
  std::unique_ptr<Dmn_DMesgNet> m_network;
};

} // namespace

struct Dmn_DMesgNetDiscovery::Impl {
  struct Peer {
    std::unique_ptr<Session> session;
    Endpoint endpoint;
    std::string fingerprint;
    std::uint64_t generation{};
  };

  struct Retry {
    Endpoint endpoint;
    std::string fingerprint;
    std::uint64_t generation{};
    std::size_t attempts{};
    Clock::time_point due{};
  };

  Policy policy;
  TransportFactory factory;
  StatusHandler handler;
  mutable std::mutex mutex;
  std::map<Key, Peer, KeyLess> peers;
  std::map<Key, Retry, KeyLess> retries;
  std::map<Key, std::uint64_t, KeyLess> generations;
  Status last{};
  std::uint64_t next_generation{};
  bool started{};

  Impl(Policy value, TransportFactory make, StatusHandler notify)
      : policy{std::move(value)}, factory{std::move(make)},
        handler{std::move(notify)} {}

  Status setLocked(StatusKind kind, std::string detail = {},
                   std::uint64_t generation = {}) {
    last = Status{kind, peers.size(), generation, std::move(detail)};
    return last;
  }

  void notify(Status status) const {
    if (handler) {
      handler(status);
    }
  }

  static std::expected<std::map<std::string, std::vector<std::uint8_t>>,
                       std::string>
  metadata(const Dmn_DiscoveryCandidate &candidate, const Policy &policy) {
    if (candidate.txt_state != Dmn_DiscoveryTxtState::Present ||
        candidate.txt.size() > policy.max_txt_entries) {
      return std::unexpected{"TXT metadata is incomplete or oversized"};
    }

    std::size_t bytes{};
    std::map<std::string, std::vector<std::uint8_t>> values;
    for (const auto &raw : candidate.txt) {
      if (raw.size() > policy.max_field_bytes ||
          raw.size() > policy.max_txt_bytes ||
          bytes > policy.max_txt_bytes - raw.size()) {
        return std::unexpected{"TXT metadata exceeds policy limits"};
      }
      bytes += raw.size();

      const auto split = std::find(raw.begin(), raw.end(), '=');
      if (split == raw.end() || split == raw.begin()) {
        return std::unexpected{"malformed TXT metadata"};
      }

      std::string key(raw.begin(), split);
      std::vector<std::uint8_t> value(split + 1, raw.end());
      if (value.empty() || value.size() > policy.max_field_bytes ||
          !values.emplace(std::move(key), std::move(value)).second) {
        return std::unexpected{"duplicate or empty TXT metadata"};
      }
    }
    return values;
  }

  std::expected<Endpoint, std::string>
  validate(const Dmn_DiscoveryCandidate &candidate) const {
    if (candidate.key.service_type != policy.service_type ||
        candidate.key.domain != policy.domain ||
        (policy.interface_index &&
         candidate.key.interface_index != policy.interface_index)) {
      return std::unexpected{"candidate does not match discovery policy"};
    }
    if (candidate.key.instance.empty() ||
        candidate.resolution != Dmn_DiscoveryResolutionState::Resolved ||
        candidate.record_state != Dmn_DiscoveryRecordState::Monitoring ||
        !candidate.srv || candidate.srv->port == 0 ||
        candidate.srv->target.empty() || candidate.srv->target == "." ||
        candidate.hostname.empty() || candidate.addresses.empty()) {
      return std::unexpected{"candidate endpoint is incomplete"};
    }

    const auto usable = [](const Dmn_DiscoveryAddress &address) {
      if (address.family == Dmn_DiscoveryAddressFamily::IPv4) {
        return address.bytes.size() == sizeof(in_addr);
      }
      if (address.bytes.size() != sizeof(in6_addr)) {
        return false;
      }
      const bool link_local{address.bytes[0] == 0xfe &&
                            (address.bytes[1] & 0xc0) == 0x80};
      return !link_local || address.interface_index.has_value();
    };
    if (!std::ranges::any_of(candidate.addresses, usable)) {
      return std::unexpected{"candidate has no usable address"};
    }

    auto values = metadata(candidate, policy);
    if (!values) {
      return std::unexpected{values.error()};
    }

    const auto get = [&](std::string_view name) -> std::optional<std::string> {
      const auto iterator = values->find(std::string{name});
      if (iterator == values->end()) {
        return std::nullopt;
      }
      return std::string(iterator->second.begin(), iterator->second.end());
    };

    const auto node = get("node");
    const auto version = get("version");
    const auto transport = get("transport");
    const auto auth = values->find("auth");
    if (!node || !version || !transport || auth == values->end() ||
        *node != policy.node_identity || *version != policy.protocol_version ||
        *transport != policy.transport || auth->second.empty()) {
      return std::unexpected{"candidate metadata is not accepted by policy"};
    }

    return Endpoint{candidate.key,
                    *node,
                    *version,
                    *transport,
                    candidate.hostname,
                    candidate.srv->port,
                    candidate.addresses.front(),
                    auth->second};
  }

  void scheduleRetryLocked(const Key &key, Endpoint endpoint,
                           std::string candidate_fingerprint,
                           std::uint64_t generation, std::size_t attempts,
                           Clock::time_point now) {
    if (attempts > policy.max_retries) {
      return;
    }
    retries.insert_or_assign(
        key, Retry{std::move(endpoint), std::move(candidate_fingerprint),
                   generation, attempts, now + policy.retry_delay});
  }
};

Dmn_DMesgNetDiscovery::Dmn_DMesgNetDiscovery(Policy policy,
                                             TransportFactory factory,
                                             StatusHandler handler)
    : m_impl{std::make_unique<Impl>(std::move(policy), std::move(factory),
                                    std::move(handler))} {
  if (m_impl->policy.service_type.empty() ||
      m_impl->policy.node_identity.empty() || m_impl->policy.max_peers == 0 ||
      m_impl->policy.max_txt_entries == 0 ||
      m_impl->policy.max_txt_bytes == 0 ||
      m_impl->policy.max_field_bytes == 0 || !m_impl->factory) {
    throw std::invalid_argument("invalid DMesgNet discovery policy or factory");
  }
}

Dmn_DMesgNetDiscovery::~Dmn_DMesgNetDiscovery() { shutdown(); }

Dmn_DMesgNetDiscovery::TransportFactory
Dmn_DMesgNetDiscovery::EnvironmentTrusted::factory(
    std::string local_node_identity) {
  return factory(std::move(local_node_identity), Options{});
}

Dmn_DMesgNetDiscovery::TransportFactory
Dmn_DMesgNetDiscovery::EnvironmentTrusted::factory(
    std::string local_node_identity, Options options) {
  if (local_node_identity.empty() || options.max_frame_bytes == 0 ||
      options.max_pending_messages == 0 || options.max_pending_bytes == 0) {
    throw std::invalid_argument(
        "invalid environment-trusted DMesgNet adapter options");
  }

  return [local_node_identity = std::move(local_node_identity),
          options](const Endpoint &endpoint) -> FactoryResult {
    try {
      const auto fd = connectEndpoint(endpoint);
      return std::make_unique<EnvironmentTrustedSession>(local_node_identity,
                                                         options, fd);
    } catch (const std::exception &error) {
      return std::unexpected(FactoryError{error.what(), true});
    }
  };
}

void Dmn_DMesgNetDiscovery::start() {
  std::optional<Status> notification;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->started) {
      m_impl->started = true;
      notification = m_impl->setLocked(StatusKind::Started);
    }
  }
  if (notification) {
    m_impl->notify(*notification);
  }
}

void Dmn_DMesgNetDiscovery::shutdown() noexcept {
  if (!m_impl) {
    return;
  }

  std::optional<Status> notification;
  std::vector<std::unique_ptr<Session>> sessions;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->started) {
      return;
    }
    m_impl->started = false;
    m_impl->retries.clear();
    m_impl->generations.clear();
    for (auto &entry : m_impl->peers) {
      sessions.push_back(std::move(entry.second.session));
    }
    m_impl->peers.clear();
    notification = m_impl->setLocked(StatusKind::Stopped);
  }
  for (auto &session : sessions) {
    session->shutdown();
  }
  if (notification) {
    m_impl->notify(*notification);
  }
}

bool Dmn_DMesgNetDiscovery::running() const noexcept {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->started;
}

std::size_t Dmn_DMesgNetDiscovery::peerCount() const noexcept {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->peers.size();
}

Dmn_DMesgNetDiscovery::Status Dmn_DMesgNetDiscovery::status() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->last;
}

void Dmn_DMesgNetDiscovery::handle(Dmn_DiscoveryEvent event,
                                   std::chrono::steady_clock::time_point now) {
  if (event.kind == Dmn_DiscoveryEventKind::Removed) {
    if (!event.key) {
      return;
    }

    std::optional<Status> notification;
    std::unique_ptr<Session> session;
    {
      std::lock_guard lock(m_impl->mutex);
      if (!m_impl->started) {
        return;
      }
      m_impl->retries.erase(*event.key);
      const auto iterator = m_impl->peers.find(*event.key);
      if (iterator == m_impl->peers.end() ||
          (event.candidate &&
           iterator->second.fingerprint != fingerprint(*event.candidate))) {
        return;
      }
      session = std::move(iterator->second.session);
      m_impl->peers.erase(iterator);
      m_impl->generations.erase(*event.key);
      notification = m_impl->setLocked(StatusKind::Removed);
    }
    if (session) {
      session->shutdown();
    }
    if (notification) {
      m_impl->notify(*notification);
    }
    return;
  }

  if (event.kind != Dmn_DiscoveryEventKind::Added &&
      event.kind != Dmn_DiscoveryEventKind::Changed) {
    return;
  }
  if (!event.candidate) {
    return;
  }

  const auto candidate_fingerprint = fingerprint(*event.candidate);
  std::optional<Status> notification;
  std::unique_ptr<Session> replaced_session;
  Endpoint endpoint;
  Key key;
  std::uint64_t generation{};
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->started) {
      return;
    }

    const auto validated = m_impl->validate(*event.candidate);
    if (!validated) {
      const auto iterator = m_impl->peers.find(event.candidate->key);
      if (iterator != m_impl->peers.end()) {
        replaced_session = std::move(iterator->second.session);
        m_impl->peers.erase(iterator);
      }
      m_impl->retries.erase(event.candidate->key);
      m_impl->generations.erase(event.candidate->key);
      notification = m_impl->setLocked(StatusKind::Rejected, validated.error());
    } else {
      endpoint = *validated;
      key = endpoint.key;
      const auto iterator = m_impl->peers.find(key);
      if (iterator != m_impl->peers.end() &&
          iterator->second.fingerprint == candidate_fingerprint) {
        notification = m_impl->setLocked(StatusKind::Duplicate);
      } else {
        if (iterator != m_impl->peers.end()) {
          replaced_session = std::move(iterator->second.session);
          m_impl->peers.erase(iterator);
        }
        m_impl->retries.erase(key);
        if (m_impl->peers.size() >= m_impl->policy.max_peers) {
          notification =
              m_impl->setLocked(StatusKind::Capacity, "peer capacity reached");
        } else {
          generation = ++m_impl->next_generation;
          m_impl->generations[key] = generation;
        }
      }
    }
  }

  if (replaced_session) {
    replaced_session->shutdown();
  }
  if (notification) {
    m_impl->notify(*notification);
    if (generation == 0) {
      return;
    }
  }
  if (generation == 0) {
    return;
  }

  auto result = m_impl->factory(endpoint);
  std::unique_ptr<Session> abandoned_session;
  {
    std::lock_guard lock(m_impl->mutex);
    const auto current_generation = m_impl->generations.find(key);
    if (!m_impl->started || current_generation == m_impl->generations.end() ||
        current_generation->second != generation) {
      if (result) {
        abandoned_session = std::move(*result);
      }
    } else if (!result) {
      if (result.error().transient && m_impl->policy.max_retries > 0) {
        m_impl->scheduleRetryLocked(key, endpoint, candidate_fingerprint,
                                    generation, 1, now);
        notification = m_impl->setLocked(StatusKind::RetryScheduled,
                                         result.error().detail);
      } else {
        notification = m_impl->setLocked(StatusKind::FactoryFailure,
                                         result.error().detail);
      }
    } else {
      const auto iterator = m_impl->peers.find(key);
      if (iterator != m_impl->peers.end()) {
        abandoned_session = std::move(*result);
      } else {
        const auto kind = !(*result)->ready() ? StatusKind::Admitted
                          : (*result)->authenticated()
                              ? StatusKind::Ready
                              : StatusKind::ConnectedUnverified;
        m_impl->peers.emplace(key,
                              Impl::Peer{std::move(*result), endpoint,
                                         candidate_fingerprint, generation});
        notification = m_impl->setLocked(kind, {}, generation);
      }
    }
  }

  if (abandoned_session) {
    abandoned_session->shutdown();
  }
  if (notification) {
    m_impl->notify(*notification);
  }
}

void Dmn_DMesgNetDiscovery::sessionLost(
    const Dmn_DiscoveryServiceKey &key, std::uint64_t generation,
    bool transient, std::chrono::steady_clock::time_point now) {
  std::optional<Status> notification;
  std::unique_ptr<Session> session;
  {
    std::lock_guard lock(m_impl->mutex);
    const auto current_generation = m_impl->generations.find(key);
    if (!m_impl->started || current_generation == m_impl->generations.end() ||
        current_generation->second != generation) {
      return;
    }
    const auto iterator = m_impl->peers.find(key);
    if (iterator == m_impl->peers.end() ||
        iterator->second.generation != generation) {
      return;
    }
    auto endpoint = iterator->second.endpoint;
    auto candidate_fingerprint = iterator->second.fingerprint;
    session = std::move(iterator->second.session);
    m_impl->peers.erase(iterator);
    if (transient && m_impl->policy.max_retries > 0) {
      m_impl->scheduleRetryLocked(key, std::move(endpoint),
                                  std::move(candidate_fingerprint), generation,
                                  1, now);
      notification = m_impl->setLocked(StatusKind::RetryScheduled);
    } else {
      notification = m_impl->setLocked(StatusKind::SessionLost, {}, generation);
    }
  }
  if (session) {
    session->shutdown();
  }
  if (notification) {
    m_impl->notify(*notification);
  }
}

void Dmn_DMesgNetDiscovery::retry(std::chrono::steady_clock::time_point now) {
  std::vector<std::pair<Key, Impl::Retry>> due;
  {
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->started) {
      return;
    }
    for (auto iterator = m_impl->retries.begin();
         iterator != m_impl->retries.end();) {
      if (iterator->second.due <= now) {
        due.emplace_back(iterator->first, std::move(iterator->second));
        iterator = m_impl->retries.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  for (auto &[key, retry_state] : due) {
    auto result = m_impl->factory(retry_state.endpoint);
    std::optional<Status> notification;
    std::unique_ptr<Session> abandoned_session;
    {
      std::lock_guard lock(m_impl->mutex);
      const auto current_generation = m_impl->generations.find(key);
      if (!m_impl->started || current_generation == m_impl->generations.end() ||
          current_generation->second != retry_state.generation) {
        if (result) {
          abandoned_session = std::move(*result);
        }
      } else if (!result) {
        if (result.error().transient &&
            retry_state.attempts < m_impl->policy.max_retries) {
          m_impl->scheduleRetryLocked(
              key, retry_state.endpoint, retry_state.fingerprint,
              retry_state.generation, retry_state.attempts + 1, now);
          notification = m_impl->setLocked(StatusKind::RetryScheduled,
                                           result.error().detail);
        } else {
          notification = m_impl->setLocked(StatusKind::RetryExhausted,
                                           result.error().detail);
        }
      } else if (m_impl->peers.contains(key)) {
        abandoned_session = std::move(*result);
      } else {
        const auto kind = !(*result)->ready() ? StatusKind::Admitted
                          : (*result)->authenticated()
                              ? StatusKind::Ready
                              : StatusKind::ConnectedUnverified;
        m_impl->peers.emplace(
            key, Impl::Peer{std::move(*result), retry_state.endpoint,
                            retry_state.fingerprint, retry_state.generation});
        notification = m_impl->setLocked(kind, {}, retry_state.generation);
      }
    }
    if (abandoned_session) {
      abandoned_session->shutdown();
    }
    if (notification) {
      m_impl->notify(*notification);
    }
  }
}

} // namespace dmn
