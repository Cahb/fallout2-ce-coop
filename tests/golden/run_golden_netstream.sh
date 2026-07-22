#!/usr/bin/env bash
# Netstream gate (P5-C STEP 2 "MAKE IT TALK": the BINARY wire -> reconstructed state).
#
# Sibling of run_golden_replay.sh. That gate validates the NARRATE TEXT serialization
# of the event stream; this one validates the REAL BINARY WIRE (F2_NETSTREAM,
# src/presenter_network.cc — the encoding a socket carries in STEP 3). Both feed the
# SAME encoding-agnostic reconstructor + validator in tools/replay.py, which
# dispatches on the stream's magic ("F2NS" = binary, else narrate text).
#
# ►► THE ORACLE IS THE STATE DUMP, NOT THE OTHER SERIALIZATION (design review
# 2026-07-15). It is tempting to gate "binary reconstructs the same world as narrate"
# — that is WEAK: it only proves the two encoders agree, so a SHARED omission (both
# written from the same reading of the spec, both mis-gating suppression the same way)
# passes silently. It is translation validation, not correctness. So each case here
# validates the binary reconstruction against `state_dump`, which reads live sim state
# INDEPENDENTLY of the presenter seam — non-circular, and free. That the profiles below
# happen to equal run_golden_replay.sh's is a welcome cross-check, not the claim.
#
# Determinism (already golden-locked) is what lets the binary and text captures come
# from SEPARATE runs: presenters are one-way sinks and cannot perturb the sim, so no
# tee is needed.
#
# A case PASSES only when BOTH hold:
#   (1) replay.py exits 0 — no reconstructed position the dump contradicts, no frame
#       seq gap, no truncated/trailing bytes (the binary front-end is strict: it
#       raises on any framing violation, so a malformed wire FAILS rather than
#       silently decoding to a plausible world);
#   (2) the PROFILE line equals the pinned expected value (regression lock).
#
# Usage: tests/golden/run_golden_netstream.sh
# Env: F2_GAME_DIR (default <repo>/FO2), F2_BIN (default <repo>/build/fallout2-ce)
#
# NOTE the binary is the CLIENT PROBE, not f2_server — deliberate. presenter_network.cc
# lives in f2_core precisely so the probe (the only binary with a golden oracle: a real
# map, real scripts, a state_dump to check against) can emit and gate the wire.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
REPLAY="$ROOT/tools/replay.py"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"' EXIT

# No stream filter: unlike narrate (stdout text needing a grep), the netstream is a
# self-delimiting binary file written straight to F2_NETSTREAM. Every event the
# presenter emits is in it, and replay.py structurally skips the types it does not
# consume (skip-unknown-T) rather than a grep dropping them.
run_case() {
    local name="$1" expect="$2" map="$3" seed="$4" ticks="$5" actions="${6:-}"
    local stream="$RESULTS/$name.nets" dump="$RESULTS/$name.dump"

    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 \
        F2_NETSTREAM="$stream" F2_JOIN_TMP="$stream.blobtmp" \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP="$dump" \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout -k 5 300 "$BIN" >/dev/null 2>&1) || true

    local rc=0
    python3 "$REPLAY" "$stream" "$dump" > "$RESULTS/$name.out" 2>&1 || rc=1
    local got
    got="$(grep -a '^PROFILE ' "$RESULTS/$name.out" || true)"
    if [ "$rc" = 0 ] && [ "$got" = "$expect" ]; then
        echo "PASS $name — ${got#PROFILE }"
    else
        echo "FAIL $name — binary wire reconstruction diverged (rc=$rc):"
        echo "    expected: $expect"
        echo "    got:      ${got:-<none>}"
        sed 's/^/    /' "$RESULTS/$name.out"
    fi
}

