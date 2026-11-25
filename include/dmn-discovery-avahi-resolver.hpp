/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-resolver.hpp
 * @brief Private, per-family Avahi service resolution.
 */

#ifndef DMN_DISCOVERY_AVAHI_RESOLVER_HPP_
#define DMN_DISCOVERY_AVAHI_RESOLVER_HPP_

#include "dmn-discovery-avahi-mapping.hpp"

#include <functional>
#include <memory>

namespace dmn::detail {

/** @brief Injectable native resolver operations. */
class DiscoveryAvahiResolverApi {
public:
  virtual ~DiscoveryAvahiResolverApi() = default;
  virtual AvahiServiceResolver *
  create(AvahiClient *, AvahiIfIndex, AvahiProtocol, const char *, const char *,
         const char *, AvahiProtocol, AvahiLookupFlags,
         AvahiServiceResolverCallback, void *) = 0;
  virtual void free(AvahiServiceResolver *) noexcept = 0;
  virtual int clientError(AvahiClient *) = 0;
  virtual const char *errorString(int) = 0;
};

[[nodiscard]] std::shared_ptr<DiscoveryAvahiResolverApi>
makeDiscoveryAvahiResolverApi();

/** @brief An owned resolver result; the owner checks it against current SRV. */
struct DiscoveryAvahiResolverEvent {
  std::string m_host;
  std::uint16_t m_port;
  DiscoveryAddress m_address;
  AvahiLookupResultFlags m_flags;
};

/**
 * @brief Own one address-family resolver on the Avahi state worker.
 *
 * Internal handlers may run synchronously inside start(). They must not stop
 * or destroy this object; its owner releases it before the native client.
 */
class DiscoveryAvahiServiceResolver {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryAvahiResolverEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;
  DiscoveryAvahiServiceResolver(std::shared_ptr<DiscoveryAvahiResolverApi> api,
                                AvahiClient *client, DiscoveryServiceKey key,
                                DiscoveryContribution contribution,
                                DiscoveryAddressFamily family, Handler handler,
                                FailureHandler failureHandler = {});
  ~DiscoveryAvahiServiceResolver();
  DiscoveryAvahiServiceResolver(const DiscoveryAvahiServiceResolver &) = delete;
  DiscoveryAvahiServiceResolver &
  operator=(const DiscoveryAvahiServiceResolver &) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  struct Deleter {
    void operator()(AvahiServiceResolver *) const noexcept;

    std::shared_ptr<DiscoveryAvahiResolverApi> m_api;
  };
  static void onResolved(AvahiServiceResolver *, AvahiIfIndex, AvahiProtocol,
                         AvahiResolverEvent, const char *, const char *,
                         const char *, const char *, const AvahiAddress *,
                         std::uint16_t, AvahiStringList *,
                         AvahiLookupResultFlags, void *) noexcept;
  void report(DiscoveryError error);
  [[nodiscard]] DiscoveryError nativeError(const char *prefix) const;

  std::shared_ptr<DiscoveryAvahiResolverApi> m_api;
  AvahiClient *m_client;
  DiscoveryServiceKey m_key;
  DiscoveryContribution m_contribution;
  DiscoveryAddressFamily m_family;
  Handler m_handler;
  FailureHandler m_failure_handler;
  std::unique_ptr<AvahiServiceResolver, Deleter> m_resolver;
  DiscoveryAvahiResolverArguments m_arguments{};
  std::optional<DiscoveryError> m_error;
  bool m_starting{false};
  unsigned m_callback_depth{0};
  bool m_callback_failed{false};
  bool m_callback_allocation_failed{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_RESOLVER_HPP_
