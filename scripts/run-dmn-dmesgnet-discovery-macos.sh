#!/bin/sh
# Run the two-process mDNS-to-DMesgNet hello qualification client on macOS.
set -eu

program=${DMN_DMESGNET_DISCOVERY_QUALIFICATION:-build-qualification/dmn-discovery-dmesgnet-qualification}

usage() {
  echo "usage: $0 listen NODE PORT [INTERFACE] [TOKEN]" >&2
  echo "       $0 connect NODE PEER [INTERFACE] [TOKEN]" >&2
  exit 2
}

[ -x "$program" ] || {
  echo "qualification executable not found or not executable: $program" >&2
  exit 1
}

role=${1:-}
node=${2:-}
peer_or_port=${3:-}
[ "$role" = listen ] || [ "$role" = connect ] || usage
[ "$#" -ge 3 ] || usage

interface_name=${4:-${DMN_DISCOVERY_INTERFACE:-}}
token=${5:-qualification-token}
if [ -z "$interface_name" ]; then
  interface_name=$(route -n get -inet 224.0.0.251 |
    awk '/interface:/{print $2; exit}')
fi

[ -n "$interface_name" ] || {
  echo "unable to determine multicast interface; pass INTERFACE" >&2
  exit 1
}

case "$interface_name" in
  *[!0-9]*)
    interface=$(python3 -c \
      'import socket, sys; print(socket.if_nametoindex(sys.argv[1]))' \
      "$interface_name")
    ;;
  *)
    interface=$interface_name
    ;;
esac

[ -n "$interface" ] || {
  echo "unable to determine interface index for: $interface_name" >&2
  exit 1
}

if [ "$role" = listen ]; then
  exec "$program" listen "$node" "$peer_or_port" "$interface" "$token"
fi

exec "$program" connect "$node" "$peer_or_port" "$interface" "$token"
