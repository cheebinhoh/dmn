/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-record-watch.hpp
 * @brief Private, per-contribution Avahi SRV and TXT record watches.
 */

#ifndef DMN_DISCOVERY_AVAHI_RECORD_WATCH_HPP_
#define DMN_DISCOVERY_AVAHI_RECORD_WATCH_HPP_

#include "dmn-discovery-avahi-mapping.hpp"

#include <functional>
#include <memory>
#include <variant>

namespace dmn::detail {

/** @brief Injectable native record browser operations. */
class DiscoveryAvahiRecordApi {
public:
  virtual ~DiscoveryAvahiRecordApi() = default;
  virtual AvahiRecordBrowser *create(AvahiClient *, AvahiIfIndex, AvahiProtocol,
                                     const char *, std::uint16_t, std::uint16_t,
                                     AvahiLookupFlags,
                                     AvahiRecordBrowserCallback, void *) = 0;
  virtual void free(AvahiRecordBrowser *) noexcept = 0;
  virtual int clientError(AvahiClient *) = 0;
  virtual const char *errorString(int) = 0;
};

[[nodiscard]] std::shared_ptr<DiscoveryAvahiRecordApi>
makeDiscoveryAvahiRecordApi();

/** @brief One owned record add or removal for the watch's contribution. */
struct DiscoveryAvahiRecordEvent {
  bool m_added;
  std::variant<DiscoverySrv, std::vector<std::uint8_t>> m_record;
  AvahiLookupResultFlags m_flags;
};

/**
 * @brief Own one record subscription on the Avahi state worker.
 *
 * Handlers are internal state-worker functions; they may run during start().
 * They must not destroy or stop the watch. The owner processes failures and
 * releases the watch before freeing its client.
 */
class DiscoveryAvahiRecordWatch {
public:
  using Handler = std::function<DiscoveryResult(DiscoveryAvahiRecordEvent)>;
  using FailureHandler = std::function<void(DiscoveryError)>;

  DiscoveryAvahiRecordWatch(std::shared_ptr<DiscoveryAvahiRecordApi> api,
                            AvahiClient *client, DiscoveryServiceKey key,
                            DiscoveryContribution contribution,
                            DiscoveryRecordKind kind, DiscoveryOptions options,
                            Handler handler,
                            FailureHandler failureHandler = {});
  ~DiscoveryAvahiRecordWatch();
  DiscoveryAvahiRecordWatch(const DiscoveryAvahiRecordWatch &) = delete;
  DiscoveryAvahiRecordWatch &
  operator=(const DiscoveryAvahiRecordWatch &) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();

private:
  struct Deleter {
    void operator()(AvahiRecordBrowser *browser) const noexcept;

    std::shared_ptr<DiscoveryAvahiRecordApi> m_api;
  };
  static void onRecord(AvahiRecordBrowser *, AvahiIfIndex, AvahiProtocol,
                       AvahiBrowserEvent, const char *, std::uint16_t,
                       std::uint16_t, const void *, std::size_t,
                       AvahiLookupResultFlags, void *) noexcept;
  void report(DiscoveryError error);
  [[nodiscard]] DiscoveryError nativeError(const char *prefix) const;

  std::shared_ptr<DiscoveryAvahiRecordApi> m_api;
  AvahiClient *m_client;
  DiscoveryServiceKey m_key;
  DiscoveryContribution m_contribution;
  DiscoveryRecordKind m_kind;
  DiscoveryOptions m_options;
  Handler m_handler;
  FailureHandler m_failure_handler;
  std::unique_ptr<AvahiRecordBrowser, Deleter> m_browser;
  std::string m_name;
  AvahiIfIndex m_interface{AVAHI_IF_UNSPEC};
  AvahiProtocol m_protocol{AVAHI_PROTO_UNSPEC};
  std::optional<DiscoveryError> m_error;
  bool m_starting{false};
  unsigned m_callback_depth{0};
  bool m_callback_failed{false};
  bool m_callback_allocation_failed{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_AVAHI_RECORD_WATCH_HPP_
