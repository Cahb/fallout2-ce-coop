#!/usr/bin/env bash
# P3 LIVE COMBAT DEMO — the first human-driven, turn-based fight over the network.
# Runs f2_server with the RESUMABLE-COMBAT session gate on (fights span beats,
# combat.cc) and streams the wire to real SDL viewers. Drive the dude's combat
# turn from the controlling viewer window:
#
#   left-click  = combat move to a tile   (cmove — AP-limited)
#   A           = attack nearest hostile  (cattack)
#   E / Enter   = end your turn           (cendturn)
#
# The viewer prints "YOUR TURN ap=N" to FO2/debug.log (DEBUGACTIVE=log) when the
# dude's turn opens; the server waits on the connected claimant — up to
# F2_SERVER_TURN_IDLE_MS of think time per action before it auto-ends the turn.
#
# MODES:
#   scripts/combat_demo.sh server    # boot ONLY the server (headless), stays up
#   scripts/combat_demo.sh client    # connect ONE viewer to a running server
#   scripts/combat_demo.sh           # all-in-one: server + one viewer (+SPECTATORS)
#
# The split modes let you control each process independently — e.g. run the server
# and first client in the background, then connect/close/reconnect a second client:
#   scripts/combat_demo.sh server &        # headless server
#   scripts/combat_demo.sh client &        # client 1 (controller — connects first)
#   scripts/combat_demo.sh client          # client 2 (spectator; Ctrl-C to drop,
#                                          #           rerun to reconnect)
# The FIRST viewer to connect wins the control claim; later viewers spectate
# (their claim is refused, input dropped). Each client gets its own join-blob
# scratch file, so concurrent viewers never clobber each other.
#
# Once a viewer is up, START the fight from another shell:
#   printf 'aggro 1\n' | nc -q1 127.0.0.1 9201
#
# SPECTATORS=N (all-in-one mode only) launches N extra watch-only viewers next to
# the controller, all windowed, joining BEFORE you aggro so combat opens clean.
#
# Env: MAP, WIRE_PORT, CMD_PORT, HOST, PACE (ms/beat; 100 = real time), IDLE_MS
# (per-action think budget, default 60s), SPECTATORS (all-in-one only, default 0).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$ROOT/FO2"
MAP="${MAP:-klatoxcv.map}"
HOST="${HOST:-127.0.0.1}"
WIRE_PORT="${WIRE_PORT:-9200}"
CMD_PORT="${CMD_PORT:-9201}"
PACE="${PACE:-100}"
IDLE_MS="${IDLE_MS:-60000}"
SPECTATORS="${SPECTATORS:-0}"

if [ ! -x "$ROOT/build/f2_server" ] || [ ! -x "$ROOT/build/fallout2-ce" ]; then
    echo "build f2_server + fallout2-ce first (cmake --build build)"; exit 1
fi

# Boot the headless server in the foreground of THIS process (so `... server &`
# backgrounds it and killing the script kills it). Clears any stale server first.
run_server() {
    pkill -x f2_server 2>/dev/null && sleep 1
    echo "server: map=$MAP wire=:$WIRE_PORT cmd=:$CMD_PORT pace=${PACE}ms idle=${IDLE_MS}ms (resumable combat ON)"
    echo "start the fight with:  printf 'aggro 1\\n' | nc -q1 $HOST $CMD_PORT"
    ( cd "$GAME" && exec env \
        F2_SERVER_MAP="$MAP" F2_SERVER_NET="$WIRE_PORT" F2_SERVER_CMD="$CMD_PORT" \
        F2_SERVER_PACE_MS="$PACE" F2_SERVER_SMOOTH_WALK=1 F2_SERVER_TICKS=5000000 \
        F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_TURN_IDLE_MS="$IDLE_MS" \
        F2_SERVER_PRES_RECORD="${F2_SERVER_PRES_RECORD:-1}" \
        "$ROOT/build/f2_server" )
}

# Connect ONE windowed viewer to a running server. A per-invocation join-blob
# scratch (keyed on this script's PID) keeps concurrent clients from clobbering
# each other's reassembly. Foreground, so Ctrl-C disconnects this client cleanly.
run_client() {
    local tmp="/tmp/f2ce_client_$$.bin"
    echo "client: connecting to $HOST:$WIRE_PORT (join scratch $tmp)"
    echo "  first viewer to connect controls; later viewers spectate (claim refused)."
    ( cd "$GAME" && exec env F2_CLIENT_CONNECT="$HOST:$WIRE_PORT" F2_WINDOWED=1 \
        F2_JOIN_TMP_CLIENT="$tmp" "$ROOT/build/fallout2-ce" )
}

# All-in-one: server (background) + controller viewer + optional spectators, torn
# down together when the controller exits. The original one-shot demo.
run_all() {
    run_server &
    SRV=$!
    sleep 1.5 # let the server boot the map and start listening

    # With spectators, everything goes windowed so the windows fit side by side.
    local windowed=""
    [ "$SPECTATORS" -gt 0 ] && windowed="F2_WINDOWED=1"

    echo "viewer: connecting to $HOST:$WIRE_PORT (controller)"
    # Controller connects FIRST (backgrounded) so it wins the control claim; the
    # script then blocks on it (wait), exactly as the old foreground launch did.
    ( cd "$GAME" && exec env F2_CLIENT_CONNECT="$HOST:$WIRE_PORT" \
        ${windowed:+F2_WINDOWED=1} F2_JOIN_TMP_CLIENT="/tmp/f2ce_ctrl_$$.bin" \
        "$ROOT/build/fallout2-ce" ) &
    CTRL=$!

    SPEC_PIDS=""
    if [ "$SPECTATORS" -gt 0 ]; then
        sleep 4 # let the controller finish its blob load + claim before spectators join
        for i in $(seq 1 "$SPECTATORS"); do
            echo "spectator $i: connecting (watch-only — claim will be refused)"
            ( cd "$GAME" && exec env F2_CLIENT_CONNECT="$HOST:$WIRE_PORT" F2_WINDOWED=1 \
                F2_JOIN_TMP_CLIENT="/tmp/f2ce_spectator_$i.bin" "$ROOT/build/fallout2-ce" ) &
            SPEC_PIDS="$SPEC_PIDS $!"
            sleep 3
        done
    fi

    wait "$CTRL" 2>/dev/null
    # Controller exited (quit / disconnect) — tear the server + spectators down.
    kill "$SRV" 2>/dev/null
    for p in $SPEC_PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
}

case "${1:-all}" in
    server|--server) run_server ;;
    client|--client) run_client ;;
    all|--all)       run_all ;;
    *)
        echo "usage: $(basename "$0") [server|client]"
        echo "  server   boot only the headless f2_server (stays up)"
        echo "  client   connect one windowed viewer to a running server"
        echo "  (no arg) server + one viewer together (SPECTATORS=N for extras)"
        exit 1
        ;;
esac
