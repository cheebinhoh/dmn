/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-slots.hpp
 * @brief Private quota for owned native discovery operations.
 *
 * The owner must retain a lease until its native object has actually been
 * freed, including during asynchronous cancellation. This private quota does
 * not itself own or free native objects. The counter state can outlive the
 * allocator safely when an outstanding lease still owns it.
 */

#ifndef DMN_DISCOVERY_SLOTS_HPP_
#define DMN_DISCOVERY_SLOTS_HPP_

#include "dmn-discovery-core.hpp"

#include <cstddef>
#include <expected>
#include <memory>

namespace dmn::detail {

struct DiscoveryNativeSlotState;

/** @brief Thread-safe admission and lifetime accounting for native handles. */
class DiscoveryNativeSlots {
public:
  /** @brief Unique ownership of one admitted native operation slot. */
  class Lease {
  public:
    Lease() = default;
    ~Lease();
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept;
    Lease &operator=(Lease &&other) noexcept;

    /** @brief Release the slot once; repeated calls have no effect. */
    void release();

  private:
    friend class DiscoveryNativeSlots;
    explicit Lease(std::shared_ptr<DiscoveryNativeSlotState> state);
    std::shared_ptr<DiscoveryNativeSlotState> m_state;
  };

  /**
   * @brief Create an allocator with a finite per-browser native quota.
   *
   * @param limit Maximum concurrently owned native operations.
   */
  explicit DiscoveryNativeSlots(std::size_t limit);

  /**
   * @brief Reserve a slot before invoking a native constructor.
   *
   * @return Unique lease or a typed resource-limit error.
   */
  [[nodiscard]] std::expected<Lease, DiscoveryError> reserve();

  /** @brief Copy the number of currently owned slots. */
  [[nodiscard]] std::size_t inUse() const;

private:
  std::shared_ptr<DiscoveryNativeSlotState> m_state;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_SLOTS_HPP_
