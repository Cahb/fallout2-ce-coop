#ifndef FALLOUT_STEAL_INTENT_H_
#define FALLOUT_STEAL_INTENT_H_

namespace fallout {

// Steal / pickpocket / plant intent queue — the steal analog of barter_intent.
// A dedicated server owns the steal screen: the thief's viewer renders it and
// sends what it wants moved, the server rolls the Steal skill and performs the
// transfer. The blocking session (inventory_ui.cc stealSessionRun) drains these.
//
// Items are referenced by PROTO id + quantity for the same reason barter does
// it: the loot screen's slot math is reverse-indexing into a live window's
// inventory (items[length - (slot + offset + 1)]), which means nothing on the
// server. The drain resolves pid -> Object* against the relevant side.
//
// ►► WHY A SEPARATE QUEUE FROM THE LOOT VERBS (take/put): a loot transfer is an
// unconditional itemMove, a steal transfer is a SKILL ROLL that can be caught
// and ends the session. Routing both through one verb would make "did the
// server roll?" depend on hidden session state at the trust boundary. Two verbs,
// two meanings.

enum StealIntentKind {
    // Take `qty` of item `pid` from the target's pockets. Rolls Steal.
    STEAL_INTENT_TAKE,
    // Plant `qty` of item `pid` from the thief onto the target. Rolls Steal
    // (vanilla prices planting exactly like taking, message 573/572 aside).
    STEAL_INTENT_PLANT,
    // Close the session (== ESC on the thief's screen). pid/qty unused.
    STEAL_INTENT_DONE,
};

struct StealIntent {
    int kind;
    // TAKE/PLANT: the item's PROTO id. Unused for DONE.
    int pid;
    // TAKE/PLANT: how many to move. Unused for DONE.
    int quantity;
};

// Enqueue an intent (FIFO). pid/quantity apply to TAKE/PLANT only.
void stealIntentPush(int kind, int pid, int quantity);

// Peek the front intent without removing it; returns false if the queue is
// empty (out is untouched).
bool stealIntentPeek(StealIntent* out);

// Remove the front intent (no-op if empty).
void stealIntentPop();

// True if any intent is queued.
bool stealIntentPending();

// Drop all queued intents. Called when a session opens, so a verb that raced
// the previous session's teardown cannot be spent on the next target.
void stealIntentClear();

} // namespace fallout

#endif /* FALLOUT_STEAL_INTENT_H_ */
