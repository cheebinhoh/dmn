/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-bonjour-operation.cpp
 * @brief Shared native reference ownership for Bonjour operations.
 */

#include "dmn-discovery-bonjour-operation.hpp"
#include "dmn-discovery-bonjour-mapping.hpp"

#include <utility>

namespace dmn::detail {

DiscoveryBonjourOperation::DiscoveryBonjourOperation(
    std::shared_ptr<DiscoveryBonjourPoller> poller,
    FailureHandler failureHandler)
    : m_poller{std::move(poller)},
      m_failure_handler{std::move(failureHandler)} {}

DiscoveryBonjourOperation::~DiscoveryBonjourOperation() {
  static_cast<void>(stop());
}

DiscoveryResult DiscoveryBonjourOperation::start(NativeStart create) {
  if (!m_poller || !create || m_ref || m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid Bonjour operation state"}};
  }

  m_error.reset();
  m_callback_failed = false;
  m_allocation_failed = false;
  DNSServiceRef ref{nullptr};
  struct ReferenceGuard {
    ~ReferenceGuard() {
      if (m_ref) {
        DNSServiceRefDeallocate(m_ref);
      }
    }

    DNSServiceRef &m_ref;
  } referenceGuard{ref};
  m_starting = true;
  struct Guard {
    ~Guard() { m_starting = false; }

    bool &m_starting;
  } guard{m_starting};
  const auto error{create(&ref)};
  if (error != 0 || !ref) {
    return std::unexpected{
        error != 0
            ? discoveryBonjourError("Bonjour operation creation failed", error)
            : DiscoveryError{DiscoveryErrorKind::kBackend,
                             "Bonjour operation returned no reference"}};
  }

  if (auto result{status()}; !result) {
    return result;
  }

  auto result{m_poller->adopt(
      reinterpret_cast<DiscoveryBonjourRef *>(ref),
      [this](DiscoveryError failure) { pollFailure(std::move(failure)); })};
  if (!result) {
    return result;
  }

  m_ref = std::exchange(ref, nullptr);

  return {};
}

DiscoveryResult DiscoveryBonjourOperation::status() const {
  if (m_allocation_failed) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  if (m_callback_failed) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "Bonjour operation callback failed"}};
  }

  if (m_error) {
    return std::unexpected{*m_error};
  }

  return {};
}

DiscoveryResult DiscoveryBonjourOperation::stop() {
  if (m_starting || m_callback_depth) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kReentrantCall,
                                          "reentrant Bonjour operation stop"}};
  }

  if (m_ref) {
    auto result{
        m_poller->remove(reinterpret_cast<DiscoveryBonjourRef *>(m_ref))};
    if (!result) {
      return result;
    }

    m_ref = nullptr;
  }

  return {};
}

void DiscoveryBonjourOperation::report(DiscoveryError error) {
  if (!m_error) {
    m_error = std::move(error);
    if (m_failure_handler) {
      m_failure_handler(*m_error);
    }
  }
}

void DiscoveryBonjourOperation::pollFailure(DiscoveryError error) {
  m_ref = nullptr;
  if (error.m_backend_domain == "bonjour" && error.m_backend_code) {
    error = discoveryBonjourError(
        "Bonjour reference failed",
        static_cast<DNSServiceErrorType>(*error.m_backend_code));
  }

  report(std::move(error));
}

} // namespace dmn::detail
