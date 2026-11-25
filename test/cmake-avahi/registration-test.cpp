/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file registration-test.cpp
 * @brief Controlled tests for native Avahi registration.
 */

#include "dmn-discovery-avahi-registration.hpp"

#include <avahi-common/error.h>
#include <avahi-common/strlst.h>

#include <stdexcept>
#include <vector>

struct AvahiClient {};
struct AvahiEntryGroup {};

namespace {
using namespace dmn::detail;

void check(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error{message};
  }
}

class FakeRegistrationApi final : public DiscoveryAvahiRegistrationApi {
public:
  AvahiEntryGroup *create(AvahiClient *, AvahiEntryGroupCallback callback,
                          void *context) override {
    m_callback = callback;
    m_context = context;
    if (m_fail_create) {
      return nullptr;
    }
    callback(&m_group, AVAHI_ENTRY_GROUP_UNCOMMITED, context);
    return &m_group;
  }
  void free(AvahiEntryGroup *) noexcept override { ++m_freed; }
  int add(AvahiEntryGroup *, AvahiIfIndex interfaceIndex,
          AvahiProtocol protocol, AvahiPublishFlags flags, const char *name,
          const char *type, const char *domain, std::uint16_t port,
          AvahiStringList *txt) override {
    m_interface = interfaceIndex;
    m_protocol = protocol;
    m_flags = flags;
    m_name = name;
    m_type = type;
    m_domain = domain;
    m_port = port;
    m_wire.resize(avahi_string_list_serialize(txt, nullptr, 0));
    avahi_string_list_serialize(txt, m_wire.data(), m_wire.size());
    return m_add_result;
  }
  int commit(AvahiEntryGroup *) override {
    if (m_commit_result == 0) {
      m_callback(&m_group, AVAHI_ENTRY_GROUP_ESTABLISHED, m_context);
    }
    return m_commit_result;
  }
  int clientError(AvahiClient *) override { return -1; }
  const char *errorString(int) override { return "fake native error"; }

  void emit(AvahiEntryGroupState state) {
    m_callback(&m_group, state, m_context);
  }

  bool m_fail_create{false};
  int m_add_result{0};
  int m_commit_result{0};
  unsigned m_freed{0};
  AvahiIfIndex m_interface{};
  AvahiProtocol m_protocol{};
  AvahiPublishFlags m_flags{};
  std::string m_name, m_type, m_domain;
  std::uint16_t m_port{};
  std::vector<std::uint8_t> m_wire;

private:
  AvahiEntryGroup m_group{};
  AvahiEntryGroupCallback m_callback{nullptr};
  void *m_context{nullptr};
};

DiscoveryAdvertisement advertisement() {
  return {{"Printer.Room", "_ipp._tcp", "local.", 7},
          8080,
          {{0, 255}, {'a', '=', 'b'}}};
}

void establishmentAndWithdrawal() {
  auto api{std::make_shared<FakeRegistrationApi>()};
  AvahiClient client{};
  std::vector<DiscoveryAvahiRegistrationEvent> events;
  DiscoveryAvahiRegistration registration{
      api, &client, advertisement(), DiscoveryOptions{},
      [&](DiscoveryAvahiRegistrationEvent event) -> DiscoveryResult {
        events.push_back(event);
        return {};
      }};
  check(registration.start().has_value(), "registration startup failed");
  check(api->m_interface == 7 && api->m_protocol == AVAHI_PROTO_UNSPEC &&
            api->m_port == 8080 && api->m_name == "Printer.Room" &&
            api->m_type == "_ipp._tcp" && api->m_domain == "local.",
        "registration arguments changed");
  check(api->m_wire == std::vector<std::uint8_t>({2, 0, 255, 3, 'a', '=', 'b'}),
        "binary TXT or ordering changed");
  check(events.size() == 1 &&
            events[0] == DiscoveryAvahiRegistrationEvent::kEstablished,
        "immediate establishment was lost");
  check(registration.stop().has_value() && registration.stop().has_value() &&
            api->m_freed == 1,
        "registration was not withdrawn exactly once");
  api->emit(AVAHI_ENTRY_GROUP_ESTABLISHED);
  check(events.size() == 1, "stopped registration delivered callback");
}

