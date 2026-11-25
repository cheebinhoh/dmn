/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file real-public-test.cpp
 * @brief Exercise the public discovery API against an isolated Avahi daemon.
 */

#include <dmn-discovery.hpp>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace dmn;
using namespace std::chrono_literals;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

template <typename Predicate>
void waitUntil(Predicate predicate, const char *message) {
  const auto deadline{std::chrono::steady_clock::now() + 15s};
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  check(predicate(), message);
}

void run() {
  auto created{Dmn_DiscoveryClient::create()};
  check(created.has_value(), "client startup failed");
  auto client{std::move(*created)};
  std::atomic<unsigned> events{0};
  std::atomic<Dmn_DiscoveryBrowser *> published{nullptr};
  std::atomic<bool> reentrantRejected{false};
  Dmn_DiscoveryQuery query;
  query.service_type = "_dmntest._tcp";
  query.address_families = {Dmn_DiscoveryAddressFamily::IPv4};
  auto started{client->browse(std::move(query), [&](Dmn_DiscoveryEvent event) {
    ++events;
    if (event.kind == Dmn_DiscoveryEventKind::Reset) {
      if (auto *handle{published.load()}) {
        const auto result{handle->stop()};
        reentrantRejected =
            !result &&
            result.error().kind == Dmn_DiscoveryErrorKind::ReentrantCall;
      }
    }
  })};
  check(started.has_value(), "browse admission failed");
  auto browser{std::move(*started)};
  published = browser.get();
  Dmn_DiscoveryQuery failingQuery;
  failingQuery.service_type = "_dmntest._tcp";
  auto failingStarted{
      client->browse(std::move(failingQuery), [](Dmn_DiscoveryEvent event) {
        if (event.kind == Dmn_DiscoveryEventKind::Added) {
          throw std::runtime_error{"callback failure"};
        }
      })};
  check(failingStarted.has_value(), "failing browser admission failed");
  auto failingBrowser{std::move(*failingStarted)};
  const std::string name{"dmn-public-" + std::to_string(getpid())};
  auto advertised{client->advertise({{name, "_dmntest._tcp", "local.", {}},
                                     12345,
                                     {{'v', '=', 0, 255}, {'k', '=', 'v'}}},
                                    [](Dmn_DiscoveryRegistrationStatus) {})};
  check(advertised.has_value(), "registration admission failed");
  auto registration{std::move(*advertised)};
  waitUntil(
      [&] {
        const auto status{registration->status()};
        return status &&
               status->state == Dmn_DiscoveryRegistrationState::Established;
      },
      "registration did not establish");
  waitUntil(
      [&] {
        const auto snapshot{browser->snapshot()};
        if (!snapshot) {
          return false;
        }
        return std::any_of(
            snapshot->candidates.begin(), snapshot->candidates.end(),
            [&](const auto &candidate) {
              return candidate.key.instance == name && candidate.srv &&
                     candidate.srv->port == 12345 &&
                     candidate.txt ==
                         std::vector<Dmn_DiscoveryTxtEntry>{{'v', '=', 0, 255},
                                                            {'k', '=', 'v'}} &&
                     std::any_of(
                         candidate.addresses.begin(), candidate.addresses.end(),
                         [](const auto &address) {
                           return address.bytes ==
                                  std::vector<std::uint8_t>{192, 0, 2, 1};
                         });
            });
      },
      "public browse and resolution timed out");
  check(events > 0, "no browser events delivered");
  waitUntil(
      [&] {
        const auto snapshot{failingBrowser->snapshot()};
        return snapshot &&
               snapshot->state == Dmn_DiscoveryBrowserState::CallbackFailure &&
               snapshot->error &&
               snapshot->error->kind == Dmn_DiscoveryErrorKind::CallbackFailure;
      },
      "browser callback failure was not retained");
  check(failingBrowser->stop().has_value(), "failed browser stop failed");
  check(browser->clearCache().has_value(), "browser reset failed");
  waitUntil([&] { return reentrantRejected.load(); },
            "reentrant callback stop was not rejected");
  waitUntil(
      [&] {
        const auto snapshot{browser->snapshot()};
        return snapshot &&
               std::any_of(
                   snapshot->candidates.begin(), snapshot->candidates.end(),
                   [&](const auto &item) { return item.key.instance == name; });
      },
      "browser did not restart after reset");
  check(registration->stop().has_value(), "withdrawal failed");
  waitUntil(
      [&] {
        const auto snapshot{browser->snapshot()};
        return snapshot && std::none_of(snapshot->candidates.begin(),
                                        snapshot->candidates.end(),
                                        [&](const auto &candidate) {
                                          return candidate.key.instance == name;
                                        });
      },
      "withdrawal was not observed");
  check(browser->stop().has_value(), "browser stop failed");
  check(client->shutdown().has_value(), "client shutdown failed");
  const auto snapshot{browser->snapshot()};
  check(snapshot && snapshot->state == Dmn_DiscoveryBrowserState::Stopped,
        "terminal browser snapshot unavailable");
  const auto status{registration->status()};
  check(status && status->state == Dmn_DiscoveryRegistrationState::Stopped,
        "terminal registration status unavailable");
}

