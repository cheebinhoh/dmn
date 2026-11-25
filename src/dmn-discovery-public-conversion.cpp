/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-discovery-public-conversion.cpp
 * @brief Copy private discovery state into platform-neutral owned values.
 */

#include "dmn-discovery-public-conversion.hpp"
#include "dmn-discovery-records.hpp"

namespace dmn::detail {
namespace {

Dmn_DiscoveryServiceKey publicKey(const DiscoveryServiceKey &key) {
  return {key.m_instance, key.m_type, key.m_domain, key.m_interface};
}

Dmn_DiscoveryOperation publicOperation(DiscoveryAttempt attempt) {
  switch (attempt) {
  case DiscoveryAttempt::kSrv:
    return Dmn_DiscoveryOperation::ServiceRecord;
  case DiscoveryAttempt::kTxt:
    return Dmn_DiscoveryOperation::TextRecord;
  case DiscoveryAttempt::kIpv4:
    return Dmn_DiscoveryOperation::IPv4Address;
  case DiscoveryAttempt::kIpv6:
    return Dmn_DiscoveryOperation::IPv6Address;
  }

  std::terminate();
}

} // namespace

DiscoveryOptions discoveryOptions(const Dmn_DiscoveryOptions &options) {
  return {options.max_browsers,
          options.max_registrations,
          options.max_instances_per_browser,
          options.max_native_operations_per_browser,
          options.max_addresses_per_instance,
          options.max_txt_entries_per_instance,
          options.max_txt_bytes_per_instance,
          options.max_records_per_kind_per_instance,
          options.max_data_events_per_browser,
          options.max_registration_events,
          options.max_control_events_per_browser,
          options.max_queued_commands,
          options.refresh_timeout};
}

DiscoveryServiceKey discoveryKey(const Dmn_DiscoveryServiceKey &key) {
  return {key.instance, key.service_type, key.domain, key.interface_index};
}

DiscoveryAdvertisement
discoveryAdvertisement(const Dmn_DiscoveryAdvertisement &advertisement) {
  return {discoveryKey(advertisement.key), advertisement.port,
          advertisement.txt};
}

std::expected<std::array<bool, 2>, DiscoveryError>
discoveryQueryFamilies(const Dmn_DiscoveryQuery &query) {
  if (!validateServiceType(query.service_type) ||
      !validateDomain(query.domain) ||
      (query.interface_index && *query.interface_index == 0)) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "invalid discovery query"}};
  }

  std::array<bool, 2> families{false, false};
  for (const auto family : query.address_families) {
    std::size_t index;
    switch (family) {
    case Dmn_DiscoveryAddressFamily::IPv4:
      index = 0;
      break;
    case Dmn_DiscoveryAddressFamily::IPv6:
      index = 1;
      break;
    default:
      return std::unexpected{DiscoveryError{
          DiscoveryErrorKind::kInvalidArgument, "invalid address family"}};
    }

    if (families[index]) {
      return std::unexpected{DiscoveryError{
          DiscoveryErrorKind::kInvalidArgument, "duplicate address family"}};
    }

    families[index] = true;
  }

  if (!families[0] && !families[1]) {
    return std::unexpected{DiscoveryError{DiscoveryErrorKind::kInvalidArgument,
                                          "no address family requested"}};
  }

  return families;
}

