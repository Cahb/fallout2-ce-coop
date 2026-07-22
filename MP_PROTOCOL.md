# MP_PROTOCOL — the dedicated-server wire protocol (P5-B)

Status: **DESIGN + IN PROGRESS.** This is the design of record for the outbound
state/event stream of the Fallout 2 CE dedicated server (REWRITE_PLAN Phase 5,
the "P5" track). It supersedes ARCHITECTURE.md's premise that "serialize the
presenter calls == the wire protocol" — that premise was *false at rest*: the
Phase-1 presenter seam (`src/presenter.h`) carried effects (console/floatText/
sfx/hud/music) but **object position/create/destroy/fid never crossed it**; the
sim mutated `obj->tile` directly and the client only got `worldInvalidate(rect)`.
P5-B *widens* the seam so world-state changes DO cross it (decision: **widen**,
not snapshot-diff — see "Why widen" below).

Read alongside: the `p5-server-plan` / `mp-actor-architecture-principle` memories
(cadence, actor model), `SERVER_LOOP_DESIGN.md` (the tick loop this rides on),
`src/presenter.h` (the seam being widened), `src/server_loop.cc` (serverServe).

--------------------------------------------------------------------------------
## 1. Cadence recap (the model these events serve)

The server is **logical-time-only, never sleeps**, beat-paced: resolve one
action/step (a beat), emit its event(s), advance the sim clock, next beat; block
only at input barriers (a player's turn / dialog choice). Each event carries
`(sequence#, sim-timestamp, presentation-duration)`. The **client self-paces**
off timestamps; the server never gates on client ACKs.

Every event splits into:
- **STATE-DELTA** — authoritative, **ALWAYS applied** (even if stale/late).
- **PRESENTATION** — animation/sfx/floattext, **skippable** under lag.

Catch-up rule: a stale event (ts behind the client's playback clock) = apply
state INSTANTLY + SKIP animation, **never discard** (dropping state = permanent
desync). Sequence# (not just ts) distinguishes a *gap* (missed event → request
snapshot + resync) from merely-behind (fast-forward by collapsing animation).

The **snapshot** (join / lag-gap resync / desync recovery) is ONE mechanism:
"full current state, then stream from seq N". v1-mandatory use = join/map-load.
v1 recovery-from-fubar may just be RECONNECT (re-fires the join snapshot).

--------------------------------------------------------------------------------
## 2. Wire vocabulary (~24 types; ~13 generic core)

The raw enumeration (~58 inbound, ~81 outbound presenter/mutation calls)
**collapses**: the variety is in DATA, not message-type count. Everything the
sim can do reduces to these primitives.

### STATE (authoritative, always applied)
- `spawn(obj)` — an object now exists in the world.
- `destroy(id)` — an object no longer exists in the world.
- `connect(id, tile, elev)` / `disconnect(id)` — an object re-parented across the
  inventory↔world-tile boundary (item drop/scatter/unload, ground pickup, script
  obj_connect/obj_disconnect). **Distinct from spawn/destroy: the object persists**,
  it just crosses between an inventory (tile==-1) and the map. The owner's inventory
  side is separately visible via `objectDelta`'s inventory bit. See §4.
- `move(id, fromTile, toTile, fromElev, toElev, durMs)` — **ONE AUTHORITATIVE HOP**
  (a single `objectSetLocation`, see §4). A walk is a STREAM of these, one per tile
  per beat (stepped server walking, F2_SERVER_SMOOTH_WALK). durMs is presentation
  pacing only: >0 = animate the hop over ~durMs (stepped walk/run), 0 = snap
  (teleport, scripted placement, within-beat sync moves). ►► PINNED 2026-07-15
  (Fable design review): the earlier `move(id, path[], durMs)` path-coalescing shape
  is REJECTED, not deferred — per-tile events are ~46B/step (50 runners ≈ 23 KB/s,
  two orders below budget), per-tile is what the sim authoritatively does (en-route
  doors/spatial procs/interception), corrections are free (a blocked walk just stops
  emitting), and recovery per §1 is per-tile-checkpointed. Do NOT repurpose
  EVENT_MOVE to carry paths — that is a silent meaning change every consumer would
  miscount; if a path announcement is ever wanted it must be a NEW event type
  (skip-unknown-T makes that cheap). Freeroam-scale bandwidth, if ever a problem,
  is solved by per-client interest management (transport), not path compression.
- `objectDelta(id, {fid?, rot?, flags?, hp?, rad?, poison?, ap?, results?, inventory?})`
  — **ONE BATCHED delta per resolved action**. Absorbs actor-state + item-move +
  equip + pickup/drop/heal/damage. Sparse: only changed fields present. The
  `inventory` field also carries intra-item changes (weapon ammo, charges) — F2 has
  no item condition/durability field. gvars/gametime are `worldDelta`, not here.
- `worldDelta({gameTime?, gvars?})` — global sim state.

### PRESENTATION (skippable cues; already in presenter.h)
`playAnim(id,kind,params)` / `sfx` / `sfxAt` / `floatText` / `console` /
`music` / `fade` / `errorBox`.

### COMBAT control
`combatEnter` / `combatExit` / `turnStart|yourTurn(deadline)` / `attackResult`
(or fold to `console`+`objectDelta`).

### SESSION / MAP control
`joinAccepted` / `snapshot begin|chunk|end` / `mapTransition` / `pause|resume` /
`resyncRequired` / `leave(id)`.

### Open defaults (none blocking)
- gvars / mvar / lvar = **all server-only in v1** (push a gvar only if a
  client-visible thing depends on it).
- delta granularity = **batch per resolved action** (leaning TLV encoding:
  sparse per-field TLV inside a delta + a per-tick multi-event frame; forward-
  compat via skip-unknown-T). **WIRE ENCODING is a P5-C serialization detail**,
  swappable behind the logical event model — NOT decided here.
- move = one path event; multi-victim blast = one batched event.

### Turn barrier
A **configurable per-player-action idle timer**, not a flat per-turn timeout.
While it's a player-actor's turn with AP left, an inactivity timer runs; ANY
committed action resets it; expiry with no action → auto-skip/end-turn. The
server never waits on a client; an engaged player is never cut off mid-thought
as long as they keep acting.

### Snapshot-only / never an event (server-owned)
RNG fingerprint, event-queue, sid/scriptIndex, checkpoints.

### ⚠ Identity risk (protocol-wide, P5-C)
`obj->id` is **not guaranteed unique** for map-placed objects (only script-owning
singletons have a unique sid/scriptIndex; see the `dtalk` verb's use of
scriptIndex over id). The wire keys state on a network id, so P5-C must mint a
truly-unique server id (e.g. a monotonic registry, or (map,load-index) tuple),
not trust `obj->id` blindly. Server-LOCAL bookkeeping (the objectDelta shadow,
§6.2) sidesteps this by keying on the `Object*` pointer and rebuilding from the
live object list each beat.

--------------------------------------------------------------------------------
## 3. Why widen (not snapshot-diff)

Two ways to get world-state onto the wire:
- **(i) widen the seam** — emit events at the sim mutation sites; events fall out
  of the mutations themselves. Correct long-term; the presenter becomes the one
  place the sim talks to the outside.
- **(ii) snapshot-diff** — retain last tick's object/global table, diff per tick,
  reuse `state_dump.cc`'s object walk. Cheaper to stand up, decouples from the
  seam.

**Decision: (i) widen.** It matches the existing Phase-1 seam philosophy (the sim
already emits console/sfx/hud through `presenter()`), keeps a single outbound
channel, and gives semantic events (move-with-path, "critter died") that a diff
can't cheaply reconstruct. BUT §4's recon shows widening is only clean for the
*lifecycle* primitives — for the fieldwise deltas we adopt a **hybrid**: batched
`objectDelta` computed at the resolved-action boundary (which is effectively a
*scoped* snapshot-diff of the one acting object). So (i) and (ii) are not
opposites; the batched delta is (ii) applied narrowly where (i) has no choke.

--------------------------------------------------------------------------------
## 4. Mutation-site map (the durable recon artifact — 2026-07-13)

Where each wire primitive is *emitted from* in f2_core. `obj->id`
(`obj_types.h`, the network id, assigned at creation via `scriptsNewObjectId()`)
is a plain `Object*` field reachable at every site below.

### CLEAN single choke points → direct lifecycle events (P5-B commit 1)
| Primitive | Hook | Notes |
|---|---|---|
| `spawn`   | `objectCreateWithFidPid` (object.cc:734, at final exit ~:821) **+** `_obj_copy` (object.cc:839, at exit ~:908) | Two live create paths. `objectCreateWithPid` is a wrapper → covered. Do NOT hook `_obj_insert` (:3499) — it re-fires on every re-link and on map load. |
| `move`    | `objectSetLocation` (object.cc:1198, after `_obj_connect_to_tile` commits ~:1242) | THE tile/elevation choke. Skip `_obj_offset` (:978, sub-tile pixel interpolation — derive client-side). `_obj_move` (:1096) is cursor-only. |
| `destroy` | `objectDestroy` (object.cc:1816, at entry before the free) | Covers the `_obj_destroy` wrapper (proto_instance.cc:732) and all direct callers. Do NOT hook `_obj_remove` (:3560) — `_obj_remove_all` (:1946, map teardown) bypasses objectDestroy anyway (that's a "world cleared" bulk case, not per-object). |
| `connect`    | `_obj_connect` (object.cc:923, after `_obj_connect_to_tile` returns 0) | Item attaches to a world tile (drop/scatter/unload/script obj_connect). Goes through `_obj_connect_to_tile` which BYPASSES `objectSetLocation` (no `move`) and `objectCreateWithFidPid` (no `spawn`) → this is the sole appearance signal. Distinct verb from `spawn` (object persists). |
| `disconnect` | `_obj_disconnect` (object.cc:950, before `return 0`, obj->tile now -1) | Item detaches from its world tile into an inventory/limbo (pickup/consume/unload/script obj_disconnect). Bypasses `objectDestroy` (object persists) → sole disappearance signal. Success path only. |

**Load-safety of the lifecycle hooks — CORRECTED 2026-07-13 (P5-B item↔world commit):**
there are TWO distinct load paths and they differ:
- **Savegame restore** (`objectLoadAllInternal`, object.cc:446 → `objectAllocate` +
  `objectRead` + `_obj_insert`; inventory via `_obj_load_obj`): bypasses
  `objectCreateWithFidPid` AND `_obj_connect`, so **no spawn / no connect / no
  disconnect fire**. Genuinely load-safe.
- **Fresh `.map` load** (map transition, new game — the path the `arvillag_entermap`
  narrate golden exercises): PRE-PLACED map objects and their inventories load
  SILENTLY via `objectLoadAll`→`_obj_load_obj` (object.cc:3244, `objectAllocate` +
  `objectRead`, no `objectCreateWithFidPid`/`_obj_connect`). The lifecycle FLOOD comes
  instead from the **map-enter SCRIPTS** that `mapLoad` runs, which create loot / spawn
  scenery via `objectCreateWithPid`→`_obj_disconnect`→`itemAdd`: `scriptExecProc(gMapSid,
  MAP_ENTER)` at map.cc:786 (**BEFORE** the `mapTransition` emit at :816) and
  `scriptsExecMapEnterProc()` at map.cc:832 (**AFTER** :816). Empirically `arvillag_
  entermap` emits ~62 `spawn` + ~29 `disconn` (Secret Blocking Hexes, Car Trunk, then
  per-critter default loot) STRADDLING the `mapTransition` line on both sides; placed
  ground items also fire `connect`. Teardown uses `_obj_remove_all` (bypasses `objectDestroy`).
- So the earlier "spawn/destroy are naturally load-safe, only move needs suppression"
  claim held ONLY for savegame restore. **The P5-C load-window guard must suppress the
  WHOLE lifecycle stream (spawn/destroy/connect/disconnect/move + the per-beat
  objectDelta scan) and it must BRACKET THE ENTIRE `mapLoad` CALL — it CANNOT be
  anchored on the `mapTransition` signal, because events fire both before (map.cc:786)
  and after (:832) that signal.** `mapTransition` still tells the client to drop its
  world and expect a fresh snapshot; the guard is what keeps the intervening flood off
  the wire. Until the NetworkPresenter lands, all these methods are no-ops → load
  emission is harmless.

**Consumer invariants for connect/disconnect (P5-C):**
- `disconnect` can arrive with NO prior `connect`: a stack-split (`itemRemove`→`_obj_copy`,
  item.cc:444) or the `give` idiom creates an item at `tile==-1` and immediately
  `_obj_disconnect`s it (the `denbus1_itemworld` golden shows `spawn id=182 tile=-1`
  then `disconn id=182` with no `connect`). Treat `objectDisconnected` idempotently;
  never assume a matching prior `objectConnected`.
- `move` (objectMoved) with `fromTile == -1` is ALSO a world-entry signal (an object
  placed from limbo via `objectSetLocation`, e.g. map-script objects), equivalent to
  `connect` for a presence-tracking consumer.
- `spawn`→`disconn`→`connect`→`disconn` on ONE `obj->id` across a give/drop/pickup
  cycle is coherent given the above (the object persists throughout).

### NO clean choke point → batched `objectDelta` at the resolved-action boundary
These deliberately are NOT hooked per-call (per-frame churn and scattered writes
would flood the wire and are a completeness trap):
- **fid** — `objectSetFid` (object.cc:1360) is the setter, but animation changes
  fid *per frame* (walk cycles); one direct-write bypass at item.cc:3247
  (unequip). fid is presentation-derivable except at rest → carry it in the
  per-action `objectDelta`, not per `objectSetFid`.
- **rotation** — `objectSetRotation` (object.cc:1504) is clean, but rotation
  churns with movement animation → same treatment as fid.
- **flags** — **no setter.** `OBJECT_HIDDEN/NO_BLOCK/FLAT/EQUIPPED/...` are set by
  scattered direct `obj->flags |= …` writes (critter.cc:884 death; proto_instance
  doors 1638/1676; combat_ai sneak 2392+; item.cc equip ×many; actions NO_SAVE).
  The only structured mutators are `objectShow`/`objectHide`/`_obj_toggle_flat`.
  → carry the whole `flags` word in `objectDelta`.
- **hp** — `critterAdjustHitPoints` (critter.cc:293) IS a clean choke (damage/
  heal/poison/rest all funnel here), but pair it with death (below) → `objectDelta`.
- **combat.results** — **no choke;** OR'd in 8 sites across 5 files
  (`_set_new_results` combat.cc:4727 covers only the attack path; `critterKill`
  ORs DAM_DEAD directly; knockout/wake/stand-up/script-injure/skill-heal each set
  their own). → diff per acting critter into `objectDelta`, do not micro-hook.

### `objectDelta` inventory + equip
- abstract inventory: `{itemAdd (item.cc:329), itemRemove (item.cc:406)}` — every
  move/loot/barter/drop-source funnels through these two (`_item_move_func`,
  `itemMoveAll`, `itemDropAll` all call them).
- equip **bypasses** add/remove: `_invenWieldFunc` (inventory.cc:310) /
  `_invenUnwieldFunc` set `OBJECT_WORN/IN_*_HAND` directly + re-fid the critter.
- item↔world lifecycle on drop/pickup: `_obj_connect` (object.cc:912) /
  `_obj_disconnect` (object.cc:939) attach/detach an item Object to the tile grid
  (distinct from inventory membership). Drop choke = `_obj_drop`
  (proto_instance.cc:678).
- watch two object mint/kill hidden inside inventory ops: `itemAdd` may
  `objectDestroy` a merged ammo stack (item.cc:398); `itemRemove` may `_obj_copy`
  a partial stack (item.cc:440) — both already covered by the lifecycle hooks.

### Death — a SEMANTIC event, not corpse-flag scraping
Death is split across paths: the **flag/XP/script half** runs in `_apply_damage`
→ `_set_new_results` (sets `DAM_DEAD`, awards XP, runs SCRIPT_PROC_DESTROY); the
**physical-corpse half** is `_show_death` (actions.cc:578) on the *animated/client*
path vs `critterKill(…,false)` (critter.cc:819) on the *non-animated/server* path.
Hooking only `critterKill` MISSES every animated death. → emit a semantic
"critter died" as an `objectDelta` (hp=0 + flags + death fid) computed when the
`DAM_DEAD` transition is observed, decoupled from which corpse-finalizer ran. On
the server path (serverLoopActive), `critterKill` is the finalizer and IS a fine
single emit point; the client-path split only matters once a thin client drives
its own combat, which is post-v1.

### `worldDelta`
- game time: `gameTimeSetTime` (scripts.cc:353) / `gameTimeAddTicks` (:363) —
  plus a **bypass**: the free-running `gGameTime += 1` at scripts.cc:770
  (`_script_chk_timed_events`, +1 per 100ms out of combat). Server is
  authoritative on time; clients take `worldDelta.gameTime`, they do not run the
  free-running increment.
- gvars: `gameSetGlobalVar` (game.cc:491) is the single write choke (script op
  `opSetGlobalVar` funnels here). Bulk `gameLoadGlobalVars` (game.cc:523) bypasses
  it on load → suppress during load.
- map/local vars: `mapSetGlobalVar` (map.cc:311) / `mapSetLocalVar` (map.cc:349,
  the funnel for both map-local and script-local). Per-map, load-populated →
  server-only in v1.

--------------------------------------------------------------------------------
## 5. Load / save-restore suppression

The join snapshot carries initial state, so the event stream must be SILENT
during map load and save-restore (else a client gets the whole map as spurious
spawn/connect/disconnect/move/delta events on top of the snapshot). Note (§4
correction): a FRESH `.map` load fires spawn AND connect/disconnect too — not just
move — so the guard must cover the whole lifecycle stream, not a subset.

Existing distinguishers:
- `serverLoopActive()` (server_loop.cc) — authoritative-server sim gate.
- `_isLoadingGame()` (loadsave.cc:1693) — true during **save-slot restore only**.
- **No dedicated `gMapLoading` flag exists.** Map transition (`mapLoad`,
  map.cc:628) is NOT covered by `_isLoadingGame()`. A NetworkPresenter needs a
  load-window guard around `mapLoad`/`mapLoadSaved` + `scriptsExecMapEnterProc`;
  add one when the NetworkPresenter lands (P5-C). Until then the widened methods
  are no-ops so load emission is harmless.

--------------------------------------------------------------------------------
## 6. Implementation sequencing (each a two-commit gate; goldens byte-identical
until a NetworkPresenter overrides a method)

The widened methods have **no-op defaults in the base `Presenter`** and are NOT
overridden by `ClientPresenter`/`NarratePresenter`, so hooking them into core
mutation sites is **inert by construction** — byte-identical on every path
(client, legacy probe, server) until P5-C's NetworkPresenter consumes them.

1. **[commit 1 — this]** Lifecycle events: add `objectCreated` / `objectMoved` /
   `objectDestroyed` no-op virtuals; hook the clean choke points (§4). Byte-
   identical.
2. **[commit 2a — done]** `objectDelta` scalar fields: `object_delta.{h,cc}` — a
   per-beat SHADOW DIFF (not the rejected "acting object set" a-priori tracking).
   Snapshot each object's syncable scalars (fid/rotation/flags + critter hp/
   radiation/poison/AP/combat.results, keyed by `Object*`); at the end of each
   serverTick diff vs the shadow and emit `objectDelta(obj, changedMask)` per
   changed object; rebaseline silently on map change. Complete by construction
   (observes the RESULT of any mutation, so the no-choke-point flags/combat.results
   are covered). Excludes tile/elevation (that's objectMoved). Byte-identical
   (side-effect-free + no-op under the null presenter); verified firing via
   NarratePresenter (wound hp=18, heal hp=23, lethal hp=0/results=0x80/corpse
   flags, door OPEN_DOOR flag deltas). Adversarial review 2026-07-13: EQUIVALENT-
   WITH-CAVEATS, byte-identical airtight (objectFindFirst global iterator is safe
   — no sim consumer resumes a find across a beat, scan runs last + pure-read).
   ►► KNOWN LIMITATIONS (deferred to P5-C, documented not silent):
   (a) party members / OBJECT_NO_SAVE actors untracked + recruit/dismiss toggle
   gap — consistent with the snapshot oracle (state_dump skips NO_SAVE too), aligns
   with party-as-removable-behavior; track via an explicit syncable set when
   PlayerActor lands. (b) objects at tile -1 / art-hidden fids invisible to the
   scan for that span (shared with the snapshot oracle → no join-vs-stream drift).
   (c) whole-flags-word diff emits client-local bits → mask to a syncable subset.
   (d) emit-during-iteration: the NetworkPresenter callback must not mutate the
   object list (buffer + flush after the loop).
2b. **[commit 2b — done]** INVENTORY-membership delta: extend the per-object
   shadow with an order-independent fingerprint of the top-level inventory
   (pid + quantity + per-item flags), diffed each beat → `OBJECT_DELTA_INVENTORY`.
   Inventory items are `Object*`s NOT in the world tile list (objectFindFirst
   never returns them), so this fingerprint is how add/remove/stack-quantity/equip
   changes become observable. Byte-identical; verified via NarratePresenter
   (give → dude inv=1, drop → inv=0). (a) INTRA-ITEM field changes — **DONE** (see
   step 3). (b) ITEM↔WORLD placement — **DONE**: see step 3.
2c. **[done]** `worldDelta` game-time: track gLastGameTime, emit
   `worldDelta(WORLD_DELTA_GAMETIME)` per beat on change (gvars/mvars server-only v1).
3. **[done]** `mapTransition` (hooked at the universal `mapLoad` choke, map.cc:628 —
   NOT mapHandleTransition, which worldmap arrivals bypass), combat-control events
   (`combatEnter/combatExit/turnStart/attackResult`, hooked in combat.cc), and the
   ITEM↔WORLD lifecycle `objectConnected`/`objectDisconnected` (hooked at `_obj_connect`/
   `_obj_disconnect`, §4). Each byte-identical, verified-firing via NarratePresenter and
   regression-locked by a narrate golden (run_golden_narrate.sh). ALSO DONE: INTRA-ITEM
   field changes — the per-item inventory hash (`objectInventoryHash`, object_delta.cc)
   folds the item data-union ints (weapon `ammoQuantity`/`ammoTypePid`; single-int
   variants alias `ammo.quantity`/`misc.charges`/`key.keyCode` at offset 0), guarded on
   `OBJ_TYPE_ITEM`. Rides `OBJECT_DELTA_INVENTORY` (no new bit). Catches a wielded weapon
   FIRING (ammo down) or charges depleting with NO membership change. Note: **F2 has NO
   item condition/durability field** — the union is exhaustively weapon/ammo/misc/key, so
   there is nothing further to hash here. Regression-locked by the `arvillag_gunfight`
   narrate golden (A/B-verified: without the fold, the dude's death-beat delta loses its
   `inv=` bit — the weapon stays wielded on death, so the bit is ammo-driven; the A/B is
   the real oracle, the golden pins determinism).
   STILL DEFERRED (optional, own commit): a semantic DEATH delta if the animated
   client-death path (_show_death) ever needs distinguishing from critterKill — but on
   the SERVER path critterKill is the sole finalizer and objectDelta already carries the
   full corpse state (hp=0 + DAM_DEAD + FLAT|NO_BLOCK flags + death fid), so this is
   REDUNDANT for v1 and only earns its place once a thin client renders its own death
   animation (a presentation cue like attackResult, post-v1). ⇒ the P5-B state-delta
   layer is COMPLETE; next is P5-C transport.
4. Refactor note for the serializer (P5-C): `floatText(Object*)`/`sfxPlayAt(Object*)`
   **must serialize owner by `obj->id`, not pointer.** Drop the client-local
   presenter methods from the wire (`worldInvalidate*`/`cursor*`/`scroll*`/
   `mouseObjects*`, H-31) — they exist only as a v1 mechanical mirror.
5. NetworkPresenter (P5-C) overrides the methods, adds the load-window guard (§5)
   and per-client sequenced event buffer; the join snapshot reuses the SAVE
   pipeline (state_dump is partial — one-level inventory, no map geometry).

--------------------------------------------------------------------------------
## 7. P5-C, slice 1 — the event CONSUMER as a file replayer (DONE 2026-07-13)

The consumer is built as a FILE REPLAYER first, before any socket: the
NarratePresenter is already the first serializer (event → text line) and a narrate
capture on disk is a serialized event log, so the first consumer is just read-back
+ reconstruct + validate. When the socket lands (later slice) the file source is
swapped for a socket; the reconstruction logic is reused. This de-risks the LOGICAL
protocol independently of wire encoding and networking.

- **Tool:** `tools/replay.py` (parser + reconstructor + validator). **Gate:**
  `tests/golden/run_golden_replay.sh` (walk/combat/cdamage/itemworld) — runs each
  case under F2_NARRATE capturing the FULL lifecycle+move stream AND the
  authoritative `state_dump` in ONE run, then replays and checks every tracked
  object's reconstructed on-map position against the dump. Needs no checked-in
  golden: the assertion is self-consistency (stream reconstructs the same run's
  authority), which IS the protocol-completeness property and is deterministic.
  Standalone (like the narrate gate), not wired into `check.sh`.
- **Result:** the STATE-DELTA protocol reconstructs on-map position for every
  tracked object across all four cases (walk 12, combat 11, cdamage 24, itemworld
  25 matched; 0 mismatched). Position is seeded from the first event that reveals
  it (spawn tile / connect / a move's fromTile) and updated by move/connect/
  disconnect/destroy.

### Findings this slice confirmed (feed them into the later P5-C slices)
- **`obj->id` collision is SEVERE and load-bearing** (§2 identity risk, now
  quantified): an arvillag dump has **1837 object lines but only 871 distinct ids**
  (~53% collision). Reconstruction-by-id is impossible for colliding objects; the
  replayer disambiguates with **pid** (spawn/connect/destroy carry it) and, for
  move-only objects, id-set membership. ⇒ **P5-C MUST mint a truly-unique server id**
  (a monotonic registry or (map,load-index) tuple); this is a prerequisite for any
  multi-object client, not a nicety.
- **The join SNAPSHOT is mandatory, not optional.** ~780–1465 dump objects per case
  are UNTRACKED by the stream: the initial map loads BEFORE the narrate presenter
  installs, and pre-placed objects/inventories load silently via `_obj_load_obj`
  (no spawn — the §4 load finding). The event stream is a DELTA channel; it can only
  reconstruct from a baseline. So the client cannot start from the stream alone.
- **NO_SAVE transients exist in the stream but not the dump** (e.g. a gore critter
  "Loser", pid 0x01000036, spawns+moves to a corner tile, never destroyed, absent
  from the dump under its pid). The dump skips OBJECT_NO_SAVE; the consumer must
  tolerate stream objects with no dump counterpart (pid-scoped classification), and
  P5-C should decide which NO_SAVE objects are wire-relevant at all.
- **Replay is INSTANT-only today:** the server stream has no presentation timing
  (InstantAnimationScheduler collapses it headless). Position/path is recoverable
  (per-hop `move` events, one per tile — the pinned single-hop meaning, §2); the DURATION + pure cues
  (projectile/swing) are a separate PRESENTATION channel the client derives from its
  own art (cadence design). State-being-instant is correct; the presentation channel
  is the next replay concern, separable from this state stream.

## 7b. Slices 2–4 (2026-07-14): unique id, load guard, join BASELINE

The three findings above are now addressed. Slices 2–3 (unique id + load guard)
landed as cb54952; slice 4 (join snapshot) was RE-SCOPED (see below).

- **Unique server id — DONE (cb54952).** `Object::netId`, minted under the server
  loop and AUTHORITATIVELY (re)assigned by a deterministic `objectAssignAllNetIds()`
  walk called from `objectDeltaReset()` (map-change rebaseline only) ⇒ netids are
  STABLE within a map, unique (arvillag: 1837 objects → 1837 distinct netids). This
  KILLS the ~53% `obj->id` collision. `tools/replay.py` and the `state_dump` are now
  keyed by netid; pid is retained only to classify NO_SAVE transients vs dumped scenery.
- **Load-window guard — DONE (cb54952).** `mapLoad` (map.cc) is bracketed
  `presenterSetEmissionsSuppressed(true .. false)`; the NarratePresenter lifecycle
  overrides early-return while suppressed, so the map-enter spawn/disconn/destroy flood
  (which straddles the `mapTransition` emit on both sides — §5) stays off the stream.
  `mapTransition` itself is NOT suppressed (it crosses the guard) → the consumer still
  gets "drop your world, expect a fresh baseline." The `arvillag_entermap` narrate
  golden now PROVES this: only the `maptrans map=6` line and ambient/post-load deltas
  survive; the ~100-line map-enter flood is gone. ⚠ any future NetworkPresenter MUST
  also honour `presenterEmissionsSuppressed()` in its lifecycle overrides.
- **Join BASELINE (text) — DONE (this slice).** The replayer's finding #2 (UNTRACKED
  ~780–1465 objects) is closed by a per-object TEXT baseline emitted at `serverInstall`
  (server_loop.cc), after the world is loaded + netids assigned, before beat 0 — via a
  new `Presenter::snapshotObject(Object*)` (base no-op → byte-identical; NarratePresenter
  prints a `snapshot` line). The emitted set MIRRORS the `state_dump` exactly (the
  objectFindFirst/Next tile walk skipping OBJECT_NO_SAVE, plus the dude explicitly). The
  replay gate's `snapshot` channel feeds `tools/replay.py`, which seeds its world before
  applying events ⇒ UNTRACKED = 0 and `matched` = the whole on-map set; the gate is
  STRONGER (a lost move on a baseline object is now a `mismatched`, not silent-untracked).
  This is the LOGICAL baseline the file replayer needs.
- **Binary network snapshot — DEFERRED to the socket/NetworkPresenter slice, by design.**
  The join baseline for a real joining CLIENT (vs the file replayer) is a SEPARATE
  artifact and is NOT built until there is a client to load it. When it lands it will
  REUSE THE SAVE PIPELINE — `_map_save_file(stream)` (map.cc: header + gvars + lvars +
  squares + `scriptSaveAll` + `objectSaveAll`) written by the server, loaded by the
  client via `mapLoad(stream)` — NOT a hand-rolled serializer. Rationale: a joining
  client has the full game assets, so the server streams DYNAMIC state over
  client-static geometry, and `mapLoad` already IS "become fully present on a map from
  a stream" (it reloads scripts before objects, regenerates NO_SAVE objects via the
  map-enter proc, and repoints gDude — all things a partial serializer must otherwise
  re-derive). Gemini's 677e7b1 hand-rolled BINARY draft (`serverSaveJoinSnapshot`/
  `clientLoadJoinSnapshot`) was DELETED as dead code (zero callers/tests) and unsound:
  its `clientLoadJoinSnapshot` called `_obj_remove_all` (which wipes the script table
  via `_scr_remove_all`) then `objectLoadAll` WITHOUT a preceding `scriptLoadAll`, so
  every object's `sid` would be nulled to -1; it also read `mapIndex` without loading
  the map and left `gMapGlobalVarsLength` stale on malloc-fail. The correct pipeline
  above avoids all three by construction.

