#!/usr/bin/env bash
# STEP-4 live viewer demo (CLIENT_JOIN_DESIGN.md). Boots f2_server on a map,
# streams the binary wire over TCP, and launches the real SDL client as a
# read-only viewer that joins + renders the world live. A dedicated command
# channel lets you poke the running sim and watch the effect in the viewer.
#
#   scripts/viewer_live.sh                 # artemple, default ports
#   MAP=kladwtwn.map scripts/viewer_live.sh
#   LOAD=8 scripts/viewer_live.sh          # restore save slot 8 instead of a map
#
# Then, from another shell, inject commands (same "verb arg arg2" vocabulary as
# the probe's F2_PROBE_ACTIONS / commandDispatch):
#   printf 'give 41 1\ndrop 41\n' | nc -q1 127.0.0.1 9201   # drop an item at the dude's feet
#   printf 'aggro 1\n'            | nc -q1 127.0.0.1 9201   # start combat (synchronous; may end the map)
#   printf 'walk 40\n'            | nc -q1 127.0.0.1 9201   # dude WALKS +40 tiles, one tile per beat (arg2=1 runs)
#   printf 'critwalk -30 1\n'     | nc -q1 127.0.0.1 9201   # nearest critter runs -30 tiles
#   printf 'walkto 22150\n'       | nc -q1 127.0.0.1 9201   # dude walks to an absolute tile
#
# Env: MAP, WIRE_PORT, CMD_PORT, PACE (ms/beat; 0 = full speed, ~10ms/beat is
# roughly real time — the server is logical-time-only, this is a demo throttle).
# F2_SERVER_SMOOTH_WALK=1 (set below) makes out-of-combat walks — script-driven
# wanderers and the walk/walkto verbs — step one tile per beat instead of
# applying the whole path in one frame, so the viewer sees motion animate.
# VIEWERS=N (STEP 5): launch N viewers; extras join MID-STREAM, staggered 5s
# apart, all windowed (F2_WINDOWED=1) so they fit side by side to eyeball sync.
#
# NAMES="Zorbax,Quixle": give each viewer its own ACCOUNT NAME, so viewer i logs
# in as the i-th name and the server binds it to that name's character
# (ACCOUNT_IDENTITY_DESIGN.md). Unset = every viewer sends the legacy bare
# `claim`, i.e. exactly today's behaviour.
#   CREATE0=.. CREATE1=..: optional "S P E C I A L [tag tag tag] [trait trait]"
#   for the matching name, used only the FIRST time that name is seen. Also
#   accepts "ask" — roll the character in vanilla's own creation screen.
#
# ►► CREATE<n> IS 0-BASED, and it lines up with the VIEWER index, not necessarily
# with the slot. CREATE0 is the first viewer, not CREATE1.
#
# Slots are handed out on a FIRST-COME basis, and with an interactive CREATE<n>=ask
# that means whoever finishes rolling first takes slot 0 — the host body, which
# worldmap travel and the other host-only screens are gated on. HOST=<name> pins
# slot 0 to that account instead, so "who can drive the map" stops being a race:
#   HOST=Cahb NAMES="Cahb,Mennoc" CREATE0=ask CREATE1=ask VIEWERS=2 ...
#
# Example — two players, each their own character, no player count anywhere:
#   NAMES="Zorbax,Quixle" CREATE0="10 1 10 1 1 1 1 0 1 2" \
#   CREATE1="1 10 1 1 10 1 1 3 4 5" VIEWERS=2 scripts/viewer_live.sh
set -u

# The i-th entry of NAMES, or empty. Takes a 0-based index; cut is 1-based.
nth_name() {
    printf '%s' "${NAMES:-}" | cut -d, -f"$(( $1 + 1 ))"
}

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
# LOAD=<1-10>: restore a save slot instead of booting a fresh map. MAP is left
# UNSET in that case — the two are alternatives, and passing both used to boot the
# map and silently discard the save (the reload then read as a save-format bug:
# full HP, no inventory). The server now prefers the slot and says so, but keeping
# MAP out of the env here means the log says what you asked for.
LOAD="${LOAD:-}"
if [ -n "$LOAD" ]; then
    MAP=""
