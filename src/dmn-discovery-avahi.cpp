/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi.cpp
 * @brief Avahi poll and client ownership on the state worker.
 */

#include "dmn-discovery-avahi.hpp"
#include "dmn-discovery-avahi-error.hpp"

#include <avahi-common/error.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dmn::detail {
namespace {

class NativeAvahiApi final : public DiscoveryAvahiApi {
  // Avahi clears its own wake pipe during prepare(), so commands need a
  // separate pipe whose readiness survives until the actual poll.
  struct PollContext {
    ~PollContext() {
      if (m_pipe[0] >= 0)
        close(m_pipe[0]);
      if (m_pipe[1] >= 0)
        close(m_pipe[1]);
    }

    int m_pipe[2]{-1, -1};
    std::atomic<int> m_error{0};
  };

  static int pollWithWakeup(struct pollfd *fds, unsigned int count, int timeout,
                            void *userdata) noexcept {
    auto &context{*static_cast<PollContext *>(userdata)};
    try {
      std::vector<struct pollfd> all(fds, fds + count);
      all.push_back({context.m_pipe[0], POLLIN, 0});
      // Bound the wait so a failed wake write cannot strand the state worker.
      constexpr int maxWaitMs{1000};
      const auto wait{timeout < 0 ? maxWaitMs : std::min(timeout, maxWaitMs)};
      const auto result{::poll(all.data(), all.size(), wait)};
      if (context.m_error.load(std::memory_order_acquire)) {
        errno = context.m_error.load(std::memory_order_relaxed);

        return -1;
      }

      if (result <= 0) {
        return result;
      }

      for (unsigned int i{0}; i < count; ++i) {
        fds[i].revents = all[i].revents;
      }

      if (all.back().revents & POLLIN) {
        char buffer[128];
        ssize_t size;
        do {
          size = read(context.m_pipe[0], buffer, sizeof(buffer));
        } while (size > 0 || (size < 0 && errno == EINTR));
        if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          return -1;
        }
      }

      if (all.back().revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = EIO;

        return -1;
      }

      return result;
    } catch (const std::bad_alloc &) {
      errno = ENOMEM;

      return -1;
    }
  }

public:
  explicit NativeAvahiApi(DiscoveryAvahiWakeWrite wakeWrite)
      : m_wake_write{std::move(wakeWrite)} {}

  AvahiSimplePoll *newPoll() override {
    auto *poll{avahi_simple_poll_new()};
    if (!poll) {
      return nullptr;
    }

    try {
      auto context{std::make_unique<PollContext>()};
      if (pipe2(context->m_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        avahi_simple_poll_free(poll);

        return nullptr;
      }

      avahi_simple_poll_set_func(poll, pollWithWakeup, context.get());
      std::lock_guard lock{m_mutex};
      m_polls.emplace(poll, std::move(context));
    } catch (const std::bad_alloc &) {
      avahi_simple_poll_free(poll);

      return nullptr;
    }

    return poll;
  }

  void freePoll(AvahiSimplePoll *poll) noexcept override {
    std::lock_guard lock{m_mutex};
    avahi_simple_poll_free(poll);
    m_polls.erase(poll);
  }

  const AvahiPoll *pollApi(AvahiSimplePoll *poll) override {
    return avahi_simple_poll_get(poll);
  }

  int iterate(AvahiSimplePoll *poll, int timeoutMs) override {
    PollContext *context;
    {
      std::lock_guard lock{m_mutex};
      context = m_polls.at(poll).get();
    }

    if (context->m_error.load(std::memory_order_acquire)) {
      return -1;
    }

    return avahi_simple_poll_iterate(poll, timeoutMs);
  }

  void wake(AvahiSimplePoll *poll) noexcept override {
    std::lock_guard lock{m_mutex};
    auto &context{*m_polls.at(poll)};
    const char byte{'W'};
    ssize_t written;
    do {
      written = m_wake_write ? m_wake_write(context.m_pipe[1], &byte, 1)
                             : write(context.m_pipe[1], &byte, 1);
    } while (written < 0 && errno == EINTR);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      context.m_error.store(errno, std::memory_order_release);
    }
  }

  AvahiClient *newClient(const AvahiPoll *poll, AvahiClientCallback callback,
                         void *context, int *error) override {
    return avahi_client_new(poll, AVAHI_CLIENT_NO_FAIL, callback, context,
                            error);
  }

  void freeClient(AvahiClient *client) noexcept override {
    avahi_client_free(client);
  }

  int clientError(AvahiClient *client) override {
    return avahi_client_errno(client);
  }

  const char *errorString(int error) override { return avahi_strerror(error); }

private:
  DiscoveryAvahiWakeWrite m_wake_write;
  std::mutex m_mutex;
  std::map<AvahiSimplePoll *, std::unique_ptr<PollContext>> m_polls;
};

} // namespace

std::shared_ptr<DiscoveryAvahiApi>
makeDiscoveryAvahiApi(DiscoveryAvahiWakeWrite wakeWrite) {
  return std::make_shared<NativeAvahiApi>(std::move(wakeWrite));
}

void DiscoveryAvahiPoller::PollDeleter::operator()(
    AvahiSimplePoll *poll) const noexcept {
  if (poll) {
    m_api->freePoll(poll);
  }
}

void DiscoveryAvahiPoller::ClientDeleter::operator()(
    AvahiClient *client) const noexcept {
  if (client) {
    m_api->freeClient(client);
  }
}

