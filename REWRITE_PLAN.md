# Fallout 2 CE — Headless Server Rewrite Plan
### Strangler-refactor roadmap: keep the engine, replace the architecture

> Companion to `SYSTEM_MAP.md` (data topology & coupling analysis).
> Strategy: in-place refactor with a behavioral oracle at every step. No clean-room rewrite.
> Execution model: AI-driven implementation, human-gated phases. Every phase ends with
> (a) green golden replays, (b) updated docs, (c) a build you can play or run headlessly.

---

## Ground rules (apply to every phase)

1. **No `#ifdef` in game logic.** Isolation happens at link time: libraries + interfaces, two binaries.
2. **Two-commit rule for semantic changes.** Mechanical restructuring (replay-identical) and
   behavior changes (replay-diff, needs explicit sign-off) are never mixed in one commit.
3. **Worklist-driven sessions.** Each phase begins by generating `WORKLIST_P<N>.md` (grep-derived,
   enumerated call sites / files, checkbox per item). Sessions consume the worklist, not the tree.
4. **CI is memory.** Golden replays + headless smoke tests run on every commit. A session never
   relies on remembering an invariant; it relies on CI catching the violation.
5. **Docs are part of the diff.** `SYSTEM_MAP.md` / decision log updated whenever reality changes.

---

## Phase 0 — Verification harness + headless boot  *(the feasibility probe)*

**Goal:** prove the sim can run without a screen, and build the oracle everything else depends on.

| # | Work item | Notes |
|---|---|---|
| 0.1 | Canonical world-state dump | New `state_dump.cc`: walk objects/critters/vars/queue/clock → deterministic text (sorted, no pointers). Reuses the field knowledge in `objectDataWrite`. This is the diffable oracle format. |
| 0.2 | Determinism audit | Single RNG entry (`random.cc`) — verify no stray `rand()`/time-based seeds in sim paths; make seed injectable via CLI/config. |
| 0.3 | Input trace record/replay | Hook the existing input queue (`input.h: enqueueInputEvent`) to record `(tick, event)` streams in the normal client; add replay injection. |
| 0.4 | Headless target | New `f2_headless` binary: null video (framebuffer in RAM, no SDL window), null audio, null mouse. Boots game init chain → `mapLoadById` → runs N ticks → dumps state → exits. Expect to stub `svga.cc`, `audio_engine.cc`, parts of `input.cc` behind their existing function seams. |
| 0.5 | Golden replay CI | Record 3–5 traces on the unmodified client (walk around, a fight, a dialog, a map transition). CI: replay headless, diff state dump against checked-in golden. |

**Gate:** same trace + same seed ⇒ byte-identical state dump, twice in a row, on CI.
**Risk:** real-time leakage into sim (`getTicks()` in animation pacing) may make replays flaky —
if so, that's *finding the Phase 2 work early*, not failure.
**Est. sessions:** 10–20. **This phase is the go/no-go decision point for the whole project.**

---

## Phase 1 — Presenter seam: split core from client

**Goal:** `f2_core` (sim) and `f2_client` (presentation) as separate libraries; sim never calls draw.

| # | Work item | Notes |
|---|---|---|
| 1.1 | Generate call-site worklist | Grep sim files for direct render/UI calls: `_obj_render_*`, `tileWindowRefresh*`, `interfaceRender*`, `displayMonitorAddMessage`, `textObjectAdd`, outline calls, `gameMouse*`, sfx triggers. Expect a few hundred sites across ~40 files. |
| 1.2 | Define `Presenter` interface | Small header in core: object moved/created/destroyed, anim requested, float-text, console message, sfx, screen-shake etc. **This interface is the embryonic wire protocol — keep it data-only (ids + POD), no pointers into core.** |
| 1.3 | Convert call sites | Mechanical, worklist-driven. `SdlPresenter` (client) reproduces old behavior 1:1; `NullPresenter` (headless) drops or logs. |
| 1.4 | CMake split | `f2_core` static lib (object/map/tile-math/combat/stats/skills/critter/item/proto/queue/scripts/interpreter), `f2_client` lib, three binaries: `fallout2-ce`, `f2_headless`, (mapper keeps building). Enforce: `f2_core` has no SDL include, checked in CI. |

**Gate:** golden replays byte-identical; normal client visually indistinguishable (human spot-check).
**Est. sessions:** 20–35. Grindy but low-risk; everything is replay-protected.

