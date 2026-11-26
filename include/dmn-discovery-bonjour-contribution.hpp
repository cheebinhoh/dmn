/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-contribution.hpp
 * @brief Worker-owned Bonjour record and address reconciliation.
 */

#ifndef DMN_DISCOVERY_BONJOUR_CONTRIBUTION_HPP_
#define DMN_DISCOVERY_BONJOUR_CONTRIBUTION_HPP_

#include "dmn-discovery-bonjour-address.hpp"
#include "dmn-discovery-bonjour-record.hpp"
#include "dmn-discovery-session.hpp"

#include <array>

namespace dmn::detail {

/**
 * @brief Reconcile one live Bonjour browse observation on the state worker.
 *
 * The owner drives after native callbacks and model changes, routes due retry
 * IDs, and cleans up before removing the observation or stopping the poller.
 * The owner must not drive or destroy this object inside a native callback.
 */
class DiscoveryBonjourContribution {
public:
  DiscoveryBonjourContribution(DiscoverySessionHandle session,
                               std::uint64_t epoch, DiscoveryServiceKey key,
                               std::uint64_t source, DiscoveryOptions options,
                               std::shared_ptr<DiscoveryBonjourPoller> poller,
                               std::array<bool, 2> addressFamilies = {true,
                                                                      true});
  ~DiscoveryBonjourContribution();
  DiscoveryBonjourContribution(const DiscoveryBonjourContribution &) = delete;
  DiscoveryBonjourContribution &
  operator=(const DiscoveryBonjourContribution &) = delete;
  DiscoveryBonjourContribution(DiscoveryBonjourContribution &&) = delete;
  DiscoveryBonjourContribution &
  operator=(DiscoveryBonjourContribution &&) = delete;

  [[nodiscard]] DiscoveryResult start();
  [[nodiscard]] DiscoveryResult drive();
  [[nodiscard]] DiscoveryResult retry(DiscoveryAttemptId id);
  void invalidate() noexcept { m_invalidated = true; }
  void releaseNative() noexcept;
  [[nodiscard]] DiscoveryResult cleanup();
  [[nodiscard]] bool cleaned() const { return m_cleaned; }

private:
  struct Operation {
    std::optional<std::uint64_t> m_slot;
    std::uint64_t m_token{0};
    std::uint64_t m_resolution{0};
    std::unique_ptr<DiscoveryBonjourRecordWatch> m_watch;
    std::unique_ptr<DiscoveryBonjourAddressLookup> m_lookup;
    std::optional<DiscoveryError> m_failure;
    bool m_success{false};
    bool m_blocked{false};
  };
  [[nodiscard]] DiscoveryResult startWatch(std::size_t index);
  [[nodiscard]] DiscoveryResult startLookup(std::size_t index,
                                            std::uint64_t resolution,
                                            std::uint64_t token,
                                            const DiscoverySrv &srv);
  [[nodiscard]] DiscoveryResult release(Operation &operation,
                                        bool modelActive = true);
  [[nodiscard]] DiscoveryResult recordsUpdate(
      std::function<DiscoveryResult(DiscoveryRecords &, std::uint64_t)> update);
  void failed(std::size_t index, DiscoveryError error);

  DiscoverySessionHandle m_session;
  std::uint64_t m_epoch;
  DiscoveryServiceKey m_key;
  std::uint64_t m_source;
  DiscoveryOptions m_options;
  std::shared_ptr<DiscoveryBonjourPoller> m_poller;
  std::array<bool, 2> m_address_families;
  std::array<Operation, 4> m_operations;
  std::string m_full_name;
  bool m_started{false};
  bool m_cleaned{false};
  bool m_cleanup_allocation_failed{false};
  bool m_driving{false};
  bool m_invalidated{false};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_CONTRIBUTION_HPP_
