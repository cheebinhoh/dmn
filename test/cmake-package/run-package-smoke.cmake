# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

if (NOT DEFINED DMN_BINARY_DIR OR NOT DEFINED DMN_SOURCE_DIR OR
    NOT DEFINED DMN_INSTALL_LIBDIR OR NOT DEFINED DMN_INSTALL_INCLUDEDIR OR
    NOT DEFINED DMN_CTEST_COMMAND)
  message(FATAL_ERROR "Missing package smoke-test configuration")
endif()

string(RANDOM LENGTH 10 ALPHABET 0123456789abcdef _suffix)
# Keep staging and consumer build files unique so parallel CTest runs do not
# share or overwrite temporary install prefixes.
set(_test_root "${DMN_BINARY_DIR}/cmake-package-smoke-${_suffix}")
set(_stage "${_test_root}/stage")
set(_relocated "${_test_root}/relocated")
set(_consumer_build "${_test_root}/consumer-build")

function(run_checked)
  execute_process(
    COMMAND ${ARGV}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
  )
  if (NOT _result EQUAL 0)
    file(REMOVE_RECURSE "${_test_root}")
    message(FATAL_ERROR
            "Command failed (${_result}): ${ARGV}\n${_stdout}\n${_stderr}")
  endif()
endfunction()

function(package_test_error text)
  file(REMOVE_RECURSE "${_test_root}")
  message(FATAL_ERROR "${text}")
endfunction()

set(_dependency_pkg_config_paths)

if (DEFINED ENV{DMN_LIBFIU_SOURCE_DIR})
  if (NOT DEFINED ENV{DMN_LIBFIU_MAKE_EXECUTABLE})
    message(FATAL_ERROR "Missing make executable for fetched libfiu")
  endif()
  set(_fiu_prefix "${_test_root}/libfiu")
  run_checked("${CMAKE_COMMAND}" -E chdir "$ENV{DMN_LIBFIU_SOURCE_DIR}/libfiu"
              "$ENV{DMN_LIBFIU_MAKE_EXECUTABLE}" install-lib
              "PREFIX=${_fiu_prefix}")
  list(APPEND _dependency_pkg_config_paths "${_fiu_prefix}/lib/pkgconfig")
endif()

if (DEFINED ENV{DMN_RDKAFKA_BINARY_DIR})
  set(_rdkafka_prefix "${_test_root}/rdkafka")
  set(_rdkafka_build_command "${CMAKE_COMMAND}" --build "${DMN_BINARY_DIR}"
                             --target "rdkafka++")
  set(_rdkafka_install_command "${CMAKE_COMMAND}" --install
                               "$ENV{DMN_RDKAFKA_BINARY_DIR}" --prefix
                               "${_rdkafka_prefix}")
  if (DEFINED DMN_BUILD_CONFIG AND NOT DMN_BUILD_CONFIG STREQUAL "")
    list(APPEND _rdkafka_build_command --config "${DMN_BUILD_CONFIG}")
    list(APPEND _rdkafka_install_command --config "${DMN_BUILD_CONFIG}")
  endif()
  run_checked(${_rdkafka_build_command})
  run_checked(${_rdkafka_install_command})

  file(GLOB_RECURSE _rdkafka_pkg_config_files
       "${_rdkafka_prefix}/*/pkgconfig/rdkafka.pc")
  if (NOT _rdkafka_pkg_config_files)
    package_test_error("Fetched librdkafka install did not provide rdkafka.pc")
  endif()
  foreach(_rdkafka_pkg_config_file IN LISTS _rdkafka_pkg_config_files)
    file(STRINGS "${_rdkafka_pkg_config_file}" _rdkafka_pkg_config_lines)
    set(_rdkafka_pkg_config_contents "")
    foreach(_line IN LISTS _rdkafka_pkg_config_lines)
      if (_line MATCHES "^prefix=")
        set(_line "prefix=${_rdkafka_prefix}")
      elseif (_line MATCHES "^Requires\\.private:")
        continue()
      endif()
      string(APPEND _rdkafka_pkg_config_contents "${_line}\n")
    endforeach()
    file(WRITE "${_rdkafka_pkg_config_file}"
         "${_rdkafka_pkg_config_contents}")
    get_filename_component(_rdkafka_pkg_config_dir
                           "${_rdkafka_pkg_config_file}" DIRECTORY)
    list(APPEND _dependency_pkg_config_paths "${_rdkafka_pkg_config_dir}")
  endforeach()
endif()

