#!/usr/bin/env bash
# THE AUTO-DECLINING ENCOUNTER PROMPT. Pumps Outdoorsman so the detection roll passes,
# then travels until a DETECTED encounter fires.
#
# PASS  = "[wmenc] prompt box OPEN" and the box STAYS open (nobody is there to click it,
#         so the server's barrier should sit and wait).
# FAIL  = a "prompt box CLOSED after ~16ms rc=0 -> encdecline" right behind the OPEN,
#         and/or "[wmenc] ESC INJECTED by ticker: pump() failed / server gone".
#
# ►► SAVE SETUP: these load slot $SLOT (default 9) and the server may write map .SAV
# files into it, so DO NOT point them at a slot you care about. Copy one first:
#     cp -r FO2/data/SAVEGAME/SLOT11 FO2/data/SAVEGAME/SLOT09
# and delete it afterwards. Ports are 947x/948x/949x so they do not collide with a
# live server on 9200/9201.
set -u
ROOT=/mnt/NVME/Projects/fallout2-ce
GAME="$ROOT/FO2"
OUT="${OUT:-$ROOT/build/encprompt-smoke}"
WIRE=9490
CMD=9491
SLOT="${SLOT:-9}"

rm -rf "$OUT"; mkdir -p "$OUT"

( cd "$GAME" && exec env \
    F2_SERVER_LOAD="$SLOT" F2_SERVER_NET="$WIRE" F2_SERVER_CMD="$CMD" \
    F2_AUTOSAVE_SECS=0 F2_SERVER_PACE_MS=0 F2_SERVER_TICKS=60000 \
    F2_WORLDMAP_STREAM=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/f2_server" >"$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 4

( cd "$GAME" && exec env \
    F2_CLIENT_CONNECT="127.0.0.1:$WIRE" F2_NO_MUSIC=1 \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "$ROOT/build/fallout2-ce" >"$OUT/client.err" 2>&1 ) &
CLI=$!
sleep 8

send() { printf '%s\n' "$1" | timeout 5 nc -q1 127.0.0.1 "$CMD" >/dev/null 2>&1; }

# Outdoorsman (skill 17) to the cap so detection all but always passes.
# Outdoorsman no longer needs pumping — encnext arms detection directly.
send "encnext"

for trip in $(seq 1 10); do
    kill -0 "$SRV" 2>/dev/null || break
    grep -qa "prompt emitted" "$OUT/server.err" && break
    send "encnext"; send "entermap -2"
    sleep 2
    send "wmmove 250 250"
    sleep 10
done
sleep 6

kill "$SRV" 2>/dev/null; kill "$CLI" 2>/dev/null
sleep 1; wait 2>/dev/null

echo "== server =="; grep -aE "\[wmenc\]|encdecline|encaccept" "$OUT/server.err" | head -8
echo "== client =="; grep -aE "\[wmenc\]" "$OUT/client.err" | head -8
echo "OUT=$OUT"
