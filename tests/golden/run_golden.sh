#!/usr/bin/env bash
# Phase 0 golden replay runner (REWRITE_PLAN.md item 0.5).
#
# Replays each case headlessly under the synthetic clock and diffs the
# canonical state dump against the checked-in golden. Any refactor that
# changes simulation behavior fails the diff.
#
# Usage:
#   tests/golden/run_golden.sh            # verify all cases
#   BLESS=1 tests/golden/run_golden.sh    # re-record goldens (after an
#                                         # intentional, signed-off change)
# Env overrides:
#   F2_GAME_DIR  game assets dir (default <repo>/FO2)
#   F2_BIN       game binary     (default <repo>/build/fallout2-ce)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
GOLDEN_DIR="$ROOT/tests/golden"
BLESS="${BLESS:-0}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"' EXIT

run_case() {
    local name="$1" map="$2" seed="$3" ticks="$4" trace="${5:-}" aggro="${6:-}" actions="${7:-}"
    local out
    out="$(mktemp)"

    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP="$out" \
        ${trace:+F2_INPUT_REPLAY="$GOLDEN_DIR/$trace"} \
        ${aggro:+F2_PROBE_AGGRO="$aggro"} \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout 300 "$BIN" > /dev/null 2>&1) || {
        echo "FAIL $name (binary exited non-zero)"
        rm -f "$out"
        return
    }

    if [ "$BLESS" = "1" ]; then
        cp "$out" "$GOLDEN_DIR/$name.golden.txt"
        echo "BLESSED $name ($(wc -l < "$out") lines)"
    elif diff -q "$GOLDEN_DIR/$name.golden.txt" "$out" > /dev/null 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name — state diverged from golden:"
        diff "$GOLDEN_DIR/$name.golden.txt" "$out" | head -20 || true
    fi

    rm -f "$out"
}

