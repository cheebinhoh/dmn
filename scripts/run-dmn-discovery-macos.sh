#!/bin/sh
# Run the installed discovery qualification client on macOS.
set -eu

program=${DMN_DISCOVERY_QUALIFICATION:-build-qualification/dmn-discovery-qualification}
interface_name=${DMN_DISCOVERY_INTERFACE:-}

usage() {
  echo "usage: $0 advertise INSTANCE [INTERFACE]" >&2
  echo "       $0 browse INSTANCE HOSTNAME [INTERFACE]" >&2
  exit 2
}

[ -x "$program" ] || {
  echo "qualification executable not found or not executable: $program" >&2
  exit 1
}

role=${1:-}
instance=${2:-}
case "$role" in
  advertise)
    [ "$#" -eq 2 ] || [ "$#" -eq 3 ] || usage
    ;;
  browse)
    [ "$#" -eq 3 ] || [ "$#" -eq 4 ] || usage
    hostname=$3
    ;;
  *)
    usage
    ;;
esac

if [ "$role" = advertise ] && [ "$#" -eq 3 ]; then
  interface_name=$3
elif [ "$role" = browse ] && [ "$#" -eq 4 ]; then
  interface_name=$4
fi

if [ -z "$interface_name" ]; then
  interface_name=$(route -n get -inet 224.0.0.251 |
    awk '/interface:/{print $2; exit}')
fi

[ -n "$interface_name" ] || {
  echo "unable to determine the multicast interface; set DMN_DISCOVERY_INTERFACE" >&2
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

if [ "$role" = advertise ]; then
  advertiser_hostname="$(scutil --get LocalHostName).local"
  printf 'ADVERTISER_HOSTNAME: %s\n' "$advertiser_hostname" >&2
  exec "$program" advertise "$instance" "$interface"
fi

exec "$program" browse "$instance" "$hostname" "$interface"
