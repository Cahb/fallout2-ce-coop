#!/usr/bin/env bash
# CI guard for the Phase 1 core/client split (REWRITE_PLAN 1.4):
# f2_core must contain no direct SDL include. The simulation core is meant to
# be buildable into a headless/server binary; a direct `#include <SDL...>` or
# `#include "SDL..."` in an f2_core source is a layering regression.
#
# (Note: some core sources still pull SDL *transitively* via svga.h — that
# residual is a known, deferred P5 decoupling item. This check only forbids
# NEW direct SDL includes creeping into the sim core.)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CML="$ROOT/CMakeLists.txt"

# Extract the file list inside add_library(f2_core OBJECT ... ).
mapfile -t CORE_FILES < <(
    awk '
        /add_library\(f2_core OBJECT/ {inblock=1; next}
        inblock && /^\)/ {inblock=0}
        inblock {
            if (match($0, /"([^"]+)"/, m)) print m[1]
        }
    ' "$CML"
)

if [ "${#CORE_FILES[@]}" -eq 0 ]; then
    echo "check_core_no_sdl: could not find f2_core source list in CMakeLists.txt" >&2
    exit 2
fi

violations=0
for f in "${CORE_FILES[@]}"; do
    path="$ROOT/$f"
    [ -f "$path" ] || continue
    while IFS= read -r line; do
        echo "SDL include in f2_core: $f: $line" >&2
        violations=$((violations + 1))
    done < <(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]SDL' "$path")
done

if [ "$violations" -ne 0 ]; then
    echo "FAIL: $violations direct SDL include(s) in f2_core (see above)." >&2
    exit 1
fi

echo "OK: f2_core (${#CORE_FILES[@]} files) has no direct SDL includes."
