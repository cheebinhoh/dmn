/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-registration.cpp
 * @brief Owned Avahi entry-group registration and withdrawal.
 */

#include "dmn-discovery-avahi-registration.hpp"
#include "dmn-discovery-avahi-error.hpp"

#include <avahi-common/error.h>
#include <avahi-common/strlst.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace dmn::detail {
namespace {

class NativeRegistrationApi final : public DiscoveryAvahiRegistrationApi {
public:
  AvahiEntryGroup *create(AvahiClient *client, AvahiEntryGroupCallback callback,
                          void *context) override {
    return avahi_entry_group_new(client, callback, context);
  }
  void free(AvahiEntryGroup *group) noexcept override {
    avahi_entry_group_free(group);
  }
  int add(AvahiEntryGroup *group, AvahiIfIndex interfaceIndex,
          AvahiProtocol protocol, AvahiPublishFlags flags, const char *name,
          const char *type, const char *domain, std::uint16_t port,
          AvahiStringList *txt) override {
    return avahi_entry_group_add_service_strlst(group, interfaceIndex, protocol,
                                                flags, name, type, domain,
                                                nullptr, port, txt);
  }
  int commit(AvahiEntryGroup *group) override {
    return avahi_entry_group_commit(group);
  }
  int clientError(AvahiClient *client) override {
    return avahi_client_errno(client);
  }
  const char *errorString(int error) override { return avahi_strerror(error); }
};

struct StringListDeleter {
  void operator()(AvahiStringList *list) const noexcept {
    if (list) {
      avahi_string_list_free(list);
    }
  }
};

struct StartingGuard {
  bool &m_starting;
  ~StartingGuard() { m_starting = false; }
};

} // namespace

std::shared_ptr<DiscoveryAvahiRegistrationApi>
makeDiscoveryAvahiRegistrationApi() {
  return std::make_shared<NativeRegistrationApi>();
}

DiscoveryResult validateDiscoveryAvahiAdvertisement(
    const DiscoveryOptions &options,
    const DiscoveryAdvertisement &advertisement) {
  if (auto result{validateAdvertisement(options, advertisement)}; !result) {
    return result;
  }
  const auto &key{advertisement.m_key};
  if (advertisement.m_txt.size() > 1 &&
      std::any_of(advertisement.m_txt.begin(), advertisement.m_txt.end(),
                  [](const auto &entry) { return entry.empty(); })) {
    return std::unexpected{DiscoveryError{
        DiscoveryErrorKind::kUnsupported,
        "Avahi cannot preserve empty TXT entries in a multi-entry record"}};
  }
  const auto domain{canonicalizeDomain(key.m_domain)};
  const auto local{canonicalizeDomain("local.")};
  if (!domain || !local || *domain != *local) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kUnsupported,
                       "non-local registration unsupported"}};
  }
  if (key.m_interface &&
      *key.m_interface > static_cast<std::uint32_t>(
                             std::numeric_limits<AvahiIfIndex>::max())) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi interface"}};
  }
  return {};
}

void DiscoveryAvahiRegistration::Deleter::operator()(
    AvahiEntryGroup *group) const noexcept {
  if (group) {
    m_api->free(group);
  }
}

DiscoveryAvahiRegistration::DiscoveryAvahiRegistration(
    std::shared_ptr<DiscoveryAvahiRegistrationApi> api, AvahiClient *client,
    DiscoveryAdvertisement advertisement, DiscoveryOptions options,
    Handler handler, FailureHandler failureHandler)
    : m_api{std::move(api)}, m_client{client},
      m_advertisement{std::move(advertisement)}, m_options{options},
      m_handler{std::move(handler)},
      m_failure_handler{std::move(failureHandler)},
      m_group{nullptr, Deleter{m_api}} {}

DiscoveryAvahiRegistration::~DiscoveryAvahiRegistration() = default;

