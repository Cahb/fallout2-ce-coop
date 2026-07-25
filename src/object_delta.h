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

} // namespace fallout

#endif /* FALLOUT_OBJECT_DELTA_H_ */
