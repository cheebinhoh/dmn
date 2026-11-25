#!/bin/sh
# Run only test-owned Avahi and D-Bus processes, with no host network access.
set -eu

if [ "$#" -lt 2 ]; then
  echo "usage: $0 AVAHI_CONFIG TEST_COMMAND [ARG ...]" >&2
  exit 2
fi

exec unshare -Urnmpf --mount-proc --kill-child sh -eu -s -- "$@" <<'NAMESPACE'
mount -t tmpfs -o mode=755 tmpfs /run
mount -t tmpfs -o mode=755 tmpfs /etc/avahi
mkdir -p /etc/avahi/services /run/systemd/resolve /run/home
printf "nameserver 127.0.0.1\n" > /run/systemd/resolve/stub-resolv.conf
export HOME=/run/home
unset XDG_RUNTIME_DIR
printf "root:x:0:0:root:/root:/bin/sh\navahi:x:0:0:Avahi:/run/avahi-daemon:/bin/false\n" > /run/passwd
printf "root:x:0:\navahi:x:0:\n" > /run/group
mount --bind /run/passwd /etc/passwd
mount --bind /run/group /etc/group
ip link set lo up
ip link add dmn0 type dummy
ip address add 192.0.2.1/24 dev dmn0
ip link set dmn0 multicast on up
exec dbus-run-session -- sh -eu -s -- "$@" <<'BUS'
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
if [ "${DMN_TEST_MANAGED_AVAHI:-0}" = 1 ]; then
  export DMN_TEST_AVAHI_CONFIG="$1"
  shift
  exec "$@"
fi
avahi-daemon --no-drop-root --no-chroot --no-rlimits --file="$1" &
daemon=$!
trap 'kill "$daemon" 2>/dev/null || true; wait "$daemon" 2>/dev/null || true' EXIT HUP INT TERM
shift
"$@"
BUS
NAMESPACE
