#ifndef FALLOUT_INVENTORY_UI_H_
#define FALLOUT_INVENTORY_UI_H_

#include "obj_types.h"

namespace fallout {

// Client-side inventory UI seam for inventory.cc (REWRITE_PLAN, TU split).
//
// The modal inventory/loot/steal/barter/quantity windows and all their
// rendering/cursor/context-menu chrome that used to live in inventory.cc now
// live in inventory_ui.cc (the f2_client side). The public UI entry points
// (inventoryOpen, inventoryOpenUseItemOn, inventoryOpenLooting,
// inventoryOpenStealing, inventoryOpenTrade, _inven_set_timer,
// inven_get_current_target_obj) stay declared in inventory.h since existing
// callers include that.
//
// This header declares the pieces the split newly exposes: five shared file-
// statics. Unlike the other TU splits, the data flows inward as well as out --
// the moved UI populates the equipped-item/dude slots, and core inventory.cc
// (critterGetItem1/critterGetItem2/critterGetArmor and _inven_reset_dude) reads
// and writes them. Their definitions stay in inventory.cc (core owns them);
// they were de-static'd for this seam. This exposure is the mechanical stand-in
// for the eventual client-init hook (see CMake split task).

// Dude whose inventory is currently open, and its cached armor fid marker.
extern Object* _inven_dude;
extern int _inven_pid;

// Equipped-slot caches populated by the inventory UI and read back by core
// critterGet* while the window is open.
extern Object* gInventoryArmor;
extern Object* gInventoryLeftHandItem;
extern Object* gInventoryRightHandItem;

} // namespace fallout

#endif /* FALLOUT_INVENTORY_UI_H_ */
