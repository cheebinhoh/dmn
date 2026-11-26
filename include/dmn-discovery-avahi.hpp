/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi.hpp
 * @brief Private Avahi poll and client ownership seam.
 *
 * Architecture
 * ------------
 * `DiscoveryAvahiPoller` is the only owner of the Avahi simple-poll and
 * client references. `DiscoveryCommandWorker` drives it on the serialized
 * state worker. Browser, resolver, record-watch and registration objects
 * attach to that worker and translate native callbacks into the shared
 * backend-neutral record/session model. No Avahi callback invokes application
 * code directly.
 */

#ifndef DMN_DISCOVERY_AVAHI_HPP_
#define DMN_DISCOVERY_AVAHI_HPP_

#include "dmn-discovery-command.hpp"

#include <avahi-client/client.h>
#include <avahi-common/simple-watch.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace dmn::detail {

/** @brief Injectable native calls for deterministic constructor testing. */
class DiscoveryAvahiApi {
public:
  virtual ~DiscoveryAvahiApi() = default;
  virtual AvahiSimplePoll *newPoll() = 0;
  virtual void freePoll(AvahiSimplePoll *) noexcept = 0;
  virtual const AvahiPoll *pollApi(AvahiSimplePoll *) = 0;
  virtual int iterate(AvahiSimplePoll *, int timeoutMs) = 0;
  virtual void wake(AvahiSimplePoll *) noexcept = 0;
  virtual AvahiClient *newClient(const AvahiPoll *, AvahiClientCallback, void *,
                                 int *) = 0;
  virtual void freeClient(AvahiClient *) noexcept = 0;
  virtual int clientError(AvahiClient *) = 0;
  virtual const char *errorString(int) = 0;
};

/** @brief Production bindings to the Avahi C API. */
using DiscoveryAvahiWakeWrite =
    std::function<std::ptrdiff_t(int, const void *, std::size_t)>;
[[nodiscard]] std::shared_ptr<DiscoveryAvahiApi>
makeDiscoveryAvahiApi(DiscoveryAvahiWakeWrite wakeWrite = {});

/**
 * @brief Avahi client state delivered synchronously on the state worker.
 *
 * The native client is borrowed and valid only until the next failure or
 * poller stop. It may be null when client construction itself failed.
 */
struct DiscoveryAvahiEvent {
  AvahiClientState m_state;
  int m_error;
  AvahiClient *m_client;
};

/**
 * @brief Avahi poll/client lifetime owned by DiscoveryCommandWorker.
 *
 * The handler is an internal state-worker callback, never application code.
 * On failure it must invalidate objects belonging to that client before
 * returning. Cleanup must release all remaining client-owned objects before
 * returning, including when it reports an error.
 * The optional drive callback runs on the worker before each poll; it must
 * process due work and return the earliest remaining monotonic deadline.
 * The command worker serializes start, wait and stop; only wake is concurrent.
 */
class DiscoveryAvahiPoller final : public DiscoveryCommandPoller {
public:
  using Handler = std::function<void(DiscoveryAvahiEvent)>;
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  using Cleanup = std::function<DiscoveryResult()>;
  using Deadline = std::optional<std::chrono::steady_clock::time_point>;
  using Drive = std::function<std::expected<Deadline, DiscoveryError>()>;
  DiscoveryAvahiPoller(std::shared_ptr<DiscoveryAvahiApi> api, Handler handler,
                       Clock clock = std::chrono::steady_clock::now,
                       Cleanup cleanup = {}, Drive drive = {});
  DiscoveryAvahiPoller(const DiscoveryAvahiPoller &) = delete;
  DiscoveryAvahiPoller &operator=(const DiscoveryAvahiPoller &) = delete;
  DiscoveryAvahiPoller(DiscoveryAvahiPoller &&) = delete;
  DiscoveryAvahiPoller &operator=(DiscoveryAvahiPoller &&) = delete;
  [[nodiscard]] DiscoveryResult start() override;
  [[nodiscard]] DiscoveryResult wait(bool block) override;
  void wake() noexcept override;
  [[nodiscard]] DiscoveryResult stop() override;

private:
  struct PollDeleter {
    void operator()(AvahiSimplePoll *poll) const noexcept;

    std::shared_ptr<DiscoveryAvahiApi> m_api;
  };
  struct ClientDeleter {
    void operator()(AvahiClient *client) const noexcept;

    std::shared_ptr<DiscoveryAvahiApi> m_api;
  };
  static void onClient(AvahiClient *, AvahiClientState, void *) noexcept;
  [[nodiscard]] DiscoveryResult callbackStatus() const;
  [[nodiscard]] DiscoveryResult createClient();
  [[nodiscard]] DiscoveryResult handleClientFailure();

  std::shared_ptr<DiscoveryAvahiApi> m_api;
  Handler m_handler;
  Clock m_clock;
  Cleanup m_cleanup;
  Drive m_drive;
  std::unique_ptr<AvahiSimplePoll, PollDeleter> m_poll;
  std::unique_ptr<AvahiClient, ClientDeleter> m_client;
  bool m_callback_failed{false};
  bool m_callback_allocation_failed{false};
  std::optional<int> m_client_error;
  std::optional<std::chrono::steady_clock::time_point> m_retry_due;
  unsigned m_next_delay_seconds{1};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_HPP_
