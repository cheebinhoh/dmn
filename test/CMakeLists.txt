# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

# [TEST TYPE: GTEST]
# Core unit and component tests (CTest label: dmn).
# Install into a temporary prefix, relocate it, and build an independent
# consumer to verify the generated package rather than the build-tree targets.
set(_dmn_protobuf_root "")
set(_dmn_protobuf_fetch_build_dir "")
if (DEFINED Protobuf_ROOT AND NOT Protobuf_ROOT STREQUAL "")
  set(_dmn_protobuf_root "${Protobuf_ROOT}")
elseif (DEFINED ENV{Protobuf_ROOT} AND NOT "$ENV{Protobuf_ROOT}" STREQUAL "")
  set(_dmn_protobuf_root "$ENV{Protobuf_ROOT}")
elseif (ENABLE_DMN_DMESGNET AND TARGET protobuf::libprotobuf)
  if (DEFINED protobuf_BINARY_DIR AND EXISTS "${protobuf_BINARY_DIR}")
    set(_dmn_protobuf_fetch_build_dir "${protobuf_BINARY_DIR}")
  elseif (EXISTS "${CMAKE_BINARY_DIR}/_deps/protobuf-build")
    set(_dmn_protobuf_fetch_build_dir "${CMAKE_BINARY_DIR}/_deps/protobuf-build")
  endif()
endif()

add_test(NAME dmn-cmake-package
           COMMAND "${CMAKE_COMMAND}"
                   "-DDMN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                   "-DDMN_BINARY_DIR=${CMAKE_BINARY_DIR}"
                   "-DDMN_GENERATOR=${CMAKE_GENERATOR}"
                   "-DDMN_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
                   "-DDMN_BUILD_CONFIG=$<CONFIG>"
                   "-DDMN_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
                   "-DDMN_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
                   "-DDMN_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
                   "-DDMN_EXPECT_DMESGNET=${ENABLE_DMN_DMESGNET}"
                   "-DDMN_EXPECT_ANCS=${ENABLE_ANCS}"
                   "-DDMN_EXPECT_DBUS=${ENABLE_DBUS}"
                   "-DDMN_EXPECT_BLUEZ=${ENABLE_BLUEZ}"
                   "-DDMN_ICU_ROOT=${ICU_ROOT}"
                   "-DDMN_PROTOBUF_ROOT=${_dmn_protobuf_root}"
                   "-DDMN_PROTOBUF_FETCH_BUILD_DIR=${_dmn_protobuf_fetch_build_dir}"
                   "-DDMN_EXPECT_ANCS_BLUEZ_MONITOR=$<AND:$<BOOL:${ENABLE_ANCS}>,$<BOOL:${ENABLE_BLUEZ}>>"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake-package/run-package-smoke.cmake"
)

set(_dmn_cmake_package_environment)
if (ENABLE_FAULT_INJECTION AND TARGET dmn-libfiu)
  list(APPEND _dmn_cmake_package_environment
       "DMN_LIBFIU_SOURCE_DIR=${libfiu_SOURCE_DIR}"
       "DMN_LIBFIU_MAKE_EXECUTABLE=${LIBFIU_MAKE_EXECUTABLE}"
  )
endif()

if (ENABLE_DMN_DMESGNET AND DMN_RDKAFKA_TARGET STREQUAL "rdkafka")
  list(APPEND _dmn_cmake_package_environment
       "DMN_RDKAFKA_BINARY_DIR=${rdkafka_BINARY_DIR}"
  )
endif()

if (_dmn_cmake_package_environment)
  set_tests_properties(dmn-cmake-package PROPERTIES
                       ENVIRONMENT "${_dmn_cmake_package_environment}"
  )
endif()

set_tests_properties(dmn-cmake-package PROPERTIES LABELS "dmn;cmake-package")

