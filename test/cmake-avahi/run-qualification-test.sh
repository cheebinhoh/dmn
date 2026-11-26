#!/bin/sh
# Exercise only test-owned registrations inside the isolated Avahi namespace.
set -eu

program=$1
dir=$(mktemp -d /tmp/dmn-qualification.XXXXXX)
advertiser=
browser=
cleanup() {
  result=$?
  if [ "$result" -ne 0 ]; then
    if [ -f "$dir/advertiser.log" ]; then cat "$dir/advertiser.log"; fi
    if [ -f "$dir/browser.log" ]; then cat "$dir/browser.log"; fi
  fi
  if [ -n "$advertiser" ]; then
    kill "$advertiser" 2>/dev/null || true
    wait "$advertiser" 2>/dev/null || true
  fi
  if [ -n "$browser" ]; then
    kill "$browser" 2>/dev/null || true
    wait "$browser" 2>/dev/null || true
  fi
  rm -f "$dir/input" "$dir/advertiser.log" "$dir/browser.log"
  rmdir "$dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkfifo "$dir/input"
exec 3<>"$dir/input"
interface=$(ip -o link show dmn0 | cut -d: -f1)
"$program" advertise dmn-qualification-isolated "$interface" \
  <"$dir/input" >"$dir/advertiser.log" 2>&1 &
advertiser=$!
for i in $(seq 1 200); do
  if grep -q '^ESTABLISHED:' "$dir/advertiser.log"; then break; fi
  if ! kill -0 "$advertiser" 2>/dev/null; then exit 1; fi
  sleep 0.1
done
grep -q '^ESTABLISHED:' "$dir/advertiser.log"
"$program" browse dmn-qualification-isolated dmn-test.local "$interface" \
  >"$dir/browser.log" 2>&1 &
browser=$!
for i in $(seq 1 400); do
  if grep -q '^READY:' "$dir/browser.log"; then break; fi
  if ! kill -0 "$browser" 2>/dev/null; then exit 1; fi
  sleep 0.1
done
grep -q '^READY:' "$dir/browser.log"
printf '\n' >&3
wait "$advertiser"
advertiser=
wait "$browser"
browser=
cat "$dir/advertiser.log" "$dir/browser.log"
