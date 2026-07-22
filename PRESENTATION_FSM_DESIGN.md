# PRESENTATION_FSM_DESIGN.md — one per-critter presentation state machine for the viewer

STATUS: IMPLEMENTED (src/client_present.{h,cc}). Steps 0-1 (pose helper + fid route-don't-
kill), step 3 (shipped as **present-tile rebucketing**, a DEVIATION from the written
derived-offset below — see §6 step 3 revision), and step 2 (registry MERGE) all landed:
2a co-located the three modules (commit 230f6ef), 2b fused them into one netId-keyed
`PresEntry` table with three orthogonal concerns (glide / replay / door) and deleted the
cross-module `replaySuspended` coupling in favour of the derived predicate
`glideSuspended = replay==Active && !posed` (commit 7545601, adversarial-reviewed + live-
verified behavior-neutral).

⚠ SECTIONS SUPERSEDED BY THE SHIPPED CODE (do not implement from these — read the code):
§2's "the FSM never touches obj->tile" is false (present-tile rebucketing writes it); §3.2's
PresEntry is the rejected derived-offset model (shipped keeps appliedX/Y + the offset
tripwire as LOAD-BEARING); §3.3's transition table predates rebucketing + the snap-forward
asymmetry + hit-then-flee; §5.6's "delete all interlock flags into one linear enum" —
shipped keeps posed/parkedFid/pending buckets as per-concern sub-state (a linear enum can't
model glide+replay coexistence) and deletes ONLY replaySuspended+its setter. TRUST §1
(problem), §4.3 (pose helper), §5.1 (API names), §5.2 (queue stays in decoder), §6 recipes,
§7. Companion to COMBAT_CLIENT_DESIGN.md §3 and MP_PROTOCOL.md §1/§2 (snap-forward +
durMs-discriminator rules stay pinned). Banked follow-ups (§6 step 4 + review): derived
offsets, fold pendingRot into the stand fid, unify the two pending buckets, unify flags
routing, drain-grace, clear the door concern in presForgetObject (latent UAF). Uncertainties
flagged inline with **[UNSURE]**.

--------------------------------------------------------------------------------
## 0. Executive summary

Collapse the three client presentation registries — `gWalks` (client_anim.cc:126),
`gDeferred` (client_combat_anim.cc:66), `gDoors`/`gDoorTargetFrame`
(client_door_anim.cc:26-27) — into **one owner module with one entry per presented
object**, a five-state machine: **IDLE, PARKED, GLIDE, REPLAY, DOOR_SLIDE**.

The one arbitration rule: **sim state always writes through at decode; render-channel
writes (fid / frame / rotation) write through only when the object is IDLE — otherwise
the decoder ROUTES them into the entry's `pendingPose`, which is committed exactly once
when the entry retires.** Every retirement applies: settled fid, `objectSetFrame(0)`,
final rotation, offset removal. Tripwires (polling `obj->fid` for foreign writes and
killing the glide) are replaced by routing; what survives of them is a debug-only
logger and the existing time-based self-heal backstops.

This makes the three concrete failure cases non-events by construction (§4), deletes
the `posed`/`parkedFid`/`replaySuspended`/`heldHops` interlock machinery, and gives
every future authority source (spawn pop-in presentation, put-away/reload/swap
replays, smooth knockback) a transition to map onto instead of a fourth registry.

First de-risking step: convert the walk tripwire from *kill* to *route + log* for the
fid channel only (§6 step 1) — it is simultaneously the smallest slice of the design
and the instrument that names the still-unpinned client-local source of failure case 1.

--------------------------------------------------------------------------------
## 1. The problem, grounded

### 1.1 Three registries, each with private pose/retract/race logic

| registry | file | owns while active | its private race machinery |
|---|---|---|---|
| `gWalks` (out-of-combat + in-combat glides) | client_anim.cc:126 | fid, frame, rotation, pixel offset (`obj->x/y`) | fid/offset **tripwire** (client_anim.cc:516-527) kills the walk on any foreign write; `posed`/`parkedFid` (client_anim.cc:61-66) split the tripwire baseline for held walks; `replaySuspended` (client_anim.cc:90-97) turns the tripwire *off* while a weapon-draw replay legitimately writes the same channels; `heldHops` (client_anim.cc:71-78) embeds a second queue inside the walk |
| `gDeferred` (attack / take-out replay + deferred final state) | client_combat_anim.cc:66 | fid, frame, rotation, flags — indirectly, via `reg_anim` sequences it registers | reserved-vs-active lifetimes (client_combat_anim.cc:43-52); its own frame-reset knowledge (client_combat_anim.cc:114-122); it must reach *into* gWalks every frame to keep `replaySuspended` asserted (client_combat_anim.cc:372-383) |
| `gDoors` | client_door_anim.cc:26 | frame (via reg_anim) | wall-clock cap + terminal-frame fallback snap (client_door_anim.cc:76-92) |

