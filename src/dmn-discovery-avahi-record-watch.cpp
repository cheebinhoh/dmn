/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-record-watch.cpp
 * @brief Owned Avahi SRV/TXT subscriptions for a browse contribution.
 */

#include "dmn-discovery-avahi-record-watch.hpp"
#include "dmn-discovery-avahi-error.hpp"

#include <avahi-common/defs.h>
#include <avahi-common/domain.h>
#include <avahi-common/error.h>

#include <array>
#include <cstring>
#include <utility>

namespace dmn::detail {
namespace {

class NativeRecordApi final : public DiscoveryAvahiRecordApi {
public:
  AvahiRecordBrowser *create(AvahiClient *client, AvahiIfIndex interfaceIndex,
                             AvahiProtocol protocol, const char *name,
                             std::uint16_t clazz, std::uint16_t type,
                             AvahiLookupFlags flags,
                             AvahiRecordBrowserCallback callback,
                             void *context) override {
    return avahi_record_browser_new(client, interfaceIndex, protocol, name,
                                    clazz, type, flags, callback, context);
  }

  void free(AvahiRecordBrowser *browser) noexcept override {
    avahi_record_browser_free(browser);
  }

  int clientError(AvahiClient *client) override {
    return avahi_client_errno(client);
  }

  const char *errorString(int error) override { return avahi_strerror(error); }
};

} // namespace

std::shared_ptr<DiscoveryAvahiRecordApi> makeDiscoveryAvahiRecordApi() {
  return std::make_shared<NativeRecordApi>();
}

void DiscoveryAvahiRecordWatch::Deleter::operator()(
    AvahiRecordBrowser *browser) const noexcept {
  if (browser) {
    m_api->free(browser);
  }
}

DiscoveryAvahiRecordWatch::DiscoveryAvahiRecordWatch(
    std::shared_ptr<DiscoveryAvahiRecordApi> api, AvahiClient *client,
    DiscoveryServiceKey key, DiscoveryContribution contribution,
    DiscoveryRecordKind kind, DiscoveryOptions options, Handler handler,
    FailureHandler failureHandler)
    : m_api{std::move(api)}, m_client{client}, m_key{std::move(key)},
      m_contribution{contribution}, m_kind{kind}, m_options{options},
      m_handler{std::move(handler)},
      m_failure_handler{std::move(failureHandler)},
      m_browser{nullptr, Deleter{m_api}} {}

DiscoveryAvahiRecordWatch::~DiscoveryAvahiRecordWatch() = default;

DiscoveryResult DiscoveryAvahiRecordWatch::start() {
  if (!m_api || !m_client || m_browser || m_starting || m_callback_depth ||
      !validateOptions(m_options) || !validateInstanceName(m_key.m_instance) ||
      !validateServiceType(m_key.m_type) || !validateDomain(m_key.m_domain) ||
      (m_kind != DiscoveryRecordKind::kSrv &&
       m_kind != DiscoveryRecordKind::kTxt)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi record watch"}};
  }

  auto args{makeAvahiResolverArguments(m_key.m_interface, m_contribution,
                                       DiscoveryAddressFamily::kIpv4)};
  if (!args) {
    return std::unexpected{args.error()};
  }

  std::array<char, AVAHI_DOMAIN_NAME_MAX> name{};
  if (avahi_service_name_join(name.data(), name.size(),
                              m_key.m_instance.c_str(), m_key.m_type.c_str(),
                              m_key.m_domain.c_str()) < 0 ||
      !validateDomain(name.data())) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Avahi service name"}};
  }

  m_name = name.data();
  m_interface = args->m_interface;
  m_protocol = args->m_query_protocol;
  m_error.reset();
  m_callback_failed = false;
  m_callback_allocation_failed = false;
  const auto type{static_cast<std::uint16_t>(m_kind == DiscoveryRecordKind::kSrv
                                                 ? AVAHI_DNS_TYPE_SRV
                                                 : AVAHI_DNS_TYPE_TXT)};
  m_starting = true;
  AvahiRecordBrowser *browser;
  try {
    browser = m_api->create(m_client, m_interface, m_protocol, m_name.c_str(),
                            AVAHI_DNS_CLASS_IN, type,
                            static_cast<AvahiLookupFlags>(0), onRecord, this);
  } catch (...) {
    m_starting = false;

    throw;
  }

  m_starting = false;
  m_browser.reset(browser);
  auto result{status()};
  if (!result) {
    m_browser.reset();

    return result;
  }

