/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file unified-session-test.cpp
 * @brief Controlled unified browser and advertisement lifecycle.
 */
#include "dmn-discovery-avahi-registration-session.hpp"
#ifdef FIU_ENABLE
#include <fiu-control.h>
#include <fiu.h>
#endif

#include <atomic>
#include <avahi-common/error.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

struct AvahiSimplePoll {};
struct AvahiClient {};
struct AvahiEntryGroup {};
struct AvahiServiceBrowser {};
struct AvahiRecordBrowser {
  unsigned index;
};
struct AvahiServiceResolver {
  unsigned index;
};

namespace {
using namespace dmn::detail;
using namespace std::chrono_literals;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error{message};
}
template <typename F> void waitFor(F f) {
  auto deadline{std::chrono::steady_clock::now() + 4s};
  while (!f() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  check(f(), "timed out");
}

class Poll final : public DiscoveryAvahiApi {
public:
  AvahiSimplePoll *newPoll() override { return &poll; }
  void freePoll(AvahiSimplePoll *) noexcept override { ++pollFreed; }
  const AvahiPoll *pollApi(AvahiSimplePoll *) override { return &api; }
  int iterate(AvahiSimplePoll *, int timeout) override {
    std::unique_lock lock{mutex};
    if (timeout < 0)
      changed.wait(lock, [&] { return woken || event; });
    else if (timeout > 0)
      changed.wait_for(lock, std::chrono::milliseconds{timeout},
                       [&] { return woken || event; });
    woken = false;
    auto action{std::move(event)};
    event = {};
    lock.unlock();
    if (action)
      action();
    return failIteration ? -1 : 0;
  }
  void wake(AvahiSimplePoll *) noexcept override {
    std::lock_guard lock{mutex};
    woken = true;
    changed.notify_one();
  }
  AvahiClient *newClient(const AvahiPoll *, AvahiClientCallback cb, void *ctx,
                         int *) override {
    callback = cb;
    context = ctx;
    ++clients;
    cb(&client, AVAHI_CLIENT_S_RUNNING, ctx);
    return &client;
  }
  void freeClient(AvahiClient *) noexcept override {
    if (onClientFree)
      onClientFree();
    ++clientFreed;
  }
  int clientError(AvahiClient *) override { return AVAHI_ERR_DISCONNECTED; }
  const char *errorString(int) override { return "disconnect"; }
  void post(std::function<void()> f) {
    std::lock_guard lock{mutex};
    check(!event, "pending test event");
    event = std::move(f);
    changed.notify_one();
  }
  void disconnect() {
    post([this] { callback(&client, AVAHI_CLIENT_FAILURE, context); });
  }
  void disconnectNow() { callback(&client, AVAHI_CLIENT_FAILURE, context); }
  void fail() {
    post([this] { failIteration = true; });
  }
  std::atomic<int> clients{0}, clientFreed{0}, pollFreed{0};
  std::function<void()> onClientFree;

private:
  AvahiSimplePoll poll{};
  AvahiClient client{};
  AvahiPoll api{};
  std::mutex mutex;
  std::condition_variable changed;
  bool woken{false};
  bool failIteration{false};
  std::function<void()> event;
  AvahiClientCallback callback{};
  void *context{};
};
class Groups final : public DiscoveryAvahiRegistrationApi {
public:
  AvahiEntryGroup *create(AvahiClient *, AvahiEntryGroupCallback cb,
                          void *ctx) override {
    callback = cb;
    context = ctx;
    ++created;
    if (duringCreate)
      duringCreate();
    return &group;
  }
  void free(AvahiEntryGroup *) noexcept override { ++freed; }
  int add(AvahiEntryGroup *, AvahiIfIndex, AvahiProtocol, AvahiPublishFlags,
          const char *, const char *, const char *, std::uint16_t,
          AvahiStringList *) override {
    return 0;
  }
  int commit(AvahiEntryGroup *) override {
    callback(&group, AVAHI_ENTRY_GROUP_ESTABLISHED, context);
    return 0;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "error"; }
  std::atomic<int> created{0}, freed{0};
  std::function<void()> duringCreate;

private:
  AvahiEntryGroup group{};
  AvahiEntryGroupCallback callback{};
  void *context{};
};
class Browsers final : public DiscoveryAvahiBrowserApi {
public:
  AvahiServiceBrowser *create(AvahiClient *, AvahiIfIndex, AvahiProtocol,
                              const char *, const char *, AvahiLookupFlags,
                              AvahiServiceBrowserCallback cb,
                              void *ctx) override {
    callback = cb;
    context = ctx;
    ++created;
    if (failCreate)
      return nullptr;
    inCreate = true;
    if (duringCreate)
      duringCreate();
    emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET);
    emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET6);
    inCreate = false;
    return &browser;
  }
  void free(AvahiServiceBrowser *) noexcept override {
    if (inCallback || inCreate)
      wrongFree = true;
    ++freed;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "error"; }
  void emit(AvahiBrowserEvent event, AvahiProtocol proto,
            const char *domain = "local.",
            AvahiLookupResultFlags flags = AVAHI_LOOKUP_RESULT_MULTICAST) {
    inCallback = true;
    callback(&browser, 7, proto, event, "Printer", "_ipp._tcp", domain, flags,
             context);
    inCallback = false;
  }
  std::atomic<int> created{0}, freed{0};
  std::atomic<bool> wrongFree{false};
  std::atomic<bool> failCreate{false};
  std::function<void()> duringCreate;

