/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file private-bus-test.cpp
 * @brief Test an absent Avahi daemon on an isolated D-Bus.
 */

#include "dmn-discovery-avahi.hpp"

#include <chrono>
#include <future>
#include <iostream>

int main() {
  using namespace dmn::detail;
  DiscoveryCommandWorker worker{4, 1};
  std::promise<void> connecting{};
  bool reported{false};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      makeDiscoveryAvahiApi(), [&](DiscoveryAvahiEvent event) {
        if (event.m_state == AVAHI_CLIENT_CONNECTING && !reported) {
          reported = true;
          connecting.set_value();
        }
      })};
  auto connected{connecting.get_future()};
  const auto started{worker.attachPoller(poller)};
  if (!started) {
    std::cerr << started.error().m_message << '\n';
    return 1;
  }
  const bool absent{connected.wait_for(std::chrono::seconds{3}) ==
                    std::future_status::ready};
  const bool woke{
      worker.execute([]() -> DiscoveryResult { return {}; }).has_value()};
  const bool stopped{worker.shutdown().has_value()};
  if (!absent || !woke || !stopped) {
    std::cerr << "Avahi absent-daemon or shutdown check failed\n";
    return 1;
  }
  return 0;
}
