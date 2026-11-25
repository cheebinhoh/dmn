/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file browser-test.cpp
 * @brief Controlled native service browser lifecycle tests.
 */

#include "dmn-discovery-avahi-browser.hpp"

#include <stdexcept>
#include <string>
#include <vector>

struct AvahiClient {};
struct AvahiServiceBrowser {};

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

class FakeBrowserApi final : public DiscoveryAvahiBrowserApi {
public:
  AvahiServiceBrowser *create(AvahiClient *, AvahiIfIndex interfaceIndex,
                              AvahiProtocol protocol, const char *type,
                              const char *domain, AvahiLookupFlags flags,
                              AvahiServiceBrowserCallback callback,
                              void *context) override {
    m_interface = interfaceIndex;
    m_protocol = protocol;
    m_type = type;
    m_domain = domain;
    m_flags = flags;
    m_callback = callback;
    m_context = context;
    if (m_fail) {
      return nullptr;
    }
    if (m_immediate) {
      emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
    }
    return &m_browser;
  }
  void free(AvahiServiceBrowser *) noexcept override { ++m_freed; }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "fake failure"; }

  void emit(AvahiBrowserEvent event, AvahiProtocol protocol,
            AvahiIfIndex interfaceIndex = 7, const char *type = "_ipp._tcp",
            const char *domain = "local.") {
    m_callback(&m_browser, interfaceIndex, protocol, event, "Printer.Room",
               type, domain, AVAHI_LOOKUP_RESULT_MULTICAST, m_context);
  }

  bool m_fail{false};
  bool m_immediate{false};
  int m_freed{0};
  AvahiIfIndex m_interface{0};
  AvahiProtocol m_protocol{0};
  std::string m_type;
  std::string m_domain;
  AvahiLookupFlags m_flags{};

private:
  AvahiServiceBrowser m_browser{};
  AvahiServiceBrowserCallback m_callback{nullptr};
  void *m_context{nullptr};
};

void rejectsResultsOutsideQuery() {
  for (int mismatch = 0; mismatch < 3; ++mismatch) {
    auto api{std::make_shared<FakeBrowserApi>()};
    AvahiClient client{};
    int delivered{0};
    DiscoveryAvahiServiceBrowser browser{
        api,
        &client,
        {"_ipp._tcp", "local.", 7},
        [&](DiscoveryAvahiBrowseEvent) -> DiscoveryResult {
          ++delivered;
          return {};
        }};
    check(browser.start().has_value(), "browser startup failed");
    api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET, mismatch == 0 ? 8 : 7,
              mismatch == 1 ? "_http._tcp" : "_ipp._tcp",
              mismatch == 2 ? "example.org." : "local.");
    check(delivered == 0 && !browser.status(),
          "out-of-query browse result was accepted");
    check(browser.stop().has_value(), "browser cleanup failed");
  }
}

void lifecycleAndTransport() {
  auto api{std::make_shared<FakeBrowserApi>()};
  api->m_immediate = true;
  AvahiClient client{};
  std::vector<DiscoveryAvahiBrowseEvent> events;
  DiscoveryAvahiServiceBrowser browser{
      api,
      &client,
      {"_ipp._tcp", "local.", 7},
      [&](DiscoveryAvahiBrowseEvent event) -> DiscoveryResult {
        events.push_back(std::move(event));
        return {};
      }};
  check(browser.start().has_value(), "browser startup failed");
  check(api->m_interface == 7 && api->m_protocol == AVAHI_PROTO_UNSPEC &&
            api->m_type == "_ipp._tcp" && api->m_domain == "local.",
        "browser query changed");
  check(events.size() == 1 && events[0].m_added &&
            events[0].m_contribution == DiscoveryContribution::kIpv4 &&
            events[0].m_key.m_instance == "Printer.Room",
        "immediate callback was lost");
  api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET6);
  api->emit(AVAHI_BROWSER_REMOVE, AVAHI_PROTO_INET);
  check(events.size() == 3 &&
            events[1].m_contribution == DiscoveryContribution::kIpv6 &&
            !events[2].m_added,
        "transport-specific callbacks were not delivered");
  check(browser.stop().has_value() && browser.stop().has_value() &&
            api->m_freed == 1,
        "browser was not freed exactly once");
  api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
  check(events.size() == 3, "callback after stop was delivered");
}