client_net.cc pokes all three from decode: `onMove` (client_net.cc:607-663) calls
`clientAnimOnMove` + `clientCombatAnimNotifyReposition`; `onObjectDelta`
(client_net.cc:750-771) branches per-field between `clientCombatAnimDeferDelta`,
`clientAnimDeferRotation`, and direct write-through; `onCombatEnter`
(client_net.cc:974) calls `clientAnimStandDownAll`; destroy/disconnect call two
different forget hooks (client_net.cc:671-672, 697-699). The presentation pump
(client_net.cc:252-361) additionally coordinates the three registries by *polling*
their activity predicates.

### 1.2 Why it whack-a-moles

The load-bearing invariant — "the store is never wrong, only the pixels are late"
(client_combat_anim.h:24-32) — is enforced *negatively*: each presenter watches
`obj->fid`/`obj->x/y` for writes it didn't make and self-destructs (snap) when it sees
one. That means **every legitimate new writer of a render channel is indistinguishable
from an authority override** until someone hand-adds a suspension flag:

- The weapon-draw replay writing fid over a held approach walk → `replaySuspended`
  (the equip-teleport bug class, COMBAT_CLIENT_DESIGN.md §6 "WEAPON draw quirks").
- The held walk wearing its stand fid instead of its walk fid → `parkedFid` split
  baseline (client_anim.cc:61-66).
- An authoritative rotation landing mid-glide → the `clientAnimDeferRotation` side
  channel (client_anim.cc:428-440, client_net.cc:767).
- The next one — whatever streaming-events-fidelity or reload/put-away adds — breaks
  some presenter's assumption again. COMBAT_CLIENT_DESIGN.md §6 already banks "RANDOM
  COMBAT-GLIDE TELEPORTS" with the tripwire as a listed suspect (lines 510-514).

### 1.3 Measured facts this design leans on

- The server does **not** emit a fid delta for a plain moving critter:
  `serverAnimStepOnce` (server_anim.cc:311-338) sets only rotation + location
  (objectSetRotation at :315, objectSetLocation at :337 inside the
  `presenterSetNextMoveDurationMs` bracket). Verified live this session: a fid-diff
  trace fired 0 times over a smooth walk. So the mid-glide STAND fid of failure
  case 1 is client-local, or a scenario-specific authoritative delta (the server CAN
  emit fids: `animationRegisterSetFid`, server_anim.cc:595-608, and the in-combat
  wield arms the fid as state). **[UNSURE — root cause not pinned; §6 step 1 is the
  instrument.]** A code-visible client-local candidate: the drain/retract/recreate
  churn of §4.1(b).
- `objectSetFid` (object.cc:1412) never resets `obj->frame`
  ([[frame-index-render-gotcha]]); a stale high frame on a 1-frame corpse art renders
  NOTHING. Today two modules independently know this (client_anim.cc:196-198,
  client_combat_anim.cc:114-122).
- All three registries are viewer-only (enabled flags set exclusively in
  `mainClientViewer`, main.cc:806-814), so this whole design is golden-invisible by
  construction — headless and every gate are untouched.

--------------------------------------------------------------------------------
## 2. Authority arbitration — the core rule

Two planes, cleanly split:

1. **Sim state** (tile/elevation via `objectSetLocation`, hp/ap/radiation/poison/
   results, inventory) — applied at decode, always, immediately. Never held, never
   routed. Identical to today (client_net.cc:620, 826-830). The FSM never touches it.

2. **Render channels** (fid, frame, rotation, pixel offset; flags see §5.4) — owned
   by exactly one presenter state per object at any moment:

   > **RULE: a render-channel write from the wire applies directly iff the object is
   > IDLE (no FSM entry). If an entry exists, the write lands in the entry's
   > `pendingPose` (last-writer-wins per channel) and is committed exactly once when
   > the entry retires. Every retirement commits through one helper that sets fid,
   > `objectSetFrame(0)`, rotation, and removes any presentation offset.**

Consequences:

- **No sniffing.** Today's tripwire exists to *detect* foreign writes after the fact.
  Under the rule there are no foreign writes to detect: the decoder — the only
  authoritative writer — routes, and the FSM's own states are the only other writers.
  The remaining "unknown writer" risk is covered by a debug-only checker (§5.5), not
  by a kill switch.
- **The deferred-final-state HOLD stops being a special combat mechanism** and becomes
  the general case: `clientCombatAnimDeferDelta` (client_combat_anim.cc:314-337) is
  just "route to pendingPose" for objects in REPLAY/PARKED; the same routing now also
  covers GLIDE — which is exactly what failure case 1 needs.
- **Authority still always wins, on the same schedule as today**: pendingPose commits
  at replay completion / glide drain (same moments `resolve()` and `walkRetract` fire
  now), and every hard-authority event (snap MOVE, elevation change, connect,
  destroy, rebaseline, combat-enter stand-down, stall/cap backstops) forces immediate
  retirement. "Behind = snap forward, never replay" (client_anim.h:12-14) is
  preserved verbatim.
- **Failure direction stays "play/snap, never freeze"**: all of today's self-heal
  backstops carry over unchanged — pump self-heal release (client_net.cc:358-360),
  held-glide stall snap (client_anim.cc:576-579), reserve stall (client_combat_anim.cc
  :392-398), replay/door wall-clock caps (client_combat_anim.cc:62, client_door_anim.cc:23).

