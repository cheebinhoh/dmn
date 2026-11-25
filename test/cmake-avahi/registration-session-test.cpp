/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file registration-session-test.cpp
 * @brief Controlled native registration/session integration tests.
 */

#include "dmn-discovery-avahi-registration-session.hpp"

#include <avahi-common/error.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

struct AvahiSimplePoll {};
struct AvahiClient {};
struct AvahiEntryGroup {
  AvahiEntryGroupCallback callback;
  void *context;
};

namespace {
using namespace dmn::detail;
using namespace std::chrono_literals;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

template <typename Predicate> void waitFor(Predicate predicate) {
  const auto deadline{std::chrono::steady_clock::now() + 4s};
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  check(predicate(), "timed out waiting for registration state");
}

class Poll final : public DiscoveryAvahiApi {
public:
  AvahiSimplePoll *newPoll() override { return &m_poll; }
  void freePoll(AvahiSimplePoll *) noexcept override { ++pollsFreed; }
  const AvahiPoll *pollApi(AvahiSimplePoll *) override { return &m_api; }
  int iterate(AvahiSimplePoll *, int timeout) override {
    std::unique_lock lock{m_mutex};
    if (timeout < 0) {
      m_changed.wait(lock, [&] { return m_woken || m_event; });
    } else if (timeout > 0) {
      m_changed.wait_for(lock, std::chrono::milliseconds{timeout},
                         [&] { return m_woken || m_event; });
    }
    m_woken = false;
    auto event{std::move(m_event)};
    m_event = {};
    lock.unlock();
    if (event) {
      event();
    }
    return m_fail_iteration ? -1 : 0;
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
    ++clientsCreated;
    callback(&m_client,
             initiallyRunning ? AVAHI_CLIENT_S_RUNNING
                              : AVAHI_CLIENT_CONNECTING,
             context);
    return &m_client;
  }
  void freeClient(AvahiClient *) noexcept override { ++clientsFreed; }
  int clientError(AvahiClient *) override { return AVAHI_ERR_DISCONNECTED; }
  const char *errorString(int) override { return "disconnected"; }
  void post(std::function<void()> event) {
    std::lock_guard lock{m_mutex};
    check(!m_event, "only one pending fake event supported");
    m_event = std::move(event);
    m_changed.notify_one();
  }
  void disconnect() {
    post([this] { m_callback(&m_client, AVAHI_CLIENT_FAILURE, m_context); });
  }
  void disconnectNow() {
    m_callback(&m_client, AVAHI_CLIENT_FAILURE, m_context);
  }
  void running() {
    post([this] { m_callback(&m_client, AVAHI_CLIENT_S_RUNNING, m_context); });
  }
  void failIteration() {
    post([this] { m_fail_iteration = true; });
  }
  bool initiallyRunning{true};
  std::atomic<int> clientsCreated{0}, clientsFreed{0}, pollsFreed{0};

private:
  AvahiSimplePoll m_poll{};
  AvahiClient m_client{};
  AvahiPoll m_api{};
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_woken{false};
  bool m_fail_iteration{false};
  std::function<void()> m_event;
  AvahiClientCallback m_callback{nullptr};
  void *m_context{nullptr};
};

class Groups final : public DiscoveryAvahiRegistrationApi {
public:
  AvahiEntryGroup *create(AvahiClient *, AvahiEntryGroupCallback callback,
                          void *context) override {
    if (failCreateAllocation) {
      throw std::bad_alloc{};
    }
    if (created++ == 0) {
      worker = std::this_thread::get_id();
    }
    check(worker == std::this_thread::get_id(), "create off worker");
    auto *group{new AvahiEntryGroup{callback, context}};
    current = group;
    inCreate = true;
    if (duringCreate) {
      duringCreate();
    }
    callback(group, AVAHI_ENTRY_GROUP_UNCOMMITED, context);
    inCreate = false;
    return group;
  }
  void free(AvahiEntryGroup *group) noexcept override {
    if (worker != std::this_thread::get_id() || inCallback || inCreate) {
      wrongFree = true;
    }
    if (current == group) {
      current = nullptr;
    }
    ++freed;
    delete group;
  }
  int add(AvahiEntryGroup *, AvahiIfIndex, AvahiProtocol, AvahiPublishFlags,
          const char *, const char *, const char *, std::uint16_t,
          AvahiStringList *) override {
    return 0;
  }
  int commit(AvahiEntryGroup *group) override {
    if (commitResult != 0) {
      return commitResult;
    }
    emit(group, AVAHI_ENTRY_GROUP_ESTABLISHED);
    return 0;
  }
  int clientError(AvahiClient *) override { return AVAHI_ERR_FAILURE; }
  const char *errorString(int) override { return "fake failure"; }
  void emit(AvahiEntryGroup *group, AvahiEntryGroupState state) {
    inCallback = true;
    group->callback(group, state, group->context);
    inCallback = false;
  }
  std::atomic<int> created{0}, freed{0};
  int commitResult{0};
  bool failCreateAllocation{false};
  std::function<void()> duringCreate;
  std::atomic<bool> wrongFree{false};
  AvahiEntryGroup *current{nullptr}; // Accessed only by the worker.

private:
  std::thread::id worker;
  bool inCallback{false};
  bool inCreate{false};
};

DiscoveryAdvertisement advertisement() {
  return {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {{'a'}}};
}

void lifecycle() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
  check(client.has_value(), "client creation failed");
  auto registration{(*client)->advertise(advertisement(), {})};
  check(registration.has_value(), "registration admission failed");
  check(registration->status().m_state ==
            DiscoveryRegistrationState::kEstablished,
        "synchronous establishment lost");
  check(groups->created == 1, "entry group not created");