if (ENABLE_VALGRIND)
  # [TEST TYPE: VALGRIND]
  # Run the selected regular GTest executables under Valgrind.
  SET(DMN_VALGRIND_TESTS
        dmn-test-async
        dmn-test-async-2
        dmn-test-async-2-queue-lf
        dmn-test-async-queue-lf
        dmn-test-blockingqueue
        dmn-test-blockingqueue-2
        dmn-test-blockingqueue-3
        dmn-test-blockingqueue-4
        dmn-test-blockingqueue-5
        dmn-test-blockingqueue-6
        dmn-test-blockingqueue-lf-1
        dmn-test-blockingqueue-lf-2
        dmn-test-blockingqueue-lf-3
        dmn-test-blockingqueue-lf-4
        dmn-test-blockingqueue-lf-5
        dmn-test-blockingqueue-lf-6
        dmn-test-blockingqueue-lf-7
        dmn-test-blockingqueue-lf-8
        dmn-test-blockingqueue-lf-stress
        dmn-test-blockingqueue-lf-wait-empty
        dmn-test-blockingqueue-perf-1
        dmn-test-blockingqueue-perf-2
        dmn-test-blockingqueue-shutdown
        dmn-test-inflight-guard
        dmn-test-interval-btree
        dmn-test-io
        dmn-test-io-contract
        dmn-test-pipe
        dmn-test-pipe-2
        dmn-test-pipe-3
        dmn-test-pipe-4
        dmn-test-pipe-queue-lf
        dmn-test-pipe-queue-lf-2
        dmn-test-pipe-queue-lf-3
        dmn-test-pipe-queue-lf-4
        dmn-test-pipe-scheduled
        dmn-test-proc
        dmn-test-pub-sub
        dmn-test-pub-sub-2
        dmn-test-pub-sub-2-queue-lf
        dmn-test-pub-sub-errors
        dmn-test-pub-sub-queue-lf
        dmn-test-runtime
        dmn-test-runtime-2
        dmn-test-runtime-3
        dmn-test-runtime-queue-lf
        dmn-test-runtime-queue-lf-2
        dmn-test-runtime-queue-lf-3
        dmn-test-runtime-state
        dmn-test-singleton
        dmn-test-socket
        dmn-test-state
        dmn-test-timer
        dmn-test-umbrella
        dmn-test-util
  )

  if (ENABLE_DMN_DMESGNET)
    list(APPEND DMN_VALGRIND_TESTS
           dmn-test-dlock-1
           dmn-test-dlock-2
           dmn-test-dlock-3
           dmn-test-dlock-4
           dmn-test-dlock-5
           dmn-test-dlock-dmesg-seam
           dmn-test-dmesg-1
           dmn-test-dmesg-10
           dmn-test-dmesg-11
           dmn-test-dmesg-2
           dmn-test-dmesg-3
           dmn-test-dmesg-5
           dmn-test-dmesg-6
           dmn-test-dmesg-7
           dmn-test-dmesg-8
           dmn-test-dmesg-9
           dmn-test-dmesg-conflict
           dmn-test-dmesg-pb
           dmn-test-dmesg-pb-util
           dmn-test-dmesgnet-1
           dmn-test-dmesgnet-2
           dmn-test-dmesgnet-3
           dmn-test-dmesgnet-4
           dmn-test-dmesgnet-5
           dmn-test-dmesgnet-6
           dmn-test-dmesgnet-shutdown
           dmn-test-kafka-config
    )
  endif()

  # Library tests that do not link GUI code (GTK/GLib/COSMIC) also run under
  # Valgrind. GUI engine/selection tests are intentionally excluded.
  if (ENABLE_ANCS)
    list(APPEND DMN_VALGRIND_TESTS
           dmn-test-ancs-client
           dmn-test-ancs-parser
    )
  endif()

  if (ENABLE_DBUS)
    list(APPEND DMN_VALGRIND_TESTS
           dmn-test-dbus-client
           dmn-test-dbus-client-session
    )

    if (ENABLE_DMN_DMESGNET)
      list(APPEND DMN_VALGRIND_TESTS
             dmn-test-dbus-facade
             dmn-test-dbus-io
      )
    endif()
  endif()

  if (ENABLE_BLUEZ)
    # dmn-test-bluez-device-list spawns CLI subprocesses and does not finish
    # in reasonable time under Valgrind, so it stays a regular test only.
    list(APPEND DMN_VALGRIND_TESTS
           dmn-test-bluez-gatt-client
    )
  endif()

  ADD_TEST_VALGRIND(dmn
                      ${DMN_VALGRIND_TESTS}
  )

  add_custom_target(valgrind-check
                      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L valgrind
                      DEPENDS ${DMN_VALGRIND_TESTS}
                      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