#        name       expected PROFILE                                     map           seed ticks actions
# The four cases mirror run_golden_replay.sh's, and the profiles MATCH it exactly —
# the binary wire carries the same state the text stream does.
run_case walk      "PROFILE matched=1837 moved=11 mismatched=0 not_in_dump=0" arvillag.map 42 3000 "300:walkto:20527" > "$RESULTS/1.log" 2>&1 &
run_case combat    "PROFILE matched=3659 moved=8 mismatched=0 not_in_dump=0"  klatoxcv.map 42 4000 "" > "$RESULTS/2.log" 2>&1 &
run_case cdamage   "PROFILE matched=2864 moved=17 mismatched=0 not_in_dump=0"  denbus1.map  42 1500 "300:cdamage:100,600:cdamage:15" > "$RESULTS/3.log" 2>&1 &
run_case itemworld "PROFILE matched=2863 moved=19 mismatched=0 not_in_dump=0"  denbus1.map  42 1500 "300:give:41,600:drop:41,900:pickup:0" > "$RESULTS/4.log" 2>&1 &
# ►► THE MAP-CHANGE CASE — the one no other gate has, and the reason this file exists
# beyond encoding coverage. A mid-run transition RECYCLES every netId
# (objectAssignAllNetIds resets the counter to 1 and re-walks the new object set), so
# without the post-transition re-baseline (serverEmitBaseline, server_loop.cc) every
# netId the consumer holds silently comes to mean a DIFFERENT object — the stream does
# not go incomplete, it LIES, and NOTHING else in the suite would notice: none of the
# four cases above changes maps, and replay.py's own docstring depended on that.
# arvillag -> denbus1 (entermap:6) crosses a real exit grid.
# A/B-VERIFIED (2026-07-15, measured — not estimated): with serverEmitBaselineIfMapChanged
# suppressed, this case reports
#     matched=7 moved=6 mismatched=134 not_in_dump=1704   (UNTRACKED=1117)
# and REPLAY FAILs. With it, matched=2863 mismatched=0 not_in_dump=1 UNTRACKED=0.
# So the case has real teeth: it is the difference between a coherent world and
# near-total garbage, and it is invisible to every other gate in the suite.
# (not_in_dump was 2 before the STEP-4 objectAssignAllNetIds NO_SAVE-skip, which
# aligned the walk domain with the blob/delta domain — CLIENT_JOIN_DESIGN.md §C.
# It went 1 -> 0 when objectMoved stopped emitting MOVE for netId 0: the last
# phantom was gEgg, which objectSetLocation drags along with the dude, so the wire
# announced a ghost the reconstruction had to carry that the dump never contains.
# not_in_dump=0 is now the real invariant: everything the wire names is a real,
# dumped object.)
run_case xmap      "PROFILE matched=2863 moved=10 mismatched=0 not_in_dump=0"  arvillag.map 42 600  "100:entermap:6" > "$RESULTS/5.log" 2>&1 &
# ►► THE SAME-INDEX RE-ENTRY CASE — subtler than xmap and it broke the same way.
# Loading the SAME map index twice (leave and come back) tears down and rebuilds every
# object while mapGetCurrentMap() never changes, so an index-equality check misses the
# rebaseline entirely and the fresh objects keep netId 0 (objectAllocate zeroes it,
# object.cc:3444) => every object collides on 0. THIS is why the change signal is
# mapGetLoadGeneration() and not the map index (map.h).
# A/B-VERIFIED (2026-07-15, measured): keyed on the map INDEX this case reports
#     matched=2 moved=1 mismatched=0 not_in_dump=2864
# and replay.py STILL PRINTS "REPLAY OK" — its exit check is `not mismatches and
# matches`, and 2 matches with 0 mismatches passes. Only the pinned PROFILE catches it.
# That is exactly why the profile pin, not the exit code, is this gate's teeth.
run_case xmap_same "PROFILE matched=2863 moved=10 mismatched=0 not_in_dump=0"  arvillag.map 42 900  "100:entermap:6,400:entermap:6" > "$RESULTS/6.log" 2>&1 &
wait

cat "$RESULTS"/*.log
grep -q "^FAIL" "$RESULTS"/*.log && exit 1
exit 0
