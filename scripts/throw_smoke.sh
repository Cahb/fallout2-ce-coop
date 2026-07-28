#!/usr/bin/env bash
# THROW SMOKE — evidence for PRESENTATION_PACING_DESIGN.md §12 (projectiles/throwables).
# NOT A GATE: it uses wall-clock sleeps and reads TRACES, so it proves the mechanism
# fires, not that the picture is right. The picture needs the owner's eyes.
#
# Why it exists: there is NO throw fixture anywhere in the golden suite, and the throw
# path cannot be exercised headlessly — `clientViewerActive()` is false in the headless
# client probe, so every deferral/mint in §12 is inert there. This boots a REAL dedicated
# server plus a REAL viewer client on dummy video (which IS viewer-active), throws a
# spear, and reads the [adopt]/[preplay] traces back.
#
#   scripts/throw_smoke.sh                       # spear on artemple (the known-good case)
#   OUT=/tmp/t2 scripts/throw_smoke.sh           # keep the logs somewhere else
#   MAP=klatoxcv.map TPID=45 HITMODE=2 ...       # other weapon/map (see the TRAPS below)
#
# WHAT TO READ (all under F2_TRACE_EVENTS, client stderr):
#   [adopt] PARK type=4/5 net=N ... live=0   a CONNECT(4)/DISCONNECT(5) for the thrown
#                                            item arrived BEFORE its flight played, and
#                                            was parked instead of applied. type=5 with
#                                            live=0 is the bug §12 exists to fix.
#   [adopt] MINT net=N ... obj=ok            the transient minted at EXECUTE, one clock.
#   [preplay] refsDROPPED=0 transientsMinted=1   the flight actually replayed.
# The pre-§12-step-2 signature, for comparison, was refsDROPPED=5 transientsMinted=0
# with the [disc] line landing BEFORE the [preplay] instead of after.
#
# ►► TRAPS (both cost a run to find):
#  1. `wield N` picks the FIRST weapon in the dude's inventory, not the one you just
#     gave — so give the throwable to a dude who carries nothing better.
#  2. A Throwing Knife (pid 45) makes the premade dude fight UNARMED: the server logs
#     `inven_wield FAILED (art missing) ... weapAnim=1`, so no throw is ever attempted.
#     The Spear (pid 7, weapAnim=4, HITMODE=3 = secondary = throw) has art and works.
#     If you see no [adopt] lines at all, grep the server log for inven_wield FIRST.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="${OUT:-$ROOT/build/throw-smoke}"
WIRE="${WIRE_PORT:-9360}"
CMD="${CMD_PORT:-9361}"
MAP="${MAP:-artemple.map}"
TPID="${TPID:-7}"        # 7 = Spear
HITMODE="${HITMODE:-3}"  # 3 = RIGHT_WEAPON_SECONDARY = throw, for a spear

[ -x "$ROOT/build/f2_server" ] || { echo "no f2_server (build broken?)"; exit 1; }
[ -x "$ROOT/build/fallout2-ce" ] || { echo "no fallout2-ce (build broken?)"; exit 1; }
[ -d "$GAME" ] || { echo "no FO2/ assets"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"
pkill -x f2_server 2>/dev/null; pkill -x fallout2-ce 2>/dev/null; sleep 1

( cd "$GAME" && exec env \
    F2_SERVER_MAP="$MAP" F2_SERVER_NET="$WIRE" F2_SERVER_CMD="$CMD" \
    F2_SERVER_PACE_MS=30 F2_SERVER_TICKS=12000 \
    F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_PRES_RECORD=1 \
    F2_SERVER_SMOOTH_WALK=1 F2_SERVER_TURN_IDLE_MS=4000 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 2
kill -0 "$SRV" 2>/dev/null || { echo "SERVER DIED AT BOOT"; tail -20 "$OUT/server.err"; exit 1; }

# A REAL viewer, not the headless probe — this is the whole point (clientViewerActive()).
( cd "$GAME" && exec env \
    F2_CLIENT_CONNECT="127.0.0.1:$WIRE" \
    F2_TRACE_EVENTS=1 F2_NO_MUSIC=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/fallout2-ce" >"$OUT/client.err" 2>&1 ) &
CLI=$!
sleep 8
kill -0 "$CLI" 2>/dev/null || { echo "CLIENT DIED"; tail -30 "$OUT/client.err"; }

send() { printf '%s\n' "$1" | timeout 5 nc -q1 127.0.0.1 "$CMD" >/dev/null 2>&1; echo "  -> $1"; }

send "give $TPID 3"
send "wield 1"
sleep 1
send "aggro 1"
sleep 3
send "cattack -1 $HITMODE"
sleep 6
send "cattack -1 $HITMODE"
sleep 6

kill "$SRV" 2>/dev/null; kill "$CLI" 2>/dev/null
sleep 1; pkill -x f2_server 2>/dev/null; pkill -x fallout2-ce 2>/dev/null
wait 2>/dev/null

echo "== server: did the throw even happen? =="
grep -aE "cattack|inven_wield" "$OUT/server.err" | tail -6
echo "== client: the §12 mechanism (interleaved, in order) =="
grep -aE "\[adopt\]|\[preplay\]|\[disc\] net=" "$OUT/client.err" | head -25
MINTED=$(grep -ac "\[adopt\] MINT" "$OUT/client.err")
echo "== verdict: [adopt] MINT lines=$MINTED (0 = no throw was thrown, read the TRAPS) =="
echo "   logs: $OUT/{client,server}.err"
