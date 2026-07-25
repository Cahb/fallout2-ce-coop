# Co-op live-play bug report — 2026-07-21 (2 players, host + 1 extra)

Source: owner + buddy playing a real co-op session. All observations are LIVE, mostly
without logs and mostly without a confirmed repro. Hypotheses below are the owner's or
mine and are **UNVERIFIED** — per `dont-declare-not-a-bug-confidently`, none of them is
a diagnosis yet.

**SCOPE RULE (owner, explicit).** Items A–N below are *exactly* what was observed in this
session and nothing else. Do not fold in older bug backlogs — some of that is STALE. Where
this document cites a previously-banked hypothesis it is offered as *background that may
already be wrong*: re-verify it against today's source, do not inherit its conclusion, and
do not add any bug that is not one of A–N.

Repo context an investigator should hold: `MP_PROPOSAL.md` (co-op spec of record),
`MP_PROTOCOL.md` (wire), `PRESENTATION_RECORD_REPLAY_SPEC.md` + `src/pres_record.h`
(the record/replay presentation channel), `COMBAT_MOVE_RECORD_DESIGN.md`,
`CLIENT_JOIN_DESIGN.md`. Two-lane mental model: **immediate authoritative STATE** (server
deltas, applied unconditionally) vs **paced PRESENTATION** (recorded sequences / glides,
drained over time). Most symptoms below smell like an ORDERING race between those lanes,
or like a piece of state that is presented but never replicated.

---

## A. Map transition × combat × dead player — black map, no worldmap

**Observed.** P2 is DEAD. P1 is ALIVE and **in combat**. P1 runs to a map exit grid.
The worldmap screen appears — then "combat ends" fires, and P1 ends up **on an empty
black map with no worldmap view**. Input state after that: broken/unclear.

**Shape.** A map transition is being taken *while a combat session is still live*, and the
combat-exit that lands afterwards tears down or overwrites the screen the transition just
pushed. Candidate order: `exit grid → host-only map transition (MP_PROPOSAL Ch 14.2) →
worldmap driver pushed → deferred/queued COMBAT_EXIT applies → screen state clobbered`.

**Why it may be co-op-specific.** With one player dead, whatever ends combat (last living
enemy / roster re-evaluation / the dead actor leaving the turn order) may fire *late*,
right on top of the transition. Related known bug: the 62s combat-state desync, where the
client's `_inCombat` flips independently of the server's (`[cbtstate]` trace already in
tree at `client_net.cc`).

**Questions to answer from source.** Can `exit grid` be honored while `_combat` is active
server-side — is there a gate? Where is COMBAT_EXIT queued on the client (behind the replay
queue?) and what does it do to the current screen/window stack? Does the worldmap driver
survive a combat-exit arriving after it starts? Is there a path where the map is unloaded
but no new map/worldmap is presented (the "black map")?

## B. Player appearance (skin) desyncs on map load — tribal vs vault suit

**Observed.** After a map load, a player sometimes renders as the **tribal/"Nard" skin**
and sometimes as the **vault-suit** skin — inconsistently, and differently between clients.
This is not cosmetic: the **tribal art set lacks animations for most weapons** (basically
spear only), so an actor stuck on the tribal fid plays wrong/missing weapon animations.

**Shape.** The base appearance FID (the dude's art set — `ART_TYPE_CRITTER` base frame,
derived from armor + the proto's base fid) is being derived locally on each client rather
than taken authoritatively from the server, OR the derivation runs at a moment when the
local actor binding (`gDude` / `rebindLocalActor`) is still pointing at the host's actor.
Known adjacent trap: **cached gDude-derived anchors** — anything that caches a
gDude-derived pointer/value earlier in a rebaseline is left pointing at the HOST
(exemplar: `_inven_dude`). Also known: `artExists()` checks against the tribal art set
fail for guns (`inventory.cc:378`), which is exactly the "no animation for anything but a
spear" signature.

**Questions.** Where is the dude's base/appearance fid decided on load (join blob apply,
`_obj_load_dude`, `_proto_dude_init`, armor `equipmentApply`)? Is the base art set part of
the replicated object state or re-derived per-client? Does the extra player share
`gDudeProto` (co-op v1 = N bodies, ONE sheet) and does the *fid* leak between actors?

## C. Combat desyncs, worse in crowded spaces (no repro)

**Observed.** "Legit heavy combat desyncs", not reproducible; the owner's read is that it
degrades **in crowded scenes** (many actors/many concurrent presentations) while most of
the time combat plays great.

**Shape.** Presentation backlog: the record/replay channel presents one sequence at a time
per actor, paced, over synchronous global engine state. More actors = more sequences =
longer drain = more windows in which the authoritative state has moved on while the
viewer is still playing something old. Suspect the pump/drain watchdog (`[busy] STUCK`)
and any per-actor held-delta state.

**Questions.** Is there any backpressure or drop policy when a viewer's replay queue grows?
What happens to held deltas (position/AP/fid) if a sequence never drains? Is there a
bound on queue depth or a resync?

**►► ADDENDUM (owner, and this is the sharpest data point in the report).** The one clear
sighting: owner was P1 and taking his turn (running); **P2 was DEAD**; P2's viewer was
showing events from **1–2 turns ago**. Not a glitch — a sustained, growing lag.

Hypothesis worth testing before any other for this item: **the turn barrier is the only
backpressure in the system, and a dead player is never waited on.** The server blocks on a
LIVING actor's turn until that client answers, and that block is incidentally what lets that
viewer's paced replay queue drain. A dead player has no turn, so nothing ever throttles the
server on his behalf; if sequences are emitted faster than they replay (and replay is
serial and paced in real wall-time), his queue grows monotonically and never recovers. This
also reframes "crowded spaces": crowding is not the cause, it is just the easiest way to
push emission rate past drain rate.

If that holds, the missing piece is a **policy, not a fix**: either real backpressure, or a
catch-up rule (skip/compress queued sequences, or snap to authoritative state, once lag
exceeds a threshold). Owner design decision. Check: is there any queue-depth bound, drop
path, or lag metric on the viewer side at all? Does a dead/spectating client take a
different pump path from a living one?

## D. Invisible weapon + throw originating from the wrong tile