  poll->disconnect();
  waitFor([&] {
    return registration->status().m_state ==
           DiscoveryRegistrationState::kUnavailable;
  });
  waitFor([&] { return groups->freed == 1 && poll->clientsFreed == 1; });
  waitFor([&] {
    return groups->created == 2 && registration->status().m_state ==
                                       DiscoveryRegistrationState::kEstablished;
  });

  poll->post(
      [groups] { groups->emit(groups->current, AVAHI_ENTRY_GROUP_COLLISION); });
  waitFor([&] {
    return registration->status().m_state ==
           DiscoveryRegistrationState::kCollision;
  });
  waitFor([&] { return groups->freed == 2; });
  check(registration->stop().has_value(), "registration stop failed");
  check(registration->stop().has_value(), "repeated stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
  check(!groups->wrongFree && poll->pollsFreed == 1,
        "incorrect worker or callback cleanup");
}

void stopOwnsOnlyOne() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
  check(client.has_value(), "client creation failed");
  auto first{(*client)->advertise(advertisement(), {})};
  auto second{(*client)->advertise(advertisement(), {})};
  check(first && second, "two registrations not accepted");
  check(first->stop().has_value(), "first stop failed");
  check(groups->freed == 1 && second->nativeActive(),
        "first stop affected second registration");
  check((*client)->shutdown().has_value(), "shutdown failed");
  check(groups->freed == 2 && !groups->wrongFree,
        "shutdown did not release second registration");
}

