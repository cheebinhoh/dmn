/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file real-unified-test.cpp
 * @brief Exercise unified sessions against an isolated Avahi daemon.
 */

#include "dmn-discovery-avahi-registration-session.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iostream>
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
void waitUntil(Predicate predicate, const char *message) {
  const auto deadline{std::chrono::steady_clock::now() + 15s};
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  check(predicate(), message);
}

void run() {
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, makeDiscoveryAvahiApi(), makeDiscoveryAvahiRegistrationApi(),
      makeDiscoveryAvahiBrowserApi(), makeDiscoveryAvahiRecordApi(),
      makeDiscoveryAvahiResolverApi())};
  check(client.has_value(), "unified client startup failed");
  auto browser{
      (*client)->browse({"_dmntest._tcp", "local.", {}}, {}, {true, false})};
  check(browser.has_value(), "browse admission failed");
  const std::string name{"dmn-unified-" + std::to_string(getpid())};
  auto registration{
      (*client)->advertise({{name, "_dmntest._tcp", "local.", {}},
                            12345,
                            {{'v', '=', 0, 255}, {'k', '=', 'v'}}},
                           {})};
  check(registration.has_value(), "registration admission failed");
  waitUntil(
      [&] {
        return registration->status().m_state ==
               DiscoveryRegistrationState::kEstablished;
      },
      "registration did not establish");
  waitUntil(
      [&] {
        const auto snapshot{browser->snapshot()};
        return std::any_of(
            snapshot.m_browser.m_candidates.begin(),
            snapshot.m_browser.m_candidates.end(), [&](const auto &candidate) {
              return candidate.m_key.m_instance == name &&
                     candidate.m_records.m_srv &&
                     candidate.m_records.m_srv->m_port == 12345 &&
                     candidate.m_records.m_txt_wire ==
                         std::vector<std::uint8_t>{4, 'v', '=', 0,  255,
                                                   3, 'k', '=', 'v'} &&
                     std::any_of(candidate.m_records.m_addresses.begin(),
                                 candidate.m_records.m_addresses.end(),
                                 [](const auto &address) {
                                   return address.m_bytes ==
                                          std::vector<std::uint8_t>{192, 0, 2,
                                                                    1};
                                 });
            });
      },
      "unified browse and resolution timed out");
  check(registration->stop().has_value(), "withdrawal failed");
  waitUntil(
      [&] {
        const auto snapshot{browser->snapshot()};
        return std::none_of(snapshot.m_browser.m_candidates.begin(),
                            snapshot.m_browser.m_candidates.end(),
                            [&](const auto &candidate) {
                              return candidate.m_key.m_instance == name;
                            });
      },
      "withdrawal was not observed");
  check(browser->stop().has_value(), "browser stop failed");
  check((*client)->shutdown().has_value(), "client shutdown failed");
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
