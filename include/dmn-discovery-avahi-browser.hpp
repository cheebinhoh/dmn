/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-browser.hpp
 * @brief Private service-browser ownership on the Avahi worker.
 */

#ifndef DMN_DISCOVERY_AVAHI_BROWSER_HPP_
#define DMN_DISCOVERY_AVAHI_BROWSER_HPP_

#include "dmn-discovery-avahi-mapping.hpp"

#include <functional>
#include <memory>

namespace dmn::detail {

/** @brief Injectable service browser operations. */
class DiscoveryAvahiBrowserApi {
public:
  virtual ~DiscoveryAvahiBrowserApi() = default;
  virtual AvahiServiceBrowser *create(AvahiClient *, AvahiIfIndex,
                                      AvahiProtocol, const char *, const char *,
                                      AvahiLookupFlags,
                                      AvahiServiceBrowserCallback, void *) = 0;
  virtual void free(AvahiServiceBrowser *) noexcept = 0;
  virtual int clientError(AvahiClient *) = 0;
  virtual const char *errorString(int) = 0;
};

[[nodiscard]] std::shared_ptr<DiscoveryAvahiBrowserApi>
makeDiscoveryAvahiBrowserApi();

/** @brief Explicit service query; a missing interface means all interfaces. */
struct DiscoveryAvahiQuery {
  std::string m_type;
  std::string m_domain;
  std::optional<std::uint32_t> m_interface;
};

/**
 * @brief Own a single native browser on the Avahi state worker.
 *
 * Handlers are internal state-worker functions, never application callbacks.
 * A handler may run synchronously inside start(). It must not destroy or stop
 * this browser; the owner handles a reported failure after the callback.
 * The owner must destroy the browser before its AvahiClient.
 */
class DiscoveryAvahiServiceBrowser {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryAvahiBrowseEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;

  DiscoveryAvahiServiceBrowser(std::shared_ptr<DiscoveryAvahiBrowserApi> api,
                               AvahiClient *client, DiscoveryAvahiQuery query,
                               Handler handler,
                               FailureHandler failureHandler = {});
  ~DiscoveryAvahiServiceBrowser();
  DiscoveryAvahiServiceBrowser(const DiscoveryAvahiServiceBrowser &) = delete;
  DiscoveryAvahiServiceBrowser &
  operator=(const DiscoveryAvahiServiceBrowser &) = delete;
  DiscoveryAvahiServiceBrowser(DiscoveryAvahiServiceBrowser &&) = delete;
  DiscoveryAvahiServiceBrowser &
  operator=(DiscoveryAvahiServiceBrowser &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  struct Deleter {
    void operator()(AvahiServiceBrowser *browser) const noexcept;

    std::shared_ptr<DiscoveryAvahiBrowserApi> m_api;
  };
  static void onBrowse(AvahiServiceBrowser *, AvahiIfIndex, AvahiProtocol,
                       AvahiBrowserEvent, const char *, const char *,
                       const char *, AvahiLookupResultFlags, void *) noexcept;
  void report(DiscoveryError error);
  [[nodiscard]] DiscoveryError nativeError(const char *prefix) const;

  std::shared_ptr<DiscoveryAvahiBrowserApi> m_api;
  AvahiClient *m_client;
  DiscoveryAvahiQuery m_query;
  Handler m_handler;
  FailureHandler m_failure_handler;
  std::unique_ptr<AvahiServiceBrowser, Deleter> m_browser;
  std::optional<DiscoveryError> m_error;
  bool m_starting{false};
  unsigned m_callback_depth{0};
  bool m_callback_failed{false};
  bool m_callback_allocation_failed{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_BROWSER_HPP_
