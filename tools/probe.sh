#!/bin/bash
# Usage: tools/probe.sh '<command>' ['<command>'...]   Asks the running game (started by tools/try.sh, which sets
# WWHD_PROBE) and prints its answers: peek / str / call / poke / names (runtime/src/mods/probe.h).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="$ROOT/build/try/probe${TAG:+-$TAG}.txt"
rm -f "$P.out"
printf '%s\n' "$@" > "$P"
for _ in $(seq 100); do [ -f "$P.out" ] && { cat "$P.out"; exit 0; }; sleep 0.05; done
echo "No answer in 5 s: is the game running in play (tools/try.sh)?"; exit 1
