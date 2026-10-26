#!/usr/bin/env bash
# Issue #10 check: after the Modoc toilet is blown (gvar 297 bit 1), the shit-covered
# B&B is ELEVATION 1 of modinn (map 19); MODINN's map_enter moves only gDude there
# (override_map_start). When a NON-HOST player initiates the transition, gDude is
# that player for the load, and every other actor — the host included — must still
# be re-planted beside it. Before the fix the host stayed on elevation 0: empty map.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMD=9393
ERR="$ROOT/build/issue10_modoc_toilet_smoke.err"
INITIATOR="${INITIATOR:-1}"   # 1 = the bug case; 0 = control (host initiates)

rm -f "$ERR"
( cd "$ROOT/FO2" && exec env \
    F2_SERVER_MAP=modshit.map F2_SERVER_CMD=$CMD F2_SERVER_PLAYERS=2 \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=400 \
    timeout -k 3 30 "$ROOT/build/f2_server" ) > "$ERR" 2>&1 &
SRV=$!
send() { printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null; sleep 1; }

sleep 4
send "setgvar 297 1"          # toilet blown
send "actors"
send "entermapas $INITIATOR 19"
sleep 2
send "actors"
wait $SRV 2>/dev/null

grep -aE "f2_server: (setgvar|entermapas|actors)" "$ERR"
after=$(grep -a "f2_server: actors map=19 " "$ERR")
if [ -z "$after" ]; then echo "FAIL: never arrived on map 19"; exit 1; fi
n=$(echo "$after" | wc -l)
bad=$(echo "$after" | grep -vc "elev=1$")
if [ "$n" -ge 2 ] && [ "$bad" = 0 ]; then
    echo "PASS: issue #10 — all $n actors on elevation 1 (initiator slot $INITIATOR)"
else
    echo "FAIL: $bad of $n actors not on elevation 1 (initiator slot $INITIATOR)"; exit 1
fi
