#!/usr/bin/env bash
# MID-STREAM JOIN gate (STEP 5). A second SDL-dummy viewer connects to a live,
# already-streaming server; PASS requires the joiner's baseline tripwire to
# score clean (ok>0 bad=0 — every SNAPSHOT_OBJECT matches its blob-loaded world,
# i.e. the join-time netId re-walk aligned) and the first viewer to survive the
# rebaseline broadcast (world reload) without dropping.
#
# Oracle notes: the tripwire line is debugPrint'd by the JOINER only (viewer A
# runs without DEBUGACTIVE — two processes share FO2/debug.log and the later
# boot truncates it). Viewer A's health is asserted via the server's stderr
# (joined line present, no drop before teardown) and its live PID.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="$ROOT/build/midjoin-gate"
WIRE=9320
CMD=9321
SRV=""
VIEW_A=""
VIEW_B=""

fail() {
    echo "FAIL midjoin — $1"
    cat "$OUT/server.err" 2>/dev/null | tail -5
    kill "$VIEW_A" "$VIEW_B" "$SRV" 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL midjoin — no f2_server"; exit 1; }
[ -x "$ROOT/build/fallout2-ce" ] || { echo "FAIL midjoin — no fallout2-ce"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL midjoin — no FO2/ assets"; exit 1; }

mkdir -p "$OUT"
rm -f "$GAME/debug.log" "$OUT/server.err"

( cd "$GAME" && exec env F2_SERVER_MAP=artemple.map F2_SERVER_NET=$WIRE F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=20 F2_SERVER_SMOOTH_WALK=1 F2_SERVER_TICKS=5000000 \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

# Viewer A: joins at stream start (the classic path). No DEBUGACTIVE (see above).
( cd "$GAME" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    F2_CLIENT_CONNECT=127.0.0.1:$WIRE F2_JOIN_TMP_CLIENT=/tmp/f2ce_gate_viewA.bin \
    "$ROOT/build/fallout2-ce" >/dev/null 2>&1 ) &
VIEW_A=$!
sleep 3
kill -0 "$VIEW_A" 2>/dev/null || fail "viewer A died before join"

# Churn the world so the joiner's blob is a genuinely mid-stream cut (objects
# off their boot tiles → the stale-order netId trap this gate exists to catch).
# ±200 = one hex row: ±8 on artemple is blocked, _make_path yields 0 steps and
# the "churn" silently never moved anything (found via STEP-6 offset sweep).
printf 'walk 200\n' >/dev/tcp/127.0.0.1/$CMD || fail "cmd inject 1 failed"
sleep 2

# Viewer B: the mid-stream joiner. DEBUGACTIVE=log → FO2/debug.log is B's.
( cd "$GAME" && exec env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy DEBUGACTIVE=log \
    F2_CLIENT_CONNECT=127.0.0.1:$WIRE F2_JOIN_TMP_CLIENT=/tmp/f2ce_gate_viewB.bin \
    "$ROOT/build/fallout2-ce" >/dev/null 2>&1 ) &
VIEW_B=$!
sleep 4

# Post-join churn: both viewers must be consuming the same live stream.
printf 'walk -200 1\n' >/dev/tcp/127.0.0.1/$CMD || fail "cmd inject 2 failed"
sleep 2

kill -0 "$VIEW_A" 2>/dev/null || fail "viewer A died after the join rebaseline"
kill -0 "$VIEW_B" 2>/dev/null || fail "viewer B (joiner) died"

kill "$VIEW_A" "$VIEW_B" "$SRV" 2>/dev/null
wait 2>/dev/null

grep -q "client joined mid-stream (2 total)" "$OUT/server.err" \
    || fail "server never accepted the mid-stream join"
grep -q "client dropped on write" "$OUT/server.err" \
    && fail "a client dropped on write during the run"

TRIP=$(grep -a "baseline tripwire" "$GAME/debug.log" 2>/dev/null | tail -1)
[ -n "$TRIP" ] || fail "joiner logged no tripwire line (blob never landed?)"
echo "$TRIP" | grep -q "bad=0" || fail "tripwire misalignment: $TRIP"
echo "$TRIP" | grep -Eq "ok=[1-9]" || fail "tripwire scored zero objects: $TRIP"
grep -aq "world loaded (load #1)" "$GAME/debug.log" || fail "joiner never loaded a world"

rm -f "$GAME/debug.log" /tmp/f2ce_gate_viewA.bin /tmp/f2ce_gate_viewB.bin
echo "PASS midjoin — joiner tripwire clean ($TRIP), viewer A survived the rebaseline"
