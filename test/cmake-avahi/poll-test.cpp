/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file poll-test.cpp
 * @brief Controlled Avahi poll and client lifetime tests.
 */

#include "dmn-discovery-avahi-browser.hpp"
#include "dmn-discovery-avahi.hpp"

#include <avahi-common/error.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
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

class FakeAvahi final : public DiscoveryAvahiApi {
public:
  AvahiSimplePoll *newPoll() override { return &m_poll; }
  void freePoll(AvahiSimplePoll *) noexcept override {
    m_destroyed.push_back("poll");
  }
  const AvahiPoll *pollApi(AvahiSimplePoll *) override { return &m_api; }
  int iterate(AvahiSimplePoll *, int timeoutMs) override {
    std::unique_lock lock{m_mutex};
    m_last_timeout = timeoutMs;
    if (timeoutMs > 0) {
      m_retry_waiting = true;
      m_changed.notify_all();
    }
    if (timeoutMs != 0 && !m_observe_timeout) {
      m_changed.wait(lock,
                     [&] { return m_woken || m_fail_poll || m_disconnect; });
    }
    m_woken = false;
    if (m_disconnect) {
      m_disconnect = false;
      const auto callback{m_callback};
      const auto context{m_context};
      lock.unlock();
      callback(&m_client, AVAHI_CLIENT_FAILURE, context);
      return 0;
    }
    return m_fail_poll ? -1 : 0;
  }
  void wake(AvahiSimplePoll *) noexcept override {
    std::lock_guard lock{m_mutex};
    m_woken = true;
    m_changed.notify_one();
  }
  AvahiClient *newClient(const AvahiPoll *, AvahiClientCallback callback,
                         void *context, int *error) override {
    if (m_fail_client) {
      *error = m_creation_error;
      return nullptr;
    }
    ++m_created;
    m_callback = callback;
    m_context = context;
    callback(&m_client, m_initial_state, context);
    return &m_client;
  }
  void freeClient(AvahiClient *) noexcept override {
    m_destroyed.push_back("client");
  }
  int clientError(AvahiClient *) override { return m_client_error; }
  const char *errorString(int) override { return "fake native error"; }

  void failPoll() {
    std::lock_guard lock{m_mutex};
    m_fail_poll = true;
    m_changed.notify_one();
  }

  void disconnect() {
    std::lock_guard lock{m_mutex};
    m_disconnect = true;
    m_changed.notify_one();
  }

  bool waitForRetryPoll() {
    std::unique_lock lock{m_mutex};
    return m_changed.wait_for(lock, std::chrono::seconds{2},
                              [&] { return m_retry_waiting; });
  }

  bool m_fail_client{false};
  int m_creation_error{17};
  AvahiClientState m_initial_state{AVAHI_CLIENT_CONNECTING};
  int m_client_error{17};
  unsigned m_created{0};
  bool m_observe_timeout{false};
  int m_last_timeout{0};
  std::vector<std::string> m_destroyed;

private:
  AvahiSimplePoll m_poll{};
  AvahiClient m_client{};
  AvahiPoll m_api{};
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_woken{false};
  bool m_fail_poll{false};
  bool m_disconnect{false};
  bool m_retry_waiting{false};
  AvahiClientCallback m_callback{nullptr};
  void *m_context{nullptr};
};

class PollBrowserApi final : public DiscoveryAvahiBrowserApi {
public:
  explicit PollBrowserApi(std::shared_ptr<FakeAvahi> api) : m_api{api} {}
  AvahiServiceBrowser *create(AvahiClient *client, AvahiIfIndex, AvahiProtocol,
                              const char *, const char *, AvahiLookupFlags,
                              AvahiServiceBrowserCallback, void *) override {
    check(client != nullptr, "immediate callback did not provide client");
    return &m_browser;
  }
  void free(AvahiServiceBrowser *) noexcept override {
    m_api->m_destroyed.push_back("browser");
  }
  int clientError(AvahiClient *) override { return 17; }
  const char *errorString(int) override { return "fake native error"; }

private:
  std::shared_ptr<FakeAvahi> m_api;
  AvahiServiceBrowser m_browser{};
};

