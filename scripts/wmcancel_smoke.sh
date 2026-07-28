#!/usr/bin/env bash
# THE 2026-07-27 VIEWER SEGFAULT, headless: two worldmap sessions, both CANCELLED.
#
# Owner's repro: random encounter -> worldmap -> travel -> ESC back to the encounter
# -> leave again -> SIGSEGV in the CLIENT (programFatalError -> siglongjmp).
# Mechanism: the viewer's wmInterfaceInit ran _scr_remove_all + programListFree, and
# the SCRIPT_FLAG_0x10 survivors (dude + party) kept the freed Program.
#
# Needs a REAL viewer on dummy video: the whole path is behind clientViewerActive().
#
# ►► SAVE SETUP: these load slot $SLOT (default 9) and the server may write map .SAV
# files into it, so DO NOT point them at a slot you care about. Copy one first:
#     cp -r FO2/data/SAVEGAME/SLOT11 FO2/data/SAVEGAME/SLOT09
# and delete it afterwards. Ports are 947x/948x/949x so they do not collide with a
# live server on 9200/9201.
set -u
ROOT=/mnt/NVME/Projects/fallout2-ce
GAME="$ROOT/FO2"
OUT="${OUT:-$ROOT/build/wmcancel-smoke}"
WIRE=9470
CMD=9471
SLOT="${SLOT:-9}"

rm -rf "$OUT"; mkdir -p "$OUT"

( cd "$GAME" && exec env \
    F2_SERVER_LOAD="$SLOT" F2_SERVER_NET="$WIRE" F2_SERVER_CMD="$CMD" \
    F2_AUTOSAVE_SECS=0 F2_SERVER_PACE_MS=30 F2_SERVER_TICKS=20000 \
    F2_WORLDMAP_STREAM=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 4
kill -0 "$SRV" 2>/dev/null || { echo "SERVER DIED AT BOOT"; tail -25 "$OUT/server.err"; exit 1; }

( cd "$GAME" && exec env \
    F2_CLIENT_CONNECT="127.0.0.1:$WIRE" F2_NO_MUSIC=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/fallout2-ce" >"$OUT/client.err" 2>&1 ) &
CLI=$!
sleep 8
kill -0 "$CLI" 2>/dev/null || { echo "CLIENT DIED BEFORE WE STARTED"; tail -30 "$OUT/client.err"; }

send() { printf '%s\n' "$1" | timeout 5 nc -q1 127.0.0.1 "$CMD" >/dev/null 2>&1; }

for round in 1 2 3; do
    kill -0 "$CLI" 2>/dev/null || { echo "!! CLIENT DEAD before round $round"; break; }
    echo "-- round $round: open worldmap, then cancel it"
    send "entermap -2"
    sleep 3
    send "wmesc"
    sleep 4
done
sleep 3

CLIENT_ALIVE=0; kill -0 "$CLI" 2>/dev/null && CLIENT_ALIVE=1
kill "$SRV" 2>/dev/null; kill "$CLI" 2>/dev/null
sleep 1; wait 2>/dev/null

echo "== CLIENT_ALIVE=$CLIENT_ALIVE =="
echo "== server [wmsrv] =="; grep -a "\[wmsrv\]" "$OUT/server.err" | head -8
echo "== client tail =="; tail -12 "$OUT/client.err"
echo "OUT=$OUT"
