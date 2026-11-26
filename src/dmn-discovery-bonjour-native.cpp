/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-native.cpp
 * @brief Native DNSService reference operations.
 */

#include "dmn-discovery-bonjour-poll.hpp"

#include <dns_sd.h>

namespace dmn::detail {
namespace {

class NativeBonjour final : public DiscoveryBonjourApi {
public:
  int socketFd(DiscoveryBonjourRef *ref) override {
    return DNSServiceRefSockFD(reinterpret_cast<DNSServiceRef>(ref));
  }

  int processResult(DiscoveryBonjourRef *ref) override {
    return DNSServiceProcessResult(reinterpret_cast<DNSServiceRef>(ref));
  }

  void deallocate(DiscoveryBonjourRef *ref) noexcept override {
    DNSServiceRefDeallocate(reinterpret_cast<DNSServiceRef>(ref));
  }
};

} // namespace

std::shared_ptr<DiscoveryBonjourApi> makeDiscoveryBonjourApi() {
  return std::make_shared<NativeBonjour>();
}

} // namespace dmn::detail