void failuresAreVisible() {
  auto api{std::make_shared<FakeBrowserApi>()};
  api->m_fail = true;
  AvahiClient client{};
  DiscoveryAvahiServiceBrowser browser{
      api, &client, {"_ipp._tcp", "local.", {}}, nullptr};
  auto result{browser.start()};
  check(!result &&
            result.error().m_message.find("fake failure") != std::string::npos,
        "creation error was not surfaced");
  check(api->m_freed == 0, "failed creation freed a nonexistent browser");

  api = std::make_shared<FakeBrowserApi>();
  api->m_immediate = true;
  DiscoveryAvahiServiceBrowser throwing{
      api,
      &client,
      {"_ipp._tcp", "local.", {}},
      [](DiscoveryAvahiBrowseEvent) -> DiscoveryResult {
        throw std::runtime_error{"callback"};
      }};
  check(!throwing.start() && api->m_freed == 1,
        "immediate callback failure did not roll back");

  api = std::make_shared<FakeBrowserApi>();
  std::optional<DiscoveryError> reported;
  DiscoveryAvahiServiceBrowser asynchronous{
      api,
      &client,
      {"_ipp._tcp", "local.", {}},
      nullptr,
      [&](DiscoveryError error) { reported = std::move(error); }};
  check(asynchronous.start().has_value(), "browser startup failed");
  api->emit(AVAHI_BROWSER_FAILURE, AVAHI_PROTO_INET);
  const auto status{asynchronous.status()};
  check(!status &&
            status.error().m_message.find("fake failure") !=
                std::string::npos &&
            reported && reported->m_message == status.error().m_message,
        "asynchronous browser failure was hidden");
  check(asynchronous.stop().has_value(), "failed browser cleanup failed");

  api = std::make_shared<FakeBrowserApi>();
  reported.reset();
  DiscoveryAvahiServiceBrowser asynchronousThrow{
      api,
      &client,
      {"_ipp._tcp", "local.", {}},
      [](DiscoveryAvahiBrowseEvent) -> DiscoveryResult {
        throw std::runtime_error{"callback"};
      },
      [&](DiscoveryError error) { reported = std::move(error); }};
  check(asynchronousThrow.start().has_value(), "browser startup failed");
  api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
  check(!asynchronousThrow.status() && reported &&
            reported->m_message == "Avahi browser callback failed",
        "throwing callback was not reported");
  check(asynchronousThrow.stop().has_value(), "browser cleanup failed");

  api = std::make_shared<FakeBrowserApi>();
  reported.reset();
  DiscoveryAvahiServiceBrowser allocationFailure{
      api,
      &client,
      {"_ipp._tcp", "local.", {}},
      [](DiscoveryAvahiBrowseEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      },
      [&](DiscoveryError error) { reported = std::move(error); }};
  check(allocationFailure.start().has_value(), "browser startup failed");
  api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
  const auto allocationStatus{allocationFailure.status()};
  check(!allocationStatus &&
            allocationStatus.error().m_kind ==
                DiscoveryErrorKind::kResourceLimit &&
            reported && reported->m_kind == DiscoveryErrorKind::kResourceLimit,
        "browser allocation failure classification was lost");
  check(allocationFailure.stop().has_value(), "browser cleanup failed");

  api = std::make_shared<FakeBrowserApi>();
  DiscoveryAvahiServiceBrowser failedNotification{
      api,
      &client,
      {"_ipp._tcp", "local.", {}},
      [](DiscoveryAvahiBrowseEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      },
      [](DiscoveryError) { throw std::bad_alloc{}; }};
  check(failedNotification.start().has_value(), "browser startup failed");
  api->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
  const auto retained{failedNotification.status()};
  check(!retained &&
            retained.error().m_kind == DiscoveryErrorKind::kResourceLimit,
        "failed error notification lost the allocation-failure latch");
  check(failedNotification.stop().has_value(), "browser cleanup failed");
}
} // namespace

int main() {
  lifecycleAndTransport();
  failuresAreVisible();
  rejectsResultsOutsideQuery();
}
