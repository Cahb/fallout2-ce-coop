#ifndef FALLOUT_CLIENT_PRESENT_H_
#define FALLOUT_CLIENT_PRESENT_H_

#include "combat_defs.h" // Attack
#include "obj_types.h"

namespace fallout {

// One owner for all viewer-side presentation of authoritative wire state
// (PRESENTATION_FSM_DESIGN.md). Merges the three former registries into a single
// module — this is step 2 "co-location" of that plan (the entry fusion + FSM state
// model is a separate follow-up; today the three concerns still live in three
// internal containers, but behind one lifecycle):
//
//   • GLIDE   (was client_anim / gWalks): plays a walk/run cycle and glides the
//             sprite between tiles for each authoritative MOVE hop instead of
//             snapping. Owns fid/frame/rotation and the between-tile pixel offset;
//             it also lags obj->tile behind authority mid-glide (present-tile
//             rebucketing, #6/#9) and reconciles at drain/cancel/stand-down.
//   • REPLAY  (was client_combat_anim / gDeferred): replays the vanilla attack /
//             take-out choreography via _action_attack + reg_anim, and HOLDS a
//             participant's final fid/flags/rotation until the replay completes
//             (deferred-final-state).
//   • DOOR    (was client_door_anim / gDoors): replays the vanilla door open/close
//             frame slide so doors don't snap and crossers don't warp through.
//
// The load-bearing invariant is unchanged: sim state (obj->tile/elevation, hp/ap,
// inventory) is ALWAYS the wire's latest authoritative value, applied at decode;
// this module only paces the PIXELS (MP_PROTOCOL.md §1: behind = snap forward,
// never replay). Off by default; only mainClientViewer enables it (presSetEnabled),
// so the headless joining client and every golden gate stay byte-identical.

// -- lifecycle (viewer-only) --------------------------------------------------
// Enable/disable the whole presentation layer. Disabling drops every in-flight
// glide/replay/door WITHOUT retracting (used at viewer shutdown).
void presSetEnabled(bool enabled);
// Discard-all: the object list itself is being replaced (map transition / new join
// blob). Ends any in-flight reg_anim sequence (one animationStop) before the old
// objects are freed, then drops all bookkeeping unapplied — the incoming baseline
// defines the new truth.
void presReset();
// Stand every in-flight glide down (retract offset + restore stand fid/frame at the
// authoritative tile) then drop them. For combat ENTER, so a mid-run critter doesn't
// freeze wearing its running fid. Replays/doors are untouched (combat can't overlap
// one on enter today).
void presStandDownAll();
// An object is being destroyed/disconnected: drop its glide and end any replay
// reg_anim that still points at it (snapping surviving participants to their held
// final state) before it is freed. Doors are never per-object-forgotten (they are
// not destroyed mid-presentation). Safe when obj has no presentation.
void presForgetObject(Object* obj);

// End only obj's WALK GLIDE, leaving any in-flight replay (and the object itself)
// alone. For a critter that DIES mid-walk: presForgetObject is too big a hammer
// there — it also cancels the reg_anim batch, which is the death animation we want
// to watch — while doing nothing leaves the walk glide alive on the corpse, where it
// keeps re-asserting its sub-tile offset over the death animation (visible in
// F2_TRACE_EVENTS as repeated "[walk] OFFSET-FOREIGN" on a dead netId).
//
// That matters beyond cosmetics: the offset moves the SPRITE without moving
// obj->tile, so the body is drawn up to a tile away from where every tile-based
// check says it is. A player walks to where the corpse looks, and the server
// refuses the loot as "not adjacent" — the exact desync the owner hit.
void presEndGlideFor(Object* obj);
// Per-frame advance, called once from the viewer render loop AFTER the presentation
// pump (client_net presentationTick). Steps glides, then the reg_anim sequences
// (_object_animate), then reaps completed replays, then reaps completed door slides
// — this ordering is load-bearing (glide runs before foreign reg_anim writes land).
void presAdvance();

// -- glide (GLIDE) ------------------------------------------------------------
// Decoder hook, called AFTER the authoritative objectSetLocation of a MOVE.
// durMs > 0 is the animate-vs-snap discriminator (MP_PROTOCOL.md §2): stepped
// walk/run hops stamp sim-ms-per-tile; teleports/scripted placements carry 0.
// hold=true queues the hop's glide WITHOUT starting it (combat sequencing, §3.d):
// the sprite parks at the origin until a matching clientAnimRelease, so a future
// turn's approach cannot glide ahead of its own TURN_START.
// `run` is AUTHORITATIVE, off the wire (EVENT_MOVE). Do not re-derive it from
// durMs — that made the animation a function of server load. See
// presenter_network.h presenterSetNextMoveRun.
void clientAnimOnMove(Object* obj, int fromTile, int toTile, int fromElevation, int toElevation, int durMs, bool hold = false, bool run = false);

// Drop any presentation glide on obj (an attack pose is taking over, or a non-MOVE
// path authoritatively repositioned it). Reconciles the lagging obj->tile to its
// authoritative destination first. Safe when obj has none.
void clientAnimCancel(Object* obj);

// The one render-channel write helper: objectSetFid + objectSetFrame(0), so the
// frame-index gotcha can never be reintroduced. Every fid write on a synced sprite
// must go through this.
void clientApplyPose(Object* obj, int fid);

// Route an authoritative fid onto an in-flight glide (applied at drain) instead of
// writing it through obj->fid mid-glide (which would kill the run). Returns false
// if the object has no active glide, so the caller writes through normally.
bool clientAnimDeferFid(Object* obj, int fid);
// Route an authoritative OBJECT_DELTA_ROTATION onto an in-flight glide (applied at
// drain, after per-hop travel facing) so a parked/gliding sprite doesn't snap-turn
// to its end direction before moving. Returns true if obj has a glide (deferred);
// false if not (apply now).
bool clientAnimDeferRotation(Object* obj, int rotation);

// Is obj mid-glide (any hop, held or playing)? The frame loop folds the dude's glide
// into the combat-busy input lock.
bool clientAnimActiveFor(Object* obj);
// Is obj gliding with PLAYABLE (released, not merely held) hops? The pump holds an
// attack until its participants stop playable-gliding, and holds a TURN_START/EXIT
// while ANY glide is playable.
bool clientAnimPlayableActiveFor(Object* obj);
bool clientAnimAnyPlayableActive();
// Count of hops (held + playable) still queued for obj's glide; -1 if none. The
// per-hex AP display polls this to tick the dude's combat-move AP down per hop.
int clientAnimHopsRemaining(Object* obj);

// Release up to `hops` of obj's held tail-hops for playback (a queued kMoveRelease
// reached the pump front). clientAnimReleaseAll frees every hold — the pump's
// self-heal when nothing is left to sequence against.
void clientAnimRelease(Object* obj, int hops);
void clientAnimReleaseAll();

// Presentation-progress heartbeat. The pump calls Note whenever it drains an event
// or an attack is animating; a gliding hop notes it internally. Held glides and
// combat reserves measure their stall backstop against LastProgressTick so patient
// waiting in a long, draining fight never trips a snap — only a true freeze does.
void clientAnimNotePresentationProgress();
unsigned int clientAnimLastProgressTick();

// -- combat replay (REPLAY) ---------------------------------------------------
// True while a replay is in flight (at least one participant still animating). The
// decoder gates on this to SERIALIZE attacks (start the next only once the previous
// is idle) and to defer end-of-combat chrome until a killing blow's death animation
// has played out. A weapon-draw take-out counts as an active replay too.
bool clientCombatAnimActive();

// Reserve deferral for an attack/take-out participant at DECODE, before its replay
// plays — closes the same-beat leak (the corpse fid / face-target rotation ride the
// same beat's OBJECT_DELTA). A reserved entry holds deltas but does NOT count active.
// Idempotent; VIEWER-only.
void clientCombatAnimReserve(Object* obj);

// Force-resolve cap for a recorded in-combat walk (up to kMaxBurstHops tiles at the walk
// pace + trailing rotate + slack) — well above the generic 2 s replay cap so a legit multi-
// tile walk isn't killed mid-stride.
constexpr unsigned int kMoveReplayCapMs = 8000;

// Promote a recorded-seq participant to Active at PLAY time — holds the pump while it
// animates (clientCombatAnimActive) and resolves its held deltas on completion via
// advanceReplays. The in-combat MOVE ops use this so a recorded walk serializes against the
// rest of the turn. capMs (0 = generic) raises the per-entry force-resolve cap. ownsMoveFrame
// (the MOVE ops) claims the front per-seq hold frame for this activation so the seq reaps its
// OWN endpoint on completion, not a later queued seq's. VIEWER-only.
void clientCombatAnimMarkActive(Object* obj, unsigned int capMs, bool ownsMoveFrame = false);

// In-combat MOVE held-delta hooks (COMBAT_MOVE_RECORD_DESIGN.md). ArmMoveHold: at the
// recorded walk's decode, reserve + flag the mover so its authoritative position/AP deltas
// are held. DeferMove/DeferAp: called from the OBJECT MOVE / delta decoder — return true
// (and stash for reconcile at walk completion) iff the object is an armed recorded-walk
// mover, else false (caller applies normally). VIEWER-only.
void clientCombatAnimArmMoveHold(Object* obj);
bool clientCombatAnimDeferMove(Object* obj, int tile, int elev);
bool clientCombatAnimDeferAp(Object* obj, int ap);
// The front (currently-executing) hold frame's authoritative END tile, or -1 if the mover
// has no held position. The MOVE replay walks to THIS (where the server's walk actually
// stopped) instead of the recorded INTENT dest, which can be the target's own occupied tile
// (an approach) → an empty path → no walk → the drift-snap teleport. VIEWER-only.
int clientCombatAnimHeldMoveTile(Object* obj);

// Hold a durMs<=0 authoritative MOVE (knockback snap) when obj has a live replay — the
// coupled slide animating the displacement. Committed at resolveHeld (the slide's action
// frame) so the slide runs from the true origin, not a stale post-snap one (bug J; pacing
// §8.5). Returns false (caller snaps as today) when obj has no replay entry. VIEWER-only.
bool clientCombatAnimDeferSnapMove(Object* obj, int tile, int elev);

// Deferred-final-state hook from the OBJECT_DELTA decoder. While obj is a replay
// participant (reserved OR active) this HOLDS its fid/flags/rotation (applied on
// completion) and returns true so the caller skips them; returns false when obj is
// not a participant. Numeric deltas (hp/ap/...) are NEVER held — apply regardless.
bool clientCombatAnimDeferDelta(Object* obj, bool hasFid, int fid, bool hasFlags, unsigned int flags,
    bool hasRot, int rot);

// A MOVE (or any non-attack reposition) on a participant ends its hold immediately —
// apply the held fid/flags/rot now and release it (knockback-rides-MOVE). Safe when
// obj is not a participant.
void clientCombatAnimNotifyReposition(Object* obj);

// Replay one decoded attack (reconstructed Attack from EVENT_ATTACK_RESULT). Registers
// the vanilla sequence via _action_attack and takes participants under deferral.
void clientCombatAnimPlay(Attack* attack);

} // namespace fallout

#endif /* FALLOUT_CLIENT_PRESENT_H_ */