else
    MAP="${MAP:-artemple.map}"
fi
WIRE_PORT="${WIRE_PORT:-9200}"
CMD_PORT="${CMD_PORT:-9201}"
# 100ms wall per beat == kServerTickDelta (100ms sim per beat) → sim runs at
# true real time, and stepped walks pace like the original (~5 walk / ~10 run
# tiles/s). Lower = faster-than-life demo, higher = slow motion.
PACE="${PACE:-100}"
VIEWERS="${VIEWERS:-1}"

if [ ! -x "$ROOT/build/f2_server" ] || [ ! -x "$ROOT/build/fallout2-ce" ]; then
    echo "build f2_server + fallout2-ce first (cmake --build build)"; exit 1
fi

echo "server: ${LOAD:+save slot $LOAD}${MAP:+map $MAP} wire=:$WIRE_PORT cmd=:$CMD_PORT pace=${PACE}ms"
# F2_SERVER_RESUMABLE_COMBAT=1: beat-spanning combat — required for the P3 combat
# presentation (anim replay / move glide) AND for player-initiated combat start
# (the 'A'-key cstart verb → _combat(nullptr) session). Overridable from the env.
( cd "$GAME" && env ${MAP:+F2_SERVER_MAP="$MAP"} ${LOAD:+F2_SERVER_LOAD="$LOAD"} \
    ${HOST:+F2_SERVER_HOST="$HOST"} \
    F2_SERVER_NET="$WIRE_PORT" F2_SERVER_CMD="$CMD_PORT" \
    F2_SERVER_RESUMABLE_COMBAT="${F2_SERVER_RESUMABLE_COMBAT:-1}" \
    F2_SERVER_PACE_MS="$PACE" F2_SERVER_SMOOTH_WALK=1 \
    F2_SERVER_PRES_RECORD=1 F2_DIALOG_STREAM=1 "$ROOT/build/f2_server" ) &
SRV=$!

# Give the server a moment to boot the map and start listening.
sleep 1.5

# Extra viewers (VIEWERS>1) join MID-STREAM, staggered so you can watch each
# join land (existing viewers hitch once per join — the C.4 rebaseline
# broadcast reloads every client's world). Each needs its own blob scratch.
WINDOWED_ENV=""
EXTRA_PIDS=""
if [ "$VIEWERS" -gt 1 ]; then
    WINDOWED_ENV="1"
    for i in $(seq 2 "$VIEWERS"); do
        (
            sleep $(( (i - 1) * 5 ))
            cd "$GAME" || exit 1
            vname="$(nth_name "$(( i - 1 ))")"
            eval "vcreate=\${CREATE$(( i - 1 )):-}"
            # exec: the subshell PID becomes the binary, so the teardown kill
            # below actually reaches it (a plain child would be orphaned).
            exec env F2_CLIENT_CONNECT="127.0.0.1:$WIRE_PORT" F2_WINDOWED=1 \
                ${vname:+F2_PLAYER_NAME="$vname"} \
                ${vcreate:+F2_PLAYER_CREATE="$vcreate"} \
                F2_JOIN_TMP_CLIENT="/tmp/f2ce_join_view_$i.bin" "$ROOT/build/fallout2-ce"
        ) &
        EXTRA_PIDS="$EXTRA_PIDS $!"
    done
fi

V1_NAME="$(nth_name 0)"
V1_CREATE="${CREATE0:-}"
echo "viewer: connecting to 127.0.0.1:$WIRE_PORT${V1_NAME:+ as '$V1_NAME'}"
( cd "$GAME" && env F2_CLIENT_CONNECT="127.0.0.1:$WIRE_PORT" \
    ${WINDOWED_ENV:+F2_WINDOWED=1} \
    ${V1_NAME:+F2_PLAYER_NAME="$V1_NAME"} \
    ${V1_CREATE:+F2_PLAYER_CREATE="$V1_CREATE"} \
    "$ROOT/build/fallout2-ce" )

# Viewer exited (quit / disconnect) — tear the server + any extra viewers down.
kill "$SRV" 2>/dev/null
for p in $EXTRA_PIDS; do kill "$p" 2>/dev/null; done
wait 2>/dev/null
