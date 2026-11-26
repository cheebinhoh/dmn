/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file discovery-qualification.cpp
 * @brief Opt-in cross-host test of the installed discovery API.
 */

#include <dmn-discovery.hpp>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace dmn;
using namespace std::chrono_literals;

constexpr std::string_view kType{"_dmntest._tcp"};
constexpr std::uint16_t kPort{12345};
const std::vector<Dmn_DiscoveryTxtEntry> kTxt{{'v', '=', 0, 255},
                                              {'k', '=', 'v'}};
constexpr auto kTimeout{60s};
constexpr auto kHoldTimeout{5min};

void check(bool value, std::string_view message) {
  if (!value) {
    throw std::runtime_error{std::string{message}};
  }
}

std::uint32_t parseInterface(std::string_view text) {
  std::uint32_t value{};
  const auto [end, error]{
      std::from_chars(text.data(), text.data() + text.size(), value)};
  check(error == std::errc{} && end == text.data() + text.size() && value != 0,
        "interface must be a nonzero numeric index");

  return value;
}

bool sameDnsName(std::string_view left, std::string_view right) {
  if (left.ends_with('.')) {
    left.remove_suffix(1);
  }

  if (right.ends_with('.')) {
    right.remove_suffix(1);
  }

  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](unsigned char a, unsigned char b) {
                      const auto fold = [](unsigned char c) {
                        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
                      };

                      return fold(a) == fold(b);
                    });
}

bool usableAddress(const Dmn_DiscoveryAddress &address) {
  if (address.family == Dmn_DiscoveryAddressFamily::IPv4) {
    return address.bytes.size() == 4 && address.bytes[0] != 0 &&
           address.bytes[0] != 127 && address.bytes[0] < 224;
  }

  if (address.bytes.size() != 16 || address.bytes[0] == 0xff ||
      std::all_of(address.bytes.begin(), address.bytes.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    return false;
  }

  const bool linkLocal{address.bytes[0] == 0xfe &&
                       (address.bytes[1] & 0xc0) == 0x80};
  const bool loopback{
      std::all_of(address.bytes.begin(), address.bytes.end() - 1,
                  [](std::uint8_t byte) { return byte == 0; }) &&
      address.bytes.back() == 1};

  return !loopback && (!linkLocal || address.interface_index.has_value());
}

template <typename Predicate>
void waitUntil(Predicate predicate, std::string_view message) {
  const auto deadline{std::chrono::steady_clock::now() + kTimeout};
  do {
    if (predicate()) {
      return;
    }

    std::this_thread::sleep_for(25ms);
  } while (std::chrono::steady_clock::now() < deadline);

  throw std::runtime_error{std::string{message}};
}

void awaitWithdrawalRequest() {
  const auto deadline{std::chrono::steady_clock::now() + kHoldTimeout};
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const auto remaining{std::chrono::ceil<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now())};
    const auto result{
        poll(&input, 1, static_cast<int>(std::max(remaining, 0ms).count()))};
    if (result < 0 && errno == EINTR) {
      continue;
    }

    check(result >= 0, "waiting for withdrawal input failed");
    if (result == 0) {
      break;
    }

    check((input.revents & POLLIN) != 0, "withdrawal input closed");
    char byte{};
    const auto count{read(STDIN_FILENO, &byte, 1)};
    if (count < 0 && errno == EINTR) {
      continue;
    }

    check(count == 1, "withdrawal input closed");
    if (byte != '\n') {
      continue;
    }

    return;
  }

  throw std::runtime_error{"withdrawal input timed out"};
}

void advertise(Dmn_DiscoveryClient &client, std::string instance,
               std::uint32_t interface) {
  auto result{client.advertise(
      {{std::move(instance), std::string{kType}, "local.", interface},
       kPort,
       kTxt},
      [](Dmn_DiscoveryRegistrationStatus) {})};
  check(result.has_value(), "advertisement admission failed");
  auto registration{std::move(*result)};
  waitUntil(
      [&] {
        auto status{registration->status()};
        check(status.has_value(), "registration status failed");
        check(status->state == Dmn_DiscoveryRegistrationState::Registering ||
                  status->state ==
                      Dmn_DiscoveryRegistrationState::Unavailable ||
                  status->state == Dmn_DiscoveryRegistrationState::Established,
              status->error ? status->error->detail : "registration failed");

        return status->state == Dmn_DiscoveryRegistrationState::Established;
      },
      "registration establishment timed out");
  std::cout << "ESTABLISHED: press Enter after browser reports READY"
            << std::endl;
  awaitWithdrawalRequest();
  check(registration->stop().has_value(), "registration withdrawal failed");
  std::cout << "WITHDRAWN" << std::endl;
}

