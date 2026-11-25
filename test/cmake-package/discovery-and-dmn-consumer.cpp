/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file discovery-and-dmn-consumer.cpp
 * @brief Link discovery and the existing Dmn library together.
 */

#include <dmn-discovery.hpp>
#include <dmn-proc.hpp>

int main() {
  bool ran{false};
  dmn::Dmn_Proc worker{"package test", [&] { ran = true; }};
  if (!worker.exec() || !worker.wait() || !ran) {
    return 1;
  }
  dmn::Dmn_DiscoveryOptions options;
  options.max_browsers = 0;
  auto client{dmn::Dmn_DiscoveryClient::create(options)};
  return !client && client.error().kind ==
                        dmn::Dmn_DiscoveryErrorKind::InvalidArgument
             ? 0
             : 1;
}
