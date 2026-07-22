#!/usr/bin/env bash
# Fixed-timestep SERVER-LOOP golden gate (SERVER_LOOP_DESIGN.md).
#
# Replays each case headlessly under F2_SERVER_LOOP=1 and diffs the state dump
# against the checked-in server golden (tests/golden/server/*.golden.txt). This
# is the second gate alongside run_golden.sh: run_golden.sh pins the LEGACY
# frame-driven probe byte-for-byte; this pins the server loop's deterministic,
# outcome-equivalent behavior. The two golden sets legitimately differ in
# timing/RNG-cadence fields (game_time, rng_state, queue timestamps, ambient
# wander, RNG damage/heal rolls) — see the equivalence review.
#
# Differences from run_golden.sh: F2_SERVER_LOOP=1; input-replay SPACE traces
# dropped (auto-end-turn); the walk trace replaced by a walkto intent. Combat
# (klatoxcv, restfight) resolves with the dude passing every turn — the same
# semantics the old SPACE traces produced. F2_FAKE_CLOCK stays set (still
# load-bearing for the wmtravel probe driver).
#
# Usage:
#   tests/golden/run_golden_server.sh          # verify all cases
#   BLESS=1 tests/golden/run_golden_server.sh  # re-record (signed-off change)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_BIN:-$ROOT/build/fallout2-ce}"
GOLDEN_DIR="$ROOT/tests/golden/server"
BLESS="${BLESS:-0}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"; pkill -9 -x fallout2-ce 2>/dev/null || true' EXIT

mkdir -p "$GOLDEN_DIR"

run_case() {
    local name="$1" map="$2" seed="$3" ticks="$4" aggro="${5:-}" actions="${6:-}"
    local out
    out="$(mktemp)"

    (cd "$GAME_DIR" && env \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1 F2_SERVER_LOOP=1 \
        F2_PROBE_MAP="$map" F2_PROBE_SEED="$seed" F2_PROBE_TICKS="$ticks" \
        F2_PROBE_DUMP="$out" \
        ${aggro:+F2_PROBE_AGGRO="$aggro"} \
        ${actions:+F2_PROBE_ACTIONS="$actions"} \
        timeout -k 5 300 "$BIN" > /dev/null 2>&1) || {
        echo "FAIL $name (binary exited non-zero / timed out)"
        rm -f "$out"
        return
    }

    if [ "$BLESS" = "1" ]; then
        cp "$out" "$GOLDEN_DIR/$name.golden.txt"
        echo "BLESSED $name ($(wc -l < "$out") lines)"
    elif diff -q "$GOLDEN_DIR/$name.golden.txt" "$out" > /dev/null 2>&1; then
        echo "PASS $name"
    else
        echo "FAIL $name — state diverged from server golden:"
        diff "$GOLDEN_DIR/$name.golden.txt" "$out" | head -20 || true
    fi

    rm -f "$out"
}