  if (!m_browser) {
    return std::unexpected{nativeError("Avahi record watch creation failed")};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiRecordWatch::status() const {
  if (m_callback_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Avahi record callback failed"}};
  }

  if (m_error) {
    return std::unexpected{*m_error};
  }

  return {};
}

DiscoveryResult DiscoveryAvahiRecordWatch::stop() {
  if (m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "reentrant Avahi record watch stop"}};
  }

  m_browser.reset();

  return {};
}

DiscoveryError
DiscoveryAvahiRecordWatch::nativeError(const char *prefix) const {
  const auto code{m_api->clientError(m_client)};

  return discoveryAvahiError(prefix, code, m_api->errorString(code));
}

void DiscoveryAvahiRecordWatch::report(DiscoveryError error) {
  if (!m_error) {
    m_error = std::move(error);
    if (m_failure_handler) {
      m_failure_handler(*m_error);
    }
  }
}

void DiscoveryAvahiRecordWatch::onRecord(
    AvahiRecordBrowser *browser, AvahiIfIndex interfaceIndex,
    AvahiProtocol protocol, AvahiBrowserEvent event, const char *name,
    std::uint16_t clazz, std::uint16_t type, const void *rdata,
    std::size_t size, AvahiLookupResultFlags flags, void *context) noexcept {
  auto &self{*static_cast<DiscoveryAvahiRecordWatch *>(context)};
  if (!browser || (!self.m_starting &&
                   (!self.m_browser || self.m_browser.get() != browser))) {
    return;
  }

  ++self.m_callback_depth;
  try {
    if (!self.m_error && !self.m_callback_failed) {
      if (event == AVAHI_BROWSER_FAILURE) {
        self.report(self.nativeError("Avahi record watch failed"));
      } else if (event != AVAHI_BROWSER_CACHE_EXHAUSTED &&
                 event != AVAHI_BROWSER_ALL_FOR_NOW) {
        const auto expectedType{self.m_kind == DiscoveryRecordKind::kSrv
                                    ? AVAHI_DNS_TYPE_SRV
                                    : AVAHI_DNS_TYPE_TXT};
        const auto nameSize{name ? strnlen(name, AVAHI_DOMAIN_NAME_MAX) : 0};
        auto actualName{
            name && nameSize < AVAHI_DOMAIN_NAME_MAX
                ? canonicalizeDomain(std::string_view{name, nameSize})
                : std::expected<std::string, DiscoveryError>{std::unexpected{
                      DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                     "invalid Avahi record name"}}}};
        auto expectedName{canonicalizeDomain(self.m_name)};
        if ((event != AVAHI_BROWSER_NEW && event != AVAHI_BROWSER_REMOVE) ||
            interfaceIndex != self.m_interface || protocol != self.m_protocol ||
            clazz != AVAHI_DNS_CLASS_IN || type != expectedType ||
            !actualName || !expectedName || *actualName != *expectedName) {
          self.report(
              {DiscoveryErrorKind::kBackend, "invalid Avahi record callback"});
        } else {
          std::expected<DiscoveryAvahiRecordEvent, DiscoveryError> mapped{
              std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                             "invalid record kind"}}};
          if (self.m_kind == DiscoveryRecordKind::kSrv) {
            auto srv{mapAvahiSrvRecord(rdata, size)};
            if (srv) {
              mapped = DiscoveryAvahiRecordEvent{event == AVAHI_BROWSER_NEW,
                                                 std::move(*srv), flags};
            } else {
              mapped = std::unexpected{srv.error()};
            }
          } else {
            auto txt{mapAvahiTxtRecord(rdata, size,
                                       self.m_options.m_max_txt_entries,
                                       self.m_options.m_max_txt_bytes)};
            if (txt) {
              mapped = DiscoveryAvahiRecordEvent{event == AVAHI_BROWSER_NEW,
                                                 std::move(*txt), flags};
            } else {
              mapped = std::unexpected{txt.error()};
            }
          }

          if (!mapped) {
            self.report(mapped.error());
          } else if (self.m_handler) {
            auto result{self.m_handler(std::move(*mapped))};
            if (!result) {
              self.report(result.error());
            }
          }
        }
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
        self.m_failure_handler(
            {DiscoveryErrorKind::kBackend, "Avahi record callback failed"});
      }
    } catch (...) {
      self.m_callback_failed = true;
    }
  }

  --self.m_callback_depth;
}

} // namespace dmn::detail
