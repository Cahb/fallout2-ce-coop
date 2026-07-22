#!/usr/bin/env bash
# Narrate-golden runner (P5-B event-stream regression lock).
#
# The server/legacy goldens pin the SIMULATION STATE (the final canonical
# dump). This runner pins the SERVER'S EVENT STREAM — the sequence of
# presentation-seam calls a dedicated server would serialize onto the wire
# (MP_PROTOCOL.md §2). It replays each case under F2_NARRATE (the narrating
# presenter, presenter_narrate.cc) + the server loop, filters stdout to the
# STRUCTURAL channels, and diffs against a checked-in golden. A refactor that
# silently stops firing an event — or fires a spurious one — fails the diff
# even when the final state dump is unchanged.
#
# Channels captured (the wire-relevant STATE/lifecycle stream):
#   spawn destroy delta maptrans           (object_delta.cc / object.cc / map.cc)
#   connect disconn                        (object.cc _obj_connect/_obj_disconnect —
#                                           item<->world tile lifecycle)
#   combatEnter combatExit turnStart attackResult   (combat.cc — once hooked)
# Deliberately DROPPED:
#   move  — the per-hop path flood (thousands of lines/case); the resulting
#           positions are already pinned by the state-dump goldens.
#   world — the in-game clock ticks once per beat (pure cadence noise; the
#           final game-time is pinned by the state dump).
#   log float error — localized free-text flavor (combat log, float numbers).
#           It is derived from state already carried by delta/attackResult and
#           carries codepage (non-ASCII) bytes; excluding it keeps the goldens
#           clean ASCII and structural.
#
# Usage:
#   tests/golden/run_golden_narrate.sh            # verify all cases
#   BLESS=1 tests/golden/run_golden_narrate.sh    # re-record (after a signed-
#                                                 # off, intentional change)
# Env overrides:
#   F2_GAME_DIR  game assets dir (default <repo>/FO2)
#   F2_BIN       game binary     (default <repo>/build/fallout2-ce)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
GOLDEN_DIR="$ROOT/tests/golden/narrate"
BLESS="${BLESS:-0}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"' EXIT

mkdir -p "$GOLDEN_DIR"

# Structural event channels only. grep -a: some in-game strings (float/log) can
# carry high-ASCII codepage bytes that make GNU grep treat the piped stream as
# binary and drop ALL matches; -a forces text mode. (The structural lines the
# filter keeps are pure ASCII — ids/pids/hex/tiles — so the golden stays clean.)
STRUCT_FILTER='^\[t=[0-9]+\] (spawn|destroy|connect|disconn|delta|maptrans|combatEnter|combatExit|turnStart|attackResult)\b'

run_case() {
    local name="$1" map="$2" seed="$3" ticks="$4" aggro="${5:-}" actions="${6:-}"
    local out
    out="$(mktemp)"

    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 F2_NARRATE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP=/dev/null \
        ${aggro:+F2_PROBE_AGGRO="$aggro"} \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout -k 5 300 "$BIN" 2>/dev/null | grep -aE "$STRUCT_FILTER" > "$out") || {
        # grep exits 1 when a case legitimately emits zero structural events;
        # only a non-zero binary/timeout is a real failure. Distinguish via the
        # golden: an empty capture that matches an empty golden is still a PASS.
        true
    }

    if [ "$BLESS" = "1" ]; then
        cp "$out" "$GOLDEN_DIR/$name.narrate.txt"
        echo "BLESSED $name ($(wc -l < "$out") event lines)"
    elif [ ! -f "$GOLDEN_DIR/$name.narrate.txt" ]; then
        echo "FAIL $name — no golden (run with BLESS=1 to record)"
    elif diff -q "$GOLDEN_DIR/$name.narrate.txt" "$out" > /dev/null 2>&1; then
        echo "PASS $name ($(wc -l < "$out") event lines)"
    else
        echo "FAIL $name — event stream diverged from narrate golden:"
        diff "$GOLDEN_DIR/$name.narrate.txt" "$out" | head -30 || true
    fi

    rm -f "$out"
}

