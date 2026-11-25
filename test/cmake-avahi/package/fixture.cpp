#include <avahi-client/client.h>
#include <avahi-common/simple-watch.h>

int fixture_avahi() {
  auto *volatile factory = &avahi_client_new;
  auto *poll = avahi_simple_poll_new();
  if (!poll || !factory)
    return 1;
  avahi_simple_poll_free(poll);
  return 0;
}
