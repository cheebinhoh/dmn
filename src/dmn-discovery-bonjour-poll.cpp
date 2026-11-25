/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-poll.cpp
 * @brief Private Bonjour reference polling and lifetime management.
 */

#include "dmn-discovery-bonjour-poll.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace dmn::detail {

namespace {
DiscoveryResult systemError(const char *operation, int error) {
  return std::unexpected{DiscoveryError{
      DiscoveryErrorKind::kBackend,
      std::string{operation} + ": " + std::strerror(error), "posix", error}};
}

bool configureFd(int fd) {
  const int status{fcntl(fd, F_GETFL)};
  const int descriptor{fcntl(fd, F_GETFD)};

  return status != -1 && descriptor != -1 &&
         fcntl(fd, F_SETFL, status | O_NONBLOCK) != -1 &&
         fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) != -1;
}
} // namespace

struct DiscoveryBonjourPoller::Entry {
  ~Entry() {
    if (m_ref) {
      m_api->deallocate(m_ref);
    }
  }

  std::shared_ptr<DiscoveryBonjourApi> m_api;
  DiscoveryBonjourRef *m_ref;
  int m_fd;
  ErrorHandler m_on_error;
  bool m_active{true};
};

DiscoveryBonjourPoller::DiscoveryBonjourPoller(
    std::shared_ptr<DiscoveryBonjourApi> api, std::size_t maxReferences,
    Clock clock, Drive drive)
    : m_api{std::move(api)}, m_max_references{maxReferences},
      m_clock{std::move(clock)}, m_drive{std::move(drive)} {}

DiscoveryBonjourPoller::~DiscoveryBonjourPoller() {
  const auto result{stop()};
  (void)result;
}

DiscoveryResult DiscoveryBonjourPoller::start() {
  if (!m_api || !m_clock || m_max_references == 0 || m_wake_read != -1) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour poller state"}};
  }

  int fds[2]{-1, -1};
  if (pipe(fds) != 0) {
    return systemError("Bonjour wake pipe", errno);
  }

  if (!configureFd(fds[0]) || !configureFd(fds[1])) {
    const int error{errno};
    close(fds[0]);
    close(fds[1]);

    return systemError("Bonjour wake pipe configuration", error);
  }

  std::lock_guard lock{m_wake_mutex};
  m_wake_read = fds[0];
  m_wake_write = fds[1];
  m_wake_error = 0;

  return {};
}

DiscoveryResult DiscoveryBonjourPoller::adopt(DiscoveryBonjourRef *ref,
                                              ErrorHandler onError) {
  if (!ref || m_wake_read == -1) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour reference"}};
  }

  const auto matches{[ref](const auto &entry) { return entry->m_ref == ref; }};
  if (std::ranges::any_of(m_entries, matches) ||
      std::ranges::any_of(m_snapshot, matches)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "Bonjour reference already owned"}};
  }

  const auto retired{std::ranges::count_if(
      m_snapshot, [](const auto &entry) { return !entry->m_active; })};
  if (m_entries.size() >= m_max_references ||
      static_cast<std::size_t>(retired) >=
          m_max_references - m_entries.size()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "Bonjour reference limit reached"}};
  }

  const int fd{m_api->socketFd(ref)};
  if (fd < 0) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Bonjour reference has no socket"}};
  }

  auto entry{std::make_shared<Entry>(m_api, ref, fd, std::move(onError), true)};
  try {
    m_entries.push_back(entry);
  } catch (...) {
    // Ownership is transferred only after successful admission.
    entry->m_ref = nullptr;
    throw;
  }

  return {};
}

DiscoveryResult DiscoveryBonjourPoller::remove(DiscoveryBonjourRef *ref) {
  const auto found{std::ranges::find_if(
      m_entries, [ref](const auto &entry) { return entry->m_ref == ref; })};
  if (found == m_entries.end()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "Bonjour reference not owned"}};
  }

  (*found)->m_active = false;
  m_entries.erase(found);

  return {};
}

