#!/usr/bin/env bash
# Slice-3 smoke: prove the BARTER STREAM events actually reach the wire.
#
# ►► WHY A WIRE CLIENT IS MANDATORY HERE. NetworkPresenter is installed only when
# F2_SERVER_NET is set AND a client connects (server_main.cc). With no socket the
# presenter is the base one, whose barterBegin/barterState/barterEnd are empty
# virtuals -- so the trade runs perfectly and emits NOTHING, and a smoke without a
# client "passes" while proving zero. That is what the first attempt did.
set -u
ROOT="/mnt/NVME/Projects/fallout2-ce"
GAME="$ROOT/FO2"
BIN="$ROOT/build/f2_server"
NET=9394
CMD=9395
ERR="$ROOT/build/barter_stream.err"
DUMP="$ROOT/build/barter_stream.dump"

pkill -x f2_server 2>/dev/null
pkill -f "nc 127.0.0.1 $NET" 2>/dev/null
sleep 1
rm -f "$ERR" "$DUMP"

(
    cd "$GAME" || exit 1
    exec env \
        F2_SERVER_MAP=denbus1.map F2_SERVER_CMD=$CMD F2_SERVER_NET=$NET \
        F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=1200 \
        F2_DIALOG_STREAM=1 F2_TRACE_EVENTS=1 F2_SERVER_DUMP="$DUMP" \
        F2_SERVER_ACTIONS="20:give:41:300,60:dtalk:47" \
        timeout -k 3 90 "$BIN"
) > "$ERR" 2>&1 &
SRV=$!

# Hold the wire open so NetworkPresenter installs and the pump's "no clients"
# bail never fires. Discard the bytes; the [barter] trace is the oracle.
sleep 1
nc 127.0.0.1 $NET > /dev/null 2>&1 &
NCPID=$!

send() {
    printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null ||
        printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null
}

sleep 8
for v in "dsay 0" "btake 259 1" "boffer 41 200" "bcommit 0" "bdone 0" "dend"; do
    echo "-> $v"
    send "$v"
    sleep 3
done

sleep 5
kill $NCPID 2>/dev/null
wait $SRV 2>/dev/null

echo "======== barter stream events ========"
grep -a "\[barter\]" "$ERR" || echo "(none emitted)"
echo "======== did the trade run at all? ========"
grep -a "command 'b\|dialog\] ENTER\|dialog\] LEAVE" "$ERR" | head
echo "======== dude caps (300 = no trade, 100 = traded) ========"
if [ -f "$DUMP" ]; then
    grep -aA3 "^dude_traits" "$DUMP" | grep -a "pid=0x00000029"
else
    echo "NO DUMP (server did not reach its tick cap)"
fi
