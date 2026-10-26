#!/usr/bin/env bash
# Issue #8 check: plain-looting an OWNED container must run its pickup_p_proc gate.
# Liz's box in Broken Hills (Hilbox.int, scripts.lst idx 666) blocks on a failed
# Steal roll. A low-skill dude trying N times must get refused at least once; before
# the fix the server granted every time (pickup_p_proc never ran).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMD=9392
ERR="$ROOT/build/issue8_owned_container_smoke.err"
DUMP="$ROOT/build/issue8_owned_container_smoke.dump"
MAP="${MAP:-broken1.map}"
IDX=666
TRIES=10

rm -f "$ERR" "$DUMP"
( cd "$ROOT/FO2" && exec env \
    F2_SERVER_MAP="$MAP" F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=500 F2_SERVER_DUMP="$DUMP" \
    timeout -k 3 35 "$ROOT/build/f2_server" ) > "$ERR" 2>&1 &
SRV=$!
send() { printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null; sleep 0.7; }

sleep 4
for _ in $(seq $TRIES); do send "lootscript $IDX"; done
wait $SRV 2>/dev/null

grep -aE "f2_server: lootscript|interact loot REFUSED" "$ERR" | head -25
tries=$(grep -ac "f2_server: lootscript $IDX — netId" "$ERR")
refused=$(grep -ac "f2_server: lootscript $IDX — .*granted=0" "$ERR")
if [ "$tries" = 0 ]; then echo "FAIL: container not found on $MAP (try MAP=broken2.map)"; exit 1; fi
# lootOpenCheck has exactly two BLOCKED paths: a CLOSED multi-frame container, or
# a pickup_p_proc override. With 1-frame art (or the container opened) a refusal can
# only be the script. (Its map_var 18 alarm is consumed by Liz's heartbeat at once,
# so it is not a usable proof in the end-of-run dump.)
scriptRefusals=$(grep -aE "f2_server: lootscript $IDX — .*artFrames=1 granted=0" "$ERR" | wc -l)
openedRefusals=$(grep -ac "interact loot REFUSED .*useRc=0 openRc=2 frame=1" "$ERR")
proved=$((scriptRefusals + openedRefusals))
echo "refusals provably from pickup_p_proc: $proved"
if [ "$proved" -ge 1 ]; then
    echo "PASS: issue #8 — owner gate ran: $refused/$tries plain loots refused"
else
    echo "FAIL: $tries plain loots, $refused refused, none provably by pickup_p_proc"; exit 1
fi
