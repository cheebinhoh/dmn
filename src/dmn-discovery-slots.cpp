/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-slots.cpp
 * @brief RAII accounting for bounded native discovery operations.
 */

#include "dmn-discovery-slots.hpp"

#include <mutex>
#include <utility>

namespace dmn::detail {

struct DiscoveryNativeSlotState {
  explicit DiscoveryNativeSlotState(std::size_t limit) : m_limit{limit} {}

  std::mutex m_mutex;
  std::size_t m_limit;
  std::size_t m_used{0};
};

DiscoveryNativeSlots::Lease::Lease(
    std::shared_ptr<DiscoveryNativeSlotState> state)
    : m_state{std::move(state)} {}

DiscoveryNativeSlots::Lease::~Lease() { release(); }

DiscoveryNativeSlots::Lease::Lease(Lease &&other) noexcept
    : m_state{std::move(other.m_state)} {}

DiscoveryNativeSlots::Lease &
DiscoveryNativeSlots::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    release();
    m_state = std::move(other.m_state);
  }

  return *this;
}

void DiscoveryNativeSlots::Lease::release() {
  if (!m_state) {
    return;
  }

  auto state{std::move(m_state)};
  std::lock_guard lock{state->m_mutex};
  --state->m_used;
}

DiscoveryNativeSlots::DiscoveryNativeSlots(std::size_t limit)
    : m_state{std::make_shared<DiscoveryNativeSlotState>(limit)} {}

std::expected<DiscoveryNativeSlots::Lease, DiscoveryError>
DiscoveryNativeSlots::reserve() {
  std::lock_guard lock{m_state->m_mutex};
  if (m_state->m_used >= m_state->m_limit) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "native operation limit reached"}};
  }

  ++m_state->m_used;

  return Lease{m_state};
}

std::size_t DiscoveryNativeSlots::inUse() const {
  std::lock_guard lock{m_state->m_mutex};

  return m_state->m_used;
}

} // namespace dmn::detail
