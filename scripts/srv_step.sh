#!/usr/bin/env bash
# srv_step.sh — the P5 reactive de-stub driver ([[p5-server-plan]] STEP 1).
#
# One shot: rebuild f2_server, boot it on a map headlessly, and if it dies,
# print the backtrace. Every serverStubAbort in that trail names the next
# f2_client symbol the running core still reaches = the next thing to sever.
#
#   ./scratch/srv_step.sh [map.map] [ticks]
#
# Defaults to artemple.map (the known-good map that reached "runs to completion").
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The binary must run FROM the game-data dir (master.dat + the data tree are
# resolved relative to cwd) — exactly like scripts/server_run_case.sh does for
# the probe. Running from the repo root just fails gameDbInit and looks like a
# boot bug.
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
MAP="${1:-artemple.map}"
TICKS="${2:-2000}"
LOG="$ROOT/scratch/srv_${MAP%%.*}.log"

cd "$ROOT"

echo "=== build f2_server"
if ! cmake --build build --target f2_server -j"$(nproc)" 2>&1 | tail -5; then
    echo "BUILD FAILED" >&2
    exit 1
fi

echo "=== run  F2_SERVER_MAP=$MAP  (ticks=$TICKS)  in $GAME_DIR -> $LOG"
(cd "$GAME_DIR" && gdb -q -batch \
    -ex "set confirm off" \
    -ex "set env F2_SERVER_MAP=$MAP" \
    -ex "set env F2_SERVER_TICKS=$TICKS" \
    -ex "run" \
    -ex "bt 40" \
    "$ROOT/build/f2_server" 2>&1) | tee "$LOG"

echo "=== abort stubs hit:"
grep -oE 'serverStubAbort[^)]*\)|f2_server: [^\n]*' "$LOG" | sort -u || true
