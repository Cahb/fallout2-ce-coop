#ifndef FALLOUT_SERVER_ANIM_H_
#define FALLOUT_SERVER_ANIM_H_

namespace fallout {

// Beat hook for the env-gated stepped walk engine (F2_SERVER_SMOOTH_WALK=1,
// server_anim.cc): advance every in-flight walk by at most one tile. Called
// once per beat by the f2_server drive loop (server_main.cc intentsDrain) —
// the ONLY advance point, which is why animationIsBusy() must stay false
// (anything spinning on it without resolving beats would never see progress).
// No-op when no walk is in flight (always, unless the env gate is set).
void serverAnimAdvanceWalks();

// Is a stepped walk enqueued for `owner`? Polled by the interaction latch
// (server_control.cc) to distinguish "still approaching" from "arrived / never
// pathed" (INTERACTION_UX_DESIGN.md §2.3). Forward-declared Object so the header
// stays dependency-light.
struct Object;
bool serverAnimWalkInFlightFor(Object* owner);

} // namespace fallout

#endif /* FALLOUT_SERVER_ANIM_H_ */