**Observed, two coupled symptoms.**
1. Buddy was holding a **spear that never appeared on my screen** — his actor rendered
   unarmed; his combat animation looked like it was "skipping frames" from my viewpoint.
2. In a later fight he **threw a spear** — I saw the throw animation, but the projectile
   **flew in from across the map**, from roughly the position **my client believed he was
   at** rather than his real tile.

**Shape.** (1) is a wielded-item / equip-appearance replication gap for a *remote* actor
(the equip-appearance and take-out-weapon presentation are known weak spots — the
"equip-while-gliding" write-through issue and the NPC in-combat draw gap). (2) is
strong evidence the **throw's flight sequence takes its origin from the local (stale)
object position** rather than from an origin carried in the recorded sequence — i.e. the
projectile start tile is re-derived on the viewer. If the actor's rendered position has
drifted (see the permanent glide-offset desync, #10), the arc starts from the drift.

**Questions.** Does the recorded throw/projectile sequence carry an explicit origin tile,
or does the viewer read `obj->tile` at play time? Same question for melee/ranged impact
anchoring. And: is the wielded-item fid for a *non-local* actor derived from replicated
inventory, or from an object delta that can be missed?

## E. denbus2 "services" dialog — money taken, teleport, dialog re-enters, SOFTLOCK

**Observed.** In `denbus2.map` there is a woman with a solicitation dialog (~$300–750).
Paying works: money is taken, the player is teleported to the room — and then the **dialog
appears twice** (or re-enters), after which the game is **softlocked: no input does
anything**. Present since **day one of dialog support**; reproducible in the sense that it
always happens.

**Shape (strong lead).** This is the classic **lvar double-fire** class documented for
co-op: a script state machine latched against one player can be advanced by another, and
`dude_obj` resolution on the passive sim path is host-anchored. But note it "has been like
this since day 1", which suggests it may also reproduce SOLO — that is the first thing to
check, because it separates a co-op scoping bug from a dialog-driver re-entrancy bug.
Second candidate: the script does `gdialog` work around a **teleport/map-move**, and our
dialog driver is a modal pump nested inside the interpreter's call stack — a teleport
mid-dialog (or a `start_gdialog` issued while a dialog is already active) can leave the
driver's stack unbalanced, which is exactly what "no input does anything" looks like.

**Ask for.** The actual script (`den` prostitute — extract from `master.dat`/`patch000.dat`
per the DAT2 recipe), and whether it calls `gfade`/`gdialog_*` around a `critter_dmg`/
`move_to`/`float_msg` sequence. Then: what does our driver do on a re-entrant
`start_gdialog`, and what unwedges the pump if the dialog never ends?

## F. No player revive / death is a dead end

**Missing feature, owner-specified shape.** A dead player should be revivable by using a
**healing item (stimpak / healing powder) on the corpse**. Today there is no revive at all;
death is only *gated* (a dead player cannot act). Open sub-questions the owner raised:
does the engine even *allow* targeting a dead critter with a healing item (the use-item-on
path may reject dead targets outright)? Also unresolved and load-bearing: **host-ownership
transfer** — worldmap, map transitions and dialog are host-only, so a dead HOST freezes the
whole party's ability to travel.

Related: a dead player carried to the next map — what body/animation state does the corpse
arrive in (see G).

## G. Death / gib presentation resyncs to a generic corpse

**Observed.** When a critter is blown to pieces, at some point the server appears to
**resync the gibs to a standard death (lying) — or even standing — animation**. Sometimes
the **gibs are not rendered at all** (no explosion-death animation). Owner also saw a
**C4 explosion play TWICE** once. And: when a dead player is carried to the next map, the
arriving corpse state looks wrong.

**Known partial root cause (already banked).** `_show_death` does not set the corpse fid;
the viewer's gib corpse comes from the death animation's *final frame*, while the server
finalizes via `critterKill(..., -1, ...)` → generic single-frame death corpse, which rides
an OBJECT_DELTA and **overwrites** the viewer's gib. Unexplained differential: a placed
BOMB gibs correctly, an in-combat ROCKET does not — so the channel/timing differs, not just
`critterKill`. "Gibs not rendered at all" and "settles to STANDING" are new data points and
may be the same delta-overwrite arriving *early* (cf. the stale-frame render gotcha:
`objectSetFid` does not reset `obj->frame`, and a stale frame ≥ the new art's frame count
renders NOTHING — that would explain an invisible explosion).

**Double C4 explosion** — separate: look for a path that can present the same explosion via
both the bespoke `EVENT_EXPLOSION_FX` cue AND the record channel (progressive cue retirement
is incomplete: some cues are still the default while recorded paths also exist).

## H. Cannot put items INTO a corpse (dead scorpion) — vanilla or ours?

**Observed.** Items cannot be placed into a dead scorpion's body. Owner unsure whether
vanilla behaves the same. Adjacent known bug: **gibbed critters cannot be LOOTED at all**
(explosion-killed critters drop items but the loot interaction does nothing). Worth
resolving both against vanilla behavior before treating either as a regression.

## I. Container (chest / ice box) open-closed animation state desyncs

**Observed.** World containers with open/closed animation states get out of sync between
clients — one player sees it open, the other closed. Not always; showed up mostly while
deliberately stress-testing.

**Shape.** Same family as the door-state cue: the container's visual state is presented via
a cue/record sequence, and its authoritative frame/fid may not be part of the replicated
object state (or is, but the presentation lane overwrites it locally and never reconciles).
Also ask the standing question: **what does a client that joins 10 minutes late see?** —
a transient one-shot cue is invisible to late joiners; container state must be in the
baseline.

## J. Explosion-killed player GLIDES several hexes, then dies on arrival

**Observed.** A C4 explosion that kills a player: the body **slowly slides/glides ~5+ hexes
to a destination tile**, then plays the death animation on arrival.

**Shape.** The knockback's authoritative position change is being applied as a *position
delta*, which the viewer's free-roam glide machinery interpolates as an ordinary walk
(slow, tile-by-tile), instead of being presented as the recorded knockback animation. The
death then plays when the glide drains. This is the same seam as K.

## K. Knockback / knockdown presentation collides with the position change

**Observed.** Sometimes only an SFX plays and the player is simply +1 hex away; sometimes
the knockback animation plays, sometimes not. Owner's own read (and it matches the code
model): **two things must happen at once** — play the knockback/knockdown animation AND move
the actor to the pushed-to hex — and they currently fight each other.