private:
  AvahiServiceBrowser browser{};
  AvahiServiceBrowserCallback callback{};
  void *context{};
  bool inCallback{false}, inCreate{false};
};
class Records final : public DiscoveryAvahiRecordApi {
public:
  struct Item {
    AvahiRecordBrowser native;
    AvahiRecordBrowserCallback callback;
    void *context;
    std::string name;
    std::uint16_t type;
    AvahiProtocol protocol;
    bool live{true};
  };
  AvahiRecordBrowser *create(AvahiClient *, AvahiIfIndex, AvahiProtocol proto,
                             const char *name, std::uint16_t,
                             std::uint16_t type, AvahiLookupFlags,
                             AvahiRecordBrowserCallback cb,
                             void *ctx) override {
    items.push_back(std::make_unique<Item>(Item{
        {static_cast<unsigned>(items.size())}, cb, ctx, name, type, proto}));
    auto &item{*items.back()};
    ++created;
    if (type == AVAHI_DNS_TYPE_SRV)
      emit(item, AVAHI_BROWSER_NEW);
    return &item.native;
  }
  void free(AvahiRecordBrowser *b) noexcept override {
    if (inCallback)
      wrongFree = true;
    items[b->index]->live = false;
    ++freed;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "record failure"; }
  void emit(Item &item, AvahiBrowserEvent event) {
    static const std::vector<std::uint8_t> srv{0,   0,   0,   0,   0x1f, 0x90,
                                               4,   'H', 'o', 's', 't',  5,
                                               'l', 'o', 'c', 'a', 'l',  0};
    inCallback = true;
    item.callback(&item.native, 7, item.protocol, event, item.name.c_str(),
                  AVAHI_DNS_CLASS_IN, item.type, srv.data(), srv.size(),
                  AVAHI_LOOKUP_RESULT_MULTICAST, item.context);
    inCallback = false;
  }
  std::vector<std::unique_ptr<Item>> items;
  std::atomic<int> created{0}, freed{0};
  std::atomic<bool> wrongFree{false};

private:
  bool inCallback{false};
};
class Resolvers final : public DiscoveryAvahiResolverApi {
public:
  struct Item {
    AvahiServiceResolver native;
    AvahiServiceResolverCallback callback;
    void *context;
    AvahiProtocol transport, family;
    bool live{true};
  };
  AvahiServiceResolver *create(AvahiClient *, AvahiIfIndex,
                               AvahiProtocol transport, const char *,
                               const char *, const char *, AvahiProtocol family,
                               AvahiLookupFlags flags,
                               AvahiServiceResolverCallback cb,
                               void *ctx) override {
    check((flags & AVAHI_LOOKUP_NO_TXT) != 0, "missing NO_TXT");
    items.push_back(std::make_unique<Item>(Item{
        {static_cast<unsigned>(items.size())}, cb, ctx, transport, family}));
    auto &item{*items.back()};
    ++created;
    if (emitFound)
      emit(item, AVAHI_RESOLVER_FOUND);
    return &item.native;
  }
  void free(AvahiServiceResolver *r) noexcept override {
    if (inCallback)
      wrongFree = true;
    items[r->index]->live = false;
    ++freed;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "resolver failure"; }
  void emit(Item &item, AvahiResolverEvent event, unsigned char last = 1) {
    AvahiAddress address{};
    address.proto = item.family;
    if (item.family == AVAHI_PROTO_INET) {
      const unsigned char bytes[]{10, 0, 0, last};
      std::memcpy(&address.data.ipv4.address, bytes, sizeof(bytes));
    } else {
      address.data.ipv6.address[0] = 0x20;
      address.data.ipv6.address[15] = last;
    }
    inCallback = true;
    item.callback(&item.native, 7, item.transport, event, "Printer",
                  "_ipp._tcp", "local.", "host.local.", &address, 8080, nullptr,
                  AVAHI_LOOKUP_RESULT_MULTICAST, item.context);
    inCallback = false;
  }
  std::vector<std::unique_ptr<Item>> items;
  std::atomic<int> created{0}, freed{0};
  std::atomic<bool> wrongFree{false};
  std::atomic<bool> emitFound{true};

private:
  bool inCallback{false};
};

