# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

cmake_minimum_required(VERSION 3.26)

project(DmnPackageConsumer LANGUAGES CXX)

# This fixture intentionally runs outside Dmn's source/build target graph.
find_package(Dmn CONFIG REQUIRED)

if (NOT DMN_EXPECT_DMESGNET AND TARGET PkgConfig::RDKAFKA)
  message(FATAL_ERROR "DMesg-disabled package must not require Kafka")
endif()

foreach(target IN ITEMS Dmn::dmn Dmn::dmn-gatt)
  if (NOT TARGET ${target})
    message(FATAL_ERROR "Required imported target is missing: ${target}")
  endif()
endforeach()

if (DMN_EXPECT_DISCOVERY)
  add_executable(dmn-discovery-package-consumer discovery-consumer.cpp)
  target_link_libraries(dmn-discovery-package-consumer PRIVATE Dmn::dmn)
  add_executable(dmn-discovery-and-dmn-package-consumer discovery-and-dmn-consumer.cpp)
  target_link_libraries(dmn-discovery-and-dmn-package-consumer
    PRIVATE Dmn::dmn)
  add_executable(dmn-discovery-qualification discovery-qualification.cpp)
  target_link_libraries(dmn-discovery-qualification PRIVATE Dmn::dmn)
  if (DMN_EXPECT_DMESGNET)
    add_executable(dmn-discovery-dmesgnet-consumer
                   discovery-dmesgnet-consumer.cpp)
    target_link_libraries(dmn-discovery-dmesgnet-consumer PRIVATE Dmn::dmn)
  endif()
endif()

get_target_property(_dmn_compile_definitions
                    Dmn::dmn INTERFACE_COMPILE_DEFINITIONS)
if (DMN_EXPECT_DISCOVERY AND
    NOT "DMN_ENABLE_DMN_DISCOVERY=1" IN_LIST _dmn_compile_definitions)
  message(FATAL_ERROR "Dmn::dmn must propagate DMN_ENABLE_DMN_DISCOVERY=1")
elseif (NOT DMN_EXPECT_DISCOVERY AND
        "DMN_ENABLE_DMN_DISCOVERY=1" IN_LIST _dmn_compile_definitions)
  message(FATAL_ERROR "Discovery feature definition must be absent")
endif()
if (DMN_EXPECT_DMESGNET AND
    NOT "DMN_ENABLE_DMN_DMESGNET=1" IN_LIST _dmn_compile_definitions)
  message(FATAL_ERROR
          "Dmn::dmn must propagate DMN_ENABLE_DMN_DMESGNET=1")
endif()

if (NOT DMN_EXPECT_DMESGNET AND
    "DMN_ENABLE_DMN_DMESGNET=1" IN_LIST _dmn_compile_definitions)
  message(FATAL_ERROR
          "Dmn::dmn must not propagate DMN_ENABLE_DMN_DMESGNET=1")
endif()

if (DMN_EXPECT_DBUS AND NOT TARGET Dmn::dmn-dbus)
  message(FATAL_ERROR "Expected Dmn::dmn-dbus")
endif()

if (DMN_EXPECT_ANCS AND NOT TARGET Dmn::dmn-ancs)
  message(FATAL_ERROR "Expected Dmn::dmn-ancs")
endif()

if (DMN_EXPECT_BLUEZ AND NOT TARGET Dmn::dmn-bluez)
  message(FATAL_ERROR "Expected Dmn::dmn-bluez")
endif()

if (DMN_EXPECT_ANCS_BLUEZ_MONITOR AND
    NOT TARGET Dmn::dmn-ancs-bluez-monitor)
  message(FATAL_ERROR "Expected Dmn::dmn-ancs-bluez-monitor")
endif()

if (NOT DMN_EXPECT_DBUS AND TARGET Dmn::dmn-dbus)
  message(FATAL_ERROR "Dmn::dmn-dbus must be absent when DBus is disabled")
endif()

if (NOT DMN_EXPECT_ANCS AND TARGET Dmn::dmn-ancs)
  message(FATAL_ERROR "Dmn::dmn-ancs must be absent when ANCS is disabled")
endif()

if (NOT DMN_EXPECT_ANCS_BLUEZ_MONITOR AND
    TARGET Dmn::dmn-ancs-bluez-monitor)
  message(FATAL_ERROR
          "Dmn::dmn-ancs-bluez-monitor must be absent unless both features are enabled")
endif()

if (NOT DMN_EXPECT_BLUEZ AND TARGET Dmn::dmn-bluez)
  message(FATAL_ERROR "Dmn::dmn-bluez must be absent when BlueZ is disabled")
endif()

add_executable(dmn-package-consumer main.cpp)
target_link_libraries(dmn-package-consumer PRIVATE Dmn::dmn Dmn::dmn-gatt)
target_compile_definitions(dmn-package-consumer
                             PRIVATE
                             DMN_PACKAGE_EXPECT_DMESGNET=$<BOOL:${DMN_EXPECT_DMESGNET}>
                             DMN_PACKAGE_EXPECT_ANCS=$<BOOL:${DMN_EXPECT_ANCS}>
                             DMN_PACKAGE_EXPECT_DBUS=$<BOOL:${DMN_EXPECT_DBUS}>
                             DMN_PACKAGE_EXPECT_BLUEZ=$<BOOL:${DMN_EXPECT_BLUEZ}>
                             DMN_PACKAGE_EXPECT_ANCS_BLUEZ_MONITOR=$<BOOL:${DMN_EXPECT_ANCS_BLUEZ_MONITOR}>
)

foreach(target IN ITEMS
        Dmn::dmn-ancs
        Dmn::dmn-dbus
        Dmn::dmn-bluez
        Dmn::dmn-ancs-bluez-monitor)
  if (TARGET ${target})
    target_link_libraries(dmn-package-consumer PRIVATE ${target})
  endif()
endforeach()

enable_testing()

add_test(NAME dmn-package-consumer COMMAND dmn-package-consumer)
if (DMN_EXPECT_DISCOVERY)
  add_test(NAME dmn-discovery-package-consumer
           COMMAND dmn-discovery-package-consumer)
  add_test(NAME dmn-discovery-and-dmn-package-consumer
           COMMAND dmn-discovery-and-dmn-package-consumer)
  add_test(NAME dmn-discovery-qualification-help
           COMMAND dmn-discovery-qualification --help)
  if (DMN_EXPECT_DMESGNET)
    add_test(NAME dmn-discovery-dmesgnet-consumer
             COMMAND dmn-discovery-dmesgnet-consumer)
  endif()
endif()
