/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-registration.cpp
 * @brief Private advertisement validation and lifecycle model.
 */

#include "dmn-discovery-registration.hpp"

#include <limits>
#include <utility>

namespace dmn::detail {

DiscoveryRegistration::DiscoveryRegistration(
    DiscoveryAdvertisement advertisement, bool backendAvailable,
    Handler handler)
    : m_advertisement{std::move(advertisement)},
      m_status{backendAvailable ? DiscoveryRegistrationState::kRegistering
                                : DiscoveryRegistrationState::kUnavailable,
               {}},
      m_handler{std::move(handler)} {}

DiscoveryResult
validateAdvertisement(const DiscoveryOptions &options,
                      const DiscoveryAdvertisement &advertisement) {
  if (auto result{validateOptions(options)}; !result) {
    return std::unexpected{result.error()};
  }

  const auto &key{advertisement.m_key};
  if (!validateInstanceName(key.m_instance) ||
      !validateServiceType(key.m_type) || !validateDomain(key.m_domain) ||
      (key.m_interface && *key.m_interface == 0) || advertisement.m_port == 0 ||
      !validateTxt(advertisement.m_txt, options.m_max_txt_entries,
                   options.m_max_txt_bytes)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid discovery advertisement"}};
  }

  return {};
}

std::expected<std::unique_ptr<DiscoveryRegistration>, DiscoveryError>
DiscoveryRegistration::create(const DiscoveryOptions &options,
                              DiscoveryAdvertisement advertisement,
                              bool backendAvailable, Handler handler) {
  if (auto result{validateAdvertisement(options, advertisement)}; !result) {
    return std::unexpected{result.error()};
  }

  return std::unique_ptr<DiscoveryRegistration>{new DiscoveryRegistration{
      std::move(advertisement), backendAvailable, std::move(handler)}};
}

void DiscoveryRegistration::setHandler(Handler handler) {
  m_handler = std::move(handler);
}

DiscoveryResult
DiscoveryRegistration::validateGeneration(std::uint64_t generation) const {
  if (generation != m_generation ||
      (m_status.m_state != DiscoveryRegistrationState::kRegistering &&
       m_status.m_state != DiscoveryRegistrationState::kEstablished)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kBackend,
                                          "stale registration callback"}};
  }

  return {};
}

DiscoveryResult
DiscoveryRegistration::transition(DiscoveryRegistrationState state,
                                  std::optional<DiscoveryError> error) {
  m_status = {state, std::move(error)};
  try {
    if (m_handler) {
      m_handler(m_status);
    }
  } catch (const std::bad_alloc &) {
    markOverflow();

    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kResourceLimit, {}}};
  }

  return {};
}

DiscoveryResult DiscoveryRegistration::established(std::uint64_t generation) {
  if (auto result{validateGeneration(generation)}; !result) {
    return result;
  }

  if (m_status.m_state != DiscoveryRegistrationState::kEstablished) {
    return transition(DiscoveryRegistrationState::kEstablished);
  }

  return {};
}

DiscoveryResult DiscoveryRegistration::collision(std::uint64_t generation,
                                                 std::string message) {
  if (auto result{validateGeneration(generation)}; !result) {
    return result;
  }

  return transition(
      DiscoveryRegistrationState::kCollision,
      DiscoveryError{DiscoveryErrorKind::kBackend, std::move(message)});
}

DiscoveryResult DiscoveryRegistration::failed(std::uint64_t generation,
                                              std::string message) {
  return failed(generation, DiscoveryError{DiscoveryErrorKind::kBackend,
                                           std::move(message)});
}

DiscoveryResult DiscoveryRegistration::failed(std::uint64_t generation,
                                              DiscoveryError error) {
  if (auto result{validateGeneration(generation)}; !result) {
    return result;
  }

  return transition(DiscoveryRegistrationState::kFailed, std::move(error));
}

DiscoveryResult DiscoveryRegistration::backendLost() {
  if (m_status.m_state == DiscoveryRegistrationState::kStopped ||
      m_status.m_state == DiscoveryRegistrationState::kOverflow) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "registration stopped"}};
  }

  if (m_status.m_state == DiscoveryRegistrationState::kUnavailable) {
    return {};
  }

  if (m_generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kResourceLimit,
                                          "registration generation exhausted"}};
  }

  ++m_generation;

  return transition(DiscoveryRegistrationState::kUnavailable);
}

DiscoveryResult DiscoveryRegistration::backendAvailable() {
  if (m_status.m_state == DiscoveryRegistrationState::kStopped ||
      m_status.m_state == DiscoveryRegistrationState::kOverflow) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "registration stopped"}};
  }

  if (m_status.m_state != DiscoveryRegistrationState::kUnavailable) {
    return {};
  }

  return transition(DiscoveryRegistrationState::kRegistering);
}

DiscoveryResult DiscoveryRegistration::backendFailed(DiscoveryError error) {
  if (m_status.m_state == DiscoveryRegistrationState::kStopped ||
      m_status.m_state == DiscoveryRegistrationState::kOverflow) {
    return std::unexpected{
        DiscoveryError{DiscoveryErrorKind::kBackend, "registration stopped"}};
  }

  return transition(DiscoveryRegistrationState::kFailed, std::move(error));
}

DiscoveryResult DiscoveryRegistration::stop() {
  m_status.m_state = DiscoveryRegistrationState::kStopped;

  return {};
}

void DiscoveryRegistration::markOverflow() noexcept {
  if (m_status.m_state != DiscoveryRegistrationState::kOverflow) {
    m_status.m_state = DiscoveryRegistrationState::kOverflow;
    m_status.m_error = std::move(m_overflow_error);
  }
}

bool DiscoveryRegistration::overflowed() const noexcept {
  return m_status.m_state == DiscoveryRegistrationState::kOverflow;
}

void DiscoveryRegistration::markDeliveryFailure(DiscoveryError error) {
  m_status = {DiscoveryRegistrationState::kFailed, std::move(error)};
}

DiscoveryRegistrationStatus DiscoveryRegistration::status() const {
  return m_status;
}

const DiscoveryAdvertisement &DiscoveryRegistration::advertisement() const {
  return m_advertisement;
}

std::uint64_t DiscoveryRegistration::generation() const { return m_generation; }

} // namespace dmn::detail
