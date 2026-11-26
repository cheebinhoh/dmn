/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file discovery-dmesgnet-qualification.cpp
 * @brief Two-process mDNS-to-DMesgNet hello qualification client.
 *
 * This opt-in program is deliberately small and operational rather than a
 * unit test. The listen process advertises a TCP endpoint and accepts one
 * framed DMesgNet connection. The connect process browses for the advertised
 * node, forwards the owned discovery events to Dmn_DMesgNetDiscovery, and
 * sends one "hello" DMesg message through the admitted session.
 */

#include <dmn-discovery.hpp>
#include <dmn-dmesgnet-discovery.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "dmn-io.hpp"
#include "proto/dmn-dmesg.pb.h"

namespace {

using namespace dmn;
using namespace std::chrono_literals;

constexpr std::string_view kServiceType{"_dmn-hello._tcp"};
constexpr std::string_view kDomain{"local."};
constexpr std::string_view kVersion{"1"};
constexpr std::string_view kTransport{"tcp"};
constexpr auto kTimeout{90s};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

std::uint32_t parseNumber(std::string_view text, std::string_view label) {
  std::uint32_t value{};
  const auto [end, error]{
      std::from_chars(text.data(), text.data() + text.size(), value)};
  check(error == std::errc{} && end == text.data() + text.size() && value != 0,
        label);
  return value;
}

std::vector<std::uint8_t> bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

std::vector<Dmn_DiscoveryTxtEntry> metadata(std::string_view node,
                                            std::string_view token) {
  return {bytes(std::string{"node="} + std::string{node}), bytes("version=1"),
          bytes("transport=tcp"),
          bytes(std::string{"auth="} + std::string{token})};
}

template <typename Predicate>
void waitUntil(Predicate predicate, std::string_view message) {
  const auto deadline{std::chrono::steady_clock::now() + kTimeout};
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(50ms);
  }
  throw std::runtime_error{std::string{message}};
}

class FramedIo final : public Dmn_Io<std::string> {
public:
  explicit FramedIo(int fd) : m_fd{fd} {}
  ~FramedIo() noexcept override { shutdown(); }

  FramedIo(const FramedIo &) = delete;
  FramedIo &operator=(const FramedIo &) = delete;

  std::optional<std::string> read() override {
    std::uint32_t network_size{};
    if (!readExact(&network_size, sizeof(network_size))) {
      return std::nullopt;
    }
    const auto size{ntohl(network_size)};
    check(size <= 1024 * 1024, "received DMesgNet frame is too large");
    std::string value(size, '\0');
    check(size == 0 || readExact(value.data(), value.size()),
          "received incomplete DMesgNet frame");
    return value;
  }

  using Dmn_Io<std::string>::write;

  void write(const std::string &value) override {
    check(value.size() <= std::numeric_limits<std::uint32_t>::max(),
          "DMesgNet frame is too large");
    const auto network_size{htonl(static_cast<std::uint32_t>(value.size()))};
    writeExact(&network_size, sizeof(network_size));
    if (!value.empty()) {
      writeExact(value.data(), value.size());
    }
  }

  void write(std::string &&value) override { write(std::as_const(value)); }

