/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file record-watch-test.cpp
 * @brief Controlled tests for native record-watch ownership and mapping.
 */

#include "dmn-discovery-avahi-record-watch.hpp"

#include <avahi-common/defs.h>

#include <stdexcept>
#include <string>
#include <vector>

struct AvahiClient {};
struct AvahiRecordBrowser {};

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

class FakeRecordApi final : public DiscoveryAvahiRecordApi {
public:
  AvahiRecordBrowser *create(AvahiClient *, AvahiIfIndex interfaceIndex,
                             AvahiProtocol protocol, const char *name,
                             std::uint16_t clazz, std::uint16_t type,
                             AvahiLookupFlags,
                             AvahiRecordBrowserCallback callback,
                             void *context) override {
    m_interface = interfaceIndex;
    m_protocol = protocol;
    m_name = name;
    m_class = clazz;
    m_type = type;
    m_callback = callback;
    m_context = context;
    if (m_fail) {
      return nullptr;
    }
    if (m_immediate) {
      emit(AVAHI_BROWSER_NEW, m_data);
    }
    return &m_browser;
  }
  void free(AvahiRecordBrowser *) noexcept override { ++m_freed; }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "fake native error"; }

  void emit(AvahiBrowserEvent event, const std::vector<std::uint8_t> &data) {
    m_callback(&m_browser, m_interface, m_protocol, event, m_name.c_str(),
               m_class, m_type, data.data(), data.size(),
               AVAHI_LOOKUP_RESULT_MULTICAST, m_context);
  }

  bool m_fail{false};
  bool m_immediate{false};
  unsigned m_freed{0};
  AvahiIfIndex m_interface{};
  AvahiProtocol m_protocol{};
  std::uint16_t m_class{};
  std::uint16_t m_type{};
  std::string m_name;
  std::vector<std::uint8_t> m_data;

private:
  AvahiRecordBrowser m_browser{};
  AvahiRecordBrowserCallback m_callback{nullptr};
  void *m_context{nullptr};
};

DiscoveryServiceKey key() { return {"Printer.Room", "_ipp._tcp", "local.", 7}; }

void srvAddRemoveAndLifetime() {
  auto api{std::make_shared<FakeRecordApi>()};
  api->m_immediate = true;
  api->m_data = {0, 0, 0, 0, 0x1f, 0x90, 4, 'H', 'o', 's', 't', 0};
  AvahiClient client{};
  std::vector<DiscoveryAvahiRecordEvent> events;
  DiscoveryAvahiRecordWatch watch{
      api,
      &client,
      key(),
      DiscoveryContribution::kIpv4,
      DiscoveryRecordKind::kSrv,
      DiscoveryOptions{},
      [&](DiscoveryAvahiRecordEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));
        return {};
      }};
  check(watch.start().has_value(), "record watch startup failed");
  check(api->m_interface == 7 && api->m_protocol == AVAHI_PROTO_INET &&
            api->m_class == AVAHI_DNS_CLASS_IN &&
            api->m_type == AVAHI_DNS_TYPE_SRV &&
            api->m_name.find("Printer\\.Room") != std::string::npos,
        "record watch did not retain source and escaped name");
  check(events.size() == 1 && events[0].m_added &&
            std::get<DiscoverySrv>(events[0].m_record).m_port == 8080,
        "immediate SRV callback was not mapped");
  api->emit(AVAHI_BROWSER_REMOVE, api->m_data);
  check(events.size() == 2 && !events[1].m_added,
        "record removal was not delivered");
  check(watch.stop().has_value() && watch.stop().has_value() &&
            api->m_freed == 1,
        "record watch cleanup failed");
  api->emit(AVAHI_BROWSER_NEW, api->m_data);
  check(events.size() == 2, "stopped watch delivered a callback");
}

void txtValidationAndFailures() {
  auto api{std::make_shared<FakeRecordApi>()};
  AvahiClient client{};
  std::vector<DiscoveryAvahiRecordEvent> events;
  std::optional<DiscoveryError> failure;
  DiscoveryAvahiRecordWatch watch{
      api,
      &client,
      key(),
      DiscoveryContribution::kIpv6,
      DiscoveryRecordKind::kTxt,
      DiscoveryOptions{},
      [&](DiscoveryAvahiRecordEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));
        return {};
      },
      [&](DiscoveryError error) { failure = std::move(error); }};
  check(watch.start().has_value(), "TXT watch startup failed");
  check(api->m_protocol == AVAHI_PROTO_INET6 &&
            api->m_type == AVAHI_DNS_TYPE_TXT,
        "TXT watch used wrong native arguments");
  api->emit(AVAHI_BROWSER_NEW, {2, 0, 255, 0});
  check(events.size() == 1 &&
            std::get<std::vector<std::uint8_t>>(events[0].m_record) ==
                std::vector<std::uint8_t>({2, 0, 255, 0}),
        "binary TXT was not copied");
  api->emit(AVAHI_BROWSER_NEW, {2, 0});
  check(failure && !watch.status(), "malformed TXT failure was hidden");
  check(watch.stop().has_value(), "TXT watch stop failed");

  api = std::make_shared<FakeRecordApi>();
  api->m_fail = true;
  DiscoveryAvahiRecordWatch failed{api,
                                   &client,
                                   key(),
                                   DiscoveryContribution::kIpv4,
                                   DiscoveryRecordKind::kSrv,
                                   DiscoveryOptions{},
                                   nullptr};
  const auto result{failed.start()};
  check(!result && result.error().m_message.find("fake native error") !=
                       std::string::npos,
        "native watch creation failure was hidden");

  api = std::make_shared<FakeRecordApi>();
  failure.reset();
  DiscoveryAvahiRecordWatch allocationFailure{
      api,
      &client,
      key(),
      DiscoveryContribution::kIpv4,
      DiscoveryRecordKind::kTxt,
      DiscoveryOptions{},
      [](DiscoveryAvahiRecordEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      },
      [&](DiscoveryError error) { failure = std::move(error); }};
  check(allocationFailure.start().has_value(), "watch startup failed");
  api->emit(AVAHI_BROWSER_NEW, {1, 'a'});
  const auto allocationStatus{allocationFailure.status()};
  check(!allocationStatus &&
            allocationStatus.error().m_kind ==
                DiscoveryErrorKind::kResourceLimit &&
            failure && failure->m_kind == DiscoveryErrorKind::kResourceLimit,
        "record allocation failure classification was lost");
  check(allocationFailure.stop().has_value(), "watch cleanup failed");
}
} // namespace

int main() {
  srvAddRemoveAndLifetime();
  txtValidationAndFailures();
}