---

## Phase 2 — De-entangle animation from logic

**Goal:** game outcomes never wait on frames; pathfinding and AP rules live in core.

| # | Work item | Notes |
|---|---|---|
| 2.1 ✓ | Extract pathfinder | DONE — `pathfinderFindPath`/`_make_path`/`_make_straight_path(_func)` (+ `canUseDoor`) → `path.cc` in **f2_core**. Replay-identical. |
| 2.2 ✓ | Movement rules module | DONE — per-step combat movement AP charge → `movement.cc` in **f2_core** (`movementChargeApForStep`). Replay-identical. |
| 2.3 ◐ | `AnimationScheduler` interface | SEAM DONE (`animation_scheduler.{h,cc}`, base = real-time no-op, inert). Instant activation ATTEMPTED + REVERTED — see status below. |
| 2.4 ○ | Combat wait points | DEFERRED (depends on 2.3-live). |

**Gate:** two-commit rule applies heavily here — 2.1/2.2 must be replay-identical; 2.3/2.4 will
change timing-sensitive replays → re-record goldens with explicit human sign-off per diff.
**Risk:** highest subtle-bug density in the project (interrupted moves, knockback anims carrying
damage application). Mitigate with headless AI-vs-AI combat fuzzing (seeded, thousands of fights,
assert invariants: HP ≥ 0 consistency, AP conservation, no stuck turns).
**Est. sessions:** 15–30.

