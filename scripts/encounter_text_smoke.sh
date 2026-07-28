#!/usr/bin/env bash
# Does the "You have encountered: ..." line actually reach the wire on a random
# encounter entered through the worldmap driver? Tees the outbound wire and greps it.
#
# ►► SAVE SETUP: these load slot $SLOT (default 9) and the server may write map .SAV
# files into it, so DO NOT point them at a slot you care about. Copy one first:
#     cp -r FO2/data/SAVEGAME/SLOT11 FO2/data/SAVEGAME/SLOT09
# and delete it afterwards. Ports are 947x/948x/949x so they do not collide with a
# live server on 9200/9201.
set -u
ROOT=/mnt/NVME/Projects/fallout2-ce
GAME="$ROOT/FO2"
OUT="${OUT:-$ROOT/build/enctext-smoke}"
WIRE=9480
CMD=9481
SLOT="${SLOT:-9}"
DEST="${DEST:-250 250}"

rm -rf "$OUT"; mkdir -p "$OUT"

( cd "$GAME" && exec env \
    F2_SERVER_LOAD="$SLOT" F2_SERVER_NET="$WIRE" F2_SERVER_CMD="$CMD" \
    F2_SERVER_NET_TEE="$OUT/wire.bin" \
    F2_AUTOSAVE_SECS=0 F2_SERVER_PACE_MS=0 F2_SERVER_TICKS=40000 \
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

send() { printf '%s\n' "$1" | timeout 5 nc -q1 127.0.0.1 "$CMD" >/dev/null 2>&1; }

for trip in 1 2 3 4 5 6; do
    kill -0 "$SRV" 2>/dev/null || break
    grep -qa "\[wmenc\] roll:" "$OUT/server.err" && break
    echo "-- trip $trip"
    send "entermap -2"
    sleep 2
    send "wmmove $DEST"
    sleep 12
done
sleep 3

kill "$SRV" 2>/dev/null; kill "$CLI" 2>/dev/null
sleep 1; wait 2>/dev/null

echo "== [wmenc] =="; grep -a "\[wmenc\]" "$OUT/server.err" | head -6
echo "== [wmsrv] entered =="; grep -a "\[wmsrv\] entered" "$OUT/server.err" | head -4
echo "== encounter line on the WIRE? =="
strings "$OUT/wire.bin" 2>/dev/null | grep -i "encounter" | head -5 || echo "(none)"
echo "== client saw it? =="; grep -ai "encounter" "$OUT/client.err" | head -5
echo "OUT=$OUT"
