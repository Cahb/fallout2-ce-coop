#ifndef INVENTORY_H
#define INVENTORY_H

#include <functional>

#include "obj_types.h"

namespace fallout {

typedef enum Hand {
    // Item1 (Punch)
    HAND_LEFT,
    // Item2 (Kick)
    HAND_RIGHT,
    HAND_COUNT,
} Hand;

typedef void InventoryPrintItemDescriptionHandler(char* string);

void _inven_reset_dude();
void inventoryOpen();
void _adjust_ac(Object* critter, Object* oldArmor, Object* newArmor);
void inventoryOpenUseItemOn(Object* targetObj);
Object* critterGetItem2(Object* critter);
Object* critterGetItem1(Object* critter);
Object* critterGetArmor(Object* critter);
Object* objectGetCarriedObjectByPid(Object* obj, int pid);
int objectGetCarriedQuantityByPid(Object* obj, int pid);
Object* _inven_find_type(Object* obj, int itemType, int* indexPtr);
Object* _inven_find_id(Object* obj, int id);
Object* _inven_index_ptr(Object* obj, int index);
// Makes critter equip a given item in a given hand slot with an animation.
// 0 - left hand, 1 - right hand. If item is armor, hand value is ignored.
int _inven_wield(Object* critter, Object* item, int hand);
// Same as inven_wield but allows to wield item without animation.
int _invenWieldFunc(Object* critter, Object* item, int hand, bool animate);
// Makes critter unequip an item in a given hand slot with an animation.
int _inven_unwield(Object* critter, int hand);
// Same as inven_unwield but allows to unwield item without animation.
int _invenUnwieldFunc(Object* critter, int hand, bool animate);
int inventoryOpenLooting(Object* looter, Object* target);
int inventoryOpenStealing(Object* thief, Object* target);
void inventoryOpenTrade(int win, Object* barterer, Object* playerTable, Object* bartererTable, int barterMod);

// Park the server tick inside an OPEN trade and service the control channel
// until the driver's next barter intent lands (the barter twin of
// gameDialogSetServerPump). Returning false bails the trade.
//
// Without this the headless drain is FIXTURE-shaped, not interactive: it breaks
// the instant the intent queue runs dry, which is right for a golden that
// pre-queues every move but means a live trade closes before the player has
// touched anything. Installed by f2_server only; nullptr everywhere else, which
// is what keeps the goldens on their original break-on-empty behavior.
void barterSetServerPump(std::function<bool()> pump);

// VIEWER-side trade window: render the mirrored tables the barter stream built,
// and hold the screen until the server ends the trade. Not a trade -- the trade
// is the server's; this is what it looks like. See the note at the definition.
void inventoryOpenTradeViewer(Object* merchant, Object* playerTable, Object* merchantTable);
int _inven_set_timer(Object* item);
Object* inven_get_current_target_obj();

} // namespace fallout

#endif /* INVENTORY_H */
