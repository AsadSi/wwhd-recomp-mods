#!/bin/bash
# tools/duo.sh [try.sh case] [port]: online co-op on one Mac. Two copies of the game (try.sh <case>, default coop):
# the host on <port> (7777), then a guest joining it on 127.0.0.1, each with its own user folder, live pad
# (build/try/pad-host.txt, pad-guest.txt), screenshots (build/try/shots-host, shots-guest) and trace
# (build/try/<case>-host-*.txt, <case>-guest-*.txt). The guest plays a copy of save/ (build/try/save-guest), which
# its saves would go to (none do: a guest's are blocked), and takes no keyboard (WWHD_NO_HOST_INPUT): its pad file drives
# it (HOST_ENV, GUEST_ENV: more variables for each). A screenshot of both at once: touch build/try/shot-host.now build/try/shot-guest.now. Stop: tools/duo.sh stop.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ours() { for p in $(pgrep -x wwhd); do [ "$(lsof -a -p "$p" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')" = "$ROOT" ] && echo "$p"; done; }
if [ "${1:-}" = stop ]; then for p in $(ours); do kill "$p"; done; exit 0; fi
[ -n "$(ours)" ] && { echo "A game of this repo is already running; close it first."; exit 1; }
CASE=${1:-coop} PORT=${2:-7777}
rm -rf "$ROOT/build/try/save-guest" && cp -R "$ROOT/save" "$ROOT/build/try/save-guest"
cd "$ROOT"
TAG=host MORE="WWHD_COOP=host:$PORT WWHD_COOP_NAME=Host ${HOST_ENV:-}" tools/try.sh "$CASE" > build/try/duo-host.log 2>&1 &
sleep 10
TAG=guest SECOND=1 SAVE="$ROOT/build/try/save-guest" MORE="WWHD_COOP=join:127.0.0.1:$PORT WWHD_COOP_NAME=Guest WWHD_NO_HOST_INPUT=1 ${GUEST_ENV:-}" \
  tools/try.sh "$CASE" > build/try/duo-guest.log 2>&1 &
sleep 2
echo "Host and guest started (try.sh $CASE, port $PORT): build/try/duo-host.log, duo-guest.log"
