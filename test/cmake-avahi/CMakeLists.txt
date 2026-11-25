# Copyright © 2026 Chee Bin HOH. All rights reserved.

cmake_minimum_required(VERSION 3.26)
project(dmn_avahi_provider_smoke LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DMN_ALLOW_DEPENDENCY_FETCH ON CACHE BOOL "Allow source downloads")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DmnAvahi.cmake")
if (NOT TARGET DmnAvahiInternal::client)
  dmn_prepare_avahi()
endif()
if (DMN_EXPECT_AVAHI_PROVIDER AND
    NOT DMN_AVAHI_SELECTED_PROVIDER STREQUAL DMN_EXPECT_AVAHI_PROVIDER)
  message(FATAL_ERROR "Unexpected Avahi provider: ${DMN_AVAHI_SELECTED_PROVIDER}")
endif()

add_executable(dmn-avahi-provider-smoke main.cpp)
target_link_libraries(dmn-avahi-provider-smoke PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-provider-smoke PRIVATE -Wall -Wextra -Wpedantic)
enable_testing()
add_test(NAME dmn-avahi-provider-smoke COMMAND dmn-avahi-provider-smoke)
set_tests_properties(dmn-avahi-provider-smoke PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

option(DMN_AVAHI_PACKAGE_FIXTURE "Enable the private relocatable package fixture" OFF)
if (DMN_AVAHI_PACKAGE_FIXTURE)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  add_library(dmn-avahi-package-fixture STATIC package/fixture.cpp)
  target_link_libraries(dmn-avahi-package-fixture PRIVATE "${DMN_AVAHI_TARGET}")
  set(_fixture_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/AvahiFixture")
  dmn_install_avahi("${_fixture_config_dir}")
  install(TARGETS dmn-avahi-package-fixture EXPORT AvahiFixtureTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development)
  install(EXPORT AvahiFixtureTargets NAMESPACE AvahiFixture::
          DESTINATION "${_fixture_config_dir}" COMPONENT Development)
  configure_package_config_file(package/AvahiFixtureConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AvahiFixtureConfig.cmake"
    INSTALL_DESTINATION "${_fixture_config_dir}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AvahiFixtureConfig.cmake"
          DESTINATION "${_fixture_config_dir}" COMPONENT Development)
  if (ENABLE_VALGRIND)
    find_program(VALGRIND_EXECUTABLE valgrind REQUIRED)
  endif()
  add_test(NAME dmn-avahi-package-relocation
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DFIXTURE_DIR=${CMAKE_CURRENT_LIST_DIR}/package"
      "-DPROVIDER=${DMN_AVAHI_SELECTED_PROVIDER}"
      "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DDATADIR=${CMAKE_INSTALL_DATADIR}"
      "-DCONFIG=$<CONFIG>"
      "-DVALGRIND=${VALGRIND_EXECUTABLE}"
      -P "${CMAKE_CURRENT_LIST_DIR}/package/relocate.cmake")
  set_tests_properties(dmn-avahi-package-relocation PROPERTIES
    LABELS "dmn;dmn-discovery;dmn-avahi;packaging" TIMEOUT 120)
  if (DMN_AVAHI_SELECTED_PROVIDER STREQUAL "SOURCE")
    add_test(NAME dmn-avahi-package-system-static
      COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DFIXTURE_DIR=${CMAKE_CURRENT_LIST_DIR}"
        "-DCONFIG=$<CONFIG>"
        "-DENABLE_VALGRIND=${ENABLE_VALGRIND}"
        -P "${CMAKE_CURRENT_LIST_DIR}/package/system-static.cmake")
    set_tests_properties(dmn-avahi-package-system-static PROPERTIES
      LABELS "dmn;dmn-discovery;dmn-avahi;packaging" TIMEOUT 120)
  endif()
endif()

find_package(Threads REQUIRED)
add_executable(dmn-avahi-poll-test
  poll-test.cpp
  ../../src/dmn-discovery-avahi.cpp
  ../../src/dmn-discovery-avahi-browser.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp
  ../../src/dmn-discovery-command.cpp)
target_include_directories(dmn-avahi-poll-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-poll-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
target_compile_options(dmn-avahi-poll-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-poll-test COMMAND dmn-avahi-poll-test)
set_tests_properties(dmn-avahi-poll-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-mapping-test
  mapping-test.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp)
target_include_directories(dmn-avahi-mapping-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-mapping-test PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-mapping-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-mapping-test COMMAND dmn-avahi-mapping-test)
set_tests_properties(dmn-avahi-mapping-test PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-browser-test
  browser-test.cpp
  ../../src/dmn-discovery-avahi-browser.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp)
target_include_directories(dmn-avahi-browser-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-browser-test PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-browser-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-browser-test COMMAND dmn-avahi-browser-test)
set_tests_properties(dmn-avahi-browser-test PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-session-integration-test
  session-integration-test.cpp
  ../../src/dmn-discovery-avahi.cpp
  ../../src/dmn-discovery-avahi-browser.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-command.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-delivery.cpp
  ../../src/dmn-discovery-dispatcher.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-registration.cpp
  ../../src/dmn-discovery-retry.cpp
  ../../src/dmn-discovery-session.cpp
  ../../src/dmn-discovery-slots.cpp)
target_include_directories(dmn-avahi-session-integration-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-session-integration-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
target_compile_options(dmn-avahi-session-integration-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-session-integration-test COMMAND dmn-avahi-session-integration-test)
set_tests_properties(dmn-avahi-session-integration-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-record-watch-test
  record-watch-test.cpp
  ../../src/dmn-discovery-avahi-record-watch.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp)
target_include_directories(dmn-avahi-record-watch-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-record-watch-test PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-record-watch-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-record-watch-test COMMAND dmn-avahi-record-watch-test)
set_tests_properties(dmn-avahi-record-watch-test PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-resolver-test
  resolver-test.cpp
  ../../src/dmn-discovery-avahi-resolver.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp)
target_include_directories(dmn-avahi-resolver-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-resolver-test PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-resolver-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-resolver-test COMMAND dmn-avahi-resolver-test)
set_tests_properties(dmn-avahi-resolver-test PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-registration-test
  registration-test.cpp
  ../../src/dmn-discovery-avahi-registration.cpp
  ../../src/dmn-discovery-registration.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-retry.cpp)
target_include_directories(dmn-avahi-registration-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-registration-test PRIVATE "${DMN_AVAHI_TARGET}")
target_compile_options(dmn-avahi-registration-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-registration-test COMMAND dmn-avahi-registration-test)
set_tests_properties(dmn-avahi-registration-test PROPERTIES LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-registration-session-test
  registration-session-test.cpp
  ../../src/dmn-discovery-avahi-registration-session.cpp
  ../../src/dmn-discovery-avahi-registration.cpp
  ../../src/dmn-discovery-avahi-contribution.cpp
  ../../src/dmn-discovery-avahi-browser.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-avahi-record-watch.cpp
  ../../src/dmn-discovery-avahi-resolver.cpp
  ../../src/dmn-discovery-avahi.cpp
  ../../src/dmn-discovery-command.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-delivery.cpp
  ../../src/dmn-discovery-dispatcher.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-registration.cpp
  ../../src/dmn-discovery-retry.cpp
  ../../src/dmn-discovery-session.cpp
  ../../src/dmn-discovery-slots.cpp)
target_include_directories(dmn-avahi-registration-session-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-registration-session-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
target_compile_options(dmn-avahi-registration-session-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-registration-session-test COMMAND dmn-avahi-registration-session-test)
set_tests_properties(dmn-avahi-registration-session-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")

set(_avahi_unified_sources
  ../../src/dmn-discovery-avahi-registration-session.cpp
  ../../src/dmn-discovery-avahi-registration.cpp
  ../../src/dmn-discovery-avahi-contribution.cpp
  ../../src/dmn-discovery-avahi-browser.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-avahi-record-watch.cpp
  ../../src/dmn-discovery-avahi-resolver.cpp
  ../../src/dmn-discovery-avahi.cpp
  ../../src/dmn-discovery-command.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-delivery.cpp
  ../../src/dmn-discovery-dispatcher.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-registration.cpp
  ../../src/dmn-discovery-retry.cpp
  ../../src/dmn-discovery-session.cpp
  ../../src/dmn-discovery-slots.cpp)
add_executable(dmn-avahi-unified-session-test
  unified-session-test.cpp
  ${_avahi_unified_sources})
target_include_directories(dmn-avahi-unified-session-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-unified-session-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
if (ENABLE_FAULT_INJECTION AND DMN_LIBFIU_TARGET)
  target_compile_definitions(dmn-avahi-unified-session-test PRIVATE FIU_ENABLE=1)
  target_link_libraries(dmn-avahi-unified-session-test PRIVATE "${DMN_LIBFIU_TARGET}")
  if (TARGET libfiu-build)
    add_dependencies(dmn-avahi-unified-session-test libfiu-build)
  endif()
endif()
target_compile_options(dmn-avahi-unified-session-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-unified-session-test COMMAND dmn-avahi-unified-session-test)
set_tests_properties(dmn-avahi-unified-session-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")

add_executable(dmn-avahi-contribution-test
  contribution-test.cpp
  ../../src/dmn-discovery-avahi-contribution.cpp
  ../../src/dmn-discovery-avahi-record-watch.cpp
  ../../src/dmn-discovery-avahi-resolver.cpp
  ../../src/dmn-discovery-avahi-mapping.cpp
  ../../src/dmn-discovery-command.cpp
  ../../src/dmn-discovery-core.cpp
  ../../src/dmn-discovery-delivery.cpp
  ../../src/dmn-discovery-dispatcher.cpp
  ../../src/dmn-discovery-records.cpp
  ../../src/dmn-discovery-registration.cpp
  ../../src/dmn-discovery-retry.cpp
  ../../src/dmn-discovery-session.cpp
  ../../src/dmn-discovery-slots.cpp)
target_include_directories(dmn-avahi-contribution-test PRIVATE ../../src ../../include)
target_link_libraries(dmn-avahi-contribution-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
if (ENABLE_FAULT_INJECTION AND DMN_LIBFIU_TARGET)
  target_compile_definitions(dmn-avahi-contribution-test PRIVATE FIU_ENABLE=1)
  target_link_libraries(dmn-avahi-contribution-test PRIVATE "${DMN_LIBFIU_TARGET}")
  if (TARGET libfiu-build)
    add_dependencies(dmn-avahi-contribution-test libfiu-build)
  endif()
endif()
target_compile_options(dmn-avahi-contribution-test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME dmn-avahi-contribution-test COMMAND dmn-avahi-contribution-test)
set_tests_properties(dmn-avahi-contribution-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")

if (ENABLE_DMN_DISCOVERY AND TARGET Dmn::dmn)
  add_executable(dmn-avahi-public-allocation-test public-allocation-test.cpp)
  target_include_directories(dmn-avahi-public-allocation-test PRIVATE ../../src ../../include)
  target_link_libraries(dmn-avahi-public-allocation-test PRIVATE Dmn::dmn)
  target_compile_options(dmn-avahi-public-allocation-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME dmn-avahi-public-allocation-test COMMAND dmn-avahi-public-allocation-test)
  set_tests_properties(dmn-avahi-public-allocation-test PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")
endif()

option(DMN_AVAHI_REAL_DAEMON_TEST "Run Avahi adapter against an isolated test daemon" OFF)
if (DMN_AVAHI_REAL_DAEMON_TEST)
  find_program(DMN_TEST_UNSHARE unshare REQUIRED)
  find_program(DMN_TEST_IP ip REQUIRED)
  find_program(DMN_TEST_MOUNT mount REQUIRED)
  find_program(DMN_TEST_DBUS dbus-run-session REQUIRED)
  find_program(DMN_TEST_AVAHI avahi-daemon REQUIRED)
  add_executable(dmn-avahi-real-daemon-test
    real-daemon-test.cpp
    ../../src/dmn-discovery-avahi.cpp
    ../../src/dmn-discovery-avahi-browser.cpp
    ../../src/dmn-discovery-avahi-mapping.cpp
    ../../src/dmn-discovery-avahi-record-watch.cpp
    ../../src/dmn-discovery-avahi-registration.cpp
    ../../src/dmn-discovery-avahi-resolver.cpp
    ../../src/dmn-discovery-command.cpp
    ../../src/dmn-discovery-core.cpp
    ../../src/dmn-discovery-records.cpp
    ../../src/dmn-discovery-registration.cpp
    ../../src/dmn-discovery-retry.cpp)
  target_include_directories(dmn-avahi-real-daemon-test PRIVATE ../../src ../../include)
  target_link_libraries(dmn-avahi-real-daemon-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
  target_compile_options(dmn-avahi-real-daemon-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME dmn-avahi-real-daemon-test
    COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
      "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
      "$<TARGET_FILE:dmn-avahi-real-daemon-test>")
  set_tests_properties(dmn-avahi-real-daemon-test PROPERTIES
    TIMEOUT 75 LABELS "dmn;dmn-discovery;dmn-avahi;real-daemon")

  add_executable(dmn-avahi-real-unified-test real-unified-test.cpp)
  target_sources(dmn-avahi-real-unified-test PRIVATE ${_avahi_unified_sources})
  target_include_directories(dmn-avahi-real-unified-test PRIVATE ../../src ../../include)
  target_link_libraries(dmn-avahi-real-unified-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
  target_compile_options(dmn-avahi-real-unified-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME dmn-avahi-real-unified-test
    COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
      "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
      "$<TARGET_FILE:dmn-avahi-real-unified-test>")
  set_tests_properties(dmn-avahi-real-unified-test PROPERTIES
    TIMEOUT 75 LABELS "dmn;dmn-discovery;dmn-avahi;real-daemon")

  if (ENABLE_DMN_DISCOVERY AND TARGET Dmn::dmn)
    add_executable(dmn-avahi-real-public-test real-public-test.cpp)
    target_link_libraries(dmn-avahi-real-public-test PRIVATE Dmn::dmn)
    target_compile_options(dmn-avahi-real-public-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dmn-avahi-real-public-test
      COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
        "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
        "$<TARGET_FILE:dmn-avahi-real-public-test>")
    set_tests_properties(dmn-avahi-real-public-test PROPERTIES
      TIMEOUT 75 LABELS "dmn;dmn-discovery;dmn-avahi;real-daemon")

    add_test(NAME dmn-avahi-real-recovery-test
      COMMAND "${CMAKE_COMMAND}" -E env DMN_TEST_MANAGED_AVAHI=1
        sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
        "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
        "$<TARGET_FILE:dmn-avahi-real-public-test>" --recovery)
    set_tests_properties(dmn-avahi-real-recovery-test PROPERTIES
      TIMEOUT 100 LABELS "dmn;dmn-discovery;dmn-avahi;real-daemon")
  endif()
endif()

option(ENABLE_VALGRIND "Register Avahi provider memory checks" OFF)
if (ENABLE_VALGRIND)
  find_program(VALGRIND_EXECUTABLE valgrind REQUIRED)
  foreach(test_target dmn-avahi-provider-smoke dmn-avahi-poll-test dmn-avahi-mapping-test dmn-avahi-browser-test dmn-avahi-record-watch-test dmn-avahi-resolver-test dmn-avahi-registration-test dmn-avahi-session-integration-test dmn-avahi-registration-session-test dmn-avahi-unified-session-test dmn-avahi-contribution-test)
    add_test(NAME valgrind-${test_target}
      COMMAND "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
              "$<TARGET_FILE:${test_target}>")
    set_tests_properties(valgrind-${test_target} PROPERTIES LABELS valgrind TIMEOUT 30)
  endforeach()
  if (DMN_AVAHI_REAL_DAEMON_TEST)
    add_test(NAME valgrind-dmn-avahi-real-daemon-test
      COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
        "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
        "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
        "$<TARGET_FILE:dmn-avahi-real-daemon-test>")
    set_tests_properties(valgrind-dmn-avahi-real-daemon-test PROPERTIES
      LABELS "valgrind;real-daemon" TIMEOUT 90)
    add_test(NAME valgrind-dmn-avahi-real-unified-test
      COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
        "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
        "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
        "$<TARGET_FILE:dmn-avahi-real-unified-test>")
    set_tests_properties(valgrind-dmn-avahi-real-unified-test PROPERTIES
      LABELS "valgrind;real-daemon" TIMEOUT 90)
    if (TARGET dmn-avahi-real-public-test)
      add_test(NAME valgrind-dmn-avahi-real-public-test
        COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
          "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
          "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
          "$<TARGET_FILE:dmn-avahi-real-public-test>")
      set_tests_properties(valgrind-dmn-avahi-real-public-test PROPERTIES
        LABELS "valgrind;real-daemon" TIMEOUT 90)
      add_test(NAME valgrind-dmn-avahi-real-recovery-test
        COMMAND "${CMAKE_COMMAND}" -E env DMN_TEST_MANAGED_AVAHI=1
          sh "${CMAKE_CURRENT_LIST_DIR}/run-isolated-daemon-test.sh"
          "${CMAKE_CURRENT_LIST_DIR}/isolated-daemon.conf"
          "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
          "$<TARGET_FILE:dmn-avahi-real-public-test>" --recovery)
      set_tests_properties(valgrind-dmn-avahi-real-recovery-test PROPERTIES
        LABELS "valgrind;real-daemon" TIMEOUT 120)
    endif()
  endif()
endif()

option(DMN_AVAHI_PRIVATE_BUS_TEST "Test Avahi against a private D-Bus without a daemon" OFF)
if (DMN_AVAHI_PRIVATE_BUS_TEST)
  find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
  add_executable(dmn-avahi-private-bus-test
    private-bus-test.cpp
    ../../src/dmn-discovery-avahi.cpp
    ../../src/dmn-discovery-command.cpp)
  target_include_directories(dmn-avahi-private-bus-test PRIVATE ../../src ../../include)
  target_link_libraries(dmn-avahi-private-bus-test PRIVATE "${DMN_AVAHI_TARGET}" Threads::Threads)
  target_compile_options(dmn-avahi-private-bus-test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME dmn-avahi-private-bus-test
    COMMAND "${DBUS_RUN_SESSION}" -- sh -c
      "DBUS_SYSTEM_BUS_ADDRESS=\"$DBUS_SESSION_BUS_ADDRESS\" exec \"$1\""
      dmn-test "$<TARGET_FILE:dmn-avahi-private-bus-test>")
  set_tests_properties(dmn-avahi-private-bus-test
    PROPERTIES TIMEOUT 20 LABELS "dmn;dmn-discovery;dmn-avahi")
  if (ENABLE_VALGRIND)
    add_test(NAME valgrind-dmn-avahi-private-bus-test
      COMMAND "${DBUS_RUN_SESSION}" -- sh -c
        "DBUS_SYSTEM_BUS_ADDRESS=\"$DBUS_SESSION_BUS_ADDRESS\" exec \"$@\""
        dmn-test "${VALGRIND_EXECUTABLE}" --error-exitcode=1 --leak-check=full
        "$<TARGET_FILE:dmn-avahi-private-bus-test>")
    set_tests_properties(valgrind-dmn-avahi-private-bus-test
      PROPERTIES LABELS valgrind TIMEOUT 30)
  endif()
endif()

# Private adapter fixtures compile discovery sources outside the Dmn target.
add_library(dmn-avahi-test-proc STATIC ../../src/dmn-proc.cpp)
target_include_directories(dmn-avahi-test-proc PUBLIC ../../include)
target_link_libraries(dmn-avahi-test-proc PUBLIC Threads::Threads)
foreach(_target IN ITEMS
    dmn-avahi-poll-test
    dmn-avahi-session-integration-test
    dmn-avahi-registration-session-test
    dmn-avahi-unified-session-test
    dmn-avahi-contribution-test
    dmn-avahi-real-daemon-test
    dmn-avahi-real-unified-test
    dmn-avahi-private-bus-test)
  if (TARGET "${_target}")
    target_link_libraries("${_target}" PRIVATE dmn-avahi-test-proc)
  endif()
endforeach()
