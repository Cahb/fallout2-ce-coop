#include "loot_highlight.h"

#include <SDL.h>

#include "critter.h"
#include "display_monitor.h"
#include "item.h"
#include "kb.h"
#include "map.h"
#include "obj_types.h"
#include "object.h"
#include "proto.h"
#include "proto_types.h"
#include "tile.h"

namespace fallout {

// Whether the overlay is currently showing.
static bool gLootHighlightOn = false;
// Previous physical state of the bind key, for rising-edge (press) detection.
static int gLootHighlightPrevKey = KEY_STATE_UP;

// Classifies an object into a color-coded overlay category, returning the
// OUTLINE_TYPE_LOOT_* to draw, or 0 if it should not be highlighted.
//   - Containers (lockers, safes, bags) AND loose items are all OBJ_TYPE_ITEM;
//     itemGetType == ITEM_TYPE_CONTAINER splits container (cyan) from item (yellow).
//   - Dead critters are corpses (orange); living critters/NPCs are skipped.
//   - Doors and map transitions (stairs/ladders/elevators) are OBJ_TYPE_SCENERY
//     (green). Other generic scenery is highlighted (cyan) only when the engine
//     itself would offer a USE action (_obj_action_can_use) — this is the second
//     kind of container: openable lockers/safes/desks/bookshelves are
//     SCENERY_TYPE_GENERIC, not items, so the ITEM branch never sees them.
//     Decorative clutter has no can-use flag and stays dark.
static int lootHighlightType(Object* obj)
{
    if (obj == nullptr) {
        return 0;
    }

    if ((obj->flags & (OBJECT_HIDDEN | OBJECT_NO_HIGHLIGHT)) != 0) {
        return 0;
    }

    switch (FID_TYPE(obj->fid)) {
    case OBJ_TYPE_ITEM:
        return itemGetType(obj) == ITEM_TYPE_CONTAINER
            ? OUTLINE_TYPE_LOOT_CONTAINER
            : OUTLINE_TYPE_LOOT_ITEM;
    case OBJ_TYPE_CRITTER:
        return critterIsDead(obj) ? OUTLINE_TYPE_LOOT_CORPSE : 0;
    case OBJ_TYPE_SCENERY: {
        Proto* proto = nullptr;
        if (protoGetProto(obj->pid, &proto) == -1) {
            return 0;
        }
        switch (proto->scenery.type) {
        case SCENERY_TYPE_DOOR:
        case SCENERY_TYPE_STAIRS:
        case SCENERY_TYPE_ELEVATOR:
        case SCENERY_TYPE_LADDER_UP:
        case SCENERY_TYPE_LADDER_DOWN:
            return OUTLINE_TYPE_LOOT_DOOR;
        default:
            // Generic scenery: openable containers (lockers/safes/desks) and
            // other usable props. Key on the same predicate the cursor uses to
            // offer a USE action so clutter without the can-use flag stays dark.
            return _obj_action_can_use(obj) ? OUTLINE_TYPE_LOOT_CONTAINER : 0;
        }
    }
    default:
        return 0;
    }
}

// Walks the current elevation once and reconciles the outline set to `wantOn`:
// sets OUTLINE_TYPE_LOOT on lootables that carry no outline, clears the outlines
// we own from anything no longer wanted. Only ever touches OUTLINE_TYPE_LOOT, so
// combat/hover outlines are left alone. Returns true if anything changed (the
// caller then repaints). No cross-frame object pointers are kept, so a corpse
// that gets looted or revived between frames simply drops out of the walk.
static bool lootHighlightSync(bool wantOn)
{
    bool changed = false;

    Object* obj = objectFindFirstAtElevation(gElevation);
    while (obj != nullptr) {
        int cur = obj->outline & OUTLINE_TYPE_MASK;
        bool mine = (cur & OUTLINE_LOOT_MASK) != 0;
        int want = wantOn ? lootHighlightType(obj) : 0;

        if (want != 0) {
            if (cur == 0) {
                // Fresh object: claim it. objectSetOutline refuses if any outline
                // is already present (e.g. a hover/combat outline) — leave those
                // to their owners.
                if (objectSetOutline(obj, want, nullptr) == 0) {
                    objectEnableOutline(obj, nullptr);
                    changed = true;
                }
            } else if (mine && cur != want) {
                // Ours, but its category changed — recolor it.
                objectClearOutline(obj, nullptr);
                if (objectSetOutline(obj, want, nullptr) == 0) {
                    objectEnableOutline(obj, nullptr);
                }
                changed = true;
            }
        } else if (mine) {
            objectClearOutline(obj, nullptr);
            changed = true;
        }

        obj = objectFindNextAtElevation();
    }

    return changed;
}

void lootHighlightUpdate()
{
    int key = gPressedPhysicalKeys[SDL_SCANCODE_LALT];
    bool pressed = (key != KEY_STATE_UP && gLootHighlightPrevKey == KEY_STATE_UP);
    gLootHighlightPrevKey = key;

    if (pressed) {
        gLootHighlightOn = !gLootHighlightOn;
        displayMonitorAddMessage(const_cast<char*>(
            gLootHighlightOn ? "Highlight lootables: ON" : "Highlight lootables: OFF"));
    } else if (!gLootHighlightOn) {
        // Idle when off: nothing to reconcile or repaint.
        return;
    }

    if (lootHighlightSync(gLootHighlightOn)) {
        tileWindowRefresh();
    }
}

void lootHighlightClear()
{
    gLootHighlightOn = false;
    gLootHighlightPrevKey = KEY_STATE_UP;
    if (lootHighlightSync(false)) {
        tileWindowRefresh();
    }
}

} // namespace fallout