void collisionAndFailures() {
  auto api{std::make_shared<FakeRegistrationApi>()};
  AvahiClient client{};
  std::vector<DiscoveryAvahiRegistrationEvent> events;
  DiscoveryAvahiRegistration registration{
      api, &client, advertisement(), DiscoveryOptions{},
      [&](DiscoveryAvahiRegistrationEvent event) -> DiscoveryResult {
        events.push_back(event);
        return {};
      }};
  check(registration.start().has_value(), "registration startup failed");
  api->emit(AVAHI_ENTRY_GROUP_COLLISION);
  check(events.size() == 2 &&
            events.back() == DiscoveryAvahiRegistrationEvent::kCollision,
        "collision was not delivered");
  check(registration.stop().has_value(), "collision withdrawal failed");

  api = std::make_shared<FakeRegistrationApi>();
  api->m_add_result = AVAHI_ERR_COLLISION;
  events.clear();
  DiscoveryAvahiRegistration immediateCollision{
      api, &client, advertisement(), DiscoveryOptions{},
      [&](DiscoveryAvahiRegistrationEvent event) -> DiscoveryResult {
        events.push_back(event);
        return {};
      }};
  check(immediateCollision.start().has_value() && events.size() == 1 &&
            events[0] == DiscoveryAvahiRegistrationEvent::kCollision,
        "immediate collision was not reported");
  check(immediateCollision.stop().has_value(), "collision cleanup failed");

  api = std::make_shared<FakeRegistrationApi>();
  api->m_fail_create = true;
  DiscoveryAvahiRegistration failed{api, &client, advertisement(),
                                    DiscoveryOptions{}, nullptr};
  const auto result{failed.start()};
  check(!result && result.error().m_message.find("fake native error") !=
                       std::string::npos,
        "native group creation failure was hidden");

  api = std::make_shared<FakeRegistrationApi>();
  api->m_add_result = -1;
  DiscoveryAvahiRegistration failedAdd{api, &client, advertisement(),
                                       DiscoveryOptions{}, nullptr};
  check(!failedAdd.start() && api->m_freed == 1,
        "failed service add did not release its group");

  api = std::make_shared<FakeRegistrationApi>();
  api->m_add_result = AVAHI_ERR_ACCESS_DENIED;
  DiscoveryAvahiRegistration denied{api, &client, advertisement(),
                                    DiscoveryOptions{}, nullptr};
  const auto error{denied.start()};
  check(!error && error.error().m_backend_domain == "avahi" &&
            error.error().m_backend_code == AVAHI_ERR_ACCESS_DENIED &&
            error.error().m_native_category ==
                DiscoveryNativeErrorCategory::kPermission,
        "native permission error identity was lost");

  api = std::make_shared<FakeRegistrationApi>();
  std::optional<DiscoveryError> reported;
  DiscoveryAvahiRegistration allocationFailure{
      api,
      &client,
      advertisement(),
      DiscoveryOptions{},
      [](DiscoveryAvahiRegistrationEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      },
      [&](DiscoveryError failure) { reported = std::move(failure); }};
  const auto allocationStatus{allocationFailure.start()};
  check(!allocationStatus &&
            allocationStatus.error().m_kind ==
                DiscoveryErrorKind::kResourceLimit &&
            reported &&
            reported->m_kind == DiscoveryErrorKind::kResourceLimit &&
            api->m_freed == 1,
        "registration allocation failure classification was lost");

  api = std::make_shared<FakeRegistrationApi>();
  api->m_add_result = AVAHI_ERR_COLLISION;
  DiscoveryAvahiRegistration collisionAllocationFailure{
      api, &client, advertisement(), DiscoveryOptions{},
      [](DiscoveryAvahiRegistrationEvent) -> DiscoveryResult {
        throw std::bad_alloc{};
      }};
  const auto collisionStatus{collisionAllocationFailure.start()};
  check(!collisionStatus &&
            collisionStatus.error().m_kind ==
                DiscoveryErrorKind::kResourceLimit &&
            api->m_freed == 1,
        "collision allocation failure classification was lost");
}

void emptyTxtCapability() {
  auto api{std::make_shared<FakeRegistrationApi>()};
  AvahiClient client{};
  auto mixed{advertisement()};
  mixed.m_txt.push_back({});
  const auto preflight{
      validateDiscoveryAvahiAdvertisement(DiscoveryOptions{}, mixed)};
  check(!preflight &&
            preflight.error().m_kind == DiscoveryErrorKind::kUnsupported,
        "preflight accepted unsupported TXT");
  DiscoveryAvahiRegistration rejected{api, &client, std::move(mixed),
                                      DiscoveryOptions{}, nullptr};
  const auto mixedResult{rejected.start()};
  check(!mixedResult &&
            mixedResult.error().m_kind == DiscoveryErrorKind::kUnsupported &&
            api->m_freed == 0,
        "mixed empty TXT entries were silently changed");

  api = std::make_shared<FakeRegistrationApi>();
  auto duplicateEmpty{advertisement()};
  duplicateEmpty.m_txt = {{}, {}};
  DiscoveryAvahiRegistration rejectedDuplicates{
      api, &client, std::move(duplicateEmpty), DiscoveryOptions{}, nullptr};
  check(!rejectedDuplicates.start(),
        "duplicate empty TXT entries were silently changed");

  api = std::make_shared<FakeRegistrationApi>();
  auto empty{advertisement()};
  empty.m_txt = {{}};
  DiscoveryAvahiRegistration accepted{api, &client, std::move(empty),
                                      DiscoveryOptions{}, nullptr};
  check(accepted.start().has_value() &&
            api->m_wire == std::vector<std::uint8_t>{0},
        "single empty TXT record was rejected");
  check(accepted.stop().has_value(), "empty TXT registration cleanup failed");
}

void preflightValidation() {
  auto valid{advertisement()};
  check(validateDiscoveryAvahiAdvertisement({}, valid).has_value(),
        "valid Avahi advertisement rejected");
  valid.m_key.m_domain = "example.com.";
  auto result{validateDiscoveryAvahiAdvertisement({}, valid)};
  check(!result && result.error().m_kind == DiscoveryErrorKind::kUnsupported,
        "non-local registration accepted");
  valid = advertisement();
  valid.m_port = 0;
  result = validateDiscoveryAvahiAdvertisement({}, valid);
  check(!result &&
            result.error().m_kind == DiscoveryErrorKind::kInvalidArgument,
        "invalid port accepted");
}
} // namespace

int main() {
  establishmentAndWithdrawal();
  collisionAndFailures();
  emptyTxtCapability();
  preflightValidation();
}
