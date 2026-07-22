#!/usr/bin/env bash
# Join-snapshot round-trip gate (P5-C STEP 4 "MAKE IT JOINABLE", build slice S1;
# CLIENT_JOIN_DESIGN.md §B/§C).
#
# Proves the join BLOB round-trips: a "server" serializes its live map + dude via
# the save pipeline (mapSaveToStream = _map_save_file + _obj_save_dude, with the
# B2 saved-bit stamp), and a fresh "client" instance loads it back (inner mapLoad
# + _obj_load_dude, B4) and reconstructs the SAME world. The load-bearing claim is
# §C: netId is NOT persisted, yet both sides run objectAssignAllNetIds() over the
# same object set in the same order, so every object arrives at the SAME netId BY
# CONSTRUCTION (none on the wire). If the walk domains ever diverge, the netid=
# fields shift and this gate FAILs.
#
# Producer:  F2_SERVER_BLOB_OUT=<blob>  -> writes [map body][dude], dumps state.
# Loader:    F2_CLIENT_BLOB_IN=<blob>   -> loads it, dumps state.
# Both dump with netid= (state_dump gate keys off these env vars).
#
# A case PASSES when the two dumps are IDENTICAL after excluding fields that
# legitimately differ:
#   - rng_state / queue_next_time: the loader runs a different code path (adopts the
#     server clock, no first-run spawns), so RNG/timed-event cadence differs.
#   - map_name: case only (ARTEMPLE.MAP vs artemple.map — the same map).
#   - script_idx=: the per-object script-binding SLOT. The server's map-enter proc
#     clears it on fired-and-removed spatial scripts (server=-1); the viewer runs NO
#     scripts (§E), so it keeps the loaded slot. The script BINDING itself (sid=) is
#     still compared and MATCHES — only the cached index differs, and it is inert for
#     a puppet (no script ever reads it) and carried by no delta.
# EVERYTHING else — every obj/dude line's tile/elev/rot/flags/pid/sid/NETID, every
# critter stat, every inventory item — must match byte-for-byte. The netid match is
# the §C load-bearing claim.
#
# Usage: tests/golden/run_golden_join.sh
# Env: F2_GAME_DIR (default <repo>/FO2), F2_BIN (default <repo>/build/fallout2-ce)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"; pkill -9 -x fallout2-ce 2>/dev/null || true' EXIT

# Normalize the dump for comparison: drop the legitimately-divergent header lines
# and fold case (map_name). Everything surviving this filter is state the join
# MUST reproduce exactly.
normalize() {
    grep -avE '^(rng_state|queue_next_time|map_name) ' "$1" \
        | sed -E 's/ script_idx=-?[0-9]+//' \
        | tr 'A-Z' 'a-z'
}

run_case() {
    local name="$1" map="$2" seed="$3"
    local blob="$RESULTS/$name.blob"
    local sdump="$RESULTS/$name.server" cdump="$RESULTS/$name.client"

    # Producer: load the map, number the syncable set, serialize, dump.
    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" \
        F2_SERVER_BLOB_OUT="$blob" F2_PROBE_DUMP="$sdump" \
        timeout -k 5 200 "$BIN" >/dev/null 2>&1) || {
        echo "FAIL $name — producer exited non-zero / timed out"; return; }

    if [ ! -s "$blob" ] || [ ! -s "$sdump" ]; then
        echo "FAIL $name — producer wrote no blob/dump"; return; fi

    local gtime
    gtime="$(grep -a '^game_time ' "$sdump" | awk '{print $2}')"

    # Loader: boot, drop the probe map, become present on the blob's map, dump.
    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" \
        F2_CLIENT_BLOB_IN="$blob" F2_CLIENT_BLOB_TIME="$gtime" \
        F2_PROBE_DUMP="$cdump" \
        timeout -k 5 200 "$BIN" >/dev/null 2>&1) || {
        echo "FAIL $name — loader exited non-zero / timed out (segfault?)"; return; }

    if [ ! -s "$cdump" ]; then
        echo "FAIL $name — loader wrote no dump"; return; fi

    local objs
    objs="$(grep -acE '^(obj|dude) ' "$sdump")"
    if diff <(normalize "$sdump") <(normalize "$cdump") > "$RESULTS/$name.diff" 2>&1; then
        echo "PASS $name — $objs objects reconstructed identically (netIds + state)"
    else
        echo "FAIL $name — reconstructed state diverged from source ($(grep -cE '^[<>]' "$RESULTS/$name.diff") lines):"
        head -30 "$RESULTS/$name.diff" | sed 's/^/    /'
    fi
}

#        name     map          seed
{
    run_case artemple artemple.map 1337
    run_case denbus1  denbus1.map  42
    run_case kladwtwn kladwtwn.map 42
    run_case newr1    newr1.map    42
} | tee "$RESULTS/summary.log"

grep -q "^FAIL" "$RESULTS/summary.log" && exit 1
exit 0
