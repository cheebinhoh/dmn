# Copyright © 2026 Chee Bin HOH. All rights reserved.
#
# Private dependency reconstruction shared by the build and installed package.

function(_dmn_avahi_dbus_target)
  if (TARGET DmnAvahiInternal::dbus)
    return()
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DMN_AVAHI_DBUS REQUIRED IMPORTED_TARGET dbus-1)
  get_target_property(_libraries PkgConfig::DMN_AVAHI_DBUS INTERFACE_LINK_LIBRARIES)
  set(_links PkgConfig::DMN_AVAHI_DBUS)
  if ("${_libraries}" MATCHES "\\.a(;|$)")
    # FindPkgConfig's imported target uses the non-static metadata. Include
    # Requires.private/Libs.private when it resolved a static D-Bus library.
    set(_private_libs ${DMN_AVAHI_DBUS_STATIC_LIBRARIES})
    list(REMOVE_ITEM _private_libs dbus-1)
    list(APPEND _links ${_private_libs})
  endif()
  add_library(DmnAvahiInternal::dbus INTERFACE IMPORTED)
  set_target_properties(DmnAvahiInternal::dbus PROPERTIES
    INTERFACE_LINK_LIBRARIES "${_links}")
  if ("${_libraries}" MATCHES "\\.a(;|$)")
    set_target_properties(DmnAvahiInternal::dbus PROPERTIES
      INTERFACE_LINK_DIRECTORIES "${DMN_AVAHI_DBUS_STATIC_LIBRARY_DIRS}"
      INTERFACE_LINK_OPTIONS "${DMN_AVAHI_DBUS_STATIC_LDFLAGS_OTHER}")
  endif()
endfunction()

function(_dmn_avahi_system_target)
  if (TARGET DmnAvahiInternal::client)
    return()
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DMN_AVAHI REQUIRED IMPORTED_TARGET avahi-client)
  get_target_property(_libraries PkgConfig::DMN_AVAHI INTERFACE_LINK_LIBRARIES)
  set(_links PkgConfig::DMN_AVAHI)
  # Avahi's own .pc file omits libdbus for static links and puts common
  # before client. CMake deduplicates repeated interface targets, so append
  # the resolved common library explicitly after the client.
  if ("${_libraries}" MATCHES "\\.a(;|$)")
    foreach(_library IN LISTS _libraries)
      if (_library MATCHES "(^|/)libavahi-common\\.(a|so(\\.[0-9.]+)?)$")
        list(APPEND _links "${_library}")
      endif()
    endforeach()
    _dmn_avahi_dbus_target()
    find_package(Threads REQUIRED)
    list(APPEND _links DmnAvahiInternal::dbus Threads::Threads "${CMAKE_DL_LIBS}")
  endif()
  add_library(DmnAvahiInternal::client INTERFACE IMPORTED)
  set_target_properties(DmnAvahiInternal::client PROPERTIES
    INTERFACE_LINK_LIBRARIES "${_links}")
endfunction()
