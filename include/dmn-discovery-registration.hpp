/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-registration.hpp
 * @brief Private, caller-serialized advertisement state.
 *
 * This model owns no native registration and does not dispatch callbacks.
 * The session owner must serialize access, withdraw native work and drain
 * application callbacks separately.
 */

#ifndef DMN_DISCOVERY_REGISTRATION_HPP_
#define DMN_DISCOVERY_REGISTRATION_HPP_

#include "dmn-discovery-core.hpp"

#include <functional>
#include <memory>

namespace dmn::detail {

/** @brief Owned advertisement; the native daemon chooses its target host. */
struct DiscoveryAdvertisement {
  DiscoveryServiceKey m_key;
  std::uint16_t m_port;
  DiscoveryTxt m_txt;
};

/** @brief Validate an advertisement against the portable discovery limits. */
[[nodiscard]] DiscoveryResult
validateAdvertisement(const DiscoveryOptions &options,
                      const DiscoveryAdvertisement &advertisement);

/** @brief Locally observed registration lifecycle. */
enum class DiscoveryRegistrationState {
  kRegistering,
  kEstablished,
  kCollision,
  kUnavailable,
  kFailed,
  kOverflow,
  kStopped
};

/** @brief Owned registration state and last reported error. */
struct DiscoveryRegistrationStatus {
  DiscoveryRegistrationState m_state;
  std::optional<DiscoveryError> m_error;
  bool m_callback_failed{false};
  bool m_explicit_stop{false};
};

/**
 * @brief Caller-serialized registration model with stale-generation checks.
 *
 * Its handler is an internal synchronous notification, not an application
 * callback. The owner is responsible for bounded asynchronous delivery.
 */
class DiscoveryRegistration {
public:
  using Handler = std::function<void(const DiscoveryRegistrationStatus &)>;

  /**
   * @brief Validate and accept an owned advertisement.
   *
   * Acceptance is not establishment and does not start native work.
   *
   * @param options Resource limits.
   * @param advertisement Advertisement copied into this model.
   * @param backendAvailable Initial backend availability.
   * @param handler Internal notification sink.
   * @return Owned model or a typed validation error.
   */
  [[nodiscard]] static std::expected<std::unique_ptr<DiscoveryRegistration>,
                                     DiscoveryError>
  create(const DiscoveryOptions &options, DiscoveryAdvertisement advertisement,
         bool backendAvailable, Handler handler);

  /** @brief Replace the internal synchronous notification sink. */
  void setHandler(Handler handler);

  /** @brief Report successful native registration for a live generation. */
  [[nodiscard]] DiscoveryResult established(std::uint64_t generation);

  /** @brief Report a name collision without renaming the advertisement. */
  [[nodiscard]] DiscoveryResult collision(std::uint64_t generation,
                                          std::string message);

  /** @brief Report a native registration failure. */
  [[nodiscard]] DiscoveryResult failed(std::uint64_t generation,
                                       std::string message);

  /** @brief Report a typed native registration failure. */
  [[nodiscard]] DiscoveryResult failed(std::uint64_t generation,
                                       DiscoveryError error);

  /** @brief Invalidate the current native generation after backend loss. */
  [[nodiscard]] DiscoveryResult backendLost();

  /** @brief Request registration again after backend recovery. */
  [[nodiscard]] DiscoveryResult backendAvailable();

  /** @brief Report a terminal backend failure, including while unavailable. */
  [[nodiscard]] DiscoveryResult backendFailed(DiscoveryError error);

  /** @brief Mark this registration stopped without emitting a notification. */
  [[nodiscard]] DiscoveryResult stop();

  /** @brief Latch an overflow without recursively delivering another event. */
  void markOverflow() noexcept;

  /** @brief Read overflow state without copying its diagnostic. */
  [[nodiscard]] bool overflowed() const noexcept;

  /** @brief Retain an internal delivery failure without emitting an event. */
  void markDeliveryFailure(DiscoveryError error);

  /** @brief Copy current status independently of callback delivery. */
  [[nodiscard]] DiscoveryRegistrationStatus status() const;

  /** @brief Read the owned advertisement. */
  [[nodiscard]] const DiscoveryAdvertisement &advertisement() const;

  /** @brief Read the current native generation token. */
  [[nodiscard]] std::uint64_t generation() const;

private:
  DiscoveryRegistration(DiscoveryAdvertisement advertisement,
                        bool backendAvailable, Handler handler);
  [[nodiscard]] DiscoveryResult
  validateGeneration(std::uint64_t generation) const;
  [[nodiscard]] DiscoveryResult
  transition(DiscoveryRegistrationState state,
             std::optional<DiscoveryError> error = {});

  DiscoveryAdvertisement m_advertisement;
  DiscoveryRegistrationStatus m_status;
  DiscoveryError m_overflow_error{DiscoveryErrorKind::kResourceLimit,
                                  "registration delivery overflow"};
  std::uint64_t m_generation{1};
  Handler m_handler;
};

} // namespace dmn::detail

#endif // DMN_DISCOVERY_REGISTRATION_HPP_