# Cases run concurrently; each writes its verdict to $RESULTS.
#        name             map           seed  ticks  trace              aggro
run_case artemple_idle    artemple.map  1337  2000   ""                 ""  > "$RESULTS/1.log" 2>&1 &
run_case arvillag_idle    arvillag.map  42    3000   ""                 ""  > "$RESULTS/2.log" 2>&1 &
run_case arvillag_walk    arvillag.map  42    3000   trace_walk.txt     ""  > "$RESULTS/3.log" 2>&1 &
run_case klatoxcv_combat  klatoxcv.map  42    4000   trace_endturn.txt  3   > "$RESULTS/4.log" 2>&1 &
# Coverage case for Batch-1 converted paths: XP/level-up (stat.cc),
# radiation+poison (critter.cc), stimpak drug effect (item.cc, pid 40).
# H-43: pcLevelUpApply awards SP for the levels gained from the xp grant.
run_case arvillag_actions arvillag.map  42    2500   ""                 ""  "300:xp:4500,600:rad:150,900:poison:45,1200:drug:40,1500:levelup:1" > "$RESULTS/5.log" 2>&1 &
# Batch-2 coverage: skillUse paths (hurt -> first aid/doctor time-skips+fades,
# sneak toggle) and proto_instance door use (sfx+frame+msg).
run_case arvillag_skills  arvillag.map  42    2500   ""                 ""  "300:hurt:25,600:useskill:6,900:useskill:7,1200:useskill:8" > "$RESULTS/6.log" 2>&1 &
run_case kladwtwn_door    kladwtwn.map  42    2200   ""                 ""  "300:usedoor:0,900:usedoor:0" > "$RESULTS/7.log" 2>&1 &
# Batch-6 coverage: inventory context-menu actions extracted to core —
# itemDropStack (money split, single money, live explosive, multi-drop
# re-fetch loop), weaponUnloadIntoInventory (10mm pistol -> ammo pack),
# itemUseDrug (stimpak heal after hurt), itemUseFromInventory (flare).
# H-6 coverage: weaponLoadAmmo (reload the unloaded pistol; pack merge +
# consumed-pack re-fetch), containerStoreItem (stow mentats into a bag).
# H-7/H-9 coverage: lootall (open gates + hidden-box detach/reattach +
# take-all from nearest container), stealall (equip strip/re-equip cycle).
run_case arvillag_invmenu arvillag.map  42    2500   ""                 ""  "300:give:41,320:give:41,340:give:41,400:drop:41,450:give:41,470:drop:41,500:give:206,550:drop:206,600:give:8,650:unload:8,680:hurt:20,700:give:40,720:usedrug:40,800:give:79,850:useitem:79,900:give:53,920:give:53,940:give:53,1000:drop:53,1100:reload:8,1150:give:46,1200:give:53,1250:stow:53,1300:lootall:0,1350:stealall:0" > "$RESULTS/8.log" 2>&1 &
# H-13 coverage: worldmap travel sim extracted to core (worldmapTravelStep/
# RestHeal/MarkVisited/ClockTick/EncounterCheck). wmtravel arg packs the
# destination as destX*10000+destY: walk (173,122) -> (280,230). Pins the
# 18000-ticks-per-step clock advance (143 steps), arrival + area re-match,
# H-16 subtile exploration marking, and the 1000ms rest-heal cadence on the
# hurt dude (state dump 'worldmap' line + game_time + dude hp).
run_case arvillag_wmtravel arvillag.map 42    1200   ""                 ""  "300:hurt:30,400:wmtravel:2800230" > "$RESULTS/9.log" 2>&1 &
# H-44/H-47/H-48 coverage: perk-dialog commits extracted to core. xp grant
# reaches level 13 (>= perk min levels); mentats (+2 IN) qualify the dude for
# Educated. perk:18 Educated (+2 unspent SP -> dude_pcstat 0), perk:28
# Lifegiver (+4 max HP and heal -> dude_stat 7 / dude hp), perk:51 Tag! +
# tag4:12 (science as 4th tag -> dude_tags), perk:52 Mutate! + mutate:2
# (drop Heavy Handed, gain Small Frame -> dude_traits, +1 AG in dude_stat 5).
run_case arvillag_perks   arvillag.map  42    1500   ""                 ""  "300:xp:80000,400:give:53,450:usedrug:53,500:perk:18,600:perk:28,700:perk:51,750:tag4:12,800:perk:52,850:mutate:2" > "$RESULTS/10.log" 2>&1 &
# H-40/H-42 coverage: pipboy rest sim + intent decode extracted to core
# (restSimPacing/MinutesTick/MinutesFinish/HoursTick/HoursFinish/
# OverdueEvents, restUntilHealedDuration, restOptionDecode/
# restUntilHourDuration). hurt:30 wounds the dude (14/44), rest:90 pins the
# 1h30m clock advance across both sim phases (no heal: 84 accrued rest
# minutes < 180 cadence), restopt:8 pins the until-morning wall-clock math
# (clock lands on next-day 8:00) + the 180-minute heal cadence over ~23h,
# restopt:12 pins the rest-until-healed hpToHeal/healingRate*3.0 formula and
# the two-pass 24h/3h chunking (dude back to 44/44). Effects pinned via
# game_time + dude hp + queue_next_time in the state dump.
run_case arvillag_rest    arvillag.map  42    1200   ""                 ""  "300:hurt:30,400:rest:90,500:restopt:8,600:restopt:12" > "$RESULTS/11.log" 2>&1 &
# Cross-system determinism (user-requested): sleep THEN fight. Rest 6h on a
# rest-allowed map (clock interpolation + queue processing incl. RNG-drawing
# events), then aggro 3 villagers into combat driven by a hand-authored SPACE
# trace. Pins that the post-rest RNG/queue state makes the entire fight
# reproducible (dump is byte-identical across runs, rng_state included).
run_case arvillag_restfight arvillag.map 42  4000   trace_restfight.txt ""  "250:mark:0,300:hurt:10,350:mark:0,400:rest:360,450:mark:0,500:aggro:3,3000:mark:0" > "$RESULTS/12.log" 2>&1 &
# H-49 coverage: the character editor's begin/rollback transaction over
# committed dude state, extracted to character_transaction.cc. charsnap:0
# takes the snapshot (hp=44, unspent SP=0), then the committed state is
# mutated - hurt:25 drops the dude to 19/44 and levelup:0 awards 10 unspent
# skill points - and charroll:0 rolls it back via the extracted restore steps
# (proto/name + skill points + derived-stat recompute + hit-points adjust).
# Pins that the rollback returns dude hp and dude_pcstat 0 to the snapshot.
run_case arvillag_chartxn arvillag.map 42    1500   ""                 ""  "300:charsnap:0,400:hurt:25,500:levelup:0,600:charroll:0" > "$RESULTS/13.log" 2>&1 &
# Batch-7 audit coverage (dense town): The Den (denbus1 — 2862 objects, 71
# critters), a far richer map-load + object set than the Arroyo cases. Two
# lootall passes drive the loot session primitives (lootOpenCheck/detach/
# take-all/reattach) against the nearest containers. Verified byte-identical
# dbf2552 (pre-Batch-7) -> HEAD, so it pins that the core/client TU splits and
# the f2_core/f2_client library split did not perturb dense map load or loot.
run_case denbus1_loot     denbus1.map   42    1500   ""                 ""  "400:lootall:0,800:lootall:0" > "$RESULTS/14.log" 2>&1 &
wait

cat "$RESULTS"/*.log
grep -q "^FAIL" "$RESULTS"/*.log && exit 1
exit 0
