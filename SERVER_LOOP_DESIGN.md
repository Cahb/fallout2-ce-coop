# Headless Server Tick Loop — Design Sketch

Design for the fixed-timestep server/headless simulation loop that decouples the
sim from animation/render/frame timing — the keystone that unblocks instant
animation (Phase 2.3-live / 2.4) and underpins the Phase 5 server. Produced as a
design pass (not yet implemented). Context: the instant-animation retrofit into
the current frame-driven headless loop failed on four timing couplings (see
REWRITE_PLAN.md Phase 2 status); this loop dissolves them by construction.

## 1. Loop structure
- New `src/server_loop.cc` + `src/sim_clock.cc` (f2_core, SDL-free).
- **Sim clock = tick count.** `gSimNow` advanced by fixed `kServerTickDelta = 100`
  bk-ms per tick. Fed in at the ONE existing bridge: `tickersExecute()`
  (`input.cc`) changes `gTickerLastTimestamp = getTicks()` →
  `= simClockInstalled() ? simClockNow() : getTicks()`. Everything downstream
  (`_get_bk_time`, `_doBkProcesses`, `_script_chk_timed_events`, `_dude_fidget`,
  queue, script timers) is untouched. 100ms/tick → the `>=100` game-clock gate
  fires every tick (gGameTime +1/tick out of combat); map-update every 300 ticks.
  `F2_FAKE_CLOCK` becomes irrelevant to the *core* sim time base — BUT it stays
  load-bearing for one probe-side driver: the `wmtravel` intent handler
  (main.cc) uses `getTicks()` as a per-step monotonic counter for its rest-heal
  cadence, and the sim clock does not advance inside that inner loop. So the
  server golden path keeps `F2_FAKE_CLOCK=1`; the S3 "delete server
  F2_FAKE_CLOCK" step must first replace the probe wmtravel loop with a real
  server worldmap driver (empirically confirmed 2026-07-12 by the equivalence
  review).
- **One tick, in order:** (1) `intentsDrain(tick)` — apply scheduled intents
  (successor of F2_PROBE_ACTIONS + input replay; call core entry points directly,
  no key/mouse events); (2) `simClockAdvance(100)`; (3) `_process_bk()` reused
  as-is (tickersExecute stamps the sim clock, runs `_doBkProcesses`,
  `_object_animate` w/ instant scheduler draining to completion in registered
  order, `_dude_fidget`); (4) `scriptsHandleRequests()` (combat entry, §3); (5)
  `mapHandleTransition()`. Reusing `_process_bk`/tickers (vs an explicit call
  list) preserves the exact intra-pump order the RNG stream depends on — explicit
  ordering is a later cleanup, not a prerequisite.

## 2. Exclusion strategy — the 4 couplings dissolved
1. **Sim clock ↔ getTicks call-count:** gone by construction (clock fed from
   `gSimNow`, fixed delta/tick; getTicks calls no longer perturb sim time).
2. **UI render loops spin on getTicks:** install **NullPresenter** — the deadlock
   loops (`interfaceBarEndButtonsShow/Hide`) sit behind `hudEndButtonsShow/Hide`
   presenter methods and simply never execute headless.
3. **Replay keyed on pump count (`gIteration`):** input replay retired for the
   server path, replaced by the tick-stamped intent queue — no per-pump injection.
4. **Nested combat loop on timed input:** server combat driver (§3) — no fps
   limiter / renderPresent / timed input; instant scheduler collapses the
   `_combat_turn_running` waits to one pump.

Additional exclusions required beyond the two seams:
- **Movies:** `gameMoviePlay()` needs a headless no-op (still set the "seen" flags
  / run sim side effects) so `opPlayGameMovie` from scripts can't hang. Best as a
  `Presenter::moviePlay()` (Null no-ops post-bookkeeping); a `headless` early-return
  is an acceptable v1.
