/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-sessions.hpp
 * @brief Private Bonjour browser and session integration.
 *
 * This is the macOS backend composition root. It connects the shared
 * `DiscoverySessionClient` lifecycle and callback dispatcher to one
 * `DiscoveryBonjourPoller`, then owns browser, registration and contribution
 * objects on that worker. It produces the same backend-neutral events as the
 * Avahi composition root; it does not create DMesgNet sessions.
 */

#ifndef DMN_DISCOVERY_BONJOUR_SESSIONS_HPP_
#define DMN_DISCOVERY_BONJOUR_SESSIONS_HPP_

#include "dmn-discovery-bonjour-browser.hpp"
#include "dmn-discovery-bonjour-contribution.hpp"
#include "dmn-discovery-bonjour-registration.hpp"
#include "dmn-discovery-session.hpp"

#include <array>
#include <map>
#include <memory>

namespace dmn::detail {

/**
 * @brief Worker-owned Bonjour browser and registration integration.
 *
 * The owner must coordinate destruction with calls.
 */
class DiscoveryBonjourSessions {
public:
  /**
   * @brief Start the private session worker and its bounded native poller.
   *
   * @param options Per-client discovery limits.
   * @param clock Monotonic clock for timers and retries.
   * @return The owned session integration or a typed startup error.
   */
  [[nodiscard]] static std::expected<std::shared_ptr<DiscoveryBonjourSessions>,
                                     DiscoveryError>
  create(DiscoveryOptions options,
         DiscoveryBonjourPoller::Clock clock = std::chrono::steady_clock::now);
  ~DiscoveryBonjourSessions();
  DiscoveryBonjourSessions(const DiscoveryBonjourSessions &) = delete;
  DiscoveryBonjourSessions &
  operator=(const DiscoveryBonjourSessions &) = delete;
  DiscoveryBonjourSessions(DiscoveryBonjourSessions &&) = delete;
  DiscoveryBonjourSessions &operator=(DiscoveryBonjourSessions &&) = delete;

  /**
   * @brief Admit an interface-scoped browser on the session worker.
   *
   * @param query Owned service type, domain and optional interface.
   * @param handler Callback invoked by the session dispatcher.
   * @param addressFamilies Enabled IPv4 and IPv6 lookups, respectively.
   * @return Browser handle or a typed admission error.
   */
  [[nodiscard]] std::expected<DiscoverySessionHandle, DiscoveryError>
  browse(DiscoveryBonjourQuery query, DiscoverySessionClient::Handler handler,
         std::array<bool, 2> addressFamilies = {true, true});

  /**
   * @brief Admit an explicitly requested local service registration.
   *
   * @param advertisement Owned service description.
   * @param handler Callback invoked by the session dispatcher.
   * @return Registration handle or a typed admission error.
   */
  [[nodiscard]] std::expected<DiscoveryRegistrationHandle, DiscoveryError>
  advertise(DiscoveryAdvertisement advertisement,
            DiscoverySessionClient::RegistrationHandler handler);

  /** @brief Stop browser sessions and drain the worker and dispatcher. */
  [[nodiscard]] DiscoveryResult shutdown();

private:
  struct Browser;
  struct Registration;
  class DrivingPoller;

  DiscoveryBonjourSessions(DiscoveryOptions options,
                           DiscoveryBonjourPoller::Clock clock);
  [[nodiscard]] DiscoveryResult start(Browser &browser);
  void start(Registration &registration);
  [[nodiscard]] DiscoveryResult onBrowse(Browser &browser,
                                         DiscoveryBonjourBrowseEvent event);
  [[nodiscard]] DiscoveryResult cleanup(Browser &browser);
  [[nodiscard]] DiscoveryResult cancel(std::uint64_t id);
  void cancelRegistration(std::uint64_t id);
  [[nodiscard]] DiscoveryResult reset(std::uint64_t id);
  [[nodiscard]] DiscoveryBonjourPoller::DriveResult drive();
  void propagateUnavailable(const DiscoveryError &error);
  void failAll(const DiscoveryError &error);
  [[nodiscard]] DiscoveryResult cleanupAll();

  DiscoveryOptions m_options;
  DiscoveryBonjourPoller::Clock m_clock;
  DiscoverySessionClient m_client;
  std::shared_ptr<DiscoveryBonjourPoller> m_poller;
  std::map<std::uint64_t, std::unique_ptr<Browser>> m_browsers;
  std::map<std::uint64_t, std::unique_ptr<Registration>> m_registrations;
  std::uint64_t m_next_id{1};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_SESSIONS_HPP_
