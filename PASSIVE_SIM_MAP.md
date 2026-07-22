# PASSIVE_SIM_MAP.md — autonomous / passive-sim → engine coverage map

**Purpose.** This is **surface 3 of 3** of the headless-decoupling coverage set.
Sibling maps cover the two *externally-triggered* surfaces:

- `SCRIPT_OPCODE_MAP.md` (surface 1) — scripts → engine, all 181 game opcodes.
- `PLAYER_ACTION_MAP.md` (surface 2) — player UI input → engine.

This document covers the third surface: the **autonomous / passive simulation
that runs on its own with NO player key/mouse input and NO script opcode
call** — the *time-driven and queue-driven* state changes the engine performs
by itself as game time advances. The load-bearing question for the headless
server is the same as the other maps: does the fixed-timestep server loop
(`F2_SERVER_LOOP=1`, `src/server_loop.cc`, see `SERVER_LOOP_DESIGN.md`) already
drive each passive system correctly, or is it a gap / a cadence hazard.

Passive sim is almost entirely one mechanism: **the event queue** (`src/queue.cc`),
pumped by the game-clock ticker `_script_chk_timed_events` (`src/scripts.cc`),
which is itself run from `_process_bk()` — the exact function the server loop
calls each tick. So the headline is favourable: the server tick *already* drives
the whole queue at a deterministic cadence. The open items are (a) two bulk
**time-jump drivers** (rest, worldmap travel) that live *outside* `serverTick`
in probe/UI code, and (b) the **ANIM-WELDED + RNG-cadence** ambient-motion path
(`_dude_fidget`), which is the same coupling family that caused the earlier
instant-animation divergences.

## Column definitions & headless-class schema (identical to `SCRIPT_OPCODE_MAP.md`)

- **Headless-class** (the load-bearing column):
  - `PURE-SIM` — mutates sim state directly, no animation/UI/blocking dependency → headless-safe.
  - `ANIM-WELDED` — outcome welded to a `reg_anim` completion callback that never fires headless → needs the `serverLoopActive()` / instant-scheduler fast-path.
  - `MODAL` — opens a blocking input loop headless.
  - `PURE-UI` — presentation only (float text, console msg, sfx, gfx) → no-op headless.
  - `TIME-RNG` — advances game time / draws RNG / (re)schedules queue events → works, but perturbs determinism cadence.
  - Secondary tags noted where relevant (e.g. `PURE-SIM+TIME-RNG`).
- **Server-loop status** (the key column): `DRIVEN` = the existing `serverTick`
  already ticks this correctly headless · `GAP` = correct behaviour depends on
  code outside `serverTick` (a probe/UI driver) or on an un-landed fast-path ·
  `N/A`.