Dmn_DiscoveryError discoveryPublicError(const DiscoveryError &error,
                                        Dmn_DiscoveryErrorKind backendKind) {
  Dmn_DiscoveryErrorKind kind{backendKind};
  switch (error.m_kind) {
  case DiscoveryErrorKind::kInvalidArgument:
    kind = Dmn_DiscoveryErrorKind::InvalidArgument;
    break;
  case DiscoveryErrorKind::kUnsupported:
    kind = Dmn_DiscoveryErrorKind::Unsupported;
    break;
  case DiscoveryErrorKind::kResourceLimit:
    kind = Dmn_DiscoveryErrorKind::ResourceLimit;
    break;
  case DiscoveryErrorKind::kBackend:
    break;
  case DiscoveryErrorKind::kStopped:
    kind = Dmn_DiscoveryErrorKind::Stopped;
    break;
  case DiscoveryErrorKind::kReentrantCall:
    kind = Dmn_DiscoveryErrorKind::ReentrantCall;
    break;
  case DiscoveryErrorKind::kCancelled:
    kind = Dmn_DiscoveryErrorKind::Cancelled;
    break;
  case DiscoveryErrorKind::kCallbackFailure:
    kind = Dmn_DiscoveryErrorKind::CallbackFailure;
    break;
  }

  if (error.m_kind == DiscoveryErrorKind::kBackend && error.m_native_category) {
    switch (*error.m_native_category) {
    case DiscoveryNativeErrorCategory::kUnavailable:
      kind = Dmn_DiscoveryErrorKind::BackendUnavailable;
      break;
    case DiscoveryNativeErrorCategory::kPermission:
      kind = Dmn_DiscoveryErrorKind::Permission;
      break;
    case DiscoveryNativeErrorCategory::kResourceLimit:
      kind = Dmn_DiscoveryErrorKind::ResourceLimit;
      break;
    case DiscoveryNativeErrorCategory::kUnsupported:
      kind = Dmn_DiscoveryErrorKind::Unsupported;
      break;
    case DiscoveryNativeErrorCategory::kCollision:
      kind = Dmn_DiscoveryErrorKind::Collision;
      break;
    }
  }

  return {kind, error.m_message, error.m_backend_domain, error.m_backend_code};
}

Dmn_DiscoveryValueResult<Dmn_DiscoveryCandidate>
discoveryPublicCandidate(const DiscoveryCandidate &candidate,
                         const DiscoveryOptions &options) {
  Dmn_DiscoveryCandidate result;
  result.key = publicKey(candidate.m_key);
  result.provenance = {
      candidate.m_provenance.m_cached, candidate.m_provenance.m_local,
      candidate.m_provenance.m_wide_area, candidate.m_provenance.m_multicast};
  switch (candidate.m_records.m_state) {
  case DiscoveryRecordState::kUnresolved:
    break;
  case DiscoveryRecordState::kResolving:
    result.resolution = Dmn_DiscoveryResolutionState::Resolving;
    result.record_state = Dmn_DiscoveryRecordState::Monitoring;
    break;
  case DiscoveryRecordState::kReady:
    result.resolution = Dmn_DiscoveryResolutionState::Resolved;
    result.record_state = Dmn_DiscoveryRecordState::Monitoring;
    break;
  case DiscoveryRecordState::kUnavailable:
    result.resolution = Dmn_DiscoveryResolutionState::Unavailable;
    result.record_state = Dmn_DiscoveryRecordState::Unavailable;
    break;
  case DiscoveryRecordState::kMonitorIncomplete:
    result.record_state = Dmn_DiscoveryRecordState::MonitoringIncomplete;
    break;
  case DiscoveryRecordState::kMultipleRecordsUnsupported:
    result.record_state = Dmn_DiscoveryRecordState::MultipleRecordsUnsupported;
    break;
  case DiscoveryRecordState::kGenerationExhausted:
    return std::unexpected{
        Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::ResourceLimit,
                           "discovery record generation exhausted",
                           {},
                           {}}};
  }

  if (candidate.m_records.m_srv) {
    const auto &srv{*candidate.m_records.m_srv};
    result.srv = Dmn_DiscoverySrvRecord{srv.m_priority, srv.m_weight,
                                        srv.m_port, srv.m_target};
    result.hostname = srv.m_target;
  }

  result.addresses.reserve(candidate.m_records.m_addresses.size());
  for (const auto &address : candidate.m_records.m_addresses) {
    result.addresses.push_back(
        {address.m_family == DiscoveryAddressFamily::kIpv4
             ? Dmn_DiscoveryAddressFamily::IPv4
             : Dmn_DiscoveryAddressFamily::IPv6,
         address.m_bytes, address.m_interface});
  }

  if (candidate.m_records.m_txt_wire) {
    auto parsed{parseTxtRecord(*candidate.m_records.m_txt_wire,
                               options.m_max_txt_entries,
                               options.m_max_txt_bytes)};
    if (!parsed) {
      return std::unexpected{discoveryPublicError(
          parsed.error(), Dmn_DiscoveryErrorKind::Internal)};
    }

    result.txt_state = Dmn_DiscoveryTxtState::Present;
    result.txt = std::move(*parsed);
  }

  if (candidate.m_refresh) {
    Dmn_DiscoveryRefreshState state{Dmn_DiscoveryRefreshState::Pending};
    if (candidate.m_refresh->m_state == DiscoveryRefreshState::kSucceeded) {
      state = Dmn_DiscoveryRefreshState::Succeeded;
    } else if (candidate.m_refresh->m_state == DiscoveryRefreshState::kFailed) {
      state = Dmn_DiscoveryRefreshState::Failed;
    }

    result.refresh =
        Dmn_DiscoveryRefreshStatus{candidate.m_refresh->m_id, state};
  }

  result.failures.reserve(candidate.m_failures.size());
  for (const auto &failure : candidate.m_failures) {
    const DiscoveryError error{
        DiscoveryErrorKind::kBackend, failure.m_error.m_message,
        failure.m_error.m_backend_domain, failure.m_error.m_backend_code,
        failure.m_error.m_native_category};
    result.failures.push_back(
        {publicOperation(failure.m_operation.m_attempt),
         discoveryPublicError(error, Dmn_DiscoveryErrorKind::Resolution),
         failure.m_error.m_recoverable});
  }

  return result;
}