void immediateCallbackAndStop() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  bool connecting{false};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, [&](DiscoveryAvahiEvent event) {
        check(worker.inWorkerContext(), "callback not on state worker");
        connecting = event.m_state == AVAHI_CLIENT_CONNECTING && event.m_client;
      })};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  check(connecting, "immediate connecting callback missing");
  check(worker.execute([]() -> DiscoveryResult { return {}; }).has_value(),
        "command did not wake poll");
  check(worker.shutdown().has_value(), "poll shutdown failed");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "native teardown order incorrect");
  check(worker.shutdown().has_value(), "repeated shutdown failed");
}

void clientFailureRollsBack() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  api->m_fail_client = true;
  auto poller{std::make_shared<DiscoveryAvahiPoller>(api, nullptr)};
  auto result{worker.attachPoller(poller)};
  check(!result && result.error().m_message.find("fake native error") !=
                       std::string::npos,
        "client error was not reported");
  check(api->m_destroyed == std::vector<std::string>{"poll"},
        "failed client leaked poll");
  check(worker.shutdown().has_value(), "worker did not survive failed startup");
}

void throwingImmediateCallbackRollsBack() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, [](DiscoveryAvahiEvent) { throw std::runtime_error{"callback"}; })};
  check(!worker.attachPoller(poller), "callback failure was not reported");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "callback failure leaked native resources");
  check(worker.shutdown().has_value(),
        "worker did not survive callback failure");
}

void allocationFailureRollsBack() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, [](DiscoveryAvahiEvent) { throw std::bad_alloc{}; })};
  auto result{worker.attachPoller(poller)};
  check(!result && result.error().m_kind == DiscoveryErrorKind::kResourceLimit,
        "callback allocation failure was misclassified");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "allocation failure leaked native resources");
  check(worker.shutdown().has_value(),
        "worker did not survive allocation failure");
}

void nativeFailureRollsBack() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  api->m_initial_state = AVAHI_CLIENT_FAILURE;
  int observedError{0};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, [&](DiscoveryAvahiEvent event) { observedError = event.m_error; })};
  const auto result{worker.attachPoller(poller)};
  check(!result && result.error().m_message.find("fake native error") !=
                       std::string::npos,
        "native failure was not reported");
  check(observedError == 17, "native callback error missing");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "native failure leaked resources");
  check(worker.shutdown().has_value(), "worker did not survive native failure");
}

void pollFailureStopsWorker() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(api, nullptr)};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  api->failPoll();
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{2}};
  while (std::chrono::steady_clock::now() < deadline) {
    auto result{worker.submit([]() -> DiscoveryResult { return {}; })};
    if (!result) {
      break;
    }

    static_cast<void>(result->wait_for(std::chrono::milliseconds{10}));
  }
  const auto result{worker.shutdown()};
  check(!result && result.error().m_message == "Avahi poll failed",
        "poll failure was not reported");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "poll failure leaked native resources");
}

void disconnectRetriesWithBoundedBackoff() {
  auto api{std::make_shared<FakeAvahi>()};
  api->m_client_error = AVAHI_ERR_DISCONNECTED;
  int seconds{0};
  unsigned failures{0};
  DiscoveryAvahiPoller poller{api,
                              [&](DiscoveryAvahiEvent event) {
                                if (event.m_state == AVAHI_CLIENT_FAILURE) {
                                  ++failures;
                                }
                              },
                              [&] {
                                return std::chrono::steady_clock::time_point{} +
                                       std::chrono::seconds{seconds};
                              }};
  check(poller.start().has_value(), "poll startup failed");
  api->disconnect();
  check(poller.wait(false).has_value(), "disconnect stopped the poller");
  check(failures == 1 && api->m_created == 1 &&
            api->m_destroyed == std::vector<std::string>{"client"},
        "disconnect did not report and release the old client");
  seconds = 1;
  check(poller.wait(false).has_value(), "first retry stopped the poller");
  check(api->m_created == 2, "first retry did not recreate client");
  api->disconnect();
  check(poller.wait(false).has_value(), "second disconnect stopped the poller");
  seconds = 2;
  check(poller.wait(false).has_value(), "backoff wait stopped the poller");
  check(api->m_created == 2, "second retry ran before its backoff");
  api->m_initial_state = AVAHI_CLIENT_S_RUNNING;
  seconds = 3;
  check(poller.wait(false).has_value(), "second retry stopped the poller");
  check(api->m_created == 3 && failures == 2,
        "second retry did not respect backoff");
  api->disconnect();
  check(poller.wait(false).has_value(), "third disconnect stopped the poller");
  seconds = 4;
  check(poller.wait(false).has_value() && api->m_created == 4,
        "running client did not reset the retry delay");
  check(poller.stop().has_value(), "retrying poller did not stop");
}

