/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-delivery.cpp
 * @brief Bounded event ordering and overflow latch implementation.
 */

#include "dmn-discovery-delivery.hpp"

#ifdef FIU_ENABLE
#include "dmn-fault-injection.hpp"
#endif

#include <limits>
#include <utility>

namespace dmn::detail {

DiscoveryDeliveryQueue::DiscoveryDeliveryQueue(std::size_t maxData,
                                               std::size_t maxControl)
    : m_max_data{maxData}, m_max_control{maxControl} {}

DiscoveryResult DiscoveryDeliveryQueue::admit() {
  if (m_state == DiscoveryDeliveryState::kStopped) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery delivery stopped"}};
  }

  if (m_state == DiscoveryDeliveryState::kOverflow) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "discovery delivery overflow"}};
  }

  if (m_sequence == std::numeric_limits<std::uint64_t>::max()) {
    return overflow();
  }

  return {};
}

DiscoveryResult DiscoveryDeliveryQueue::overflow() {
  latchOverflow();

  return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                        "discovery delivery overflow"}};
}

void DiscoveryDeliveryQueue::latchOverflow() noexcept {
  if (m_state != DiscoveryDeliveryState::kRunning) {
    return;
  }

  m_state = DiscoveryDeliveryState::kOverflow;
  m_overflowed = true;
  m_data.clear();
  m_control.clear();
  m_emergency.reset();
  if (m_epoch != std::numeric_limits<std::uint64_t>::max()) {
    ++m_epoch;
  }

  if (m_sequence != std::numeric_limits<std::uint64_t>::max()) {
    m_emergency = DiscoveryDelivery{
        m_epoch, ++m_sequence, {}, DiscoveryControl::kOverflow};
  }
}

DiscoveryResult DiscoveryDeliveryQueue::pushData(DiscoveryEvent event) {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (m_data.size() >= m_max_data) {
    return overflow();
  }

  event.m_epoch = m_epoch;
  event.m_sequence = m_sequence + 1;
  m_data.push_back(
      DiscoveryDelivery{m_epoch, event.m_sequence, std::move(event), {}});
  ++m_sequence;

  return {};
}

DiscoveryResult
DiscoveryDeliveryQueue::pushRegistration(DiscoveryRegistrationStatus status) {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (m_data.size() >= m_max_data) {
    return overflow();
  }

  m_data.push_back(
      DiscoveryDelivery{m_epoch, m_sequence + 1, {}, {}, std::move(status)});
  ++m_sequence;

  return {};
}

DiscoveryResult DiscoveryDeliveryQueue::registrationBackendChange(
    DiscoveryRegistrationStatus status) {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (m_epoch == std::numeric_limits<std::uint64_t>::max() ||
      m_control.size() >= m_max_control) {
    return overflow();
  }

  const bool unavailable{status.m_state ==
                         DiscoveryRegistrationState::kUnavailable};
  m_control.push_back(DiscoveryDelivery{
      m_epoch + 1, m_sequence + 1, {}, {}, std::move(status)});
  if (unavailable) {
    m_data.clear();
  }

  ++m_epoch;
  ++m_sequence;

  return {};
}

DiscoveryResult DiscoveryDeliveryQueue::pushControl(DiscoveryControl control) {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (control == DiscoveryControl::kOverflow) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "reserved overflow control"}};
  }

  if (m_control.size() >= m_max_control) {
    return overflow();
  }

  m_control.push_back(DiscoveryDelivery{m_epoch, m_sequence + 1, {}, control});
  ++m_sequence;

  return {};
}

DiscoveryResult DiscoveryDeliveryQueue::reset() {
  if (m_state == DiscoveryDeliveryState::kStopped) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kStopped,
                                          "discovery delivery stopped"}};
  }

  if (m_epoch == std::numeric_limits<std::uint64_t>::max()) {
    return overflow();
  }

  m_data.clear();
  m_emergency.reset();
  ++m_epoch;
  m_state = DiscoveryDeliveryState::kRunning;
  m_overflowed = false;

#ifdef FIU_ENABLE
  if (fiu_fail("dmn/discovery/delivery/reset-allocation")) {
    throw std::bad_alloc{};
  }
#endif
  return pushControl(DiscoveryControl::kReset);
}

DiscoveryResult DiscoveryDeliveryQueue::backendLost() {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (m_epoch == std::numeric_limits<std::uint64_t>::max()) {
    return overflow();
  }

  m_data.clear();
  m_emergency.reset();
  ++m_epoch;
#ifdef FIU_ENABLE
  if (fiu_fail("dmn/discovery/delivery/backend-lost-allocation")) {
    throw std::bad_alloc{};
  }
#endif
  if (auto result{pushControl(DiscoveryControl::kBackendUnavailable)};
      !result) {
    return result;
  }

  return pushControl(DiscoveryControl::kReset);
}

DiscoveryResult DiscoveryDeliveryQueue::backendAvailable() {
  if (auto result{admit()}; !result) {
    return result;
  }

  if (m_epoch == std::numeric_limits<std::uint64_t>::max()) {
    return overflow();
  }

  ++m_epoch;

  return pushControl(DiscoveryControl::kBackendAvailable);
}

DiscoveryResult DiscoveryDeliveryQueue::failOverflow() {
  if (auto result{admit()}; !result) {
    return result;
  }

  return overflow();
}

void DiscoveryDeliveryQueue::stop() {
  if (m_state != DiscoveryDeliveryState::kStopped &&
      m_epoch != std::numeric_limits<std::uint64_t>::max()) {
    ++m_epoch;
  }

  m_state = DiscoveryDeliveryState::kStopped;
  m_data.clear();
  m_control.clear();
  m_emergency.reset();
}

std::optional<DiscoveryDelivery> DiscoveryDeliveryQueue::pop() {
  if (m_emergency) {
    auto result{std::move(m_emergency)};
    m_emergency.reset();

    return result;
  }

  if (m_data.empty() && m_control.empty()) {
    return std::nullopt;
  }

  auto &queue{m_data.empty() ||
                      (!m_control.empty() &&
                       m_control.front().m_sequence < m_data.front().m_sequence)
                  ? m_control
                  : m_data};
  auto result{std::move(queue.front())};
  queue.pop_front();

  return result;
}

std::uint64_t DiscoveryDeliveryQueue::epoch() const { return m_epoch; }

std::uint64_t DiscoveryDeliveryQueue::sequence() const { return m_sequence; }

DiscoveryDeliveryState DiscoveryDeliveryQueue::state() const { return m_state; }

bool DiscoveryDeliveryQueue::overflowed() const { return m_overflowed; }

} // namespace dmn::detail
