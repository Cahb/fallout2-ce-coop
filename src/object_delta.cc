#include "object_delta.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "game.h"
#include "map.h"
#include "object.h"
#include "presenter.h"
#include "proto_types.h"
#include "scripts.h"
#include "server_loop.h"
#include "server_players.h"

namespace fallout {

// A snapshot of an object's syncable scalar fields (MP_PROTOCOL.md §6.2). The
// critter-only fields are 0 for non-critters (never diffed for them).
struct ObjectShadow {
    int fid;
    int rotation;
    unsigned int flags;
    // Critter scalars.
    int hp;
    int radiation;
    int poison;
    int ap;
    int results;
    // Order-independent fingerprint of the top-level inventory (pid + quantity +
    // per-item flags + per-item intra-item union fields). Detects add/remove/
    // stack-quantity/equip changes AND intra-item changes (weapon ammoQuantity/
    // ammoTypePid, ammo/misc charges) — see objectInventoryHash. Inventory items
    // are Object*s NOT in the world tile list, so objectFindFirst never returns
    // them; this fingerprint is how all their changes become observable.
    unsigned int inventoryHash;
};

// murmur3 fmix32 finalizer — avalanches a 32-bit value so a per-item hash is
// non-linear in its inputs before being summed (see objectInventoryHash).
static unsigned int objectHashMix(unsigned int h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Commutative (order-independent) fingerprint of an owner's top-level inventory.
// Each item's (pid, quantity, flags, intra-item fields) is avalanched through
// objectHashMix BEFORE the '+' combine: keeping order-independence but breaking
// linearity in quantity, so a sum-preserving mutation (e.g. moving k units between
// two persistent stacks in one beat: -k on one, +k on the other) no longer cancels
// to the same hash (review 2026-07-13). Residual collision is now ~2^-32 random,
// not adversarially constructible.
//
// INTRA-ITEM fields (MP_PROTOCOL.md §6.2b piece 4, 2026-07-13): the per-item hash
// also folds the two ints of the item data union — for a weapon these are
// {ammoQuantity, ammoTypePid}; for the single-int variants they alias
// {ammo.quantity} / {misc.charges} / {key.keyCode} at offset 0 (offset 4 is unused
// union storage that is never written, so it is stable → contributes a constant, no
// false delta). This catches the case membership-hashing alone misses: a wielded
// weapon FIRING (ammoQuantity decrements) or an item's charges depleting WITHOUT any
// add/remove — so it rides the existing OBJECT_DELTA_INVENTORY bit. Reading
// data.item is valid because an inventory only holds OBJ_TYPE_ITEM objects. Still
// TOP-LEVEL only: an item nested inside a carried container is not walked (a deeper
// deferral, consistent with the one-level snapshot oracle).
static unsigned int objectInventoryHash(Object* obj)
{
    const Inventory* inventory = &(obj->data.inventory);
    unsigned int hash = (unsigned int)inventory->length;
    for (int i = 0; i < inventory->length; i++) {
        const InventoryItem* entry = &(inventory->items[i]);
        const Object* item = entry->item;
        unsigned int itemHash = (unsigned int)item->pid * 2654435761u
            + (unsigned int)entry->quantity * 2246822519u
            + item->flags * 40503u;
        // Intra-item union fields — guarded on OBJ_TYPE_ITEM so data.item is the
        // valid union arm. An inventory only ever holds items today (itemAdd), so
        // the guard is always true and byte-identical; it hardens against a future
        // path parking a non-item here, where data.item would alias
        // data.critter.combat.{maneuver,ap} (same offsets) and ap's per-beat churn
        // would spuriously fire the delta (review 2026-07-13).
        if (PID_TYPE(item->pid) == OBJ_TYPE_ITEM) {
            itemHash += (unsigned int)item->data.item.weapon.ammoQuantity * 2166136261u
                + (unsigned int)item->data.item.weapon.ammoTypePid * 16777619u;
        }
        hash += objectHashMix(itemHash); // '+' so item order does not matter
    }
    return hash;
}

// Keyed by Object* (NOT obj->id, which is not unique for map-placed objects —
// MP_PROTOCOL.md §2 identity risk). Server-local; rebuilt from the live object
// list each scan, so freed/reused pointers can't linger.
static std::unordered_map<Object*, ObjectShadow> gShadow;
// The map-load generation the shadow was baselined against (map.h). NOT the map
// index: re-entering the SAME map rebuilds every object while the index is
// unchanged, so an index check leaves the shadow keyed on freed Object* and the
// new objects' netIds unassigned (all 0).
static unsigned int gLastMapGeneration = 0;
// worldDelta baseline: last emitted in-game clock (MP_PROTOCOL.md §2 worldDelta).
static unsigned int gLastGameTime = 0;

static bool objectIsCritter(Object* obj)
{
    return PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER;
}

// Sync the object unless it is a client-local non-entity (mouse cursors, the
// egg) — those carry OBJECT_NO_SAVE. The dude is OBJECT_NO_SAVE too but IS the
// primary actor, so it is always included (mirrors state_dump's dude handling).
//
// KNOWN LIMITATION (deferred to P5-C, review 2026-07-13): party members ALSO
// carry OBJECT_NO_SAVE while recruited (party_member.cc), so companion deltas
// are not tracked and recruit/dismiss is a membership-toggle gap (a recruited
// critter goes untracked with no objectDestroyed; a dismissed one is baselined
// with no objectCreated). This is CONSISTENT with the snapshot oracle
// (state_dump also skips NO_SAVE except the dude), so it is a deferral aligned
// with "party is a removable v1 behavior" ([[mp-actor-architecture-principle]]),
// not a delta-vs-snapshot divergence. When party actors become first-class
// (PlayerActor), track by an explicit syncable set rather than this
// runtime-mutable flag. Likewise, objectFindFirst (below) only walks the by-tile
// buckets and skips art-type-hidden fids, so an object parked at tile -1 or one
// whose FID_TYPE flips to a hidden category is invisible to the scan for that
// span — also shared with the snapshot oracle, so no join-vs-stream divergence.
static bool objectIsSyncable(Object* obj)
{
    // Player actors are NO_SAVE but ARE in the sync domain — they ride the join
    // blob's actor appendix and take a netId slot each (MP_PROPOSAL.md Ch 5.2).
    // With an empty registry this is verbatim the old `obj == gDude`.
    return (obj->flags & OBJECT_NO_SAVE) == 0 || playerActorIs(obj);
}

static ObjectShadow objectCaptureShadow(Object* obj)
{
    ObjectShadow shadow = {};
    shadow.fid = obj->fid;
    shadow.rotation = obj->rotation;
    shadow.flags = obj->flags;
    // data.inventory is a always-live field (not in the type union), valid for
    // critters AND containers (loot) — mirrors state_dump's unconditional dump.
    shadow.inventoryHash = objectInventoryHash(obj);
    if (objectIsCritter(obj)) {
        shadow.hp = obj->data.critter.hp;
        shadow.radiation = obj->data.critter.radiation;
        shadow.poison = obj->data.critter.poison;
        shadow.ap = obj->data.critter.combat.ap;
        shadow.results = obj->data.critter.combat.results;
    }
    return shadow;
}

static unsigned int objectDiffShadow(const ObjectShadow& before, const ObjectShadow& after, bool isCritter)
{
    unsigned int mask = 0;
    if (before.fid != after.fid) mask |= OBJECT_DELTA_FID;
    if (before.rotation != after.rotation) mask |= OBJECT_DELTA_ROTATION;
    // Whole-word flags diff: fires on ANY bit including client-local/render bits.
    // Harmless (the wire carries the current word), but the P5-C NetworkPresenter
    // should mask to a syncable-flags subset to avoid wire noise (review 2026-07-13).
    if (before.flags != after.flags) mask |= OBJECT_DELTA_FLAGS;
    if (before.inventoryHash != after.inventoryHash) mask |= OBJECT_DELTA_INVENTORY;
    if (isCritter) {
        if (before.hp != after.hp) mask |= OBJECT_DELTA_HP;
        if (before.radiation != after.radiation) mask |= OBJECT_DELTA_RADIATION;
        if (before.poison != after.poison) mask |= OBJECT_DELTA_POISON;
        if (before.ap != after.ap) mask |= OBJECT_DELTA_AP;
        if (before.results != after.results) mask |= OBJECT_DELTA_COMBAT_RESULTS;
    }
    return mask;
}

void objectDeltaReset()
{
    if (serverLoopActive()) {
        objectAssignAllNetIds();
    }
    gShadow.clear();
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (objectIsSyncable(obj)) {
            gShadow.emplace(obj, objectCaptureShadow(obj));
        }
        obj = objectFindNext();
    }
    gLastMapGeneration = mapGetLoadGeneration();
    gLastGameTime = gameTimeGetTime();
}

void objectDeltaScan()
{
    // A map transition wholesale-replaces the object set; rebaseline silently
    // (the new map's state belongs to a snapshot, not the delta stream).
    if (mapGetLoadGeneration() != gLastMapGeneration) {
        objectDeltaReset();
        return;
    }

    std::unordered_map<Object*, ObjectShadow> next;
    next.reserve(gShadow.size());

    // NOTE (P5-C): objectDelta is emitted INSIDE this iteration. The null and
    // narrate presenters don't touch the object list, so it is safe today; a
    // future NetworkPresenter callback MUST NOT spawn/destroy objects here (it
    // would invalidate the find iterator) — buffer emits and flush after the loop.
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (objectIsSyncable(obj)) {
            ObjectShadow current = objectCaptureShadow(obj);
            auto it = gShadow.find(obj);
            if (it != gShadow.end()) {
                unsigned int mask = objectDiffShadow(it->second, current, objectIsCritter(obj));
                if (mask != 0) {
                    if ((mask & OBJECT_DELTA_INVENTORY) != 0 && getenv("F2_TRACE_EVENTS") != nullptr) {
                        fprintf(stderr, "[inv] DELTA net=%d invBit=1 (hash %u->%u)\n",
                            obj->netId, it->second.inventoryHash, current.inventoryHash);
                    }
                    presenter()->objectDelta(obj, mask);
                }
            } else {
                // Newly created this beat. SPAWN (objectCreated) already announced
                // its BIRTH state, but the object may have been MUTATED later in the
                // same beat — e.g. an instant kill flattens a just-spawned corpse
                // (FLAT|NO_BLOCK set after objectCreated fired), or a spawn is rotated
                // into place. Emit its beat-END syncable state as a full delta so a
                // consumer converges regardless of same-beat mutation (SPAWN =
                // existence; objectDelta = authoritative state). No-op under the null
                // presenter (goldens unaffected); does not move the object, so the
                // netstream position profile is unchanged.
                unsigned int mask = OBJECT_DELTA_FID | OBJECT_DELTA_ROTATION | OBJECT_DELTA_FLAGS;
                if (objectIsCritter(obj)) {
                    mask |= OBJECT_DELTA_HP | OBJECT_DELTA_RADIATION | OBJECT_DELTA_POISON
                        | OBJECT_DELTA_AP | OBJECT_DELTA_COMBAT_RESULTS;
                }
                presenter()->objectDelta(obj, mask);
            }
            next.emplace(obj, current);
        }
        obj = objectFindNext();
    }

    gShadow.swap(next);

    // worldDelta: in-game clock advance (gvars/mvars stay server-only in v1).
    unsigned int gameTime = gameTimeGetTime();
    if (gameTime != gLastGameTime) {
        gLastGameTime = gameTime;
        presenter()->worldDelta(WORLD_DELTA_GAMETIME);
    }
}

} // namespace fallout