void unified() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  std::atomic<int> seconds{0};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers, [&] {
        return std::chrono::steady_clock::time_point{} +
               std::chrono::seconds{seconds.load()};
      })};
  check(client.has_value(), "client create");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'a'}}}, {})};
  auto session{
      (*client)->browse({"_ipp._tcp", "local.", 7}, {}, {true, false})};
  check(registration && session, "admission");
  waitFor([&] {
    return session->snapshot().m_browser.m_candidates.size() == 1 &&
           resolvers->created == 2;
  });
  check(poll->clients == 1 && groups->created == 1 && records->created == 4,
        "unified ownership or sources incorrect");
  check(
      session->snapshot().m_browser.m_candidates[0].m_provenance.m_multicast ==
          true,
      "native browse provenance was not preserved");
  check(session->snapshot()
                .m_browser.m_candidates[0]
                .m_records.m_addresses.size() == 1,
        "family restriction ignored");
  poll->post([browsers] {
    browsers->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET, "local.",
                   AVAHI_LOOKUP_RESULT_LOCAL);
  });
  waitFor([&] {
    return session->snapshot().m_browser.m_candidates[0].m_provenance.m_local ==
           true;
  });
  poll->post([browsers] {
    browsers->emit(AVAHI_BROWSER_REMOVE, AVAHI_PROTO_INET, "\\108ocal.");
  });
  waitFor([&] { return records->freed >= 2 && resolvers->freed >= 1; });
  check(session->snapshot().m_browser.m_candidates.size() == 1,
        "removing one transport removed service");
  check(!session->snapshot().m_browser.m_candidates[0].m_provenance.m_local,
        "removed source retained provenance");
  std::atomic<int> beforeCreated{-1}, beforeFreed{-1};
  poll->post(
      [browsers, resolvers, session = *session, &beforeCreated, &beforeFreed] {
        auto &old{*resolvers->items.back()};
        browsers->emit(AVAHI_BROWSER_REMOVE, AVAHI_PROTO_INET6);
        browsers->emit(AVAHI_BROWSER_NEW, AVAHI_PROTO_INET6, "local.",
                       AVAHI_LOOKUP_RESULT_CACHED);
        resolvers->emit(old, AVAHI_RESOLVER_FOUND, 9);
        const auto candidate{session.snapshot().m_browser.m_candidates.at(0)};
        check(candidate.m_records.m_addresses.at(0).m_bytes.back() == 1,
              "removed source applied a stale callback");
        beforeCreated = resolvers->created.load();
        beforeFreed = resolvers->freed.load();
      });
  waitFor([&] {
    return beforeCreated >= 0 && resolvers->created > beforeCreated &&
           resolvers->freed > beforeFreed;
  });
  check(session->snapshot().m_browser.m_candidates[0].m_provenance.m_cached ==
            true,
        "re-added source lost provenance");
  const int beforeRetry{resolvers->created};
  poll->post([resolvers] {
    resolvers->emit(*resolvers->items.back(), AVAHI_RESOLVER_FAILURE);
  });
  waitFor([&] {
    return !session->snapshot().m_browser.m_candidates[0].m_failures.empty();
  });
  seconds = 1;
  poll->post([] {});
  waitFor([&] {
    return resolvers->created > beforeRetry &&
           session->snapshot().m_browser.m_candidates[0].m_failures.empty();
  });
  const int beforeRefresh{resolvers->created};
  auto refresh{session->refresh({"Printer", "_ipp._tcp", "local.", 7})};
  check(refresh.has_value(), "refresh failed");
  waitFor([&] { return resolvers->created > beforeRefresh; });
  resolvers->emitFound = false;
  const int beforeTimeout{resolvers->created};
  auto timeoutRefresh{session->refresh({"Printer", "_ipp._tcp", "local.", 7})};
  check(timeoutRefresh.has_value(), "timeout refresh failed");
  waitFor([&] { return resolvers->created > beforeTimeout; });
  seconds = 6;
  poll->post([] {});
  waitFor([&] {
    const auto candidate{session->snapshot().m_browser.m_candidates[0]};
    return candidate.m_refresh &&
           candidate.m_refresh->m_state == DiscoveryRefreshState::kFailed;
  });
  resolvers->emitFound = true;
  const int beforeReset{resolvers->created};
  check(session->clearCache().has_value(), "reset failed");
  waitFor([&] {
    return browsers->created == 2 && resolvers->created >= beforeReset + 2;
  });
  poll->disconnect();
  waitFor([&] {
    return !session->snapshot().m_backend_available && groups->freed == 1;
  });
  seconds = 7;
  poll->post([] {});
  waitFor([&] {
    return poll->clients == 2 && browsers->created == 3 && groups->created == 2;
  });
  check(session->stop().has_value(), "browser stop failed");
  check(registration->nativeActive(), "browser stop withdrew registration");
  check((*client)->shutdown().has_value(), "shutdown failed");
  check(!browsers->wrongFree && !records->wrongFree && !resolvers->wrongFree,
        "native object freed during callback");
  check(poll->clientFreed == 2 && poll->pollFreed == 1, "poll teardown");
}

