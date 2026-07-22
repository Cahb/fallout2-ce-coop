# Temple Demo Roadmap — Demo v1 north star

Status: **living plan** (2026-07-17). Owner-driven. This steers the P5 server/wire
track toward a concrete, demoable goal. See also `memory/p5-server-plan.md` (the
active track), `COMBAT_CLIENT_DESIGN.md`, `INTERACTION_UX_DESIGN.md`, `MP_PROTOCOL.md`,
and `IDEAS.md` (vision + the singleton constraints this respects).

## North star

1. **Demo v1 = solo-pass the Temple of Trials** over the dedicated server + wire viewer
   (the FO2 intro dungeon; the default start is the tribal Temple skin).
2. **Then: P2 co-op** — a *second, independently and manually driven* free-roam player
   (NOT an auto-follow companion) on the **same map** as P1, who can proceed through
   the Temple alongside P1 (P1 drives the dialogues / map transitions).

Party/leash system is **scrapped** — no "free party" mechanic. A player is a first-class
actor; being present on the same map is the only coupling. (See
`memory/mp-actor-architecture-principle.md`.)

## What the Temple requires vs. what exists

| System | Needed to pass | Status |
|---|---|---|
| Combat (unarmed/melee: ants, then Cameron's fistfight) | REQUIRED | Have it — wire-playable (crosshair attack, aimed shots, outlines) |
| Movement + map exit-grid → Arroyo (map change) | REQUIRED | Mechanism exists (`EVENT_MAP_TRANSITION` + rebaseline blob); mid-run transition once crashed the core-only server on the unsevered `automapSaveCurrent` symbol — automap symbols now no-op'd. **De-stub-in-progress: re-run the all-map sweep to surface any next domino.** |
| Intra-map elevation change (floor↔floor stairs, if the Temple has them) | MAYBE (structure unverified) | **NOT WIRED** — deltas carry no tile/elevation, no elevation event, `gElevation` never follows the dude → the dude vanishes to another floor. See "Map & elevation change" below. |
| The locked door — **Lockpick** skill on it | REQUIRED (or explosives) | Server verb exists (`skill <netId> LOCKPICK`, out-of-combat). VERIFY end-to-end + that the door opens |
| Dialog (Cameron fight trigger; the exit / elder) | REQUIRED | **THE gap** — engine works headless (`denbus1_dialog` golden) but the viewer can't see/pick options (TALK falls back to LOOK). §5 streaming gap |
| Loot containers / USE-item (urns: plastic explosive, healing powder) | OPTIONAL | In progress. NOT on the minimal path — Lockpick avoids the explosive; healing is optional |

### Key insight — REVISED 2026-07-18 (empirical, user live-play)
Original "lockpick avoids the explosive" is **WRONG**: the blocking Temple door is
**script-locked / unlockpickable** ("only boom boom") — lockpick can *never* open it,
regardless of skill. So the plastic explosive is the **only** way through → **C4 / explosive
USE is on the critical path.** Good news on the rest:
- **The explosive is obtainable** — it's in a **basket** (a bag container), which the loot
  slice already handles. No closed-container needed to get it.
- **Closed containers** (ice box / chest / footlocker) are **NOT critical path** — just
  annoying bonus-loot boxes the player can't open. Banked polish (the open-anim decouple),
  not a blocker.
- **C4 / explosive USE** — double-blocked (useitem explosive guard + the `SET_TIMER` modal
  unwired). The slice = timer-decouple + arm + *probably* an explosion-outcome headless
  decouple (the reg_anim-callback class — **verify** the explosion→door-destroy runs headless
  vs. no-ops); the door-destroy itself streams via `EVENT_DESTROY` ✓; blast visual = cosmetic.
Dialog is still a hard blocker (Cameron + exit). **So the real solo-Temple critical path =
C4-usage + dialog** (+ the combat-desync bug). Demo-hygiene alt if we want to skip C4:
script-open/destroy that door at start.

## Phase A — solo pass (Demo v1)

Ordered by necessity. **OWNER DECISION: build the TRUE demo, not a hack** — implement
**closed-containers + C4/explosive + dialog** for real (no skill/inventory hygiene
shortcuts, no script-open-the-door). Remaining build set: **combat-desync fix →
closed-container decouple (banked, off critical path) → dialog streaming.** (Working over the
wire: map transition, doors, combat, poison DoT, spear wield, loot, useitem heals, C4
arm+detonate, lockpick-to-advance.)

- **A0. Loot containers — DONE** (take/put/takeall/drag + loot-target reconcile). Off the
  minimal Temple path.
- **A1. Dialog options over the wire** — the real Temple blocker. Stream the active
  node's reply + option list to the viewer; viewer renders + picks; choice returns as the
  existing `dsay`/`dend` intents. Server dialog driver already exists; the gap is viewer
  render + input + option streaming.
- **A2. Verify Lockpick-the-door** over the wire (skill verb → door opens). Small.
- **A3. Verify unarmed Cameron fight + ants** end-to-end on the wire. Mostly polish.
- **A4. Verify Temple→Arroyo exit-grid transition** on the viewer. Likely already works.
- **A5. USE-item** — out-of-combat `useitem` (healing powder / stimpak self-use) + C4 arm
  DONE. In-combat item use + use-on-world still deferred.
- **A6. Map & elevation change (DEMO-CRITICAL — surfaced 2026-07-18).** Recon findings:
  - **Map change (exit grid / stairs with `destinationMap>0`): mechanism exists, de-stub
    in progress.** `useStairs` (`proto_instance.cc:1585`) → `mapLoad` →
    `presenter()->mapTransition` (`map.cc:851`) → `EVENT_MAP_TRANSITION` → viewer resets +
    the next blob rebuilds (`mapLoad` sets `gElevation`). BUT a mid-run transition **crashed**
    the core-only server on the unsevered client symbol `automapSaveCurrent` (`map.cc:1361`,
    the automap-save step). Automap symbols now no-op'd (`server_stubs.cc`); **re-run the
    all-map sweep to surface any next domino** — fresh map loads always worked (join), the
    *transition* path was never fully severed.
  - **Intra-map elevation change (stairs `destinationMap==0`, floor↔floor in one map):
    NOT WIRED.** (1) `OBJECT_DELTA` carries no tile/elevation (`object_delta.cc:136-147`;
    movement rides walk-hop events); (2) no event fires on an intra-map elevation change;
    (3) `gElevation` is never assigned in the viewer — only `mapLoad` sets it. So the dude
    changes floor server-side and **vanishes** from the viewer. Fix = stream the actor's
    elevation change + have the viewer call `mapSetElevation(myActor->elevation)`. This is
    the "`gElevation` follows the actor" work also needed for P2 co-op.
  - **DO FIRST: verify `artemple.map`'s structure empirically** (intra-map elevation stairs
    vs single-floor + a map sequence). Map metadata is `.dat`-packed (not in `data/`); cheap
    check is loading it on the server / observing in-game. That decides whether elevation-
    follow is on the Temple critical path or just map-transition-verify is.

