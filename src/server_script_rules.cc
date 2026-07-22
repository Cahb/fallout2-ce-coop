#include "server_script_rules.h"

#include <stdio.h>
#include <stdlib.h>

#include "interpreter.h"
#include "item.h"
#include "object.h"
#include "proto.h"
#include "server_players.h"

namespace fallout {

// The two opcodes a script uses to put an item in someone's inventory.
// (Numbers are the frozen bytecode ABI; names come from their registration
// sites in interpreter_extra.cc.)
#define OPCODE_ADD_OBJ_TO_INVEN 0x80D8 // add_obj_to_inven(owner, item)
#define OPCODE_ADD_MULT_OBJS_TO_INVEN 0x8116 // add_mult_objs_to_inven(owner, item, count)

// ---------------------------------------------------------------------------
// RULE: mirror ARMOR granted to a player actor onto every other player actor.
//
// Scripts hand gear to `dude_obj`, which resolves to the HOST — so the Temple's
// completion suit, and the premade's starting kit, land on P1 alone and every
// other player stands there in their underwear. Extras only appear to "inherit"
// gear when a grant happens to fire BEFORE they are spawned (they are _obj_copy
// of the host at boot), which is why this looked intermittent.
//
// DELIBERATELY NARROW — armor only, by owner ruling. The general "mirror every
// item grant" rule is wrong in two ways this one cannot be:
//   * caps are ITEMS (pid 41), so mirroring them literally prints currency;
//   * a quest token later removed with rm_obj_from_inven(dude_obj, ...) would be
//     removed from the host only, leaving the extras holding ghosts.
// Armor is gear: no economy, and nothing script-critical counts it. When the
// removal side is taught the same trick, this can widen.
//
// This is the v1 stopgap. The real answer is per-player character sheets (each
// player picks their own premade, so starting kit and rewards are per-player by
// construction) — see AGENTS notes / MP_PROPOSAL Ch 15.
// ---------------------------------------------------------------------------

// Pre-hook: the operand stack is untouched, so the arguments are still readable.
// Stack order is the REVERSE of the source call, so depth 0 is the last argument.
static void serverMirrorArmorGrant(int opcode, Program* program)
{
    if (playerActorCount() < 2) {
        return;
    }

    // add_obj_to_inven(owner, item)          -> depth 0 = item,  depth 1 = owner
    // add_mult_objs_to_inven(owner, item, n) -> depth 0 = count, depth 1 = item, depth 2 = owner
    int itemDepth = opcode == OPCODE_ADD_MULT_OBJS_TO_INVEN ? 1 : 0;

    ProgramValue itemValue;
    ProgramValue ownerValue;
    if (!programStackPeekValue(program, itemDepth, &itemValue)
        || !programStackPeekValue(program, itemDepth + 1, &ownerValue)) {
        return;
    }
    if (itemValue.opcode != VALUE_TYPE_PTR || ownerValue.opcode != VALUE_TYPE_PTR) {
        return;
    }

    Object* item = static_cast<Object*>(itemValue.pointerValue);
    Object* owner = static_cast<Object*>(ownerValue.pointerValue);
    if (item == nullptr || owner == nullptr) {
        return;
    }

    // Only a grant aimed at a PLAYER is ours to mirror; NPCs and containers are
    // ordinary world state.
    if (!playerActorIs(owner)) {
        return;
    }
    if (itemGetType(item) != ITEM_TYPE_ARMOR) {
        return;
    }

    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* other = playerActorAt(slot);
        if (other == nullptr || other == owner) {
            continue;
        }

        // A COPY per player — never the granted object itself, which the handler
        // is about to hand to `owner` (and which itemAdd would refuse anyway once
        // it has an owner).
        Object* copy = nullptr;
        if (objectCreateWithPid(&copy, item->pid) != 0 || copy == nullptr) {
            fprintf(stderr, "f2_server: mirror-grant pid=%d slot=%d create FAILED\n", item->pid, slot);
            continue;
        }

        // Inventory-resident, never in the world (objectCreateWithPid places it).
        _obj_disconnect(copy, nullptr);
        if (itemAdd(other, copy, 1) != 0) {
            objectDestroy(copy, nullptr);
            fprintf(stderr, "f2_server: mirror-grant pid=%d slot=%d add FAILED\n", item->pid, slot);
            continue;
        }

        fprintf(stderr, "f2_server: mirror-grant pid=%d -> slot=%d net=%d\n",
            item->pid, slot, other->netId);
    }
}

void serverScriptRulesInstall()
{
    interpreterAddOpcodePreHook(OPCODE_ADD_OBJ_TO_INVEN, serverMirrorArmorGrant);
    interpreterAddOpcodePreHook(OPCODE_ADD_MULT_OBJS_TO_INVEN, serverMirrorArmorGrant);
}

} // namespace fallout