- **Golden** — `COVERED` (a server golden drives this exact path), `INDIRECT`
  (a golden exercises the substrate but not this handler's own trigger/outcome),
  `NONE`. `?` flags a low-confidence row.

### How the server loop drives passive sim (the spine)

`serverTick()` (`src/server_loop.cc:23`) each tick: `intentsDrain` → `simClockAdvance(100)` → `_process_bk()` → `scriptsHandleRequests()` → `mapHandleTransition()`.

- `_process_bk()` (`src/input.cc:189`) → `tickersExecute()` (`src/input.cc:277`),
  which under `serverLoopActive()` stamps `gTickerLastTimestamp = simClockNow()`
  instead of `getTicks()` (`src/input.cc:288`) — this is the ONE seam that makes
  every downstream `_get_bk_time()` consumer run on sim time.
- The sim tickers are `_doBkProcesses` (`src/scripts.cc:675`), `_object_animate`
  and `_dude_fidget` (registered `src/map.cc:243-244`).
- `_doBkProcesses` → `_script_chk_timed_events` (`src/scripts.cc:749`) is the
  passive-sim heart: every `>=100` bk-ms it does **`gGameTime += 1`** (out of
  combat, `scripts.cc:770`) then **drains the queue** while
  `gameTimeGetTime() >= queueGetNextEventTime()` via `queueProcessEvents()`
  (`scripts.cc:776-782`). At `kServerTickDelta = 100` this fires **every tick**,
  so game time advances +1/tick and the queue is serviced every tick. It also
  runs `SCRIPT_PROC_MAP_UPDATE` light scripts every 30000 bk-ms (`scripts.cc:759`)
  and round-robins one `critter_p_proc` per pump (`_script_chk_critters`,
  `scripts.cc:706`).

---

## 1. Master table — the event queue (`src/queue.cc`)

The queue has **14 event types** (`EventType` enum, `src/queue.h:9-25`,
`EVENT_TYPE_COUNT = 14`); the handler/free/read/write table is
`gEventTypeDescriptions[]` (`src/queue.cc:54-69`). Each row is
`{handler, free, read, write, clear-on-leave-map, leave-map-fn}`. All are
enqueued via `queueAddEvent(delay, owner, data, type)` (`queue.cc:239`; `time =
gameTimeGetTime() + delay`) and processed by `queueProcessEvents()`
(`queue.cc:345`) — which the server tick drives (see spine above).

| # | EVENT_TYPE | Handler (@file:line) | Scheduled by (@file:line) | Mutates | Headless-class | Server-loop status | Golden |
|---|---|---|---|---|---|---|---|
| 0 | `DRUG` | `drugEffectEventProcess` (item.cc:2869) | `_insert_drug_effect` (item.cc:2635) ← `_item_d_take_drug`/`itemUseDrug`; delay `600*duration` | Wears off drug: re-applies `_perform_drug_effect` to reverse bonus stats; may kill | PURE-SIM (+TIME-RNG: `randomBetween` in perform for min/max stat rolls) | DRIVEN | COVERED (arvillag_actions `drug:40`) |
| 1 | `KNOCKOUT` | `knockoutEventProcess` (critter.cc:1248) | `combat.cc:4752` on DAM_KNOCKED_OUT; delay `10*(35-3*END)` | Clears KO/KD → sets KNOCKED_DOWN; in combat sets ENGAGING maneuver, else `_dude_standup` | PURE-SIM (standup = `objectSetFid`, no reg_anim) | DRIVEN | INDIRECT (combat goldens can KO) |
| 2 | `WITHDRAWAL` | `withdrawalEventProcess` (item.cc:2982) | `_insert_withdrawal` (item.cc:2933) ← drug addiction roll (item.cc:2844) / `_item_wd_clear_all`; delay `600*duration` | Onset/end of addiction: add/remove withdrawal perk, clear addiction gvar | PURE-SIM | DRIVEN | INDIRECT (arvillag_actions drug path can roll addiction) |
| 3 | `SCRIPT` | `scriptEventProcess` (scripts.cc:860) | `scriptAddTimerEvent` (scripts.cc:817) ← opcode `op_add_timer_event` 0x80F0 (interp_extra.cc:2510) | Runs `SCRIPT_PROC_TIMED` on the owning script → arbitrary sim effects | PURE-SIM (effect = whatever the timed proc does) | DRIVEN | NONE (no golden schedules a script timer) |
| 4 | `GAME_TIME` | `gameTimeEventProcess` (scripts.cc:406) | `gameTimeScheduleUpdateEvent` (scripts.cc:389) at map load / self-reschedule; fires at next **midnight** | `_scriptsCheckGameEvents` (endgame movies, Arroyo timers, town rep), `_critter_check_rads(gDude)`, reschedules itself | TIME-RNG (`statRoll` ENDURANCE for rad level) | DRIVEN | NONE (goldens run ≤4000 ticks → midnight not reached) |
| 5 | `POISON` | `poisonEventProcess` (critter.cc:379) | `critterAdjustPoison` (critter.cc:352); delay `10*(505-5*poison)` | dude only: poison −2, HP −1; returns 1 (stop) when HP≤5 | PURE-SIM | DRIVEN | COVERED (arvillag_actions `poison:45`) |
| 6 | `RADIATION` | `radiationEventProcess` (critter.cc:628) | `_critter_check_rads` (critter.cc:534, delay `HOUR*rand(4,18)`) + 7-day heal self-reschedule (critter.cc:638) | `_process_rads`: applies/removes rad bonus-stat penalties; may `critterKill` | PURE-SIM | DRIVEN | INDIRECT (arvillag_actions `rad:150` sets radiation state; the damage handler fires hours later, past the golden window) |
| 7 | `FLARE` | `flareEventProcess` (queue.cc:439) | `proto_instance.cc:850` on light-flare; delay `72000` (2h) | `_obj_destroy(flare)` (burn-out) | PURE-SIM | DRIVEN | NONE |
| 8 | `EXPLOSION` | `explosionEventProcess` → `_queue_do_explosion_` (queue.cc:446/458) | `_obj_use_explosive` (proto_instance.cc:928) on timer set | `actionExplode` (area damage, may enter combat), `_obj_destroy`; re-queues +50 if busy | ANIM-WELDED (`actionExplode` animate=true → reg_anim; re-queue loop) | DRIVEN? (needs instant-scheduler/`serverLoopActive` audit for the animate path) | NONE |
| 9 | `ITEM_TRICKLE` | `miscItemTrickleEventProcess` (item.cc:2303) | `miscItemTurnOn` (item.cc:2382/2390) for stealth-boy/geiger; self-reschedule (item.cc:2314), delay 600/3000 | Consumes 1 charge; turns item off when depleted | PURE-SIM | DRIVEN | NONE |
| 10 | `SNEAK` | `sneakEventProcess` (critter.cc:1196) | self-reschedule (critter.cc:1222) once dude enables sneak state | `skillRoll(SNEAK)` → sets `_sneak_working`, reschedules (100–600) | TIME-RNG (per-cycle skill roll) | DRIVEN | NONE |
| 11 | `EXPLOSION_FAILURE` | `explosionFailureEventProcess` (queue.cc:497) | `_obj_use_explosive` (proto_instance.cc:928) on failed TRAPS roll | Console msg + `_queue_do_explosion_` (premature detonation) | ANIM-WELDED (same as EXPLOSION) | DRIVEN? (same audit) | NONE |
| 12 | `MAP_UPDATE_EVENT` | `mapUpdateEventProcess` (scripts.cc:508) | `gameTimeScheduleUpdateEvent` (scripts.cc:397) + self-reschedule every 600 (scripts.cc:518) | Runs `SCRIPT_PROC_MAP_UPDATE` on all map scripts | PURE-SIM | DRIVEN | COVERED (idle goldens run >600 ticks → fires) |
| 13 | `GSOUND_SFX_EVENT` | `ambientSoundEffectEventProcess` (game_sound.cc:2102) | `_gsound_sfx_q_start` at map load (map.cc:836) + self-reschedule (game_sound.cc:2128), delay `10*rand(15,20)` | Rolls next ambient sfx index (RNG), plays sound; reschedules | TIME-RNG (draws RNG each cycle; sound itself PURE-UI) | DRIVEN | INDIRECT (idle goldens; perturbs RNG cadence even with no audio) |

**Not a queue event (clarification):** *door auto-close* is **not** a passive
queue event in F2CE — doors open/close only via script/action/player use
(`_obj_use_door`, `objectOpenClose`, proto_instance.cc; already decoupled per
`SCRIPT_OPCODE_MAP.md`). *Super-stimpak delayed damage* is not a distinct event
type either — it is scheduled as an ordinary `EVENT_TYPE_DRUG` with a negative
HP modifier via `_insert_drug_effect` (row 0). No separate handler exists.

`_queue_leaving_map()` (queue.cc:511) clears event types whose `field_10` flag
is set (all but POISON and RADIATION) on map exit — a passive teardown, driven
by `mapHandleTransition` in the server tick.

---

## 2. Master table — per-tick systems (run by `_process_bk`, not the queue)

These are driven every server tick through the ticker set, gated on
`_get_bk_time()` (= sim clock under `serverLoopActive()`).

| System | Trigger (@file:line) | Mutates | Headless-class | Server-loop status | Golden |
|---|---|---|---|---|---|
| **Game clock advance** | `_script_chk_timed_events` (scripts.cc:770), every ≥100 bk-ms, out of combat | `gGameTime += 1` | TIME | DRIVEN (100ms/tick → +1/tick) | COVERED (every golden; `game_time` in dump) |
| **Queue drain** | `_script_chk_timed_events` (scripts.cc:776-782) | Pops all due events → their handlers | PURE-SIM/TIME-RNG | DRIVEN | COVERED (idle/actions/rest) |
| **Map-update light scripts** | `_script_chk_timed_events` (scripts.cc:759), every 30000 bk-ms (300 ticks) | `scriptsExecMapUpdateScripts(SCRIPT_PROC_MAP_UPDATE)` | PURE-SIM | DRIVEN | INDIRECT (idle) |
| **Ambient critter scripts** (`critter_p_proc`) | `_script_chk_critters` (scripts.cc:706), one critter/round-robin per pump, out of combat/dialog | Arbitrary per-critter script effects | PURE-SIM | DRIVEN | INDIRECT (idle) |
| **`_object_animate`** (scheduled object anims) | ticker (map.cc:243) | Advances any registered object animation to next frame/completion | ANIM-WELDED | DRIVEN (InstantAnimationScheduler drains per pump) | INDIRECT (idle) |
| **`_dude_fidget`** (AMBIENT MOTION) | ticker (map.cc:244 / combat.cc:2727); gated `getTicksBetween(now,lastTime) > nextTime` (animation.cc:2566), out of combat | Picks a random on-screen standing critter (`randomBetween`, animation.cc:2594), runs a fidget `reg_anim`; recomputes `nextTime` | **ANIM-WELDED + TIME-RNG** | DRIVEN (instant scheduler completes it) — **but the getTicks-gated `nextTime` cadence is the exact coupling that caused the earlier instant-animation divergences; the sim-clock feed re-times it, hence the mandatory idle re-bless** | COVERED (arvillag_idle, artemple_idle) |

---

## 3. Master table — bulk time-jump drivers (advance game time in a chunk)

These are the systems that do **not** advance one tick at a time; they push
`gameTimeAddTicks(...)` in bulk and then call `queueProcessEvents()` directly to
service everything that came due during the jump. **They are the real passive-sim
gaps: none of them lives inside `serverTick` — each is a probe-side or UI-side
driver.** A real headless server must grow server-side equivalents.

| System | Trigger / driver (@file:line) | Mutates | Headless-class | Server-loop status | Golden |
|---|---|---|---|---|---|
| **Rest / natural healing** | `restSimMinutesTick`/`restSimHoursTick`/`restSimOverdueEvents` (party_member.cc:906-991) + `restHealCheck`/`restHealApply`/`_partyMemberRestingHeal` (party_member.cc:829-880). Driven headless by **`probeRestRun` in main.cc:445** (mirrors `pipboyRest`, pipboy.cc:1953). | Advances `gGameTime` per frame, `queueProcessEvents()` each step, heals party by `STAT_HEALING_RATE` per 180 rest-min | PURE-SIM | **GAP** — driven by the probe rest driver, not `serverTick`; `SERVER_LOOP_DESIGN.md §6` flags rest as a frame-driven sub-loop needing the driver-seam treatment | COVERED via probe driver (arvillag_rest, arvillag_restfight) |
| **Worldmap travel** (time passage + walk-heal) | `worldmap.cc:3508` (`gameTimeAddTicks`+`queueProcessEvents`); walk-heal `_partyMemberRestingHeal(3)` per ~1000ms walked (worldmap.cc:2565). Driven headless by the **`wmtravel` probe intent (main.cc)**. | Advances game time, drains queue, heals party, may trigger encounter | PURE-SIM (+TIME-RNG encounters) | **GAP** — probe `wmtravel` uses `getTicks()` as its per-step counter and still needs `F2_FAKE_CLOCK=1`; `SERVER_LOOP_DESIGN.md §1/§6` requires replacing it with a real server worldmap driver before deleting server `F2_FAKE_CLOCK` (S3) | COVERED via probe driver (arvillag_wmtravel) |
| **`op_game_time_advance`** (script-driven jump) | `interpreter_extra.cc:2770-2775` (`gameTimeAddTicks(DAY)` loop + `queueProcessEvents`) | Advances game time in day chunks, drains queue | PURE-SIM+TIME-RNG | DRIVEN (opcode runs inside a script during a tick; see `SCRIPT_OPCODE_MAP.md`) — listed here for completeness as the script-triggered sibling of rest/worldmap | INDIRECT |

---

## 4. Remaining passive-sim work (the deduped GAP / hazard list)

Everything the **queue** carries (rows 0–13) plus the per-tick systems (§2) is
**already correctly driven by `serverTick`** — this is the large, done part.
What remains:

1. **Rest / natural-healing driver — GAP.** The heal + time-jump logic is
   extracted into core (`party_member.cc restSim*/restHeal*`), but the *driver*
   that steps it headlessly is `probeRestRun` in `main.cc`, not the server loop.
   A live server needs a server-side rest driver (or to fold rest into an intent
   that `serverTick` advances). Same driver-seam family as combat_ui was.
2. **Worldmap-travel driver — GAP + `F2_FAKE_CLOCK` dependency.** The `wmtravel`
   probe intent drives `worldmap.cc` time-advance/heal using `getTicks()` and
   still requires `F2_FAKE_CLOCK=1`. Per `SERVER_LOOP_DESIGN.md §1`, deleting
   server `F2_FAKE_CLOCK` (cleanup step S3) is **blocked** on writing a real
   server worldmap driver first.
3. **`_dude_fidget` ambient-motion cadence — HAZARD (ANIM-WELDED + TIME-RNG).**
   Functionally DRIVEN (instant scheduler completes the anim), but its
   `getTicks`-gated `nextTime` throttle and per-fire `randomBetween` draw make it
   the RNG-cadence-sensitive path that produced the earlier instant-animation
   divergences (`SERVER_LOOP_DESIGN.md §5 S0 / §6`). The sim ticker set must stay
   frozen; any change to fidget cadence is a knowing golden re-bless, not a bug.
4. **`GSOUND_SFX_EVENT` RNG cadence — HAZARD (TIME-RNG).** Drives no audio
   headless but still rolls `randomBetween` / `wmSfxRollNextIdx` each cycle,
   perturbing the shared RNG stream; started at every map load. Benign but must
   be held constant across re-blesses.
5. **`EXPLOSION` / `EXPLOSION_FAILURE` animate path — VERIFY.** Handlers call
   `actionExplode(..., animate=true)` (reg_anim) and re-queue when busy. Needs
   the same `serverLoopActive()`/instant-scheduler audit the other ANIM-WELDED
   paths got; **no golden exercises it**, so it is unproven headless. Marked `?`.
6. **Uncovered-but-DRIVEN queue events (no golden, low risk).** `FLARE`,
   `ITEM_TRICKLE`, `SNEAK`, `SCRIPT`-timer, and the `GAME_TIME` midnight handler
   are all PURE-SIM/TIME-RNG and driven by the tick, but **no server golden
   reaches them** (goldens run ≤4000 ticks; midnight, 2h flare, sneak-enable,
   script timers are never scheduled in the fixtures). Coverage gap, not a
   behaviour gap — candidates for new goldens if these paths need pinning.

---

## 5. Summary counts

**Queue event types: 14** (`EVENT_TYPE_COUNT`). By headless-class:

- `PURE-SIM`: 5 — DRUG(+RNG), KNOCKOUT, WITHDRAWAL, POISON, ITEM_TRICKLE, plus SCRIPT (effect-dependent) → effectively 6.
- `TIME-RNG` (schedules/reschedules or draws RNG): GAME_TIME, SNEAK, GSOUND_SFX (+ DRUG/RADIATION secondary).
- `PURE-SIM` with scheduling: RADIATION, MAP_UPDATE_EVENT, FLARE.
- `ANIM-WELDED`: 2 — EXPLOSION, EXPLOSION_FAILURE (verify).

**Per-tick systems: 6** — game-clock advance, queue drain, map-update light
scripts, ambient critter scripts, `_object_animate`, `_dude_fidget`. All DRIVEN;
`_dude_fidget` + `_object_animate` are ANIM-WELDED (drained by the instant
scheduler), `_dude_fidget` additionally RNG-cadence-sensitive.

**Bulk time-jump drivers: 3** — rest (GAP), worldmap travel (GAP), op_game_time_advance (DRIVEN, script-side).

**Server-loop status tally (passive systems, 14 queue + 6 tick + 3 jump = 23):**

- **DRIVEN correctly headless by `serverTick`: 20/23** — all 14 queue events
  (2 explosion rows pending an animate-path audit) and all 6 per-tick systems,
  plus op_game_time_advance.
- **GAP (needs work outside the current server loop): 2** — the rest driver and
  the worldmap-travel driver (both currently probe-side; worldmap also gates the
  `F2_FAKE_CLOCK` deletion).
- **HAZARD (DRIVEN but cadence/RNG-sensitive, freeze-and-re-bless): 3** —
  `_dude_fidget`, `GSOUND_SFX_EVENT`, and the explosion animate path (also a
  verify item).

**Golden coverage:** COVERED = DRUG, POISON, MAP_UPDATE, game-clock, fidget,
rest, worldmap (7). INDIRECT = KNOCKOUT, WITHDRAWAL, RADIATION, GSOUND_SFX,
object-anim, map-update-scripts, critter scripts (7). NONE = SCRIPT-timer,
GAME_TIME-midnight, FLARE, EXPLOSION, EXPLOSION_FAILURE, ITEM_TRICKLE, SNEAK (7).

**Bottom line.** The passive/autonomous sim is overwhelmingly the event queue,
and the fixed-timestep server loop *already drives it correctly headless* by
reusing `_process_bk` → `_script_chk_timed_events` (game clock +1/tick and
per-tick queue drain). The only true open work is the **two bulk time-jump
drivers (rest, worldmap travel)** that still live in probe/UI code rather than in
`serverTick`, plus **holding the RNG-cadence-sensitive ambient paths
(`_dude_fidget`, ambient sfx) and the explosion animate path frozen/verified**.