Dmn_DiscoveryValueResult<Dmn_DiscoverySnapshot>
discoveryPublicSnapshot(const DiscoverySessionSnapshot &snapshot,
                        const DiscoveryOptions &options) {
  Dmn_DiscoverySnapshot result;
  result.epoch = snapshot.m_delivery.m_epoch;
  result.sequence = snapshot.m_delivery.m_sequence;
  if (snapshot.m_delivery.m_state == DiscoveryDeliveryState::kStopped &&
      snapshot.m_explicit_stop) {
    result.state = Dmn_DiscoveryBrowserState::Stopped;
  } else if (snapshot.m_delivery.m_state == DiscoveryDeliveryState::kOverflow) {
    result.state = Dmn_DiscoveryBrowserState::Overflow;
  } else if (snapshot.m_cleanup_error) {
    result.state = Dmn_DiscoveryBrowserState::Failed;
  } else if (snapshot.m_delivery.m_callback_failed) {
    result.state = Dmn_DiscoveryBrowserState::CallbackFailure;
  } else if (snapshot.m_delivery.m_state == DiscoveryDeliveryState::kStopped) {
    result.state = Dmn_DiscoveryBrowserState::Stopped;
  } else if (!snapshot.m_backend_available) {
    result.state = Dmn_DiscoveryBrowserState::BackendUnavailable;
  }

  if (snapshot.m_cleanup_error) {
    result.error = discoveryPublicError(*snapshot.m_cleanup_error,
                                        Dmn_DiscoveryErrorKind::Internal);
  } else if (snapshot.m_delivery.m_callback_failed) {
    result.error = Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::CallbackFailure,
                                      "discovery callback failed",
                                      {},
                                      {}};
  } else if (snapshot.m_delivery.m_overflowed) {
    result.error = Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::ResourceLimit,
                                      "discovery session overflow",
                                      {},
                                      {}};
  }

  result.candidates.reserve(snapshot.m_browser.m_candidates.size());
  for (const auto &candidate : snapshot.m_browser.m_candidates) {
    auto converted{discoveryPublicCandidate(candidate, options)};
    if (!converted) {
      return std::unexpected{converted.error()};
    }

    result.candidates.push_back(std::move(*converted));
  }

  return result;
}