**Status (2026-07-12):** 2.1 + 2.2 landed (both promoted into `f2_core`); 2.3's scheduler
seam landed (inert). Activating the instant scheduler was attempted three ways and reverted.
Root cause: under `F2_FAKE_CLOCK` "time" is a function of `getTicks()` **call-count** plus
frame/render work, not a real clock — so completing animations instantly cascades into four
distinct timing couplings: (1) ambient-critter wander RNG diverges, (2) getTicks-gated UI
render loops deadlock, (3) the `gIteration`-keyed input-replay traces desync, (4) shifted
game-time fires a script `opPlayGameMovie` that hangs headless (movie sync loop). Conclusion:
instant animation is **not a retrofit** into the frame-driven headless loop — it belongs in a
dedicated fixed-timestep server tick loop (the Phase-5 `f2_server` main loop) that never runs
movies / UI / frame-pacing, sidestepping all four couplings by construction. The 2.3 seam is
the ready hook for it. (Full findings + gdb evidence in the agent's project memory.)

---

## Phase 3 — De-modalize the blocking loops

**Goal:** no nested event loops in core; everything is a state machine advanced by tick/messages.

| # | Work item | Notes |
|---|---|---|
| 3.1 | Combat state machine | `_combat()` / `_combat_turn_run` → `CombatSession` ticked from the main loop: states (idle → starting → actor-turn → resolving → ending), consumes intents (attack/move/end-turn), emits presenter events. AI turns run to completion within a tick; player turns wait on intents. |
| 3.2 | Dialog as session | `game_dialog.cc` + `dialog.cc` globals → `DialogSession` (speaker, script program, current reply, options list). Client renders session state; choice = intent. Script procs still run synchronously inside the session — only the *waiting for the human* part becomes async. |
| 3.3 | Barter / loot / elevator / skilldex | Same pattern, much smaller. |
| 3.4 | Intent layer | Formalize `scriptsRequest*` + new player intents into one typed command queue (`intents.h`): Move, Attack, Use, PickUp, DialogChoice, EndTurn, Transition. Single validation choke point — this later becomes the network command handler verbatim. |
| 3.5 | `GameMode` retirement | Static bitfield → per-session state; audit its readers (mostly UI, some script opcodes). |

**Gate:** headless tests drive a full combat and a full dialog tree via intents only; client still
plays normally (its UI now *produces* intents).
**Est. sessions:** 25–40. The design-heavy phase; expect the most human decision points.

---

## Phase 4 — Identity & instancing

**Goal:** "the player" and "the world" become values, not globals. Multiplayer-shaped, still 1 process = 1 world.

| # | Work item | Notes |
|---|---|---|
| 4.1 | `Character` aggregate | Fold `PcStat` array, tagged skills, traits, kill counts, dude-state flags, GCD identity into `struct Character { Object* body; ... }`. Thread through `pcGetStat`-family call sites (worklist-driven; hundreds of mechanical edits). `gDude` survives as `party[0].body` alias during transition. |
| 4.2 | Multi-character support | N `Character`s on one map: spawn, per-character sneak/level-up, XP split policy (**design decision**), per-character dialog sessions (world keeps ticking — **design decision**). |
| 4.3 | `World` object | Clock, GVARs, RNG stream, worldmap state, party roster into one struct; per-instance seed. |
| 4.4 | *(optional / v2)* `MapInstance` | Wrap `gMapHeader`, `_square[]`, MVAR/LVAR pools, spatial index, light grids, spatial scripts, combat session into an instanceable struct → players on different maps, ticked sequentially. **Explicitly deferred until co-op on one map is proven fun.** |

**Gate:** headless sim of 2 characters fighting/looting/talking on one map; replay determinism holds.
**Est. sessions:** 20–35 (without 4.4).

---

## Phase 5 — Network v1: tethered co-op (2–4 players, one map)

**Goal:** dedicated `f2_server` + thin-client mode of the existing client.

| # | Work item | Notes |
|---|---|---|
| 5.1 | `f2_server` binary | Main loop: drain client intents → validate → tick queue/combat/scripts → `gameTimeAddTicks` → presenter flush. Mostly assembled from parts built in phases 0–4. |
| 5.2 | `NetworkPresenter` | Serializes presenter events; full-state snapshot on join (reuse save-format knowledge), event stream after. Server-authoritative; no client prediction in v1 (turn-based/pausable design tolerates latency). |
| 5.3 | Thin-client mode | Existing client renders remote world: local `Object` mirror updated from event stream; input layer emits intents instead of calling core. Client keeps `f2_core` linked for art/proto/message data only. |
| 5.4 | Sessions & persistence | Join/leave, character files server-side, world save = existing save pipeline. |
| 5.5 | Co-op design decisions land here | Pause-vs-continue during dialog, combat turn ownership, loot rules, worldmap travel votes. Each is a small implementation once decided. |

**Gate:** 2 clients on LAN: walk, fight a scripted encounter, trade items, transition maps together.
**Est. sessions:** 25–45.

---

## Cumulative estimates

| Phase | Sessions | Output tokens (rough) | Human gate effort |
|---|---|---|---|
| 0 Harness + headless | 10–20 | 2–4M | 1–2 hrs (review dumps, run replays) |
| 1 Presenter split | 20–35 | 4–8M | 1–2 hrs (play-test client) |
| 2 Animation de-entangle | 15–30 | 4–8M | 3–5 hrs (sign off replay diffs, play-test) |
| 3 De-modalize | 25–40 | 6–12M | 4–6 hrs (design decisions + play-test) |
| 4 Identity/instancing | 20–35 | 5–10M | 2–4 hrs (design decisions) |
| 5 Network v1 | 25–45 | 6–12M | 4–8 hrs (LAN testing) |
| **Total** | **~115–205** | **~27–54M output** | **~15–27 hrs spread over the project** |

Kill-switch economics: Phase 0 is ~7% of total spend and definitively answers "will this work."
Phases 0–2 alone yield a deterministic, headless, testable Fallout 2 sim — independently valuable
(TAS/AI experiments, regression testing for CE itself) even if multiplayer is never built.

## Open design decisions (owner: you; needed by phase in parentheses)

1. World behavior while a player is in dialog/barter: pause world, pause locally, or keep ticking? (P3)
2. ~~Combat: strict single initiative order for all players, or simultaneous player turns? (P3)~~
   **DECIDED (user, 2026-07-15): sequential, vanilla-skeleton, player block.** Initiative
   follows the original game (initiator slot 0, sequence-sorted roster; NPC-initiated =
   normal order). Connected players act as one contiguous chain anchored at gDude's slot:
   host, then player+1, player+2, then the roster continues — so a host-initiated fight has
   the whole player party act before the defenders. Deliberate divergence from vanilla: the
   original interleaves party members individually by Sequence stat; players are grouped
   after the host regardless. Consequence for the combat session machine: ONE contiguous
   player input-barrier window per round ("waiting on player K of M", per-player idle
   timer), and the player-turn slot iterates a list of player actors (v1 list = {gDude}).
3. XP: shared pool, split, or per-contributor? (P4)
4. Worldmap travel & map transitions: leader decides vs. vote vs. proximity requirement? (P5)
5. Persistence model: host-owned save vs. server campaign with per-player characters? (P5)
