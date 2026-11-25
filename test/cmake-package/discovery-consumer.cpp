/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file discovery-consumer.cpp
 * @brief Link only the installed, relocated discovery target.
 */

#include <dmn-discovery.hpp>

int main() {
  dmn::Dmn_DiscoveryOptions options;
  options.max_browsers = 0;
  auto client = dmn::Dmn_DiscoveryClient::create(options);
  return !client && client.error().kind ==
                        dmn::Dmn_DiscoveryErrorKind::InvalidArgument
             ? 0
             : 1;
}
