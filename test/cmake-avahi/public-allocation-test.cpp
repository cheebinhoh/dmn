/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file public-allocation-test.cpp
 * @brief Verify public allocation errors when allocation remains unavailable.
 */

#include "dmn-discovery-session.hpp"
#include <dmn-discovery.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

namespace {
thread_local bool failAllocation{false};
thread_local std::size_t failAllocationSize{0};
} // namespace

void *operator new(std::size_t size) {
  if (failAllocation || (failAllocationSize && size == failAllocationSize)) {
    throw std::bad_alloc{};
  }
  if (void *memory{std::malloc(size ? size : 1)}) {
    return memory;
  }
  throw std::bad_alloc{};
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {
using namespace dmn::detail;

bool conversionErrorSurvivesAllocationFailure(bool registration) {
  constexpr std::size_t messageSize{2000};
  DiscoverySessionClient client{{}};
  const auto failConversion = [] {
    DiscoveryError error{DiscoveryErrorKind::kResourceLimit,
                         std::string(messageSize, 'x')};
    failAllocationSize = messageSize + 1;
    throw DiscoveryDeliveryFailure{std::move(error)};
  };
  if (registration) {
    auto handle{client.advertise(
        {{"Printer", "_ipp._tcp", "local.", 7}, 8080, {}},
        [&](DiscoveryRegistrationStatus status) {
          if (status.m_state == DiscoveryRegistrationState::kEstablished) {
            failConversion();
          }
        })};
    if (!handle || !handle->established(handle->generation())) {
      return false;
    }
    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::seconds{3}};
    while (std::chrono::steady_clock::now() < deadline) {
      auto status{handle->status()};
      if (status.m_callback_failed) {
        return status.m_error &&
               status.m_error->m_kind == DiscoveryErrorKind::kResourceLimit &&
               status.m_error->m_message.size() == messageSize;
      }
      std::this_thread::yield();
    }
    return false;
  }

  auto handle{client.browse([&](DiscoveryDelivery) { failConversion(); })};
  if (!handle || !handle->observe(handle->snapshot().m_delivery.m_epoch,
                                  {"Printer", "_ipp._tcp", "local.", 7},
                                  DiscoveryContribution::kIpv4)) {
    return false;
  }
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{3}};
  while (std::chrono::steady_clock::now() < deadline) {
    auto snapshot{handle->snapshot()};
    if (snapshot.m_delivery.m_callback_failed) {
      return snapshot.m_cleanup_error &&
             snapshot.m_cleanup_error->m_kind ==
                 DiscoveryErrorKind::kResourceLimit &&
             snapshot.m_cleanup_error->m_message.size() == messageSize;
    }
    std::this_thread::yield();
  }
  return false;
}

bool commandAllocationFailurePreservesWorker() {
  DiscoveryCommandWorker worker{2, 2};
  auto failed{worker.execute([]() -> DiscoveryResult {
    failAllocation = true;
    throw std::bad_alloc{};
  })};
  auto next{worker.execute([]() -> DiscoveryResult {
    failAllocation = false;
    return {};
  })};
  return !failed &&
         failed.error().m_kind == DiscoveryErrorKind::kResourceLimit &&
         next.has_value() && worker.shutdown().has_value();
}
} // namespace

int main() {
  bool resourceLimit{false};
  bool threw{false};
  failAllocation = true;
  try {
    auto result{dmn::Dmn_DiscoveryClient::create()};
    resourceLimit = !result && result.error().kind ==
                                   dmn::Dmn_DiscoveryErrorKind::ResourceLimit;
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  failAllocation = false;
  if (threw || !resourceLimit) {
    std::cerr << "public allocation failure did not produce a resource-limit "
                 "result\n";
    return 1;
  }
  const bool browser{conversionErrorSurvivesAllocationFailure(false)};
  const bool registration{conversionErrorSurvivesAllocationFailure(true)};
  const bool command{commandAllocationFailurePreservesWorker()};
  if (!browser || !registration || !command) {
    std::cerr << "allocation failure handling failed: browser=" << browser
              << ", registration=" << registration << ", command=" << command
              << '\n';
    return 1;
  }
  return 0;
}
