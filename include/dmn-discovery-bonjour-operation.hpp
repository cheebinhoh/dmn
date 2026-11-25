/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-operation.hpp
 * @brief Private Bonjour callback and reference lifetime helper.
 */

#ifndef DMN_DISCOVERY_BONJOUR_OPERATION_HPP_
#define DMN_DISCOVERY_BONJOUR_OPERATION_HPP_

#include "dmn-discovery-bonjour-poll.hpp"

#include <dns_sd.h>

#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace dmn::detail {

/** @brief Worker-serialized ownership shared by native operation adapters. */
class DiscoveryBonjourOperation {
public:
  using FailureHandler = std::function<void(DiscoveryError)>;
  using NativeStart = std::function<DNSServiceErrorType(DNSServiceRef *)>;

  DiscoveryBonjourOperation(std::shared_ptr<DiscoveryBonjourPoller> poller,
                            FailureHandler failureHandler);
  ~DiscoveryBonjourOperation();
  DiscoveryBonjourOperation(const DiscoveryBonjourOperation &) = delete;
  DiscoveryBonjourOperation &
  operator=(const DiscoveryBonjourOperation &) = delete;

  [[nodiscard]] DiscoveryResult start(NativeStart create);
  [[nodiscard]] DiscoveryResult status() const;
  [[nodiscard]] DiscoveryResult stop();
  void report(DiscoveryError error);

  /** @brief Catch all exceptions before returning through a native C callback.
   */
  template <typename Callback>
  void invoke(DNSServiceRef ref, Callback &&callback) noexcept {
    if (!ref || (!m_starting && m_ref != ref)) {
      return;
    }

    ++m_callback_depth;
    try {
      if (!m_error && !m_callback_failed) {
        std::forward<Callback>(callback)();
      }
    } catch (const std::bad_alloc &) {
      m_callback_failed = true;
      m_allocation_failed = true;
      try {
        if (m_failure_handler) {
          m_failure_handler({DiscoveryErrorKind::kResourceLimit, {}});
        }
      } catch (...) {
        // The allocation-failure latch remains authoritative.
      }
    } catch (...) {
      m_callback_failed = true;
      try {
        if (m_failure_handler) {
          m_failure_handler({DiscoveryErrorKind::kBackend,
                             "Bonjour operation callback failed"});
        }
      } catch (...) {
        m_callback_failed = true;
      }
    }

    --m_callback_depth;
  }

private:
  void pollFailure(DiscoveryError error);

  std::shared_ptr<DiscoveryBonjourPoller> m_poller;
  FailureHandler m_failure_handler;
  DNSServiceRef m_ref{nullptr};
  std::optional<DiscoveryError> m_error;
  bool m_starting{false};
  bool m_callback_failed{false};
  bool m_allocation_failed{false};
  unsigned m_callback_depth{0};
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_BONJOUR_OPERATION_HPP_
