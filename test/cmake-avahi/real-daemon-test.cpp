/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file real-daemon-test.cpp
 * @brief Opt-in native adapter test against an isolated Avahi daemon.
 */

#include "dmn-discovery-avahi-browser.hpp"
#include "dmn-discovery-avahi-record-watch.hpp"
#include "dmn-discovery-avahi-registration.hpp"
#include "dmn-discovery-avahi-resolver.hpp"
#include "dmn-discovery-avahi.hpp"

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace dmn::detail;
using namespace std::chrono_literals;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

template <typename Predicate>
void waitUntil(DiscoveryAvahiPoller &poller, Predicate predicate,
               const char *message) {
  const auto deadline{std::chrono::steady_clock::now() + 15s};
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    const auto result{poller.wait(false)};
    if (!result) {
      throw std::runtime_error{result.error().m_message};
    }
    std::this_thread::sleep_for(10ms);
  }
  check(predicate(), message);
}

void run() {
  AvahiClient *client{nullptr};
  DiscoveryAvahiPoller poller{
      makeDiscoveryAvahiApi(), [&](DiscoveryAvahiEvent event) {
        if (event.m_state == AVAHI_CLIENT_S_RUNNING) {
          client = event.m_client;
        } else if (event.m_state == AVAHI_CLIENT_FAILURE) {
          if (client) {
            throw std::runtime_error{"daemon disconnected during test"};
          }
          client = nullptr;
        }
      }};
  check(poller.start().has_value(), "poller startup failed");
  waitUntil(poller, [&] { return client != nullptr; }, "daemon did not start");

  const std::string name{"dmn-isolated-" + std::to_string(getpid())};
  std::optional<DiscoveryAvahiBrowseEvent> observed;
  bool removed{false};
  bool established{false};
  std::optional<DiscoveryError> failureError;
  const auto failure = [&](DiscoveryError error) {
    std::cerr << "native callback failure: " << error.m_message << '\n';
    failureError = std::move(error);
  };
  DiscoveryAvahiServiceBrowser browser{
      makeDiscoveryAvahiBrowserApi(),
      client,
      {"_dmntest._tcp", "local.", {}},
      [&](DiscoveryAvahiBrowseEvent event) -> DiscoveryResult {
        if (event.m_key.m_instance == name) {
          if (event.m_added) {
            observed = std::move(event);
          } else {
            removed = true;
          }
        }
        return {};
      },
      failure};
  check(browser.start().has_value(), "browser startup failed");
  DiscoveryAvahiRegistration registration{
      makeDiscoveryAvahiRegistrationApi(),
      client,
      {{name, "_dmntest._tcp", "local.", {}},
       12345,
       {{'v', '=', 0, 255}, {'k', '=', 'v'}}},
      {},
      [&](DiscoveryAvahiRegistrationEvent event) -> DiscoveryResult {
        established = event == DiscoveryAvahiRegistrationEvent::kEstablished;
        return {};
      },
      failure};
  check(registration.start().has_value(), "registration startup failed");
  waitUntil(
      poller, [&] { return established && observed.has_value(); },
      "establishment or browse timed out");

  bool srv{false};
  bool txt{false};
  bool address{false};
  DiscoveryAvahiRecordWatch srvWatch{
      makeDiscoveryAvahiRecordApi(),
      client,
      observed->m_key,
      observed->m_contribution,
      DiscoveryRecordKind::kSrv,
      {},
      [&](DiscoveryAvahiRecordEvent event) -> DiscoveryResult {
        if (event.m_added) {
          srv = std::get<DiscoverySrv>(event.m_record).m_port == 12345;
        }
        return {};
      },
      failure};
  DiscoveryAvahiRecordWatch txtWatch{
      makeDiscoveryAvahiRecordApi(),
      client,
      observed->m_key,
      observed->m_contribution,
      DiscoveryRecordKind::kTxt,
      {},
      [&](DiscoveryAvahiRecordEvent event) -> DiscoveryResult {
        if (event.m_added) {
          txt =
              std::get<std::vector<std::uint8_t>>(event.m_record) ==
              std::vector<std::uint8_t>{4, 'v', '=', 0, 255, 3, 'k', '=', 'v'};
        }
        return {};
      },
      failure};
  DiscoveryAvahiServiceResolver resolver{
      makeDiscoveryAvahiResolverApi(),
      client,
      observed->m_key,
      observed->m_contribution,
      DiscoveryAddressFamily::kIpv4,
      [&](DiscoveryAvahiResolverEvent event) -> DiscoveryResult {
        address =
            event.m_port == 12345 &&
            event.m_address.m_bytes == std::vector<std::uint8_t>{192, 0, 2, 1};
        return {};
      },
      failure};
  check(srvWatch.start().has_value(), "SRV watch startup failed");
  check(txtWatch.start().has_value(), "TXT watch startup failed");
  check(resolver.start().has_value(), "resolver startup failed");
  waitUntil(
      poller, [&] { return srv && txt && address; },
      "record watch or resolution timed out");
  check(registration.stop().has_value(), "withdrawal failed");
  waitUntil(poller, [&] { return removed; }, "browse removal timed out");
  check(!failureError, "native callback reported failure");
  check(resolver.stop().has_value() && srvWatch.stop().has_value() &&
            txtWatch.stop().has_value() && browser.stop().has_value(),
        "native operation cleanup failed");
  check(poller.stop().has_value(), "poller cleanup failed");
}
} // namespace

int main() {
  try {
    run();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
