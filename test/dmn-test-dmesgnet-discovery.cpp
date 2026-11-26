#include "dmn-dmesgnet-discovery.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Coordinator = dmn::Dmn_DMesgNetDiscovery;

struct FakeSession final : Coordinator::Session {
  explicit FakeSession(int &closed) : closed{closed} {}
  void shutdown() noexcept override { ++closed; }
  int &closed;
};

struct Fixture {
  int created{};
  int closed{};
  Coordinator::Policy policy{"_dmn._tcp", "local.", 2, "node-a", "1",
                             "tls",       1,        8, 128,      64};

  dmn::Dmn_DiscoveryCandidate candidate() const {
    dmn::Dmn_DiscoveryCandidate c;
    c.key = {"peer", "_dmn._tcp", "local", 2};
    c.resolution = dmn::Dmn_DiscoveryResolutionState::Resolved;
    c.record_state = dmn::Dmn_DiscoveryRecordState::Monitoring;
    c.srv = dmn::Dmn_DiscoverySrvRecord{0, 0, 443, "peer.local."};
    c.hostname = "peer.local.";
    c.addresses.push_back(
        {dmn::Dmn_DiscoveryAddressFamily::IPv6,
         {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
         std::nullopt});
    c.addresses.push_back(
        {dmn::Dmn_DiscoveryAddressFamily::IPv4, {127, 0, 0, 1}, 2});
    c.txt_state = dmn::Dmn_DiscoveryTxtState::Present;
    c.txt = {bytes("node=node-a"), bytes("version=1"), bytes("transport=tls"),
             bytes("auth=credential")};
    return c;
  }

  static dmn::Dmn_DiscoveryTxtEntry bytes(const std::string &value) {
    return {value.begin(), value.end()};
  }
};

dmn::Dmn_DiscoveryEvent added(dmn::Dmn_DiscoveryCandidate candidate) {
  dmn::Dmn_DiscoveryEvent event;
  event.kind = dmn::Dmn_DiscoveryEventKind::Added;
  event.key = candidate.key;
  event.candidate = std::move(candidate);
  return event;
}

TEST(DmnDmesgNetDiscovery, ValidatesMetadataAndDeduplicates) {
  Fixture f;
  Coordinator coordinator(f.policy, [&](const Coordinator::Endpoint &endpoint) {
    EXPECT_EQ(endpoint.node_identity, "node-a");
    ++f.created;
    return Coordinator::FactoryResult{std::make_unique<FakeSession>(f.closed)};
  });
  coordinator.handle(added(f.candidate()));
  coordinator.handle(added(f.candidate()));
  EXPECT_EQ(f.created, 0);
  EXPECT_EQ(coordinator.peerCount(), 0u); // ignored before explicit start
  coordinator.start();
  coordinator.handle(added(f.candidate()));
  coordinator.handle(added(f.candidate()));
  EXPECT_EQ(f.created, 1);
  EXPECT_EQ(coordinator.peerCount(), 1u);
  auto invalid = f.candidate();
  invalid.txt[0] = Fixture::bytes("node=other");
  coordinator.handle(added(std::move(invalid)));
  EXPECT_EQ(coordinator.status().kind, Coordinator::StatusKind::Rejected);
}

TEST(DmnDmesgNetDiscovery, RemovesStaleGenerationAndClosesOnce) {
  Fixture f;
  Coordinator coordinator(f.policy, [&](const Coordinator::Endpoint &) {
    ++f.created;
    return Coordinator::FactoryResult{std::make_unique<FakeSession>(f.closed)};
  });
  coordinator.start();
  auto c = f.candidate();
  coordinator.handle(added(c));
  auto changed = c;
  changed.srv->port = 444;
  coordinator.handle(added(changed));
  EXPECT_EQ(f.created, 2);
  EXPECT_EQ(f.closed, 1);
  dmn::Dmn_DiscoveryEvent removed;
  removed.kind = dmn::Dmn_DiscoveryEventKind::Removed;
  removed.key = c.key;
  removed.candidate = c;
  coordinator.handle(std::move(removed));
  EXPECT_EQ(coordinator.peerCount(), 1u);
  removed.key = changed.key;
  removed.candidate = changed;
  coordinator.handle(std::move(removed));
  EXPECT_EQ(f.closed, 2);
}

TEST(DmnDmesgNetDiscovery, RejectsChangedInvalidEndpointAndClosesPeer) {
  Fixture f;
  Coordinator coordinator(f.policy, [&](const Coordinator::Endpoint &) {
    ++f.created;
    return Coordinator::FactoryResult{std::make_unique<FakeSession>(f.closed)};
  });
  coordinator.start();
  auto candidate{f.candidate()};
  coordinator.handle(added(candidate));
  ASSERT_EQ(coordinator.peerCount(), 1u);

  candidate.record_state = dmn::Dmn_DiscoveryRecordState::MonitoringIncomplete;
  dmn::Dmn_DiscoveryEvent changed;
  changed.kind = dmn::Dmn_DiscoveryEventKind::Changed;
  changed.key = candidate.key;
  changed.candidate = candidate;
  coordinator.handle(std::move(changed));

  EXPECT_EQ(coordinator.status().kind, Coordinator::StatusKind::Rejected);
  EXPECT_EQ(coordinator.peerCount(), 0u);
  EXPECT_EQ(f.closed, 1);
}

TEST(DmnDmesgNetDiscovery, EnforcesCapacityAndShutdownLifecycle) {
  Fixture f;
  f.policy.max_peers = 1;
  Coordinator coordinator(f.policy, [&](const Coordinator::Endpoint &) {
    ++f.created;
    return Coordinator::FactoryResult{std::make_unique<FakeSession>(f.closed)};
  });
  coordinator.start();
  coordinator.handle(added(f.candidate()));
  auto second = f.candidate();
  second.key.instance = "second";
  coordinator.handle(added(second));
  EXPECT_EQ(coordinator.status().kind, Coordinator::StatusKind::Capacity);
  EXPECT_EQ(coordinator.peerCount(), 1u);
  coordinator.shutdown();
  EXPECT_FALSE(coordinator.running());
  EXPECT_EQ(f.closed, 1);
  coordinator.handle(added(second));
  EXPECT_EQ(f.created, 1);
}

TEST(DmnDmesgNetDiscovery, RetriesTransientFactoryFailuresWithCallerTime) {
  Fixture f;
  f.policy.retry_delay = std::chrono::milliseconds{10};
  f.policy.max_retries = 2;
  std::size_t attempts{};
  Coordinator coordinator(f.policy, [&](const Coordinator::Endpoint &) {
    ++attempts;
    if (attempts == 1) {
      return Coordinator::FactoryResult{
          std::unexpected(Coordinator::FactoryError{"temporary", true})};
    }
    return Coordinator::FactoryResult{std::make_unique<FakeSession>(f.closed)};
  });

  coordinator.start();
  const auto start = std::chrono::steady_clock::time_point{};
  coordinator.handle(added(f.candidate()), start);
  EXPECT_EQ(coordinator.status().kind, Coordinator::StatusKind::RetryScheduled);
  EXPECT_EQ(coordinator.peerCount(), 0u);

  coordinator.retry(start + std::chrono::milliseconds{9});
  EXPECT_EQ(attempts, 1u);
  coordinator.retry(start + std::chrono::milliseconds{10});
  EXPECT_EQ(attempts, 2u);
  EXPECT_EQ(coordinator.status().kind, Coordinator::StatusKind::Ready);
  EXPECT_EQ(coordinator.peerCount(), 1u);
}

TEST(DmnDmesgNetDiscovery, IgnoresStaleSessionLossAndNotifiesStatus) {
  Fixture f;
  std::vector<Coordinator::StatusKind> statuses;
  Coordinator coordinator(
      f.policy,
      [&](const Coordinator::Endpoint &) {
        return Coordinator::FactoryResult{
            std::make_unique<FakeSession>(f.closed)};
      },
      [&](const Coordinator::Status &status) {
        statuses.push_back(status.kind);
      });

  coordinator.start();
  coordinator.handle(added(f.candidate()));
  const auto generation = coordinator.status().generation;
  ASSERT_NE(generation, 0u);
  coordinator.sessionLost(f.candidate().key, generation + 1, true,
                          std::chrono::steady_clock::time_point{});
  EXPECT_EQ(coordinator.peerCount(), 1u);
  coordinator.sessionLost(f.candidate().key, generation, false,
                          std::chrono::steady_clock::time_point{});
  EXPECT_EQ(coordinator.peerCount(), 0u);
  EXPECT_EQ(f.closed, 1);
  ASSERT_GE(statuses.size(), 3u);
  EXPECT_EQ(statuses.front(), Coordinator::StatusKind::Started);
  EXPECT_EQ(statuses.back(), Coordinator::StatusKind::SessionLost);
}

TEST(DmnDmesgNetDiscovery, EnvironmentTrustedAdapterFramesLoopbackDmesgNet) {
  const auto listener{::socket(AF_INET, SOCK_STREAM, 0)};
  ASSERT_GE(listener, 0);

  int reuse{1};
  ASSERT_EQ(
      ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)),
      0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)),
            0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t address_size{sizeof(address)};
  ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                          &address_size),
            0);

  std::atomic_bool received{false};
  std::thread server{[&] {
    pollfd wait{listener, POLLIN, 0};
    if (::poll(&wait, 1, 2000) <= 0) {
      return;
    }

    const auto connection{::accept(listener, nullptr, nullptr)};
    if (connection < 0) {
      return;
    }

    std::array<std::uint8_t, sizeof(std::uint32_t)> header{};
    const auto header_size{
        ::recv(connection, header.data(), header.size(), MSG_WAITALL)};
    if (header_size != static_cast<ssize_t>(header.size())) {
      ::close(connection);
      return;
    }

    std::uint32_t network_size{};
    std::memcpy(&network_size, header.data(), sizeof(network_size));
    const auto size{ntohl(network_size)};
    std::vector<char> frame(size);
    const auto frame_size{
        size == 0 ? 0 : ::recv(connection, frame.data(), size, MSG_WAITALL)};
    received = frame_size == static_cast<ssize_t>(size);
    std::array<char, 256> drain{};
    while (::recv(connection, drain.data(), drain.size(), 0) > 0) {
    }
    ::close(connection);
  }};

  Fixture f;
  f.policy.transport = "tcp";
  auto candidate{f.candidate()};
  candidate.srv->port = ntohs(address.sin_port);
  candidate.txt[2] = Fixture::bytes("transport=tcp");

  Coordinator coordinator(f.policy,
                          Coordinator::EnvironmentTrusted::factory("node-a"));
  coordinator.start();
  coordinator.handle(added(std::move(candidate)));
  EXPECT_EQ(coordinator.status().kind,
            Coordinator::StatusKind::ConnectedUnverified);
  EXPECT_EQ(coordinator.peerCount(), 1u);
  auto handler{coordinator.openHandler(
      dmn::Dmn_DiscoveryServiceKey{"peer", "_dmn._tcp", "local", 2}, "loopback",
      "hello")};
  ASSERT_TRUE(handler.has_value());
  dmn::DMesgPb message;
  message.set_topic("hello");
  message.set_type(dmn::DMesgTypePb::message);
  message.set_sourceidentifier("node-a");
  message.mutable_body()->set_message("hello");
  (*handler)->write(message);

  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{2}};
  while (!received && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  coordinator.shutdown();

  server.join();
  ::close(listener);
  EXPECT_TRUE(received);
}

} // namespace
