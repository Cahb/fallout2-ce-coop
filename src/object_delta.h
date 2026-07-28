#ifndef FALLOUT_OBJECT_DELTA_H_
#define FALLOUT_OBJECT_DELTA_H_

namespace fallout {

struct Object;

// Per-beat object field-delta tracker (MP_PROTOCOL.md §6.2, P5-B). The fieldwise
// object state that has no clean per-call mutation choke point — fid, rotation,
// flags, and the critter scalars hp/radiation/poison/AP/combat-results — is
// captured by a SHADOW DIFF: snapshot each object's syncable scalars, and once
// per resolved beat compare against the shadow and emit presenter()->objectDelta
// for every object that changed. This is complete by construction (it observes
// the RESULT of any mutation regardless of how many scattered writes produced
// it), sidestepping the "flags have no setter / combat.results OR'd in 8 places"
// problem. Position (tile/elevation) is deliberately NOT tracked here — that is
// the objectMoved lifecycle event; and fid/rotation are captured at REST (the
// beat-end value), not per animation frame.
//
// Also emits worldDelta for the in-game clock (gameTimeGetTime) once per beat
// when it advances (MP_PROTOCOL.md §2 worldDelta; gvars/mvars stay server-only
// in v1).
//
// Lives in f2_core; side-effect-free (reads object fields, emits to the one-way
// presenter), so it is inert on the golden path (the null presenter no-ops the
// emits) and runs only on the server loop.

// Rebaseline: drop the shadow and re-snapshot the current world silently (no
// deltas emitted). Call at server-loop install and whenever the object set is
// wholesale-replaced (map transition), so the new map's objects are not reported
// as a flood of spurious deltas (the join snapshot carries initial state).
void objectDeltaReset();

// Diff every live object against the shadow, emit objectDelta for each changed
// object, then advance the shadow. Called at the end of each server beat
// (serverTick). Auto-rebaselines silently on a detected map change.
void objectDeltaScan();

// Tell the next objectDeltaScan NOT to emit OBJECT_DELTA_FRAME for this object: a
// presentation path (door/container open/close via presSeq/doorState) ALREADY streams
// the frame change as an animated slide, so also sending it as a frame delta would snap
// the viewer past the slide. Syncs the object's shadow frame to its current value, so
// the beat's diff sees no change. Call it AFTER the new frame is set, from the present
// path only — a SCRIPT-driven frame swap (op_anim: graves, levers) does NOT call this,
// so its frame still streams (that path has no other channel).
void objectDeltaNotePresentedFrame(Object* obj);

// Force `obj`'s FULL syncable state onto the wire on the next scan, by dropping its
// shadow entry: the scan treats a shadowless syncable object as newly created and
// emits fid+frame+light+rotation+flags outright (objectDeltaScan's else branch).
//
// ►► USE THIS AFTER REPAIRING AN OBJECT BY HAND. The scan is a DIFF, so it can only
// tell clients about a field it can see change — and a repair that writes the same
// value the shadow already holds, or that runs while a client is missing the object
// entirely, reaches nobody. Re-announcing the whole object converges every connected
// client without a rebaseline. Cheap: one map erase, one extra delta.
void objectDeltaForgetShadow(Object* obj);

// ─── GVAR streaming ─────────────────────────────────────────────────────────
//
// ►► THE GLOBAL VARIABLES WERE NEVER STREAMED. Before this, a viewer's
// gGameGlobalVars was whatever the join/rebaseline BLOB carried and then FROZE:
// the server mutates gvars constantly (scripts, quest state, karma/reputation,
// "you have holodisk X") and no wire event ever reported it. Everything a client
// renders FROM a gvar was therefore stale until the next rebaseline — the pipboy's
// holodisk list and quest list, the character screen's karma/reputation/addictions.
// Pick up a holodisk mid-session and it simply did not appear.
//
// Same shadow-diff shape as the object tracker above, and for the same reason it is
// the right shape here: ►► ~7 files write gGameGlobalVars[] DIRECTLY, bypassing the
// accessor ([[gvar-namespacing-landscape]]), so there is no mutation choke point to
// hook. Diffing the STORAGE observes every write regardless of who made it. (That
// asymmetry is worth remembering: it makes streaming easy where per-context
// NAMESPACING is hard, because namespacing needs the choke point and this does not.)
//
// Emitted as ONE new event type, deliberately, rather than as extra fields on
// worldDelta: every event is length-prefixed and unknown types are skipped whole by
// both the client and tools/replay.py, so a new type needs no reader changes and
// cannot move a gate.
void gvarDeltaReset();

// Diff every gvar against the shadow and emit the changed indices. Called once per
// resolved beat, beside objectDeltaScan.
void gvarDeltaScan();

} // namespace fallout

#endif /* FALLOUT_OBJECT_DELTA_H_ */
