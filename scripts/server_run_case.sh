#!/usr/bin/env bash
# Run one probe case under the FIXED-TIMESTEP SERVER LOOP (F2_SERVER_LOOP=1),
# writing the state dump to $OUT. Input-replay traces are intentionally NOT
# passed (obsolete under the server loop — SPACE end-turn becomes auto-end-turn).
# Usage: server_run_case.sh <map> <seed> <ticks> <out> [aggro] [actions]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"

map="$1"; seed="$2"; ticks="$3"; out="$4"; aggro="${5:-}"; actions="${6:-}"

(cd "$GAME_DIR" && env \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 \
    F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
    F2_PROBE_DUMP="$out" \
    ${aggro:+F2_PROBE_AGGRO="$aggro"} \
    ${actions:+F2_PROBE_ACTIONS="$actions"} \
    timeout -k 5 120 "$BIN" > /dev/null 2>&1)
rc=$?
pkill -9 -x fallout2-ce 2>/dev/null
exit $rc