#        name                map           seed  ticks  aggro  actions
# Door open: OBJECT_DELTA_FLAGS on the door (OPEN_DOOR bit) + ambient deltas.
run_case kladwtwn_door_open  kladwtwn.map  42    1200   ""     "300:usedoor:0" > "$RESULTS/1.log" 2>&1 &
# Scripted lethal + non-lethal damage: delta hp/results/flags, corpse finalize
# (critterKill), the death event stream.
run_case denbus1_cdamage     denbus1.map   42    1500   ""     "300:cdamage:100,600:cdamage:15" > "$RESULTS/2.log" 2>&1 &
# Barter (modal): inventory-membership delta (OBJECT_DELTA_INVENTORY) on the
# dude + store box, and the barter tables' spawn/destroy lifecycle.
run_case denbus1_barter      denbus1.map   42    340    ""     "100:give:41:300,286:dsay:0,288:btake:259:1,290:boffer:41:200,292:bcommit:0,294:bdone:0,300:dtalk:47" > "$RESULTS/3.log" 2>&1 &
# Melee combat: hp/AP/combat-results deltas across a fight (the combat state
# stream; combat-control events enrich this once hooked).
run_case klatoxcv_combat     klatoxcv.map  42    4000   3      "" > "$RESULTS/4.log" 2>&1 &
# In-game map transition: entermap:6 (denbus1) stages a MapTransition that
# mapHandleTransition performs on the next beat, firing the mapTransition event
# (map.cc mapLoad choke). This is the ONLY case that exercises a real map load
# under the server loop — the initial load precedes the narrate presenter and
# wmtravel never arrives. Short window so the maptrans line is the focus.
run_case arvillag_entermap   arvillag.map  42    250    ""     "100:entermap:6" > "$RESULTS/5.log" 2>&1 &
# Item<->world lifecycle (object.cc _obj_connect/_obj_disconnect): give:41 mints a
# Money stack (spawn) and disconnects it into the dude's inventory (disconn); drop:41
# re-connects it to the dude's tile (connect); pickup:0 detaches the same ground item
# back into inventory (disconn) — the full inventory<->world round-trip on ONE object
# id. Pins that a dropped item's world appearance and a picked-up item's disappearance
# cross the seam — neither rides objectMoved/objectCreated.
run_case denbus1_itemworld   denbus1.map   42    1500   ""     "300:give:41,600:drop:41,900:pickup:0" > "$RESULTS/6.log" 2>&1 &
# Intra-item field delta (MP_PROTOCOL.md §6.2b piece 4): the dude wields a .223
# pistol (give+reload+wield pid 241) then fires (cattack) in combat. Firing
# decrements the WIELDED weapon's ammoQuantity with NO inventory membership change,
# so the dude's death-beat delta carries OBJECT_DELTA_INVENTORY (inv=) ONLY because
# the per-item hash now folds intra-item union fields. A/B-verified: without that
# fold, the dude's t=50100 delta has fid/flags/hp/results but no inv bit (the weapon
# stays wielded on death — no membership change). This is the sole case that locks
# the intra-item extension; it also pins the wider gunfight combat delta stream.
# NOTE: the isolation relies on the scheduler placing the death-anim weapon drop
# (itemDropAll, a deferred callback) on a LATER beat than the ammo/death beat, so the
# t=50100 inv= bit is ammo-driven not drop-driven. The manual A/B rebuild is the real
# oracle for that; this golden pins determinism + the stream shape.
run_case arvillag_gunfight   arvillag.map  42    4000   ""     "290:give:241,295:reload:241,300:wield:1,490:cattack:60,500:aggro:3" > "$RESULTS/7.log" 2>&1 &
wait

cat "$RESULTS"/*.log
grep -q "^FAIL" "$RESULTS"/*.log && exit 1
exit 0