void retryDelayIsCapped() {
  auto api{std::make_shared<FakeAvahi>()};
  api->m_client_error = AVAHI_ERR_DISCONNECTED;
  int seconds{0};
  DiscoveryAvahiPoller poller{api, nullptr, [&] {
                                return std::chrono::steady_clock::time_point{} +
                                       std::chrono::seconds{seconds};
                              }};
  check(poller.start().has_value(), "poll startup failed");
  for (const int delay : {1, 2, 4, 8, 16, 30, 30}) {
    api->disconnect();
    check(poller.wait(false).has_value(), "disconnect stopped the poller");
    const auto created{api->m_created};
    seconds += delay - 1;
    check(poller.wait(false).has_value() && api->m_created == created,
          "retry ran before the expected delay");
    ++seconds;
    check(poller.wait(false).has_value() && api->m_created == created + 1,
          "retry did not run at the expected delay");
  }
  check(poller.stop().has_value(), "poller cleanup failed");
}

void failedCreationRetries() {
  auto api{std::make_shared<FakeAvahi>()};
  api->m_fail_client = true;
  api->m_creation_error = AVAHI_ERR_NO_DAEMON;
  int seconds{0};
  int reported{0};
  bool clientAbsent{false};
  DiscoveryAvahiPoller poller{api,
                              [&](DiscoveryAvahiEvent event) {
                                reported = event.m_error;
                                clientAbsent = event.m_client == nullptr;
                              },
                              [&] {
                                return std::chrono::steady_clock::time_point{} +
                                       std::chrono::seconds{seconds};
                              }};
  check(poller.start().has_value(), "recoverable creation failure was fatal");
  check(reported == AVAHI_ERR_NO_DAEMON && clientAbsent,
        "creation failure was not reported");
  api->m_fail_client = false;
  seconds = 1;
  check(poller.wait(false).has_value() && api->m_created == 1,
        "failed client creation was not retried");
  check(poller.stop().has_value(), "poller cleanup failed");
}

void shutdownInterruptsBackoff() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  api->m_client_error = AVAHI_ERR_DISCONNECTED;
  auto poller{std::make_shared<DiscoveryAvahiPoller>(api, nullptr)};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  api->disconnect();
  check(api->waitForRetryPoll(), "retry poll did not start");
  check(worker.shutdown().has_value(), "shutdown failed during backoff");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "shutdown during backoff leaked native resources");
}

void browserIsFreedBeforeDisconnectedClient() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  api->m_initial_state = AVAHI_CLIENT_S_RUNNING;
  api->m_client_error = AVAHI_ERR_DISCONNECTED;
  auto browserApi{std::make_shared<PollBrowserApi>(api)};
  std::unique_ptr<DiscoveryAvahiServiceBrowser> browser;
  std::promise<void> invalidated;
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, [&](DiscoveryAvahiEvent event) {
        if (event.m_state == AVAHI_CLIENT_S_RUNNING) {
          browser = std::make_unique<DiscoveryAvahiServiceBrowser>(
              browserApi, event.m_client,
              DiscoveryAvahiQuery{"_ipp._tcp", "local.", std::nullopt},
              nullptr);
          check(browser->start().has_value(), "immediate browser start failed");
        } else if (event.m_state == AVAHI_CLIENT_FAILURE) {
          browser.reset();
          invalidated.set_value();
        }
      })};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  auto invalidatedFuture{invalidated.get_future()};
  api->disconnect();
  check(invalidatedFuture.wait_for(std::chrono::seconds{2}) ==
            std::future_status::ready,
        "disconnect did not invalidate browser");
  check(worker.shutdown().has_value(), "worker shutdown failed");
  check(api->m_destroyed ==
            std::vector<std::string>{"browser", "client", "poll"},
        "browser/client teardown order incorrect");
}

