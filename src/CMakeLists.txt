# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

# build the dmn library
message(STATUS "build dmn...")
include(CMakePackageConfigHelpers)

# Keep public header sources visible in the build tree without exporting
# absolute source/build paths to installed consumers.
function(dmn_add_build_interface_sources target)
  foreach(source IN LISTS ARGN)
    target_sources(${target} INTERFACE "$<BUILD_INTERFACE:${source}>")
  endforeach()
endfunction()

add_library(dmn
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-interval-btree.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-proc.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-runtime-state.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-runtime.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-socket.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-state.cpp
)

# The alias is shared by in-tree consumers and the installed Dmn:: namespace.
add_library(Dmn::dmn ALIAS dmn)
target_compile_features(dmn PUBLIC cxx_std_23)

if (ENABLE_DMN_DISCOVERY)
  find_package(Threads REQUIRED)
  target_sources(dmn PRIVATE
    dmn-discovery.cpp
    dmn-discovery-public-conversion.cpp
    dmn-discovery-command.cpp
    dmn-discovery-core.cpp
    dmn-discovery-delivery.cpp
    dmn-discovery-dispatcher.cpp
    dmn-discovery-records.cpp
    dmn-discovery-registration.cpp
    dmn-discovery-retry.cpp
    dmn-discovery-session.cpp
    dmn-discovery-slots.cpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-command.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-core.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-delivery.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-dispatcher.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-public-conversion.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-records.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-registration.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-retry.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-session.hpp
    ${PROJECT_SOURCE_DIR}/include/dmn-discovery-slots.hpp)
  target_link_libraries(dmn PRIVATE Threads::Threads)
  target_compile_definitions(dmn PUBLIC DMN_ENABLE_DMN_DISCOVERY=1)
  dmn_add_build_interface_sources(dmn
    "${PROJECT_SOURCE_DIR}/include/dmn-discovery.hpp")

  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(dmn PRIVATE
      dmn-discovery-avahi.cpp
      dmn-discovery-avahi-browser.cpp
      dmn-discovery-avahi-contribution.cpp
      dmn-discovery-avahi-mapping.cpp
      dmn-discovery-avahi-record-watch.cpp
      dmn-discovery-avahi-registration.cpp
      dmn-discovery-avahi-registration-session.cpp
      dmn-discovery-avahi-resolver.cpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-browser.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-contribution.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-error.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-mapping.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-record-watch.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-registration-session.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-registration.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi-resolver.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-avahi.hpp)
    target_link_libraries(dmn PRIVATE "${DMN_AVAHI_TARGET}")
    dmn_install_avahi("${CMAKE_INSTALL_LIBDIR}/cmake/Dmn")
  else()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES dns_sd)
    check_cxx_source_compiles("#include <dns_sd.h>
int main() { return DNSServiceRefSockFD(nullptr); }" DMN_HAVE_BONJOUR)
    cmake_pop_check_state()
    if (NOT DMN_HAVE_BONJOUR)
      message(FATAL_ERROR "The macOS DNS-SD headers and library are required.")
    endif()
    target_sources(dmn PRIVATE
      dmn-discovery-bonjour-address.cpp
      dmn-discovery-bonjour-browser.cpp
      dmn-discovery-bonjour-contribution.cpp
      dmn-discovery-bonjour-mapping.cpp
      dmn-discovery-bonjour-native.cpp
      dmn-discovery-bonjour-operation.cpp
      dmn-discovery-bonjour-poll.cpp
      dmn-discovery-bonjour-record.cpp
      dmn-discovery-bonjour-registration.cpp
      dmn-discovery-bonjour-sessions.cpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-address.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-browser.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-contribution.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-mapping.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-operation.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-poll.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-record.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-registration.hpp
      ${PROJECT_SOURCE_DIR}/include/dmn-discovery-bonjour-sessions.hpp)
    target_compile_definitions(dmn PRIVATE DMN_DISCOVERY_USE_BONJOUR=1)
    target_link_libraries(dmn PRIVATE dns_sd)
  endif()
endif()

if (ENABLE_DMN_DMESGNET)
  target_sources(dmn
                   PRIVATE
                   ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesg.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesgnet.cpp
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dlock.pb.cc
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.cc
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.cc
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.cc
  )

  GENERATE_PROTOBUF(dmn
                      dmn-dlock.proto
                      dmn-dmesg-body.proto
                      dmn-dmesg-type.proto
                      dmn-dmesg.proto
  )
endif()

if (ENABLE_DMN_DMESGNET)
  target_sources(dmn
                   PRIVATE
                   ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-dmesgnet-kafka.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka-util.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka.cpp
  )
  target_compile_definitions(dmn PUBLIC DMN_ENABLE_DMN_DMESGNET=1)
  if (TARGET ${DMN_RDKAFKA_TARGET} AND
      DMN_RDKAFKA_TARGET STREQUAL "rdkafka")
    add_dependencies(dmn rdkafka)
  endif()
endif()

target_compile_options(dmn PRIVATE -Wall -Wextra -Wpedantic)

set_target_properties(dmn PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The GATT API is a backend-neutral contract shared by ANCS and BlueZ.
add_library(dmn-gatt INTERFACE)
add_library(Dmn::dmn-gatt ALIAS dmn-gatt)
target_compile_features(dmn-gatt INTERFACE cxx_std_23)

target_include_directories(dmn-gatt
                             INTERFACE
                             $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
)

dmn_add_build_interface_sources(dmn-gatt
                                  ${PROJECT_SOURCE_DIR}/include/dmn-gatt-client.hpp
)

if (ENABLE_ANCS)
  add_library(dmn-ancs
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-ancs-client.cpp
  )

  add_library(Dmn::dmn-ancs ALIAS dmn-ancs)

  target_include_directories(dmn-ancs
                               PUBLIC
                               $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
  )

  dmn_add_build_interface_sources(dmn-ancs
                                    ${PROJECT_SOURCE_DIR}/include/dmn-ancs-client.hpp
  )

  target_compile_options(dmn-ancs PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(dmn-ancs PUBLIC DMN_ENABLE_ANCS=1)
  target_link_libraries(dmn-ancs PUBLIC dmn dmn-gatt)

  install(TARGETS dmn-ancs
            EXPORT DmnTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Runtime
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
  )
endif()

# Public core headers: consumers see these through the dmn target interface.
dmn_add_build_interface_sources(dmn
                 ${PROJECT_SOURCE_DIR}/include/dmn-async.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue-lf.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue-mt.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-debug.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-inflight-guard.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-interval-btree.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-io.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-pipe.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-proc.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-pub-sub.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime-state.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime-task.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-singleton.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-socket.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-state.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-timer.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-util.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn.hpp
)

if (ENABLE_DMN_DMESGNET)
  dmn_add_build_interface_sources(dmn
                   ${PROJECT_SOURCE_DIR}/include/dmn-dlock.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dmesg-pb-util.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dmesg.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dmesgnet.hpp
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dlock.pb.h
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.h
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.h
                   ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.h
  )
  dmn_add_build_interface_sources(dmn
                   ${PROJECT_SOURCE_DIR}/include/kafka/dmn-dmesgnet-kafka.hpp
                   ${PROJECT_SOURCE_DIR}/include/kafka/dmn-kafka-util.hpp
                   ${PROJECT_SOURCE_DIR}/include/kafka/dmn-kafka.hpp
  )
endif()

if (ENABLE_FAULT_INJECTION)
  dmn_add_build_interface_sources(dmn
                   ${PROJECT_SOURCE_DIR}/include/dmn-fault-injection.hpp
  )
endif()

target_include_directories(dmn
                             PUBLIC
                             $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                             $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
                             $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
                             $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                             $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
)

if (ENABLE_DMN_DMESGNET AND DEFINED protobuf_SOURCE_DIR)
  target_include_directories(dmn
                               INTERFACE
                               "$<BUILD_INTERFACE:${protobuf_SOURCE_DIR}/src>"
  )
endif()

if (DEFINED absl_SOURCE_DIR)
  target_include_directories(dmn
                               INTERFACE "$<BUILD_INTERFACE:${absl_SOURCE_DIR}>"
  )
endif()

if (ENABLE_DMN_DMESGNET AND DMN_RDKAFKA_INCLUDE_DIR)
  target_include_directories(dmn
                               PUBLIC "$<BUILD_INTERFACE:${DMN_RDKAFKA_INCLUDE_DIR}>"
  )
endif()

if(ENABLE_FAULT_INJECTION)
  # Expose libfiu's enabled failure-point API to Dmn and its test executables.
  target_compile_definitions(dmn
                               PUBLIC
                               FIU_ENABLE=1
  )

  target_link_libraries(dmn
                          PUBLIC
                          "$<BUILD_INTERFACE:${DMN_LIBFIU_TARGET}>"
                          "$<INSTALL_INTERFACE:PkgConfig::LIBFIU>"
  )

  if (TARGET libfiu-build)
    add_dependencies(dmn libfiu-build)
  endif()
endif()

# Exported link interfaces use package targets; the build interface may instead
# use dependencies fetched into this build tree.
target_link_libraries(dmn
                        PUBLIC
                        "$<BUILD_INTERFACE:ICU::uc>"
                        "$<INSTALL_INTERFACE:ICU::uc>"
)

if (ENABLE_DMN_DMESGNET)
  target_link_libraries(dmn
                          PUBLIC
                          "$<BUILD_INTERFACE:protobuf::libprotobuf>"
                          "$<INSTALL_INTERFACE:protobuf::libprotobuf>"
                          "$<BUILD_INTERFACE:${DMN_RDKAFKA_TARGET}>"
                          "$<INSTALL_INTERFACE:PkgConfig::RDKAFKA>"
  )
endif()
if (NOT APPLE)
  target_link_libraries(dmn
                          PUBLIC
                          "$<BUILD_INTERFACE:atomic>"
                          "$<INSTALL_INTERFACE:atomic>"
  )
endif()

if (ENABLE_DBUS)
  add_library(dmn-dbus
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-client.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-io.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-runtime.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-value.cpp
  )

  add_library(Dmn::dmn-dbus ALIAS dmn-dbus)

  if (ENABLE_DMN_DMESGNET)
    target_sources(dmn-dbus
                     PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesgnet-dbus.cpp
    )

    dmn_add_build_interface_sources(dmn-dbus
                                      ${PROJECT_SOURCE_DIR}/include/dmn-dmesgnet-dbus.hpp
    )
  endif()

  target_include_directories(dmn-dbus
                               PUBLIC
                               $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
  )

  # Public optional API; the implementation-only runtime header stays PRIVATE.
  dmn_add_build_interface_sources(dmn-dbus
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-config.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-io.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-value.hpp
  )

  # Internal implementation header shared only by this target's .cpp files.
  target_sources(dmn-dbus
                   PRIVATE ${PROJECT_SOURCE_DIR}/include/dmn-dbus-runtime.hpp
  )

  target_compile_options(dmn-dbus PRIVATE -Wall -Wextra -Wpedantic)

  target_compile_definitions(dmn-dbus PUBLIC DMN_ENABLE_DBUS=1)

  target_link_libraries(dmn-dbus
                          PUBLIC dmn
                          PRIVATE
                          "$<BUILD_INTERFACE:PkgConfig::DBUS>"
                          "$<INSTALL_INTERFACE:PkgConfig::DBUS>"
  )

  install(TARGETS dmn-dbus
            EXPORT DmnTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Runtime
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
  )
endif()

if (ENABLE_BLUEZ)
  add_library(dmn-bluez
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-bluez-client.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-bluez-gatt-client.cpp
  )

  add_library(Dmn::dmn-bluez ALIAS dmn-bluez)

  target_include_directories(dmn-bluez
                               PUBLIC
                               $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
  )

  dmn_add_build_interface_sources(dmn-bluez
                   ${PROJECT_SOURCE_DIR}/include/dmn-bluez-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-bluez-gatt-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-gatt-client.hpp
  )

  target_compile_definitions(dmn-bluez PUBLIC DMN_ENABLE_BLUEZ=1)
  target_compile_options(dmn-bluez PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(dmn-bluez PUBLIC dmn-dbus dmn-gatt)

  install(TARGETS dmn-bluez
            EXPORT DmnTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Runtime
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
  )
endif()

if (ENABLE_ANCS AND ENABLE_BLUEZ)
  add_library(dmn-ancs-bluez-monitor
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-ancs-bluez-monitor.cpp
  )

  add_library(Dmn::dmn-ancs-bluez-monitor ALIAS dmn-ancs-bluez-monitor)

  target_include_directories(dmn-ancs-bluez-monitor
                               PUBLIC
                               $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/dmn>
  )

  dmn_add_build_interface_sources(dmn-ancs-bluez-monitor
                   ${PROJECT_SOURCE_DIR}/include/dmn-ancs-bluez-monitor.hpp
  )

  target_compile_options(dmn-ancs-bluez-monitor
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-ancs-bluez-monitor
                          PUBLIC dmn-ancs dmn-bluez
  )

  install(TARGETS dmn-ancs-bluez-monitor
            EXPORT DmnTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
            COMPONENT Runtime
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
  )
endif()

# Export the library targets as one feature-conditioned package. The
# DmnConfig.cmake template resolves only the external dependencies used by
# targets included in this build.
install(TARGETS dmn
          EXPORT DmnTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
          COMPONENT Development
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/dmn
          COMPONENT Runtime
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT Runtime
)

# Export the backend-neutral API even when no ANCS or BlueZ backend is enabled.
install(TARGETS dmn-gatt
          EXPORT DmnTargets
)

# Install the public API headers for the enabled feature set.
set(DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS)

list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
       PATTERN "dmn-dbus-runtime.hpp" EXCLUDE
       PATTERN "dmn-discovery-*.hpp" EXCLUDE
)

if (NOT ENABLE_DMN_DMESGNET)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-dlock.hpp" EXCLUDE
         PATTERN "dmn-dmesg-pb-util.hpp" EXCLUDE
         PATTERN "dmn-dmesg.hpp" EXCLUDE
         PATTERN "dmn-dmesgnet.hpp" EXCLUDE
         PATTERN "dmn-dmesgnet-dbus.hpp" EXCLUDE
         PATTERN "kafka" EXCLUDE
  )
endif()

if (NOT ENABLE_ANCS)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-ancs-client.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_ANCS OR NOT ENABLE_BLUEZ)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-ancs-bluez-monitor.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_DBUS)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-dbus-*.hpp" EXCLUDE
         PATTERN "dmn-dmesgnet-dbus.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_BLUEZ)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-bluez-*.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_FAULT_INJECTION)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-fault-injection.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_DMN_DISCOVERY)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-discovery.hpp" EXCLUDE
  )
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dmn
          COMPONENT Development
          ${DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS}
)

if (ENABLE_DMN_DMESGNET)
  # Public DMesg headers include generated protobuf headers under proto/.
  install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dlock.pb.h
            ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.h
            ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.h
            ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dmn/proto
          COMPONENT Development
  )
endif()

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/DmnConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/DmnConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Dmn"
)

write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/DmnConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(EXPORT DmnTargets
          FILE DmnTargets.cmake
          NAMESPACE Dmn::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Dmn
          COMPONENT Development
)

install(FILES
          "${PROJECT_BINARY_DIR}/DmnConfig.cmake"
          "${PROJECT_BINARY_DIR}/DmnConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Dmn
        COMPONENT Development
)

# Application entry points are maintained separately from reusable libraries.
add_subdirectory(app)
