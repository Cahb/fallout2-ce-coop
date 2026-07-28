#!/usr/bin/env bash
# MIRROR DIVERGENCE GATE — does the live wire carry enough to rebuild the server's world?
#
# ►►►► WHY THIS GATE EXISTS, AND WHAT EVERY OTHER GATE WAS MISSING. The suite has
# validated the live stream for a long time, and every one of those checks compares
# TILE AND ELEVATION. tools/replay.py's reconstructed object was literally
# `Obj(tile, elev)`. So a stream could be wrong about an object's proto, art frame,
# flags, hit points or entire inventory and 69 gates stayed green — which is exactly
# what happened, repeatedly, and each time it was found by a human clicking something
# in a live session and getting nothing:
#
#   * a dug grave rendered open but still resolved the CLOSED grave's proto, because
#     obj->pid was never a wire field at all;
#   * an armed charge kept the unarmed proto, so the verb naming it found nothing;
#   * scripted art FRAME swaps and per-object LIGHT never streamed until someone hit
#     them; GVARs likewise.
#
# All one sentence: "that field was never on the list." This gate ends the class by
# asking the server for its OWN view of every object (the `audit` command ->
# EVENT_STATE_AUDIT, src/state_audit.h) and diffing it against what the stream alone
# was able to reconstruct. A divergence here is a PROTOCOL hole, not a client bug:
# no viewer can be right about a field the wire never sent.
#
# ►► The audit record is derived from the Object struct, NOT from the delta tracker's
# field list — an oracle limited to the fields the tap already streams is blind in
# precisely the places the tap is. When a field is added to Object, add it to
# state_audit.h; this gate will then fail until the tap learns to stream it, which is
# the correct order of events.
set -u
ROOT="/mnt/NVME/Projects/fallout2-ce"
GAME="$ROOT/FO2"
OUT="$ROOT/build/audit_gate"
WIRE=9451
CMD=9452

fail() { echo "FAIL audit — $*"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/stream.bin"

pkill -x f2_server 2>/dev/null && sleep 1

( cd "$GAME" && exec env F2_SERVER_MAP=artemple.map F2_SERVER_NET=$WIRE F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=20 F2_SERVER_SMOOTH_WALK=1 F2_SERVER_TICKS=900 \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

# Capture the wire exactly as a viewer would receive it. The TEE env var is NOT
# usable here: the tee is attached AFTER acceptClients, so it misses the F2NS stream
# header and replay.py cannot parse the result.
( timeout 40 python3 "$ROOT/tools/net_capture.py" 127.0.0.1 $WIRE "$OUT/stream.bin" 15 >/dev/null 2>&1 ) &
CAP=$!
sleep 4

# Two audits with world activity in between: the first proves the baseline is
# reconstructible, the second that the DELTAS keep it that way. One audit alone
# would pass on a world nothing had happened to yet.
printf 'audit\n' | timeout 5 nc -q1 localhost $CMD >/dev/null 2>&1
sleep 3
printf 'give 41 2\n' | timeout 5 nc -q1 localhost $CMD >/dev/null 2>&1
sleep 3
printf 'audit\n' | timeout 5 nc -q1 localhost $CMD >/dev/null 2>&1
sleep 4

kill "$SRV" 2>/dev/null
wait "$CAP" 2>/dev/null
wait 2>/dev/null

[ -s "$OUT/stream.bin" ] || fail "captured no wire stream"
SENT=$(grep -ac "state audit sent" "$OUT/server.err")
[ "$SENT" -ge 2 ] || fail "server logged $SENT audits, expected 2 (the CMD channel did not deliver)"

REPORT="$OUT/report.txt"
python3 "$ROOT/tools/replay.py" --audit "$OUT/stream.bin" >"$REPORT" 2>&1
RC=$?

# rc 2 = nothing was checked. That must fail as loudly as a divergence: a gate that
# passes when it verified nothing is worse than no gate.
[ "$RC" != 2 ] || fail "$(head -1 "$REPORT")"
if [ "$RC" != 0 ]; then
    echo "---- divergences ----"
    head -25 "$REPORT"
    fail "the stream could not reproduce the server's own state (see $REPORT)"
fi

echo "PASS audit — $(grep -c '^AUDIT #' "$REPORT") audits, mirror reproduced the server exactly"
grep '^AUDIT #' "$REPORT" | sed 's/^/       /'
