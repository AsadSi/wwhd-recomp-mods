#!/bin/bash
# [PLAYERS=2..4] [WANDER=1] [HOST_SAVE=1] [GUEST_AFTER=s] tools/duo.sh [try.sh case] [port]: online co-op on one Mac (HOST_SAVE: the host
# plays a Quest Log you pick, not the test state; WANDER: the guests move by themselves). Copies of the game (try.sh <case>, default coop):
# the host on <port> (7777), then a guest joining it on 127.0.0.1, each with its own user folder, live pad
# (build/try/pad-host.txt, pad-guest.txt), screenshots (build/try/shots-host, shots-guest) and trace
# (build/try/<case>-host-*.txt, <case>-guest-*.txt). The guest plays a copy of save/ (build/try/save-guest), which
# its saves would go to (none do: a guest's are blocked), starting GUEST_AFTER seconds (10) after the one before, and takes no keyboard
# (WWHD_NO_HOST_INPUT; GUEST_KEYS=1: it does, whichever window is in front gets the keys): its pad file drives
# it (HOST_ENV, GUEST_ENV: more variables for each). A screenshot of both at once: touch build/try/shot-host.now build/try/shot-guest.now. Stop: tools/duo.sh stop.
# CHAOS=<profile> [CHAOS_SEED=n]: the guests reach the host through a bad network (tools/chaos: lan good_wifi bad_wifi mobile_tether transatlantic hell; its log
# build/try/chaos.jsonl, its ports build/try/chaos.json). Change it live: python3 -I tools/chaos/chaos.py call --rpc-port <rpc> set_profile '{"profile":"hell"}'.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ours() { for p in $(pgrep -x wwhd); do [ "$(lsof -a -p "$p" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p')" = "$ROOT" ] && echo "$p"; done; }
stop_chaos() { [ -f "$ROOT/build/try/chaos.pid" ] && kill "$(cat "$ROOT/build/try/chaos.pid")" 2>/dev/null; rm -f "$ROOT/build/try/chaos.pid" "$ROOT/build/try/chaos.json"; }
if [ "${1:-}" = stop ]; then for p in $(ours); do kill "$p"; done; stop_chaos; exit 0; fi
if [ "${1:-}" = count ]; then ours | wc -l | tr -d ' '; exit 0; fi
[ -n "$(ours)" ] && { echo "A game of this repo is already running; close it first."; exit 1; }
CASE=${1:-coop} PORT=${2:-7777} PLAYERS=${PLAYERS:-2}
cd "$ROOT"
JOIN=$PORT
if [ -n "${CHAOS:-}" ]; then
  stop_chaos
  python3 -I tools/chaos/chaos.py relay --target "127.0.0.1:$PORT" --profile "$CHAOS" ${CHAOS_SEED:+--seed "$CHAOS_SEED"} --log build/try/chaos.jsonl --info-file build/try/chaos.json > /dev/null &
  echo $! > build/try/chaos.pid
  until [ -s build/try/chaos.json ]; do sleep 0.1; done
  JOIN=$(python3 -c 'import json; print(json.load(open("build/try/chaos.json"))["listen"])')
fi
TAG=host OWN_SAVE=${HOST_SAVE:-} MORE="WWHD_COOP=host:$PORT WWHD_COOP_NAME=Host ${HOST_ENV:-}" tools/try.sh "$CASE" > build/try/duo-host.log 2>&1 &
for n in $([ "$PLAYERS" -ge 2 ] && seq 2 "$PLAYERS"); do  # (macOS seq 2 1 counts down) guest, guest2, guest3: each its own tag, save copy, pad, shots and trace
  K=$([ "$n" -gt 2 ] && echo $((n - 1))); G=guest$K
  rm -rf "$ROOT/build/try/save-$G" && cp -R "$ROOT/save" "$ROOT/build/try/save-$G"
  sleep "${GUEST_AFTER:-10}"
  TAG=$G SECOND=1 SAVE="$ROOT/build/try/save-$G" MORE="WWHD_COOP=join:127.0.0.1:$JOIN WWHD_COOP_NAME=Guest$K $([ -n "${GUEST_KEYS:-}" ] || echo WWHD_NO_HOST_INPUT=1) ${GUEST_ENV:-}" \
    tools/try.sh "$CASE" > "build/try/duo-$G.log" 2>&1 &
done
# WANDER=1: each guest walks about, swings and spin-attacks by itself (its live pad), to watch its puppet on the others
if [ -n "${WANDER:-}" ]; then
  (while [ -n "$(ours)" ]; do
     for f in build/try/pad-guest*.txt; do
       case $((RANDOM % 6)) in
         0|1|2) echo "0 $(( RANDOM % 3 - 1 )).0 $(( RANDOM % 3 - 1 )).0" > "$f" ;;
         3) echo "4000 0 0" > "$f" ;;
         4) echo "4000 0 0" > "$f"; sleep 1.5 ;;
         *) echo "0 0 0" > "$f" ;;
       esac
     done
     sleep 0.6
   done) > /dev/null 2>&1 &
fi
sleep 2
echo "Host and $((PLAYERS - 1)) guest(s) started (try.sh $CASE, port $PORT): build/try/duo-host.log, duo-guest*.log"