Dmn_DiscoveryValueResult<Dmn_DiscoveryEvent>
discoveryPublicEvent(const DiscoveryDelivery &delivery,
                     const DiscoveryOptions &options) {
  Dmn_DiscoveryEvent result;
  result.epoch = delivery.m_epoch;
  result.sequence = delivery.m_sequence;
  if (delivery.m_data) {
    switch (delivery.m_data->m_kind) {
    case DiscoveryEventKind::kAdded:
      result.kind = Dmn_DiscoveryEventKind::Added;
      break;
    case DiscoveryEventKind::kChanged:
      result.kind = Dmn_DiscoveryEventKind::Changed;
      break;
    case DiscoveryEventKind::kRemoved:
      result.kind = Dmn_DiscoveryEventKind::Removed;
      break;
    }

    auto candidate{
        discoveryPublicCandidate(delivery.m_data->m_candidate, options)};
    if (!candidate) {
      return std::unexpected{candidate.error()};
    }

    result.key = candidate->key;
    result.candidate = std::move(*candidate);

    return result;
  }

  if (!delivery.m_control) {
    return std::unexpected{Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::Internal,
                                              "invalid discovery delivery",
                                              {},
                                              {}}};
  }

  switch (*delivery.m_control) {
  case DiscoveryControl::kReset:
    result.kind = Dmn_DiscoveryEventKind::Reset;
    break;
  case DiscoveryControl::kBackendAvailable:
    result.kind = Dmn_DiscoveryEventKind::BackendAvailable;
    break;
  case DiscoveryControl::kBackendUnavailable:
    result.kind = Dmn_DiscoveryEventKind::BackendUnavailable;
    result.browser_state = Dmn_DiscoveryBrowserState::BackendUnavailable;
    break;
  case DiscoveryControl::kOverflow:
    result.kind = Dmn_DiscoveryEventKind::Error;
    result.browser_state = Dmn_DiscoveryBrowserState::Overflow;
    result.error = Dmn_DiscoveryError{Dmn_DiscoveryErrorKind::ResourceLimit,
                                      "discovery session overflow",
                                      {},
                                      {}};
    break;
  }

  return result;
}

Dmn_DiscoveryRegistrationStatus
discoveryPublicRegistrationStatus(const DiscoveryRegistrationStatus &status) {
  Dmn_DiscoveryRegistrationStatus result;
  switch (status.m_state) {
  case DiscoveryRegistrationState::kRegistering:
    result.state = Dmn_DiscoveryRegistrationState::Registering;
    break;
  case DiscoveryRegistrationState::kEstablished:
    result.state = Dmn_DiscoveryRegistrationState::Established;
    break;
  case DiscoveryRegistrationState::kCollision:
    result.state = Dmn_DiscoveryRegistrationState::Collision;
    break;
  case DiscoveryRegistrationState::kUnavailable:
    result.state = Dmn_DiscoveryRegistrationState::Unavailable;
    break;
  case DiscoveryRegistrationState::kFailed:
    result.state = Dmn_DiscoveryRegistrationState::Failed;
    break;
  case DiscoveryRegistrationState::kOverflow:
    result.state = Dmn_DiscoveryRegistrationState::Overflow;
    break;
  case DiscoveryRegistrationState::kStopped:
    result.state = Dmn_DiscoveryRegistrationState::Stopped;
    break;
  }

  if (status.m_callback_failed && !status.m_explicit_stop) {
    result.state = status.m_error && status.m_error->m_kind ==
                                         DiscoveryErrorKind::kCallbackFailure
                       ? Dmn_DiscoveryRegistrationState::CallbackFailure
                       : Dmn_DiscoveryRegistrationState::Failed;
  }

  if (status.m_error) {
    const auto fallback{status.m_state == DiscoveryRegistrationState::kCollision
                            ? Dmn_DiscoveryErrorKind::Collision
                        : status.m_state ==
                                DiscoveryRegistrationState::kUnavailable
                            ? Dmn_DiscoveryErrorKind::BackendUnavailable
                            : Dmn_DiscoveryErrorKind::Internal};
    result.error = discoveryPublicError(*status.m_error, fallback);
  }

  return result;
}

} // namespace dmn::detail
