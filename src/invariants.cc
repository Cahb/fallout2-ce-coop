#include "invariants.h"

#include <cstdio>
#include <cstdlib>

#include "critter.h"
#include "item.h"
#include "object.h"
#include "obj_types.h"
#include "proto_types.h"
#include "stat.h"

namespace fallout {

// Abort the run on a violated invariant. Prints to stderr (never stdout/the
// dump, so the success path stays byte-identical) and exits non-zero so the
// golden runner flags the case. Not marked noreturn to keep call sites simple.
static void fail(int beat, Object* obj, const char* what)
{
    fprintf(stderr,
        "\nINVARIANT VIOLATION [beat %d]: %s (obj id=%d pid=0x%08X)\n",
        beat, what, obj != nullptr ? obj->id : -1,
        obj != nullptr ? obj->pid : -1);
    fflush(stderr);
    exit(70); // EX_SOFTWARE — any non-zero is a FAIL to the golden runner.
}

// Well-formedness of one object's top-level inventory (v1: one level deep, the
// same depth the state dump walks).
static void checkInventory(int beat, Object* owner)
{
    Inventory* inventory = &(owner->data.inventory);
    if (inventory->length < 0) {
        fail(beat, owner, "inventory length negative");
    }
    for (int i = 0; i < inventory->length; i++) {
        const InventoryItem& entry = inventory->items[i];
        if (entry.item == nullptr) {
            fail(beat, owner, "inventory entry has null item");
        }
        if (entry.quantity < 1) {
            fail(beat, entry.item, "inventory entry quantity < 1");
        }
    }
}

void invariantsCheck(int beat)
{
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        // Inventory well-formedness applies to every object type (data.inventory
        // is the always-live ObjectData member, valid outside the union).
        checkInventory(beat, obj);

        if (PID_TYPE(obj->pid) != OBJ_TYPE_CRITTER) {
            continue;
        }

        // AP is spent down to zero and reset per round; it must never go
        // negative (an over-spend bug would let an actor act for free).
        if (obj->data.critter.combat.ap < 0) {
            fail(beat, obj, "critter AP negative");
        }

        // Current HP never exceeds the critter's maximum (an over-heal that
        // forgets to clamp, or a max-HP recompute drift, would break this).
        int hp = critterGetStat(obj, STAT_CURRENT_HIT_POINTS);
        int maxHp = critterGetStat(obj, STAT_MAXIMUM_HIT_POINTS);
        if (hp > maxHp) {
            fail(beat, obj, "critter current HP exceeds maximum");
        }

        // A critter marked dead (DAM_DEAD, the definitive kill flag set by
        // critterKill) must be a non-blocking corpse — UNLESS its proto carries
        // CRITTER_FLAT (those never toggle OBJECT_NO_BLOCK on death). This is
        // exactly critterKill's postcondition; it catches the corpse-state
        // regression class where a decoupled death applies damage/XP but leaves
        // the body blocking (the equivalence review's concern). Keyed on
        // DAM_DEAD (not hp<=0) to avoid the transient window between HP hitting
        // zero and death being finalized within the beat.
        if ((obj->data.critter.combat.results & DAM_DEAD) != 0
            && !_critter_flag_check(obj->pid, CRITTER_FLAT)
            && (obj->flags & OBJECT_NO_BLOCK) == 0) {
            fail(beat, obj, "dead critter still blocks (no OBJECT_NO_BLOCK)");
        }
    }
}

} // namespace fallout
