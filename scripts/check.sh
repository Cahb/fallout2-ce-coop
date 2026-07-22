#!/usr/bin/env bash
# All-in-one verify gate: build + golden replay suite.
#
# Usage:
#   scripts/check.sh              # build (linux + windows), then all golden cases
#   scripts/check.sh --build-only # just the builds
#
# Exits nonzero on build error or any golden FAIL. Build warnings are
# suppressed (pre-existing upstream noise); errors always shown.
#
# BUILD TYPES — both trees carry DEBUG SYMBOLS on purpose. This is a WIP engine;
# a crash must give a usable core/backtrace without a rebuild-to-reproduce round
# trip. Linux is RelWithDebInfo rather than Debug because the golden runners
# impose hard wall-clock timeouts (as tight as `timeout -k 3 25`) that an -O0
# build would blow, turning a slow build into spurious FAILs. Windows has no
# gate timing to break, so it is full Debug.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

BUILD_LOG="$(mktemp)"
trap 'rm -f "$BUILD_LOG"' EXIT

if ! cmake --build "$ROOT/build" -j"$(nproc)" > "$BUILD_LOG" 2>&1; then
    echo "BUILD FAILED (linux):"
    grep -E "error|Error" "$BUILD_LOG" | head -30
    exit 1
fi
echo "BUILD OK (linux: fallout2-ce + f2_server)"

# Windows CLIENT cross-build (mingw-w64). The host runs the dedicated server on
# Linux and players only need fallout2-ce.exe, so f2_server is deliberately NOT
# built for Windows — server_net.cc is POSIX sockets, and CMakeLists.txt gates
# the target on `NOT WIN32`. Cross-building here catches the portability breaks
# that a Linux-only gate misses (MSVCRT printf specifiers, <sys/*.h> includes,
# case-sensitive header names) at the commit that introduces them rather than at
# release time.
#
# SKIPPED, not failed, when the cross compiler is absent: this must stay usable
# on a box without mingw installed. A PRESENT toolchain that fails to build is a
# hard error.
if command -v x86_64-w64-mingw32-g++ > /dev/null 2>&1; then
    if [ ! -f "$ROOT/build-win/CMakeCache.txt" ]; then
        echo "configuring windows cross-build (first run, fetches SDL2 — slow)..."
        if ! cmake -B "$ROOT/build-win" \
                -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake" \
                -DCMAKE_BUILD_TYPE=Debug > "$BUILD_LOG" 2>&1; then
            echo "CONFIGURE FAILED (windows):"
            tail -20 "$BUILD_LOG"
            exit 1
        fi
    fi
    if ! cmake --build "$ROOT/build-win" -j"$(nproc)" > "$BUILD_LOG" 2>&1; then
        echo "BUILD FAILED (windows):"
        grep -E "error|Error" "$BUILD_LOG" | head -30
        exit 1
    fi
    echo "BUILD OK (windows: fallout2-ce.exe)"
else
    echo "BUILD SKIP (windows: x86_64-w64-mingw32-g++ not installed)"
fi

# Phase 1 layering guard: the simulation core (f2_core) must not directly
# include SDL (REWRITE_PLAN 1.4).
if ! "$ROOT/scripts/check_core_no_sdl.sh"; then
    exit 1
fi

# P5-C layering guard: the core-only server target (f2_server) must link
# f2_core without f2_client / SDL (p5-server-plan).
if ! "$ROOT/scripts/check_server_core_only.sh"; then
    exit 1
fi

if [ "${1:-}" = "--build-only" ]; then
    exit 0
fi

# Gate 1: legacy frame-driven probe — byte-identical replay determinism.
"$ROOT/tests/golden/run_golden.sh" || exit 1

# Gate 2: fixed-timestep server loop (F2_SERVER_LOOP) — deterministic,
# outcome-equivalent to legacy (SERVER_LOOP_DESIGN.md; separate golden set
# because timing/RNG-cadence fields legitimately differ).
echo
echo "== server-loop gate =="
"$ROOT/tests/golden/run_golden_server.sh" || exit 1

