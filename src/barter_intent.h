#ifndef FALLOUT_BARTER_INTENT_H_
#define FALLOUT_BARTER_INTENT_H_

namespace fallout {

// Barter (merchant trade) intent queue — the barter analog of combat_intent /
// dialog_intent. Under the server loop the blocking barter loop
// `inventoryOpenTrade` is not driven by mouse/keyboard: the server branch drains
// these intents and calls the same core mutations the UI move-helpers and the
// 'M'/'T' verbs drive (itemMoveForce / barterAttemptTransaction /
// _barter_end_to_talk_to). Data only, no SDL/inventory dependency, so this lives
// in f2_core.
//
// Items are referenced by PROTO id + quantity, NOT by slot index: the barter
// UI's slot math is fragile reverse-indexing into a table's inventory
// (items[length - (slot + offset + 1)]) that only makes sense with the live
// window. The drain resolves pid -> Object* by scanning the relevant source
// inventory and moves it directly, bypassing the display move-helpers.

enum BarterIntentKind {
    // Move `qty` of item `pid` from the dude onto the player offer table
    // (_ptable). The player's half of the offer.
    BARTER_INTENT_OFFER_ITEM,
    // Move `qty` of item `pid` from the merchant onto the barterer table
    // (_btable). The goods the player wants in return.
    BARTER_INTENT_TAKE_ITEM,
    // Move `qty` of item `pid` OFF a table back to its owner (undo an
    // OFFER/TAKE). Scans the player table first (returns to dude), then the
    // barterer table (returns to merchant).
    BARTER_INTENT_UNOFFER_ITEM,
    // Attempt the transaction (== the 'M'/Offer button): valuate the offer and,
    // if good, commit the two-way table transfer. arg/qty unused.
    BARTER_INTENT_COMMIT,
    // Finish bartering (== the 'T'/Talk-Done button): return both tables to
    // their owners and leave the barter loop. arg/qty unused.
    BARTER_INTENT_DONE,
    // Cancel out of barter (== ESC): leave the loop without a table sweep
    // (teardown handles the tables). arg/qty unused.
    BARTER_INTENT_CANCEL,
};

struct BarterIntent {
    int kind;
    // OFFER/TAKE/UNOFFER: the item's PROTO id. Unused for COMMIT/DONE/CANCEL.
    int pid;
    // OFFER/TAKE/UNOFFER: how many to move. Unused for COMMIT/DONE/CANCEL.
    int quantity;
};

// Enqueue an intent (FIFO). pid/quantity apply to OFFER/TAKE/UNOFFER only.
void barterIntentPush(int kind, int pid, int quantity);

// Peek the front intent without removing it; returns false if the queue is
// empty (out is untouched).
bool barterIntentPeek(BarterIntent* out);

// Remove the front intent (no-op if empty).
void barterIntentPop();

// True if any intent is queued.
bool barterIntentPending();

// Drop all queued intents (called at serverRun start so runs are independent).
void barterIntentClear();

} // namespace fallout

#endif /* FALLOUT_BARTER_INTENT_H_ */