void browserIsFreedOnNormalShutdown() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  api->m_initial_state = AVAHI_CLIENT_S_RUNNING;
  auto browserApi{std::make_shared<PollBrowserApi>(api)};
  std::unique_ptr<DiscoveryAvahiServiceBrowser> browser;
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api,
      [&](DiscoveryAvahiEvent event) {
        if (event.m_state == AVAHI_CLIENT_S_RUNNING) {
          browser = std::make_unique<DiscoveryAvahiServiceBrowser>(
              browserApi, event.m_client,
              DiscoveryAvahiQuery{"_ipp._tcp", "local.", std::nullopt},
              nullptr);
          check(browser->start().has_value(), "immediate browser start failed");
        }
      },
      std::chrono::steady_clock::now,
      [&]() -> DiscoveryResult {
        browser.reset();
        return {};
      })};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  check(worker.shutdown().has_value(), "worker shutdown failed");
  check(api->m_destroyed ==
            std::vector<std::string>{"browser", "client", "poll"},
        "normal shutdown destroyed client before browser");
}

void cleanupErrorsAreReported() {
  DiscoveryCommandWorker worker{4, 1};
  auto api{std::make_shared<FakeAvahi>()};
  auto poller{std::make_shared<DiscoveryAvahiPoller>(
      api, nullptr, std::chrono::steady_clock::now, []() -> DiscoveryResult {
        return std::unexpected{
            DiscoveryError{DiscoveryErrorKind::kBackend, "cleanup failed"}};
      })};
  check(worker.attachPoller(poller).has_value(), "poll startup failed");
  auto result{worker.shutdown()};
  check(!result && result.error().m_message == "cleanup failed",
        "cleanup failure was hidden");
  check(api->m_destroyed == std::vector<std::string>{"client", "poll"},
        "cleanup failure leaked poll resources");
}

void drivesTimersWithoutBlockingPastTheirDeadline() {
  auto api{std::make_shared<FakeAvahi>()};
  api->m_observe_timeout = true;
  using TimePoint = std::chrono::steady_clock::time_point;
  auto now{TimePoint{}};
  std::optional<TimePoint> due{now + std::chrono::milliseconds{1501}};
  unsigned expired{0};
  DiscoveryAvahiPoller poller{
      api,
      nullptr,
      [&] { return now; },
      {},
      [&]() -> std::expected<DiscoveryAvahiPoller::Deadline, DiscoveryError> {
        if (due && *due <= now) {
          ++expired;
          due.reset();
        }
        return due;
      }};
  check(poller.start().has_value(), "poll startup failed");
  check(poller.wait(true).has_value() && api->m_last_timeout == 1501,
        "timer did not bound poll timeout");
  now += std::chrono::milliseconds{1501};
  check(poller.wait(false).has_value() && expired == 1,
        "due timer was not driven");
  due = TimePoint::max();
  now = TimePoint{} - std::chrono::seconds{1};
  check(poller.wait(true).has_value() &&
            api->m_last_timeout == std::numeric_limits<int>::max(),
        "distant timer overflowed poll timeout");
  check(poller.stop().has_value(), "timer poller cleanup failed");
}

