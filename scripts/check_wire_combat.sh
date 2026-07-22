#!/usr/bin/env bash
# Gate 10: P3 wire combat. Boots f2_server on the aggro fixture (klatoxcv) with
# the resumable-combat session gate ON but NO F2_SERVER_TURN_WAIT, then drives a
# fight through the claim-gated VIEWER WIRE (server_control.cc) — claim + a bare
# `cattack`, exactly what the SDL viewer sends. Asserts (tools/wire_combat_probe.py):
#   - the wire client's claim is honored and the barrier waits on it (no TURN_WAIT),
#   - a bare `cattack` up the wire mid-fight resolves as a dude attack.
# The inverse of gate 9 (which injects via the debug CMD port); this proves the
# actual viewer path end to end.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="$ROOT/build/wire-combat-gate"
PROBE="$ROOT/tools/wire_combat_probe.py"
WIRE=9342
CMD=9343
SRV=""

fail() {
    echo "FAIL wire-combat — $1"
    tail -30 "$OUT/server.err" 2>/dev/null | sed 's/^/    srv| /'
    kill "$SRV" 2>/dev/null
    wait 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL wire-combat — no f2_server (build broken?)"; exit 1; }
[ -f "$PROBE" ] || { echo "FAIL wire-combat — no wire_combat_probe.py"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL wire-combat — no FO2/ assets"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/probe.out"
pkill -x f2_server 2>/dev/null && sleep 1

# NO F2_SERVER_TURN_WAIT: the connected claimant alone must hold the dude's turn
# open. F2_SERVER_TURN_IDLE_MS=2000 auto-ends the turn if injection is late.
( cd "$GAME" && exec env \
    F2_SERVER_MAP=klatoxcv.map F2_SERVER_NET=$WIRE F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=5 F2_SERVER_TICKS=6000 \
    F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_TURN_IDLE_MS=2000 \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

python3 "$PROBE" 127.0.0.1 $WIRE $CMD 28 >"$OUT/probe.out" 2>>"$OUT/server.err"
RC=$?
kill "$SRV" 2>/dev/null
wait 2>/dev/null

if [ "$RC" != 0 ]; then
    sed 's/^/    probe| /' "$OUT/probe.out"
    fail "wire-combat probe failed (rc=$RC)"
fi
grep -q "control claimed by session" "$OUT/server.err" || fail "server never registered the wire claim"
grep -q "control cattack" "$OUT/server.err" || fail "server never received a wire cattack"
PASS_LINE=$(grep -a "WIRE COMBAT PROBE PASS" "$OUT/probe.out" | tail -1)
echo "PASS wire-combat — $PASS_LINE"
