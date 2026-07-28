#ifndef FALLOUT_CLIENT_STEAL_H_
#define FALLOUT_CLIENT_STEAL_H_

namespace fallout {

struct Object;

// Viewer half of the steal stream (EVENT_STEAL_BEGIN/STATE/END). The server owns
// the session — it rolls the Steal skill for every transfer and parks the world
// while the thief works; this owns what that LOOKS like on each machine.
//
// ►► NO MIRRORS HERE, UNLIKE client_barter, AND THAT IS THE WHOLE DIFFERENCE. A
// trade shuffles items across scratch tables that are created with pid -1 and so
// never reach the wire at all, which is why barter had to snapshot their contents
// into the event and rebuild throwaway containers to draw. A steal moves items
// between two REAL world objects that every viewer already mirrors by netId, so
// the item movement rides the ordinary OBJECT_DELTA_INVENTORY reconcile and this
// module holds nothing but two netIds and three flags. EVENT_STEAL_STATE carries
// no payload; it is the heartbeat that flushes those deltas out of a parked tick.

// A session opened: `thiefNetId` has their hands in `targetNetId`'s pockets.
// Latched only — the screen is opened from the MAIN loop, never from the decoder
// (applying wire events from inside a blocking modal frees the world under an
// open screen).
void clientStealOnBegin(int thiefNetId, int targetNetId);

// The server accepted (or refused) a move and flushed the resulting inventory
// deltas. Nothing to apply — the deltas already landed through the normal
// reconcile — so this only marks the screen dirty for a repaint.
void clientStealOnState();

// The session ended (closed, caught, or a server-side bail). Latched: the open
// screen's loop notices and returns, then the main loop finalizes.
void clientStealOnEnd();

// True while the screen should be up on this viewer. The main loop opens the
// screen when this is true and the loot GameMode is not already held (the same
// "active && not already showing" cue barter enters on).
bool clientStealActive();

// True once the server ended the session: the open loop breaks on this.
bool clientStealEndPending();

// Clear the latches after the screen has closed. Called from the main loop, at
// the same point the barter finalize runs and for the same reason: the state a
// blocking loop is reading must not be torn down while it is inside it.
void clientStealFinalize();

// The thief pressed ESC: true the FIRST time only, until the session ends. The
// close is a request the server answers with EVENT_STEAL_END, and pressing ESC
// again while that is in flight only produced duplicate `sdone`s that arrived
// after the session had closed — and got answered, once each, with "You aren't
// stealing from anyone."
bool clientStealConsumeCloseRequest();

// True iff THIS viewer is the thief. Everyone else has the same screen open as a
// WITNESS: their clicks move nothing and send nothing (the server would refuse
// them — "You're only watching" — so this keeps the round trip off the wire).
bool clientStealIsDriver();

// The two participants, resolved from the netIds. Null if either is not in this
// viewer's object map (a session for actors we cannot see is not renderable).
Object* clientStealThief();
Object* clientStealTarget();

// True once since the last call: a STATE arrived, so the screen must repaint.
bool clientStealConsumeDirty();

} // namespace fallout

#endif /* FALLOUT_CLIENT_STEAL_H_ */
