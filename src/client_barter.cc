#include "client_barter.h"

#include "client_net.h"
#include "item.h"
#include "obj_types.h"
#include "object.h"
#include "proto_types.h"

namespace fallout {

static bool gBarterOpen = false;
static bool gBarterEndPending = false;
static bool gBarterStatePending = false;
static int gBarterDriverNetId = 0;
static Object* gBarterMerchant = nullptr;
static Object* gBarterPlayerTable = nullptr;
static Object* gBarterMerchantTable = nullptr;
static Object* gBarterDriverInv = nullptr;
static Object* gBarterMerchantInv = nullptr;
static int gBarterOfferValue = 0;
static int gBarterAskingValue = 0;
static bool gBarterDirty = false;

// Latched snapshot of the last STATE, applied at a safe point by
// clientBarterApplyPending (see the header for why the apply is deferred). Four
// lists in the wire order: driver inventory, merchant inventory, player table,
// merchant table.
static const int kBarterMaxRows = 64;
static int gPendingPids[4][kBarterMaxRows];
static int gPendingQtys[4][kBarterMaxRows];
static int gPendingCounts[4] = { 0, 0, 0, 0 };
static int gPendingOfferValue = 0;
static int gPendingAskingValue = 0;
static int gPendingResult = -1; // last-commit result latched with the snapshot
static int gBarterResult = -1;  // applied result, consumed once by the trade loop

// Build one throwaway container to hang mirrored stacks off.
//
// ►► A REAL CONTAINER PROTO, NOT pid -1. These were pid -1 originally, matching
// the server's own scratch tables — but that match was a non-reason here (nothing
// compares table pids) and it made these the ONLY proto-less objects in the whole
// viewer scheme. That is a crash waiting for a caller: the inventory screen
// promotes list members into subject slots (_container_enter sets _inven_dude
// from a container item), and every subject goes through proto/stat/art
// resolution, where protoGetProto(-1) hands back a null the caller dereferences.
// One such conflation already segfaulted live play. Minting them proto-backed
// closes the entire fault class BY CONSTRUCTION rather than by remembering which
// slot is safe. PROTO_ID_JESSE_CONTAINER is what the sim's own hiddenBox uses.
//
// The load-bearing property is unchanged and independent of pid: netId 0
// (objectCreateWithFidPid only mints one under serverLoopActive), so the net map
// never indexes these and no wire event can address them.
static Object* barterCreateTable()
{
    Object* table = nullptr;
    if (objectCreateWithFidPid(&table, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return nullptr;
    }
    table->flags |= OBJECT_HIDDEN;
    return table;
}

// Empty a mirror without touching anything the net map knows about. The contents
// are locally-minted item objects (netId 0), so destroying them cannot orphan a
// _net entry — the double-free trap that bites real inventory items does not
// apply here, precisely because these were never on the wire.
static void barterClearTable(Object* table)
{
    if (table == nullptr) {
        return;
    }
    Inventory* inv = &(table->data.inventory);
    while (inv->length > 0) {
        Object* item = inv->items[0].item;
        int qty = inv->items[0].quantity;
        itemRemove(table, item, qty);
        objectDestroy(item, nullptr);
    }
}

static void barterFillTable(Object* table, const int* pids, const int* qtys, int count)
{
    if (table == nullptr) {
        return;
    }
    barterClearTable(table);
    for (int i = 0; i < count; i++) {
        if (pids[i] == -1) {
            continue;
        }
        Object* item = nullptr;
        if (objectCreateWithFidPid(&item, -1, pids[i]) == -1) {
            continue;
        }
        // A freshly created item is on the ground conceptually; itemAdd moves it
        // into the container and merges it with any matching stack.
        if (itemAdd(table, item, qtys[i] > 0 ? qtys[i] : 1) != 0) {
            objectDestroy(item, nullptr);
        }
    }
}

// Free the four mirror containers (with their contents) and null the pointers.
// The immediate teardown, distinct from the LATCHED end (gBarterEndPending): this
// runs only where no trade loop holds pointers into the mirrors — from
// clientBarterFinalize (main loop) or when a stale trade is replaced on BEGIN.
static void barterDestroyMirrors()
{
    barterClearTable(gBarterPlayerTable);
    barterClearTable(gBarterMerchantTable);
    barterClearTable(gBarterDriverInv);
    barterClearTable(gBarterMerchantInv);
    if (gBarterDriverInv != nullptr) {
        objectDestroy(gBarterDriverInv, nullptr);
        gBarterDriverInv = nullptr;
    }
    if (gBarterMerchantInv != nullptr) {
        objectDestroy(gBarterMerchantInv, nullptr);
        gBarterMerchantInv = nullptr;
    }
    if (gBarterPlayerTable != nullptr) {
        objectDestroy(gBarterPlayerTable, nullptr);
        gBarterPlayerTable = nullptr;
    }
    if (gBarterMerchantTable != nullptr) {
        objectDestroy(gBarterMerchantTable, nullptr);
        gBarterMerchantTable = nullptr;
    }
}

void clientBarterOnBegin(int merchantNetId, int driverNetId)
{
    if (gBarterOpen) {
        // A BEGIN with a trade already open means we missed an END (a dropped
        // event, or a server bail that raced us). Tear the stale one down rather
        // than leaking its mirrors — failure direction is re-open, never wedge.
        // Safe to free immediately: a fresh BEGIN means the old trade's window is
        // already gone, so nothing holds pointers into the stale mirrors.
        barterDestroyMirrors();
    }

    gBarterMerchant = objectFindByNetId(merchantNetId);
    gBarterDriverNetId = driverNetId;
    gBarterPlayerTable = barterCreateTable();
    gBarterMerchantTable = barterCreateTable();
    gBarterDriverInv = barterCreateTable();
    gBarterMerchantInv = barterCreateTable();
    gBarterOfferValue = 0;
    gBarterAskingValue = 0;
    gPendingResult = -1;
    gBarterResult = -1;
    gBarterStatePending = false;
    gBarterEndPending = false;
    gBarterDirty = true;
    gBarterOpen = true;
}

void clientBarterOnState(const ClientBarterList& driverInv, const ClientBarterList& merchantInv,
    const ClientBarterList& playerTable, const ClientBarterList& merchantTable,
    int offerValue, int askingValue, int resultCode)
{
    if (!gBarterOpen) {
        return;
    }
    // ►► LATCH ONLY — DO NOT REBUILD THE MIRRORS HERE. This runs at wire-decode
    // time, pumped from inside the trade loop; freeing/recreating item objects now
    // would dangle the pointers the drag gesture and quantity dial hold. Copy the
    // rows into pending storage; clientBarterApplyPending applies them at the loop
    // top where nothing holds a mirror pointer.
    const ClientBarterList* lists[4] = { &driverInv, &merchantInv, &playerTable, &merchantTable };
    for (int list = 0; list < 4; list++) {
        int n = lists[list]->count;
        if (n < 0) {
            n = 0;
        }
        if (n > kBarterMaxRows) {
            n = kBarterMaxRows;
        }
        for (int i = 0; i < n; i++) {
            gPendingPids[list][i] = lists[list]->pids[i];
            gPendingQtys[list][i] = lists[list]->qtys[i];
        }
        gPendingCounts[list] = n;
    }
    gPendingOfferValue = offerValue;
    gPendingAskingValue = askingValue;
    gPendingResult = resultCode;
    gBarterStatePending = true;
    gBarterDirty = true;
}

int clientBarterConsumeResult()
{
    int was = gBarterResult;
    gBarterResult = -1;
    return was;
}

void clientBarterApplyPending()
{
    if (!gBarterOpen || gBarterEndPending || !gBarterStatePending) {
        return;
    }
    barterFillTable(gBarterDriverInv, gPendingPids[0], gPendingQtys[0], gPendingCounts[0]);
    barterFillTable(gBarterMerchantInv, gPendingPids[1], gPendingQtys[1], gPendingCounts[1]);
    barterFillTable(gBarterPlayerTable, gPendingPids[2], gPendingQtys[2], gPendingCounts[2]);
    barterFillTable(gBarterMerchantTable, gPendingPids[3], gPendingQtys[3], gPendingCounts[3]);
    gBarterOfferValue = gPendingOfferValue;
    gBarterAskingValue = gPendingAskingValue;
    // Surface the latched commit result once, but only for an actual commit
    // (resultCode >= 0). Plain moves carry -1 and must not clobber a result the
    // loop has not consumed yet.
    if (gPendingResult >= 0) {
        gBarterResult = gPendingResult;
        gPendingResult = -1;
    }
    gBarterStatePending = false;
}

bool clientBarterConsumeDirty()
{
    bool was = gBarterDirty;
    gBarterDirty = false;
    return was;
}

void clientBarterOnEnd()
{
    if (!gBarterOpen) {
        return;
    }
    // ►► LATCH ONLY. Same hazard as onState: this is decoded from inside the trade
    // loop. Flipping clientBarterActive() false (via gBarterEndPending) makes the
    // loop break; the actual mirror teardown is clientBarterFinalize, run from the
    // main loop once the window has closed and no loop holds a mirror pointer.
    gBarterEndPending = true;
    gBarterDirty = true;
}

void clientBarterFinalize()
{
    if (!gBarterOpen || !gBarterEndPending) {
        return;
    }
    barterDestroyMirrors();
    gBarterMerchant = nullptr;
    gBarterDriverNetId = 0;
    gBarterOfferValue = 0;
    gBarterAskingValue = 0;
    gBarterStatePending = false;
    gBarterEndPending = false;
    gBarterDirty = false;
    gBarterOpen = false;
}

bool clientBarterActive()
{
    // End-pending reads as inactive so the trade loop breaks and the main loop
    // opens no new window; the mirrors survive until clientBarterFinalize.
    return gBarterOpen && !gBarterEndPending;
}

bool clientBarterIsDriver()
{
    return gBarterOpen && gDude != nullptr && gDude->netId == gBarterDriverNetId;
}

Object* clientBarterPlayerTable() { return gBarterPlayerTable; }
Object* clientBarterMerchantTable() { return gBarterMerchantTable; }
Object* clientBarterDriver()
{
    Object* driver = objectFindByNetId(gBarterDriverNetId);
    // Fall back to the local actor rather than returning null: every caller uses
    // this for stats/art, and a null there is a crash, not a blank panel.
    return driver != nullptr ? driver : gDude;
}
Object* clientBarterDriverInv() { return gBarterDriverInv; }
Object* clientBarterMerchantInv() { return gBarterMerchantInv; }
Object* clientBarterMerchant() { return gBarterMerchant; }
int clientBarterOfferValue() { return gBarterOfferValue; }
int clientBarterAskingValue() { return gBarterAskingValue; }

} // namespace fallout
