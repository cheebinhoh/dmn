# Copyright © 2026 Chee Bin HOH. All rights reserved.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/DmnAvahiDependencies.cmake")

# Resolve Avahi for the Linux discovery backend. The source provider builds
# only the client/common libraries; it never installs or starts a daemon.
function(dmn_prepare_avahi)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Avahi is supported only on Linux.")
  endif()
  set(DMN_AVAHI_PROVIDER "AUTO" CACHE STRING
      "Avahi provider: AUTO, SYSTEM, or SOURCE")
  set_property(CACHE DMN_AVAHI_PROVIDER PROPERTY STRINGS AUTO SYSTEM SOURCE)
  if (NOT DMN_AVAHI_PROVIDER MATCHES "^(AUTO|SYSTEM|SOURCE)$")
    message(FATAL_ERROR "DMN_AVAHI_PROVIDER must be AUTO, SYSTEM, or SOURCE.")
  endif()

  find_package(PkgConfig REQUIRED)
  if (NOT DMN_AVAHI_PROVIDER STREQUAL "SOURCE")
    pkg_check_modules(DMN_AVAHI QUIET IMPORTED_TARGET avahi-client)
    if (TARGET PkgConfig::DMN_AVAHI)
      _dmn_avahi_system_target()
      set(DMN_AVAHI_TARGET DmnAvahiInternal::client PARENT_SCOPE)
      set(DMN_AVAHI_SELECTED_PROVIDER SYSTEM PARENT_SCOPE)
      return()
    endif()
    if (DMN_AVAHI_PROVIDER STREQUAL "SYSTEM")
      message(FATAL_ERROR "Avahi development files were not found via pkg-config.")
    endif()
  endif()

  if (NOT DMN_ALLOW_DEPENDENCY_FETCH)
    message(FATAL_ERROR
            "Avahi development files were not found or SOURCE was selected, "
            "but DMN_ALLOW_DEPENDENCY_FETCH=OFF.")
  endif()
  if (CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The Avahi source provider does not support cross-compiling.")
  endif()
  _dmn_avahi_dbus_target()
  find_package(Threads REQUIRED)
  find_program(DMN_AVAHI_MAKE NAMES gmake make REQUIRED)
  include(ExternalProject)

  get_target_property(_dbus_libraries PkgConfig::DMN_AVAHI_DBUS INTERFACE_LINK_LIBRARIES)
  if ("${_dbus_libraries}" MATCHES "\\.a(;|$)")
    execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" ${PKG_CONFIG_ARGN}
                            --static --libs dbus-1
      OUTPUT_VARIABLE _dbus_static_flags OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
    set(_dbus_configure_env "DBUS_LIBS=${_dbus_static_flags}")
  endif()
  set(_prefix "${CMAKE_BINARY_DIR}/_deps/avahi-install")
  file(MAKE_DIRECTORY "${_prefix}/include" "${_prefix}/lib")
  ExternalProject_Add(dmn-avahi-build
    URL https://github.com/avahi/avahi/releases/download/v0.8/avahi-0.8.tar.gz
    URL_HASH SHA256=060309d7a333d38d951bc27598c677af1796934dbd98e1024e7ad8de798fedda
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/avahi-download"
    DOWNLOAD_NAME avahi-0.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    INSTALL_DIR "${_prefix}"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
      ${_dbus_configure_env}
      <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-nls
      --disable-glib --disable-gobject --disable-libevent --disable-qt4
      --disable-qt5 --disable-gtk --disable-gtk3 --disable-gdbm
      --disable-libdaemon --disable-python --disable-pygobject
      --disable-python-dbus --disable-mono --disable-monodoc
      --disable-autoipd --disable-doxygen-doc --disable-manpages
      --disable-xmltoman --disable-shared --enable-static --with-pic
      --with-xml=none --with-distro=none
    BUILD_COMMAND "${DMN_AVAHI_MAKE}" -C <BINARY_DIR>/avahi-common
      COMMAND "${DMN_AVAHI_MAKE}" -C <BINARY_DIR>/avahi-client
    INSTALL_COMMAND "${DMN_AVAHI_MAKE}" -C <BINARY_DIR>/avahi-common install
      COMMAND "${DMN_AVAHI_MAKE}" -C <BINARY_DIR>/avahi-client install
    INSTALL_BYPRODUCTS "${_prefix}/lib/libavahi-client.a"
                       "${_prefix}/lib/libavahi-common.a"
  )

  add_library(DmnAvahiInternal::common STATIC IMPORTED)
  set_target_properties(DmnAvahiInternal::common PROPERTIES
    IMPORTED_LOCATION "${_prefix}/lib/libavahi-common.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include"
    INTERFACE_LINK_LIBRARIES Threads::Threads
  )
  add_dependencies(DmnAvahiInternal::common dmn-avahi-build)
  add_library(DmnAvahiInternal::client STATIC IMPORTED)
  set_target_properties(DmnAvahiInternal::client PROPERTIES
    IMPORTED_LOCATION "${_prefix}/lib/libavahi-client.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include"
    INTERFACE_LINK_LIBRARIES "DmnAvahiInternal::common;DmnAvahiInternal::dbus;${CMAKE_DL_LIBS}"
  )
  add_dependencies(DmnAvahiInternal::client dmn-avahi-build)
  set(DMN_AVAHI_TARGET DmnAvahiInternal::client PARENT_SCOPE)
  set(DMN_AVAHI_SELECTED_PROVIDER SOURCE PARENT_SCOPE)
endfunction()

# Call only when an exported target actually uses Avahi. The resulting private
# config must be included before the containing package's exported targets.
function(dmn_install_avahi config_dir)
  include(GNUInstallDirs)
  foreach(_dir "${config_dir}" "${CMAKE_INSTALL_LIBDIR}" "${CMAKE_INSTALL_DATADIR}")
    if (IS_ABSOLUTE "${_dir}")
      message(FATAL_ERROR "Avahi packaging requires relative install directories for relocation.")
    endif()
  endforeach()
  if (NOT DMN_AVAHI_SELECTED_PROVIDER MATCHES "^(SYSTEM|SOURCE)$")
    message(FATAL_ERROR "Call dmn_prepare_avahi before dmn_install_avahi.")
  endif()
  file(RELATIVE_PATH DMN_AVAHI_LIB_RELATIVE
       "${CMAKE_INSTALL_PREFIX}/${config_dir}"
       "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/dmn/avahi")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DmnAvahiInstalled.cmake.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/DmnAvahiInstalled.cmake" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/DmnAvahiInstalled.cmake"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DmnAvahiDependencies.cmake"
          DESTINATION "${config_dir}" COMPONENT Development)
  if (DMN_AVAHI_SELECTED_PROVIDER STREQUAL "SOURCE")
    ExternalProject_Get_Property(dmn-avahi-build SOURCE_DIR)
    install(FILES "${CMAKE_BINARY_DIR}/_deps/avahi-install/lib/libavahi-client.a"
                  "${CMAKE_BINARY_DIR}/_deps/avahi-install/lib/libavahi-common.a"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/dmn/avahi" COMPONENT Development)
    install(FILES "${SOURCE_DIR}/LICENSE"
                  "${CMAKE_BINARY_DIR}/_deps/avahi-download/avahi-0.8.tar.gz"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dmn/avahi"
            COMPONENT Development)
    install(FILES "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/avahi-NOTICE.txt"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/dmn/avahi"
            RENAME NOTICE COMPONENT Development)
  endif()
endfunction()