#        name             map           seed  ticks  aggro  actions
run_case artemple_idle    artemple.map  1337  2000   ""     ""  > "$RESULTS/1.log" 2>&1 &
run_case arvillag_idle    arvillag.map  42    3000   ""     ""  > "$RESULTS/2.log" 2>&1 &
# walkto:20527 walks the dude 10 hexes E (replaces the input-replay walk trace).
run_case arvillag_walk    arvillag.map  42    3000   ""     "300:walkto:20527"  > "$RESULTS/3.log" 2>&1 &
# Combat: aggro fixture, dude auto-ends every turn (was trace_endturn SPACE).
run_case klatoxcv_combat  klatoxcv.map  42    4000   3      ""  > "$RESULTS/4.log" 2>&1 &
run_case arvillag_actions arvillag.map  42    2500   ""     "300:xp:4500,600:rad:150,900:poison:45,1200:drug:40,1500:levelup:1" > "$RESULTS/5.log" 2>&1 &
run_case arvillag_skills  arvillag.map  42    2500   ""     "300:hurt:25,600:useskill:6,900:useskill:7,1200:useskill:8" > "$RESULTS/6.log" 2>&1 &
run_case kladwtwn_door    kladwtwn.map  42    2200   ""     "300:usedoor:0,900:usedoor:0" > "$RESULTS/7.log" 2>&1 &
# Single open (no re-close): pins the door-outcome DECOUPLE. Headless the
# reg_anim completion callbacks (_set_door_state_open/_check_door_state) that
# carry the open state never fired, so _obj_use_door was a silent no-op; the
# serverLoopActive() fast-path now applies the outcome directly. The nearest
# door ends OPEN → its dumped flags gain OBJECT_OPEN_DOOR (SHOOT_THRU|LIGHT_THRU|
# NO_BLOCK). The two-usedoor case above nets back to closed and can't observe it.
run_case arvillag_invmenu arvillag.map  42    2500   ""     "300:give:41,320:give:41,340:give:41,400:drop:41,450:give:41,470:drop:41,500:give:206,550:drop:206,600:give:8,650:unload:8,680:hurt:20,700:give:40,720:usedrug:40,800:give:79,850:useitem:79,900:give:53,920:give:53,940:give:53,1000:drop:53,1100:reload:8,1150:give:46,1200:give:53,1250:stow:53,1300:lootall:0,1350:stealall:0" > "$RESULTS/8.log" 2>&1 &
run_case arvillag_wmtravel arvillag.map 42    1200   ""     "300:hurt:30,400:wmtravel:2800230" > "$RESULTS/9.log" 2>&1 &
run_case arvillag_perks   arvillag.map  42    1500   ""     "300:xp:80000,400:give:53,450:usedrug:53,500:perk:18,600:perk:28,700:perk:51,750:tag4:12,800:perk:52,850:mutate:2" > "$RESULTS/10.log" 2>&1 &
run_case arvillag_rest    arvillag.map  42    1200   ""     "300:hurt:30,400:rest:90,500:restopt:8,600:restopt:12" > "$RESULTS/11.log" 2>&1 &
# Rest-then-fight: aggro via action, dude auto-ends turns (was trace_restfight).
run_case arvillag_restfight arvillag.map 42  4000   ""     "250:mark:0,300:hurt:10,350:mark:0,400:rest:360,450:mark:0,500:aggro:3,3000:mark:0" > "$RESULTS/12.log" 2>&1 &
run_case arvillag_chartxn  arvillag.map 42    1500   ""     "300:charsnap:0,400:hurt:25,500:levelup:0,600:charroll:0" > "$RESULTS/13.log" 2>&1 &
run_case denbus1_loot     denbus1.map   42    1500   ""     "400:lootall:0,800:lootall:0" > "$RESULTS/14.log" 2>&1 &
# Ground-item pickup: pins the actionPickUp DECOUPLE. Headless the _obj_pickup
# completion callback never fired (no animation), so pickup was a silent no-op;
# the serverLoopActive() fast-path now applies it directly. The nearest ground
# item (a weapon, pid 0x04) enters the dude's (empty) inventory and its world
# object is removed via _obj_disconnect (objects count -1).
run_case denbus1_pickup   denbus1.map   42    1500   ""     "300:pickup:0" > "$RESULTS/17.log" 2>&1 &
# Ladder/stairs climb: pins the _action_use_an_item_on_object DECOUPLE. Headless
# the _obj_use completion callback never fired (no animation), so the climb was
# a silent no-op; the serverLoopActive() fast-path now calls _obj_use directly.
# The nearest stairs (id=248, same-map) takes the dude elev 0 -> 1: gElevation
# and dude elev flip, map id (6) unchanged, dude tile -> the stairs destTile.
run_case denbus1_climb    denbus1.map   42    1500   ""     "300:climb:0" > "$RESULTS/18.log" 2>&1 &
# Active player combat via the combat intent queue (SERVER_LOOP_DESIGN §3): the
# dude wields a .223 pistol (give+reload+wield pid 241), then cattack queues 60
# "attack nearest hostile" intents before aggroing 3 villagers. Pins that the
# server _combat_input drives the dude's turn (attack/AP), that RANGED
# _combat_attack resolves headless via the de-coupled outcome path
# (_combat_apply_attack_results, no projectile animation), AND that critters the
# dude KILLS are finalized as corpses (go flat / OBJECT_NO_BLOCK / hp=0 via
# critterKill) rather than left blocking — the corpse-state regression the
# equivalence review caught. Deterministic; the dude kills before the village
# swarm overwhelms it.
run_case arvillag_gunfight arvillag.map 42   4000   ""     "290:give:241,295:reload:241,300:wield:1,490:cattack:60,500:aggro:3" > "$RESULTS/15.log" 2>&1 &
run_case kladwtwn_door_open kladwtwn.map 42   1200   ""     "300:usedoor:0" > "$RESULTS/16.log" 2>&1 &
# Scripted critter damage (op_critter_damage engine path). actionDamage's animated
# branch welds _apply_damage/critterKill to the _report_dmg reg_anim callback, which
# never fires under the server loop, so scripted damage/death was silently dropped.
# Pins the serverLoopActive() decouple: a lethal cdamage:100 kills the nearest
# blocking critter (hp->0, DAM_DEAD, corpse goes FLAT|NO_BLOCK via critterKill), and
# a non-lethal cdamage:15 drops the next critter's HP without killing it.
run_case denbus1_cdamage  denbus1.map   42    1500   ""     "300:cdamage:100,600:cdamage:15" > "$RESULTS/19.log" 2>&1 &
# Scripted/queued explosion (op_explosion + EXPLOSION queue-event engine path). Same
# weld as actionDamage in actionExplode: the animated branch drops radius damage,
# death, XP, and scenery destruction headless. Pins the serverLoopActive() decouple:
# explode:200 centred on the nearest blocking critter kills it (hp->0, corpse finalize).
run_case denbus1_explode  denbus1.map   42    1500   ""     "300:explode:200" > "$RESULTS/20.log" 2>&1 &
# Non-dead KNOCKBACK tile displacement. On the animated path a surviving (non-dead)
# critter is knocked to a new tile by actionKnockdown inside _show_damage_to_object;
# the server skips that animation, so _combat_knockback_headless applies the same
# displacement directly. That helper is shared by BOTH the combat-attack path
# (_combat_apply_attack_results) and this explosion path. explode:18 is sub-lethal on
# the nearest 25-HP critter (hp -> 7, stays alive), and knockback = damage/10 = 1 hex
# moves it from its tile (the blast center, 14500) to 14700. A/B-verified: WITH the
# decouple the survivor is at 14700; WITHOUT it stays at 14500 — that tile is the
# ONLY difference (objectSetLocation draws no RNG, so cadence is untouched). Short run
# keeps the displacement directly visible before ambient wander cascades.
run_case denbus1_knockback denbus1.map  42    305    ""     "300:explode:18" > "$RESULTS/21.log" 2>&1 &
# Use-skill-on-target (actionUseSkill). Its outcome (_obj_use_skill_on -> target's
# SCRIPT_PROC_USE_SKILL_ON, then skillUse) is welded to a reg_anim completion callback
# (animationRegisterCallback3) that never fires under the server loop, so a skill used
# on another object/critter was a silent no-op. Pins the serverLoopActive() decouple in
# actionUseSkill: cdamage:15 wounds the nearest critter (id=166, hp 33->18), tag4:6 tags
# First Aid so the roll can land, then useskillon:6 first-aids that same wounded critter.
# Observable on success: target hp 18->23 (skillUse heal payload), dude skill_uses_today
# 6->1 (the anti-spam slot, set only on success), dude XP 0->25 (the TARGET's
# USE_SKILL_ON script award -- proves the script-override path runs, not just skillUse),
# and game_time +1800s (gameTimeAddSeconds). The +1800s flushes queued events, so
# game_time/rng_state/queue/gvar-104(time counter)/ambient wander legitimately shift.
# Seed 47 (not 42): the single heal roll lands a success there; short 340-tick run keeps
# the heal directly visible before the time-flush cascade grows.
run_case denbus1_firstaid  denbus1.map   47    340    ""     "100:cdamage:15,120:tag4:6,300:useskillon:6" > "$RESULTS/22.log" 2>&1 &
# Aimed / called-shot attacks. The server combat path hard-coded HIT_LOCATION_UNCALLED
# (combat_ui.cc), so aimed shots were unreachable headless. The ATTACK combat intent now
# carries a hitLocation; the new caim:N verb queues one aimed "attack nearest hostile"
# with hit location N (EYES=6). Same gunfight setup (dude wields a .223, pid 241), then 5
# caim:6 (eyes) shots before aggroing 3 villagers. _combat_attack derives the +1 aimed AP
# cost, the -60 eyes to-hit penalty and the eyes crit table from defenderHitLocation, all
# in-sim -- so this deterministically diverges from the uncalled gunfight path (A/B: eyes
# vs an identical caim:8/UNCALLED run differ by ~83 lines, verified). Pins that the aimed
# path is reachable and deterministic; the dude still loses to the swarm (encounter
# balance, like gunfight), so this pins the PATH, not a win. NOTE: combat intents are
# ONE-SHOT (combat_drain.cc) -- a queued aimed shot that lands on a turn without enough AP
# for its +1 cost is DISCARDED, not held to retry next turn, so not all 5 caim shots fire
# (the queue is a probe batch, not a reactive client; the wire client re-sends per click).
run_case arvillag_aimshot  arvillag.map  42    4000   ""     "290:give:241,295:reload:241,300:wield:1,485:caim:6,488:caim:6,491:caim:6,494:caim:6,497:caim:6,500:aggro:3" > "$RESULTS/23.log" 2>&1 &
# Push a critter one hex (actionPush) — and the FIRST golden to dispatch a REAL compiled
# .int script proc. actionCheckPush requires the target to carry a push_p_proc, so the
# push:0 verb finds the nearest Den resident with one (id=166, script_idx=35) and shoves
# it. The rotate+one-hex displacement completes headless via the InstantAnimationScheduler
# (the SAME reg_anim path as walkto — NO serverLoopActive() fast-path is needed; this
# cluster was never callback-welded, unlike the doors/pickup/skill decouples). A/B vs a
# no-push baseline: the target moves tile 13687->13686 and rotates, and that obj line is
# the ONLY difference in the whole dump (the push_p_proc drew no RNG / added no time), so
# there is no cascade. Deterministic. Pins actionPush + first real script-proc dispatch.
run_case denbus1_push      denbus1.map   42    340    ""     "300:push:0" > "$RESULTS/24.log" 2>&1 &
# Headless DIALOG driver (the first MODAL-loop subsystem made server-drivable).
# The blocking _gdProcess loop is nested synchronously inside a critter's
# SCRIPT_PROC_TALK; under the server loop it drains a dialog_intent queue and
# calls _gdProcessChoice directly instead of reading the keyboard (window/render
# skipped behind serverLoopActive() in _gdCreateHeadWindow/_gdProcessInit/
# _gdProcessUpdate). dtalk:940 requests dialog with the Den Story Teller
# ("Leanne", the unique scriptIndex=940 critter); the dsay verbs walk her
# reply/option tree: greeting -> "how's Becky's family" node -> a story node,
# then dend ends the conversation. Effects (A/B vs a dtalk-less baseline, fully
# deterministic and isolated — game_time/rng_state are BYTE-IDENTICAL to the no-
# dialog run, the conversation draws no RNG and advances no time): gvar 41
# (GVAR_KARMA_WANDERER) set by the Den karma-title preamble every talk_p_proc
# runs on open, and the Story Teller's script locals allocated + set by the talk
# proc and the option procs (lvars_len grows, lvar 719 comes from the story
# branch selection) -- proving _gdProcessChoice actually executed the option
# procedures across multiple nodes, not just opened the window. v1 scope is
# reply/option only; if an option's proc switched into barter the driver aborts
# (_dialogue_switch_mode != 0), but this tree never does.
run_case denbus1_dialog    denbus1.map   42    500    ""     "286:dsay:0,288:dsay:1,290:dsay:0,292:dend,300:dtalk:940" > "$RESULTS/25.log" 2>&1 &
# Headless ENDGAME (the terminal slideshow+movie presentation path). endgame:0
# requests SCRIPT_REQUEST_ENDGAME; scriptsHandleRequests -- run every serverTick
# -- drains it into endgamePlaySlideshow() + endgamePlayMovie(), both now guarded
# to early-return under serverLoopActive(). Two things this pins:
#  (1) NO-HANG: without the guards the run hangs indefinitely (creditsOpen's
#      scroll loop + the UINT_MAX static-scene wait for a speech-end callback that
#      never fires headless) and the case times out -> FAIL. A/B-verified: guards
#      disabled -> 25s timeout; guarded -> completes in <1s.
#  (2) ENDGAME-FIRED: the slideshow/movie functions themselves mutate NO sim state
#      (A/B-verified: with the movie's _game_user_wants_to_quit=2 write suppressed,
#      this dump is BYTE-IDENTICAL to a plain denbus1 idle run). The movie's sole
#      effect is setting the terminal quit flag; that flag is not dumped directly,
#      but animation.cc short-circuits ambient motion on it, so the run
#      deterministically diverges from idle (ambient critters at different tiles,
#      shifted rng_state) -- that divergence is the observable signature that the
#      endgame actually executed, not merely that the verb was dispatched.
# NOTE: serverRun does NOT honor the terminal quit (it ticks the full count), so
# ambient sim continues past the quit point. Making the server loop break on
# _game_user_wants_to_quit is a separate fidelity improvement (it would also make
# the combat-death goldens stop at death) -- deferred, needs its own re-bless.
run_case denbus1_endgame   denbus1.map   42    500    ""     "300:endgame:0" > "$RESULTS/26.log" 2>&1 &
# Headless BARTER (the second nested MODAL loop, inventoryOpenTrade, driven inside
# a merchant conversation). dtalk:47 opens dialog with the Den store owner Tubby
# (scriptIndex 47); dsay:0 picks the "let's trade" greeting option whose proc runs
# the gdialog_barter opcode -> _dialogue_switch_mode = 2. The server _gdProcess
# branch (which must test the switch-mode BEFORE the _gdProcessChoice == -1 end
# check, since the barter option registers no new dialog options) drives
# create-win -> inventoryOpenTrade -> cleanup -> destroy-win INLINE, bypassing the
# SDL-window-building gameDialogTicker. The three hidden barter table objects are
# allocated/freed by the serverLoopActive()-guarded _gdialog_barter_create/destroy_win
# (window+buttons skipped); inventoryOpenTrade's own UI setup is skipped (it calls
# exit(1) headless because the barter-back window is -1) and its blocking for(;;)
# drains a barter_intent queue instead of reading mouse/keyboard.
# Fixture: give the dude 300 caps (give:41:300), then queue barter intents:
# btake:259:1 (put 1 of Tubby's pid-259 goods on the barterer table), boffer:41:200
# (offer 200 caps), bcommit:0 (the 'M'/Offer button -> barterAttemptTransaction),
# bdone:0 (the 'T'/Talk button -> return tables + _barter_end_to_talk_to).
# Effects, deterministic (identical x2) and conserving (total map caps and pid-259
# counts unchanged, object count == a no-barter baseline -> the 3 hidden tables are
# destroyed before the dump, not leaked): the dude pays 200 caps (300 -> 100) and
# receives 1x pid-259; the proceeds route to Tubby's STORE BOX (id=22, scriptIndex
# 169 = DITubBox: 152 -> 352 caps) -- authentic Fallout store behavior, the barter
# partner is the linked shop container, not the critter. The offer (200) sits well
# above the merchant's demand (< 100 for 1 item at the +25% bad-reaction markup), a
# wide margin so barterComputeValue's documented double-vs-float +-1 rounding
# (item.cc:3735) cannot flip BARTER_RESULT_OK <-> BAD_OFFER and destabilize the
# golden. No RNG is drawn in the barter path (rng_state shifts only from post-barter
# ambient sim over the trailing ticks).
run_case denbus1_barter    denbus1.map   42    340    ""     "100:give:41:300,286:dsay:0,288:btake:259:1,290:boffer:41:200,292:bcommit:0,294:bdone:0,300:dtalk:47" > "$RESULTS/27.log" 2>&1 &
wait

cat "$RESULTS"/*.log
grep -q "^FAIL" "$RESULTS"/*.log && exit 1
exit 0
