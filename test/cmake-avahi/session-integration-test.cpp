/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file session-integration-test.cpp
 * @brief Controlled native browse-to-session and disconnect integration.
 */

#include "dmn-discovery-avahi-browser.hpp"
#include "dmn-discovery-avahi.hpp"
#include "dmn-discovery-session.hpp"

#include <avahi-common/error.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

struct AvahiSimplePoll {};
struct AvahiClient {};
struct AvahiServiceBrowser {};

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

class FakePoll final : public DiscoveryAvahiApi {
public:
  AvahiSimplePoll *newPoll() override { return &m_poll; }
  void freePoll(AvahiSimplePoll *) noexcept override {
    m_destroyed.push_back("poll");
  }
  const AvahiPoll *pollApi(AvahiSimplePoll *) override { return &m_api; }
  int iterate(AvahiSimplePoll *, int timeout) override {
    std::unique_lock lock{m_mutex};
    if (timeout != 0) {
      m_changed.wait(lock, [&] { return m_woken || m_disconnect; });
    }
    m_woken = false;
    if (m_disconnect) {
      m_disconnect = false;
      const auto callback{m_callback};
      const auto context{m_context};
      lock.unlock();
      callback(&m_client, AVAHI_CLIENT_FAILURE, context);
    }
    return 0;
  }
  void wake(AvahiSimplePoll *) noexcept override {
    std::lock_guard lock{m_mutex};
    m_woken = true;
    m_changed.notify_one();
  }
  AvahiClient *newClient(const AvahiPoll *, AvahiClientCallback callback,
                         void *context, int *) override {
    m_callback = callback;
    m_context = context;
    callback(&m_client, AVAHI_CLIENT_S_RUNNING, context);
    return &m_client;
  }
  void freeClient(AvahiClient *) noexcept override {
    m_destroyed.push_back("client");
  }
  int clientError(AvahiClient *) override { return AVAHI_ERR_DISCONNECTED; }
  const char *errorString(int) override { return "disconnected"; }

  void disconnect() {
    std::lock_guard lock{m_mutex};
    m_disconnect = true;
    m_changed.notify_one();
  }

  std::vector<std::string> m_destroyed;

private:
  AvahiSimplePoll m_poll{};
  AvahiClient m_client{};
  AvahiPoll m_api{};
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_woken{false};
  bool m_disconnect{false};
  AvahiClientCallback m_callback{nullptr};
  void *m_context{nullptr};
};

class FakeBrowser final : public DiscoveryAvahiBrowserApi {
public:
  explicit FakeBrowser(std::shared_ptr<FakePoll> poll) : m_poll{poll} {}
  AvahiServiceBrowser *create(AvahiClient *, AvahiIfIndex, AvahiProtocol,
                              const char *, const char *, AvahiLookupFlags,
                              AvahiServiceBrowserCallback callback,
                              void *context) override {
    m_callback = callback;
    m_context = context;
    return &m_browser;
  }
  void free(AvahiServiceBrowser *) noexcept override {
    m_poll->m_destroyed.push_back("browser");
  }
  int clientError(AvahiClient *) override { return AVAHI_ERR_DISCONNECTED; }
  const char *errorString(int) override { return "disconnected"; }

  void emit(AvahiBrowserEvent event, AvahiProtocol protocol) {
    m_callback(&m_browser, 7, protocol, event, "Printer.Room", "_ipp._tcp",
               "local.", AVAHI_LOOKUP_RESULT_MULTICAST, m_context);
  }

private:
  std::shared_ptr<FakePoll> m_poll;
  AvahiServiceBrowser m_browser{};
  AvahiServiceBrowserCallback m_callback{nullptr};
  void *m_context{nullptr};
};

void browseAndDisconnect() {
  DiscoverySessionClient client{{}};
  auto session{client.browse(nullptr)};
  check(session.has_value(), "fake browser session could not be created");
  std::uint64_t epoch{session->snapshot().m_delivery.m_epoch};
  std::optional<std::uint64_t> ipv4, ipv6;
  auto api{std::make_shared<FakePoll>()};
  auto browserApi{std::make_shared<FakeBrowser>(api)};
  std::unique_ptr<DiscoveryAvahiServiceBrowser> browser;
  std::promise<void> invalidated;
  std::atomic<int> seconds{0};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api,
      [&](DiscoveryAvahiEvent event) {
        if (event.m_state == AVAHI_CLIENT_S_RUNNING) {
          browser = std::make_unique<DiscoveryAvahiServiceBrowser>(
              browserApi, event.m_client,
              DiscoveryAvahiQuery{"_ipp._tcp", "local.", 7},
              [&](DiscoveryAvahiBrowseEvent change) -> DiscoveryResult {
                auto &generation{change.m_contribution ==
                                         DiscoveryContribution::kIpv4
                                     ? ipv4
                                     : ipv6};
                if (change.m_added) {
                  auto result{session->observe(epoch, change.m_key,
                                               change.m_contribution)};
                  if (!result) {
                    return std::unexpected{result.error()};
                  }
                  generation = *result;
                  return {};
                }
                if (!generation) {
                  return std::unexpected{DiscoveryError{
                      DiscoveryErrorKind::kBackend, "missing contribution"}};
                }
                auto result{session->removeObservation(
                    epoch, change.m_key, change.m_contribution, *generation)};
                generation.reset();
                return result;
              });
          check(browser->start().has_value(), "native browser start failed");
        } else if (event.m_state == AVAHI_CLIENT_FAILURE) {
          browser.reset();
          check(session->backendLost(epoch).has_value(), "backend loss failed");
          invalidated.set_value();
        }
      },
      [&] {
        return std::chrono::steady_clock::time_point{} +
               std::chrono::seconds{seconds.load()};
      },
      [&]() -> DiscoveryResult {
        browser.reset();
        return {};
      })};
  check(client.attachPoller(poller).has_value(), "poll attachment failed");
  check(client
            .dispatchNative([&]() -> DiscoveryResult {
              browserApi->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
              browserApi->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET6);
              return browser->status();
            })
            .has_value(),
        "native adds failed");
  check(session->snapshot().m_browser.m_candidates.size() == 1,
        "two transports did not merge");
  check(client
            .dispatchNative([&]() -> DiscoveryResult {
              browserApi->emit(AVAHI_BROWSER_REMOVE, AVAHI_PROTO_INET);
              return browser->status();
            })
            .has_value(),
        "native remove failed");
  check(session->snapshot().m_browser.m_candidates.size() == 1,
        "removing one transport removed both");
  auto done{invalidated.get_future()};
  api->disconnect();
  check(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready,
        "disconnect was not processed");
  check(session->snapshot().m_browser.m_candidates.empty(),
        "disconnect retained stale observations");
  check(client.shutdown().has_value(), "client shutdown failed");
  check(api->m_destroyed ==
            std::vector<std::string>{"browser", "client", "poll"},
        "native teardown order was incorrect");
}
} // namespace

int main() { browseAndDisconnect(); }
