#!/usr/bin/env bash
# CI guard for the P5-C core-only server target (p5-server-plan):
# f2_server links f2_core WITHOUT f2_client and WITHOUT SDL. The whole point of
# the target is to prove the simulation core can be the basis of a headless
# dedicated server independent of the SDL presentation layer, so an SDL symbol
# or shared-library dependency creeping into f2_server is a layering regression
# (it would mean a client symbol stub was replaced by a real SDL-coupled impl,
# or f2_client was linked in).
#
# Runs only when the binary exists (desktop builds; the target is skipped on
# Android/iOS). The SDL checks below are ELF/Linux-oriented (ldd + nm -u); on
# platforms lacking those tools a check is skipped rather than failing, so this
# gate's SDL coverage is strongest on the Linux CI where the slice is developed.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/f2_server"

if [ ! -x "$BIN" ]; then
    echo "check_server_core_only: build/f2_server not present — skipping (not built)."
    exit 0
fi

fail=0

# 1) No SDL shared-library dependency.
if command -v ldd >/dev/null 2>&1; then
    if ldd "$BIN" 2>/dev/null | grep -iq 'libsdl'; then
        echo "FAIL: f2_server has an SDL shared-library dependency:" >&2
        ldd "$BIN" | grep -i 'libsdl' >&2
        fail=1
    fi
fi

# 2) No undefined SDL_ symbols (would fault at load / prove SDL coupling).
if command -v nm >/dev/null 2>&1; then
    n=$(nm -u "$BIN" 2>/dev/null | grep -c 'SDL_')
    if [ "$n" -ne 0 ]; then
        echo "FAIL: f2_server references $n undefined SDL_ symbol(s):" >&2
        nm -u "$BIN" 2>/dev/null | grep 'SDL_' | head >&2
        fail=1
    fi
fi

# 3) It actually starts and exits cleanly. Catches a regression where a core
#    change adds a global constructor that reaches an abort stub at startup —
#    that faults at runtime, not link time, so the SDL checks alone would miss it.
#
# What counts as "cleanly": ANY ordinary exit status. With no env set the server
# now prints its usage ("nothing to run. Set F2_SERVER_MAP=...") and exits 1 —
# that is a correct, deliberate exit and this check predates it (it was written
# before the lobby landed, when a bare run booted something). Testing for rc==0
# made the gate fail on every build and, because check.sh exits on it, no golden
# ever ran.
#
# A startup abort is what we are actually hunting, and that dies by SIGNAL:
# the shell reports those as 128+signo (SIGABRT = 134, SIGSEGV = 139). So the
# only failing band is >= 128. Note the old message's `rc=$?` was itself wrong —
# inside `if ! cmd`, `$?` is the negated test result, not the command's status.
"$BIN" >/dev/null 2>&1
rc=$?
if [ "$rc" -ge 128 ]; then
    echo "FAIL: f2_server died on signal $((rc - 128)) at startup (rc=$rc)." >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "OK: f2_server links core-only (no SDL dep, no SDL_ symbols) and exits clean."