# Gate 2b: RECORD-PURITY — run f2_server (the recording backend) twice per case,
# F2_SERVER_PRES_RECORD off vs on, and require byte-identical sim state + NO_SAVE
# object count. Proves the presentation recorder perturbs ZERO simulation (RNG
# snapshot/restore + transient cleanup are sound), so it can never corrupt a save or
# leak transients. Differential — no baseline, no rebless; guards every record family
# (PRESENTATION_RECORD_REPLAY_SPEC.md).
echo
echo "== record-purity gate (f2_server: F2_SERVER_PRES_RECORD off vs on) =="
"$ROOT/tests/golden/run_record_purity.sh" || exit 1

# Gate 3: netstream — the BINARY wire (F2_NETSTREAM) reconstructed and validated
# against an independent state_dump (P5-C STEP 2). This gate caught two
# silent-corruption holes nothing else in check.sh could see (netId recycling on
# map change; map index is not a change signal); wired in 2026-07-15 so the wire
# is guarded on every build. (Siblings run_golden_replay.sh / run_golden_narrate.sh
# stay hand-run — replay is subsumed by this, narrate is a debug channel.)
echo
echo "== netstream gate (binary wire vs state_dump) =="
"$ROOT/tests/golden/run_golden_netstream.sh" || exit 1

# Gate 4: netsocket — the wire over a real TCP socket (P5-C STEP 3). Transport
# faithfulness: socket-carried bytes == file-carried bytes for the same
# deterministic f2_server run (content correctness is gate 3's job). Guards the
# SocketByteSink so a transport regression cannot ship silently.
echo
echo "== netsocket gate (socket transport == file transport) =="
"$ROOT/tests/golden/run_golden_netsocket.sh" || exit 1

# Gate 5: join round-trip — the STEP-4 snapshot BLOB (save pipeline) serialized by
# a "server" and reconstructed by a fresh "client" instance. Pins the load-bearing
# §C claim (netId is not persisted, yet both sides reproduce it by re-running
# objectAssignAllNetIds over the same object set) plus full world fidelity
# (positions/flags/inventory). Guards the join pipeline on every build.
echo
echo "== join round-trip gate (blob save == reconstructed load) =="
"$ROOT/tests/golden/run_golden_join.sh" || exit 1

# Gate 6: joining client — the STEP-4 §D decoder. Inverse of the netstream gate: a
# headless client CONSUMES the wire (join blob + live event stream) and rebuilds the
# tactical object world; its reconstruction must match the server's over all synced
# state (obj/dude position/flags/hp + game_time). Guards the decoder on every build.
echo
echo "== joining-client gate (wire consumed == server world) =="
"$ROOT/tests/golden/run_golden_client.sh" || exit 1

# Gate 7: mid-stream join (STEP 5). A second SDL-dummy viewer connects to a live,
# already-streaming server; the joiner's baseline tripwire must score clean
# (netId re-walk alignment) and the first viewer must survive the C.4 rebaseline
# broadcast. Live sockets + wall-clock staggering — the one gate that exercises
# acceptPending/serverRequestRebaseline end to end.
echo
echo "== mid-stream join gate (late viewer + rebaseline broadcast) =="
"$ROOT/scripts/check_midjoin.sh" || exit 1

# Gate 8: controllable client (STEP 6). A raw wire client drives the CONTROL plane
# (claim + mv) and the server's authoritative move must ride the wire back out.
# The one gate that exercises per-session identity + serverControlLine end to end.
echo
echo "== control gate (wire client claim+mv → authoritative MOVE) =="
"$ROOT/scripts/check_control.sh" || exit 1

# Gate 9: resumable server combat (P5-C / P2). The beat-spanning combat session
# machine (F2_SERVER_RESUMABLE_COMBAT) — a fight spans beats, dude turns carry an
# idle-timer deadline, and a mid-fight CMD injection lands. Impossible pre-P2.
echo
echo "== resumable gate (beat-spanning combat: span + deadline + mid-fight attack) =="
"$ROOT/scripts/check_resumable.sh" || exit 1

# Gate 10: P3 wire combat. A fight driven entirely through the claim-gated VIEWER
# WIRE (claim + bare cattack, the viewer's 'A' key) with NO TURN_WAIT — the
# connected claimant alone holds the dude's turn open and the wire attack resolves.
echo
echo "== wire-combat gate (claimant drives a fight over the wire: claim + cattack → dude attack) =="
"$ROOT/scripts/check_wire_combat.sh" || exit 1

echo
echo "ALL GATES PASS"
