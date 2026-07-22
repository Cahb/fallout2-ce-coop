#!/usr/bin/env bash
# Server-side smoke for the BARTER block-and-pump loop (the barter twin of
# dialog_pump_smoke.sh). Proves the pump ENGAGES and drains LIVE intents, which
# the goldens cannot: every barter golden pre-queues its whole trade before the
# conversation opens, so it passes even with no pump at all.
#
# No F2_SERVER_NET -> haveNet=false, so the pump neither bails on "no clients"
# nor needs one; it services the CMD port (haveCmd). dtalk opens Tubby's dialog,
# dsay picks the barter option, and THEN -- while the server is parked inside
# inventoryOpenTrade -- we inject the trade one verb at a time and watch it move.
#
# The tell that the pump is real: the trade survives an EMPTY intent queue
# between injections. Without a pump inventoryOpenTrade breaks the instant the
# queue runs dry, so the trade would be over before the second verb arrived.
set -u
ROOT="/mnt/NVME/Projects/fallout2-ce"
GAME="$ROOT/FO2"
BIN="$ROOT/build/f2_server"
CMD=9378
ERR="$ROOT/build/barter_pump_smoke.err"
DUMP="$ROOT/build/barter_pump_smoke.dump"

pkill -x f2_server 2>/dev/null; sleep 1
rm -f "$ERR" "$DUMP"

# give:41:300 stocks the dude with 300 caps (the fixture's opening balance) so the
# offer below can clear Tubby's asking price.
( cd "$GAME" && exec env \
    F2_SERVER_MAP=denbus1.map F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=600 \
    F2_DIALOG_STREAM=1 F2_SERVER_DUMP="$DUMP" \
    F2_SERVER_ACTIONS="20:give:41:300,40:dtalk:47" \
    DEBUGACTIVE=screen \
    timeout -k 3 40 "$BIN" ) > "$ERR" 2>&1 &
SRV=$!

send() {
    printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || \
    printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null
}

# Past tick 40: dialog open, server parked in the DIALOG pump.
sleep 4
echo "--- picking the barter option (server is blocked in the dialog pump) ---"
send "dsay 0"; sleep 2

echo "--- injecting the trade ONE VERB AT A TIME, queue empty in between ---"
for v in "btake 259 1" "boffer 41 200" "bcommit 0" "bdone 0"; do
    echo "    -> $v"
    send "$v"
    sleep 2   # long enough that the queue is DRY before the next verb
done

sleep 2
send "dend"
wait $SRV 2>/dev/null

echo "======== pump / trade lines ========"
grep -aE "block-and-pump ACTIVE|\[barter\]|command 'b(take|offer|commit|done)'|command 'dsay'|PUMP BAIL" "$ERR" | head -40

echo "======== outcome (dude caps + goods) ========"
if [ -f "$DUMP" ]; then
    # pid 0x29 = 41 (caps), 0x103 = 259 (the goods). The dude block is the two
    # inv lines directly under dude_traits.
    grep -aA3 "^dude_traits" "$DUMP" | head -4
    echo "--- Tubby's store box (id=22): the proceeds land HERE, not on Tubby ---"
    # Report, do not assert a literal. denbus1 is AI-heavy and this smoke sets no
    # seed and no fake clock, so the box's STARTING balance moves run to run (161
    # here, 152 in the golden fixture). What must hold is the DELTA: the dude is
    # down exactly what the box is up. Run the same script with the four b* verbs
    # removed to get the control baseline.
    grep -aA8 "^obj id=22 " "$DUMP" | grep -a "pid=0x00000029" | head -1
else
    echo "NO DUMP — server did not reach its tick cap"
fi
