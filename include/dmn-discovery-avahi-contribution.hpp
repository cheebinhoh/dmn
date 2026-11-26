/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-avahi-contribution.hpp
 * @brief Worker-owned native operations for one live browse contribution.
 */
#ifndef DMN_DISCOVERY_AVAHI_CONTRIBUTION_HPP_
#define DMN_DISCOVERY_AVAHI_CONTRIBUTION_HPP_

#include "dmn-discovery-avahi-record-watch.hpp"
#include "dmn-discovery-avahi-resolver.hpp"
#include "dmn-discovery-session.hpp"

#include <array>

namespace dmn::detail {

/**
 * All methods, callbacks and destruction run on the session's state worker.
 * The parent has already called observe(), owns the client, and must call
 * drive() after native callback batches and after any model change (including
 * changes from other contributions and refresh). Never drive or clean up
 * inside a native callback. Dispatch each due retry to its source via retry().
 * The address-family flags select IPv4 and IPv6, in that order.
 * Call cleanup() before removing the observation or freeing the client.
 * On epoch invalidation cleanup still frees native objects; stale session
 * errors can be returned because the session already released its leases.
 * A drive/start/retry error is never a success indication: backend errors are
 * reported even when their retry was scheduled. The parent must continue
 * driving live contributions and route all IDs from takeDueRetries(). Native
 * backend failures and SRV mismatches are considered recoverable; other
 * failures are not. An exception during drive cleans up and terminates this
 * owner; resource-limit and terminal session errors require parent teardown.
 */
class DiscoveryAvahiContribution {
public:
  DiscoveryAvahiContribution(
      DiscoverySessionHandle session, std::uint64_t epoch,
      DiscoveryServiceKey key, DiscoveryContribution contribution,
      std::uint64_t source, DiscoveryOptions options, AvahiClient *client,
      std::shared_ptr<DiscoveryAvahiRecordApi> records,
      std::shared_ptr<DiscoveryAvahiResolverApi> resolvers,
      std::array<bool, 2> addressFamilies = {true, true});
  ~DiscoveryAvahiContribution();
  DiscoveryAvahiContribution(const DiscoveryAvahiContribution &) = delete;
  DiscoveryAvahiContribution &
  operator=(const DiscoveryAvahiContribution &) = delete;
  DiscoveryAvahiContribution(DiscoveryAvahiContribution &&) = delete;
  DiscoveryAvahiContribution &
  operator=(DiscoveryAvahiContribution &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult drive();
  [[nodiscard]] DiscoveryResult retry(DiscoveryAttemptId id);
  /** @brief Ignore further native callbacks until deferred cleanup. */
  void invalidate() noexcept { m_invalidated = true; }

  void releaseNative() noexcept;
  [[nodiscard]] DiscoveryResult cleanup();
  /** @brief Whether an unrecoverable exception or cleanup ended this owner. */
  [[nodiscard]] bool cleaned() const { return m_cleaned; }

private:
  struct Operation {
    std::optional<std::uint64_t> m_slot;
    std::uint64_t m_token{0};
    std::uint64_t m_resolution{0};
    std::unique_ptr<DiscoveryAvahiRecordWatch> m_watch;
    std::unique_ptr<DiscoveryAvahiServiceResolver> m_resolver;
    std::optional<DiscoveryError> m_failure;
    bool m_success{false};
    bool m_blocked{false};
  };
  [[nodiscard]] DiscoveryResult startWatch(std::size_t index);
  [[nodiscard]] DiscoveryResult startResolver(std::size_t index,
                                              std::uint64_t resolution,
                                              std::uint64_t token);
  [[nodiscard]] DiscoveryResult release(Operation &operation,
                                        bool modelActive = true);
  [[nodiscard]] DiscoveryResult recordsUpdate(
      std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update);
  void failed(std::size_t index, DiscoveryError error);

  DiscoverySessionHandle m_session;
  std::uint64_t m_epoch;
  DiscoveryServiceKey m_key;
  DiscoveryContribution m_contribution;
  std::uint64_t m_source;
  DiscoveryOptions m_options;
  AvahiClient *m_client;
  std::shared_ptr<DiscoveryAvahiRecordApi> m_record_api;
  std::shared_ptr<DiscoveryAvahiResolverApi> m_resolver_api;
  std::array<bool, 2> m_address_families;
  std::array<Operation, 4> m_operations;
  bool m_started{false};
  bool m_cleaned{false};
  bool m_cleanup_allocation_failed{false};
  bool m_driving{false};
  bool m_invalidated{false};
};

} // namespace dmn::detail
#endif // DMN_DISCOVERY_AVAHI_CONTRIBUTION_HPP_