void browse(Dmn_DiscoveryClient &client, std::string_view instance,
            std::string_view hostname, std::uint32_t interface) {
  Dmn_DiscoveryQuery query;
  query.service_type = kType;
  query.interface_index = interface;
  auto result{client.browse(std::move(query), [](Dmn_DiscoveryEvent) {})};
  check(result.has_value(), "browse admission failed");
  auto browser{std::move(*result)};
  std::optional<Dmn_DiscoveryServiceKey> key;
  const auto findResolved = [&] {
    auto snapshot{browser->snapshot()};
    check(snapshot.has_value(), "browser snapshot failed");
    if (snapshot->state == Dmn_DiscoveryBrowserState::BackendUnavailable) {
      return false;
    }

    check(snapshot->state == Dmn_DiscoveryBrowserState::Running,
          snapshot->error ? snapshot->error->detail : "browser failed");
    for (const auto &candidate : snapshot->candidates) {
      if (candidate.key.instance == instance &&
          sameDnsName(candidate.key.service_type, kType) &&
          sameDnsName(candidate.key.domain, "local.") &&
          candidate.key.interface_index == interface && candidate.srv &&
          candidate.srv->port == kPort &&
          sameDnsName(candidate.srv->target, hostname) &&
          sameDnsName(candidate.hostname, hostname) && candidate.txt == kTxt &&
          std::any_of(candidate.addresses.begin(), candidate.addresses.end(),
                      usableAddress)) {
        key = candidate.key;

        return true;
      }
    }

    return false;
  };
  waitUntil(findResolved, "initial browse and resolution timed out");
  const auto refresh{browser->refresh(*key)};
  check(refresh.has_value(), "refresh admission failed");
  waitUntil(
      [&] {
        auto snapshot{browser->snapshot()};
        check(snapshot.has_value() &&
                  snapshot->state == Dmn_DiscoveryBrowserState::Running,
              "browser failed while refreshing");
        for (const auto &candidate : snapshot->candidates) {
          if (candidate.key.instance == instance &&
              candidate.key.interface_index == interface && candidate.refresh &&
              candidate.refresh->id == *refresh) {
            check(candidate.refresh->state != Dmn_DiscoveryRefreshState::Failed,
                  "refresh failed");

            return candidate.refresh->state ==
                   Dmn_DiscoveryRefreshState::Succeeded;
          }
        }

        return false;
      },
      "refresh timed out");
  check(browser->clearCache().has_value(), "local cache reset failed");
  key.reset();
  waitUntil(findResolved, "browse after reset timed out");
  std::cout << "READY: withdraw the test registration on the other host"
            << std::endl;
  waitUntil(
      [&] {
        auto snapshot{browser->snapshot()};
        check(snapshot.has_value(), "browser snapshot failed");
        check(snapshot->state == Dmn_DiscoveryBrowserState::Running,
              "browser failed or daemon unavailable while awaiting removal");

        return std::none_of(snapshot->candidates.begin(),
                            snapshot->candidates.end(), [&](const auto &item) {
                              return item.key.instance == instance &&
                                     item.key.interface_index == interface;
                            });
      },
      "withdrawal observation timed out");
  check(browser->stop().has_value(), "browser stop failed");
  std::cout << "REMOVED" << std::endl;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    std::cout << "usage: discovery-qualification advertise INSTANCE INTERFACE\n"
                 "       discovery-qualification browse INSTANCE HOSTNAME "
                 "INTERFACE\n";

    return 0;
  }

  try {
    const bool isAdvertise{argc == 4 &&
                           std::string_view{argv[1]} == "advertise"};
    const bool isBrowse{argc == 5 && std::string_view{argv[1]} == "browse"};
    check(isAdvertise || isBrowse, "invalid arguments; use --help");
    const auto interface{parseInterface(argv[argc - 1])};
    auto client{Dmn_DiscoveryClient::create()};
    check(client.has_value(), "client creation failed");
    if (isAdvertise) {
      advertise(**client, argv[2], interface);
    } else {
      browse(**client, argv[2], argv[3], interface);
    }

    check((*client)->shutdown().has_value(), "client shutdown failed");
  } catch (const std::exception &error) {
    std::cerr << "qualification failed: " << error.what() << '\n';

    return 1;
  }

  return 0;
}