DiscoveryAvahiPoller::DiscoveryAvahiPoller(
    std::shared_ptr<DiscoveryAvahiApi> api, Handler handler, Clock clock,
    Cleanup cleanup, Drive drive)
    : m_api{std::move(api)}, m_handler{std::move(handler)},
      m_clock{std::move(clock)}, m_cleanup{std::move(cleanup)},
      m_drive{std::move(drive)}, m_poll{nullptr, PollDeleter{m_api}},
      m_client{nullptr, ClientDeleter{m_api}} {}

DiscoveryResult DiscoveryAvahiPoller::start() {
  if (!m_api || !m_clock || m_poll || m_client) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi poller state"}};
  }

  m_callback_failed = false;
  m_callback_allocation_failed = false;
  m_client_error.reset();
  m_retry_due.reset();
  m_next_delay_seconds = 1;
  m_poll.reset(m_api->newPoll());
  if (!m_poll) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "Avahi poll allocation failed"}};
  }

  return createClient();
}

DiscoveryResult DiscoveryAvahiPoller::createClient() {
  const auto *pollApi{m_api->pollApi(m_poll.get())};
  if (!pollApi) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi poll API unavailable"}};
  }

  int error{0};
  m_client.reset(m_api->newClient(pollApi, onClient, this, &error));
  if (!m_client) {
    if (m_callback_failed) {
      return callbackStatus();
    }

    if (m_client_error) {
      return handleClientFailure();
    }

    if (error == AVAHI_ERR_DISCONNECTED || error == AVAHI_ERR_NO_DAEMON) {
      m_client_error = error;
      try {
        if (m_handler) {
          m_handler({AVAHI_CLIENT_FAILURE, error, nullptr});
        }
      } catch (const std::bad_alloc &) {
        m_callback_failed = true;
        m_callback_allocation_failed = true;
      } catch (...) {
        m_callback_failed = true;
      }

      return handleClientFailure();
    }

    return std::unexpected{discoveryAvahiError(
        "Avahi client creation failed", error, m_api->errorString(error))};
  }

  return handleClientFailure();
}

DiscoveryResult DiscoveryAvahiPoller::handleClientFailure() {
  if (m_callback_failed) {
    return callbackStatus();
  }

  if (!m_client_error) {
    return {};
  }

  if (*m_client_error != AVAHI_ERR_DISCONNECTED &&
      *m_client_error != AVAHI_ERR_NO_DAEMON) {
    return callbackStatus();
  }

  m_client.reset();
  m_client_error.reset();
  const auto now{m_clock()};
  const auto delay{std::chrono::seconds{m_next_delay_seconds}};
  m_retry_due = now > std::chrono::steady_clock::time_point::max() - delay
                    ? std::chrono::steady_clock::time_point::max()
                    : now + delay;
  m_next_delay_seconds = std::min(m_next_delay_seconds * 2, 30u);

  return {};
}

DiscoveryResult DiscoveryAvahiPoller::wait(bool block) {
  if (!m_poll) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi poll is not started"}};
  }

  if (m_retry_due && m_clock() >= *m_retry_due) {
    m_retry_due.reset();
    auto result{createClient()};
    if (!result) {
      return result;
    }
  }

  Deadline deadline{m_retry_due};
  if (m_drive) {
    auto next{m_drive()};
    if (!next) {
      return std::unexpected{next.error()};
    }

    if (*next && (!deadline || **next < *deadline)) {
      deadline = *next;
    }
  }

  int timeout{block ? -1 : 0};
  if (block && deadline) {
    const auto now{m_clock()};
    if (*deadline <= now) {
      timeout = 0;
    } else {
      const auto maxWait{
          std::chrono::milliseconds{std::numeric_limits<int>::max()}};
      if (now <= std::chrono::steady_clock::time_point::max() - maxWait &&
          *deadline >= now + maxWait) {
        timeout = std::numeric_limits<int>::max();
      } else {
        timeout = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(*deadline - now)
                .count());
      }
    }
  }

  if (m_api->iterate(m_poll.get(), timeout) != 0) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "Avahi poll failed"}};
  }

  return handleClientFailure();
}

void DiscoveryAvahiPoller::wake() noexcept {
  if (m_poll) {
    m_api->wake(m_poll.get());
  }
}

DiscoveryResult DiscoveryAvahiPoller::stop() {
  DiscoveryResult result{};
  if (m_poll && m_cleanup) {
    try {
      result = m_cleanup();
    } catch (const std::bad_alloc &) {
      result = std::unexpected{
          DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
    } catch (...) {
      result = std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                              "Avahi cleanup callback failed"}};
    }
  }

  m_client.reset();
  m_poll.reset();
  m_retry_due.reset();

  return result;
}

DiscoveryResult DiscoveryAvahiPoller::callbackStatus() const {
  if (m_callback_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi state callback failed"}};
  }

  if (m_client_error) {
    return std::unexpected{
        discoveryAvahiError("Avahi client failed", *m_client_error,
                            m_api->errorString(*m_client_error))};
  }

  return {};
}

void DiscoveryAvahiPoller::onClient(AvahiClient *client, AvahiClientState state,
                                    void *context) noexcept {
  auto *self{static_cast<DiscoveryAvahiPoller *>(context)};
  try {
    const int error{
        state == AVAHI_CLIENT_FAILURE ? self->m_api->clientError(client) : 0};
    if (state == AVAHI_CLIENT_FAILURE) {
      self->m_client_error = error;
    } else if (state == AVAHI_CLIENT_S_RUNNING) {
      self->m_next_delay_seconds = 1;
    }

    if (self->m_handler) {
      self->m_handler({state, error, client});
    }
  } catch (const std::bad_alloc &) {
    self->m_callback_failed = true;
    self->m_callback_allocation_failed = true;
  } catch (...) {
    self->m_callback_failed = true;
  }
}

} // namespace dmn::detail