- **Ticker registration audit:** client-only tickers (`gameMouseRefresh`,
  `colorCycleTicker`, `_gsound_bkg_proc`, `textObjectsTicker`, `wmMouseBkProc`,
  dialog tickers) must not register in the server build. The sim ticker set is
  `_doBkProcesses` + `_object_animate` + `_dude_fidget`.
- fps limiter / renderPresent: never referenced by the server loop (client-only).
- Script-opened modal UIs (dialog/loot/elevator opcodes): out of v1-golden scope
  but each is a future hang like movies — needs a per-opcode no-op/intent pass
  before real game content runs on the server.

## 3. Combat — survives nested, minimal restructure
- `_combat_turn_run` with the instant scheduler: first `_process_bk` drains all
  sads → completion callbacks drop `_combat_turn_running` to 0 → loop exits in ~1
  iteration; damage-on-completion ordering preserved by the scheduler contract.
- `_combat_input`/`_combat_turn_run` are already a link seam (core calls them by
  name through `combat_ui.h`). Provide **server implementations**
  (`src/server_combat_driver.cc`): server `_combat_turn_run` = pump w/o
  render/throttle; server `_combat_input` = consume pending dude-turn intents
  (`attack`, `moveto`, `endturn`) then break via the extracted authorities
  `combatPlayerTurnShouldBreak/OutOfAp/Resolve`. **Default with no intent = end
  turn** (deterministic; replaces the SPACE-key traces entirely).
- Gate-OFF (default) a full encounter resolves within the tick that enters it;
  world-time-frozen-in-combat matches original semantics.
- **RESUMABLE SESSION MACHINE (P2, `F2_SERVER_RESUMABLE_COMBAT=1`, default OFF —
  no longer deferred).** With the gate on (server loop only), `_combat()` sets up
  a file-static session in combat.cc and RETURNS; `serverTick` calls
  `combatSessionActive()`/`combatSessionAdvance()` once per beat (after
  `scriptsHandleRequests`, before `mapHandleTransition`) so a fight spans beats:
  - States: kRoundBegin → kTurns → (kPlayerTurn) → kRoundEnd → … → kEnding.
    kRoundBegin/kRoundEnd are bookkeeping and fall through; a turn consumes the beat.
  - **One AI turn per beat** (pacing rule): kTurns runs one `_combat_turn(obj,false)`
    synchronously (drains via the instant scheduler) then yields.
  - **Player block barrier** (kPlayerTurn): the dude-turn is split from the
    monolithic `_combat_turn` into BEGIN (once) / per-beat PUMP (reuses the shared
    `combatServerPumpIntents`, the extracted `_combat_input` drain body) / END
    (`combatPlayerTurnResolve` + `_combat_turn_run` + epilogue + dude-dead/elevation
    checks). Player actors are a LIST (v1 = {gDude}) per REWRITE_PLAN open-q #2 —
    never "exactly one player turn".
  - **Wait policy**: the barrier waits only when there is someone to wait for — a
    wire claimant (`serverClaimantConnected()`), pending intents, or
    `F2_SERVER_TURN_WAIT=1`. Otherwise the turn ends immediately (= legacy
    auto-end), so a client-less server never stalls.
  - **Idle timer**: per committed action, sim-ms; budget `F2_SERVER_TURN_IDLE_MS`
    (default 30000). Any consumed intent rearms it; expiry ends the turn. The
    budget is stamped into `turnStart.deadlineMs` for player turns (AI turns and
    gate-off keep 0). Sim clock now advances during a fight, so timed events tick
    per beat mid-combat — vanilla-client-equivalent, no code change.
  - `_gcsd` is anchored to a BY-VALUE copy of `*csd` in the session (OPEN RISK a:
    the caller's CSD is a stack/soon-freed buffer — scripts.cc memsets gScriptsCSD
    the instant `_combat` returns).
  - Gate OFF is byte-identical (the whole golden suite + gate 9 the resumable proof).

## 4. Harness / goldens
- `serverRun()` replaces `mainHeadlessProbe`'s inner pump loop (main.cc ~578); map
  boot / seed / aggro fixture / state_dump bracketing stay.