  void shutdown() noexcept override {
    const auto fd{std::exchange(m_fd, -1)};
    if (fd >= 0) {
      ::shutdown(fd, SHUT_RDWR);
      ::close(fd);
    }
  }

private:
  bool readExact(void *data, std::size_t size) {
    auto *bytes_ptr{static_cast<char *>(data)};
    while (size != 0) {
      const auto count{::recv(m_fd, bytes_ptr, size, 0)};
      if (count <= 0) {
        return false;
      }
      bytes_ptr += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  void writeExact(const void *data, std::size_t size) {
    const auto *bytes_ptr{static_cast<const char *>(data)};
    while (size != 0) {
      const auto count{::send(m_fd, bytes_ptr, size, 0)};
      check(count > 0, "DMesgNet socket write failed");
      bytes_ptr += count;
      size -= static_cast<std::size_t>(count);
    }
  }

  int m_fd;
};

int createListener(std::uint16_t port) {
  const auto fd{::socket(AF_INET, SOCK_STREAM, 0)};
  check(fd >= 0, "cannot create TCP listener");
  int reuse{1};
  check(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0,
        "cannot configure TCP listener");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  check(::bind(fd, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) == 0,
        "cannot bind TCP listener");
  check(::listen(fd, 1) == 0, "cannot listen for TCP connection");
  return fd;
}

std::optional<Dmn_DiscoveryServiceKey>
findCandidate(const Dmn_DiscoveryBrowser &browser, std::string_view instance) {
  auto snapshot{browser.snapshot()};
  check(snapshot.has_value(), "discovery snapshot failed");
  for (const auto &candidate : snapshot->candidates) {
    if (candidate.key.instance == instance &&
        candidate.resolution == Dmn_DiscoveryResolutionState::Resolved &&
        candidate.record_state == Dmn_DiscoveryRecordState::Monitoring &&
        candidate.srv && candidate.srv->port != 0 &&
        !candidate.addresses.empty()) {
      return candidate.key;
    }
  }
  return std::nullopt;
}

void advertise(Dmn_DiscoveryClient &client, std::string_view node,
               std::uint16_t port, std::uint32_t interface,
               std::string_view token,
               std::unique_ptr<Dmn_DiscoveryRegistration> &registration) {
  auto result{client.advertise({{std::string{node}, std::string{kServiceType},
                                 std::string{kDomain}, interface},
                                port,
                                metadata(node, token)},
                               [](Dmn_DiscoveryRegistrationStatus) {})};
  if (!result) {
    throw std::runtime_error{"discovery advertisement was not accepted: " +
                             result.error().detail};
  }
  registration = std::move(*result);
  waitUntil(
      [&] {
        auto status{registration->status()};
        check(status.has_value(), "discovery registration status failed");
        return status->state == Dmn_DiscoveryRegistrationState::Established;
      },
      "discovery registration did not become established");
}

dmn::DMesgPb helloMessage(std::string_view node, std::string_view text) {
  dmn::DMesgPb message;
  message.set_topic("hello");
  message.set_type(dmn::DMesgTypePb::message);
  message.set_sourceidentifier(std::string{node});
  message.mutable_body()->set_message(std::string{text});
  return message;
}

void listenNode(std::string node, std::uint16_t port, std::uint32_t interface,
                std::string token) {
  const auto listener{createListener(port)};
  auto client{Dmn_DiscoveryClient::create()};
  check(client.has_value(), "discovery client creation failed");
  std::unique_ptr<Dmn_DiscoveryRegistration> registration;
  advertise(**client, node, port, interface, token, registration);
  std::cout << "LISTENING node=" << node << " service=" << kServiceType
            << " port=" << port << std::endl;

  const auto connection{::accept(listener, nullptr, nullptr)};
  check(connection >= 0, "TCP accept failed");
  ::close(listener);

  auto io{std::make_shared<FramedIo>(connection)};
  Dmn_DMesgNet network{node, io, io};
  waitUntil([&] { return network.getStatus().m_ready; },
            "listener DMesgNet did not become ready");
  auto handler{network.openHandler("hello-listener", "hello")};
  auto message{handler->read()};
  check(message.has_value() && message->body().message() == "hello",
        "did not receive the hello message");
  std::cout << "HELLO_RECEIVED from=" << message->sourceidentifier()
            << std::endl;
  std::this_thread::sleep_for(1s);
  network.shutdown();
  check(registration->stop().has_value(), "discovery withdrawal failed");
  check((*client)->shutdown().has_value(), "discovery shutdown failed");
}

void connectNode(std::string node, std::string peer, std::uint32_t interface,
                 std::string token) {
  static_cast<void>(token);
  auto client{Dmn_DiscoveryClient::create()};
  check(client.has_value(), "discovery client creation failed");

  Dmn_DMesgNetDiscovery::Policy policy;
  policy.service_type = std::string{kServiceType};
  policy.domain = std::string{kDomain};
  policy.interface_index = interface;
  policy.node_identity = peer;
  policy.protocol_version = std::string{kVersion};
  policy.transport = std::string{kTransport};
  auto coordinator{std::make_unique<Dmn_DMesgNetDiscovery>(
      std::move(policy),
      Dmn_DMesgNetDiscovery::EnvironmentTrusted::factory(node),
      [](const Dmn_DMesgNetDiscovery::Status &status) {
        std::cout << "DMESGNET_STATUS kind=" << static_cast<int>(status.kind)
                  << " peers=" << status.peer_count;
        if (!status.detail.empty()) {
          std::cout << " detail=" << status.detail;
        }
        std::cout << std::endl;
      })};
  coordinator->start();

  Dmn_DiscoveryQuery query;
  query.service_type = std::string{kServiceType};
  query.domain = std::string{kDomain};
  query.interface_index = interface;
  auto browse{
      (*client)->browse(std::move(query), [&](Dmn_DiscoveryEvent event) {
        if (event.candidate) {
          std::cout
              << "DISCOVERY_EVENT instance=" << event.candidate->key.instance
              << " type=" << event.candidate->key.service_type
              << " domain=" << event.candidate->key.domain << " interface="
              << event.candidate->key.interface_index.value_or(0)
              << " resolved=" << static_cast<int>(event.candidate->resolution)
              << " record=" << static_cast<int>(event.candidate->record_state)
              << " txt=" << event.candidate->txt.size() << std::endl;
        }
        coordinator->handle(std::move(event));
      })};
  check(browse.has_value(), "discovery browse was not accepted");

  auto browser{std::move(*browse)};
  waitUntil(
      [&] {
        coordinator->retry(std::chrono::steady_clock::now());
        return coordinator->peerCount() == 1;
      },
      "discovery did not admit the peer");
  auto key{findCandidate(*browser, peer)};
  check(key.has_value(), "admitted peer was not present in the browser view");
  std::this_thread::sleep_for(5s);
  auto handler{coordinator->openHandler(*key, "hello-connector", "hello")};
  check(handler.has_value(), handler.error());
  (*handler)->write(helloMessage(node, "hello"));
  std::cout << "HELLO_SENT to=" << peer << std::endl;
  std::this_thread::sleep_for(2s);

  coordinator->shutdown();
  check(browser->stop().has_value(), "discovery browser stop failed");
  check((*client)->shutdown().has_value(), "discovery shutdown failed");
}

void usage() {
  std::cout << "usage: discovery-dmesgnet-qualification listen NODE PORT "
               "INTERFACE [TOKEN]\n"
               "       discovery-dmesgnet-qualification connect NODE PEER "
               "INTERFACE [TOKEN]\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    usage();
    return 0;
  }

  try {
    check(argc == 5 || argc == 6, "invalid arguments; use --help");
    const std::string_view role{argv[1]};
    const std::string node{argv[2]};
    const auto interface{parseNumber(argv[role == "listen" ? 4 : 4],
                                     "interface must be nonzero")};
    const std::string token{argc == 6 ? argv[5] : "qualification-token"};
    check(!node.empty() && (role == "listen" || role == "connect"),
          "invalid role or node");
    if (role == "listen") {
      const auto port{parseNumber(argv[3], "port must be nonzero")};
      check(port <= std::numeric_limits<std::uint16_t>::max(),
            "port is out of range");
      listenNode(node, static_cast<std::uint16_t>(port), interface, token);
    } else {
      connectNode(node, argv[3], interface, token);
    }
  } catch (const std::exception &error) {
    std::cerr << "qualification failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