## Phase B — second player (co-op)

- **B1. Scrap leash/party** — remove party assumptions from the player path (the actor
  model was built to allow this; do not branch player logic on party membership).
- **B2. Two independent player actors on one map** — a 2nd controllable dude object + 2nd
  claimant binding; each viewer drives *its own* actor. Leverages N-viewers + the control
  gate. Depends on finishing gDude → per-client role (partially done for UI).
- **B3. Same-map coupling** — both players share the one object list = the *cheap* case
  that respects the engine singleton wall (one gMap / one object list / one combat ctx).
  Separate maps = the wall; we stay same-map.
- **B4. Co-transition** — when P1's dialogue/exit moves Temple→Arroyo, P2 moves with it.
- **B5. (stretch)** P2 dialog / trade on its own.

## Elevation architecture (Fable investigation, 2026-07-17) — RESOLVED

- **An elevation is a fully-resident floor of one loaded `.map`** (up to 3). `mapLoad`
  instantiates every object of every elevation; `gElevation` is only the camera/render
  selection.
- **The per-tick sim ticks ALL elevations** (event queue, `critter_p_proc`, map scripts,
  movement, lighting) — proven elevation-blind. So P1 on floor 0 and P2 on floor 1 are
  both fully simulated out of combat. The wire already streams all elevations; each viewer
  owns its own render `gElevation`.
- **The ONE elevation wall is COMBAT**: a single global context (`gCombatState`,
  `_combat_list`, `_combat_elev`, `gCombatSession`, `obj->cid`), a single-`gElevation`
  roster, a start guard that drops off-`gElevation` fights (`combat.cc:3923`), and a
  whole-world freeze while in combat. Two concurrent combats = a bounded refactor
  (per-context struct + scope the freeze), made tractable by the already-landed resumable
  combat state machine — NOT a rewrite.
- **Different *map* remains the true wall** (full world teardown on `mapLoad`).
- **The Temple is ONE elevation of ONE map** (`artemple.map`); exits are map transitions to
  `arvillag` / `arcaves`. Demo v1 needs zero elevation work.
  > ⚠️ **UNVERIFIED / DISPUTED (2026-07-18).** The owner doubts the one-elevation claim
  > ("multi-level or a map sequence, don't remember"). This is load-bearing and was a Fable
  > assertion, not an empirical check — **do not trust it.** See "Map & elevation change"
  > below; verify `artemple.map`'s structure empirically before relying on it.

**P2 leash decision (was deferred):** grant **same-map any-elevation free-roam** — it's
nearly free (sim already ticks all floors; work = scrap the party sync-yank, make each
viewer's `gElevation` follow its own actor, sweep a short leak list: `obj_on_screen`,
`_ai_search_environ`, rest checks). "Same elevation" only needs enforcing for **combat
participation**, and even that means "the fight is on one floor," not "P2 must live on P1's."

## Open decisions

- Concurrent two-floor combat: optional stretch (bounded refactor) — defer past Demo v1.

## Respected constraints (do not fight these in v1)

- Engine singletons (one gMap / object list / combat context) — same-map co-op only.
- Modal loops block the tick — every viewer modal must pump the wire + bail on combat.
- Server is single-threaded/sync — concurrent P1/P2 actions on a shared object (e.g. one
  container) serialize fine; no per-object locks (two players may share a loot pile).
