#!/usr/bin/env bash
# Gate 9: resumable server combat (P5-C / P2). Boots the f2_server on the aggro
# fixture map (klatoxcv) with the resumable-combat session gate ON, drives a real
# fight over the wire + debug CMD port, and asserts the four properties that are
# impossible without the beat-spanning session machine (see tools/resumable_probe.py).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="$ROOT/build/resumable-gate"
PROBE="$ROOT/tools/resumable_probe.py"
WIRE=9340
CMD=9341
SRV=""

fail() {
    echo "FAIL resumable — $1"
    tail -30 "$OUT/server.err" 2>/dev/null | sed 's/^/    srv| /'
    kill "$SRV" 2>/dev/null
    wait 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL resumable — no f2_server (build broken?)"; exit 1; }
[ -f "$PROBE" ] || { echo "FAIL resumable — no resumable_probe.py"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL resumable — no FO2/ assets"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/probe.out"
# A stale f2_server holding the ports makes boot fail spuriously.
pkill -x f2_server 2>/dev/null && sleep 1

# F2_SERVER_TURN_WAIT=1 forces the player barrier to wait (so the fight reliably
# reaches a dude turn we can inject into); F2_SERVER_TURN_IDLE_MS=1500 keeps the
# fight flowing if injection is late (idle auto-end-turn). PACE keeps it watchable
# and lets the CMD/wire keep up.
( cd "$GAME" && exec env \
    F2_SERVER_MAP=klatoxcv.map F2_SERVER_NET=$WIRE F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=5 F2_SERVER_TICKS=6000 \
    F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_TURN_WAIT=1 F2_SERVER_TURN_IDLE_MS=1500 \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

python3 "$PROBE" 127.0.0.1 $WIRE $CMD 30 >"$OUT/probe.out" 2>>"$OUT/server.err"
RC=$?
kill "$SRV" 2>/dev/null
wait 2>/dev/null

if [ "$RC" != 0 ]; then
    sed 's/^/    probe| /' "$OUT/probe.out"
    fail "resumable probe failed (rc=$RC)"
fi
grep -q "command 'aggro'" "$OUT/server.err" || fail "server never received the aggro command"
PASS_LINE=$(grep -a "RESUMABLE PROBE PASS" "$OUT/probe.out" | tail -1)
echo "PASS resumable — $PASS_LINE"
