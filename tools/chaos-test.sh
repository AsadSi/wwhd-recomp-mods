#!/bin/bash
# tools/chaos-test.sh [profile] [seed]: online co-op over a bad network, host and guest on this Mac (tools/duo.sh, CHAOS=).
# Checks, from the games' traces and the relay's counts: the guest joins; progress set on each game reaches the other; the
# relay really did drop packets each way; 3 s of nothing (under the 5 s timeout) leaves the session up; 7 s of nothing ends it
# on both, the guest back at the title, neither game crashed. Exit 0 only if all pass. Traces stay in build/try.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROFILE=${1:-bad_wifi} SEED=${2:-}
cd "$ROOT"
fails=0
say() { echo "$1: $2"; [ "$1" = PASS ] || fails=$((fails + 1)); }
# wait_for <seconds> <file> <text>
wait_for() { for _ in $(seq "$(($1 * 2))"); do grep -q "$3" "$2" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
check() { if wait_for "$1" "$2" "$3"; then say PASS "$4"; else say FAIL "$4 (no \"$3\" in $2 after $1 s)"; fi; }
call() { python3 -I tools/chaos/chaos.py call --rpc-port "$RPC" "$@"; }
alive() { [ "$(tools/duo.sh count)" = 2 ]; }

rm -f build/try/coop-host-*.txt build/try/coop-guest-*.txt
CHAOS=$PROFILE CHAOS_SEED=$SEED GUEST_AFTER=12 WANDER=1 STATE_FILE=save/coop-dock.wwstate \
  HOST_ENV="WWHD_COOP_TEST_PROGRESS=0C8:01:01" GUEST_ENV="WWHD_COOP_TEST_PROGRESS=0C9:01:01" tools/duo.sh coop || exit 1
RPC=$(python3 -c 'import json; print(json.load(open("build/try/chaos.json"))["rpc"])')
trap 'tools/duo.sh stop' EXIT
until H=$(ls build/try/coop-host-*.txt 2>/dev/null) && G=$(ls build/try/coop-guest-*.txt 2>/dev/null); do sleep 0.5; done
echo "$PROFILE: $H $G"

check 120 "$G" "coop: welcomed as player" "guest joined"
check 120 "$G" "coop: the host's world" "guest took the host's world"
check 90 "$H" "coop: progress in:" "guest's progress reached the host"
check 90 "$G" "coop: progress in:" "host's progress reached the guest"
call stats > build/try/chaos-stats.json
python3 - "$PROFILE" build/try/chaos-stats.json <<'PY' && say PASS "the relay's counts fit the profile" || say FAIL "the relay's counts do not fit the profile"
import json, sys
profile, s = sys.argv[1], json.load(open(sys.argv[2]))
for d in ("c2s", "s2c"):
    print(f"  {d}: seen {s[d]['seen']}, dropped {s[d]['dropped']}, duplicated {s[d]['duplicated']}, {s[d]['reasons']}")
ok = all(s[d]["forwarded"] > 300 for d in s)
if profile != "lan":
    ok = ok and all(s[d]["dropped"] > 0 for d in s)  # no drops: this run proved nothing about loss
sys.exit(0 if ok else 1)
PY

call disconnect '{"duration_ms":3000}' > /dev/null
sleep 8
if grep -q "Connection lost" "$H" "$G" || ! alive; then say FAIL "a 3 s outage ended the session"; else say PASS "a 3 s outage left the session up"; fi

call partition '{"dir":"both","on":true,"duration_ms":7000}' > /dev/null
check 12 "$H" "left: Connection lost" "a 7 s outage: the host lost the guest"
check 12 "$G" "coop: back to the title" "a 7 s outage: the guest went to the title"
check 30 "$G" "coop: at the title" "the guest reached the title"
sleep 3
if alive; then say PASS "both games still running"; else say FAIL "a game is gone (crash?)"; fi

echo "$PROFILE: $fails failed"
[ "$fails" = 0 ]
