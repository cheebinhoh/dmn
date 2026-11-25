/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file contribution-test.cpp
 * @brief Controlled contribution reconciliation without a daemon.
 */
#include "dmn-discovery-avahi-contribution.hpp"
#include <avahi-common/defs.h>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#include <fiu.h>
#endif

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

struct AvahiClient {};
struct AvahiRecordBrowser {
  unsigned index;
};
struct AvahiServiceResolver {
  unsigned index;
};

namespace {
using namespace dmn::detail;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error{message};
}

class Records final : public DiscoveryAvahiRecordApi {
public:
  struct Item {
    AvahiRecordBrowser native;
    AvahiRecordBrowserCallback callback;
    void *context;
    std::string name;
    std::uint16_t type;
    bool live{true};
  };
  AvahiRecordBrowser *create(AvahiClient *, AvahiIfIndex, AvahiProtocol,
                             const char *name, std::uint16_t,
                             std::uint16_t type, AvahiLookupFlags,
                             AvahiRecordBrowserCallback callback,
                             void *context) override {
    if (throwAllocation)
      throw std::bad_alloc{};
    items.push_back(std::make_unique<Item>(Item{
        {static_cast<unsigned>(items.size())}, callback, context, name, type}));
    auto &item{*items.back()};
    if (type == AVAHI_DNS_TYPE_SRV)
      emit(item, AVAHI_BROWSER_NEW, srv);
    return &item.native;
  }
  void free(AvahiRecordBrowser *browser) noexcept override {
    checkNotCallback();
    items[browser->index]->live = false;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "test failure"; }
  void emit(Item &item, AvahiBrowserEvent event,
            const std::vector<std::uint8_t> &data) {
    inCallback = true;
    item.callback(&item.native, 7, AVAHI_PROTO_INET, event, item.name.c_str(),
                  AVAHI_DNS_CLASS_IN, item.type, data.data(), data.size(),
                  AVAHI_LOOKUP_RESULT_MULTICAST, item.context);
    inCallback = false;
  }
  void checkNotCallback() noexcept {
    if (inCallback)
      std::terminate();
  }
  bool inCallback{false};
  bool throwAllocation{false};
  std::vector<std::unique_ptr<Item>> items;
  std::vector<std::uint8_t> srv{0,   0,   0, 0,   0x1f, 0x90, 4,   'H', 'o',
                                's', 't', 5, 'l', 'o',  'c',  'a', 'l', 0};
};

class Resolvers final : public DiscoveryAvahiResolverApi {
public:
  struct Item {
    AvahiServiceResolver native;
    AvahiServiceResolverCallback callback;
    void *context;
    AvahiProtocol family;
    bool live{true};
  };
  AvahiServiceResolver *create(AvahiClient *, AvahiIfIndex, AvahiProtocol,
                               const char *, const char *, const char *,
                               AvahiProtocol address, AvahiLookupFlags flags,
                               AvahiServiceResolverCallback callback,
                               void *context) override {
    check((flags & AVAHI_LOOKUP_NO_TXT) != 0, "resolver queried TXT");
    items.push_back(std::make_unique<Item>(Item{
        {static_cast<unsigned>(items.size())}, callback, context, address}));
    emit(*items.back(), AVAHI_RESOLVER_FOUND, "host.local.", 8080, 1);
    return &items.back()->native;
  }
  void free(AvahiServiceResolver *resolver) noexcept override {
    if (inCallback)
      std::terminate();
    items[resolver->index]->live = false;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "test failure"; }
  void emit(Item &item, AvahiResolverEvent event, const char *host,
            std::uint16_t port, unsigned char last) {
    AvahiAddress address{};
    address.proto = item.family;
    if (item.family == AVAHI_PROTO_INET) {
      std::array<unsigned char, 4> bytes{10, 0, 0, last};
      std::memcpy(&address.data.ipv4.address, bytes.data(), bytes.size());
    } else {
      address.data.ipv6.address[0] = 0x20;
      address.data.ipv6.address[1] = 1;
      address.data.ipv6.address[15] = last;
    }
    inCallback = true;
    item.callback(&item.native, 7, AVAHI_PROTO_INET, event, "Printer",
                  "_ipp._tcp", "local.", host, &address, port, nullptr,
                  AVAHI_LOOKUP_RESULT_MULTICAST, item.context);
    inCallback = false;
  }
  bool inCallback{false};
  std::vector<std::unique_ptr<Item>> items;
};