if (_dependency_pkg_config_paths)
  list(JOIN _dependency_pkg_config_paths ":" _pkg_config_path)
  if (DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    string(APPEND _pkg_config_path ":$ENV{PKG_CONFIG_PATH}")
  endif()
  set(ENV{PKG_CONFIG_PATH} "${_pkg_config_path}")
endif()

set(_install_command "${CMAKE_COMMAND}" --install "${DMN_BINARY_DIR}"
                     --prefix "${_stage}")
if (DEFINED DMN_BUILD_CONFIG AND NOT DMN_BUILD_CONFIG STREQUAL "")
  list(APPEND _install_command --config "${DMN_BUILD_CONFIG}")
endif()
run_checked(${_install_command})

file(RENAME "${_stage}" "${_relocated}")

if (DMN_EXPECT_DMESGNET)
  foreach(_header IN ITEMS
          dmn-dmesg.hpp
          proto/dmn-dlock.pb.h
          proto/dmn-dmesg-body.pb.h
          proto/dmn-dmesg-type.pb.h
          proto/dmn-dmesg.pb.h)
    if (NOT EXISTS "${_relocated}/${DMN_INSTALL_INCLUDEDIR}/dmn/${_header}")
      package_test_error("Expected installed DMesg header: ${_header}")
    endif()
  endforeach()
else()
  foreach(_header IN ITEMS
          dmn-dmesg.hpp
          proto/dmn-dlock.pb.h
          proto/dmn-dmesg-body.pb.h
          proto/dmn-dmesg-type.pb.h
          proto/dmn-dmesg.pb.h)
    if (EXISTS "${_relocated}/${DMN_INSTALL_INCLUDEDIR}/dmn/${_header}")
      package_test_error("DMesg-disabled package installed ${_header}")
    endif()
  endforeach()
endif()

if (DEFINED DMN_PROTOBUF_FETCH_BUILD_DIR AND
    NOT DMN_PROTOBUF_FETCH_BUILD_DIR STREQUAL "")
  set(_protobuf_prefix "${_test_root}/protobuf")
  set(_protobuf_build_command "${CMAKE_COMMAND}" --build
                              "${DMN_PROTOBUF_FETCH_BUILD_DIR}")
  set(_protobuf_install_command "${CMAKE_COMMAND}" --install
                                "${DMN_PROTOBUF_FETCH_BUILD_DIR}"
                                --prefix "${_protobuf_prefix}")
  if (DEFINED DMN_BUILD_CONFIG AND NOT DMN_BUILD_CONFIG STREQUAL "")
    list(APPEND _protobuf_build_command --config "${DMN_BUILD_CONFIG}")
    list(APPEND _protobuf_install_command --config "${DMN_BUILD_CONFIG}")
  endif()
  run_checked(${_protobuf_build_command})
  run_checked(${_protobuf_install_command})

  file(GLOB_RECURSE _protobuf_configs
       "${_protobuf_prefix}/*/cmake/protobuf/protobuf-config.cmake")
  list(LENGTH _protobuf_configs _protobuf_config_count)
  if (NOT _protobuf_config_count EQUAL 1)
    package_test_error("Fetched Protobuf install did not provide one CMake config")
  endif()
  list(GET _protobuf_configs 0 _protobuf_config)
  get_filename_component(_protobuf_config_dir "${_protobuf_config}" DIRECTORY)
  set(DMN_PROTOBUF_ROOT "${_protobuf_prefix}")
endif()

set(_consumer_prefix_paths "${_relocated}")
if (DEFINED DMN_PROTOBUF_ROOT AND NOT DMN_PROTOBUF_ROOT STREQUAL "")
  list(APPEND _consumer_prefix_paths "${DMN_PROTOBUF_ROOT}")
endif()
if (DEFINED DMN_CMAKE_PREFIX_PATH AND NOT DMN_CMAKE_PREFIX_PATH STREQUAL "")
  list(APPEND _consumer_prefix_paths ${DMN_CMAKE_PREFIX_PATH})
endif()
if (DEFINED CMAKE_PREFIX_PATH AND NOT CMAKE_PREFIX_PATH STREQUAL "")
  list(APPEND _consumer_prefix_paths ${CMAKE_PREFIX_PATH})
endif()

list(REMOVE_DUPLICATES _consumer_prefix_paths)
list(JOIN _consumer_prefix_paths ":" _consumer_prefix_path)
if (DEFINED ENV{CMAKE_PREFIX_PATH} AND NOT "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
  string(APPEND _consumer_prefix_path ":$ENV{CMAKE_PREFIX_PATH}")
endif()
set(ENV{CMAKE_PREFIX_PATH} "${_consumer_prefix_path}")

set(_configure_command "${CMAKE_COMMAND}"
                       -S "${DMN_SOURCE_DIR}/test/cmake-package"
                       -B "${_consumer_build}"
                       "-DDmn_DIR=${_relocated}/${DMN_INSTALL_LIBDIR}/cmake/Dmn"
                       -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON
                       -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
                       -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
                       -DDMN_EXPECT_DMESGNET=${DMN_EXPECT_DMESGNET}
                       -DDMN_EXPECT_ANCS=${DMN_EXPECT_ANCS}
                       -DDMN_EXPECT_DBUS=${DMN_EXPECT_DBUS}
                       -DDMN_EXPECT_BLUEZ=${DMN_EXPECT_BLUEZ}
                       -DDMN_EXPECT_ANCS_BLUEZ_MONITOR=${DMN_EXPECT_ANCS_BLUEZ_MONITOR})

if (DEFINED _protobuf_config_dir)
  list(APPEND _configure_command "-DProtobuf_DIR=${_protobuf_config_dir}")
endif()

if (DEFINED DMN_GENERATOR AND NOT DMN_GENERATOR STREQUAL "")
  list(APPEND _configure_command -G "${DMN_GENERATOR}")
endif()
if (DEFINED DMN_BUILD_TYPE AND NOT DMN_BUILD_TYPE STREQUAL "")
  list(APPEND _configure_command "-DCMAKE_BUILD_TYPE=${DMN_BUILD_TYPE}")
endif()

if (DEFINED DMN_ICU_ROOT AND NOT DMN_ICU_ROOT STREQUAL "")
  list(APPEND _configure_command "-DICU_ROOT=${DMN_ICU_ROOT}")
elseif (DEFINED ICU_ROOT AND NOT ICU_ROOT STREQUAL "")
  list(APPEND _configure_command "-DICU_ROOT=${ICU_ROOT}")
elseif (DEFINED ENV{ICU_ROOT} AND NOT "$ENV{ICU_ROOT}" STREQUAL "")
  list(APPEND _configure_command "-DICU_ROOT=$ENV{ICU_ROOT}")
endif()

if (DEFINED DMN_PROTOBUF_ROOT AND NOT DMN_PROTOBUF_ROOT STREQUAL "")
  list(APPEND _configure_command "-DProtobuf_ROOT=${DMN_PROTOBUF_ROOT}")
elseif (DEFINED Protobuf_ROOT AND NOT Protobuf_ROOT STREQUAL "")
  list(APPEND _configure_command "-DProtobuf_ROOT=${Protobuf_ROOT}")
elseif (DEFINED ENV{Protobuf_ROOT} AND NOT "$ENV{Protobuf_ROOT}" STREQUAL "")
  list(APPEND _configure_command "-DProtobuf_ROOT=$ENV{Protobuf_ROOT}")
endif()

run_checked(${_configure_command})

set(_missing_dependency_build "${_test_root}/missing-dependency-build")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${DMN_SOURCE_DIR}/test/cmake-package"
          -B "${_missing_dependency_build}"
          "-DDmn_DIR=${_relocated}/${DMN_INSTALL_LIBDIR}/cmake/Dmn"
          -DCMAKE_DISABLE_FIND_PACKAGE_ICU=TRUE
          -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON
          -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
  RESULT_VARIABLE _missing_dependency_result
  OUTPUT_VARIABLE _missing_dependency_stdout
  ERROR_VARIABLE _missing_dependency_stderr
)

if (_missing_dependency_result EQUAL 0)
  package_test_error("Consumer configuration succeeded without required ICU")
endif()
string(FIND "${_missing_dependency_stdout}${_missing_dependency_stderr}"
       "ICU" _missing_dependency_error_index)
if (_missing_dependency_error_index EQUAL -1)
  package_test_error("Missing ICU did not produce a clear dependency error")
endif()

set(_build_command "${CMAKE_COMMAND}" --build "${_consumer_build}")
if (DEFINED DMN_BUILD_CONFIG AND NOT DMN_BUILD_CONFIG STREQUAL "")
  list(APPEND _build_command --config "${DMN_BUILD_CONFIG}")
endif()
run_checked(${_build_command})

file(GLOB _package_files
     "${_relocated}/${DMN_INSTALL_LIBDIR}/cmake/Dmn/*.cmake")
foreach(_package_file IN LISTS _package_files)
  file(READ "${_package_file}" _package_contents)
  string(FIND "${_package_contents}" "${DMN_SOURCE_DIR}" _source_path_index)
  string(FIND "${_package_contents}" "${DMN_BINARY_DIR}" _build_path_index)
  if (NOT _source_path_index EQUAL -1 OR NOT _build_path_index EQUAL -1)
    package_test_error(
      "Installed package leaks a Dmn source/build path: ${_package_file}")
  endif()
endforeach()

set(_consumer_test_command "${DMN_CTEST_COMMAND}" --test-dir "${_consumer_build}"
                           --output-on-failure)
if (DEFINED DMN_BUILD_CONFIG AND NOT DMN_BUILD_CONFIG STREQUAL "")
  list(APPEND _consumer_test_command -C "${DMN_BUILD_CONFIG}")
endif()
run_checked(${_consumer_test_command})

file(REMOVE_RECURSE "${_test_root}")