void concurrentClients() {
  std::promise<void> start;
  const auto ready{start.get_future().share()};
  std::vector<std::future<void>> clients;
  clients.reserve(4);
  try {
    for (unsigned index{0}; index < 4; ++index) {
      clients.push_back(std::async(std::launch::async, [ready] {
        ready.wait();
        for (unsigned attempt{0}; attempt < 3; ++attempt) {
          auto created{Dmn_DiscoveryClient::create()};
          check(created.has_value(), "concurrent client startup failed");
          auto client{std::move(*created)};
          Dmn_DiscoveryQuery query;
          query.service_type = "_dmntest._tcp";
          auto started{
              client->browse(std::move(query), [](Dmn_DiscoveryEvent) {})};
          check(started.has_value(), "concurrent browse admission failed");
          auto browser{std::move(*started)};
          check(browser->stop().has_value(), "concurrent browser stop failed");
          check(client->shutdown().has_value(),
                "concurrent client shutdown failed");
        }
      }));
    }
  } catch (...) {
    start.set_value();
    throw;
  }
  start.set_value();
  for (auto &client : clients) {
    client.get();
  }
}

class TestDaemon {
public:
  ~TestDaemon() { stop(); }

  void start() {
    check(m_pid == -1, "test daemon already started");
    const auto *config{std::getenv("DMN_TEST_AVAHI_CONFIG")};
    check(config != nullptr, "missing isolated daemon configuration");
    const std::string file{"--file=" + std::string{config}};
    m_pid = fork();
    check(m_pid >= 0, "test daemon fork failed");
    if (m_pid == 0) {
      execlp("avahi-daemon", "avahi-daemon", "--no-drop-root", "--no-chroot",
             "--no-rlimits", file.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
  }

  void stop() noexcept {
    if (m_pid > 0) {
      static_cast<void>(kill(m_pid, SIGTERM));
      int status;
      while (waitpid(m_pid, &status, 0) == -1 && errno == EINTR) {
      }
      m_pid = -1;
    }
  }

private:
  pid_t m_pid{-1};
};

void recovery() {
  TestDaemon daemon;
  auto created{Dmn_DiscoveryClient::create()};
  check(created.has_value(), "client startup failed");
  auto client{std::move(*created)};
  Dmn_DiscoveryQuery query;
  query.service_type = "_dmntest._tcp";
  auto started{client->browse(std::move(query), [](Dmn_DiscoveryEvent) {})};
  check(started.has_value(), "browse admission failed");
  auto browser{std::move(*started)};
  const std::string name{"dmn-recovery-" + std::to_string(getpid())};
  auto advertised{
      client->advertise({{name, "_dmntest._tcp", "local.", {}}, 12345, {{'r'}}},
                        [](Dmn_DiscoveryRegistrationStatus) {})};
  check(advertised.has_value(), "registration admission failed");
  auto registration{std::move(*advertised)};
  auto unavailable = [&] {
    auto snapshot{browser->snapshot()};
    auto status{registration->status()};
    return snapshot &&
           snapshot->state == Dmn_DiscoveryBrowserState::BackendUnavailable &&
           status &&
           status->state == Dmn_DiscoveryRegistrationState::Unavailable;
  };
  auto established = [&] {
    auto snapshot{browser->snapshot()};
    auto status{registration->status()};
    return snapshot && status &&
           status->state == Dmn_DiscoveryRegistrationState::Established &&
           std::any_of(snapshot->candidates.begin(), snapshot->candidates.end(),
                       [&](const auto &candidate) {
                         return candidate.key.instance == name;
                       });
  };
  waitUntil(unavailable, "initial daemon absence not reported");
  daemon.start();
  waitUntil(established, "initial daemon recovery failed");
  auto before{browser->snapshot()};
  check(before.has_value(), "initial browser snapshot failed");
  daemon.stop();
  waitUntil(unavailable, "daemon loss not reported");
  auto lost{browser->snapshot()};
  check(lost && lost->candidates.empty() && lost->epoch > before->epoch,
        "daemon loss did not invalidate the browser view");
  daemon.start();
  waitUntil(established, "daemon restart did not restore discovery");
  check(registration->stop().has_value(), "recovered registration stop failed");
  check(browser->stop().has_value(), "recovered browser stop failed");
  check(client->shutdown().has_value(), "recovered client shutdown failed");
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view{argv[1]} == "--recovery") {
      recovery();
    } else {
      run();
      concurrentClients();
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
