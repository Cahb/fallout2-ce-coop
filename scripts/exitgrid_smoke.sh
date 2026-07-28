#!/usr/bin/env bash
# EXIT-GRID SMOKE — the reported softlock, end to end, with a REAL viewer.
#
# NOT A GATE: wall-clock sleeps, and it needs a viewer on dummy video because the
# whole bug lives behind clientViewerActive() (false in the headless client probe,
# so every guard involved is inert there).
#
# THE BUG IT PINS: walk off the edge of a map DURING COMBAT and the client
# disappeared. Not a crash — no core file is ever written — but a clean return out
# of main, which is why it was so hard to read. objectSetLocation's `obj == gDude`
# block let the VIEWER discover the exit grid under its own replayed dude and latch
# a local MapTransition; mapSetTransition opens with
#     if (isInCombat()) { _game_user_wants_to_quit = 1; }
# and on a viewer that combat state is a MIRROR. Vanilla survives the same line
# because the 1 is an in-band "leave this loop" signal that combat.cc clears again;
# the viewer has no such loop, so the flag reached the frame loop and ended it.
#
# ASSERTION: the client is STILL ALIVE after the transition. Secondary: the server
# really did run the worldmap driver (so we exercised the path, not just idled), and
# the client never had to use the [viewer] in-band backstop.
#
#   scripts/exitgrid_smoke.sh
#   SLOT=11 TILE=26322 scripts/exitgrid_smoke.sh
#
# ►► READS THE SAVE ONLY. F2_AUTOSAVE_SECS=0 is not optional: autosave rotates into
# slots 11-15 and would otherwise overwrite the very save this reproduces from.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
OUT="${OUT:-$ROOT/build/exitgrid-smoke}"
WIRE="${WIRE_PORT:-9370}"
CMD="${CMD_PORT:-9371}"
SLOT="${SLOT:-11}"
TILE="${TILE:-26322}"   # at/next to cave3's exit grid (from the live report)

[ -x "$ROOT/build/f2_server" ] || { echo "no f2_server"; exit 1; }
[ -x "$ROOT/build/fallout2-ce" ] || { echo "no fallout2-ce"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"

( cd "$GAME" && exec env \
    F2_SERVER_LOAD="$SLOT" F2_SERVER_NET="$WIRE" F2_SERVER_CMD="$CMD" \
    F2_AUTOSAVE_SECS=0 \
    F2_SERVER_PACE_MS=30 F2_SERVER_TICKS=12000 \
    F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_PRES_RECORD=1 \
    F2_SERVER_SMOOTH_WALK=1 F2_WORLDMAP_STREAM=1 F2_SERVER_TURN_IDLE_MS=3000 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 3
kill -0 "$SRV" 2>/dev/null || { echo "SERVER DIED AT BOOT"; tail -20 "$OUT/server.err"; exit 1; }

( cd "$GAME" && exec env \
    F2_CLIENT_CONNECT="127.0.0.1:$WIRE" \
    F2_TRACE_EVENTS=1 F2_NO_MUSIC=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/fallout2-ce" >"$OUT/client.err" 2>&1 ) &
CLI=$!
sleep 8
kill -0 "$CLI" 2>/dev/null || { echo "CLIENT DIED BEFORE WE EVEN MOVED"; tail -30 "$OUT/client.err"; }

send() { printf '%s\n' "$1" | timeout 5 nc -q1 127.0.0.1 "$CMD" >/dev/null 2>&1; }

# Walk to the edge. In combat a turn only buys a few hexes, so ask repeatedly and
# let the AP-limited walks accumulate across turns.
for i in $(seq 1 14); do
    kill -0 "$CLI" 2>/dev/null || { echo "!! CLIENT DIED at walk iteration $i"; break; }
    send "walkto $TILE"
    sleep 2
done
sleep 4

CLIENT_ALIVE=0
kill -0 "$CLI" 2>/dev/null && CLIENT_ALIVE=1

kill "$SRV" 2>/dev/null; kill "$CLI" 2>/dev/null
sleep 1; wait 2>/dev/null

echo "== server: did we reach the worldmap driver? =="
grep -aE "\[wmsrv\]" "$OUT/server.err" | head -5
echo "== client: worldmap + any in-band quit backstop =="
grep -aE "onWorldmapBegin|onWorldmapEnd|\[viewer\] consumed" "$OUT/client.err" | head -5
echo "== client tail =="
tail -4 "$OUT/client.err"
echo
if [ "$CLIENT_ALIVE" = "1" ]; then
    echo "PASS exitgrid — client SURVIVED the in-combat map exit"
else
    echo "FAIL exitgrid — client exited (the reported bug)"
fi
echo "   logs: $OUT/{client,server}.err"