void reconnectAndOperationTimersSharePollTimeout() {
  auto api{std::make_shared<FakeAvahi>()};
  api->m_observe_timeout = true;
  api->m_client_error = AVAHI_ERR_DISCONNECTED;
  using TimePoint = std::chrono::steady_clock::time_point;
  auto now{TimePoint{}};
  std::optional<TimePoint> due{now + std::chrono::microseconds{500001}};
  DiscoveryAvahiPoller poller{
      api,
      nullptr,
      [&] { return now; },
      {},
      [&]() -> std::expected<DiscoveryAvahiPoller::Deadline, DiscoveryError> {
        return due;
      }};
  check(poller.start().has_value(), "poll startup failed");
  api->disconnect();
  check(poller.wait(false).has_value(), "disconnect stopped the poller");
  check(poller.wait(true).has_value() && api->m_last_timeout == 501,
        "operation timer did not preempt reconnect or round up");
  due = now + std::chrono::seconds{3};
  check(poller.wait(true).has_value() && api->m_last_timeout == 1000,
        "reconnect timer did not preempt operation timer");
  check(poller.stop().has_value(), "timer poller cleanup failed");
}

void timerDriveFailureIsReported() {
  auto api{std::make_shared<FakeAvahi>()};
  DiscoveryAvahiPoller poller{
      api,
      nullptr,
      std::chrono::steady_clock::now,
      {},
      []() -> std::expected<DiscoveryAvahiPoller::Deadline, DiscoveryError> {
        return std::unexpected{
            DiscoveryError{DiscoveryErrorKind::kBackend, "timer failed"}};
      }};
  check(poller.start().has_value(), "poll startup failed");
  auto result{poller.wait(false)};
  check(!result && result.error().m_message == "timer failed",
        "timer failure was hidden");
  check(poller.stop().has_value(), "timer poller cleanup failed");
}

void nativeWakeBeforePollIsNotLost() {
  auto api{makeDiscoveryAvahiApi()};
  auto *poll{api->newPoll()};
  check(poll != nullptr, "native poll creation failed");
  api->wake(poll);
  auto pending{
      std::async(std::launch::async, [&] { return api->iterate(poll, 2000); })};
  const bool prompt{pending.wait_for(std::chrono::milliseconds{500}) ==
                    std::future_status::ready};
  const auto result{pending.get()};
  auto blocked{
      std::async(std::launch::async, [&] { return api->iterate(poll, 2000); })};
  const bool wasBlocked{blocked.wait_for(std::chrono::milliseconds{20}) ==
                        std::future_status::timeout};
  api->wake(poll);
  const bool woken{blocked.wait_for(std::chrono::milliseconds{500}) ==
                   std::future_status::ready};
  const auto blockedResult{blocked.get()};
  api->freePoll(poll);
  check(prompt && result == 0, "native poll lost an earlier wakeup");
  check(wasBlocked && woken && blockedResult == 0,
        "native poll did not wake while blocked");
}

void nativeWakeFailureInterruptsPoll() {
  auto api{makeDiscoveryAvahiApi(
      [](int, const void *, std::size_t) -> std::ptrdiff_t {
        errno = EIO;
        return -1;
      })};
  auto *poll{api->newPoll()};
  check(poll != nullptr, "native poll creation failed");
  auto blocked{
      std::async(std::launch::async, [&] { return api->iterate(poll, -1); })};
  const bool wasBlocked{blocked.wait_for(std::chrono::milliseconds{20}) ==
                        std::future_status::timeout};
  api->wake(poll);
  const bool prompt{blocked.wait_for(std::chrono::seconds{2}) ==
                    std::future_status::ready};
  const auto result{blocked.get()};
  api->freePoll(poll);
  check(wasBlocked && prompt && result < 0,
        "failed wake did not terminate native poll");
}
} // namespace

int main() {
  immediateCallbackAndStop();
  clientFailureRollsBack();
  throwingImmediateCallbackRollsBack();
  allocationFailureRollsBack();
  nativeFailureRollsBack();
  pollFailureStopsWorker();
  disconnectRetriesWithBoundedBackoff();
  retryDelayIsCapped();
  failedCreationRetries();
  shutdownInterruptsBackoff();
  browserIsFreedBeforeDisconnectedClient();
  browserIsFreedOnNormalShutdown();
  cleanupErrorsAreReported();
  drivesTimersWithoutBlockingPastTheirDeadline();
  reconnectAndOperationTimersSharePollTimeout();
  timerDriveFailureIsReported();
  nativeWakeBeforePollIsNotLost();
  nativeWakeFailureInterruptsPoll();
}