void offlineAdmissionAndPreflight() {
  auto poll{std::make_shared<Poll>()};
  poll->initiallyRunning = false;
  auto groups{std::make_shared<Groups>()};
  auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
  check(client.has_value(), "client creation failed");
  auto invalid{advertisement()};
  invalid.m_txt.push_back({});
  auto rejected{(*client)->advertise(std::move(invalid), {})};
  check(!rejected &&
            rejected.error().m_kind == DiscoveryErrorKind::kUnsupported,
        "unsupported advertisement accepted offline");
  auto kept{(*client)->advertise(advertisement(), {})};
  auto stopped{(*client)->advertise(advertisement(), {})};
  check(kept && stopped &&
            kept->status().m_state == DiscoveryRegistrationState::kUnavailable,
        "offline admission failed");
  check(stopped->stop().has_value(), "offline stop failed");
  poll->running();
  waitFor([&] {
    return kept->status().m_state == DiscoveryRegistrationState::kEstablished;
  });
  check(groups->created == 1, "recovery restarted stopped registration");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void nativeFailuresAndOverflow() {
  {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    groups->failCreateAllocation = true;
    auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration &&
              registration->status().m_state ==
                  DiscoveryRegistrationState::kFailed &&
              registration->status().m_error &&
              registration->status().m_error->m_kind ==
                  DiscoveryErrorKind::kResourceLimit,
          "native allocation failure hidden");
    check(!registration->nativeActive(), "failed native work remained active");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
  {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    groups->commitResult = AVAHI_ERR_COLLISION;
    auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration && registration->status().m_state ==
                              DiscoveryRegistrationState::kCollision,
          "immediate collision hidden");
    check(groups->freed == 1 && !groups->wrongFree,
          "immediate collision cleanup failed");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
  {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    groups->commitResult = AVAHI_ERR_FAILURE;
    auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration && registration->status().m_state ==
                              DiscoveryRegistrationState::kFailed,
          "commit failure hidden");
    check(groups->freed == 1, "failed commit leaked group");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
  {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration.has_value(), "registration admission failed");
    poll->post(
        [groups] { groups->emit(groups->current, AVAHI_ENTRY_GROUP_FAILURE); });
    waitFor([&] {
      return registration->status().m_state ==
             DiscoveryRegistrationState::kFailed;
    });
    waitFor([&] { return groups->freed == 1; });
    check(!groups->wrongFree, "failure freed group in callback");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
  {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    std::atomic<int> transitions{0};
    auto client{DiscoveryAvahiRegistrationSessions::create(
        {}, poll, groups, std::chrono::steady_clock::now,
        [&](DiscoveryRegistrationFaultPoint point) {
          if (point == DiscoveryRegistrationFaultPoint::kTransitionDelivery &&
              ++transitions == 2) {
            throw std::bad_alloc{};
          }
        })};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration && registration->status().m_state ==
                              DiscoveryRegistrationState::kOverflow,
          "delivery overflow hidden");
    check(groups->freed == 1 && !groups->wrongFree,
          "overflow did not safely withdraw group");
    check((*client)->shutdown().has_value(), "shutdown failed");
  }
}

void destructionWithdraws() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  DiscoveryRegistrationHandle registration;
  {
    auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
    check(client.has_value(), "client creation failed");
    auto accepted{(*client)->advertise(advertisement(), {})};
    check(accepted.has_value(), "registration admission failed");
    registration = *accepted;
  }
  check(groups->freed == 1 && poll->clientsFreed == 1 && !groups->wrongFree,
        "destruction did not release group before client");
  check(registration.status().m_state == DiscoveryRegistrationState::kStopped,
        "status not retained after destruction");
}

void nestedClientLossDuringCreate() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  groups->duringCreate = [poll] { poll->disconnectNow(); };
  auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
  check(client.has_value(), "client creation failed");
  auto registration{(*client)->advertise(advertisement(), {})};
  check(registration && registration->status().m_state ==
                            DiscoveryRegistrationState::kUnavailable,
        "nested disconnect not applied");
  check(groups->freed == 1 && !groups->wrongFree,
        "nested disconnect freed group during native creation");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void fatalBackendFailures() {
  for (const bool callbackFailure : {false, true}) {
    auto poll{std::make_shared<Poll>()};
    auto groups{std::make_shared<Groups>()};
    std::atomic<bool> inject{false};
    auto client{DiscoveryAvahiRegistrationSessions::create(
        {}, poll, groups, std::chrono::steady_clock::now,
        [&](DiscoveryRegistrationFaultPoint point) {
          if (point == DiscoveryRegistrationFaultPoint::kTransitionDelivery &&
              inject.exchange(false)) {
            throw std::runtime_error{"injected transition failure"};
          }
        })};
    check(client.has_value(), "client creation failed");
    auto registration{(*client)->advertise(advertisement(), {})};
    check(registration.has_value(), "registration admission failed");
    if (callbackFailure) {
      inject = true;
      poll->disconnect();
    } else {
      poll->failIteration();
    }
    waitFor([&] {
      return registration->status().m_state ==
             DiscoveryRegistrationState::kFailed;
    });
    waitFor([&] { return poll->clientsFreed == 1; });
    check(groups->freed == 1 && !groups->wrongFree,
          "fatal failure did not withdraw native group");
    check(registration->status().m_error.has_value(),
          "fatal failure detail missing");
    check(!(*client)->shutdown(), "fatal poll failure reported as success");
  }
}

