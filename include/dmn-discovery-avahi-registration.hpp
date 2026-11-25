/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-registration.hpp
 * @brief Private Avahi entry-group registration ownership.
 */

#ifndef DMN_DISCOVERY_AVAHI_REGISTRATION_HPP_
#define DMN_DISCOVERY_AVAHI_REGISTRATION_HPP_

#include "dmn-discovery-registration.hpp"

#include <avahi-client/publish.h>

#include <functional>
#include <memory>

namespace dmn::detail {

/** @brief Injectable native entry group operations. */
class DiscoveryAvahiRegistrationApi {
public:
  virtual ~DiscoveryAvahiRegistrationApi() = default;
  virtual AvahiEntryGroup *create(AvahiClient *, AvahiEntryGroupCallback,
                                  void *) = 0;
  virtual void free(AvahiEntryGroup *) noexcept = 0;
  virtual int add(AvahiEntryGroup *, AvahiIfIndex, AvahiProtocol,
                  AvahiPublishFlags, const char *, const char *, const char *,
                  std::uint16_t, AvahiStringList *) = 0;
  virtual int commit(AvahiEntryGroup *) = 0;
  virtual int clientError(AvahiClient *) = 0;
  virtual const char *errorString(int) = 0;
};

[[nodiscard]] std::shared_ptr<DiscoveryAvahiRegistrationApi>
makeDiscoveryAvahiRegistrationApi();

/** @brief Check Avahi-specific capabilities without invoking native calls. */
[[nodiscard]] DiscoveryResult validateDiscoveryAvahiAdvertisement(
    const DiscoveryOptions &options,
    const DiscoveryAdvertisement &advertisement);

enum class DiscoveryAvahiRegistrationEvent { kEstablished, kCollision };

/**
 * @brief Own one Avahi entry group on the state worker.
 *
 * Internal callbacks may run during start(); they must not stop or destroy
 * this object. The owner withdraws the group before releasing its client.
 */
class DiscoveryAvahiRegistration {
public:
  using Handler =
      std::function<DiscoveryResult(DiscoveryAvahiRegistrationEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;
  DiscoveryAvahiRegistration(std::shared_ptr<DiscoveryAvahiRegistrationApi> api,
                             AvahiClient *client,
                             DiscoveryAdvertisement advertisement,
                             DiscoveryOptions options, Handler handler,
                             FailureHandler failureHandler = {});
  ~DiscoveryAvahiRegistration();
  DiscoveryAvahiRegistration(const DiscoveryAvahiRegistration &) = delete;
  DiscoveryAvahiRegistration &
  operator=(const DiscoveryAvahiRegistration &) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  struct Deleter {
    void operator()(AvahiEntryGroup *) const noexcept;

    std::shared_ptr<DiscoveryAvahiRegistrationApi> m_api;
  };
  static void onGroup(AvahiEntryGroup *, AvahiEntryGroupState, void *) noexcept;
  void report(DiscoveryError error);
  void notify(DiscoveryAvahiRegistrationEvent event);
  [[nodiscard]] DiscoveryError nativeError(const char *prefix, int code) const;

  std::shared_ptr<DiscoveryAvahiRegistrationApi> m_api;
  AvahiClient *m_client;
  DiscoveryAdvertisement m_advertisement;
  DiscoveryOptions m_options;
  Handler m_handler;
  FailureHandler m_failure_handler;
  std::unique_ptr<AvahiEntryGroup, Deleter> m_group;
  std::optional<DiscoveryError> m_error;
  bool m_starting{false};
  unsigned m_callback_depth{0};
  bool m_callback_failed{false};
  bool m_callback_allocation_failed{false};
  bool m_collision{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_REGISTRATION_HPP_