**Shape.** DAM_KNOCKED_BACK / DAM_KNOCKED_DOWN presentation vs the authoritative position
delta: whichever lane wins, the other is dropped. Banked test gap already noted:
multi-victim knockback and DAM_KNOCKED_DOWN standup have **no fixture**. The convergent fix
shape is the held-delta family — hold the position field until the knockback sequence has
played, then reconcile.

## L. Movie / video playback blocks the host (nothing happens)

**Observed.** With video playback enabled, when the host crosses a map trigger that should
play a movie, **nothing happens at all** — not even a black screen — and the game appears
stuck (can't do anything).

**Shape.** The movie player is a *client-side modal* driven from core code that on the
dedicated server has no device. Suspect: the server calls the movie play path, the
presenter override returns immediately or blocks, and either (a) the server is parked in a
block-and-pump driver that never advances the sim clock (a known class: every time-keyed
cadence silently dies), or (b) no cue is ever sent to the viewer so the client shows
nothing while the server waits. Needs a movie driver like the other modal drivers
(dialog/rest/endgame/worldmap/barter).

## M. Weapon RELOAD / ammo state is not replicated

**Observed.** Ammo in a weapon is not streamed/synced. Practical consequence: after firing
a rocket launcher the player **cannot reload it** and must throw the weapon away.

**Shape.** The weapon's loaded-ammo count lives on the item object (`ammoQuantity` in the
item's data) and the reload action is presumably either not exposed as a control verb or
its result is not shipped in the object/inventory delta. Both halves need checking: is
RELOAD reachable at all from the viewer (inventory UI / active-item slot / the `A` reload
key), and does the resulting ammo state reach every client?

## N. Camera quality-of-life (two requests, not bugs)

1. **On loading a save, the camera centers on the map ENTRANCE, not on the player** — you
   have to hunt for your own character.
2. **The camera is leashed** to within X tiles of the player. The owner wants free camera
   roam with no lock distance.

---

## Cross-cutting themes worth stating once

- **Presented-but-not-replicated state.** Skin/base fid (B), wielded weapon (D1), container
  open state (I), weapon ammo (M) — each is a piece of state a viewer renders that may be
  derived locally rather than replicated authoritatively. A single audit question shrinks
  the list: *for every visual attribute the viewer draws, who owns it, and what does a
  late joiner see?*
- **Position vs animation lane collisions.** J, K, D2 and the known permanent glide-offset
  desync are all "the authoritative position moved while a presentation owned that actor's
  position". The banked fix shape is the held-delta family + a presentation watchdog that
  snaps a stale glide to the authoritative tile.
- **Screen/driver lifecycle races.** A (worldmap × combat-exit), E (dialog re-entry), L
  (movie) are all modal-driver lifecycle problems: a driver being entered, re-entered, or
  torn down by an event that arrives from the other lane.

---

# ADDENDUM — reported later the same day (owner, from memory, no logs)

These are NOT part of the A–N scope set above; they were recalled after that report was
closed. Same rules apply: observations are live, hypotheses are UNVERIFIED.

## O. Misclicking a non-enemy target in combat silently ENDS the turn

**Observed.** In combat, aiming an attack and misclicking on **yourself**, or on a **door**,
skips the turn. No message, no obvious reason — the turn is just gone.

**►► UPDATE (owner, later same day): it is NOT only invalid targets.** Firing a ROCKET
LAUNCHER at a legitimate enemy skips the turn the same way. That reframes O: it is not a
bad-target bug, it is the **common landing zone for every REJECTED action**. Item M is a
supplier — ammo is not replicated, so the client offers an attack the server knows is
impossible, the server rejects it, and the rejection is spent as a turn. Fixing O therefore
pays for M's symptom too, and for the door/self cases. Prioritise accordingly.

**►► ROOT CAUSE FOUND (2026-07-21, reading combat_drain.cc while chasing the wire-combat
gate). It is DELIBERATE, and the comment says so.** `serverCombatPumpForSlot`, the
COMBAT_INTENT_ATTACK branch:
```c
if (target == nullptr
    || _combat_check_bad_shot(actor, target, hitMode, aiming) != COMBAT_BAD_SHOT_OK
    || _combat_attack(actor, target, hitMode, intent.hitLocation) != 0) {
    combatIntentPopForSlot(actorSlot);
    outcome.stop = CombatPumpStop::kEndTurn;      // <-- the turn is spent
    break;
}
```
Any attack that cannot execute — no target, out of range, not enough AP, **no ammo**, aim
blocked — is discarded AND ends the turn. So clicking yourself, a door, or firing a dry rocket
launcher spends the whole turn. That is exactly the reported symptom, and it **confirms the
M -> O link**: ammo is not streamed, so the viewer happily offers "fire", the server rejects it,
and the turn is gone.

The rationale in the comment is real and must not be undone: an earlier design HELD the failed
intent to retry next turn, and a permanent failure (dry weapon) then sat at the queue head
re-failing every turn, ending each turn without waiting for input — combat spun at machine speed
and the player could never act. **Do not restore retry.**

**The fix is to split the two cases**, which the current code conflates:
- *Rejected before acting* (bad target, no ammo, out of range, insufficient AP) — the actor never
  swung. Cost nothing, keep the turn, and TELL THE PLAYER why (item U).
- *Acted and failed* (the swing happened and missed / was interrupted) — costs AP, may end the turn.
Only the second is a spent turn. The first is a no-op, and pricing it as a turn is the bug. Two things to separate: (1) does vanilla end the turn here too, or
is this ours? (2) is it the *intent* layer rejecting, or the combat driver reading a failed
action as a completed one and advancing the turn barrier? A no-op must cost 0 AP AND must
not release the barrier — cf. the standing rule "price the SCREEN not the action"
(`in-combat-interaction-slice`), and the turn-end-blocking-not-loop-blocking model.

## P. Door use double-fires — "open" then immediately "close"

**Observed.** Run-to-use a door. The actor arrives and **nothing appears to happen**, so you
click again — now the door looks partly open but you cannot walk through it. Read: the
first use DID open it (just not presented), and the second use **closed it again**. Happens
both solo and with two clients. Made much worse by the fact that recovering means a
pixel-perfect hunt for the door sprite, which is sometimes not clickable at all.

**Shape (unverified).** Two candidate mechanisms, not mutually exclusive:
- **Presentation lost, state changed.** The door's open state IS toggled server-side but the
  open animation/cue never reaches the viewer, so the player has no feedback and re-issues.
  That makes this a presented-but-not-replicated case (cross-cutting theme above), and it
  overlaps bugs/002 (door anim blocks movement) and bugs/004 (migrate doors to pres/replay).
- **Genuine double-fire.** The approach-then-use path fires the use twice (once on arrival,
  once from the queued intent) — the same *shape* as the lvar double-fire class in
  `coop-mp-track`. If so, the visible "half-open" sprite is the second toggle's animation.

Also a UX tail worth fixing regardless: **door hitboxes are too small / sometimes unhittable**.

**►► LEADING CANDIDATE (added after item X, same day — TEST THIS FIRST).** Both `_obj_use` (door
use) and `_set_door_state_closed` are in the set of animation callbacks that **silently no-opped
server-side** (X). A door used through vanilla's action path would then play its animation — the
client shows it opening — while the server never applied `OBJECT_OPEN_DOOR`: *looks open, still
blocks*. The second press routes through `server_control.cc`'s direct fire path instead, which
does apply the toggle — so if the first press HAD landed, the second closes it again. That
reproduces both halves of the report (owner's own reading included: "first one really did open
it, second closed it back").

►► **Stage 1 of X (committed 2026-07-21) puts both callbacks in the allowlist, so P may ALREADY
be fixed.** Unverified — check it before spending any more time on P. If doors are still wrong
afterwards, the remaining suspect is presentation loss (bugs/002, bugs/004 migrate-doors-to-
pres-replay), not state.

## Q. Server/client ergonomics — the env vars ARE the product, stop making them opt-in

**Owner, explicit.** Everything below is "it should just work when you run the binary".

1. **`F2_JOIN_TMP_CLIENT` is a POSIX path and does not work by default on Windows.** Owner
   had to set `F2_JOIN_TMP_CLIENT=C:\f2ce_join.bin` by hand — that was the ONLY value that
   worked. Wanted: when unset, default to a file in the **current working directory**, built
   with platform-appropriate path handling (`compat_` path helpers / `std::filesystem`), not
   a hardcoded `/tmp/...`. Also make sure the chosen path is actually readable/writable and
   fails loudly if not.
2. **The whole dedicated-server env block should be the DEFAULT**, not opt-in. Owner's actual
   launch line — every one of these is "part of the game/server", not an experiment:
   `F2_SERVER_PLAYERS=2`, `F2_SERVER_NET=9200`, `F2_SERVER_CMD=9201`, `F2_SERVER_PACE_MS=100`,
   `F2_SERVER_RESUMABLE_COMBAT=1`, `F2_SERVER_SMOOTH_WALK=1`, `F2_SERVER_PRES_RECORD=1`,
   `F2_DIALOG_STREAM=1`, `F2_WORLDMAP_STREAM=1`, plus a default `F2_SERVER_NAME`.
3. **`F2_SERVER_TICKS` must not be load-bearing.** Unset / `0` / `-1` should mean *run
   forever* (production). Today an unset or exhausted tick cap ends the dedicated server,
   which is "annoying". Keep the cap as an explicit option for harness runs.

**►► HAZARD to respect when implementing (do not just flip the defaults).** Several of these
flags are env-gated OFF *precisely so the golden gates stay byte-identical* — `F2_DIALOG_STREAM`
is documented that way in `server_main.cc`, and `F2_SERVER_PRES_RECORD` / `SMOOTH_WALK` /
`RESUMABLE_COMBAT` all change emission or pacing. Flipping the process-wide default ON would
rewrite every golden. The correct shape is almost certainly **defaults keyed on the RUN MODE,
not on the env**: a dedicated/production launch turns them all on, while the golden + probe
harness path opts explicitly out (or pins its own profile). Decide that seam before touching
any individual flag.

4. **The admin/lobby channel must not require telnet or netcat.** Today picking a world,
   saving, loading and every admin verb is "pipe a line into `nc 127.0.0.1 9201`". Most
   Windows users have neither `telnet` (off by default since Win7) nor `nc`. The lobby is
   currently unreachable for them, which makes save/load and world selection unreachable too.
   Wanted: a way in that ships with the game. Candidate shapes, cheapest first — (a) the
   server reads admin lines on its **own stdin** so the console window you already launched
   IS the admin console; (b) the **client** gets an in-game console/lobby screen that sends
   admin verbs over the existing wire control plane (needs an admin/host authorization gate,
   since the control plane is deliberately restricted today); (c) a tiny bundled CLI. (a) is
   nearly free and unblocks a solo host immediately; (b) is the real answer for a co-op lobby
   and is the one that matches "host picks a world from the menu".

## R. Doors × NPCs — warp-through, late opens, and flapping with nobody there

**Observed (owner, live).** Three related things, listed most-suspicious last:
1. NPCs appear to **warp through closed doors**.
2. The door then opens *afterwards* — "they kinda sorta used them / got through them".
3. ►► Doors **flap open/closed with nobody touching them RIGHT NOW**. Owner explicitly flags
   1 and 2 as possibly vanilla-ish, but 3 as *not* vanilla behaviour.

**Shape for 1+2 — presentation lag, almost certainly pre-existing.** Position is authoritative
STATE applied immediately; the door open is paced PRESENTATION drained over time. When the
presentation lane is behind, the NPC's new tile lands before the door's animation, so it reads
as warping through a shut door that opens late. This is the standard two-lane ordering race and
the backpressure gap ([[presentation-backpressure-gap]]) is the amplifier. Overlaps bugs/002
and bugs/004 (migrate doors to pres/replay).

**Shape for 3 — ►► SUSPECT OUR OWN TIME-SKIP COALESCING (new 2026-07-21, unverified).** A door
does NOT move: it changes via `flags |= OBJECT_OPEN_DOOR` plus a `reg_anim` animation
(`proto_instance.cc:1760`, `_obj_use_door`). The time-skip window added today
(`presenterTimeSkipBegin/End`) coalesces **`objectMoved` only** — so during a script time skip
the NPCs snap to their final tiles while every door they passed through during the catch-up
still emits its full open/close presentation. Net effect: doors animating all over the map with
no visible actor near them, which is exactly symptom 3.

**A/B test that settles it:** run with `F2_NO_TIMESKIP_COALESCE=1` (restores the old per-tile
flood byte-for-byte). Doors still flap → pre-existing, not ours. Doors stop → ours.

**If it is ours, the fix is a widening, not a revert.** The principle behind the skip window is
"the world advanced UNWATCHED, so present final STATE, not the animation that produced it" —
that must cover the whole world, not just positions. Doors (and any other reg_anim'd scenery
state touched during the skip) should likewise be suppressed and shipped as final open/closed
state. Reverting the coalescing would just trade flapping doors back for the 320-move avalanche.

## S. Run vs WALK animation is inferred from timing, not replicated (BUG4, root-caused)

**Observed (owner, live).** Junkies wander around looking like they are playing a *panic*
animation. In combat the same class of wrongness: **some guards run, some just walk**, with no
apparent reason.

**►► ROOT CAUSE (read from source, 2026-07-21 — this is no longer a hypothesis).**
`client_present.cc:1059`:
```c
constexpr int kRunThresholdMs = 120;          // :85
int anim = durMs <= kRunThresholdMs ? ANIM_RUNNING : ANIM_WALK;
```
The viewer **re-derives** run-vs-walk by comparing the hop duration to a single hardcoded
threshold. The wire never carries the answer: `EVENT_MOVE` is
`netId, fromTile, toTile, fromElev, toElev, durMs` and nothing else. The SERVER knows the truth
— `server_anim.cc:628/717` pass `run ? ANIM_RUNNING : ANIM_WALK` into the record channel — and
then discards it for the STATE-lane move event.

Consequences, all of which match the report:
- Any pacing jitter flips the animation. Two critters moving at genuinely different speeds land
  on opposite sides of one constant → "some guards run, some walk".
- A slow-but-running critter renders as walking; a fast-but-walking one renders as running,
  which on an idle wanderer reads as panic.
- `F2_SERVER_PACE_MS`, load, and backpressure all shift `durMs`, so the animation is a function
  of SERVER LOAD. That is the tell: presentation must never depend on wall-clock pacing.

This is a textbook instance of the report's own cross-cutting theme, *presented-but-not-
replicated state* — same family as skin/base fid (B), wielded weapon (D1), container open state
(I), weapon ammo (M).

**Fix shape.** Put the anim type on the wire in `EVENT_MOVE` and have the viewer obey it,
keeping the existing `artExists` fallback (many critters genuinely have no run art — that
fallback is correct and must stay). Appending a trailing field to an event body is the
established pattern here (see `objectCreated`'s birth-flags note in `presenter_network.cc`).
Delete `kRunThresholdMs` outright once it lands — a heuristic that shadows an authoritative
value is worse than no heuristic.

Supersedes the banked "BUG4 walk/run desync" placeholder in [[presentation-viewer-bugs]].

## T. Spam-clicking an interact queues N sequences the viewer must replay (input-side backpressure)

**Observed (owner, live).** Rapid-clicking a locked door registers EVERY click. The player is
not physically stuck (movement still works) but is "stuck" watching the accumulated use
sequences replay one after another.

**Why the existing dedupe misses it.** The pending-interaction slot is already latest-wins:
`gPendingBySession[sessionId]` holds ONE entry per session and re-arming erases the previous one
(`server_control.cc:528`, "supersede this session's prior intent"). Confirmed live — a spam-click
on a DISTANT door produced 7 × `interact ... armed (approach, session 1)` and a single `FIRE`.
That path is fine.

The gap is the **already-in-range** case: no approach is required, so the intent never parks in
the latest-wins slot and fires immediately. N clicks inside one tick = N fires = N recorded
sequences, drained serially by the viewer. Locked doors make it obvious because the use fails,
so the player naturally clicks again.

**This is the INPUT side of [[presentation-backpressure-gap]]** — same missing policy (nothing
anywhere relates production rate to drain rate), arriving through the control plane instead of
the emitter. Worth fixing together with that track, not as a one-off.

**Fix shape (owner proposed a debounce; recommend coalescing instead).**
- ►► Preferred: **collapse identical (session, verb, target) interacts within a tick**, and more
  generally refuse to start a new out-of-combat interaction for an actor while that actor's
  previous interaction sequence has not completed. Needs no magic constant and no clock.
- A time debounce ("ignore if < 1-2 s since last action") is the fallback. It needs a tuned
  constant, and the honest duration to key on is the RECORDED SEQUENCE's own length — the record
  channel does carry durations — not a guessed wall-clock number.
- The general answer is layer 2 of the backpressure track (advisory lag channel: viewer reports
  last-applied sequence id / queue depth). ►► That must stay ADVISORY — the moment the sim blocks
  on it, one slow client freezes the world for everyone.

Related: a failed use on a LOCKED door should also give feedback, or the player has no reason
not to click again (overlaps P, the door double-fire / pixel-hunt item).

## U. ►► PRINCIPLE: a rejected action must SAY SO, to the player, not just to stderr

**Owner, explicit, and it reframes several bugs above.** Today the server rejects plenty of
actions and writes a line to its own stderr that no player will ever see. Live examples pulled
straight from this session's logs:
```
f2_server: control take no pid=20 in source netId=612 ignored
f2_server: control invdrop no dude item netId=2805 ignored
f2_server: control dsay ignored (no active dialog)
f2_server: control interact ... dropped (host-only screen)
```
From inside the game every one of these is *nothing happening*. The owner's framing, which is
the right one: **"why can't I loot this item from the body" is a completely different player
experience from "oh, server lag, maybe if I walk around it fixes itself."** A silent rejection
teaches the player to distrust the whole game and to spam-click (→ item T); an explicit one
costs a single line and ends the confusion.

**Rule going forward.** Every server-side reject/ignore/deny path emits BOTH:
1. the existing stderr line (operator/debug, unchanged — and note `gate-scripts-grep-log-strings`:
   several of these strings are gate contracts, so append, never reword), and
2. a **targeted** `consoleMessageStyled(netId, kMsgChannelSystem, …)` to the session that asked.
   Targeted, not broadcast — nobody else needs to see your failed click.

The plumbing already exists and is already used for the join greeting / "left the game" lines, so
this is a sweep over the reject sites in `server_control.cc`, not new machinery.

**Why it is worth doing BEFORE most of the bugs above.** It converts a whole class of "the game is
broken / desynced" reports into self-describing ones — the player is told which rule stopped them,
and WE get told too, because the message names the rejecting branch. Several items in this
document (O, P, M, T, and the loot/container complaints) would have been diagnosed in seconds if
the rejection had been visible.

Also worth stating: a rejection must never COST anything. Item O is exactly this failure —
a rejected attack is currently spent as a turn.

## V. Arming an explosive arms the WHOLE STACK, and a stack drops one unit per action

**Observed (owner, live, and it killed him).** Owner held 5 Plastic Explosives, armed *one*, then
could not get rid of them before the timer — every drop peeled a single unit, so five drops were
needed inside a 10-second fuse. Owner's read: "activated c4 stacks with unactivated ones".

**Log evidence (denbus2, this session):**
```
control useitem_armexplosive pid=85 seconds=10      <- ONE arm action
[evt] SPAWN net=2807 pid=209 ...                    <- 209 = the ARMED/ticking proto
control invdrop pid=209 qty=5                       <- and there are FIVE of them
control invdrop pid=209 qty=4
control invdrop pid=209 qty=3
control invdrop pid=209 qty=2
control invdrop pid=209 qty=1
```
Two distinct defects, either of which alone is survivable:
1. **Arming is applied to the stack, not to one unit.** Arming should split exactly ONE item off
   the stack, convert that one to the armed proto (85 → 209 / 51 → 206), and leave the remainder
   unarmed. Right now the whole stack transitions.
2. **Dropping a stack drops one unit per action.** Known and already banked in
   [[drop-count-divergence]] ("count modal still owed") — `itemRemove` peels a fresh object per
   unit. Normally a papercut; combined with (1) and a live fuse it is lethal and unavoidable.

**Also worth checking while in here:** whether armed (209) and unarmed (85) really do share a
stack slot in the inventory UI. They are different PIDs so they should not merge; if they appear
merged, that is a third bug in the stacking/merge predicate.

**Priority note.** (1) is the actual blocker and looks contained. (2) is the long-owed drop-count
modal. A player who arms an explosive and then cannot drop it has no counterplay at all.

## W. ►► BLOCKER, ROOT-CAUSED: corpse loot on the ground is a CLIENT-SIDE HALLUCINATION

**Observed (owner, live).** Kill critters carrying items (C4 blast is the fast repro). Items
appear on the ground. **No player can pick any of them up, ever.**

**Root cause — a STATE mutation is being executed inside a PRESENTATION replay.**
`_show_death` (`actions.cc:579`) is a RECORDED callback (`PRES_CB_SHOW_DEATH`) that the VIEWER
re-executes when replaying a death sequence. Its body ends with:
```c
if (anim >= 30 && anim <= 31 && !CRITTER_SPECIAL_DEATH && !CRITTER_NO_DROP) {
    itemDropAll(obj, obj->tile);          // actions.cc:611 — NO GUARD
}
```
`src/pres_record.h:193` already specifies this as the *"itemDropAll (**guarded** STATE half,
§4.3)"* — **the guard was designed and never written.** Verified absent both at the call site and
inside `itemDropAll` (no `clientViewerActive()` / `serverLoopActive()` test anywhere in either).

So the viewer drops the corpse's inventory to the ground in its OWN world. Those ground items
exist only on the client. The server still has them inside the corpse.

**Live proof (denbus2, this session):**
```
[loot-open] net=1480 dude tile=18328 ... target tile=18528 ... dist=1   <- corpse opens FINE
f2_server: control get bad target netId=1548 ignored     (x7)
f2_server: control get bad target netId=1482 ignored     (x3)
f2_server: control get bad target netId=1528 ignored
```
`get <netId>` is the GROUND-PICKUP verb (`main.cc:778/859`). It is rejected at
`server_control.cc:1167` because `objectFindByNetId` walks **only the world object list**
(`objectFindFirst`/`objectFindNext`, `object.cc:2046`) — an item still inside a container's or
critter's inventory is not in that list, so it resolves to nullptr.

Note the corpse CONTAINER path works (`[loot-open]` fired, `dist=1`). Only the ground items fail,
which is exactly what "the ground items were never really dropped" predicts.

**Fix.** Write the missing guard: only the authoritative side runs `itemDropAll`. The server's
drop then emits the `EVENT_CONNECT`s that place the items on every client for real, and `get`
resolves. Do NOT instead teach `objectFindByNetId` to search inventories — that would paper over
a client/server state divergence by making the server accept the client's fiction.

**Two adjacent things to check in the same pass:**
1. The `anim >= 30 && anim <= 31` condition means several death animations drop NOTHING at all.
   Explosion/gib deaths likely fall outside that range — which is very probably the banked
   **BUG3 "can't-loot-gibbed"** in [[presentation-viewer-bugs]], i.e. the same investigation.
2. Audit every other RECORD callback for STATE mutations that the viewer must not re-run.
   `_show_death` was tagged as needing a guard and did not get one; assume siblings share the
   flaw. This is the general hazard: a recorded callback replays REAL ENGINE CODE on the client.

## X. ►► THE PATTERN BEHIND P, W, S AND THE DIALOG DEADLOCK: we RE-DERIVE vanilla instead of routing it

Owner named this directly: *"we keep replicating behavior of some cases instead of copying it."*
Every root cause found on 2026-07-21 is the same mistake wearing a different hat.

| bug | what vanilla already knew | what we did instead |
|---|---|---|
| dialog deadlock | `_gdProcess` is running a conversation | invented `_gdialogActive()` as the gate — true only on the engine-TALK path |
| S (run/walk) | the mover's real `run` flag | re-derived it from `durMs <= 120` on the client |
| W (loot) | `_show_death` drops inventory on annihilation | split the callback and executed its STATE half on the CLIENT only |
| P / pickup | the action IS the move animation's callback | built a PARALLEL approach-then-fire in `server_control.cc` |

**►► THE CONCRETE DEFECT (new, and it is big).** `server_anim.cc` resolves BOTH
`animationRegisterCallback` (:954) and `animationRegisterCallbackForced` (:966) as
*record-and-return* — `proc` is **never invoked server-side**. The file's own header claims this
is safe: *"no state-bearing callback is known on a server anim path"*. That claim is FALSE.
`f2_core` registers all of these through it:
```
actions.cc:1345 _obj_use            actions.cc:1565 _obj_pickup
actions.cc:1593 _obj_use_container  actions.cc:1601 scriptsRequestLooting
actions.cc:1428/1543/1675 _check_scenery_ap_cost
actions.cc:2407 _talk_to            proto_instance.cc:1847 _set_door_state_closed
```
Every one is a STATE outcome. On the dedicated server every one is silently DROPPED.

This is almost certainly **P** and the generalised "use/pickup does nothing the first time":
click 1 routes through vanilla's `actionUseObject` → move + callback → callback dropped →
nothing happens. Click 2 finds the actor already adjacent, so `server_control.cc`'s OWN
immediate-fire path runs and it works. Two parallel implementations of the same feature, one of
which silently does nothing. (Hypothesis — the mechanism is proven, this specific routing is not
yet traced. It is the same mechanism proven in W.)

**►► THE STRUCTURAL ANSWER (owner's question: port the state machine instead of re-deriving it).**
Agreed, with one refinement — the goal is not to copy vanilla's *client* state machine onto the
server, it is to stop having TWO. Concretely:
1. **Make the callback chain execute server-side.** A callback is not presentation; it is the
   action's OUTCOME, deliberately scheduled at the action frame. The server must run every
   registered callback synchronously at register time (it already applies every other anim leaf's
   outcome that way). W's fix did this for ONE callback by pointer; the correct version does it
   for ALL of them, and then W's special case disappears.
2. **Delete the parallel approach-then-fire** in `server_control.cc` once (1) lands, or demote it
   to a thin arming layer that hands off to `actionUseObject`. One pipeline, not two.
3. **Standing rule going forward:** when the engine already computes a value or owns a lifecycle,
   ROUTE IT — never re-derive it behind a threshold, a heuristic, or a second predicate. Every
   row in the table above is a violation, and each one presented as a different "mystery bug".

**Risk note.** (1) is the highest-leverage change on this list AND the most dangerous: callbacks
would begin firing on paths that have silently no-opped since the server was written, and some
core code assumes the client's `_check_scenery_ap_cost` semantics
(see [[record-purity-ap-asymmetry]] — a known trap in exactly this area). It wants the full gate
suite plus adversarial review, not a live-iteration patch.

## Y. First pickup plays no animation — item just teleports into the inventory

**Observed (owner, live, 2026-07-21, immediately after the interact-latch fix).** The pickup now
works on the first click, but that first pickup shows **no pickup animation** — the item simply
vanishes from the ground and appears in the inventory. Cosmetic; owner explicitly deferred it.

**Where to start (not investigated).** `serverControlAdvancePending` calls
`interactionEmitGesture(verb, actor, target)` immediately before `interactionFire(...)`, so the
gesture IS emitted on the fire path. Candidates, cheapest first:
- the gesture is emitted but the viewer drops/【skips it because the item's DISCONNECT (and the
  repaint added the same day) lands in the same beat and removes the target the gesture animates
  against;
- the gesture rides the record/replay channel and the FIRST one of a session is lost the way the
  first latch was (suspicious symmetry with the bug just fixed — worth one look before assuming
  it is unrelated);
- ordering: DISCONNECT applied before the gesture is presented, so there is nothing left to
  animate.

Note the phrasing "FIRST pickup" — if later pickups DO animate, that asymmetry is the whole clue
and this is very likely a lost-first-event problem, not a missing-emission one.

### ►► W REOPENED (same day, after the gate)
The CLIENT half shipped (viewer no longer drops the corpse's inventory into its own world, so the
phantom un-lootable ground items are gone). The SERVER half was **backed out**: applying the drop
inside `animationRegisterCallbackForced` made RECORD and NON-RECORD mode diverge, and
`run_record_purity.sh explosion` fails on it — items on the floor with recording on, still in the
corpse with it off. Same asymmetry class as [[record-purity-ap-asymmetry]].

Tried and REJECTED: applying it in combat.cc's `if (!animated)` branch after `critterKill`,
reading the death anim back off the corpse fid. Did not fix the gate — the explosion path
evidently does not resolve through there, and/or the fid is no longer an annihilation anim by
that point. Do not repeat without first TRACING which site the non-record explosion death
actually reaches.

**Net effect today:** annihilated critters keep their inventory inside the corpse in both modes
(self-consistent, purity-clean, and what non-record mode always did). Whether that corpse is
reachable/lootable in the viewer is UNVERIFIED and is the open question. The owner DID verify the
ground-loot path working while the server half was in — so the fix shape is right, the placement
is wrong.

## Z. Walking guard now plays the WALK cycle but still moves at run SPEED

**Observed (owner, right after item S landed).** A guard that walks now shows the correct walking
animation, but covers ground very fast — animation and speed disagree.

**Read: S did not cause this, it UNMASKED it.** Before S the viewer inferred the cycle from pace
(`durMs <= 120 -> RUNNING`), so a too-fast walker was drawn running and looked self-consistent.
Now the cycle is authoritative, so the pace being wrong is finally visible. The animation is
right; the SPEED is the bug, and it is pre-existing.

**Where to look.** The stepped walker is internally consistent on paper —
`kWalkMsPerTile = 400`, `kRunMsPerTile = 100`, and `serverWalkBeatsPerStep = msPerTile /
kServerTickDelta(100)`, i.e. walk = 4 beats = 400 ms, run = 1 beat = 100 ms. So a registry walk
should emit one hop per 4 beats stamped `durMs=400`. Check, in order:
1. Does the offending mover actually go through the stepped registry, or through
   `serverAnimWalk`'s synchronous branch? That branch sets `durMs = 0` out of combat (snap), and
   a `durMs=0` hop with `run=false` is "teleport, drawn walking".
2. Does the emitted `durMs` match the observed wall-clock gap between that netId's hops? The
   `[evt] MOVE ... durMs=` lines carry both — measure, do not reason.
3. `F2_SERVER_PACE_MS` != 100 breaks the beats<->ms identity outright: beats-per-step is derived
   from `kServerTickDelta`, but wall-clock per beat is the PACE. At PACE=50 every walk moves at
   double speed while still claiming `durMs=400`.

►► (3) is the strongest candidate and is a genuine design smell: presentation duration is derived
from SIM time while the actual cadence is WALL time, and nothing ties them together.

## AA. Presentation pacing — why one client stays in sync and another turns to soup

Companion to [[presentation-backpressure-gap]]. Written after the owner asked the right question:
*why is the second player's turn not enough to pace them?*

### The mechanism (this is the useful part)
**The turn barrier paces exactly ONE client — the one it is waiting on — and it does it through
the HUMAN, not through any measurement.** On your turn the server stops emitting and blocks on
your input; you do not act until you have seen the situation; therefore the server cannot run
ahead of you. Sync by construction.

- **Two living players feel fine** because each gets their own turn, hence their own barrier.
  This looks like a working pacing system. It is not one — nothing anywhere measures a queue.
  It is a side effect of "the server waits for a human who is looking at the screen".
- **A DEAD player never gets a turn**, so nothing ever waits for them. Their only catch-up windows
  are pauses the other players happen to take. NPC turns emit ~400 ms of glide per hop, so they
  accumulate faster than they drain. Unbounded by design, not by bad luck.

### Why it reads as SCRAMBLED, not merely late
Two lanes, two clocks, one screen:
- **STATE** (turn markers, HP, combat enter/exit, teleports) applies the INSTANT it arrives.
- **PRESENTATION** (glides, animations) drains from a queue seconds behind.

So a lagged viewer shows *now* and *then* simultaneously — an NPC's turn-END marker lands while
that NPC's walk is still playing, then a player glide from ten seconds ago runs. That is the
"npc turn end, then he moves again, then other player moves" mess. Not delay. Desynchronised
lanes.

### W/A (honest about being one): spectators SNAP
A spectator has no decision riding on the animation — they need truthful STATE, not faithful
presentation. So: past a queue-depth threshold, stop replaying paced presentation for that viewer
and snap to authority. Small, targets the observed symptom, makes a dead player's view instantly
correct instead of a scrambled replay. ►► It LOSES CONTENT and does not fix the living-player
case at all. Ship it as relief, never as the answer.

### ►► THE FIX OF RECORD (owner's, 2026-07-21): a SERVER-SIDE PRESENTATION OUTBOX
**Pace the EMISSION, not the SIMULATION.** The sim keeps resolving a whole NPC turn instantly,
exactly as today. Sequences go into a server-side outbox stamped with RELEASE TIMES computed from
their own durations (the server already knows them — it stamps `durMs`, and recorded sequences
carry their length). The wire drains that outbox on schedule.

Why this is better than the ack/lag-channel framing below:
- **Nothing ever blocks.** The server paces against its OWN clock, so no slow client can stall the
  world — the constraint that kills every lockstep variant simply does not apply.
- **No new wire message.** A scheduler in front of the sink; the protocol is unchanged.
- **It fixes the GENERAL case**, not the dead spectator: the explosion flood, item T's spam-click
  backlog, and "crowded combat desyncs" are all the same overproduction.
- **The dead-spectator problem disappears** instead of being special-cased, because pacing stops
  being tied to a claimant.
- **The two clocks already agree.** `presentationPump` (client_net.cc:454) drains continuously in
  wire order and blocks only while an animation plays — i.e. the client's consumption rate IS
  animation duration, which is exactly what the server would schedule against. Same clock at both
  ends. (And note: clients do NOT gate drawing on turns; they draw other players' actions as they
  arrive. Verified, 2026-07-21.)

►► **RISK #1, get this right first: DETERMINISM.** `server_anim.cc`'s header locks the cadence as
"logical-time-only, resolve-and-advance" and every golden depends on it. The scheduler must be
strictly EMISSION-side and inert when disabled, so harness runs stay byte-identical.
`F2_SERVER_PACE_MS` is precedent that a wall-clock throttle can coexist — but it is the thing to
prove first, not last.

►► **RISK #2: drift.** It assumes clients present at exactly the advertised duration; a hitch or a
slow machine still accumulates error with nothing correcting it. So the advisory report + snap
backstop below is still wanted — but as a SMALL CORRECTION on a system that is basically right,
not as the whole mechanism.

**It does NOT mask the remaining bugs — it unmasks them.** RTT is a constant offset, not an
accumulator; only jitter accumulates, and jitter is milliseconds against a current error measured
in whole turns. Today the flood swamps every real desync (the permanent glide-offset, the "random
glidings"), which is why they have never been diagnosable. Remove the dominant term and the small
ones become measurable. Exactly what happened when item S made run/walk authoritative and thereby
exposed the pre-existing movement-speed bug (item Z).

### The advisory layer (still wanted, but as the correction, not the mechanism)
An authoritative pacing clock: the server stamps presentation with a sequence/clock, clients
report progress back ("applied through seq N, queue depth D"), and pacing is derived from that
instead of from a human happening to look at the screen.

►► **The constraint that shapes it** (already learned, do not relearn the hard way): the SIM must
never block on a client. One slow connection would freeze the world for everyone, which
contradicts "state is authoritative and presentation-independent". Reconciliation:
- the **sim clock** stays free-running and authoritative — the server never waits on a report;
- the **presentation clock is per-client**, and each client is responsible for its own catch-up
  (compress → skip → snap) as it falls behind;
- client reports are **advisory input to emission policy** (what to send, at what fidelity),
  never a gate on simulation.

That also subsumes the turn barrier honestly: today the barrier is a hidden, human-powered
special case of exactly this mechanism. With a real pacing clock it becomes one policy among
several, instead of the only thing holding presentation together.

Same missing mechanism as items T (spam-click queue) and the explosion glide-lag, and the reason
"combat desyncs in crowded spaces" — crowding is not the cause, it is just the easiest way to
push emission past drain.