void initialCallbackFailureKeepsAcceptedHandle() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  DiscoveryOptions options{};
  options.m_max_registrations = 1;
  auto client{
      DiscoveryAvahiRegistrationSessions::create(options, poll, groups)};
  check(client.has_value(), "client creation failed");
  auto registration{(*client)->advertise(
      advertisement(), [](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kUnavailable) {
          throw std::runtime_error{"initial callback failed"};
        }
      })};
  check(registration.has_value(), "accepted handle was orphaned");
  waitFor([&] { return registration->status().m_error.has_value(); });
  check(registration->stop().has_value(), "accepted handle cleanup failed");
  auto retry{(*client)->advertise(advertisement(), {})};
  check(retry.has_value(), "failed admission retained registration capacity");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void stopDrainsCallback() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto client{DiscoveryAvahiRegistrationSessions::create({}, poll, groups)};
  check(client.has_value(), "client creation failed");
  std::promise<void> entered;
  std::promise<void> release;
  auto released{release.get_future().share()};
  auto registration{(*client)->advertise(
      advertisement(), [&](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          entered.set_value();
          released.wait();
        }
      })};
  check(registration.has_value(), "registration admission failed");
  check(entered.get_future().wait_for(2s) == std::future_status::ready,
        "callback did not start");
  auto stopped{
      std::async(std::launch::async, [&] { return registration->stop(); })};
  waitFor([&] { return groups->freed == 1; });
  check(stopped.wait_for(10ms) == std::future_status::timeout,
        "stop returned before callback drained");
  release.set_value();
  check(stopped.get().has_value(), "stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}

void callbackFailureFallback() {
  auto poll{std::make_shared<Poll>()};
  auto groups{std::make_shared<Groups>()};
  auto client{DiscoveryAvahiRegistrationSessions::create(
      {}, poll, groups, std::chrono::steady_clock::now,
      [](DiscoveryRegistrationFaultPoint point) {
        if (point == DiscoveryRegistrationFaultPoint::kCallbackCleanup) {
          throw std::bad_alloc{};
        }
      })};
  check(client.has_value(), "client creation failed");
  auto registration{(*client)->advertise(
      advertisement(), [](DiscoveryRegistrationStatus status) {
        if (status.m_state == DiscoveryRegistrationState::kEstablished) {
          throw std::runtime_error{"callback failed"};
        }
      })};
  check(registration.has_value(), "registration admission failed");
  waitFor([&] {
    return registration->status().m_state ==
           DiscoveryRegistrationState::kStopped;
  });
  waitFor([&] { return groups->freed == 1; });
  check(!groups->wrongFree, "callback failure cleanup was not worker-owned");
  check(registration->stop().has_value(), "callback failure stop failed");
  check((*client)->shutdown().has_value(), "shutdown failed");
}
} // namespace

int main() {
  lifecycle();
  stopOwnsOnlyOne();
  offlineAdmissionAndPreflight();
  nativeFailuresAndOverflow();
  callbackFailureFallback();
  stopDrainsCallback();
  destructionWithdraws();
  nestedClientLossDuringCreate();
  fatalBackendFailures();
  initialCallbackFailureKeepsAcceptedHandle();
}
