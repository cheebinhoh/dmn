/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-registration.hpp
 * @brief Private Bonjour service registration adapter.
 */

#ifndef DMN_DISCOVERY_BONJOUR_REGISTRATION_HPP_
#define DMN_DISCOVERY_BONJOUR_REGISTRATION_HPP_

#include "dmn-discovery-bonjour-operation.hpp"
#include "dmn-discovery-registration.hpp"

namespace dmn::detail {

/**
 * @brief Check Bonjour registration capability without native calls.
 *
 * @param options Per-client discovery limits.
 * @param advertisement Requested service and TXT data.
 * @return Success or a typed validation/capability error.
 */
[[nodiscard]] DiscoveryResult validateDiscoveryBonjourAdvertisement(
    const DiscoveryOptions &options,
    const DiscoveryAdvertisement &advertisement);

enum class DiscoveryBonjourRegistrationEvent { kEstablished, kCollision };

/**
 * @brief Own one Bonjour registration on the state worker.
 *
 * Native callbacks can run during start(). The owner must not stop or destroy
 * this adapter until its native callback stack has unwound.
 */
class DiscoveryBonjourRegistration {
public:
  using Handler =
      std::function<DiscoveryResult(DiscoveryBonjourRegistrationEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;

  DiscoveryBonjourRegistration(std::shared_ptr<DiscoveryBonjourPoller> poller,
                               DiscoveryAdvertisement advertisement,
                               DiscoveryOptions options, Handler handler,
                               FailureHandler failureHandler = {});
  ~DiscoveryBonjourRegistration();
  DiscoveryBonjourRegistration(const DiscoveryBonjourRegistration &) = delete;
  DiscoveryBonjourRegistration &
  operator=(const DiscoveryBonjourRegistration &) = delete;
  DiscoveryBonjourRegistration(DiscoveryBonjourRegistration &&) = delete;
  DiscoveryBonjourRegistration &
  operator=(DiscoveryBonjourRegistration &&) = delete;

  /** @brief Start the native registration without automatic renaming. */
  [[nodiscard]] DiscoveryResult start();

  /** @brief Copy the current native-operation failure, if any. */
  [[nodiscard]] DiscoveryResult status() const;

  /** @brief Withdraw the native reference after any callback unwinds. */
  [[nodiscard]] DiscoveryResult stop();

private:
  static void DNSSD_API onRegistration(DNSServiceRef ref, DNSServiceFlags flags,
                                       DNSServiceErrorType error,
                                       const char *name, const char *type,
                                       const char *domain,
                                       void *context) noexcept;
  [[nodiscard]] DiscoveryResult notify(DiscoveryBonjourRegistrationEvent event);

  DiscoveryBonjourOperation m_operation;
  DiscoveryAdvertisement m_advertisement;
  DiscoveryOptions m_options;
  Handler m_handler;
  bool m_collision{false};
  bool m_started{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_REGISTRATION_HPP_