- **Probe actions map 1:1**: `F2_PROBE_ACTIONS="tick:action:arg"` is already
  tick-indexed — it IS the intent queue v0; fold in unchanged, add combat intents.
- SPACE input-replay traces become obsolete (auto-end-turn). `F2_INPUT_REPLAY`
  stays only for the legacy path during transition.
- **Re-bless all 14** (clock/RNG cadence change → byte-identical impossible).
  Validate semantically: run old-probe vs server-loop side by side, field-by-field
  diff, require invariants (deaths, XP, inventory, transitions) to match, review
  every legitimately-shifted field (gGameTime, queue timestamps, RNG-dep HP rolls).

## 5. Incremental migration
- **S0 (attempted 2026-07-12 — does NOT stand alone; reverted):** installing
  NullPresenter in the headless probe made **12/14 goldens byte-identical** (seam
  is one-way for those — good), BUT the 2 combat cases (klatoxcv_combat, restfight)
  both diverged AND became pathologically slow (>200s). Root cause: the
  ClientPresenter's HUD end-button loop (`interfaceBarEndButtonsShow`) was
  inadvertently *advancing* `getTicks()` during `_combat_begin`, and combat
  animation pacing is gated on `getTicks()` — remove that consumer (NullPresenter)
  and animations need a huge number of pumps to advance → combat crawls. So
  NullPresenter cannot land alone: it MUST be paired with the InstantAnimation
  Scheduler (which completes animations regardless of getTicks) — i.e. S0+S1+S2 are
  one interlocked change, not an incremental sequence. LESSON: build the server
  loop as a coherent whole (sim clock + instant scheduler + NullPresenter + movie
  guard together), in a fresh dedicated session; the "one small replay-identical
  step first" idea is empirically false because the couplings are mutually load-
  bearing. The 12/14 result still confirms the presenter seam is otherwise
  sim-neutral.
- **S1 (re-bless #1, non-combat):** add `sim_clock.cc` + the one-line tickersExecute
  feed change + `serverTick()` (steps 1–5, still real-time scheduler) behind
  `F2_SERVER_LOOP=1`; keep legacy loop. Re-bless non-combat goldens (semantic diff).
- **S2 (re-bless #2, combat):** install InstantAnimationScheduler + server-side
  `_combat_input`/`_combat_turn_run` + auto-end-turn; convert/delete SPACE traces.
- **S3 (cleanup):** delete legacy pump path, server `F2_FAKE_CLOCK`, input-replay
  dep; strip client tickers from server registration; maybe explicit ordered calls.

## 6. Top risks / open questions
- **Presenter-seam completeness** (the S0 gate): any presenter method with a sim
  side effect diverges NullPresenter goldens — S0 surfaces it cheaply.
- **RNG stream stability**: fidget/wander draw RNG per tick; freeze the "sim ticker
  set" once (S1); every later change is a knowing re-bless.
- **`_combat_turn_running` save/load** invariant (serialized) — never nonzero mid-drain.
- **Worldmap travel / rest** probe paths may have their own frame-driven sub-loops
  (worldmap_ui) — audit before S1; may need the same driver-seam treatment.
- **Script-opened modal UI beyond movies** — inventory pass needed before real
  content runs on the server.
- **Interactive networked turns**: RESOLVED (P2) behind `F2_SERVER_RESUMABLE_COMBAT`
  — the beat-spanning session machine (§3) lets a fight service the network between
  turns without the nested `_combat_input` blocking the tick. Gate still default-OFF
  until the wire producer/consumer side (P3) lands.

## Critical files
`src/main.cc` (mainHeadlessProbe loop) · `src/input.cc` (tickersExecute clock feed)
· `src/combat_ui.cc`+`.h` (combat driver seam) · `src/scripts.cc` (`_doBkProcesses`
/ game-clock cadence) · `src/animation_scheduler.h` + `src/presenter.h` (the two
seams to compose: install InstantAnimationScheduler + NullPresenter).