## 7c. Slice 5 (2026-07-14): the f2_server core-only CMake target

The transport work needs a place to live: a binary that links the simulation
core WITHOUT the client. This slice establishes that target as a LINK-BOUNDARY
MILESTONE and, in doing so, measures the true client-severance surface.

- **`f2_server` = `f2_core` (as a STATIC archive) + `server_main.cc` +
  `server_stubs.cc`; NO `f2_client`, NO SDL.** CMakeLists builds
  `f2_core_archive` from `$<TARGET_OBJECTS:f2_core>` (reusing the already-compiled
  core objects, no recompile) so the linker pulls only the object files reachable
  from the server entry points. `server_main.cc` references
  `serverServe`/`serverRun`/`serverTick`/`commandDispatch` to pull the genuine
  server codepath into the closure, then prints a banner and exits — it does NOT
  boot the asset/SDL-entangled engine.
- **The severance surface is BROAD, not "small".** The P5-A note ("only
  pipboy/animation/input remain") was true for `command.cc` in isolation; the
  WHOLE core's server codepath references **375 client symbols across ~45
  client files** (window 55, dialog/game_dialog 53, animation 34, art 22,
  sound 26, game_mouse 16, interface, movie, render, endgame, character_editor…).
  Crucially the INFRASTRUCTURE the server genuinely needs — timing
  (`getTicks`/`_get_bk_time`/`tickers*`), the background pump (`_process_bk`),
  file/string I/O (`compat_*`), `debugPrint`, and the movement/animation path
  (`reg_anim_*`/`animationRegister*`/`_object_animate`) — is itself SDL-coupled
  (input.cc, platform_compat.cc, debug.cc, animation.cc all `#include <SDL.h>`).
  So a fully-runnable standalone server is a multi-slice de-SDL effort, NOT a
  command.cc fix.
- **`src/server_stubs.cc` IS the enumerated severance surface.** 355 free-function
  stubs (signatures lifted verbatim from the declaring client headers, body =
  a `[[noreturn]] serverStubAbort`), 14 data-symbol placeholders, `FrmImage`
  members, and 2 global-namespace `sfall_kb` helpers. Each stub is a client
  dependency the core has not yet been decoupled from; replacing them with
  de-SDL'd implementations (infrastructure first, presentation never) is the
  incremental road to a runnable server. A mis-routed call aborts loudly.
- **Verified core-only.** `ldd`/`nm -u` show zero SDL — every SDL reference lives
  in client `.cc` files, which the stubs replace, so no SDL enters the binary.
  New guard `scripts/check_server_core_only.sh` (wired into `check.sh`)
  regression-locks this. Purely additive: legacy 14/14 byte-identical, server
  27/27, replay 4/4, narrate 7/7 all unchanged.
- **NEXT:** socket accept/framing + client registry + `NetworkPresenter` +
  per-client sequenced buffer. The binary join snapshot (§7b) lands there, with a
  real client to load it — and each de-stubbed capability (starting with the
  timing/pump/file infrastructure) moves the server from "links" toward "runs".

## 7d. STEP 2 (2026-07-15): the binary wire — NetworkPresenter

`src/presenter_network.{h,cc}` (f2_core) is the third `Presenter` subclass and the
first real consumer of the widened state seam. `F2_NETSTREAM=<path>` installs it
(`serverInstall`, server_loop.cc), beside the existing `F2_NARRATE` branch.

### Wire format v1 (little-endian, byte-packed)
```
stream: "F2NS" | u16 version=1
frame:  u32 seq | u32 simTs | u32 payloadLen | u16 eventCount | payload
event:  u8 type | u8 flags | u16 len | payload[len]
```
- **seq is per-FRAME, not per-event.** The frame is the atomic transport unit;
  events inherit `(seq, simTs)` from its header. A per-event seq would be redundant
  state that must never disagree with frame order. §1's gap rule survives intact: a
  missed frame seq = every event in it missed = exactly the "snapshot + resync" case.
  `payloadLen` makes a frame length-prefixed, so there is no partial-frame case.
- **The frame boundary is the BEAT** (`Presenter::beatEnd`, new) — the sim's
  resolution quantum, and already what `objectDeltaScan` batches on, so no finer
  boundary exists. It fires AFTER `invariantsCheck`, so an aborting beat never
  flushes a partial frame. Frames are SPARSE: an idle beat emits nothing (seq counts
  frames, not beats).
- **Forward compat:** every event is length-prefixed → unknown types skip whole.
- **Mid-stream join (STEP 5, implemented):** one encoder, one byte stream — there
  is NO per-client framing. A late joiner receives the `"F2NS"|version` preamble
  from the TRANSPORT at accept (server_net.cc `acceptPending`, bytes must match
  the encoder's `begin()`), then taps the shared flow at the next frame boundary.
  The client therefore SEEDS its expected seq from the first frame it receives
  (client_net.cc) — gap detection holds from there; only the absolute start is
  relaxed. A join obligates a rebaseline BROADCAST that beat (fresh netId walk +
  blob + baseline to ALL clients — CLIENT_JOIN_DESIGN.md C.4): the walk resets
  the whole-stream netId domain, so a targeted blob cannot exist without netIds
  on the wire (rejected sidecar). Existing clients pay a world-reload hitch per
  join. Pre-blob events at a joiner (the tail of its accept beat) are dropped by
  the decoder (`!loaded` gate) — the blob that follows carries that state.
- **STRINGS ARE NOT UTF-8.** u16 length + RAW bytes. Fallout `.msg` text is
  codepage-encoded; console/floatText/errorBox carry high-ASCII verbatim. (Same
  reason the gates need `grep -a` and replay.py uses `errors="replace"`. The old
  "0x85 NEL terminator" claim was FALSE — corrected by whole-tree grep.)
- **`move.durMs` HAS a producer since the stepped-walk engine** (F2_SERVER_SMOOTH_WALK,
  server_anim.cc): stepped hops stamp their sim-ms-per-tile pace (walk 200 / run 100);
  everything else (teleports, scripted placement, within-beat sync moves, every
  golden path) stays 0 = snap. The client uses durMs>0 as its animate-vs-snap
  discriminator (tile adjacency alone would falsely animate a 1-hex scripted
  teleport) and slaves per-tile anim DURATION to it — art fps selects frames only
  (§7). **`turnStart.deadlineMs` HAS a producer since the resumable-combat session
  (P2, `F2_SERVER_RESUMABLE_COMBAT`, combat.cc):** a session-driven PLAYER turn
  stamps the per-action idle-timer budget (`F2_SERVER_TURN_IDLE_MS`, default 30000)
  — the §2 Turn barrier's timer, now real. AI turns and every gate-off/legacy path
  keep 0 (no timer). The old "reserved always-0 slot; do not fix that zero" note
  referred to faking a producer; under the resumable gate we ARE the producer, so a
  nonzero deadline on a player `turnStart` is now legitimate wire data.

### Dropped from the wire (verified by whole-tree call-site grep, 2026-07-15)
- `ambientSoundLoad` — map.cc:628 is its SOLE core call site and every arg is a
  compile-time constant `("wind2", 12, 13, 16)`: **zero information**. The client
  re-derives ambience from `mapTransition`. Dropping it also dissolves a causal-order
  wart (it fires at the TOP of mapLoad, BEFORE the mapTransition emit, both inside the
  suppression window). Reordering the core call is forbidden — it would break
  ClientPresenter's legacy call order, which the goldens pin.
- `musicPlayLevel` — **ZERO core call sites**; client-driven, can never fire from the
  server loop. Overriding it would create a dead wire type. (`musicStop` DOES fire,
  worldmap.cc:4373, and is carried.)
- The ~19 client-local chrome virtuals (§6.4 / H-31).

### `attackResult` stays, tagged PRESENTATION
All STATE it implies rides `objectDelta`, so it is skippable under lag — but it is the
ONLY causal combat cue on the seam (§2's `playAnim` has no virtual), so without it a
thin client cannot render an attack at all. Object refs serialize by netId (§6.4).

### ⚠ SUPPRESSION IS ADVISORY — every override must self-gate
Nothing in `presenter.cc` or the base class consults `presenterEmissionsSuppressed()`;
the sim emits regardless. NetworkPresenter gates EVERY override except `mapTransition`
(the deliberate exemption: the consumer must hear "drop your world" even though the
map-enter flood is withheld). This is broader than NarratePresenter's 8 — the
PRESENTATION cues gate too, because mapLoad runs MAP_ENTER scripts (map.cc:786/832)
inside the window and those emit console/float text.

### ⚠ netId RECYCLING — a mid-run map change USED TO CORRUPT THE WORLD (now FIXED)
`objectAssignAllNetIds()` (object.cc:4489) opens with `objectSetNextNetId(1)`: a map
transition RESETS the counter and reassigns every object from 1, driven by
`objectDeltaReset()` ← `objectDeltaScan()`'s silent map-change rebaseline. So every
netId a consumer already held came to mean a DIFFERENT object. `mapTransition` said
"drop your world" and NOTHING delivered the replacement (the `snapshotObject` walk ran
only at `serverInstall`). The stream did not go incomplete — **it lied**.

**AND THE INDEX IS NOT THE SIGNAL.** Keying the rebaseline on `mapGetCurrentMap()`
misses a **same-index reload** (leave a map and come back): mapLoad tears down and
rebuilds every object while the index never changes, and the rebuilt objects carry
`netId 0` (objectAllocate zeroes it, object.cc:3444) because only
`objectAssignAllNetIds()` ever assigns one — so EVERY object collides on 0. Measured
on `entermap:6,entermap:6`: `matched=2 not_in_dump=2864`, a garbage world. This also
left `objectDeltaScan`'s shadow keyed on freed `Object*` (a pre-existing latent bug,
not introduced by the wire).
⇒ **`mapGetLoadGeneration()` (map.h/map.cc) is THE map-change signal** — a monotonic
counter bumped once per `mapLoad`. Both `objectDeltaScan` (object_delta.cc) and
`serverEmitBaselineIfMapChanged` (server_loop.cc) now key on it. Never use the index.

**FIX:** `serverEmitBaseline()` (server_loop.cc) is now shared by BOTH the join
baseline and a post-transition re-emission (`serverEmitBaselineIfMapChanged`, called
from `serverTick` after `invariantsCheck`) — "snapshot is ONE mechanism" (§1). The
walk is bracketed by new `snapshotBegin(mapIndex, elevation)` / `snapshotEnd()`
delimiters so a consumer can seed atomically; `snapshotBegin` means **REPLACE your
world, do not merge** (merging leaves stale phantoms colliding with recycled netids).
The baseline flushes as its own frame. **seq stays MONOTONIC across a transition** —
resetting it would destroy the §1 gap-detection property that resync depends on.

**Gate:** `tests/golden/run_golden_netstream.sh` (6 cases) validates the binary
reconstruction against `state_dump`. Its `xmap` case (arvillag→denbus1 via
`entermap:6`) is the FIRST gate in the suite to cross a map — nothing else did, and
replay.py's docstring openly depended on that. A/B-measured: with the re-baseline
suppressed it reports `matched=7 mismatched=134 not_in_dump=1704` (UNTRACKED=1117) and
FAILS; with it, `matched=2863 mismatched=0 not_in_dump=2` UNTRACKED=0. The `xmap_same`
case (`entermap:6,entermap:6`) pins the same-index re-entry above.

⚠ **replay.py's EXIT CODE does not catch either hole** — `ok = not mismatches and
matches`, so the same-index garbage world (2 matches, 0 mismatches) printed
**"REPLAY OK"**. The PINNED PROFILE is what has teeth here, not rc. Do not weaken the
pins to "rc==0".

### The oracle is the DUMP, not the other serialization
"Binary reconstructs the same world as narrate" is WEAK — it proves only that the two
encoders agree, so a SHARED omission passes silently (translation validation, not
correctness). Each case validates against `state_dump`, which reads live sim state
INDEPENDENTLY of the presenter seam. Profiles matching run_golden_replay.sh's is a
welcome cross-check, not the claim.

### KNOWN GAP (banked, wire-visible, invisible to every gate)
`OBJECT_DELTA_INVENTORY` is a CHANGE-DETECTION bit, not a payload: the shadow keeps
only a 32-bit fingerprint (`objectInventoryHash`), so the bit means "re-send the
list" and NetworkPresenter re-serializes the whole top-level inventory. But the
fingerprint is **TOP-LEVEL ONLY** — an item nested in a carried container is never
walked, so a container's contents can change with NO delta bit raised. Consistent with
the snapshot oracle (state_dump is partial the same way) → no join-vs-stream
divergence and not a new hole, but no gate can see it: both serializations share the
blind spot. Closing it needs a RECURSIVE fingerprint, not a serializer change.

## 8. C2S — upstream control (STEP 6, the first controllable client)

Everything above is S2C (server→client). STEP 6 adds the upstream direction: a wire
client can drive the authoritative actor.

**Framing (v1).** Upstream is plain **newline-delimited text lines** — `verb [arg]
[arg2]\n` — sent on the **same TCP socket** the client already reads the S2C stream
from (`ClientConnection::sendLine`). This is deliberately the v0 string vocabulary
`commandDispatch` already speaks. A typed **binary command frame** is the banked
upgrade once the protocol is frozen (REWRITE_PLAN 3.4); **per-client acks** are
banked on per-client framing (there is no per-client S2C framing today — one encoder,
one broadcast byte stream, §7b/STEP 5), so v1 has no upstream ack: the client learns
its intent landed by seeing the resulting MOVE/delta ride the S2C stream back out.

**Trust boundary — wire clients are RESTRICTED.** Viewer-wire lines do NOT reach the
unrestricted `commandDispatch`; they are routed to the STEP-6 **control plane**
(`server_control.cc`, `serverControlLine`), a separate dispatcher speaking a tiny
allow-listed vocabulary. (Keeping the full debug vocabulary off the wire also keeps
the golden-shared `commandDispatch` free of any network trust concern.) The dedicated
debug/admin channel (`F2_SERVER_CMD`, telnet/nc) stays **unrestricted** — it is a
local operator tool, not a wire client.

**Per-client identity.** `SocketByteSink` stamps each connection with a stable,
monotonically-increasing `sessionId` (never reused) carried on every inbound line.
Authority is bound to a *session*, not to `gDude` (the mp-actor principle): the
executor is actor-parameterized and merely resolves to `gDude` for v1.

**Verbs (v1).**
- `claim` — grant control to the sending session if unclaimed (idempotent for the
  holder). Exactly one live claimant at a time. Released automatically when its
  socket disconnects (validated each drain against the sink's live session set).
- `mv <tile> <run>` — move the claimed actor to an **absolute** tile; `run != 0`
  runs. Honored only from the current claimant, only **out of combat**, and only for
  a tile in `[0, gHexGridSize)` (a client's `tileFromScreenXY` can yield `-1` out of
  bounds → rejected). Executes through the same public `animationRegister*` entry
  points as `walkto`, so the smooth-walk engine drives it unchanged.

A cheap per-session-per-beat flood cap (32 lines) bounds a misbehaving client.

**Gate:** `scripts/check_control.sh` (check.sh gate 8) — a raw wire client sends
`claim` + `mv`, and the dude's authoritative MOVE must appear on the S2C stream.