Commit points are state-dependent and explicit (this is the one subtle part — §3.3):
PARKED deliberately does NOT commit on entry (that's what "reserve" means: the corpse
fid must not land before the fall animation), while REPLAY→anything and final
retirement DO commit.

--------------------------------------------------------------------------------
## 3. State model

### 3.1 States and what each owns

One table, keyed by **netId** with a cached `Object*` (rationale §5.3). "Owns" = this
state's advance code is the only writer of that channel this frame.

| state | meaning | fid | frame | rotation | offset | advanced by |
|---|---|---|---|---|---|---|
| **IDLE** | no entry; settled | wire writes through | wire (via pose helper → frame 0) | wire | none (obj->x/y = 0) | — |
| **PARKED** | entry exists, nothing animating: held glide awaiting release, or reserved replay participant awaiting its pump slot | frozen (whatever it wore at entry) | frozen | frozen | anchored at `presLocation` (§3.2) | FSM (anchor upkeep only) |
| **GLIDE** | playable walk/run hops draining | FSM: walk/run fid (`animFid`, client_anim.cc:151) | FSM: art-fps frame selection (client_anim.cc:600-603) | FSM: per-hop travel facing (client_anim.cc:546) | FSM: hop interpolation (client_anim.cc:160-183) | FSM per-frame |
| **REPLAY** | a reg_anim sequence in flight (attack, take-out; future: put-away/reload/swap, smooth knockback) | delegated to reg_anim (`_object_animate`) | delegated | delegated | delegated (draw sequences nudge sub-tile offsets) + FSM keeps `presLocation` anchor | `_object_animate` (main.cc:1008); FSM watches `animationIsBusy` + cap |
| **DOOR_SLIDE** | door frame walk (scenery, not critter — same table, same lifecycle) | n/a (doors don't change fid) | delegated to reg_anim; terminal-frame snap on retire (client_door_anim.cc:82-87) | n/a | n/a | same as REPLAY |

Deliberate NON-states, and why:

- **WALK vs RUN**: one GLIDE state; the anim code (ANIM_WALK vs ANIM_RUNNING off the
  hop pace, client_anim.cc:286-292) is a parameter, not a state.
- **KNOCKED / DEAD / COMBAT_POSE**: these are *poses* (fid+flags values), not
  ownership epochs. The machine tracks who owns the channels over time; what the
  settled pose *is* comes from authority (pendingPose) or synthesis (stand-down).
  Encoding critter semantics as states is how registries proliferate.
- **DOOR_WAIT** (crosser waiting at threshold): not a critter state — it is queue
  ordering. The crosser is PARKED (held glide); the door's kDoor event ahead of the
  crosser's kMoveRelease in the presentation queue (client_net.cc:630-649, 1199-1210)
  is what makes it wait. The global queue decides WHEN; the FSM decides WHO OWNS.
  That seam is kept (§5.2).

### 3.2 Entry contents

```
struct PresEntry {
    int netId; Object* obj;              // cached; invalidated by destroy/reset hooks
    State state;                          // PARKED | GLIDE | REPLAY | DOOR_SLIDE
    // -- presentation position (replaces appliedX/appliedY re-anchoring) --
    std::deque<Hop> hops;                 // pending glide segments (held + playable)
    int playableHops;                     // release budget (inverse of today's heldHops)
    // presLocation = origin of hops.front() + fractional progress; the pixel offset
    // is DERIVED each frame: screen(presLocation) - screen(obj->tile). Idempotent —
    // an authoritative objectSetLocation (which zeroes x/y, object.cc:1235) needs no
    // "re-anchor" bookkeeping (today: client_anim.cc:272-280) and no offset tripwire.
    unsigned int startedAt, readyAt, frameAt; int ticksPerFrame;  // verbatim from Walk
    // -- deferred authority (replaces gDeferred's Deferred + Walk::pendingRot) --
    struct { bool hasFid; int fid; bool hasFlags; unsigned int flags;
             bool hasRot; int rot; } pendingPose;
    unsigned int since;                   // state-entry tick (caps/stall, as today)
    int doorTargetFrame;                  // DOOR_SLIDE fallback snap
};
```

Deleted outright: `posed`, `parkedFid`, `replaySuspended`, `lastPushGen` moves to a
module-global as today (client_anim.cc:130), `heldHops` becomes `playableHops`
(counting released instead of held — same arithmetic, reads positively).

### 3.3 Transition table

Events: wire decode (left column group), pump commands (middle), internal (right).
`commit` = apply pendingPose via the pose helper (settled fid or synthesized
stand-fid at final rotation, frame 0, flags per §5.4) then clear it. `retire` =
commit + drop offset + erase entry (→IDLE).

| from \ event | MOVE durMs>0, immediate | MOVE durMs>0, hold | MOVE snap (durMs≤0 / elev / non-adjacent, client_anim.cc:234-246) | pose delta (fid/rot[/flags]) | reserve (attack/take-out decode) | pump: release N | pump: start replay | anim idle / cap | hops drained | combat-enter stand-down | destroy / reset |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **IDLE** | →GLIDE (pose walk fid now, as client_anim.cc:325-329) | →PARKED (hops queued, sprite untouched) | no-op (state already applied) | write through | →PARKED (empty hops, hold deltas) | n/a | →REPLAY | n/a | n/a | no-op | no-op |
| **PARKED** | append hop, playable | append hop, held | retire (snap) | →pendingPose | no-op (idempotent, as client_combat_anim.cc:84-92) | →GLIDE (pose now, re-clock — today's client_anim.cc:589-596 + 470-473) | →REPLAY (hops kept! the draw-before-approach case) | n/a | n/a | retire | discard+erase |
| **GLIDE** | append hop (burst/lag caps verbatim, client_anim.cc:248-264) | append hop, held | retire (snap) | →pendingPose ← **this is failure case 1's fix** | →PARKED-like hold: keep gliding, start holding deltas (entry gains reserve semantics) | increase playableHops | pump never does this (it waits out playable glides, client_net.cc:291-330); forced path = snap-retire then →REPLAY, as clientAnimCancel in clientCombatAnimPlay today (client_combat_anim.cc:240-247) | n/a | commit → IDLE if no held hops & no reserve; else →PARKED | retire | discard+erase |
| **REPLAY** | knockback-rides-MOVE: commit now + keep/append glide per hold flag (today: clientCombatAnimNotifyReposition, client_combat_anim.cc:339-350 — see §4.4) | same | commit + retire (snap) | →pendingPose | no-op (back-to-back attack keeps held state, client_combat_anim.cc:104-108) | queue for after replay | serialized by pump (never concurrent per object) | **commit**; then →GLIDE if playable hops, →PARKED if held hops/reserve remain, else retire | n/a | commit + retire **[UNSURE — today combat-enter can't overlap a replay; keep an assert]** | animationStop + retire survivors (client_combat_anim.cc:189-208 semantics) |
| **DOOR_SLIDE** | n/a | n/a | n/a | write flags through (door open/closed state rides delta already) | n/a | n/a | n/a | terminal-frame snap + retire | n/a | no-op | discard+erase |

Backstops (unchanged semantics, now per-entry fields instead of per-registry):
- PARKED with held hops or reserve: stall snap against `presProgressTick` after 5 s of
  *total* presentation freeze (client_anim.cc:114-120, client_combat_anim.cc:57-63 —
  the progress-not-wallclock distinction is preserved verbatim).
- REPLAY / DOOR_SLIDE: 2 s wall-clock cap from playback start (client_combat_anim.cc:62,
  client_door_anim.cc:23).
- Pump self-heal releaseAll when queue empty + nothing animating (client_net.cc:358-360).

Commit-point summary (the answer to "HOLD × FSM ownership"):
- **PARKED entry: no commit** — reserve means "pixels lag on purpose".
- **REPLAY exit: always commit** — this is today's `resolve()` (armed fid after a
  draw, corpse SF after a kill), including the frame-0 reset.
- **GLIDE drain to IDLE: commit** — generalizes today's `walkRetract` + deferred-rot
  application (client_anim.cc:550-565), but as ONE code path: final rotation is folded
  into the synthesized stand fid instead of the fragile "retract first, rotate after
  erase" ordering the current code needs (client_anim.cc:556-564).
- **GLIDE drain to PARKED: no commit** — more work is queued; the sprite freezes in
  its current pose at presLocation (this is what makes draw-at-destination and
  multi-segment sequencing composable).

--------------------------------------------------------------------------------
## 4. The three failure cases, resolved by construction

### 4.1 RUN slides in STAND pose / choppy snap (out of combat)

Two mechanisms, both covered:

(a) **Authoritative-fid variant**: any `OBJECT_DELTA_FID` (STAND or otherwise)
arriving while GLIDE is active routes to pendingPose instead of tripping
client_anim.cc:516-527 into a walk-kill+snap. The critter keeps its run fid and its
glide; the delta lands at drain. Non-event, per the rule. (Today the trace at
client_net.cc:754-757 even predicts the kill: "walk ACTIVE — will trip".)

(b) **Client-local churn variant** (code-visible candidate, since the server provably
emits no fid on plain moves — §1.3): RUN hops stamp durMs=100 (client_anim.cc:122-123)
equal to both the server beat and `kStartDelayMs` (client_anim.cc:101), so the hop
queue hovers at depth ~1; any jitter drains it → `walkErase` → `walkRetract` sets
STAND+frame0 (client_anim.cc:195-198) → the next hop builds a fresh walk that waits
another 100 ms parked → visible STAND flash + positional snap, repeating = "choppy
snap", and the offset survives into the STAND-posed park = "sliding in STAND pose".
FSM fix: **drain grace** — GLIDE with an empty hop queue does not retire immediately;
it lingers (posture held, cycle frozen or marking time; propose freeze at the walk
fid's frame, cheapest) for `kDrainGraceMs ≈ 1 server beat + jitter (~150 ms)`. A hop
arriving within the grace resumes seamlessly (re-clock, no re-pose, no re-park delay);
expiry retires normally. This knob is only expressible when one owner holds the whole
lifecycle — today the retract and the re-create live on opposite sides of `walkErase`.
**[UNSURE which variant the live repro is; step 1's route+log instrument decides. The
design fixes both regardless.]**

### 4.2 run → manual combat-enter freezes on ~frame 1

Fixed this session by `clientAnimStandDownAll` (client_anim.cc:379-390,
client_net.cc:964-974) — but as a special-purpose second reset function next to the
bare-clear `clientAnimReset` (client_anim.cc:372-377), i.e. exactly the pattern that
whack-a-moles. In the FSM, combat-enter is the ordinary **retire-all** transition, and
*every* retirement goes through the single commit helper (settled fid + frame 0 +
offset removal). A mid-run critter cannot freeze wearing its run fid because there is
no exit path that skips the helper. The two reset flavors remain — `presReset()`
(discard: the object list itself is dying; today's clientAnimReset +
clientCombatAnimReset + clientDoorAnimReset callers at client_net.cc:496-498, 906-908)
vs `presStandDownAll()` (retire-all: world stays) — but as two documented lifecycle
verbs of one module, not per-registry conventions.

### 4.3 The frame-index gotcha cannot be reintroduced

One pose helper, used by every write path:

```
void presApplyPose(Object* obj, int fid) { objectSetFid(obj, fid, ...); objectSetFrame(obj, 0, ...); }
```

Used by: IDLE write-through in `onObjectDelta`, every commit/retire, GLIDE posing.
Rule (grep-enforceable in review): **no client-side code outside this module and this
helper calls `objectSetFid` on a synced object.** Today the knowledge lives in three
places (client_anim.cc:196-198, client_combat_anim.cc:114-122, and the arm-fid site
client_combat_anim.cc:263-266) and is absent from the IDLE write-through path
(client_net.cc:758 applies a bare `objectSetFid` — safe only because settled deltas
happen to pair with frame-safe fids; the helper closes even that latent hole).
The one deliberate exception: REPLAY delegates fid/frame to reg_anim sequences, which
manage frames internally (`_anim_change_fid` semantics) — that is ownership
delegation, not a bypass.

### 4.4 Everything COMBAT_CLIENT_DESIGN.md already locked in, preserved

- **Weapon-draw before shot, at the destination** (§6 quirks 1-3): reserve at decode →
  PARKED holding the armed-fid delta; pump's draw-vs-move reorder (client_net.cc:303-330)
  unchanged; draw = REPLAY *while the entry still carries held hops* — the whole
  `replaySuspended` dance (client_anim.cc:346-370 + client_combat_anim.cc:372-383,
  cross-module, per-frame) reduces to "REPLAY owns the channels; the hops sit in the
  same entry". Resume-and-re-baseline disappears because there is no tripwire baseline
  to resync.
- **Door-wait-at-threshold**: unchanged — hold decision at decode (client_net.cc:630-631
  incl. `doorPresentationPending`), kDoor ahead of kMoveRelease in the queue, pump
  blocks while DOOR_SLIDE active (client_net.cc:266-274).
- **Knockback-rides-MOVE**: a MOVE on a REPLAY participant commits its held pose
  immediately (today's `clientCombatAnimNotifyReposition`, client_net.cc:654) and the
  hop then glides or snaps per durMs. Same event, now a labeled transition
  (REPLAY×MOVE) instead of a cross-registry callback.
- **Deferred-final-state hold**: is now the pendingPose mechanism itself (§2); the
  reserved/active split maps to PARKED/REPLAY; same-beat-leak closure (reserve at
  decode, client_net.cc:1144-1154) unchanged.
- **Held-glide combat sequencing** (§3.d): hold flag at decode, kMoveRelease
  coalescing, burst-vs-trickle caps, release re-clock, playable-vs-held pump
  predicates — all verbatim; only the storage moves (heldHops → playableHops).
- **Serialized attacks, one-at-a-time** and the throw-skip (client_combat_anim.cc:
  223-233), arm-fid-before-fire (client_combat_anim.cc:249-266), forgetObject's
  animationStop+resolve-survivors (client_combat_anim.cc:189-208): kept as REPLAY
  entry/exit logic, code moved not rewritten.

--------------------------------------------------------------------------------
## 5. Seam / API

### 5.1 Proposed module

`src/client_present.{h,cc}` (f2_client; name fits the banked IDEAS.md folder
restructure `client/{ui,hud,sfx,…}` → `client/present/` later). Built on the same
public primitives the current modules use — `animFid`/`buildFid`, `objectSetFid`,
`objectSetFrame`, `objectSetNextFrame`, `objectSetRotation`, `_obj_offset`,
`objectSetLocation` (read-only awareness), `ANIM_*`, `artExists`/`artLock`/
`artGetFramesPerSecond`, `reg_anim_begin/end` + `animationRegister*` +
`animationIsBusy`/`animationStop`, `_action_attack`, `tileToScreenXY`/
`tileGetRotationTo`/`tileDistanceBetween`, `getTicks`. Nothing new from core.

```
// lifecycle
void presSetEnabled(bool);                       // viewer-only, as all three today
void presReset();                                // discard-all (object list dying)
void presStandDownAll();                         // retire-all (combat enter, etc.)
void presForgetObject(Object*);                  // destroy/disconnect (both hooks merge)

// decode ingress — the ONLY authoritative render-channel writers
void presOnMove(Object*, from, to, fromElev, toElev, durMs, bool hold);
bool presRoutePoseDelta(Object*, hasFid, fid, hasFlags, flags, hasRot, rot);
     // true = routed to pendingPose (entry exists); false = caller write-through
     // (numeric fields NEVER pass through here — decoder applies them directly)
void presReserve(Object*);                       // attack/take-out participant at decode

// pump commands (ordering stays in client_net.cc — §5.2)
void presRelease(Object*, int hops);  void presReleaseAll();
void presPlayAttack(Attack*);         void presPlayTakeOut(Object*, int weaponCode);
void presPlayDoor(Object*, bool opening, int targetFrame);

// pump/frame-loop predicates (1:1 with today's, so pump logic doesn't churn)
bool presReplayActive();                          // was clientCombatAnimActive || clientDoorAnimActive
bool presGlideActiveFor(Object*);  bool presGlidePlayableFor(Object*);
bool presAnyGlidePlayable();       int  presHopsRemaining(Object*);
void presNoteProgress();           unsigned int presLastProgressTick();

// per-frame advance — replaces the 4-call ordering in main.cc:998-1010
void presAdvance();   // internally: glide step, _object_animate(), replay/door reap
```

`presAdvance()` swallowing `_object_animate()` keeps the §E "tickers removed"
invariant literally true (main.cc:1001-1007) while making the advance ordering
(glides → sequences → reaps) a module invariant instead of a main.cc convention.

### 5.2 What stays in client_net.cc (deliberately NOT absorbed)

The **presentation queue** (`PresKind`, `enqueue`, `presentationPump`,
client_net.cc:172-361) stays in the decoder. Rationale: it is *cross-object wire-order
sequencing* (turn flips, attack serialization, caption pairing, door-before-crosser) —
a different concern from per-object channel ownership. The seam is: **the queue
decides WHEN each presentation intent starts; the FSM decides HOW it plays and WHO
owns the pixels meanwhile.** The pump's blocking predicates all exist in the API
above, 1:1. (Post-v1, under the folder restructure, the queue could move into
client/present/ as a sibling file — mechanical, not part of this design.)

Also unchanged: per-hit HP / per-hex AP display deferral (client_net.cc:222-250,
1312-1356) — numeric-display pacing, not channel ownership; it consumes
`presHopsRemaining` exactly as it consumes `clientAnimHopsRemaining` today.

### 5.3 Keying

Table keyed by netId, `Object*` cached in the entry. Today all three registries key
raw `Object*` — safe only because destroy/disconnect/reset hooks are perfectly placed
(client_net.cc:671-672, 697-699, 496-498, 906-908). Keeping those hooks (they become
`presForgetObject`) AND keying by netId means a missed hook degrades to a lookup miss
instead of a freed-pointer dereference. Cheap defense; the hooks remain the real
contract.

### 5.4 Flags routing — narrower than fid **[open question]**

Today: flags write through during a glide (the walk tripwire checks only fid/x/y,
client_anim.cc:516-518) but are held for replay participants (client_combat_anim.cc:
124-127 applies them at resolve). Proposal: keep exactly that — **route flags to
pendingPose only when the entry is PARKED-for-replay or REPLAY; write through in
GLIDE/PARKED-for-glide** — because a flags delta mid-walk (e.g. HIDDEN from a script)
should not wait on pixels, and matching today's semantics keeps step 2 of the
migration behavior-neutral. Flag for review: the alternative (hold flags whenever fid
is held) is more uniform but changes out-of-combat behavior nobody has complained
about.

### 5.5 Debug tripwire (log-only)

Keep a per-frame consistency check under `F2_TRACE_EVENTS`: for GLIDE entries, if
`obj->fid` differs from the FSM's expectation, **log and re-assert ownership** (re-pose)
instead of killing the glide. This is the migration instrument for §4.1's unknown
writer and a permanent canary for new engine paths that write fid outside the rule
(e.g. a future re-enabled ticker). It must never become load-bearing — the rule is
routing, the log is a smoke detector.

### 5.6 Collapse map

| today | becomes |
|---|---|
| client_anim.cc `Walk` + advance/offset/retract logic | GLIDE/PARKED states; hop math, fps frame selection, burst/lag caps, jitter delay, stall backstop move verbatim |
| `posed` / `parkedFid` / `replaySuspended` / `clientAnimSetReplaySuspended` | **deleted** — expressed by PARKED vs GLIDE vs REPLAY |
| `heldHops` + `clientAnimRelease/ReleaseAll` | `playableHops` + `presRelease/ReleaseAll` (same arithmetic) |
| `clientAnimDeferRotation` + `hasPendingRot` | **deleted** — rotation routes via `presRoutePoseDelta`, folded into the commit's stand fid |
| `appliedX/appliedY` + re-anchor on every MOVE (client_anim.cc:272-280) + offset tripwire | **deleted** — offset derived per-frame from `presLocation` |
| `clientAnimReset` vs `clientAnimStandDownAll` | `presReset` vs `presStandDownAll`, shared retire helper |
| client_combat_anim.cc `Deferred` (reserved/active), `resolve`, caps | PARKED/REPLAY, the universal commit, per-entry caps |
| `clientCombatAnimReserve/DeferDelta/NotifyReposition/ForgetObject` | `presReserve` / `presRoutePoseDelta` / the REPLAY×MOVE transition / `presForgetObject` |
| `clientCombatAnimPlay/PlayTakeOut` bodies (throw skip, arm-fid, reg_anim registration) | `presPlayAttack/PlayTakeOut` — moved, not rewritten |
| client_door_anim.cc entire module | DOOR_SLIDE state (~40 lines) |
| client_net.cc onObjectDelta's 3-way hold branching (750-771) | `if (!presRoutePoseDelta(...)) { write fid/rot through via presApplyPose }` |
| main.cc 4-call advance block (998-1010) | `presAdvance()` |

Net: three modules (~1134 lines of .cc + ~236 of headers) → one module, estimated ~700-800 lines
(the hop math, replay registration, and comments dominate and survive; the interlock
machinery and duplicated lifecycle plumbing are what disappear).

--------------------------------------------------------------------------------
## 6. Migration plan

Hard constraint: **no headless oracle for any of this** — goldens pin sim determinism
only, and every one of these modules is dark headless ([[anim-decouple-verification]]).
Each step therefore ends with a live-viewer spot-check per
[[visual-verification-protocol]] (hand the user a demo recipe + targeted questions;
no screenshot floods). Gates 1-10 stay green as the regression floor after every step
(they prove we didn't leak into headless, nothing more).

**Step 0 — pose helper sweep (trivial, behavior-identical).**
Introduce `presApplyPose` and use it at the existing fid-set sites
(client_anim.cc:196, client_combat_anim.cc:114-122 + 263-266, client_net.cc:758).
Verify: build; one combat kill (corpse visible — the SF-frame case) + one walk.
Risk: ~nil. Locks in §4.3 before anything moves.

**Step 1 — route-don't-kill on the glide fid channel (THE de-risking step).**
In `onObjectDelta`, when the target has an active walk, route fid/rot into a new
`pendingPose` on the Walk (applied at drain, where pendingRot applies today,
client_anim.cc:559-563) instead of writing through and letting the tripwire kill.
Demote the tripwire to §5.5 log-only for the fid channel (keep the offset check as-is
for now). Smallest possible slice of the core rule, and the instrument for §4.1's
unknown writer.
Verify (live): (a) long out-of-combat RUN across the map — smooth, no STAND flashes
(if flashes persist, the log now names the writer — that's the point); (b) NPC
wield→approach→shoot still presents draw-at-destination (the replaySuspended path
still exists and must not regress); (c) `F2_TRACE_EVENTS=1` run: zero `[walk] TRIPWIRE`
kills during plain movement.
Risk concentration: interaction with `replaySuspended` (two hold mechanisms briefly
coexist). Mitigation: routing takes precedence — a suspended walk routes too.

**Step 1b — drain grace (if step 1's log implicates churn, §4.1b).**
Add the linger to walk drain. Verify: same RUN recipe; also walk (not run) pacing
unchanged; combat glides unaffected (in combat, drains hand off to held hops, not to
retirement, so grace must not delay turn-flip blocking — check AP dots still flip
promptly after the last approach).

**Step 2 — merge registries into client_present.cc (the big move).**
Mechanical relocation of client_anim + client_combat_anim + client_door_anim into the
one table + state enum; delete `posed`/`parkedFid`/`replaySuspended`/
`clientAnimSetReplaySuspended`; client_net.cc switches to the §5.1 API (thin
compatibility shims are acceptable for one commit, then deleted). No behavior change
intended — this step is pure consolidation; every transition maps 1:1 to code that
already exists.
Verify (live, the full COMBAT_CLIENT_DESIGN checklist — risk concentrates here):
1. ranged NPC mid-fight wield: draw plays at destination after approach glide, no
   teleport, armed pose after draw (quirks §6.1-3);
2. killing blow: fall animation → corpse renders (SF frame);
3. knockback: defender rides MOVE, no pre-flash of prone pose;
4. door: opens fully before crosser glides through, both in and out of combat;
5. turn pacing: AP dots/lights flip only after the outgoing actor's last glide;
   captions ride with their attack; end-of-combat chrome after the death anim;
6. combat-enter mid-run → clean STAND (case 2);
7. multi-attack beat: attacks serialize one at a time;
8. rebaseline mid-fight (second viewer joins): no crash, framing preserved,
   presentation drops cleanly (`presReset` path).
Where it can silently regress: the pump's take-out reorder (client_net.cc:303-330)
reads glide state through the predicates — verify its three branches (gliding-wait,
held-reorder, degenerate-no-release) still trigger by tracing a fight with
`F2_TRACE_EVENTS=1`.

**Step 3 — present-tile rebucketing (SHIPPED, revised from the written plan below).**
The written plan here was "derived offset": replace appliedX/appliedY with a per-frame
`screen(presLocation) - screen(obj->tile)` computation. During implementation this was
found to be a NO-OP for #6/#9: it is mathematically identical to today's offset and does
NOT change the renderer's z-order, which sorts by per-tile BUCKET (gObjectListHeadByTile,
keyed by obj->tile). The decoder jumps obj->tile to a MOVE's FINAL destination (state
first), so a multi-hop glide buckets the sprite up to ~16 tiles AHEAD of where it renders
→ it paints over walls it hasn't reached (#6) and hit-reactions/z-sort read the fled tile
(#9). What SHIPPED instead (client_anim.cc):
  - `walkRebucket()`: the glide keeps obj bucketed at the hop it is PLAYING
    (hops.front().toTile, ≤1 tile of z-lead, exactly like vanilla _object_move). obj->tile
    thus LAGS authority on the viewer for the glide's duration.
  - `walkSnapToAuthority()`: "snap forward" (drop the glide) reconciles the lagging
    obj->tile to the authoritative dest (hops.back().toTile) — UNLESS obj->tile already
    moved off the presented bucket (a real external reposition / the decoder's pre-CAP-ERASE
    jump), which is newer authority to keep. (Missing this was the STRANDING bug: a
    SNAP-KILL left the critter at the presented tile, 7 tiles short.)
  - OFFSET tripwire → route-don't-kill for small foreign drift (attack-recoil residue):
    now that the offset is bounded to ≤1 tile, obj->tile reliably distinguishes residue
    (tile unchanged → re-assert our offset, keep gliding) from a real snap (tile moved →
    kill). This is the offset-channel twin of step 1's fid route-don't-kill, and it fixed
    the auto-aggro flee teleport.
  - client_combat_anim.cc: clientCombatAnimPlay now cancels a participant's walk ONLY if
    it is PLAYING (playable); a HELD/parked walk (a defender's flee queued the same beat it
    is hit) is LEFT — enterReplay + the advance loop suspend it in place during the hit,
    and its kMoveRelease glides it AFTER (vanilla hit-then-flee). Cancelling it was the
    direct-attack flee teleport.
Reviewed adversarially (which SHIPped the pre-stranding version — it rationalized "SNAP-KILL
needn't reconcile," missed by the live oracle), then live-verified across several rounds.
The step-2 MERGE is still the right consolidation; these fixes live in client_anim.cc /
client_combat_anim.cc and will fold into client_present.{h,cc} when step 2 lands.

**Step 4 — cleanup + docs.**
Delete the three old modules and dead API; update COMBAT_CLIENT_DESIGN.md §3.c/§3.d
mechanism references and the §6 known-issue list (random combat-glide teleports should
be re-tested and either closed or re-banked with fresh trace evidence); note the
module in MEMORY via the normal channel.

Ordering rationale: steps 0-1 are small, independently shippable, and directly attack
the live bug class; step 2 lands only after the core rule has survived real use on the
riskiest channel (fid); step 3 is optional polish that the merged module makes safe to
do last.

--------------------------------------------------------------------------------
## 7. Risks and open questions

1. **Case 1's root cause is not pinned** (§1.3, §4.1). The design fixes both candidate
   mechanisms, but if the STAND fid turns out to come from a path that *also* moves
   the object (e.g. some scripted placement), routing alone won't mask the snap —
   step 1's log decides before step 2 commits the architecture. **[UNSURE]**
2. **PendingPose staleness.** Routing means a pose delta can wait behind a long glide
   (up to ~16 hops × 200 ms ≈ 3 s). For fid/rot that is the intended "pixels are
   late"; but a delta that *semantically pairs* with immediately-applied numeric state
   (e.g. death fid + hp=0 on a critter that is somehow gliding) shows a live-looking
   walker with 0 hp for the glide's tail. Today's tripwire "fixed" this by killing the
   glide (with a snap). Mitigation option if it ever bites: a DAM_DEAD/corpse-fid
   delta forces retire-then-apply (one labeled transition). Not proposed for v1 —
   the server stops movers before killing them. **[open]**
3. **Flags routing scope** (§5.4) — reviewer decision requested.
4. **REPLAY delegates channels to reg_anim**, so the FSM's ownership claim is "REPLAY
   = reg_anim owns it", not direct control. The existing safety argument (the viewer
   is the sole registrar; completion via `animationIsBusy`; caps) carries over
   unchanged (client_combat_anim.cc:16-27), but the §5 risk-2 backstop assert from
   COMBAT_CLIENT_DESIGN ("registry empty unless replay in flight") becomes cheaper to
   implement in the single module and should land with step 2.
5. **Behavior-preservation risk in step 2** is the real cost of this design: the
   current code encodes many hard-won micro-orderings in comments (pose-after-release
   re-clock, rotate-after-retract, resume-re-baseline). The transition table
   (§3.3) is the checklist; the live-verify list (§6 step 2) is the oracle. Budget an
   adversarial review pass on the merged module (mandatory per
   [[anim-decouple-verification]] — no headless oracle exists).
6. **What NOT to prototype**: absorbing the presentation queue, changing wire
   semantics, smooth knockback, spawn pop-in presentation. All are future transitions
   this design is *shaped for* (that's the payoff), but none are in scope.
7. **Prototype first** (before even step 0, if desired): a throwaway branch of step 1
   on the fid channel only, run against the RUN repro — one day of work, answers the
   two live questions (who writes STAND; does routing feel right in play) with ~40
   lines changed.
