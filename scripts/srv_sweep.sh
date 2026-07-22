#!/usr/bin/env bash
# srv_sweep.sh — boot f2_server on many maps, report which serve and which abort.
# Builds ONCE, then runs each map (no gdb; the FATAL line names the stub).
# Usage: ./scratch/srv_sweep.sh [map ...]   (defaults to the 5 golden maps)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
TICKS="${F2_SWEEP_TICKS:-200}"

MAPS=("$@")
if [ ${#MAPS[@]} -eq 0 ]; then
    MAPS=(artemple.map arvillag.map denbus1.map kladwtwn.map klatoxcv.map)
fi

cd "$ROOT"
echo "=== build f2_server"
# MUST abort on a failed build: otherwise the sweep happily runs the STALE
# binary and reports green for code that does not compile.
if ! cmake --build build --target f2_server -j"$(nproc)" > "$ROOT/scratch/build.log" 2>&1; then
    echo "BUILD FAILED — not sweeping (stale binary would lie):" >&2
    grep -E 'error|undefined' "$ROOT/scratch/build.log" | head -15 >&2
    exit 1
fi
tail -1 "$ROOT/scratch/build.log"

pass=0; fail=0
for m in "${MAPS[@]}"; do
    log="$ROOT/scratch/sweep_${m%%.*}.log"
    (cd "$GAME_DIR" && env F2_SERVER_MAP="$m" F2_SERVER_TICKS="$TICKS" \
        timeout -k 5 120 "$ROOT/build/f2_server" >"$log" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        printf 'PASS  %-16s\n' "$m"; pass=$((pass+1))
    else
        stub=$(grep -oE "client symbol '[^']+'" "$log" | head -1)
        printf 'FAIL  %-16s rc=%-4s %s\n' "$m" "$rc" "${stub:-(no stub abort — see $log)}"
        fail=$((fail+1))
    fi
done
echo "=== $pass served, $fail failed"
