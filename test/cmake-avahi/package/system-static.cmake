set(_root "${BUILD_DIR}/avahi-system-static-test")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/pkgconfig")
file(WRITE "${_root}/pkgconfig/avahi-client.pc"
  "prefix=${BUILD_DIR}/_deps/avahi-install\n"
  "libdir=\${prefix}/lib\nincludedir=\${prefix}/include\n"
  "Name: avahi-client\nDescription: static Avahi test\nVersion: 0.8\n"
  "Libs: -L\${libdir} -lavahi-common -lavahi-client\n"
  "Cflags: -I\${includedir}\n")
set(_env "PKG_CONFIG_PATH=${_root}/pkgconfig:$ENV{PKG_CONFIG_PATH}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${_env}"
  "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}" -B "${_root}/build"
  -DDMN_AVAHI_PROVIDER=SYSTEM -DDMN_AVAHI_PACKAGE_FIXTURE=ON
  "-DENABLE_VALGRIND=${ENABLE_VALGRIND}"
  -DCMAKE_BUILD_TYPE=Debug RESULT_VARIABLE _result)
if (_result)
  message(FATAL_ERROR "Static system fixture configure failed: ${_result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${_env}"
  "${CMAKE_COMMAND}" --build "${_root}/build"
  --target dmn-avahi-package-fixture --config "${CONFIG}" RESULT_VARIABLE _result)
if (_result)
  message(FATAL_ERROR "Static system fixture build failed: ${_result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${_env}"
  "${CMAKE_CTEST_COMMAND}" --test-dir "${_root}/build"
  -C "${CONFIG}" -R "^dmn-avahi-package-relocation$" --output-on-failure
  RESULT_VARIABLE _result)
if (_result)
  message(FATAL_ERROR "Static system package test failed: ${_result}")
endif()