DiscoveryResult DiscoveryBonjourPoller::wait(bool block) {
  if (m_wake_read == -1 || m_waiting) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour poll state"}};
  }

  struct SnapshotGuard {
    ~SnapshotGuard() {
      m_poller.m_snapshot.clear();
      m_poller.m_waiting = false;
    }

    DiscoveryBonjourPoller &m_poller;
  };
  m_waiting = true;
  SnapshotGuard guard{*this};
  Deadline deadline;
  if (m_drive) {
    auto next{m_drive()};
    if (!next) {
      return std::unexpected{next.error()};
    }

    deadline = *next;
  }

  m_snapshot = m_entries;
  std::vector<pollfd> fds;
  fds.reserve(m_snapshot.size() + 1);
  fds.push_back({m_wake_read, POLLIN, 0});
  for (const auto &entry : m_snapshot) {
    fds.push_back({entry->m_fd, POLLIN, 0});
  }

  if (const int error{m_wake_error.exchange(0)}; error != 0) {
    return systemError("Bonjour wake", error);
  }

  int timeout{block ? 1000 : 0};
  if (block && deadline) {
    const auto now{m_clock()};
    if (*deadline <= now) {
      timeout = 0;
    } else {
      const auto maxWait{std::chrono::milliseconds{timeout}};
      if (now > std::chrono::steady_clock::time_point::max() - maxWait ||
          *deadline < now + maxWait) {
        timeout = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(*deadline - now)
                .count());
      }
    }
  }

  const int result{poll(fds.data(), fds.size(), timeout)};
  if (result < 0 && errno == EINTR) {
    return {};
  }

  if (result < 0) {
    return systemError("Bonjour poll", errno);
  }

  if (const int error{m_wake_error.exchange(0)}; error != 0) {
    return systemError("Bonjour wake", error);
  }

  if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
    return systemError("Bonjour wake pipe", EIO);
  }

  if (fds[0].revents & POLLIN) {
    char buffer[128];
    ssize_t count{0};
    do {
      count = read(m_wake_read, buffer, sizeof(buffer));
    } while (count < 0 && errno == EINTR);
    if (count == 0) {
      return systemError("Bonjour wake read", EIO);
    }

    if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return systemError("Bonjour wake read", errno);
    }
  }

  for (std::size_t index{0}; index < m_snapshot.size(); ++index) {
    const auto &entry{m_snapshot[index]};
    const auto events{fds[index + 1].revents};
    if (!entry->m_active || events == 0) {
      continue;
    }

    std::optional<DiscoveryError> error;
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
      error = DiscoveryError{DiscoveryErrorKind::kBackend,
                             "Bonjour socket failed", "posix", EIO};
      if (!(events & POLLNVAL)) {
        error->m_native_category = DiscoveryNativeErrorCategory::kUnavailable;
      }
    } else if (events & POLLIN) {
      const int nativeError{m_api->processResult(entry->m_ref)};
      if (nativeError != 0) {
        error =
            DiscoveryError{DiscoveryErrorKind::kBackend,
                           "Bonjour reference failed", "bonjour", nativeError};
      }
    }

    if (error && entry->m_active) {
      auto onError{entry->m_on_error};
      const auto removed{remove(entry->m_ref)};
      if (!removed) {
        return removed;
      }

      if (!onError) {
        return std::unexpected{std::move(*error)};
      }

      onError(std::move(*error));
    }
  }

  return {};
}

void DiscoveryBonjourPoller::wake() noexcept {
  std::lock_guard lock{m_wake_mutex};
  if (m_wake_write == -1) {
    return;
  }

  const char byte{0};
  ssize_t result{0};
  do {
    result = write(m_wake_write, &byte, 1);
  } while (result < 0 && errno == EINTR);
  if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    m_wake_error = errno;
  }
}

DiscoveryResult DiscoveryBonjourPoller::stop() {
  if (m_waiting) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "Bonjour poll is active"}};
  }

  m_entries.clear();
  std::lock_guard lock{m_wake_mutex};
  int error{0};
  if (m_wake_read != -1) {
    if (close(m_wake_read) != 0) {
      error = errno;
    }

    if (close(m_wake_write) != 0 && error == 0) {
      error = errno;
    }

    m_wake_read = -1;
    m_wake_write = -1;
  }

  if (error != 0) {
    return systemError("Bonjour wake close", error);
  }

  return {};
}

} // namespace dmn::detail
