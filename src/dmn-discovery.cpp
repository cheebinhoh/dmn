/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery.cpp
 * @brief Public discovery API backed by the Linux Avahi session owner.
 */

#include "dmn-discovery.hpp"
#include "dmn-discovery-avahi-registration-session.hpp"
#include "dmn-discovery-public-conversion.hpp"

#include <new>
#include <utility>

namespace dmn {
namespace {

Dmn_DiscoveryError allocationError() {
  return {Dmn_DiscoveryErrorKind::ResourceLimit, {}, {}, {}};
}

Dmn_DiscoveryResult publicResult(detail::DiscoveryResult result) {
  if (!result) {
    return std::unexpected{detail::discoveryPublicError(
        result.error(), Dmn_DiscoveryErrorKind::Internal)};
  }
  return {};
}

} // namespace

struct Dmn_DiscoveryBrowser::Impl {
  detail::DiscoverySessionHandle handle;
  detail::DiscoveryOptions options;
  bool initialized{false};
};

struct Dmn_DiscoveryRegistration::Impl {
  detail::DiscoveryRegistrationHandle handle;
  bool initialized{false};
};

struct Dmn_DiscoveryClient::Impl {
  detail::DiscoveryOptions options;
  std::shared_ptr<detail::DiscoveryAvahiRegistrationSessions> backend;
};

Dmn_DiscoveryBrowser::Dmn_DiscoveryBrowser(std::shared_ptr<Impl> impl)
    : m_impl{std::move(impl)} {}

Dmn_DiscoveryBrowser::~Dmn_DiscoveryBrowser() {
  if (m_impl->initialized) {
    static_cast<void>(stop());
  }
}

Dmn_DiscoveryValueResult<Dmn_DiscoverySnapshot>
Dmn_DiscoveryBrowser::snapshot() const try {
  return detail::discoveryPublicSnapshot(m_impl->handle.snapshot(),
                                         m_impl->options);
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryValueResult<std::uint64_t>
Dmn_DiscoveryBrowser::refresh(const Dmn_DiscoveryServiceKey &key) try {
  auto result{m_impl->handle.refresh(detail::discoveryKey(key))};
  if (!result) {
    return std::unexpected{detail::discoveryPublicError(
        result.error(), Dmn_DiscoveryErrorKind::Internal)};
  }
  return *result;
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryResult Dmn_DiscoveryBrowser::clearCache() try {
  return publicResult(m_impl->handle.clearCache());
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryResult Dmn_DiscoveryBrowser::stop() try {
  return publicResult(m_impl->handle.stop());
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryRegistration::Dmn_DiscoveryRegistration(std::shared_ptr<Impl> impl)
    : m_impl{std::move(impl)} {}

Dmn_DiscoveryRegistration::~Dmn_DiscoveryRegistration() {
  if (m_impl->initialized) {
    static_cast<void>(stop());
  }
}

Dmn_DiscoveryValueResult<Dmn_DiscoveryRegistrationStatus>
Dmn_DiscoveryRegistration::status() const try {
  return detail::discoveryPublicRegistrationStatus(m_impl->handle.status());
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryResult Dmn_DiscoveryRegistration::stop() try {
  return publicResult(m_impl->handle.stop());
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryClient::Dmn_DiscoveryClient(std::shared_ptr<Impl> impl)
    : m_impl{std::move(impl)} {}

Dmn_DiscoveryClient::~Dmn_DiscoveryClient() { static_cast<void>(shutdown()); }

Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryClient>>
Dmn_DiscoveryClient::create(Dmn_DiscoveryOptions options) try {
  auto nativeOptions{detail::discoveryOptions(options)};
  if (auto validated{detail::validateOptions(nativeOptions)}; !validated) {
    return std::unexpected{detail::discoveryPublicError(
        validated.error(), Dmn_DiscoveryErrorKind::Internal)};
  }
  auto backend{detail::DiscoveryAvahiRegistrationSessions::create(
      nativeOptions, detail::makeDiscoveryAvahiApi(),
      detail::makeDiscoveryAvahiRegistrationApi(),
      detail::makeDiscoveryAvahiBrowserApi(),
      detail::makeDiscoveryAvahiRecordApi(),
      detail::makeDiscoveryAvahiResolverApi())};
  if (!backend) {
    return std::unexpected{detail::discoveryPublicError(
        backend.error(), Dmn_DiscoveryErrorKind::BackendUnavailable)};
  }
  auto impl{std::make_shared<Impl>(Impl{nativeOptions, std::move(*backend)})};
  return std::unique_ptr<Dmn_DiscoveryClient>{
      new Dmn_DiscoveryClient{std::move(impl)}};
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryBrowser>>
Dmn_DiscoveryClient::browse(Dmn_DiscoveryQuery query,
                            Dmn_DiscoveryEventHandler handler) try {
  auto families{detail::discoveryQueryFamilies(query)};
  if (!families) {
    return std::unexpected{detail::discoveryPublicError(
        families.error(), Dmn_DiscoveryErrorKind::Internal)};
  }
  if (!handler) {
    return std::unexpected{
        Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::InvalidArgument,
                           "empty discovery handler",
                           {},
                           {}}};
  }
  auto impl{std::make_shared<Dmn_DiscoveryBrowser::Impl>()};
  impl->options = m_impl->options;
  auto browser{
      std::unique_ptr<Dmn_DiscoveryBrowser>{new Dmn_DiscoveryBrowser{impl}}};
  auto handle{m_impl->backend->browse(
      {std::move(query.service_type), std::move(query.domain),
       query.interface_index},
      [handler = std::move(handler),
       options = m_impl->options](detail::DiscoveryDelivery delivery) {
        auto event{[&]() -> Dmn_DiscoveryValueResult<Dmn_DiscoveryEvent> {
          try {
            return detail::discoveryPublicEvent(delivery, options);
          } catch (const std::bad_alloc &) {
            throw detail::DiscoveryDeliveryFailure{
                {detail::DiscoveryErrorKind::kResourceLimit, {}}};
          }
        }()};
        if (!event) {
          throw detail::DiscoveryDeliveryFailure{
              {event.error().kind == Dmn_DiscoveryErrorKind::ResourceLimit
                   ? detail::DiscoveryErrorKind::kResourceLimit
                   : detail::DiscoveryErrorKind::kBackend,
               event.error().detail}};
        }
        handler(std::move(*event));
      },
      *families)};
  if (!handle) {
    return std::unexpected{detail::discoveryPublicError(
        handle.error(), Dmn_DiscoveryErrorKind::BackendUnavailable)};
  }
  impl->handle = std::move(*handle);
  impl->initialized = true;
  return browser;
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryValueResult<std::unique_ptr<Dmn_DiscoveryRegistration>>
Dmn_DiscoveryClient::advertise(Dmn_DiscoveryAdvertisement advertisement,
                               Dmn_DiscoveryRegistrationHandler handler) try {
  if (!handler) {
    return std::unexpected{
        Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::InvalidArgument,
                           "empty registration handler",
                           {},
                           {}}};
  }
  auto impl{std::make_shared<Dmn_DiscoveryRegistration::Impl>()};
  auto registration{std::unique_ptr<Dmn_DiscoveryRegistration>{
      new Dmn_DiscoveryRegistration{impl}}};
  auto handle{m_impl->backend->advertise(
      detail::discoveryAdvertisement(advertisement),
      [handler =
           std::move(handler)](detail::DiscoveryRegistrationStatus status) {
        auto converted{[&] {
          try {
            return detail::discoveryPublicRegistrationStatus(status);
          } catch (const std::bad_alloc &) {
            throw detail::DiscoveryDeliveryFailure{
                {detail::DiscoveryErrorKind::kResourceLimit, {}}};
          }
        }()};
        handler(std::move(converted));
      })};
  if (!handle) {
    return std::unexpected{detail::discoveryPublicError(
        handle.error(), Dmn_DiscoveryErrorKind::BackendUnavailable)};
  }
  impl->handle = std::move(*handle);
  impl->initialized = true;
  return registration;
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

Dmn_DiscoveryResult Dmn_DiscoveryClient::shutdown() try {
  return publicResult(m_impl->backend->shutdown());
} catch (const std::bad_alloc &) {
  return std::unexpected{allocationError()};
}

} // namespace dmn
