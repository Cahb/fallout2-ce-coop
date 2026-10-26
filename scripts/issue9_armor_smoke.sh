#!/usr/bin/env bash
# Issue #9 check: `put` of a pid you are WEARING must move the spare copy, never the
# worn one (which would keep its AC bonus on you). Server-only, headless.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMD=9391
ERR="$ROOT/build/issue9_armor_smoke.err"
LEATHER=1   # PID_LEATHER_ARMOR

rm -f "$ERR"
( cd "$ROOT/FO2" && exec env \
    F2_SERVER_MAP=denbus1.map F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=400 \
    F2_SERVER_ACTIONS="20:give:$LEATHER:2" \
    timeout -k 3 30 "$ROOT/build/f2_server" ) > "$ERR" 2>&1 &
SRV=$!
send() { printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null; sleep 1; }

sleep 4
send "wear $LEATHER"
send "lootput $LEATHER"   # moves the spare
send "lootput $LEATHER"   # only the worn copy is left: must be refused
wait $SRV 2>/dev/null

grep -aE "f2_server: (wear|lootput)" "$ERR"
wearAc=$(grep -aoP 'f2_server: wear pid=\d+ rc=0 ac=\K-?\d+' "$ERR" | head -1)
first=$(grep -a "f2_server: lootput" "$ERR" | sed -n 1p)
second=$(grep -a "f2_server: lootput" "$ERR" | sed -n 2p)
fail=0
[ -n "$wearAc" ] || { echo "FAIL: wear did not succeed"; fail=1; }
echo "$first"  | grep -q "rc=0 movedWorn=0 stillWearing=1 ac=$wearAc " || { echo "FAIL: first put took the worn copy or changed AC"; fail=1; }
echo "$second" | grep -q "rc=-1 movedWorn=0 stillWearing=1 ac=$wearAc " || { echo "FAIL: second put moved the worn armor"; fail=1; }
[ $fail = 0 ] && echo "PASS: issue #9 — worn armor never leaves via put, AC stable at $wearAc"
exit $fail
