/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file main.cpp
 * @brief Link and exercise the selected Avahi provider without a daemon.
 */

#include <avahi-client/client.h>
#include <avahi-common/simple-watch.h>

#include <memory>

int main() {
  auto *volatile clientFactory = &avahi_client_new;
  const std::unique_ptr<AvahiSimplePoll, decltype(&avahi_simple_poll_free)>
      poll{avahi_simple_poll_new(), &avahi_simple_poll_free};
  return poll && clientFactory ? 0 : 1;
}
