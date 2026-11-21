/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file main.cpp
 * @brief Link and run a minimal consumer of the installed Dmn package.
 */

#include <dmn-gatt-client.hpp>
#include <dmn-proc.hpp>

#if DMN_PACKAGE_EXPECT_DMESGNET
#include <dmn-dmesg.hpp>
#endif

#if DMN_PACKAGE_EXPECT_ANCS
#include <dmn-ancs-client.hpp>
#endif

#if DMN_PACKAGE_EXPECT_DBUS
#include <dmn-dbus-client.hpp>
#endif

#if DMN_PACKAGE_EXPECT_BLUEZ
#include <dmn-bluez-client.hpp>
#endif

#if DMN_PACKAGE_EXPECT_ANCS_BLUEZ_MONITOR
#include <dmn-ancs-bluez-monitor.hpp>
#endif

int main() {
  dmn::Dmn_Proc worker("installed-package-consumer");
  if (!worker.exec([] {}) || !worker.wait()) {
    return 1;
  }

#if DMN_PACKAGE_EXPECT_DMESGNET
  dmn::DMesgPb message;
  if (message.ByteSizeLong() != 0) {
    return 2;
  }
#endif

  return 0;
}
