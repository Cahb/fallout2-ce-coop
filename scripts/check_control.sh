#!/usr/bin/env bash
# CONTROLLABLE-CLIENT gate (STEP 6). A raw TCP wire client drives the f2_server
# CONTROL plane end to end: it connects, consumes the join baseline, then sends the
# upstream control lines `claim` + `mv <tile> 0` on the SAME socket. PASS requires a
# MOVE event for the dude's netId (netId 1) to appear in the stream afterward — i.e.
# the server accepted the claim and executed the move AUTHORITATIVELY, and it rode
# the wire back out. FAIL if the server never emits the dude MOVE (claim/mv path
# broken), or if the binaries are missing (a gate that passes on a broken build is
# worthless — check.sh builds first and aborts before this runs, and this asserts
# the artifact exists besides).
#
# Deterministic map: artemple (verified clean + walkable under f2_server; +8 tiles
# is the known-walkable churn the mid-join gate already uses). Smooth walk on so the
# move steps tile-by-tile and emits MOVE beats a wire consumer can see.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="$ROOT/build/control-gate"
PROBE="$ROOT/tools/control_probe.py"
WIRE=9330
SRV=""

fail() {
    echo "FAIL control — $1"
    tail -20 "$OUT/server.err" 2>/dev/null | sed 's/^/    srv| /'
    kill "$SRV" 2>/dev/null
    wait 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL control — no f2_server (build broken?)"; exit 1; }
[ -f "$PROBE" ] || { echo "FAIL control — no control_probe.py"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL control — no FO2/ assets"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/probe.out"

# A stale f2_server holding the port makes boot fail spuriously.
pkill -x f2_server 2>/dev/null && sleep 1

( cd "$GAME" && exec env F2_SERVER_MAP=artemple.map F2_SERVER_NET=$WIRE \
    F2_SERVER_PACE_MS=20 F2_SERVER_SMOOTH_WALK=1 F2_SERVER_TICKS=5000000 \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

# Drive the control plane. The probe connects, reads the baseline, sends claim+mv,
# and exits 0 the instant it sees the dude's authoritative MOVE on the wire.
python3 "$PROBE" 127.0.0.1 $WIRE 15 >"$OUT/probe.out" 2>>"$OUT/server.err"
RC=$?

kill "$SRV" 2>/dev/null
wait 2>/dev/null

[ "$RC" = 0 ] || fail "probe saw no authoritative dude MOVE after claim+mv (rc=$RC)"
grep -q "control claimed by session" "$OUT/server.err" \
    || fail "server never logged the claim"
grep -q "control mv tile=" "$OUT/server.err" \
    || fail "server never executed the mv"

PASS_LINE=$(grep -a "CONTROL PROBE PASS" "$OUT/probe.out" | tail -1)
echo "PASS control — $PASS_LINE"