void run() {
  DiscoveryOptions options;
  auto now{std::chrono::steady_clock::time_point{}};
  DiscoverySessionClient client{options, true, [&] { return now; }};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "browse failed");
  auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  check(source.has_value(), "observe failed");
  AvahiClient native;
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  std::unique_ptr<DiscoveryAvahiContribution> owner;
  auto execute = [&](auto fn) {
    auto result{client.dispatchNative(fn)};
    check(result.has_value(), result ? "" : result.error().m_message.c_str());
  };
  execute([&]() -> DiscoveryResult {
    owner = std::make_unique<DiscoveryAvahiContribution>(
        *session, epoch, key, DiscoveryContribution::kIpv4, *source, options,
        &native, records, resolvers);
    return owner->start();
  });
  if (session->snapshot().m_native_operations != 5) {
    throw std::runtime_error{
        "wrong operation count: " +
        std::to_string(session->snapshot().m_native_operations) +
        ", record state: " +
        std::to_string(static_cast<int>(session->snapshot()
                                            .m_browser.m_candidates.at(0)
                                            .m_records.m_state))};
  }
  auto candidate = [&] {
    return session->snapshot().m_browser.m_candidates.at(0);
  };
  check(candidate().m_records.m_addresses.size() == 2,
        "immediate results lost");

  execute([&]() -> DiscoveryResult {
    resolvers->emit(*resolvers->items[0], AVAHI_RESOLVER_FOUND, "host.local.",
                    8080, 9);
    return owner->drive();
  });
  check(candidate().m_records.m_addresses.size() == 2,
        "address was not replaced");

  execute([&]() -> DiscoveryResult {
    records->emit(*records->items[0], AVAHI_BROWSER_REMOVE, records->srv);
    // An old callback after SRV removal must not recreate an endpoint.
    resolvers->emit(*resolvers->items[0], AVAHI_RESOLVER_FOUND, "host.local.",
                    8080, 4);
    resolvers->emit(*resolvers->items[1], AVAHI_RESOLVER_FAILURE, "", 0, 0);
    return owner->drive();
  });
  check(candidate().m_records.m_addresses.empty() &&
            !resolvers->items[0]->live && !resolvers->items[1]->live &&
            candidate().m_failures.empty(),
        "removed SRV retained resolvers");
  execute([&]() -> DiscoveryResult {
    records->emit(*records->items[0], AVAHI_BROWSER_NEW, records->srv);
    return owner->drive();
  });
  check(resolvers->items.size() == 4 &&
            candidate().m_records.m_addresses.size() == 2,
        "resolver restart failed");

  execute([&]() -> DiscoveryResult {
    resolvers->emit(*resolvers->items[2], AVAHI_RESOLVER_FAILURE, "", 0, 0);
    check(resolvers->items[2]->live, "freed inside callback");
    check(!owner->drive(), "failure was hidden");
    return {};
  });
  check(!resolvers->items[2]->live && candidate().m_failures.size() == 1,
        "failure not recorded");
  check(candidate().m_failures.front().m_error.m_backend_domain == "avahi" &&
            candidate().m_failures.front().m_error.m_backend_code == -1,
        "resolver failure lost its native identity");
  now += std::chrono::seconds{1};
  execute([&]() -> DiscoveryResult {
    auto due{session->takeDueRetries(epoch)};
    check(due && due->size() == 1, "retry not scheduled");
    return owner->retry(due->front());
  });
  check(candidate().m_failures.empty() && resolvers->items.size() == 5,
        "retry did not recover");
  execute([&]() -> DiscoveryResult {
    records->emit(*records->items[1], AVAHI_BROWSER_FAILURE, {});
    check(records->items[1]->live, "freed record inside callback");
    check(!owner->drive(), "record failure was hidden");
    return {};
  });
  check(!records->items[1]->live && candidate().m_failures.size() == 1,
        "record failure not retained");
  now += std::chrono::seconds{1};
  execute([&]() -> DiscoveryResult {
    auto due{session->takeDueRetries(epoch)};
    check(due && due->size() == 1, "record retry missing");
    return owner->retry(due->front());
  });
  check(candidate().m_failures.empty(), "record recovery not cleared");
  execute([&]() -> DiscoveryResult {
    auto &resolver{*resolvers->items[resolvers->items.size() - 2]};
    resolvers->emit(resolver, AVAHI_RESOLVER_FOUND, "other.local.", 8080, 3);
    check(!owner->drive(), "mismatched SRV result was hidden");
    check(!candidate().m_failures.empty(), "mismatch did not fail attempt");
    auto refreshed{session->refresh(key)};
    check(refreshed.has_value(), "refresh failed");
    return owner->drive();
  });
  check(candidate().m_failures.empty() &&
            candidate().m_records.m_addresses.size() == 2,
        "refresh did not supersede failed lookup");
  execute([&]() -> DiscoveryResult {
    auto result{owner->cleanup()};
    owner.reset();
    return result;
  });
  check(session->snapshot().m_native_operations == 1, "slots leaked");
  check(client.shutdown().has_value(), "shutdown failed");
}

