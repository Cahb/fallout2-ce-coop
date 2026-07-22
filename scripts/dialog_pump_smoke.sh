#!/usr/bin/env bash
# A2-ACTIVATE server-side smoke: prove the block-and-pump loop engages and drains
# live intents (not the pre-queued golden path). No net → pump services the CMD
# port. dtalk opens dialog; while the server is BLOCKED in the pump we send
# dsay/dend over the CMD port; the pump must pick them up and walk the nodes.
set -u
ROOT="/mnt/NVME/Projects/fallout2-ce"
GAME="$ROOT/FO2"
BIN="$ROOT/build/f2_server"
CMD=9377
ERR="$ROOT/build/pump_smoke.err"

pkill -x f2_server 2>/dev/null; sleep 1
rm -f "$ERR"

# No F2_SERVER_NET → haveNet=false → pump won't bail on "no clients" and drives
# via CMD (haveCmd). PACE slows beats so we have time to inject. dtalk:940 opens
# the Den Story Teller dialog at tick 40 (~40*40ms=1.6s in). TICKS cap keeps it
# from running forever if something wedges.
( cd "$GAME" && exec env \
    F2_SERVER_MAP=denbus1.map F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=400 \
    F2_DIALOG_STREAM=1 F2_DIALOG_TRACE=1 F2_NARRATE=1 \
    F2_SERVER_ACTIONS="40:dtalk:940" \
    DEBUGACTIVE=screen \
    timeout -k 3 25 "$BIN" ) > "$ERR" 2>&1 &
SRV=$!

# Wait past tick 40 so the dialog is open and the server is parked in the pump.
sleep 4
echo "--- injecting dsay/dend over CMD port while server is blocked in the pump ---"
for v in "dsay 0" "dsay 1" "dsay 0" "dend"; do
    printf '%s\n' "$v" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || \
    printf '%s\n' "$v" | nc -q1 127.0.0.1 $CMD 2>/dev/null
    sleep 1
done

wait $SRV 2>/dev/null
echo "======== server.err (dialog-relevant lines) ========"
grep -aE "block-and-pump ACTIVE|dtrace|command 'dsay'|command 'dend'|control dsay|control dend|DIALOG|gdialog|Story|Leanne|dialogNode|dialogEnd" "$ERR" | head -60
echo "======== tail ========"; tail -8 "$ERR"
pkill -x f2_server 2>/dev/null; true