void overflowAndCallbackFailure() {
  for (const bool overflow : {false, true}) {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    auto browsers{std::make_shared<Browsers>()};
    auto records{std::make_shared<Records>()};
    auto resolvers{std::make_shared<Resolvers>()};
    DiscoveryOptions options{};
    if (overflow)
      options.m_max_native_operations = 2;
    auto client{DiscoveryAvahiRegistrationSessions::create(
        options, poll, groups, browsers, records, resolvers)};
    check(client.has_value(), "client create");
    auto registration{(*client)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'a'}}}, {})};
    auto browser{(*client)->browse(
        {"_ipp._tcp", "local.", 7}, [overflow](DiscoveryDelivery event) {
          if (!overflow && event.m_data)
            throw std::runtime_error{"callback failed"};
        })};
    check(registration && browser, "admission");
    if (overflow) {
      waitFor([&] {
        return browser->snapshot().m_delivery.m_state ==
               DiscoveryDeliveryState::kOverflow;
      });
    } else {
      waitFor([&] { return browser->snapshot().m_stop_pending; });
    }

    waitFor([&] { return browsers->freed == 1; });
    check(registration->nativeActive(),
          "browser failure withdrew registration");
    check(!browsers->wrongFree && !records->wrongFree && !resolvers->wrongFree,
          "browser failure freed inside callback");
    check(browser->stop().has_value(), "browser stop failed");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
}
void nestedDisconnect() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  std::atomic<bool> once{true};
  std::atomic<int> seconds{0};
  browsers->duringCreate = [poll, &once] {
    if (once.exchange(false))
      poll->disconnectNow();
  };
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers, [&] {
        return std::chrono::steady_clock::time_point{} +
               std::chrono::seconds{seconds.load()};
      })};
  check(client.has_value(), "client create");
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(browser.has_value(), "browser admission");
  waitFor([&] { return !browser->snapshot().m_backend_available; });
  check(browsers->freed == 1 && !browsers->wrongFree,
        "nested disconnect freed browser in constructor");
  waitFor([&] { return poll->clientFreed == 1; });
  seconds = 1;
  poll->post([] {});
  waitFor([&] { return poll->clients == 2 && browsers->created == 2; });
  check((*client)->shutdown().has_value(), "shutdown failed");
}
void nativeAndPollFailure(bool stopBeforeShutdown) {
  for (const bool fatal : {false, true}) {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    auto browsers{std::make_shared<Browsers>()};
    auto records{std::make_shared<Records>()};
    auto resolvers{std::make_shared<Resolvers>()};
    auto client{DiscoveryAvahiRegistrationSessions::create(
        {}, poll, groups, browsers, records, resolvers)};
    check(client.has_value(), "client create");
    auto registration{(*client)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'a'}}}, {})};
    auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
    check(registration && browser, "admission");
    if (fatal)
      poll->fail();
    else
      poll->post([browsers] {
        browsers->emit(AVAHI_BROWSER_FAILURE, AVAHI_PROTO_INET);
      });
    waitFor([&] { return browser->snapshot().m_cleanup_error.has_value(); });
    waitFor([&] { return browsers->freed == 1; });
    check(!browser->snapshot().m_backend_available, "failure was hidden");
    if (fatal) {
      waitFor([&] { return groups->freed == 1 && poll->clientFreed == 1; });
      check(registration->status().m_state ==
                DiscoveryRegistrationState::kFailed,
            "fatal poll failure not reported");
      if (stopBeforeShutdown) {
        check(browser->stop().has_value(), "browser stop after poll failure");
        check(registration->stop().has_value(),
              "registration stop after poll failure");
      }
      check(!(*client)->shutdown(), "fatal worker reported success");
      check(browser->snapshot().m_delivery.m_state ==
                DiscoveryDeliveryState::kStopped,
            "browser was not stopped after fatal worker shutdown");
      check(browser->snapshot().m_cleanup_error.has_value(),
            "browser lost its fatal error after shutdown");
      check(registration->status().m_state ==
                DiscoveryRegistrationState::kStopped,
            "registration was not stopped after fatal worker shutdown");
      check(registration->status().m_error.has_value(),
            "registration lost its fatal error after shutdown");
      check(browser->stop().has_value(), "browser stop after fatal failure");
      check(registration->stop().has_value(),
            "registration stop after fatal failure");
    } else {
      check(registration->nativeActive(),
            "browser failure withdrew registration");
      check(browser->clearCache().has_value(), "failed browser reset");
      waitFor([&] { return browsers->created == 2; });
      check(browser->snapshot().m_backend_available &&
                !browser->snapshot().m_cleanup_error,
            "reset did not recover browser");
      check(browser->stop().has_value(), "failed browser stop");
      check((*client)->shutdown().has_value(), "shutdown failed");
    }
  }
}
void failedResetStillDrains() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  std::promise<void> entered, release;
  auto released{release.get_future().share()};
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7},
                                 [&](DiscoveryDelivery event) {
                                   if (event.m_data) {
                                     entered.set_value();
                                     released.wait();
                                   }
                                 })};
  check(browser.has_value(), "browser admission");
  check(entered.get_future().wait_for(2s) == std::future_status::ready,
        "callback did not begin");
  browsers->failCreate = true;
  auto reset{
      std::async(std::launch::async, [&] { return browser->clearCache(); })};
  waitFor([&] { return browsers->created == 2; });
  const bool returnedEarly{reset.wait_for(20ms) == std::future_status::ready};
  release.set_value();
  check(!returnedEarly, "failed reset did not drain callback");
  check(!reset.get(), "failed native reset reported success");
  check(browser->stop().has_value(), "browser stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}
#ifdef FIU_ENABLE
void postAdmissionAllocationFailure() {
  check(fiu_init(0) == 0, "fiu initialization failed");
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  DiscoveryOptions options;
  options.m_max_browsers = 1;
  options.m_max_registrations = 1;
  auto client{DiscoveryAvahiRegistrationSessions::create(
      options, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  for (const char *point : {"dmn/discovery/avahi/browser-post-admission",
                            "dmn/discovery/avahi/browser-after-start"}) {
    check(fiu_enable(point, 1, nullptr, FIU_ONETIME) == 0,
          "could not enable browser fault");
    auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
    check(browser.has_value(), "accepted browser handle was lost");
    check(fiu_fail(point) == 0, "browser fault was not reached");
    check(browser->snapshot().m_delivery.m_state ==
              DiscoveryDeliveryState::kOverflow,
          "browser allocation failure was not observable");
    check(browser->stop().has_value(), "browser stop failed");
    browser = (*client)->browse({"_ipp._tcp", "local.", 7}, {});
    check(browser.has_value(), "browser capacity was not released");
    check(browser->stop().has_value(), "second browser stop failed");
  }
  for (const char *point : {"dmn/discovery/avahi/registration-post-admission",
                            "dmn/discovery/avahi/registration-after-start"}) {
    check(fiu_enable(point, 1, nullptr, FIU_ONETIME) == 0,
          "could not enable registration fault");
    auto registration{(*client)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
    check(registration.has_value(), "accepted registration handle was lost");
    check(fiu_fail(point) == 0, "registration fault was not reached");
    check(registration->status().m_state ==
              DiscoveryRegistrationState::kOverflow,
          "registration allocation failure was not observable");
    check(registration->stop().has_value(), "registration stop failed");
    registration = (*client)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {});
    check(registration.has_value(), "registration capacity was not released");
    check(registration->stop().has_value(), "second registration stop failed");
  }
  for (const char *point : {"dmn/discovery/avahi/browser-post-admission-error",
                            "dmn/discovery/avahi/browser-after-start-error"}) {
    check(fiu_enable(point, 1, nullptr, FIU_ONETIME) == 0,
          "could not enable browser exception");
    auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
    check(browser.has_value(), "browser exception lost accepted handle");
    check(fiu_fail(point) == 0, "browser exception was not reached");
    auto status{browser->snapshot()};
    check(status.m_cleanup_error &&
              status.m_cleanup_error->m_kind == DiscoveryErrorKind::kBackend &&
              !status.m_backend_available,
          "browser exception was not reported");
    check(browser->stop().has_value(), "browser stop after exception failed");
  }
  for (const char *point :
       {"dmn/discovery/avahi/registration-post-admission-error",
        "dmn/discovery/avahi/registration-after-start-error"}) {
    check(fiu_enable(point, 1, nullptr, FIU_ONETIME) == 0,
          "could not enable registration exception");
    auto registration{(*client)->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
    check(registration.has_value(),
          "registration exception lost accepted handle");
    check(fiu_fail(point) == 0, "registration exception was not reached");
    auto status{registration->status()};
    check(status.m_state == DiscoveryRegistrationState::kFailed &&
              status.m_error &&
              status.m_error->m_kind == DiscoveryErrorKind::kBackend,
          "registration exception was not reported");
    check(registration->stop().has_value(),
          "registration stop after exception failed");
  }
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void postAdmissionFailureFromCallback() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  DiscoveryOptions options;
  options.m_max_browsers = 2;
  auto created{DiscoveryAvahiRegistrationSessions::create(
      options, poll, groups, browsers, records, resolvers)};
  check(created.has_value(), "client create");
  auto client{*created};
  using BrowserResult = std::expected<DiscoverySessionHandle, DiscoveryError>;
  using RegistrationResult =
      std::expected<DiscoveryRegistrationHandle, DiscoveryError>;
  std::promise<std::pair<BrowserResult, RegistrationResult>> completed;
  auto result{completed.get_future()};
  std::atomic<bool> entered{false};
  auto first{client->browse({"_ipp._tcp", "local.", 7}, [&](DiscoveryDelivery) {
    if (entered.exchange(true))
      return;
    if (fiu_enable("dmn/discovery/avahi/browser-post-admission-nested", 1,
                   nullptr, FIU_ONETIME) != 0 ||
        fiu_enable("dmn/discovery/avahi/registration-post-admission-nested", 1,
                   nullptr, FIU_ONETIME) != 0) {
      completed.set_exception(
          std::make_exception_ptr(std::runtime_error{"fiu setup failed"}));
      return;
    }
    auto browser{client->browse({"_ipp._tcp", "local.", 7}, {})};
    auto registration{client->advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
    completed.set_value({std::move(browser), std::move(registration)});
  })};
  check(first.has_value(), "initial browser admission");
  check(result.wait_for(4s) == std::future_status::ready,
        "nested admission did not complete");
  auto [browser, registration]{result.get()};
  check(browser && registration, "nested admission lost accepted handles");
  check(browser->snapshot().m_delivery.m_state ==
                DiscoveryDeliveryState::kOverflow &&
            registration->status().m_state ==
                DiscoveryRegistrationState::kOverflow,
        "nested admission allocation failure was not observable");
  check(browser->stop().has_value() && registration->stop().has_value(),
        "nested handles failed to stop");
  check(first->stop().has_value(), "initial browser stop failed");
  check(client->shutdown().has_value(), "shutdown failed");
}

void disconnectDuringFailedAdmission() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  check(registration.has_value(), "registration admission");
  check(groups->created == 1, "registration did not start");
  std::atomic<bool> releasedBeforeClient{false};
  poll->onClientFree = [&] {
    releasedBeforeClient = groups->freed == groups->created &&
                           browsers->freed == browsers->created;
  };
  browsers->duringCreate = [poll] { poll->disconnectNow(); };
  check(fiu_enable("dmn/discovery/avahi/browser-after-disconnect", 1, nullptr,
                   FIU_ONETIME) == 0,
        "could not enable disconnect fault");
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(browser.has_value(), "browser admission lost handle");
  waitFor([&] { return poll->clientFreed == 1; });
  check(releasedBeforeClient, "native object outlived disconnected client");
  check(!browsers->wrongFree, "browser freed inside native callback");
  check(browser->stop().has_value(), "browser stop failed");
  check(registration->stop().has_value(), "registration stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void disconnectDuringFailedRegistration() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(browser.has_value(), "browser admission");
  waitFor([&] { return records->created > 0 && resolvers->created > 0; });
  std::atomic<bool> releasedBeforeClient{false};
  poll->onClientFree = [&] {
    releasedBeforeClient = groups->freed == groups->created &&
                           browsers->freed == browsers->created &&
                           records->freed == records->created &&
                           resolvers->freed == resolvers->created;
  };
  groups->duringCreate = [poll] { poll->disconnectNow(); };
  check(fiu_enable("dmn/discovery/avahi/registration-after-disconnect", 1,
                   nullptr, FIU_ONETIME) == 0,
        "could not enable disconnect fault");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  check(registration.has_value(), "registration admission lost handle");
  waitFor([&] { return poll->clientFreed == 1; });
  check(releasedBeforeClient, "native object outlived disconnected client");
  check(!browsers->wrongFree && !records->wrongFree && !resolvers->wrongFree,
        "native object freed inside callback");
  check(browser->stop().has_value(), "browser stop failed");
  check(registration->stop().has_value(), "registration stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void backendReportAllocationInvalidatesAllSessions(bool partial) {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  auto existing{(*client)->advertise(
      {{"Existing", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  check(existing.has_value(), "existing registration admission");
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(browser.has_value(), "browser admission");
  waitFor([&] { return records->created > 0 && resolvers->created > 0; });
  groups->duringCreate = [poll] { poll->disconnectNow(); };
  const char *lossPoint{
      partial ? "dmn/discovery/avahi/loss-transition-partial-error"
              : "dmn/discovery/avahi/loss-transition-error"};
  const char *reportPoint{
      partial ? "dmn/discovery/avahi/backend-report-partial-allocation"
              : "dmn/discovery/avahi/backend-report-allocation"};
  check(fiu_enable(lossPoint, 1, nullptr, FIU_ONETIME) == 0,
        "could not enable transition fault");
  check(fiu_enable(reportPoint, 1, nullptr, FIU_ONETIME) == 0,
        "could not enable backend report fault");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  check(registration.has_value(), "registration admission lost handle");
  check(fiu_fail(lossPoint) == 0 && fiu_fail(reportPoint) == 0,
        "fault was not reached");
  auto status{browser->snapshot()};
  check(status.m_delivery.m_state == DiscoveryDeliveryState::kOverflow &&
            !status.m_backend_available &&
            existing->status().m_state ==
                DiscoveryRegistrationState::kOverflow &&
            registration->status().m_state ==
                DiscoveryRegistrationState::kOverflow,
        "backend allocation failure was not latched for all sessions");
  check(browser->stop().has_value(), "browser stop failed");
  check(existing->stop().has_value(), "existing registration stop failed");
  check(registration->stop().has_value(), "registration stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void browserPreAdmissionExceptionReleasesCapacity() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  DiscoveryOptions options;
  options.m_max_browsers = 1;
  auto client{DiscoveryAvahiRegistrationSessions::create(
      options, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  check(fiu_enable("dmn/discovery/browser/model-allocation", 1, nullptr,
                   FIU_ONETIME) == 0,
        "could not enable model fault");
  auto failed{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(!failed && failed.error().m_kind == DiscoveryErrorKind::kResourceLimit,
        "model failure was not reported");
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(browser.has_value(), "failed admission retained browser capacity");
  check(browser->stop().has_value(), "browser stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void backendReportReturnedAllocationInvalidatesAllSessions() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  std::atomic<bool> failTransitions{false};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers,
      std::chrono::steady_clock::now,
      [&](DiscoveryRegistrationFaultPoint point) {
        if (failTransitions &&
            point == DiscoveryRegistrationFaultPoint::kTransitionDelivery)
          throw std::bad_alloc{};
      })};
  check(client.has_value(), "client create");
  auto existing{(*client)->advertise(
      {{"Existing", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(existing && browser, "initial admission");
  waitFor([&] { return records->created > 0 && resolvers->created > 0; });
  groups->duringCreate = [poll, &failTransitions] {
    failTransitions = true;
    poll->disconnectNow();
  };
  check(fiu_enable("dmn/discovery/avahi/loss-transition-report-error", 1,
                   nullptr, FIU_ONETIME) == 0,
        "could not enable transition fault");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  check(registration.has_value(), "registration admission lost handle");
  const auto status{browser->snapshot()};
  check(status.m_delivery.m_state == DiscoveryDeliveryState::kOverflow &&
            !status.m_backend_available &&
            existing->status().m_state == DiscoveryRegistrationState::kOverflow,
        "returned allocation error did not invalidate all sessions");
  failTransitions = false;
  check(browser->stop().has_value(), "browser stop failed");
  check(existing->stop().has_value(), "existing registration stop failed");
  check(registration->stop().has_value(), "registration stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void existingOverflowDoesNotSpreadDuringBackendFailure() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  check(fiu_enable("dmn/discovery/avahi/registration-existing-overflow", 1,
                   nullptr, FIU_ONETIME) == 0,
        "could not enable registration fault");
  auto registration{(*client)->advertise(
      {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}}, {})};
  auto browser{(*client)->browse({"_ipp._tcp", "local.", 7}, {})};
  check(registration && browser, "admission");
  check(registration->status().m_state == DiscoveryRegistrationState::kOverflow,
        "registration did not overflow");
  waitFor([&] { return records->created > 0 && resolvers->created > 0; });
  poll->fail();
  waitFor([&] { return poll->clientFreed == 1; });
  auto status{browser->snapshot()};
  check(status.m_delivery.m_state == DiscoveryDeliveryState::kRunning &&
            !status.m_backend_available && status.m_cleanup_error &&
            status.m_cleanup_error->m_kind == DiscoveryErrorKind::kBackend,
        "prior registration overflow contaminated browser failure");
  check(browser->stop().has_value(), "browser stop failed");
  check(registration->stop().has_value(), "registration stop failed");
  check(!(*client)->shutdown(), "poll failure was not retained");
}

void cleanupAllocationReleasesAllNativeObjects() {
  check(fiu_init(0) == 0, "fiu initialization failed");
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto browsers{std::make_shared<Browsers>()};
  auto records{std::make_shared<Records>()};
  auto resolvers{std::make_shared<Resolvers>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, browsers, records, resolvers)};
  check(client.has_value(), "client create");
  auto first{(*client)->browse({"_ipp._tcp", "local.", 7}, {}, {true, false})};
  auto second{(*client)->browse({"_ipp._tcp", "local.", 7}, {}, {true, false})};
  check(first && second, "browser admission");
  waitFor([&] { return records->created == 8 && resolvers->created == 4; });
  auto releasedBeforeClient{std::make_shared<std::atomic<bool>>(false)};
  poll->onClientFree = [browsers, records, resolvers, releasedBeforeClient] {
    *releasedBeforeClient = browsers->freed == browsers->created &&
                            records->freed == records->created &&
                            resolvers->freed == resolvers->created;
  };
  check(fiu_enable("dmn/discovery/avahi/browser-cleanup-allocation", 1, nullptr,
                   FIU_ONETIME) == 0,
        "could not enable cleanup fault");
  check(fiu_enable("dmn/discovery/avahi/poll-failure-allocation", 1, nullptr,
                   FIU_ONETIME) == 0,
        "could not enable poll fault");
  poll->fail();
  waitFor([&] { return poll->clientFreed == 1; });
  check(fiu_fail("dmn/discovery/avahi/poll-failure-allocation") == 0,
        "poll fault was not reached");
  check(fiu_fail("dmn/discovery/avahi/browser-cleanup-allocation") == 0,
        "cleanup fault was not reached");
  check(*releasedBeforeClient, "native objects outlived their Avahi client");
  check(!(*client)->shutdown(), "failed worker reported success");
}
#endif
} // namespace

int main() {
  unified();
  overflowAndCallbackFailure();
  nestedDisconnect();
  nativeAndPollFailure(false);
  nativeAndPollFailure(true);
  failedResetStillDrains();
#ifdef FIU_ENABLE
  postAdmissionAllocationFailure();
  postAdmissionFailureFromCallback();
  disconnectDuringFailedAdmission();
  disconnectDuringFailedRegistration();
  backendReportAllocationInvalidatesAllSessions(false);
  backendReportAllocationInvalidatesAllSessions(true);
  backendReportReturnedAllocationInvalidatesAllSessions();
  existingOverflowDoesNotSpreadDuringBackendFailure();
  browserPreAdmissionExceptionReleasesCapacity();
  cleanupAllocationReleasesAllNativeObjects();
#endif
}
