/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-registration-session.hpp
 * @brief Private Avahi registration/session integration.
 *
 * This is the Linux backend composition root. It connects the shared
 * `DiscoverySessionClient` lifecycle and callback dispatcher to Avahi's
 * poller, then owns browser, resolver, record-watch and registration objects
 * on that worker. It produces the same backend-neutral events as Bonjour and
 * does not create DMesgNet sessions.
 */

#ifndef DMN_DISCOVERY_AVAHI_REGISTRATION_SESSION_HPP_
#define DMN_DISCOVERY_AVAHI_REGISTRATION_SESSION_HPP_

#include "dmn-discovery-avahi-browser.hpp"
#include "dmn-discovery-avahi-contribution.hpp"
#include "dmn-discovery-avahi-registration.hpp"
#include "dmn-discovery-avahi.hpp"
#include "dmn-discovery-session.hpp"

#include <map>

namespace dmn::detail {

/**
 * @brief Private Avahi client; all native objects live on its worker.
 *
 * This private integration does not implement the public API.
 * The owner must coordinate destruction with calls, as for the session client.
 */
class DiscoveryAvahiRegistrationSessions
    : public std::enable_shared_from_this<DiscoveryAvahiRegistrationSessions> {
public:
  [[nodiscard]] static std::expected<
      std::shared_ptr<DiscoveryAvahiRegistrationSessions>, DiscoveryError>
  create(DiscoveryOptions options, std::shared_ptr<DiscoveryAvahiApi> pollApi,
         std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
         DiscoveryAvahiPoller::Clock clock = std::chrono::steady_clock::now,
         DiscoverySessionClient::RegistrationFault fault = {});
  [[nodiscard]] static std::expected<
      std::shared_ptr<DiscoveryAvahiRegistrationSessions>, DiscoveryError>
  create(DiscoveryOptions options, std::shared_ptr<DiscoveryAvahiApi> pollApi,
         std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
         std::shared_ptr<DiscoveryAvahiBrowserApi> browserApi,
         std::shared_ptr<DiscoveryAvahiRecordApi> recordApi,
         std::shared_ptr<DiscoveryAvahiResolverApi> resolverApi,
         DiscoveryAvahiPoller::Clock clock = std::chrono::steady_clock::now,
         DiscoverySessionClient::RegistrationFault fault = {});
  ~DiscoveryAvahiRegistrationSessions();
  DiscoveryAvahiRegistrationSessions(
      const DiscoveryAvahiRegistrationSessions &) = delete;
  DiscoveryAvahiRegistrationSessions &
  operator=(const DiscoveryAvahiRegistrationSessions &) = delete;
  DiscoveryAvahiRegistrationSessions(DiscoveryAvahiRegistrationSessions &&) =
      delete;
  DiscoveryAvahiRegistrationSessions &
  operator=(DiscoveryAvahiRegistrationSessions &&) = delete;

  [[nodiscard]] std::expected<DiscoveryRegistrationHandle, DiscoveryError>
  advertise(DiscoveryAdvertisement advertisement,
            DiscoverySessionClient::RegistrationHandler handler);
  [[nodiscard]] std::expected<DiscoverySessionHandle, DiscoveryError>
  browse(DiscoveryAvahiQuery query, DiscoverySessionClient::Handler handler,
         std::array<bool, 2> addressFamilies = {true, true});
  [[nodiscard]] DiscoveryResult shutdown();

private:
  struct Entry;
  struct BrowserEntry;
  class DrivingPoller;
  DiscoveryAvahiRegistrationSessions(
      DiscoveryOptions options,
      std::shared_ptr<DiscoveryAvahiRegistrationApi> registrationApi,
      std::shared_ptr<DiscoveryAvahiBrowserApi> browserApi,
      std::shared_ptr<DiscoveryAvahiRecordApi> recordApi,
      std::shared_ptr<DiscoveryAvahiResolverApi> resolverApi,
      DiscoveryAvahiPoller::Clock clock,
      DiscoverySessionClient::RegistrationFault fault);
  void clientEvent(DiscoveryAvahiEvent event);
  void start(Entry &entry);
  void loseClient();
  void reap();
  void cancel(std::uint64_t id);
  [[nodiscard]] DiscoveryResult cancelBrowser(std::uint64_t id);
  [[nodiscard]] DiscoveryResult resetBrowser(std::uint64_t id);
  void startBrowser(BrowserEntry &entry);
  void releaseBrowserNative(BrowserEntry &entry) noexcept;
  void releaseClientNative() noexcept;
  void failAllResourceLimit();
  [[nodiscard]] DiscoveryResult cleanupBrowser(BrowserEntry &entry);
  [[nodiscard]] DiscoveryResult onBrowse(BrowserEntry &entry,
                                         DiscoveryAvahiBrowseEvent event);
  [[nodiscard]] std::expected<DiscoveryAvahiPoller::Deadline, DiscoveryError>
  driveBrowsers();
  void failBackend(const DiscoveryError &error);
  [[nodiscard]] DiscoveryResult cleanup();

  DiscoveryOptions m_options;
  std::shared_ptr<DiscoveryAvahiRegistrationApi> m_api;
  std::shared_ptr<DiscoveryAvahiBrowserApi> m_browser_api;
  std::shared_ptr<DiscoveryAvahiRecordApi> m_record_api;
  std::shared_ptr<DiscoveryAvahiResolverApi> m_resolver_api;
  DiscoverySessionClient m_client;
  std::map<std::uint64_t, std::unique_ptr<Entry>> m_entries;
  std::map<std::uint64_t, std::unique_ptr<BrowserEntry>> m_browsers;
  std::uint64_t m_next_id{1};
  AvahiClient *m_native_client{nullptr};
  bool m_starting{false};
  bool m_loss_pending{false};
  bool m_force_poll_iteration{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_REGISTRATION_SESSION_HPP_
