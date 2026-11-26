/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file discovery-dmesgnet-consumer.cpp
 * @brief Installed-library compile and construction check for Phase 9.
 */

#include <dmn-dmesgnet-discovery.hpp>

#include <memory>

namespace {

class ConsumerSession final : public dmn::Dmn_DMesgNetDiscovery::Session {
public:
  void shutdown() noexcept override {}
};

} // namespace

int main() {
  dmn::Dmn_DMesgNetDiscovery::Policy policy;
  policy.service_type = "_dmntest._tcp";
  policy.node_identity = "package-consumer";

  dmn::Dmn_DMesgNetDiscovery coordinator(
      std::move(policy),
      [](const dmn::Dmn_DMesgNetDiscovery::Endpoint &)
          -> dmn::Dmn_DMesgNetDiscovery::FactoryResult {
        return std::make_unique<ConsumerSession>();
      });
  coordinator.start();
  coordinator.shutdown();
  return 0;
}
