/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-poll.hpp
 * @brief Private Bonjour reference polling and lifetime management.
 *
 * This implementation is private and is not installed. Operations other than
 * wake() run on the state worker.
 */

#ifndef DMN_DISCOVERY_BONJOUR_POLL_HPP_
#define DMN_DISCOVERY_BONJOUR_POLL_HPP_

#include "dmn-discovery-command.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dmn::detail {

struct DiscoveryBonjourRef;

/** @brief Narrow seam for DNSService reference operations. */
class DiscoveryBonjourApi {
public:
  virtual ~DiscoveryBonjourApi() = default;
  virtual int socketFd(DiscoveryBonjourRef *ref) = 0;
  virtual int processResult(DiscoveryBonjourRef *ref) = 0;
  virtual void deallocate(DiscoveryBonjourRef *ref) noexcept = 0;
};

/** @brief Create the native DNSService reference adapter. */
std::shared_ptr<DiscoveryBonjourApi> makeDiscoveryBonjourApi();

/** @brief Bounded, worker-owned set of interruptible native references. */
class DiscoveryBonjourPoller final : public DiscoveryCommandPoller {
public:
  using ErrorHandler = std::function<void(DiscoveryError)>;
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;
  using DriveResult = std::expected<Deadline, DiscoveryError>;
  using Drive = std::function<DriveResult()>;

  DiscoveryBonjourPoller(std::shared_ptr<DiscoveryBonjourApi> api,
                         std::size_t maxReferences,
                         Clock clock = std::chrono::steady_clock::now,
                         Drive drive = {});
  ~DiscoveryBonjourPoller() override;
  DiscoveryBonjourPoller(const DiscoveryBonjourPoller &) = delete;
  DiscoveryBonjourPoller &operator=(const DiscoveryBonjourPoller &) = delete;
  DiscoveryBonjourPoller(DiscoveryBonjourPoller &&) = delete;
  DiscoveryBonjourPoller &operator=(DiscoveryBonjourPoller &&) = delete;

  [[nodiscard]] DiscoveryResult start() override;
  [[nodiscard]] DiscoveryResult wait(bool block) override;
  void wake() noexcept override;
  [[nodiscard]] DiscoveryResult stop() override;

  /**
   * @brief Adopt a reference only on success.
   *
   * Its error handler is called on the worker for asynchronous native errors.
   * Without a handler, errors fail the poller. References removed during
   * processing are deallocated after the native call returns.
   */
  [[nodiscard]] DiscoveryResult adopt(DiscoveryBonjourRef *ref,
                                      ErrorHandler onError);
  [[nodiscard]] DiscoveryResult remove(DiscoveryBonjourRef *ref);

private:
  struct Entry;
  std::shared_ptr<DiscoveryBonjourApi> m_api;
  std::size_t m_max_references;
  Clock m_clock;
  Drive m_drive;
  std::vector<std::shared_ptr<Entry>> m_entries;
  std::vector<std::shared_ptr<Entry>> m_snapshot;
  std::mutex m_wake_mutex;
  int m_wake_read{-1};
  int m_wake_write{-1};
  std::atomic<int> m_wake_error{0};
  bool m_waiting{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_POLL_HPP_