void limits() {
  DiscoveryOptions options;
  options.m_max_native_operations = 2;
  DiscoverySessionClient client{options};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "limited browse failed");
  auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  check(source.has_value(), "limited observe failed");
  AvahiClient native;
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto result{client.dispatchNative([&]() -> DiscoveryResult {
    DiscoveryAvahiContribution owner{
        *session, epoch,   key,     DiscoveryContribution::kIpv4,
        *source,  options, &native, records,
        resolvers};
    auto started{owner.start()};
    check(!started &&
              started.error().m_kind == DiscoveryErrorKind::kResourceLimit,
          "native quota exhaustion hidden");
    static_cast<void>(owner.cleanup());
    return {};
  })};
  check(result.has_value(), "limited dispatch failed");
  check(records->items.size() == 1 && !records->items[0]->live &&
            resolvers->items.empty(),
        "quota created excess operations");
  static_cast<void>(client.shutdown());
}

void allocationCleanup() {
  DiscoveryOptions options;
  DiscoverySessionClient client{options};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "allocation browse failed");
  auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  check(source.has_value(), "allocation observe failed");
  AvahiClient native;
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  records->throwAllocation = true;
  std::unique_ptr<DiscoveryAvahiContribution> owner;
  auto result{client.dispatchNative([&]() -> DiscoveryResult {
    owner = std::make_unique<DiscoveryAvahiContribution>(
        *session, epoch, key, DiscoveryContribution::kIpv4, *source, options,
        &native, records, resolvers);
    return owner->start();
  })};
  check(!result && result.error().m_kind == DiscoveryErrorKind::kResourceLimit,
        "allocation exception was hidden");
  result = client.dispatchNative([&]() -> DiscoveryResult {
    auto cleaned{owner->cleanup()};
    owner.reset();
    return cleaned;
  });
  check(result.has_value(), "allocation cleanup failed");
  check(session->snapshot().m_native_operations == 1,
        "allocation exception leaked native slot");
  check(client.shutdown().has_value(), "allocation shutdown failed");
}

void requestedAddressFamily() {
  DiscoveryOptions options;
  DiscoverySessionClient client{options};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "browse failed");
  auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  check(source.has_value(), "observe failed");
  AvahiClient native;
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto result{client.dispatchNative([&]() -> DiscoveryResult {
    DiscoveryAvahiContribution owner{
        *session,  epoch,        key,     DiscoveryContribution::kIpv4,
        *source,   options,      &native, records,
        resolvers, {true, false}};
    auto started{owner.start()};
    if (!started) {
      return started;
    }
    check(resolvers->items.size() == 1 &&
              resolvers->items.front()->family == AVAHI_PROTO_INET,
          "unrequested address family was resolved");
    return owner.cleanup();
  })};
  check(result.has_value(), "single-family contribution failed");
  check(client.shutdown().has_value(), "shutdown failed");
}

#ifdef FIU_ENABLE
void cleanupAllocationFailure() {
  check(fiu_init(0) == 0, "fault injection initialization failed");
  DiscoveryOptions options;
  DiscoverySessionClient client{options};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "browse failed");
  auto epoch{session->snapshot().m_delivery.m_epoch};
  DiscoveryServiceKey key{"Printer", "_ipp._tcp", "local.", 7};
  auto source{session->observe(epoch, key, DiscoveryContribution::kIpv4)};
  check(source.has_value(), "observe failed");
  AvahiClient native;
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto result{client.dispatchNative([&]() -> DiscoveryResult {
    DiscoveryAvahiContribution owner{
        *session, epoch,   key,     DiscoveryContribution::kIpv4,
        *source,  options, &native, records,
        resolvers};
    auto started{owner.start()};
    if (!started) {
      return started;
    }
    check(fiu_enable("dmn/discovery/avahi/contribution-cleanup-allocation", 1,
                     nullptr, FIU_ONETIME) == 0,
          "fault injection setup failed");
    auto cleaned{owner.cleanup()};
    check(!cleaned &&
              cleaned.error().m_kind == DiscoveryErrorKind::kResourceLimit,
          "cleanup allocation failure was not reported");
    const auto repeated{owner.cleanup()};
    check(!repeated &&
              repeated.error().m_kind == DiscoveryErrorKind::kResourceLimit,
          "cleanup allocation failure was not retained");
    for (const auto &item : records->items)
      check(!item->live, "record watch survived cleanup failure");
    for (const auto &item : resolvers->items)
      check(!item->live, "resolver survived cleanup failure");
    return session->backendFailed(cleaned.error());
  })};
  check(!result && result.error().m_kind == DiscoveryErrorKind::kResourceLimit,
        "cleanup failure was not retained");
  check(session->snapshot().m_native_operations == 0,
        "cleanup failure leaked native slots");
  check(client.shutdown().has_value(), "shutdown failed");
}
#endif
} // namespace

int main() {
  run();
  limits();
  allocationCleanup();
  requestedAddressFamily();
#ifdef FIU_ENABLE
  cleanupAllocationFailure();
#endif
}
