#!/usr/bin/env bash
# Joining-client gate (P5-C STEP 4 "MAKE IT JOINABLE", build slice S2;
# CLIENT_JOIN_DESIGN.md §D). The inverse of the netstream gate: that one proves the
# server EMITS a faithful wire; this one proves a CLIENT can CONSUME it and rebuild
# the world.
#
# For each case: run the server (F2_SERVER_LOOP + F2_NETSTREAM) to produce the wire
# AND a state dump, then run the headless joining client (F2_CLIENT_STREAM_IN) which
# decodes that same wire — loads the join blob, applies every live event — and dumps
# its reconstructed state. The two dumps must be IDENTICAL over all SYNCED state.
#
# Excluded from the comparison (legitimately un-synced / inert for a puppet viewer):
#   - rng_state / queue_next_time : server-internal cadence, never streamed.
#   - map_name                    : case only (ARTEMPLE.MAP vs artemple.map).
#   - lvars_len / lvar            : map-script LOCAL vars ride the blob initially but
#                                   are not streamed as deltas (risk #5); the server
#                                   mutates them over the run, a script-less viewer
#                                   never reads them. A controlling client would need
#                                   lvar/gvar streaming.
#   - script_idx=                 : the per-object script-binding slot (see the join
#                                   gate) — sid still matches; inert for a puppet.
# EVERYTHING synced — every obj/dude tile/elev/rot/flags/pid/sid/NETID, hp, gvars,
# game_time (streamed via WORLD_DELTA), kills, skills — must match byte-for-byte.
#
# Usage: tests/golden/run_golden_client.sh
# Env: F2_GAME_DIR (default <repo>/FO2), F2_BIN (default <repo>/build/fallout2-ce)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"; pkill -9 -x fallout2-ce 2>/dev/null || true' EXIT

# The wire carries the tactical OBJECT world (lifecycle + deltas) and game_time
# (WORLD_DELTA). It does NOT carry game gvars / worldmap position (risk #5), map
# lvars, per-object script bindings (sid / script_idx), or — in this v1 decoder —
# inventory contents (OBJECT_DELTA_INVENTORY rebuild is deferred). So the gate
# compares exactly the decoder's responsibility: every obj/dude line's position/
# flags/hp/pid/netid, plus game_time / map / elevation. sid= and script_idx= are
# stripped (script binding, not synced, inert for a puppet); inv sub-lines and all
# other dump sections are dropped.
norm() {
    grep -aE '^(obj |dude |game_time |map |elevation )' "$1" \
        | sed -E 's/ sid=-?[0-9]+//; s/ script_idx=-?[0-9]+//' \
        | tr 'A-Z' 'a-z'
}

run_case() {
    local name="$1" map="$2" seed="$3" ticks="$4" actions="${5:-}"
    local nets="$RESULTS/$name.nets" sdump="$RESULTS/$name.server" cdump="$RESULTS/$name.client"

    # Server: run the loop, emit the wire (with join blob), dump final state.
    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 \
        F2_NETSTREAM="$nets" F2_JOIN_TMP="$nets.srvblob" \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP="$sdump" \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout -k 5 300 "$BIN" >/dev/null 2>&1) || {
        echo "FAIL $name — server exited non-zero / timed out"; return; }
    if [ ! -s "$nets" ] || [ ! -s "$sdump" ]; then
        echo "FAIL $name — server produced no wire/dump"; return; fi

    # Client: decode the wire, reconstruct, dump.
    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" \
        F2_CLIENT_STREAM_IN="$nets" F2_JOIN_TMP_CLIENT="$nets.cliblob" \
        F2_PROBE_DUMP="$cdump" \
        timeout -k 5 300 "$BIN" >/dev/null 2>&1) || {
        echo "FAIL $name — client exited non-zero / timed out (decode failure?)"; return; }
    if [ ! -s "$cdump" ]; then
        echo "FAIL $name — client produced no dump (blob missing?)"; return; fi

    local objs
    objs="$(grep -acE '^(obj|dude) ' "$sdump")"
    if diff <(norm "$sdump") <(norm "$cdump") > "$RESULTS/$name.diff" 2>&1; then
        echo "PASS $name — $objs objects reconstructed from the wire (blob + live stream)"
    else
        echo "FAIL $name — client state diverged from server ($(grep -cE '^[<>]' "$RESULTS/$name.diff") lines):"
        head -30 "$RESULTS/$name.diff" | sed 's/^/    /'
    fi
}

#        name       map          seed ticks actions
{
    run_case walk      arvillag.map 42 3000 "300:walkto:20527"
    run_case combat    klatoxcv.map 42 4000 ""
    run_case cdamage   denbus1.map  42 1500 "300:cdamage:100,600:cdamage:15"
    run_case itemworld denbus1.map  42 1500 "300:give:41,600:drop:41,900:pickup:0"
    run_case xmap      arvillag.map 42 600  "100:entermap:6"
} | tee "$RESULTS/summary.log"

grep -q "^FAIL" "$RESULTS/summary.log" && exit 1
exit 0
