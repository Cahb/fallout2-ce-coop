#ifndef FALLOUT_CLIENT_BARTER_H_
#define FALLOUT_CLIENT_BARTER_H_

namespace fallout {

struct Object;

// Viewer half of the barter stream (EVENT_BARTER_BEGIN/STATE/END). Mirrors
// client_dialog: the server owns the trade, this owns what the trade LOOKS like.
//
// ►► THE TABLES ARE REBUILT LOCALLY, NOT RESOLVED BY netId. The server's offer
// tables are pid==-1 scratch objects that never emit a SPAWN (object.cc
// early-returns before objectCreated), so no netId here could ever address them,
// and they are destroyed with the trade. The wire therefore carries a SNAPSHOT of
// their contents and this module builds throwaway local containers from it. Those
// mirrors carry netId 0 and are never indexed by the net map — they exist only to
// give the vanilla trade window something to draw.

// A trade opened. `merchantNetId` resolves to a real world object; `driverNetId`
// is the actor trading — every other viewer is a read-only spectator.
void clientBarterOnBegin(int merchantNetId, int driverNetId);

// The trade's whole visible state, re-sent by the server after every accepted
// move (and after a REFUSED commit, where nothing moved — that is the answer to
// a bad offer). Replaces the previous snapshot wholesale.
struct ClientBarterList {
    const int* pids;
    const int* qtys;
    int count;
};

void clientBarterOnState(const ClientBarterList& driverInv, const ClientBarterList& merchantInv,
    const ClientBarterList& playerTable, const ClientBarterList& merchantTable,
    int offerValue, int askingValue, int resultCode);

// The result of the last COMMIT (Offer button), latched from BARTER_STATE and
// applied with the snapshot. Returns a BarterResult (0 = OK, else the refusal
// reason) ONCE per commit, then -1 until the next commit. The trade loop polls
// this after a repaint to give the driver a confirm/deny cue — the server
// otherwise answers a commit only by moving items (or not).
int clientBarterConsumeResult();

// The trade ended (done, cancel, or a server-side bail). Tears the mirrors down.
void clientBarterOnEnd();

// True while a trade is open on this viewer.
bool clientBarterActive();

// True iff THIS viewer is the one driving the trade. Spectators render the same
// state but send nothing — the server would refuse them anyway ("This isn't your
// trade"), so this keeps a pointless round trip off the wire and the UI honest.
bool clientBarterIsDriver();

// The mirrors, for the render half. Null when no trade is open.
//
// ►► THE INVENTORY MIRRORS ARE NOT THE REAL OBJECTS. A spectator must see the
// DRIVER's pack, not their own, and the driver's real mirror is frozen anyway
// (the tick is parked, so no delta arrives for the whole trade). These are built
// from the snapshot instead, which is the only accurate source while a trade runs.
Object* clientBarterPlayerTable();
Object* clientBarterMerchantTable();
// The driver's REAL mirrored actor. Distinct from clientBarterDriverInv(), and
// the distinction is load-bearing: the inventory UI reads _inven_dude for stats,
// armour, encumbrance and body art (60+ sites), so it MUST be a real critter with
// a proto. Only the rendered item LIST comes from the snapshot mirror.
Object* clientBarterDriver();
Object* clientBarterDriverInv();
Object* clientBarterMerchantInv();
Object* clientBarterMerchant();

// True once since the last call: a new STATE arrived, so the window must
// repaint. Consumed (cleared) by the reader — the render loop polls this rather
// than the module reaching into the UI, keeping the dependency one-way.
bool clientBarterConsumeDirty();

// ►► DEFERRED RECONCILE. onState / onEnd run at WIRE-DECODE time, which is pumped
// from INSIDE the open trade loop (kBarter ∈ kViewerModalMask). Rebuilding the
// mirrors there — destroying and recreating every item Object — frees the very
// objects the vanilla drag gesture and quantity dial hold pointers into for their
// whole run: a use-after-free. So onState only LATCHES the snapshot rows and onEnd
// only LATCHES "ended"; the actual object churn happens here, at points the caller
// guarantees are safe:
//   * clientBarterApplyPending(): rebuild the mirror tables from the latched
//     snapshot. Called at the TOP of the trade loop, with no move-helper on the
//     stack. No-op if nothing is pending.
//   * clientBarterFinalize(): tear the mirrors down after an ended trade. Called
//     from the MAIN loop once the trade window has already closed.
void clientBarterApplyPending();
void clientBarterFinalize();

// The two server-computed valuations. NEVER recomputed here: barterComputeValue
// reads the party's best barter skill, a `dude == gDude` Master Trader check that
// a spectator would evaluate for themselves, and a script-mutated reaction.
int clientBarterOfferValue();
int clientBarterAskingValue();

} // namespace fallout

#endif /* FALLOUT_CLIENT_BARTER_H_ */
