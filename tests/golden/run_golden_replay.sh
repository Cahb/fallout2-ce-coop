#!/usr/bin/env bash
# Replay gate (P5-C slice 1: event-stream -> reconstructed state validation).
#
# The server/legacy goldens pin the SIM STATE; the narrate goldens pin the EVENT
# STREAM. This gate is the first CONSUMER: for each case it runs the server loop
# under F2_NARRATE (the first serializer of the wire events, presenter_narrate.cc)
# capturing the FULL lifecycle+move stream AND the authoritative state_dump in one
# run; then tools/replay.py reads the stream back, reconstructs each object's on-map
# position by applying the events, and (a) fails on any reconstructed position the
# dump contradicts, (b) pins the reconstruction PROFILE per case so drift fails.
# This is the file-replayer later P5-C slices reuse with a socket source instead of
# a file (MP_PROTOCOL.md §7; p5-server-plan memory).
#
# WHAT THIS IS / IS NOT (adversarial review 2026-07-13): it is a RECONSTRUCTION
# REGRESSION GATE + a semantic position check -- NOT a from-scratch completeness
# oracle. It cannot be, yet: obj->id collides ~53% and there is no join snapshot, so
# the tool disambiguates by pid and validates the subset it can resolve. The pinned
# PROFILE is what gives it teeth against LOST lifecycle events (a lost destroy /
# disconnect leaves an on-map object the dump no longer attests -> not_in_dump grows
# past the pinned count -> FAIL). Residual leniency, bounded by the missing unique
# server id (a later P5-C slice), is documented in tools/replay.py. Known reported
# deferrals (not failures): objects loaded before/silently -> JOIN SNAPSHOT.
# Note vs the narrate gate: this capture KEEPS move (every hop); the narrate
# regression goldens drop it.
#
# Usage: tests/golden/run_golden_replay.sh
# Env: F2_GAME_DIR (default <repo>/FO2), F2_BIN (default <repo>/build/fallout2-ce)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
REPLAY="$ROOT/tools/replay.py"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"' EXIT

# Lifecycle + move channels the replayer consumes (move INCLUDED, unlike narrate),
# plus the join baseline `snapshot` (the t=0 ground truth the replayer seeds from —
# every object already present before the event stream began).
STREAM_FILTER='^\[t=[0-9]+\] (snapshot|spawn|move|destroy|connect|disconn|maptrans)\b'

# A case PASSES only when BOTH hold:
#   (1) replay.py exits 0 -- no reconstructed position the dump contradicts (semantic);
#   (2) the PROFILE line equals the pinned expected value (regression lock).
# (2) is what makes a LOST destroy/disconnect fail: such a hole leaves an object
# on-map that the dump no longer attests, growing not_in_dump (and usually
# mismatched) -- so the profile drifts and the case fails. Without (2) those holes
# would slide into the non-fatal not_in_dump bucket unnoticed (adversarial review
# 2026-07-13, Finding 1/2). `moved` pins a MEANINGFUL coverage floor (matches with a
# real net displacement, not trivial same-tile self-consistency -- Finding 5).
run_case() {
    local name="$1" expect="$2" map="$3" seed="$4" ticks="$5" actions="${6:-}"
    local stream="$RESULTS/$name.stream" dump="$RESULTS/$name.dump"

    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 F2_NARRATE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP="$dump" \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout -k 5 300 "$BIN" 2>/dev/null | grep -aE "$STREAM_FILTER" > "$stream") || true

    local rc=0
    python3 "$REPLAY" "$stream" "$dump" > "$RESULTS/$name.out" 2>&1 || rc=1
    local got
    got="$(grep -a '^PROFILE ' "$RESULTS/$name.out" || true)"
    if [ "$rc" = 0 ] && [ "$got" = "$expect" ]; then
        echo "PASS $name — ${got#PROFILE }"
    else
        echo "FAIL $name — reconstruction diverged (rc=$rc):"
        echo "    expected: $expect"
        echo "    got:      ${got:-<none>}"
        sed 's/^/    /' "$RESULTS/$name.out"
    fi
}

#        name       expected PROFILE                                     map           seed ticks actions
# NOTE the profiles below are with the JOIN BASELINE snapshot seeding the world
# (run_golden_replay adds the `snapshot` channel): `matched` is now ~the entire
# on-map object set (every object validated against the dump), UNTRACKED is 0, and
# `moved` (matches with real net displacement) stays the meaningful coverage floor.
# A lost destroy/disconnect still grows not_in_dump past the pin; a lost move flips
# a baseline object matched->mismatched (STRONGER than pre-baseline: the object is
# always tracked now, never silently untracked). None of these cases change maps,
# so netids are stable and the single install-time baseline covers the whole run.
# Movement: the dude walks a path -> per-hop move events reconstruct its trajectory.
run_case walk      "PROFILE matched=1837 moved=11 mismatched=0 not_in_dump=2" arvillag.map 42 3000 "300:walkto:20527" > "$RESULTS/1.log" 2>&1 &
# Combat: critters move/die in a fight -> combat-driven position stream.
run_case combat    "PROFILE matched=3659 moved=8 mismatched=0 not_in_dump=1"  klatoxcv.map 42 4000 "" > "$RESULTS/2.log" 2>&1 &
# Death + gore: cdamage kills critters (spawns a NO_SAVE transient that collides
# with dumped scenery ids -> exercises pid-disambiguation; destroys must reduce the
# on-map set, so a lost destroy would grow not_in_dump above the pinned 2).
run_case cdamage   "PROFILE matched=2864 moved=17 mismatched=0 not_in_dump=2"  denbus1.map  42 1500 "300:cdamage:100,600:cdamage:15" > "$RESULTS/3.log" 2>&1 &
# Item<->world: give/drop/pickup -> spawn/connect/disconnect lifecycle; a lost
# disconnect would leave the picked-up item on-map, growing not_in_dump above 2.
run_case itemworld "PROFILE matched=2863 moved=19 mismatched=0 not_in_dump=2"  denbus1.map  42 1500 "300:give:41,600:drop:41,900:pickup:0" > "$RESULTS/4.log" 2>&1 &
wait

cat "$RESULTS"/*.log
grep -q "^FAIL" "$RESULTS"/*.log && exit 1
exit 0