DiscoveryResult DiscoveryAvahiRegistration::start() {
  if (!m_api || !m_client || m_group || m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi registration state"}};
  }
  if (auto result{
          validateDiscoveryAvahiAdvertisement(m_options, m_advertisement)};
      !result) {
    return result;
  }
  const auto &key{m_advertisement.m_key};
  std::unique_ptr<AvahiStringList, StringListDeleter> txt;
  for (const auto &entry : m_advertisement.m_txt) {
    auto *next{
        avahi_string_list_add_arbitrary(txt.get(), entry.data(), entry.size())};
    if (!next) {
      return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                            "Avahi TXT allocation failed"}};
    }
    static_cast<void>(txt.release());
    txt.reset(next);
  }
  m_error.reset();
  m_callback_failed = false;
  m_callback_allocation_failed = false;
  m_collision = false;
  m_starting = true;
  StartingGuard guard{m_starting};
  m_group.reset(m_api->create(m_client, onGroup, this));
  if (auto result{status()}; !result) {
    m_group.reset();
    return result;
  }
  if (!m_group) {
    return std::unexpected{nativeError("Avahi group creation failed",
                                       m_api->clientError(m_client))};
  }
  if (m_collision) {
    return {};
  }
  const auto interfaceIndex{key.m_interface
                                ? static_cast<AvahiIfIndex>(*key.m_interface)
                                : AVAHI_IF_UNSPEC};
  const int added{m_api->add(m_group.get(), interfaceIndex, AVAHI_PROTO_UNSPEC,
                             static_cast<AvahiPublishFlags>(0),
                             key.m_instance.c_str(), key.m_type.c_str(),
                             key.m_domain.c_str(), m_advertisement.m_port,
                             txt.get())};
  if (auto result{status()}; !result) {
    m_group.reset();
    return result;
  }
  if (added == AVAHI_ERR_COLLISION) {
    try {
      notify(DiscoveryAvahiRegistrationEvent::kCollision);
    } catch (const std::bad_alloc &) {
      m_callback_failed = true;
      m_callback_allocation_failed = true;
    } catch (...) {
      m_callback_failed = true;
    }
    auto result{status()};
    if (!result) {
      m_group.reset();
    }
    return result;
  }
  if (added < 0) {
    auto error{nativeError("Avahi service registration failed", added)};
    m_group.reset();
    return std::unexpected{std::move(error)};
  }
  if (m_collision) {
    return {};
  }
  const int committed{m_api->commit(m_group.get())};
  if (auto result{status()}; !result) {
    m_group.reset();
    return result;
  }
  if (committed == AVAHI_ERR_COLLISION) {
    try {
      notify(DiscoveryAvahiRegistrationEvent::kCollision);
    } catch (const std::bad_alloc &) {
      m_callback_failed = true;
      m_callback_allocation_failed = true;
    } catch (...) {
      m_callback_failed = true;
    }
    auto result{status()};
    if (!result) {
      m_group.reset();
    }
    return result;
  }
  if (committed < 0) {
    auto error{nativeError("Avahi group commit failed", committed)};
    m_group.reset();
    return std::unexpected{std::move(error)};
  }
  return {};
}

DiscoveryResult DiscoveryAvahiRegistration::status() const {
  if (m_callback_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }
  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{
        DiscoveryErrorKind::kBackend, "Avahi registration callback failed"}};
  }
  if (m_error) {
    return std::unexpected{*m_error};
  }
  return {};
}

DiscoveryResult DiscoveryAvahiRegistration::stop() {
  if (m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant Avahi registration stop"}};
  }
  m_group.reset();
  return {};
}

DiscoveryError DiscoveryAvahiRegistration::nativeError(const char *prefix,
                                                       int code) const {
  return discoveryAvahiError(prefix, code, m_api->errorString(code));
}

void DiscoveryAvahiRegistration::report(DiscoveryError error) {
  if (!m_error) {
    m_error = std::move(error);
    if (m_failure_handler) {
      m_failure_handler(*m_error);
    }
  }
}

void DiscoveryAvahiRegistration::notify(DiscoveryAvahiRegistrationEvent event) {
  if (m_collision) {
    return;
  }
  if (event == DiscoveryAvahiRegistrationEvent::kCollision) {
    m_collision = true;
  }
  if (m_handler) {
    auto result{m_handler(event)};
    if (!result) {
      report(result.error());
    }
  }
}

void DiscoveryAvahiRegistration::onGroup(AvahiEntryGroup *group,
                                         AvahiEntryGroupState state,
                                         void *context) noexcept {
  auto &self{*static_cast<DiscoveryAvahiRegistration *>(context)};
  if (!group ||
      (!self.m_starting && (!self.m_group || self.m_group.get() != group))) {
    return;
  }
  ++self.m_callback_depth;
  try {
    if (!self.m_error && !self.m_callback_failed) {
      if (state == AVAHI_ENTRY_GROUP_ESTABLISHED) {
        self.notify(DiscoveryAvahiRegistrationEvent::kEstablished);
      } else if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        self.notify(DiscoveryAvahiRegistrationEvent::kCollision);
      } else if (state == AVAHI_ENTRY_GROUP_FAILURE) {
        self.report(self.nativeError("Avahi registration failed",
                                     self.m_api->clientError(self.m_client)));
      } else if (state != AVAHI_ENTRY_GROUP_UNCOMMITED &&
                 state != AVAHI_ENTRY_GROUP_REGISTERING) {
        self.report(
            {DiscoveryErrorKind::kBackend, "invalid Avahi registration state"});
      }
    }
  } catch (const std::bad_alloc &) {
    self.m_callback_failed = true;
    self.m_callback_allocation_failed = true;
    try {
      if (self.m_failure_handler) {
        self.m_failure_handler({DiscoveryErrorKind::kResourceLimit, {}});
      }
    } catch (...) {
      // The allocation-failure latch remains authoritative.
    }
  } catch (...) {
    self.m_callback_failed = true;
    try {
      if (self.m_failure_handler) {
        self.m_failure_handler({DiscoveryErrorKind::kBackend,
                                "Avahi registration callback failed"});
      }
    } catch (...) {
      self.m_callback_failed = true;
    }
  }
  --self.m_callback_depth;
}

} // namespace dmn::detail
