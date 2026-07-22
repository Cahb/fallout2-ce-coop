#include "inventory_ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "inventory.h"
#include "item.h"

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "barter_intent.h"
#include "client_barter.h"
#include "client_dialog.h"
#include "msg_channel.h"
#include "server_players.h"
#include "client_net.h" // clientViewerActive — inventory is view-only on the wire viewer (Slice 3)
#include "color.h"
#include "combat.h"
#include "combat_ai.h"
#include "critter.h"
#include "dbox.h"
#include "debug.h"
#include "dialog.h"
#include "display_monitor.h"
#include "draw.h"
#include "game.h"
#include "game_dialog.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "light.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "platform_compat.h"
#include "presenter.h"
#include "proto.h"
#include "proto_instance.h"
#include "random.h"
#include "reaction.h"
#include "scripts.h"
#include "server_loop.h"
#include "sfall_config.h"
#include "skill.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "trait.h"
#include "window_manager.h"

// Client-side inventory UI seam extracted from inventory.cc (REWRITE_PLAN, TU
// split). Pure presentation/input: every modal inventory/loot/steal/barter/
// quantity window plus its rendering, cursor, and context-menu chrome. Core
// inventory.cc retains the SDL-free inventory data-structure and equip logic
// (critterGet*, _inven_wield/_inven_unwield, _inven_find_*, _adjust_ac,
// _inven_reset_dude). The public UI entry points (inventoryOpen, inventoryOpen*,
// _inven_set_timer, inven_get_current_target_obj) stay declared in inventory.h
// since existing callers include that. The only seam is five shared file-
// statics that core reads/writes and this layer reads/writes; they are defined
// in inventory.cc, de-static'd, and viewed here through inventory_ui.h. This
// move is mechanical and replay-identical.

namespace fallout {

#define INVENTORY_WINDOW_X 80
#define INVENTORY_WINDOW_Y 0

#define INVENTORY_TRADE_WINDOW_X 80
#define INVENTORY_TRADE_WINDOW_Y 290
#define INVENTORY_TRADE_WINDOW_WIDTH 480
#define INVENTORY_TRADE_WINDOW_HEIGHT 180

#define INVENTORY_LARGE_SLOT_WIDTH 90
#define INVENTORY_LARGE_SLOT_HEIGHT 61

#define INVENTORY_SLOT_WIDTH 64
#define INVENTORY_SLOT_HEIGHT 48

#define INVENTORY_LEFT_HAND_SLOT_X 154
#define INVENTORY_LEFT_HAND_SLOT_Y 286
#define INVENTORY_LEFT_HAND_SLOT_MAX_X (INVENTORY_LEFT_HAND_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_LEFT_HAND_SLOT_MAX_Y (INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_RIGHT_HAND_SLOT_X 245
#define INVENTORY_RIGHT_HAND_SLOT_Y 286
#define INVENTORY_RIGHT_HAND_SLOT_MAX_X (INVENTORY_RIGHT_HAND_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_RIGHT_HAND_SLOT_MAX_Y (INVENTORY_RIGHT_HAND_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_ARMOR_SLOT_X 154
#define INVENTORY_ARMOR_SLOT_Y 183
#define INVENTORY_ARMOR_SLOT_MAX_X (INVENTORY_ARMOR_SLOT_X + INVENTORY_LARGE_SLOT_WIDTH)
#define INVENTORY_ARMOR_SLOT_MAX_Y (INVENTORY_ARMOR_SLOT_Y + INVENTORY_LARGE_SLOT_HEIGHT)

#define INVENTORY_TRADE_SCROLLER_Y 30
#define INVENTORY_TRADE_INNER_SCROLLER_Y 20

#define INVENTORY_TRADE_LEFT_SCROLLER_X 29
#define INVENTORY_TRADE_LEFT_SCROLLER_Y INVENTORY_TRADE_SCROLLER_Y

#define INVENTORY_TRADE_RIGHT_SCROLLER_X 388
#define INVENTORY_TRADE_RIGHT_SCROLLER_Y INVENTORY_TRADE_SCROLLER_Y

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_X 165
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y INVENTORY_TRADE_INNER_SCROLLER_Y

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X 250
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y INVENTORY_TRADE_INNER_SCROLLER_Y

#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X 0
#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X 165
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X 250
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X 395
#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y 10
#define INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X (INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_LOOT_LEFT_SCROLLER_X 180
#define INVENTORY_LOOT_LEFT_SCROLLER_Y 37
#define INVENTORY_LOOT_LEFT_SCROLLER_MAX_X (INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_LOOT_RIGHT_SCROLLER_X 297
#define INVENTORY_LOOT_RIGHT_SCROLLER_Y 37
#define INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X (INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_SCROLLER_X 46
#define INVENTORY_SCROLLER_Y 35
#define INVENTORY_SCROLLER_MAX_X (INVENTORY_SCROLLER_X + INVENTORY_SLOT_WIDTH)

#define INVENTORY_BODY_VIEW_WIDTH 60
#define INVENTORY_BODY_VIEW_HEIGHT 100

#define INVENTORY_PC_BODY_VIEW_X 176
#define INVENTORY_PC_BODY_VIEW_Y 37
#define INVENTORY_PC_BODY_VIEW_MAX_X (INVENTORY_PC_BODY_VIEW_X + INVENTORY_BODY_VIEW_WIDTH)
#define INVENTORY_PC_BODY_VIEW_MAX_Y (INVENTORY_PC_BODY_VIEW_Y + INVENTORY_BODY_VIEW_HEIGHT)

#define INVENTORY_LOOT_RIGHT_BODY_VIEW_X 422
#define INVENTORY_LOOT_RIGHT_BODY_VIEW_Y 35

#define INVENTORY_LOOT_LEFT_BODY_VIEW_X 44
#define INVENTORY_LOOT_LEFT_BODY_VIEW_Y 35

#define INVENTORY_SUMMARY_X 297
#define INVENTORY_SUMMARY_Y 44
#define INVENTORY_SUMMARY_MAX_X 440

#define INVENTORY_WINDOW_WIDTH 499
#define INVENTORY_USE_ON_WINDOW_WIDTH 292
#define INVENTORY_LOOT_WINDOW_WIDTH 537
#define INVENTORY_TRADE_WINDOW_WIDTH 480
#define INVENTORY_TIMER_WINDOW_WIDTH 259

#define INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH 640
#define INVENTORY_TRADE_BACKGROUND_WINDOW_HEIGHT 480
#define INVENTORY_TRADE_WINDOW_OFFSET ((INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH - INVENTORY_TRADE_WINDOW_WIDTH) / 2)

#define INVENTORY_SLOT_PADDING 4

#define INVENTORY_SCROLLER_X_PAD (INVENTORY_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_SCROLLER_Y_PAD (INVENTORY_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_LOOT_LEFT_SCROLLER_X_PAD (INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD (INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_LOOT_RIGHT_SCROLLER_X_PAD (INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_LOOT_RIGHT_SCROLLER_Y_PAD (INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_LEFT_SCROLLER_X_PAD (INVENTORY_TRADE_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD (INVENTORY_TRADE_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_RIGHT_SCROLLER_X_PAD (INVENTORY_TRADE_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_RIGHT_SCROLLER_Y_PAD (INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD (INVENTORY_TRADE_INNER_LEFT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD (INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X + INVENTORY_SLOT_PADDING)
#define INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD (INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_SLOT_PADDING)

#define INVENTORY_SLOT_WIDTH_PAD (INVENTORY_SLOT_WIDTH - INVENTORY_SLOT_PADDING * 2)
#define INVENTORY_SLOT_HEIGHT_PAD (INVENTORY_SLOT_HEIGHT - INVENTORY_SLOT_PADDING * 2)

#define INVENTORY_NORMAL_WINDOW_PC_ROTATION_DELAY (1000U / ROTATION_COUNT)
#define INVENTORY_FRM_COUNT 12

typedef enum InventoryArrowFrm {
    INVENTORY_ARROW_FRM_LEFT_ARROW_UP,
    INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN,
    INVENTORY_ARROW_FRM_RIGHT_ARROW_UP,
    INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN,
    INVENTORY_ARROW_FRM_COUNT,
} InventoryArrowFrm;

typedef enum InventoryWindowCursor {
    INVENTORY_WINDOW_CURSOR_HAND,
    INVENTORY_WINDOW_CURSOR_ARROW,
    INVENTORY_WINDOW_CURSOR_PICK,
    INVENTORY_WINDOW_CURSOR_MENU,
    INVENTORY_WINDOW_CURSOR_BLANK,
    INVENTORY_WINDOW_CURSOR_COUNT,
} InventoryWindowCursor;

typedef enum InventoryWindowType {
    // Normal inventory window with quick character sheet.
    INVENTORY_WINDOW_TYPE_NORMAL,

    // Narrow inventory window with just an item scroller that's shown when
    // a "Use item on" is selected from context menu.
    INVENTORY_WINDOW_TYPE_USE_ITEM_ON,

    // Looting/strealing interface.
    INVENTORY_WINDOW_TYPE_LOOT,

    // Barter interface.
    INVENTORY_WINDOW_TYPE_TRADE,

    // Supplementary "Move items" window. Used to set quantity of items when
    // moving items between inventories.
    INVENTORY_WINDOW_TYPE_MOVE_ITEMS,

    // Supplementary "Set timer" window. Internally it's implemented as "Move
    // items" window but with timer overlay and slightly different adjustment
    // mechanics.
    INVENTORY_WINDOW_TYPE_SET_TIMER,

    INVENTORY_WINDOW_TYPE_COUNT,
} InventoryWindowType;

typedef struct InventoryWindowConfiguration {
    int frmId; // artId
    int width;
    int height;
    int x;
    int y;
} InventoryWindowDescription;

typedef struct InventoryCursorData {
    Art* frm;
    unsigned char* frmData;
    int width;
    int height;
    int offsetX;
    int offsetY;
    CacheEntry* frmHandle;
} InventoryCursorData;

typedef enum InventoryMoveResult {
    INVENTORY_MOVE_RESULT_FAILED,
    INVENTORY_MOVE_RESULT_CAUGHT_STEALING,
    INVENTORY_MOVE_RESULT_SUCCESS,
} InventoryMoveResult;

static int inventoryMessageListInit();
static int inventoryMessageListFree();
static bool _setup_inventory(int inventoryWindowType);
static void _exit_inventory(bool shouldEnableIso);
static void _display_inventory(int stackOffset, int draggedSlotIndex, int inventoryWindowType);
static void _display_target_inventory(int stackOffset, int dragSlotIndex, Inventory* inventory, int inventoryWindowType);
static void _display_inventory_info(Object* item, int quantity, unsigned char* dest, int pitch, bool isDragged);
static void _display_body(int fid, int inventoryWindowType);
static int inventoryCommonInit();
static void inventoryCommonFree();
static void inventorySetCursor(int cursor);
static void inventoryItemSlotOnMouseEnter(int btn, int keyCode);
static void inventoryItemSlotOnMouseExit(int btn, int keyCode);
static void _inven_update_lighting(Object* activeItem);
static void _inven_pickup(int keyCode, int indexOffset);
static void _switch_hand(Object* sourceItem, Object** targetSlot, Object** sourceSlot, int itemIndex);
static void _adjust_fid();
static void inventoryRenderSummary();
static int _inven_from_button(int keyCode, Object** outItem, Object*** outItemSlot, Object** outOwner);
static void inventoryRenderItemDescription(char* string);
static void inventoryExamineItem(Object* critter, Object* item);
static void inventoryWindowOpenContextMenu(int eventCode, int inventoryWindowType);
static InventoryMoveResult _move_inventory(Object* item, int slotIndex, Object* targetObj, bool isPlanting);
static void _barter_move_inventory(Object* item, int quantity, int slotIndex, int indexOffset, Object* npc, Object* sourceTable, bool fromDude);
static void _barter_move_from_table_inventory(Object* item, int quantity, int slotIndex, Object* npc, Object* sourceTable, bool fromDude);
static void inventoryWindowRenderInnerInventories(int win, Object* leftTable, Object* rightTable, int draggedSlotIndex);
static void _container_enter(int keyCode, int inventoryWindowType);
static void _container_exit(int keyCode, int inventoryWindowType);
static int _drop_into_container(Object* container, Object* item, int sourceIndex, Object** itemSlot, int quantity);
static int _drop_ammo_into_weapon(Object* weapon, Object* ammo, Object** ammoItemSlot, int quantity, int keyCode);
static void _draw_amount(int value, int inventoryWindowType);
static int inventoryQuantitySelect(int inventoryWindowType, Object* item, int maximum);
static int inventoryQuantityWindowInit(int inventoryWindowType, Object* item);
static int inventoryQuantityWindowFree(int inventoryWindowType);

// 0x46E6D0
static const int gSummaryStats[7] = {
    STAT_CURRENT_HIT_POINTS,
    STAT_ARMOR_CLASS,
    STAT_DAMAGE_THRESHOLD,
    STAT_DAMAGE_THRESHOLD_LASER,
    STAT_DAMAGE_THRESHOLD_FIRE,
    STAT_DAMAGE_THRESHOLD_PLASMA,
    STAT_DAMAGE_THRESHOLD_EXPLOSION,
};

// 0x46E6EC
static const int gSummaryStats2[7] = {
    STAT_MAXIMUM_HIT_POINTS,
    -1,
    STAT_DAMAGE_RESISTANCE,
    STAT_DAMAGE_RESISTANCE_LASER,
    STAT_DAMAGE_RESISTANCE_FIRE,
    STAT_DAMAGE_RESISTANCE_PLASMA,
    STAT_DAMAGE_RESISTANCE_EXPLOSION,
};

// 0x46E708
static const int gInventoryArrowFrmIds[INVENTORY_ARROW_FRM_COUNT] = {
    122, // left arrow up
    123, // left arrow down
    124, // right arrow up
    125, // right arrow down
};

// The number of items to show in scroller.
//
// 0x519054
static int gInventorySlotsCount = 6;

// 0x519060
static bool _inven_is_initialized = false;

// 0x519064
static int _inven_display_msg_line = 1;

// 0x519068
static const InventoryWindowDescription gInventoryWindowDescriptions[INVENTORY_WINDOW_TYPE_COUNT] = {
    { 48, INVENTORY_WINDOW_WIDTH, 377, 80, 0 },
    { 113, INVENTORY_USE_ON_WINDOW_WIDTH, 376, 80, 0 },
    { 114, INVENTORY_LOOT_WINDOW_WIDTH, 376, 80, 0 },
    { 111, INVENTORY_TRADE_WINDOW_WIDTH, 180, 80, 290 },
    { 305, INVENTORY_TIMER_WINDOW_WIDTH, 162, 140, 80 },
    { 305, INVENTORY_TIMER_WINDOW_WIDTH, 162, 140, 80 },
};

// 0x5190E0
static bool _dropped_explosive = false;

// 0x5190E4
static int gInventoryScrollUpButton = -1;

// 0x5190E8
static int gInventoryScrollDownButton = -1;

// 0x5190EC
static int gSecondaryInventoryScrollUpButton = -1;

// 0x5190F0
static int gSecondaryInventoryScrollDownButton = -1;

// 0x5190F4
static unsigned int gInventoryWindowDudeRotationTimestamp = 0;

// 0x5190F8
static int gInventoryWindowDudeRotation = 0;

// 0x5190FC
static const int gInventoryWindowCursorFrmIds[INVENTORY_WINDOW_CURSOR_COUNT] = {
    286, // pointing hand
    250, // action arrow
    282, // action pick
    283, // action menu
    266, // blank
};

// 0x519110
static Object* _last_target = nullptr;

// 0x519114
static const int _act_use[4] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_USE,
    GAME_MOUSE_ACTION_MENU_ITEM_DROP,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// 0x519124
static const int _act_no_use[3] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_DROP,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// 0x519130
static const int _act_just_use[3] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_USE,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// 0x51913C
static const int _act_nothing[2] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// 0x519144
static const int _act_weap[4] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD,
    GAME_MOUSE_ACTION_MENU_ITEM_DROP,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// 0x519154
static const int _act_weap2[3] = {
    GAME_MOUSE_ACTION_MENU_ITEM_LOOK,
    GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD,
    GAME_MOUSE_ACTION_MENU_ITEM_CANCEL,
};

// Scroll offsets to target inventory for every container nesting level (stack).
// 0x59E7EC
static int _target_stack_offset[10];

// inventory.msg
//
// 0x59E814
static MessageList gInventoryMessageList;

// Current target critter or container for every nesting level (stack).
// 0x59E81C
static Object* _target_stack[10];

// Scroll offsets to main inventory for every container nesting level (stack).
// 0x59E844
static int _stack_offset[10];

// Current critter or container for every nesting level (stack).
// 0x59E86C
static Object* _stack[10];

// 0x59E894
static int _mt_wid;

// Current barter price modifier, set from scripts.
// 0x59E898
static int _barter_mod;

// 0x59E89C
static int _btable_offset;

// 0x59E8A0
static int _ptable_offset;

// 0x59E8A4
static Inventory* _ptable_pud;

// 0x59E8A8
static InventoryCursorData gInventoryCursorData[INVENTORY_WINDOW_CURSOR_COUNT];

// 0x59E934
static Object* _ptable;

// 0x59E938
static InventoryPrintItemDescriptionHandler* gInventoryPrintItemDescriptionHandler;

// 0x59E93C
static int _im_value;

// 0x59E940
static int gInventoryCursor;

// 0x59E944
static Object* _btable;

// Current nesting level for viewing target's bag/backpack contents.
// 0x59E948
static int _target_curr_stack;

// 0x59E94C
static Inventory* _btable_pud;

// 0x59E950
static bool _inven_ui_was_disabled;

// Rotating character's fid.
//
// 0x59E95C
static int gInventoryWindowDudeFid;

// 0x59E960
static Inventory* _pud;

// 0x59E964
static int gInventoryWindow;

// Current nesting level for viewing bag/backpack contents.
// 0x59E96C
static int _curr_stack;

// 0x59E970
static int gInventoryWindowMaxY;

// 0x59E974
static int gInventoryWindowMaxX;

// 0x59E978
static Inventory* _target_pud;

// 0x59E97C
static int _barter_back_win;

static FrmImage _inventoryFrmImages[INVENTORY_FRM_COUNT];
static FrmImage _moveFrmImages[8];

// inventory_msg_init
// 0x46E73C
static int inventoryMessageListInit()
{
    char path[COMPAT_MAX_PATH];

    if (!messageListInit(&gInventoryMessageList))
        return -1;

    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "inventry.msg");
    if (!messageListLoad(&gInventoryMessageList, path))
        return -1;

    return 0;
}

// inventory_msg_free
// 0x46E7A0
static int inventoryMessageListFree()
{
    messageListFree(&gInventoryMessageList);
    return 0;
}

// 0x46E7B0
void inventoryOpen()
{
    // On a WIRE VIEWER both in-combat gates below have ALREADY been decided by
    // the server, which owns the fight: it checked that it is our turn and
    // charged the AP before granting the screen (server_control.cc invopen). The
    // viewer must not re-decide either one.
    //
    // Re-deciding would not merely duplicate work, it would break both ways:
    // _combat_whose_turn() is never set on a viewer (combat runs server-side), so
    // the turn check would read nullptr and silently refuse to open a screen the
    // player has already paid 4 AP for; and inventoryApCostApply would charge
    // those 4 AP a SECOND time against the local mirror, showing a wrong AP bar
    // until the next reconcile overwrote it.
    bool serverRuledOnCombatEntry = clientViewerActive();

    if (isInCombat() && !serverRuledOnCombatEntry) {
        if (_combat_whose_turn() != _inven_dude) {
            return;
        }
    }

    ScopedGameMode gm(GameMode::kInventory);

    if (inventoryCommonInit() == -1) {
        return;
    }

    if (isInCombat() && !serverRuledOnCombatEntry) {
        if (_inven_dude == gDude) {
            // Ledger H-1: AP rule extracted to core (item.cc); the UI keeps
            // only the failure feedback.
            if (!inventoryApCostApply(gDude)) {
                // You don't have enough action points to use inventory
                MessageListItem messageListItem;
                messageListItem.num = 19;
                if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                    presenter()->consoleMessage(messageListItem.text);
                }

                // NOTE: Uninline.
                inventoryCommonFree();

                return;
            }
        }
    }

    Object* oldArmor = critterGetArmor(_inven_dude);
    bool isoWasEnabled = _setup_inventory(INVENTORY_WINDOW_TYPE_NORMAL);
    reg_anim_clear(_inven_dude);
    inventoryRenderSummary();
    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);

    for (;;) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        // SFALL: Close with 'I'.
        if (keyCode == KEY_ESCAPE || keyCode == KEY_UPPERCASE_I || keyCode == KEY_LOWERCASE_I) {
            break;
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        _display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);

        if (gameGetState() == GAME_STATE_5) {
            break;
        }

        // Wire viewer: a live dude-inventory reconcile (a drop/give/consume streamed
        // in by the service ticker's pump, Slice 3b) mutated the mirror — repaint the
        // list + summary so the change shows immediately (this loop otherwise repaints
        // only on user events, so an async drop would linger visibly). Clamp the scroll
        // offset in case items were removed out from under it.
        if (clientViewerActive() && clientViewerConsumeDudeInvDirty()) {
            if (_stack_offset[_curr_stack] > _pud->length - gInventorySlotsCount) {
                _stack_offset[_curr_stack] = _pud->length - gInventorySlotsCount;
            }
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            inventoryRenderSummary();
            // BLIT IT. Neither _display_inventory nor inventoryRenderSummary reaches
            // the screen on their own — they draw into the window buffer, and the
            // vanilla call sites all follow up with a refresh because they run from
            // a user event that was going to refresh anyway. This block does not: it
            // fires on an ASYNC server reconcile, so without this the new contents
            // sit in the buffer, invisible, until some unrelated event blits the
            // window (the tell is a sprite that partially appears as the mouse
            // cursor's own dirty-rect drags across it).
            windowRefresh(gInventoryWindow);
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X) {
            showQuitConfirmationDialog();
        } else if (keyCode == KEY_HOME) {
            _stack_offset[_curr_stack] = 0;
            _display_inventory(0, -1, INVENTORY_WINDOW_TYPE_NORMAL);
        } else if (keyCode == KEY_ARROW_UP) {
            if (_stack_offset[_curr_stack] > 0) {
                _stack_offset[_curr_stack] -= 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            }
        } else if (keyCode == KEY_PAGE_UP) {
            _stack_offset[_curr_stack] -= gInventorySlotsCount;
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
        } else if (keyCode == KEY_END) {
            _stack_offset[_curr_stack] = _pud->length - gInventorySlotsCount;
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (gInventorySlotsCount + _stack_offset[_curr_stack] < _pud->length) {
                _stack_offset[_curr_stack] += 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
            }
        } else if (keyCode == KEY_PAGE_DOWN) {
            int v12 = gInventorySlotsCount + _stack_offset[_curr_stack];
            int v13 = v12 + gInventorySlotsCount;
            _stack_offset[_curr_stack] = v12;
            int v14 = _pud->length;
            if (v13 >= _pud->length) {
                int v15 = v14 - gInventorySlotsCount;
                _stack_offset[_curr_stack] = v14 - gInventorySlotsCount;
                if (v15 < 0) {
                    _stack_offset[_curr_stack] = 0;
                }
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
        } else if (keyCode == 2500) {
            _container_exit(keyCode, INVENTORY_WINDOW_TYPE_NORMAL);
        } else {
            if ((mouseGetEvent() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_HAND) {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);
                } else if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
                    inventoryRenderSummary();
                    windowRefresh(gInventoryWindow);
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1008) {
                    // Wire viewer (Slice 3b): the drag-drop (_inven_pickup) and ctx-menu are
                    // ALLOWED to run — the drag is purely local/visual and vanilla mutates
                    // only at the drop-RESOLUTION leaf. Each such leaf is guarded on
                    // clientViewerActive() to fire a claim-gated wire verb + skip the local
                    // mutation, so the server-authoritative inventory stays the source of
                    // truth (Slice 2 reconciles the result a frame later).
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_NORMAL);
                    } else {
                        _inven_pickup(keyCode, _stack_offset[_curr_stack]);
                    }
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_stack_offset[_curr_stack] > 0) {
                            _stack_offset[_curr_stack] -= 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
                        }
                    } else if (wheelY < 0) {
                        if (gInventorySlotsCount + _stack_offset[_curr_stack] < _pud->length) {
                            _stack_offset[_curr_stack] += 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_NORMAL);
                        }
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    _inven_dude = _stack[0];
    _adjust_fid();

    if (_inven_dude == gDude) {
        Rect rect;
        objectSetFid(_inven_dude, gInventoryWindowDudeFid, &rect);
        tileWindowRefreshRect(&rect, _inven_dude->elevation);
    }

    Object* newArmor = critterGetArmor(_inven_dude);
    if (_inven_dude == gDude) {
        if (oldArmor != newArmor) {
            interfaceRenderArmorClass(true);
        }
    }

    _exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inventoryCommonFree();

    if (_inven_dude == gDude) {
        interfaceUpdateItems(false, INTERFACE_ITEM_ACTION_DEFAULT, INTERFACE_ITEM_ACTION_DEFAULT);
    }
}

// 0x46EC90
static bool _setup_inventory(int inventoryWindowType)
{
    _dropped_explosive = 0;
    _curr_stack = 0;
    _stack_offset[0] = 0;
    gInventorySlotsCount = 6;
    _pud = &(_inven_dude->data.inventory);
    _stack[0] = _inven_dude;

    if (inventoryWindowType <= INVENTORY_WINDOW_TYPE_LOOT) {
        const InventoryWindowDescription* windowDescription = &(gInventoryWindowDescriptions[inventoryWindowType]);

        // Maintain original position in original resolution, otherwise center it.
        int inventoryWindowX = screenGetWidth() != 640
            ? (screenGetWidth() - windowDescription->width) / 2
            : INVENTORY_WINDOW_X;
        int inventoryWindowY = screenGetHeight() != 480
            ? (screenGetHeight() - windowDescription->height) / 2
            : INVENTORY_WINDOW_Y;
        gInventoryWindow = windowCreate(inventoryWindowX,
            inventoryWindowY,
            windowDescription->width,
            windowDescription->height,
            257,
            WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
        gInventoryWindowMaxX = windowDescription->width + inventoryWindowX;
        gInventoryWindowMaxY = windowDescription->height + inventoryWindowY;

        unsigned char* dest = windowGetBuffer(gInventoryWindow);

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, windowDescription->frmId, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            blitBufferToBuffer(backgroundFrmImage.getData(), windowDescription->width, windowDescription->height, windowDescription->width, dest, windowDescription->width);
        }

        gInventoryPrintItemDescriptionHandler = displayMonitorAddMessage;
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        if (_barter_back_win == -1) {
            exit(1);
        }

        gInventorySlotsCount = 3;

        // Trade inventory window is a part of game dialog, which is 640x480.
        int tradeWindowX = (screenGetWidth() - INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH) / 2 + INVENTORY_TRADE_WINDOW_X;
        int tradeWindowY = (screenGetHeight() - INVENTORY_TRADE_BACKGROUND_WINDOW_HEIGHT) / 2 + INVENTORY_TRADE_WINDOW_Y;
        gInventoryWindow = windowCreate(tradeWindowX, tradeWindowY, INVENTORY_TRADE_WINDOW_WIDTH, INVENTORY_TRADE_WINDOW_HEIGHT, 257, 0);
        gInventoryWindowMaxX = tradeWindowX + INVENTORY_TRADE_WINDOW_WIDTH;
        gInventoryWindowMaxY = tradeWindowY + INVENTORY_TRADE_WINDOW_HEIGHT;

        unsigned char* dest = windowGetBuffer(gInventoryWindow);
        unsigned char* src = windowGetBuffer(_barter_back_win);
        blitBufferToBuffer(src + INVENTORY_TRADE_WINDOW_X, INVENTORY_TRADE_WINDOW_WIDTH, INVENTORY_TRADE_WINDOW_HEIGHT, INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH, dest, INVENTORY_TRADE_WINDOW_WIDTH);

        gInventoryPrintItemDescriptionHandler = gameDialogRenderSupplementaryMessage;
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        // Create invsibile buttons representing character's inventory item
        // slots.
        for (int index = 0; index < gInventorySlotsCount; index++) {
            int btn = buttonCreate(gInventoryWindow,
                INVENTORY_LOOT_LEFT_SCROLLER_X,
                INVENTORY_SLOT_HEIGHT * (gInventorySlotsCount - index - 1) + INVENTORY_LOOT_LEFT_SCROLLER_Y,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                999 + gInventorySlotsCount - index,
                -1,
                999 + gInventorySlotsCount - index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }
        }

        int eventCode = 2005;
        int y = INVENTORY_SLOT_HEIGHT * 5 + INVENTORY_LOOT_LEFT_SCROLLER_Y;

        // Create invisible buttons representing container's inventory item
        // slots. For unknown reason it loops backwards and it's size is
        // hardcoded at 6 items.
        //
        // Original code is slightly different. It loops until y reaches -11,
        // which is a bit awkward for a loop. Probably result of some
        // optimization.
        for (int index = 0; index < 6; index++) {
            int btn = buttonCreate(gInventoryWindow,
                INVENTORY_LOOT_RIGHT_SCROLLER_X,
                y,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                eventCode,
                -1,
                eventCode,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }

            eventCode -= 1;
            y -= INVENTORY_SLOT_HEIGHT;
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        int y1 = INVENTORY_TRADE_SCROLLER_Y;
        int y2 = INVENTORY_TRADE_INNER_SCROLLER_Y;

        for (int index = 0; index < gInventorySlotsCount; index++) {
            int btn;

            // Invsibile button representing left inventory slot.
            btn = buttonCreate(gInventoryWindow,
                INVENTORY_TRADE_LEFT_SCROLLER_X,
                y1,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                1000 + index,
                -1,
                1000 + index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }

            // Invisible button representing right inventory slot.
            btn = buttonCreate(gInventoryWindow,
                INVENTORY_TRADE_RIGHT_SCROLLER_X,
                y1,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2000 + index,
                -1,
                2000 + index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }

            // Invisible button representing left suggested slot.
            btn = buttonCreate(gInventoryWindow,
                INVENTORY_TRADE_INNER_LEFT_SCROLLER_X,
                y2,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2300 + index,
                -1,
                2300 + index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }

            // Invisible button representing right suggested slot.
            btn = buttonCreate(gInventoryWindow,
                INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X,
                y2,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                2400 + index,
                -1,
                2400 + index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }

            y1 += INVENTORY_SLOT_HEIGHT;
            y2 += INVENTORY_SLOT_HEIGHT;
        }
    } else {
        // Create invisible buttons representing item slots.
        for (int index = 0; index < gInventorySlotsCount; index++) {
            int btn = buttonCreate(gInventoryWindow,
                INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_HEIGHT * (gInventorySlotsCount - index - 1) + INVENTORY_SCROLLER_Y,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                999 + gInventorySlotsCount - index,
                -1,
                999 + gInventorySlotsCount - index,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
            if (btn != -1) {
                buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
            }
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        int btn;

        // Item2 slot
        btn = buttonCreate(gInventoryWindow,
            INVENTORY_RIGHT_HAND_SLOT_X,
            INVENTORY_RIGHT_HAND_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1006,
            -1,
            1006,
            -1,
            nullptr,
            nullptr,
            nullptr,
            0);
        if (btn != -1) {
            buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
        }

        // Item1 slot
        btn = buttonCreate(gInventoryWindow,
            INVENTORY_LEFT_HAND_SLOT_X,
            INVENTORY_LEFT_HAND_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1007,
            -1,
            1007,
            -1,
            nullptr,
            nullptr,
            nullptr,
            0);
        if (btn != -1) {
            buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
        }

        // Armor slot
        btn = buttonCreate(gInventoryWindow,
            INVENTORY_ARMOR_SLOT_X,
            INVENTORY_ARMOR_SLOT_Y,
            INVENTORY_LARGE_SLOT_WIDTH,
            INVENTORY_LARGE_SLOT_HEIGHT,
            1008,
            -1,
            1008,
            -1,
            nullptr,
            nullptr,
            nullptr,
            0);
        if (btn != -1) {
            buttonSetMouseCallbacks(btn, inventoryItemSlotOnMouseEnter, inventoryItemSlotOnMouseExit, nullptr, nullptr);
        }
    }

    int fid;
    int btn;

    fid = buildFid(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    _inventoryFrmImages[0].lock(fid);

    fid = buildFid(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    _inventoryFrmImages[1].lock(fid);

    if (_inventoryFrmImages[0].isLocked() && _inventoryFrmImages[1].isLocked()) {
        btn = -1;
        switch (inventoryWindowType) {
        case INVENTORY_WINDOW_TYPE_NORMAL:
            // Done button
            btn = buttonCreate(gInventoryWindow,
                437,
                329,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                _inventoryFrmImages[0].getData(),
                _inventoryFrmImages[1].getData(),
                nullptr,
                BUTTON_FLAG_TRANSPARENT);
            break;
        case INVENTORY_WINDOW_TYPE_USE_ITEM_ON:
            // Cancel button
            btn = buttonCreate(gInventoryWindow,
                233,
                328,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                _inventoryFrmImages[0].getData(),
                _inventoryFrmImages[1].getData(),
                nullptr,
                BUTTON_FLAG_TRANSPARENT);
            break;
        case INVENTORY_WINDOW_TYPE_LOOT:
            // Done button
            btn = buttonCreate(gInventoryWindow,
                476,
                331,
                15,
                16,
                -1,
                -1,
                -1,
                KEY_ESCAPE,
                _inventoryFrmImages[0].getData(),
                _inventoryFrmImages[1].getData(),
                nullptr,
                BUTTON_FLAG_TRANSPARENT);
            break;
        }

        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        // Large arrow up (normal).
        fid = buildFid(OBJ_TYPE_INTERFACE, 100, 0, 0, 0);
        _inventoryFrmImages[2].lock(fid);

        // Large arrow up (pressed).
        fid = buildFid(OBJ_TYPE_INTERFACE, 101, 0, 0, 0);
        _inventoryFrmImages[3].lock(fid);

        if (_inventoryFrmImages[2].isLocked() && _inventoryFrmImages[3].isLocked()) {
            // Left inventory up button.
            btn = buttonCreate(gInventoryWindow,
                109,
                56,
                23,
                24,
                -1,
                -1,
                KEY_ARROW_UP,
                -1,
                _inventoryFrmImages[2].getData(),
                _inventoryFrmImages[3].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            // Right inventory up button.
            btn = buttonCreate(gInventoryWindow,
                342,
                56,
                23,
                24,
                -1,
                -1,
                KEY_CTRL_ARROW_UP,
                -1,
                _inventoryFrmImages[2].getData(),
                _inventoryFrmImages[3].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }
        }
    } else {
        // Large up arrow (normal).
        fid = buildFid(OBJ_TYPE_INTERFACE, 49, 0, 0, 0);
        _inventoryFrmImages[2].lock(fid);

        // Large up arrow (pressed).
        fid = buildFid(OBJ_TYPE_INTERFACE, 50, 0, 0, 0);
        _inventoryFrmImages[3].lock(fid);

        // Large up arrow (disabled).
        fid = buildFid(OBJ_TYPE_INTERFACE, 53, 0, 0, 0);
        _inventoryFrmImages[4].lock(fid);

        if (_inventoryFrmImages[2].isLocked() && _inventoryFrmImages[3].isLocked() && _inventoryFrmImages[4].isLocked()) {
            if (inventoryWindowType != INVENTORY_WINDOW_TYPE_TRADE) {
                // Left inventory up button.
                gInventoryScrollUpButton = buttonCreate(gInventoryWindow,
                    128,
                    39,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_ARROW_UP,
                    -1,
                    _inventoryFrmImages[2].getData(),
                    _inventoryFrmImages[3].getData(),
                    nullptr,
                    0);
                if (gInventoryScrollUpButton != -1) {
                    _win_register_button_disable(gInventoryScrollUpButton, _inventoryFrmImages[4].getData(), _inventoryFrmImages[4].getData(), _inventoryFrmImages[4].getData());
                    buttonSetCallbacks(gInventoryScrollUpButton, _gsound_red_butt_press, _gsound_red_butt_release);
                    buttonDisable(gInventoryScrollUpButton);
                }
            }

            if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                // Right inventory up button.
                gSecondaryInventoryScrollUpButton = buttonCreate(gInventoryWindow,
                    379,
                    39,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_CTRL_ARROW_UP,
                    -1,
                    _inventoryFrmImages[2].getData(),
                    _inventoryFrmImages[3].getData(),
                    nullptr,
                    0);
                if (gSecondaryInventoryScrollUpButton != -1) {
                    _win_register_button_disable(gSecondaryInventoryScrollUpButton, _inventoryFrmImages[4].getData(), _inventoryFrmImages[4].getData(), _inventoryFrmImages[4].getData());
                    buttonSetCallbacks(gSecondaryInventoryScrollUpButton, _gsound_red_butt_press, _gsound_red_butt_release);
                    buttonDisable(gSecondaryInventoryScrollUpButton);
                }
            }
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        // Large dialog down button (normal)
        fid = buildFid(OBJ_TYPE_INTERFACE, 93, 0, 0, 0);
        _inventoryFrmImages[5].lock(fid);

        // Dialog down button (pressed)
        fid = buildFid(OBJ_TYPE_INTERFACE, 94, 0, 0, 0);
        _inventoryFrmImages[6].lock(fid);

        if (_inventoryFrmImages[5].isLocked() && _inventoryFrmImages[6].isLocked()) {
            // Left inventory down button.
            btn = buttonCreate(gInventoryWindow,
                109,
                82,
                24,
                25,
                -1,
                -1,
                KEY_ARROW_DOWN,
                -1,
                _inventoryFrmImages[5].getData(),
                _inventoryFrmImages[6].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            // Right inventory down button
            btn = buttonCreate(gInventoryWindow,
                342,
                82,
                24,
                25,
                -1,
                -1,
                KEY_CTRL_ARROW_DOWN,
                -1,
                _inventoryFrmImages[5].getData(),
                _inventoryFrmImages[6].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            // Invisible button representing left character.
            buttonCreate(_barter_back_win,
                15,
                25,
                INVENTORY_BODY_VIEW_WIDTH,
                INVENTORY_BODY_VIEW_HEIGHT,
                -1,
                -1,
                2500,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);

            // Invisible button representing right character.
            buttonCreate(_barter_back_win,
                560,
                25,
                INVENTORY_BODY_VIEW_WIDTH,
                INVENTORY_BODY_VIEW_HEIGHT,
                -1,
                -1,
                2501,
                -1,
                nullptr,
                nullptr,
                nullptr,
                0);
        }
    } else {
        // Large arrow down (normal).
        fid = buildFid(OBJ_TYPE_INTERFACE, 51, 0, 0, 0);
        _inventoryFrmImages[5].lock(fid);

        // Large arrow down (pressed).
        fid = buildFid(OBJ_TYPE_INTERFACE, 52, 0, 0, 0);
        _inventoryFrmImages[6].lock(fid);

        // Large arrow down (disabled).
        fid = buildFid(OBJ_TYPE_INTERFACE, 54, 0, 0, 0);
        _inventoryFrmImages[7].lock(fid);

        if (_inventoryFrmImages[5].isLocked() && _inventoryFrmImages[6].isLocked() && _inventoryFrmImages[7].isLocked()) {
            // Left inventory down button.
            gInventoryScrollDownButton = buttonCreate(gInventoryWindow,
                128,
                62,
                22,
                23,
                -1,
                -1,
                KEY_ARROW_DOWN,
                -1,
                _inventoryFrmImages[5].getData(),
                _inventoryFrmImages[6].getData(),
                nullptr,
                0);
            buttonSetCallbacks(gInventoryScrollDownButton, _gsound_red_butt_press, _gsound_red_butt_release);
            _win_register_button_disable(gInventoryScrollDownButton, _inventoryFrmImages[7].getData(), _inventoryFrmImages[7].getData(), _inventoryFrmImages[7].getData());
            buttonDisable(gInventoryScrollDownButton);

            if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                // Invisible button representing left character.
                buttonCreate(gInventoryWindow,
                    INVENTORY_LOOT_LEFT_BODY_VIEW_X,
                    INVENTORY_LOOT_LEFT_BODY_VIEW_Y,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    -1,
                    -1,
                    2500,
                    -1,
                    nullptr,
                    nullptr,
                    nullptr,
                    0);

                // Right inventory down button.
                gSecondaryInventoryScrollDownButton = buttonCreate(gInventoryWindow,
                    379,
                    62,
                    22,
                    23,
                    -1,
                    -1,
                    KEY_CTRL_ARROW_DOWN,
                    -1,
                    _inventoryFrmImages[5].getData(),
                    _inventoryFrmImages[6].getData(),
                    nullptr,
                    0);
                if (gSecondaryInventoryScrollDownButton != -1) {
                    buttonSetCallbacks(gSecondaryInventoryScrollDownButton, _gsound_red_butt_press, _gsound_red_butt_release);
                    _win_register_button_disable(gSecondaryInventoryScrollDownButton, _inventoryFrmImages[7].getData(), _inventoryFrmImages[7].getData(), _inventoryFrmImages[7].getData());
                    buttonDisable(gSecondaryInventoryScrollDownButton);
                }

                // Invisible button representing right character.
                buttonCreate(gInventoryWindow,
                    INVENTORY_LOOT_RIGHT_BODY_VIEW_X,
                    INVENTORY_LOOT_RIGHT_BODY_VIEW_Y,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    -1,
                    -1,
                    2501,
                    -1,
                    nullptr,
                    nullptr,
                    nullptr,
                    0);
            } else {
                // Invisible button representing character (in inventory and use on dialogs).
                buttonCreate(gInventoryWindow,
                    INVENTORY_PC_BODY_VIEW_X,
                    INVENTORY_PC_BODY_VIEW_Y,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    -1,
                    -1,
                    2500,
                    -1,
                    nullptr,
                    nullptr,
                    nullptr,
                    0);
            }
        }
    }

    if (inventoryWindowType != INVENTORY_WINDOW_TYPE_TRADE) {
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
            if (!_gIsSteal) {
                // Take all button (normal)
                fid = buildFid(OBJ_TYPE_INTERFACE, 436, 0, 0, 0);
                _inventoryFrmImages[8].lock(fid);

                // Take all button (pressed)
                fid = buildFid(OBJ_TYPE_INTERFACE, 437, 0, 0, 0);
                _inventoryFrmImages[9].lock(fid);

                if (_inventoryFrmImages[8].isLocked() && _inventoryFrmImages[9].isLocked()) {
                    // Take all button.
                    btn = buttonCreate(gInventoryWindow,
                        432,
                        204,
                        39,
                        41,
                        -1,
                        -1,
                        KEY_UPPERCASE_A,
                        -1,
                        _inventoryFrmImages[8].getData(),
                        _inventoryFrmImages[9].getData(),
                        nullptr,
                        0);
                    if (btn != -1) {
                        buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
                    }
                }
            }
        }
    } else {
        // Inventory button up (normal)
        fid = buildFid(OBJ_TYPE_INTERFACE, 49, 0, 0, 0);
        _inventoryFrmImages[8].lock(fid);

        // Inventory button up (pressed)
        fid = buildFid(OBJ_TYPE_INTERFACE, 50, 0, 0, 0);
        _inventoryFrmImages[9].lock(fid);

        if (_inventoryFrmImages[8].isLocked() && _inventoryFrmImages[9].isLocked()) {
            // Left offered inventory up button.
            btn = buttonCreate(gInventoryWindow,
                128,
                113,
                22,
                23,
                -1,
                -1,
                KEY_PAGE_UP,
                -1,
                _inventoryFrmImages[8].getData(),
                _inventoryFrmImages[9].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            // Right offered inventory up button.
            btn = buttonCreate(gInventoryWindow,
                333,
                113,
                22,
                23,
                -1,
                -1,
                KEY_CTRL_PAGE_UP,
                -1,
                _inventoryFrmImages[8].getData(),
                _inventoryFrmImages[9].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }
        }

        // Inventory button down (normal)
        fid = buildFid(OBJ_TYPE_INTERFACE, 51, 0, 0, 0);
        _inventoryFrmImages[10].lock(fid);

        // Inventory button down (pressed).
        fid = buildFid(OBJ_TYPE_INTERFACE, 52, 0, 0, 0);
        _inventoryFrmImages[11].lock(fid);

        if (_inventoryFrmImages[10].isLocked() && _inventoryFrmImages[11].isLocked()) {
            // Left offered inventory down button.
            btn = buttonCreate(gInventoryWindow,
                128,
                136,
                22,
                23,
                -1,
                -1,
                KEY_PAGE_DOWN,
                -1,
                _inventoryFrmImages[10].getData(),
                _inventoryFrmImages[11].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            // Right offered inventory down button.
            btn = buttonCreate(gInventoryWindow,
                333,
                136,
                22,
                23,
                -1,
                -1,
                KEY_CTRL_PAGE_DOWN,
                -1,
                _inventoryFrmImages[10].getData(),
                _inventoryFrmImages[11].getData(),
                nullptr,
                0);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }
        }
    }

    // Ledger H-3: equip staging extracted to core (item.cc).
    equipmentDetach(_inven_dude, &gInventoryLeftHandItem, &gInventoryRightHandItem, &gInventoryArmor);

    _adjust_fid();

    bool isoWasEnabled = isoDisable();

    _gmouse_disable(0);

    return isoWasEnabled;
}

// 0x46FBD8
static void _exit_inventory(bool shouldEnableIso)
{
    _inven_dude = _stack[0];

    // Ledger H-4: equip commit extracted to core (item.cc).
    equipmentApply(_inven_dude, gInventoryLeftHandItem, gInventoryRightHandItem, gInventoryArmor);

    gInventoryRightHandItem = nullptr;
    gInventoryArmor = nullptr;
    gInventoryLeftHandItem = nullptr;

    for (int index = 0; index < INVENTORY_FRM_COUNT; index++) {
        _inventoryFrmImages[index].unlock();
    }

    if (shouldEnableIso) {
        isoEnable();
    }

    windowDestroy(gInventoryWindow);

    _gmouse_enable();

    if (_dropped_explosive) {
        // Ledger H-5: RNG-consuming resolution extracted to core (actions.cc).
        actionResolveDroppedExplosive(gDude);
        _dropped_explosive = false;
    }
}

// 0x46FDF4
static void _display_inventory(int stackOffset, int dragSlotIndex, int inventoryWindowType)
{
    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);
    int pitch;

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        pitch = INVENTORY_WINDOW_WIDTH;

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            // Clear scroll view background.
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                gInventorySlotsCount * INVENTORY_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                pitch);

            // Clear armor button background.
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X,
                INVENTORY_LARGE_SLOT_WIDTH,
                INVENTORY_LARGE_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X,
                pitch);

            if (gInventoryLeftHandItem != nullptr && gInventoryLeftHandItem == gInventoryRightHandItem) {
                // Clear item1.
                FrmImage itemBackgroundFrmImage;
                int itemBackgroundFid = buildFid(OBJ_TYPE_INTERFACE, 32, 0, 0, 0);
                if (itemBackgroundFrmImage.lock(itemBackgroundFid)) {
                    unsigned char* data = itemBackgroundFrmImage.getData();
                    int width = itemBackgroundFrmImage.getWidth();
                    int height = itemBackgroundFrmImage.getHeight();
                    blitBufferToBuffer(data, width, height, width, windowBuffer + pitch * 284 + 152, pitch);
                }
            } else {
                // Clear both items in one go.
                blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X,
                    INVENTORY_LARGE_SLOT_WIDTH * 2,
                    INVENTORY_LARGE_SLOT_HEIGHT,
                    pitch,
                    windowBuffer + pitch * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X,
                    pitch);
            }
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_USE_ITEM_ON) {
        pitch = INVENTORY_USE_ON_WINDOW_WIDTH;

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 113, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            // Clear scroll view background.
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                gInventorySlotsCount * INVENTORY_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_SCROLLER_Y + INVENTORY_SCROLLER_X,
                pitch);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        pitch = INVENTORY_LOOT_WINDOW_WIDTH;

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            // Clear scroll view background.
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_LOOT_LEFT_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                gInventorySlotsCount * INVENTORY_SLOT_HEIGHT,
                pitch,
                windowBuffer + pitch * INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_LOOT_LEFT_SCROLLER_X,
                pitch);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        pitch = INVENTORY_TRADE_WINDOW_WIDTH;

        windowBuffer = windowGetBuffer(gInventoryWindow);

        blitBufferToBuffer(windowGetBuffer(_barter_back_win) + INVENTORY_TRADE_LEFT_SCROLLER_Y * INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH + INVENTORY_TRADE_LEFT_SCROLLER_X + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount, INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH, windowBuffer + pitch * INVENTORY_TRADE_LEFT_SCROLLER_Y + INVENTORY_TRADE_LEFT_SCROLLER_X, pitch);
    } else {
        assert(false && "Should be unreachable");
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL
        || inventoryWindowType == INVENTORY_WINDOW_TYPE_USE_ITEM_ON
        || inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        if (gInventoryScrollUpButton != -1) {
            if (stackOffset <= 0) {
                buttonDisable(gInventoryScrollUpButton);
            } else {
                buttonEnable(gInventoryScrollUpButton);
            }
        }

        if (gInventoryScrollDownButton != -1) {
            if (_pud->length - stackOffset <= gInventorySlotsCount) {
                buttonDisable(gInventoryScrollDownButton);
            } else {
                buttonEnable(gInventoryScrollDownButton);
            }
        }
    }

    int y = 0;
    for (int slotIndex = 0; slotIndex + stackOffset < _pud->length && slotIndex < gInventorySlotsCount; slotIndex += 1) {
        int itemIndex = slotIndex + stackOffset + 1;

        int offset;
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            offset = pitch * (y + INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_LEFT_SCROLLER_X_PAD;
        } else {
            if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                offset = pitch * (y + INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD) + INVENTORY_LOOT_LEFT_SCROLLER_X_PAD;
            } else {
                offset = pitch * (y + INVENTORY_SCROLLER_Y_PAD) + INVENTORY_SCROLLER_X_PAD;
            }
        }

        InventoryItem* inventoryItem = &(_pud->items[_pud->length - itemIndex]);

        int inventoryFid = itemGetInventoryFid(inventoryItem->item);
        artRender(inventoryFid, windowBuffer + offset, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, pitch);

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
            offset = pitch * (y + INVENTORY_LOOT_LEFT_SCROLLER_Y_PAD) + INVENTORY_LOOT_LEFT_SCROLLER_X_PAD;
        } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            offset = pitch * (y + INVENTORY_TRADE_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_LEFT_SCROLLER_X_PAD;
        } else {
            offset = pitch * (y + INVENTORY_SCROLLER_Y_PAD) + INVENTORY_SCROLLER_X_PAD;
        }

        _display_inventory_info(inventoryItem->item, inventoryItem->quantity, windowBuffer + offset, pitch, slotIndex == dragSlotIndex);

        y += INVENTORY_SLOT_HEIGHT;
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
        if (gInventoryRightHandItem != nullptr) {
            int width = gInventoryRightHandItem == gInventoryLeftHandItem ? INVENTORY_LARGE_SLOT_WIDTH * 2 : INVENTORY_LARGE_SLOT_WIDTH;
            int inventoryFid = itemGetInventoryFid(gInventoryRightHandItem);
            artRender(inventoryFid, windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_RIGHT_HAND_SLOT_Y + INVENTORY_RIGHT_HAND_SLOT_X, width, INVENTORY_LARGE_SLOT_HEIGHT, INVENTORY_WINDOW_WIDTH);
        }

        if (gInventoryLeftHandItem != nullptr && gInventoryLeftHandItem != gInventoryRightHandItem) {
            int inventoryFid = itemGetInventoryFid(gInventoryLeftHandItem);
            artRender(inventoryFid, windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_LEFT_HAND_SLOT_Y + INVENTORY_LEFT_HAND_SLOT_X, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, INVENTORY_WINDOW_WIDTH);
        }

        if (gInventoryArmor != nullptr) {
            int inventoryFid = itemGetInventoryFid(gInventoryArmor);
            artRender(inventoryFid, windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_ARMOR_SLOT_Y + INVENTORY_ARMOR_SLOT_X, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, INVENTORY_WINDOW_WIDTH);
        }
    }

    // CE: Show items weight.
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        char formattedText[20];

        int oldFont = fontGetCurrent();
        fontSetCurrent(101);

        FrmImage backgroundFrm;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        if (backgroundFrm.lock(backgroundFid)) {
            int x = INVENTORY_LOOT_LEFT_SCROLLER_X;
            int y = INVENTORY_LOOT_LEFT_SCROLLER_Y + gInventorySlotsCount * INVENTORY_SLOT_HEIGHT + 2;
            blitBufferToBuffer(backgroundFrm.getData() + pitch * y + x, INVENTORY_SLOT_WIDTH, fontGetLineHeight(), pitch, windowBuffer + pitch * y + x, pitch);
        }

        Object* object = _stack[0];

        int color = _colorTable[992];
        if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            int carryWeight = critterGetStat(object, STAT_CARRY_WEIGHT);
            int inventoryWeight = objectGetInventoryWeight(object);
            snprintf(formattedText, sizeof(formattedText), "%d/%d", inventoryWeight, carryWeight);

            if (critterIsEncumbered(object)) {
                color = _colorTable[31744];
            }
        } else {
            int inventoryWeight = objectGetInventoryWeight(object);
            snprintf(formattedText, sizeof(formattedText), "%d", inventoryWeight);
        }

        int width = fontGetStringWidth(formattedText);
        int x = INVENTORY_LOOT_LEFT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2 - width / 2;
        int y = INVENTORY_LOOT_LEFT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + 2;
        fontDrawText(windowBuffer + pitch * y + x, formattedText, width, pitch, color);

        fontSetCurrent(oldFont);
    }

    windowRefresh(gInventoryWindow);
}

// Render inventory item.
//
// [stackOffset] is an index of the first visible item in the scrolling view.
// [dragSlotIndex] is an index of item being dragged (it decreases displayed number of items in inner functions).
//
// 0x47036C
static void _display_target_inventory(int stackOffset, int dragSlotIndex, Inventory* inventory, int inventoryWindowType)
{
    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

    int pitch;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        pitch = INVENTORY_LOOT_WINDOW_WIDTH;

        FrmImage backgroundFrmImage;
        int fid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        if (backgroundFrmImage.lock(fid)) {
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_LOOT_RIGHT_SCROLLER_X,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT * gInventorySlotsCount,
                pitch,
                windowBuffer + pitch * INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_LOOT_RIGHT_SCROLLER_X,
                pitch);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        pitch = INVENTORY_TRADE_WINDOW_WIDTH;

        unsigned char* src = windowGetBuffer(_barter_back_win);
        blitBufferToBuffer(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_TRADE_RIGHT_SCROLLER_X + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount, INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH, windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_RIGHT_SCROLLER_Y + INVENTORY_TRADE_RIGHT_SCROLLER_X, INVENTORY_TRADE_WINDOW_WIDTH);
    } else {
        assert(false && "Should be unreachable");
    }

    int y = 0;
    for (int slotIndex = 0; slotIndex < gInventorySlotsCount; slotIndex++) {
        int itemIndex = stackOffset + slotIndex;
        if (itemIndex >= inventory->length) {
            break;
        }

        int offset;
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
            offset = pitch * (y + INVENTORY_LOOT_RIGHT_SCROLLER_Y_PAD) + INVENTORY_LOOT_RIGHT_SCROLLER_X_PAD;
        } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            offset = pitch * (y + INVENTORY_TRADE_RIGHT_SCROLLER_Y_PAD) + INVENTORY_TRADE_RIGHT_SCROLLER_X_PAD;
        } else {
            assert(false && "Should be unreachable");
        }

        InventoryItem* inventoryItem = &(inventory->items[inventory->length - (itemIndex + 1)]);
        int inventoryFid = itemGetInventoryFid(inventoryItem->item);
        artRender(inventoryFid, windowBuffer + offset, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, pitch);
        _display_inventory_info(inventoryItem->item, inventoryItem->quantity, windowBuffer + offset, pitch, slotIndex == dragSlotIndex);

        y += INVENTORY_SLOT_HEIGHT;
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        if (gSecondaryInventoryScrollUpButton != -1) {
            if (stackOffset <= 0) {
                buttonDisable(gSecondaryInventoryScrollUpButton);
            } else {
                buttonEnable(gSecondaryInventoryScrollUpButton);
            }
        }

        if (gSecondaryInventoryScrollDownButton != -1) {
            if (inventory->length - stackOffset <= gInventorySlotsCount) {
                buttonDisable(gSecondaryInventoryScrollDownButton);
            } else {
                buttonEnable(gSecondaryInventoryScrollDownButton);
            }
        }
    }

    // CE: Show items weight.
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
        char formattedText[20];
        formattedText[0] = '\0';

        int oldFont = fontGetCurrent();
        fontSetCurrent(101);

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            int x = INVENTORY_LOOT_RIGHT_SCROLLER_X;
            int y = INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + 2;
            blitBufferToBuffer(backgroundFrmImage.getData() + pitch * y + x,
                INVENTORY_SLOT_WIDTH,
                fontGetLineHeight(),
                pitch,
                windowBuffer + pitch * y + x,
                pitch);
        }

        Object* object = _target_stack[_target_curr_stack];

        int color = _colorTable[992];
        if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            int currentWeight = objectGetInventoryWeight(object);
            int maxWeight = critterGetStat(object, STAT_CARRY_WEIGHT);
            snprintf(formattedText, sizeof(formattedText), "%d/%d", currentWeight, maxWeight);

            if (critterIsEncumbered(object)) {
                color = _colorTable[31744];
            }
        } else if (PID_TYPE(object->pid) == OBJ_TYPE_ITEM) {
            if (itemGetType(object) == ITEM_TYPE_CONTAINER) {
                int currentSize = containerGetTotalSize(object);
                int maxSize = containerGetMaxSize(object);
                snprintf(formattedText, sizeof(formattedText), "%d/%d", currentSize, maxSize);
            }
        } else {
            int inventoryWeight = objectGetInventoryWeight(object);
            snprintf(formattedText, sizeof(formattedText), "%d", inventoryWeight);
        }

        int width = fontGetStringWidth(formattedText);
        int x = INVENTORY_LOOT_RIGHT_SCROLLER_X + INVENTORY_SLOT_WIDTH / 2 - width / 2;
        int y = INVENTORY_LOOT_RIGHT_SCROLLER_Y + INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + 2;
        fontDrawText(windowBuffer + pitch * y + x, formattedText, width, pitch, color);

        fontSetCurrent(oldFont);
    }
}

// Renders inventory item quantity.
//
// 0x4705A0
static void _display_inventory_info(Object* item, int quantity, unsigned char* dest, int pitch, bool isDragged)
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    char formattedText[12];

    // NOTE: Original code is slightly different and probably used goto.
    bool draw = false;

    if (itemGetType(item) == ITEM_TYPE_AMMO) {
        int ammoQuantity = ammoGetCapacity(item) * (quantity - 1);

        if (!isDragged) {
            ammoQuantity += ammoGetQuantity(item);
        }

        if (ammoQuantity > 99999) {
            ammoQuantity = 99999;
        }

        snprintf(formattedText, sizeof(formattedText), "x%d", ammoQuantity);
        draw = true;
    } else {
        if (quantity > 1) {
            int v9 = quantity;
            if (isDragged) {
                v9 -= 1;
            }

            if (quantity > 1) {
                if (v9 > 99999) {
                    v9 = 99999;
                }

                snprintf(formattedText, sizeof(formattedText), "x%d", v9);
                draw = true;
            }
        }
    }

    if (draw) {
        fontDrawText(dest, formattedText, 80, pitch, _colorTable[32767]);
    }

    fontSetCurrent(oldFont);
}

// 0x470650
static void _display_body(int fid, int inventoryWindowType)
{
    if (getTicksSince(gInventoryWindowDudeRotationTimestamp) < INVENTORY_NORMAL_WINDOW_PC_ROTATION_DELAY) {
        return;
    }

    gInventoryWindowDudeRotation += 1;

    if (gInventoryWindowDudeRotation == ROTATION_COUNT) {
        gInventoryWindowDudeRotation = 0;
    }

    int rotations[2];
    if (fid == -1) {
        rotations[0] = gInventoryWindowDudeRotation;
        rotations[1] = ROTATION_SE;
    } else {
        rotations[0] = ROTATION_SW;
        rotations[1] = _target_stack[_target_curr_stack]->rotation;
    }

    int fids[2] = {
        gInventoryWindowDudeFid,
        fid,
    };

    for (int index = 0; index < 2; index += 1) {
        int fid = fids[index];
        if (fid == -1) {
            continue;
        }

        CacheEntry* handle;
        Art* art = artLock(fid, &handle);
        if (art == nullptr) {
            continue;
        }

        int frame = 0;
        if (index == 1) {
            frame = artGetFrameCount(art) - 1;
        }

        int rotation = rotations[index];

        unsigned char* frameData = artGetFrameData(art, frame, rotation);

        int framePitch = artGetWidth(art, frame, rotation);
        int frameWidth = std::min(framePitch, INVENTORY_BODY_VIEW_WIDTH);

        int frameHeight = artGetHeight(art, frame, rotation);
        if (frameHeight > INVENTORY_BODY_VIEW_HEIGHT) {
            frameHeight = INVENTORY_BODY_VIEW_HEIGHT;
        }

        int win;
        Rect rect;
        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
            unsigned char* windowBuffer = windowGetBuffer(_barter_back_win);
            int windowPitch = windowGetWidth(_barter_back_win);

            if (index == 1) {
                rect.left = 560;
                rect.top = 25;
            } else {
                rect.left = 15;
                rect.top = 25;
            }

            rect.right = rect.left + INVENTORY_BODY_VIEW_WIDTH - 1;
            rect.bottom = rect.top + INVENTORY_BODY_VIEW_HEIGHT - 1;

            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, gGameDialogSpeakerIsPartyMember ? 420 : 111, 0, 0, 0);
            if (backgroundFrmImage.lock(backgroundFid)) {
                blitBufferToBuffer(backgroundFrmImage.getData() + rect.top * 640 + rect.left,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    640,
                    windowBuffer + windowPitch * rect.top + rect.left,
                    windowPitch);
            }

            blitBufferToBufferTrans(frameData, frameWidth, frameHeight, framePitch,
                windowBuffer + windowPitch * (rect.top + (INVENTORY_BODY_VIEW_HEIGHT - frameHeight) / 2) + (INVENTORY_BODY_VIEW_WIDTH - frameWidth) / 2 + rect.left,
                windowPitch);

            win = _barter_back_win;
        } else {
            unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);
            int windowPitch = windowGetWidth(gInventoryWindow);

            if (index == 1) {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                    rect.left = 426;
                    rect.top = 39;
                } else {
                    rect.left = 297;
                    rect.top = 37;
                }
            } else {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT) {
                    rect.left = 48;
                    rect.top = 39;
                } else {
                    rect.left = 176;
                    rect.top = 37;
                }
            }

            rect.right = rect.left + INVENTORY_BODY_VIEW_WIDTH - 1;
            rect.bottom = rect.top + INVENTORY_BODY_VIEW_HEIGHT - 1;

            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
            if (backgroundFrmImage.lock(backgroundFid)) {
                blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_LOOT_WINDOW_WIDTH * rect.top + rect.left,
                    INVENTORY_BODY_VIEW_WIDTH,
                    INVENTORY_BODY_VIEW_HEIGHT,
                    INVENTORY_LOOT_WINDOW_WIDTH,
                    windowBuffer + windowPitch * rect.top + rect.left,
                    windowPitch);
            }

            blitBufferToBufferTrans(frameData, frameWidth, frameHeight, framePitch,
                windowBuffer + windowPitch * (rect.top + (INVENTORY_BODY_VIEW_HEIGHT - frameHeight) / 2) + (INVENTORY_BODY_VIEW_WIDTH - frameWidth) / 2 + rect.left,
                windowPitch);

            win = gInventoryWindow;
        }
        windowRefreshRect(win, &rect);

        artUnlock(handle);
    }

    gInventoryWindowDudeRotationTimestamp = getTicks();
}

// 0x470A2C
static int inventoryCommonInit()
{
    if (inventoryMessageListInit() == -1) {
        return -1;
    }

    _inven_ui_was_disabled = gameUiIsDisabled();

    if (_inven_ui_was_disabled) {
        gameUiEnable();
    }

    gameMouseObjectsHide();

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    int index;
    for (index = 0; index < INVENTORY_WINDOW_CURSOR_COUNT; index++) {
        InventoryCursorData* cursorData = &(gInventoryCursorData[index]);

        int fid = buildFid(OBJ_TYPE_INTERFACE, gInventoryWindowCursorFrmIds[index], 0, 0, 0);
        Art* frm = artLock(fid, &(cursorData->frmHandle));
        if (frm == nullptr) {
            break;
        }

        cursorData->frm = frm;
        cursorData->frmData = artGetFrameData(frm, 0, 0);
        cursorData->width = artGetWidth(frm, 0, 0);
        cursorData->height = artGetHeight(frm, 0, 0);
        artGetFrameOffsets(frm, 0, 0, &(cursorData->offsetX), &(cursorData->offsetY));
    }

    if (index != INVENTORY_WINDOW_CURSOR_COUNT) {
        for (; index >= 0; index--) {
            artUnlock(gInventoryCursorData[index].frmHandle);
        }

        if (_inven_ui_was_disabled) {
            gameUiDisable(0);
        }

        messageListFree(&gInventoryMessageList);

        return -1;
    }

    _inven_is_initialized = true;
    _im_value = -1;

    return 0;
}

// NOTE: Inlined.
//
// 0x470B8C
static void inventoryCommonFree()
{
    for (int index = 0; index < INVENTORY_WINDOW_CURSOR_COUNT; index++) {
        artUnlock(gInventoryCursorData[index].frmHandle);
    }

    if (_inven_ui_was_disabled) {
        gameUiDisable(0);
    }

    // NOTE: Uninline.
    inventoryMessageListFree();

    _inven_is_initialized = 0;
}

// 0x470BCC
static void inventorySetCursor(int cursor)
{
    gInventoryCursor = cursor;

    if (cursor != INVENTORY_WINDOW_CURSOR_ARROW || _im_value == -1) {
        InventoryCursorData* cursorData = &(gInventoryCursorData[cursor]);
        mouseSetFrame(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
    } else {
        inventoryItemSlotOnMouseEnter(-1, _im_value);
    }
}

// 0x470C2C
static void inventoryItemSlotOnMouseEnter(int btn, int keyCode)
{
    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
        int x;
        int y;
        mouseGetPositionInWindow(gInventoryWindow, &x, &y);

        Object* item = nullptr;
        if (_inven_from_button(keyCode, &item, nullptr, nullptr) != 0) {
            gameMouseRenderPrimaryAction(x, y, 3, gInventoryWindowMaxX, gInventoryWindowMaxY);

            int v5 = 0;
            int v6 = 0;
            _gmouse_3d_pick_frame_hot(&v5, &v6);

            InventoryCursorData* cursorData = &(gInventoryCursorData[INVENTORY_WINDOW_CURSOR_PICK]);
            mouseSetFrame(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, v5, v6, 0);

            if (item != _last_target) {
                _obj_look_at_func(_stack[0], item, gInventoryPrintItemDescriptionHandler);
            }
        } else {
            InventoryCursorData* cursorData = &(gInventoryCursorData[INVENTORY_WINDOW_CURSOR_ARROW]);
            mouseSetFrame(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
        }

        _last_target = item;
    }

    _im_value = keyCode;
}

// 0x470D1C
static void inventoryItemSlotOnMouseExit(int btn, int keyCode)
{
    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
        InventoryCursorData* cursorData = &(gInventoryCursorData[INVENTORY_WINDOW_CURSOR_ARROW]);
        mouseSetFrame(cursorData->frmData, cursorData->width, cursorData->height, cursorData->width, cursorData->offsetX, cursorData->offsetY, 0);
    }

    _im_value = -1;
}

// 0x470D5C
static void _inven_update_lighting(Object* activeItem)
{
    if (gDude == _inven_dude) {
        int lightDistance;
        if (activeItem != nullptr && activeItem->lightDistance > 4) {
            lightDistance = activeItem->lightDistance;
        } else {
            lightDistance = 4;
        }

        Rect rect;
        objectSetLight(_inven_dude, lightDistance, 0x10000, &rect);
        presenter()->worldInvalidateRect(&rect, gElevation);
    }
}

// 0x470DB8
static void _inven_pickup(int buttonCode, int indexOffset)
{
    Object* item;
    Object** itemSlot = nullptr;
    int count = _inven_from_button(buttonCode, &item, &itemSlot, nullptr);
    if (count == 0) {
        return;
    }

    int itemIndex = -1;
    Object* itemInHand = nullptr;
    Rect rect;

    switch (buttonCode) {
    case 1006:
        rect.left = 245;
        rect.top = 286;
        if (_inven_dude == gDude && interfaceGetCurrentHand() != HAND_LEFT) {
            itemInHand = item;
        }
        break;
    case 1007:
        rect.left = 154;
        rect.top = 286;
        if (_inven_dude == gDude && interfaceGetCurrentHand() == HAND_LEFT) {
            itemInHand = item;
        }
        break;
    case 1008:
        rect.left = 154;
        rect.top = 183;
        break;
    default:
        // NOTE: Original code a little bit different, this code path
        // is only for key codes below 1006.
        itemIndex = buttonCode - 1000;
        rect.left = INVENTORY_SCROLLER_X;
        rect.top = INVENTORY_SLOT_HEIGHT * itemIndex + INVENTORY_SCROLLER_Y;
        break;
    }

    if (itemIndex == -1 || _pud->items[indexOffset + itemIndex].quantity <= 1) {
        unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);
        if (gInventoryRightHandItem != gInventoryLeftHandItem || item != gInventoryLeftHandItem) {
            int height;
            int width;
            if (itemIndex == -1) {
                height = INVENTORY_LARGE_SLOT_HEIGHT;
                width = INVENTORY_LARGE_SLOT_WIDTH;
            } else {
                height = INVENTORY_SLOT_HEIGHT;
                width = INVENTORY_SLOT_WIDTH;
            }

            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
            if (backgroundFrmImage.lock(backgroundFid)) {
                blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_WINDOW_WIDTH * rect.top + rect.left,
                    width,
                    height,
                    INVENTORY_WINDOW_WIDTH,
                    windowBuffer + INVENTORY_WINDOW_WIDTH * rect.top + rect.left,
                    INVENTORY_WINDOW_WIDTH);
            }

            rect.right = rect.left + width - 1;
            rect.bottom = rect.top + height - 1;
        } else {
            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
            if (backgroundFrmImage.lock(backgroundFid)) {
                blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_WINDOW_WIDTH * 286 + 154,
                    180,
                    61,
                    INVENTORY_WINDOW_WIDTH,
                    windowBuffer + INVENTORY_WINDOW_WIDTH * 286 + 154,
                    INVENTORY_WINDOW_WIDTH);
            }

            rect.left = 154;
            rect.top = 286;
            rect.right = rect.left + 180 - 1;
            rect.bottom = rect.top + 61 - 1;
        }
        windowRefreshRect(gInventoryWindow, &rect);
    } else {
        _display_inventory(indexOffset, itemIndex, INVENTORY_WINDOW_TYPE_NORMAL);
    }

    FrmImage itemInventoryFrmImage;
    int itemInventoryFid = itemGetInventoryFid(item);
    if (itemInventoryFrmImage.lock(itemInventoryFid)) {
        int width = itemInventoryFrmImage.getWidth();
        int height = itemInventoryFrmImage.getHeight();
        unsigned char* data = itemInventoryFrmImage.getData();
        mouseSetFrame(data, width, height, width, width / 2, height / 2, 0);
        soundPlayFile("ipickup1");
    }

    if (itemInHand != nullptr) {
        _inven_update_lighting(nullptr);
    }

    do {
        sharedFpsLimiter.mark();

        inputGetInput();
        _display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (itemInventoryFrmImage.isLocked()) {
        itemInventoryFrmImage.unlock();
        soundPlayFile("iputdown");
    }

    if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_SCROLLER_Y)) {
        int x;
        int y;
        mouseGetPositionInWindow(gInventoryWindow, &x, &y);

        int targetIndex = (y - 39) / INVENTORY_SLOT_HEIGHT + indexOffset;
        if (targetIndex < _pud->length) {
            Object* targetItem = _pud->items[targetIndex].item;
            if (targetItem != item && !clientViewerActive()) {
                // Dropping item on top of another item. Viewer (Slice 3b): SKIP — the
                // container-store and ammo-load sub-drops mutate, and their contents/
                // ammo are not streamed yet (Slice A2).
                if (itemGetType(targetItem) == ITEM_TYPE_CONTAINER) {
                    if (_drop_into_container(targetItem, item, itemIndex, itemSlot, count) == 0) {
                        itemIndex = 0;
                    }
                } else {
                    if (_drop_ammo_into_weapon(targetItem, item, itemSlot, count, buttonCode) == 0) {
                        itemIndex = 0;
                    }
                }
            }
        }

        if (itemIndex == -1) {
            if (clientViewerActive()) {
                // Unequip: a hand/armor item dragged onto the list. Fire the wire verb
                // and skip the local re-add — the server-authoritative dude streams the
                // result back (Slice 2 reconcile). itemIndex==-1 ⇒ the drag came from a
                // slot, so itemSlot names it (left/right hand or armor).
                int hand = (itemSlot == &gInventoryArmor) ? HAND_COUNT
                    : (itemSlot == &gInventoryLeftHandItem) ? HAND_LEFT
                                                            : HAND_RIGHT;
                clientViewerUnwield(hand);
            } else {
                // TODO: Holy shit, needs refactoring.
                *itemSlot = nullptr;
                if (itemAdd(_inven_dude, item, 1)) {
                    *itemSlot = item;
                } else if (itemSlot == &gInventoryArmor) {
                    _adjust_ac(_stack[0], item, nullptr);
                } else if (gInventoryRightHandItem == gInventoryLeftHandItem) {
                    gInventoryLeftHandItem = nullptr;
                    gInventoryRightHandItem = nullptr;
                }
            }
        }
    } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_LEFT_HAND_SLOT_X, INVENTORY_LEFT_HAND_SLOT_Y, INVENTORY_LEFT_HAND_SLOT_MAX_X, INVENTORY_LEFT_HAND_SLOT_MAX_Y)) {
        if (clientViewerActive()) {
            // Equip to the LEFT hand → wire verb, skip local mutation. Skip the container-
            // store / ammo-load sub-drops (not streamed); mirror vanilla's branch select
            // with side-effect-free reads so no local state changes.
            bool containerStore = gInventoryLeftHandItem != nullptr && gInventoryLeftHandItem != item
                && itemGetType(gInventoryLeftHandItem) == ITEM_TYPE_CONTAINER;
            bool ammoLoad = gInventoryLeftHandItem != nullptr
                && itemGetType(gInventoryLeftHandItem) == ITEM_TYPE_WEAPON
                && itemGetType(item) == ITEM_TYPE_AMMO
                && weaponCanBeReloadedWith(gInventoryLeftHandItem, item);
            if (!containerStore && !ammoLoad) {
                clientViewerWield(item, HAND_LEFT);
            }
        } else if (gInventoryLeftHandItem != nullptr && itemGetType(gInventoryLeftHandItem) == ITEM_TYPE_CONTAINER && gInventoryLeftHandItem != item) {
            _drop_into_container(gInventoryLeftHandItem, item, itemIndex, itemSlot, count);
        } else if (gInventoryLeftHandItem == nullptr || _drop_ammo_into_weapon(gInventoryLeftHandItem, item, itemSlot, count, buttonCode)) {
            _switch_hand(item, &gInventoryLeftHandItem, itemSlot, buttonCode);
        }
    } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_RIGHT_HAND_SLOT_X, INVENTORY_RIGHT_HAND_SLOT_Y, INVENTORY_RIGHT_HAND_SLOT_MAX_X, INVENTORY_RIGHT_HAND_SLOT_MAX_Y)) {
        if (clientViewerActive()) {
            // Equip to the RIGHT hand (symmetric with the left-hand branch above).
            bool containerStore = gInventoryRightHandItem != nullptr && gInventoryRightHandItem != item
                && itemGetType(gInventoryRightHandItem) == ITEM_TYPE_CONTAINER;
            bool ammoLoad = gInventoryRightHandItem != nullptr
                && itemGetType(gInventoryRightHandItem) == ITEM_TYPE_WEAPON
                && itemGetType(item) == ITEM_TYPE_AMMO
                && weaponCanBeReloadedWith(gInventoryRightHandItem, item);
            if (!containerStore && !ammoLoad) {
                clientViewerWield(item, HAND_RIGHT);
            }
        } else if (gInventoryRightHandItem != nullptr && itemGetType(gInventoryRightHandItem) == ITEM_TYPE_CONTAINER && gInventoryRightHandItem != item) {
            _drop_into_container(gInventoryRightHandItem, item, itemIndex, itemSlot, count);
        } else if (gInventoryRightHandItem == nullptr || _drop_ammo_into_weapon(gInventoryRightHandItem, item, itemSlot, count, buttonCode)) {
            _switch_hand(item, &gInventoryRightHandItem, itemSlot, itemIndex);
        }
    } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_ARMOR_SLOT_X, INVENTORY_ARMOR_SLOT_Y, INVENTORY_ARMOR_SLOT_MAX_X, INVENTORY_ARMOR_SLOT_MAX_Y)) {
        if (itemGetType(item) == ITEM_TYPE_ARMOR) {
            if (clientViewerActive()) {
                // Equip armor → wire verb (the server hand arg is ignored for armor —
                // _inven_wield branches on ITEM_TYPE_ARMOR); skip the local swap and let
                // the reconcile stream it back. Falls through to the shared render tail.
                clientViewerWield(item, HAND_LEFT);
            } else {
                Object* currentArmor = gInventoryArmor;
                int itemAddResult = 0;
                if (itemIndex != -1) {
                    itemRemove(_inven_dude, item, 1);
                }

                if (gInventoryArmor != nullptr) {
                    if (itemSlot != nullptr) {
                        *itemSlot = gInventoryArmor;
                    } else {
                        gInventoryArmor = nullptr;
                        itemAddResult = itemAdd(_inven_dude, currentArmor, 1);
                    }
                } else {
                    if (itemSlot != nullptr) {
                        *itemSlot = gInventoryArmor;
                    }
                }

                if (itemAddResult != 0) {
                    gInventoryArmor = currentArmor;
                    if (itemIndex != -1) {
                        itemAdd(_inven_dude, item, 1);
                    }
                } else {
                    _adjust_ac(_stack[0], currentArmor, item);
                    gInventoryArmor = item;
                }
            }
        }
    } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_PC_BODY_VIEW_X, INVENTORY_PC_BODY_VIEW_Y, INVENTORY_PC_BODY_VIEW_MAX_X, INVENTORY_PC_BODY_VIEW_MAX_Y)) {
        if (_curr_stack != 0 && !clientViewerActive()) {
            // If we are looking inside nested inventory (such as backpack item), we see this item in the PC Body View instead of the player.
            // So we drop item into it. Viewer: SKIP — nested container contents aren't streamed (Slice A2).
            _drop_into_container(_stack[_curr_stack - 1], item, itemIndex, itemSlot, count);
        }
    } else if (clientViewerActive()) {
        // FOnline-style drop-to-world: releasing a dragged item OUTSIDE the inventory
        // window drops it (vanilla FO2 just returns it to its slot). Only when truly
        // outside the window rect — dead space INSIDE the window still returns-to-slot.
        // Fire invdrop (server drops the top-level stack of this pid + spawns it on the
        // ground; Slice 2 reconciles the mirror + repaints the list). Skip the local
        // mutation like every other viewer leaf.
        int mx;
        int my;
        mouseGetPosition(&mx, &my);
        Rect wrect;
        windowGetRect(gInventoryWindow, &wrect);
        if (mx < wrect.left || mx > wrect.right || my < wrect.top || my > wrect.bottom) {
            clientViewerDrop(item);
        }
    }

    _adjust_fid();
    inventoryRenderSummary();
    _display_inventory(indexOffset, -1, INVENTORY_WINDOW_TYPE_NORMAL);
    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
    if (_inven_dude == gDude) {
        Object* item;
        if (interfaceGetCurrentHand() == HAND_LEFT) {
            item = critterGetItem1(_inven_dude);
        } else {
            item = critterGetItem2(_inven_dude);
        }

        if (item != nullptr) {
            _inven_update_lighting(item);
        }
    }
}

// 0x4714E0
static void _switch_hand(Object* sourceItem, Object** targetSlot, Object** sourceSlot, int itemIndex)
{
    if (*targetSlot != nullptr) {
        if (itemGetType(*targetSlot) == ITEM_TYPE_WEAPON && itemGetType(sourceItem) == ITEM_TYPE_AMMO) {
            return;
        }

        if (sourceSlot != nullptr && (sourceSlot != &gInventoryArmor || itemGetType(*targetSlot) == ITEM_TYPE_ARMOR)) {
            if (sourceSlot == &gInventoryArmor) {
                _adjust_ac(_stack[0], gInventoryArmor, *targetSlot);
            }
            *sourceSlot = *targetSlot;
        } else {
            if (itemIndex != -1) {
                itemRemove(_inven_dude, sourceItem, 1);
            }

            Object* existingItem = *targetSlot;
            *targetSlot = nullptr;
            if (itemAdd(_inven_dude, existingItem, 1) != 0) {
                itemAdd(_inven_dude, sourceItem, 1);
                return;
            }

            itemIndex = -1;

            if (sourceSlot != nullptr) {
                if (sourceSlot == &gInventoryArmor) {
                    _adjust_ac(_stack[0], gInventoryArmor, nullptr);
                }
                *sourceSlot = nullptr;
            }
        }
    } else {
        if (sourceSlot != nullptr) {
            if (sourceSlot == &gInventoryArmor) {
                _adjust_ac(_stack[0], gInventoryArmor, nullptr);
            }
            *sourceSlot = nullptr;
        }
    }

    *targetSlot = sourceItem;

    if (itemIndex != -1) {
        itemRemove(_inven_dude, sourceItem, 1);
    }
}

// 0x4716E8
static void _adjust_fid()
{
    int fid;
    if (FID_TYPE(_inven_dude->fid) == OBJ_TYPE_CRITTER) {
        Proto* proto;

        int v0 = _art_vault_guy_num;

        if (protoGetProto(_inven_pid, &proto) == -1) {
            v0 = proto->fid & 0xFFF;
        }

        if (gInventoryArmor != nullptr) {
            protoGetProto(gInventoryArmor->pid, &proto);
            if (critterGetStat(_inven_dude, STAT_GENDER) == GENDER_FEMALE) {
                v0 = proto->item.data.armor.femaleFid;
            } else {
                v0 = proto->item.data.armor.maleFid;
            }

            if (v0 == -1) {
                v0 = _art_vault_guy_num;
            }
        }

        int animationCode = 0;
        if (interfaceGetCurrentHand()) {
            if (gInventoryRightHandItem != nullptr) {
                protoGetProto(gInventoryRightHandItem->pid, &proto);
                if (proto->item.type == ITEM_TYPE_WEAPON) {
                    animationCode = proto->item.data.weapon.animationCode;
                }
            }
        } else {
            if (gInventoryLeftHandItem != nullptr) {
                protoGetProto(gInventoryLeftHandItem->pid, &proto);
                if (proto->item.type == ITEM_TYPE_WEAPON) {
                    animationCode = proto->item.data.weapon.animationCode;
                }
            }
        }

        fid = buildFid(OBJ_TYPE_CRITTER, v0, 0, animationCode, 0);
    } else {
        fid = _inven_dude->fid;
    }

    gInventoryWindowDudeFid = fid;
}

// 0x4717E4
void inventoryOpenUseItemOn(Object* targetObj)
{
    ScopedGameMode gm(GameMode::kUseOn);

    if (inventoryCommonInit() == -1) {
        return;
    }

    bool isoWasEnabled = _setup_inventory(INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
    for (;;) {
        sharedFpsLimiter.mark();

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        _display_body(-1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);

        int keyCode = inputGetInput();
        switch (keyCode) {
        case KEY_HOME:
            _stack_offset[_curr_stack] = 0;
            _display_inventory(0, -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        case KEY_ARROW_UP:
            if (_stack_offset[_curr_stack] > 0) {
                _stack_offset[_curr_stack] -= 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            }
            break;
        case KEY_PAGE_UP:
            _stack_offset[_curr_stack] -= gInventorySlotsCount;
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
                _display_inventory(_stack_offset[_curr_stack], -1, 1);
            }
            break;
        case KEY_END:
            _stack_offset[_curr_stack] = _pud->length - gInventorySlotsCount;
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        case KEY_ARROW_DOWN:
            if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                _stack_offset[_curr_stack] += 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            }
            break;
        case KEY_PAGE_DOWN:
            _stack_offset[_curr_stack] += gInventorySlotsCount;
            if (_stack_offset[_curr_stack] + gInventorySlotsCount >= _pud->length) {
                _stack_offset[_curr_stack] = _pud->length - gInventorySlotsCount;
                if (_stack_offset[_curr_stack] < 0) {
                    _stack_offset[_curr_stack] = 0;
                }
            }
            _display_inventory(_stack_offset[_curr_stack], -1, 1);
            break;
        case 2500:
            _container_exit(keyCode, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
            break;
        default:
            if ((mouseGetEvent() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_HAND) {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode < 1000 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                    } else {
                        int inventoryItemIndex = _pud->length - (_stack_offset[_curr_stack] + keyCode - 1000 + 1);
                        // SFALL: Fix crash when clicking on empty space in the inventory list
                        // opened by "Use Inventory Item On" (backpack) action icon
                        if (inventoryItemIndex < _pud->length && inventoryItemIndex >= 0) {
                            InventoryItem* inventoryItem = &(_pud->items[inventoryItemIndex]);
                            if (clientViewerActive()) {
                                // Viewer: don't run the local action-on-object (it would
                                // mutate the authoritative dude/target locally). Send the
                                // claim-gated wire verb — the server walks the dude to the
                                // target and runs the real _action_use_an_item_on_object;
                                // the outcome streams back (Slice 2 reconcile). netId 0 =
                                // an unsynced target → drop the pick, just close.
                                if (targetObj->netId > 0) {
                                    clientViewerUseItemOn(targetObj->netId, inventoryItem->item->pid);
                                }
                            } else {
                                // Ledger H-2: AP gate + debit extracted to core
                                // (actions.cc).
                                actionUseItemOnObjectWithApCost(gDude, targetObj, inventoryItem->item);
                            }
                            keyCode = KEY_ESCAPE;
                        } else {
                            keyCode = -1;
                        }
                    }
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_SCROLLER_X, INVENTORY_SCROLLER_Y, INVENTORY_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_stack_offset[_curr_stack] > 0) {
                            _stack_offset[_curr_stack] -= 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                        }
                    } else if (wheelY < 0) {
                        if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                            _stack_offset[_curr_stack] += 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_USE_ITEM_ON);
                        }
                    }
                }
            }
        }

        if (keyCode == KEY_ESCAPE) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    _exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inventoryCommonFree();
}

// Renders character's summary of SPECIAL stats, equipped armor bonuses,
// and weapon's damage/range.
//
// 0x471D5C
static void inventoryRenderSummary()
{
    int summaryStats[7];
    memcpy(summaryStats, gSummaryStats, sizeof(summaryStats));

    int summaryStats2[7];
    memcpy(summaryStats2, gSummaryStats2, sizeof(summaryStats2));

    char formattedText[80];

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            152,
            188,
            INVENTORY_WINDOW_WIDTH,
            windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            INVENTORY_WINDOW_WIDTH);
    }

    // Render character name.
    const char* critterName = critterGetName(_stack[0]);
    fontDrawText(windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X, critterName, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

    bufferDrawLine(windowBuffer,
        INVENTORY_WINDOW_WIDTH,
        INVENTORY_SUMMARY_X,
        3 * fontGetLineHeight() / 2 + INVENTORY_SUMMARY_Y,
        INVENTORY_SUMMARY_MAX_X,
        3 * fontGetLineHeight() / 2 + INVENTORY_SUMMARY_Y,
        _colorTable[992]);

    MessageListItem messageListItem;

    int offset = INVENTORY_WINDOW_WIDTH * 2 * fontGetLineHeight() + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X;
    for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
        messageListItem.num = stat;
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            fontDrawText(windowBuffer + offset, messageListItem.text, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
        }

        int value = critterGetStat(_stack[0], stat);
        snprintf(formattedText, sizeof(formattedText), "%d", value);
        fontDrawText(windowBuffer + offset + 24, formattedText, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();
    }

    offset -= INVENTORY_WINDOW_WIDTH * 7 * fontGetLineHeight();

    for (int index = 0; index < 7; index += 1) {
        messageListItem.num = 7 + index;
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            fontDrawText(windowBuffer + offset + 40, messageListItem.text, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
        }

        if (summaryStats2[index] == -1) {
            int value = critterGetStat(_stack[0], summaryStats[index]);
            snprintf(formattedText, sizeof(formattedText), "   %d", value);
        } else {
            int value1 = critterGetStat(_stack[0], summaryStats[index]);
            int value2 = critterGetStat(_stack[0], summaryStats2[index]);
            const char* format = index != 0 ? "%d/%d%%" : "%d/%d";
            snprintf(formattedText, sizeof(formattedText), format, value1, value2);
        }

        fontDrawText(windowBuffer + offset + 104, formattedText, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();
    }

    bufferDrawLine(windowBuffer, INVENTORY_WINDOW_WIDTH, INVENTORY_SUMMARY_X, 18 * fontGetLineHeight() / 2 + 48, INVENTORY_SUMMARY_MAX_X, 18 * fontGetLineHeight() / 2 + 48, _colorTable[992]);
    bufferDrawLine(windowBuffer, INVENTORY_WINDOW_WIDTH, INVENTORY_SUMMARY_X, 26 * fontGetLineHeight() / 2 + 48, INVENTORY_SUMMARY_MAX_X, 26 * fontGetLineHeight() / 2 + 48, _colorTable[992]);

    Object* itemsInHands[2] = {
        gInventoryLeftHandItem,
        gInventoryRightHandItem,
    };

    const int hitModes[2] = {
        HIT_MODE_LEFT_WEAPON_PRIMARY,
        HIT_MODE_RIGHT_WEAPON_PRIMARY,
    };

    const int secondaryHitModes[2] = {
        HIT_MODE_LEFT_WEAPON_SECONDARY,
        HIT_MODE_RIGHT_WEAPON_SECONDARY,
    };

    const int unarmedHitModes[2] = {
        HIT_MODE_PUNCH,
        HIT_MODE_KICK,
    };

    offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();

    for (int index = 0; index < 2; index += 1) {
        Object* item = itemsInHands[index];
        if (item == nullptr) {
            formattedText[0] = '\0';

            // No item
            messageListItem.num = 14;
            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                fontDrawText(windowBuffer + offset, messageListItem.text, 120, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
            }

            offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();

            // Unarmed dmg:
            messageListItem.num = 24;
            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                // SFALL: Display the actual damage values of unarmed attacks.
                // CE: Implementation is different.
                int hitMode = unarmedHitModes[index];
                if (_stack[0] == gDude) {
                    int actions[2];
                    interfaceGetItemActions(&(actions[0]), &(actions[1]));

                    bool isSecondary = actions[index] == INTERFACE_ITEM_ACTION_SECONDARY
                        || actions[index] == INTERFACE_ITEM_ACTION_SECONDARY_AIMING;

                    if (index == HAND_LEFT) {
                        hitMode = unarmedGetPunchHitMode(isSecondary);
                    } else {
                        hitMode = unarmedGetKickHitMode(isSecondary);
                    }
                }

                // Formula is the same as in `weaponGetDamage`.
                int minDamage;
                int maxDamage;
                int bonusDamage = unarmedGetDamage(hitMode, &minDamage, &maxDamage);
                int meleeDamage = critterGetStat(_stack[0], STAT_MELEE_DAMAGE);
                // TODO: Localize unarmed attack names.
                snprintf(formattedText, sizeof(formattedText), "%s %d-%d",
                    messageListItem.text,
                    bonusDamage + minDamage,
                    bonusDamage + meleeDamage + maxDamage);
            }

            fontDrawText(windowBuffer + offset, formattedText, 120, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

            offset += 3 * INVENTORY_WINDOW_WIDTH * fontGetLineHeight();
            continue;
        }

        const char* itemName = itemGetName(item);
        fontDrawText(windowBuffer + offset, itemName, 140, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

        offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();

        int itemType = itemGetType(item);
        if (itemType != ITEM_TYPE_WEAPON) {
            if (itemType == ITEM_TYPE_ARMOR) {
                // (Not worn)
                messageListItem.num = 18;
                if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                    fontDrawText(windowBuffer + offset, messageListItem.text, 120, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
                }
            }

            offset += 3 * INVENTORY_WINDOW_WIDTH * fontGetLineHeight();
            continue;
        }

        // SFALL: Fix displaying secondary mode weapon range.
        int hitMode = hitModes[index];
        if (_stack[0] == gDude) {
            int actions[2];
            interfaceGetItemActions(&(actions[0]), &(actions[1]));

            bool isSecondary = actions[index] == INTERFACE_ITEM_ACTION_SECONDARY
                || actions[index] == INTERFACE_ITEM_ACTION_SECONDARY_AIMING;

            if (isSecondary) {
                hitMode = secondaryHitModes[index];
            }
        }

        int range = weaponGetRange(_stack[0], hitMode);

        int damageMin;
        int damageMax;
        weaponGetDamageMinMax(item, &damageMin, &damageMax);

        // CE: Fix displaying secondary mode weapon damage (affects throwable
        // melee weapons - knifes, spears, etc.).
        int attackType = weaponGetAttackTypeForHitMode(item, hitMode);

        formattedText[0] = '\0';

        int meleeDamage;
        if (attackType == ATTACK_TYPE_MELEE || attackType == ATTACK_TYPE_UNARMED) {
            meleeDamage = critterGetStat(_stack[0], STAT_MELEE_DAMAGE);

            // SFALL: Display melee damage without "Bonus HtH Damage" bonus.
            if (damageModGetBonusHthDamageFix() && !damageModGetDisplayBonusDamage()) {
                meleeDamage -= 2 * perkGetRank(gDude, PERK_BONUS_HTH_DAMAGE);
            }
        } else {
            meleeDamage = 0;
        }

        messageListItem.num = 15; // Dmg:
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            if (attackType != 4 && range <= 1) {
                // SFALL: Display bonus damage.
                if (damageModGetBonusHthDamageFix() && damageModGetDisplayBonusDamage()) {
                    // CE: Just in case check for attack type, however it looks
                    // like we cannot be here with anything besides melee or
                    // unarmed.
                    if (_stack[0] == gDude && (attackType == ATTACK_TYPE_MELEE || attackType == ATTACK_TYPE_UNARMED)) {
                        // See explanation in `weaponGetDamage`.
                        damageMin += 2 * perkGetRank(gDude, PERK_BONUS_HTH_DAMAGE);
                    }
                }
                snprintf(formattedText, sizeof(formattedText), "%s %d-%d", messageListItem.text, damageMin, damageMax + meleeDamage);
            } else {
                MessageListItem rangeMessageListItem;
                rangeMessageListItem.num = 16; // Rng:
                if (messageListGetItem(&gInventoryMessageList, &rangeMessageListItem)) {
                    // SFALL: Display bonus damage.
                    if (damageModGetDisplayBonusDamage()) {
                        // CE: There is a bug in Sfall diplaying wrong damage
                        // bonus for melee weapons with range > 1 (spears,
                        // sledgehammers) and throwables (secondary mode).
                        if (_stack[0] == gDude && attackType == ATTACK_TYPE_RANGED) {
                            int damageBonus = 2 * perkGetRank(gDude, PERK_BONUS_RANGED_DAMAGE);
                            damageMin += damageBonus;
                            damageMax += damageBonus;
                        }
                    }

                    snprintf(formattedText, sizeof(formattedText), "%s %d-%d   %s %d", messageListItem.text, damageMin, damageMax + meleeDamage, rangeMessageListItem.text, range);
                }
            }

            fontDrawText(windowBuffer + offset, formattedText, 140, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
        }

        offset += INVENTORY_WINDOW_WIDTH * fontGetLineHeight();

        if (ammoGetCapacity(item) > 0) {
            int ammoTypePid = weaponGetAmmoTypePid(item);

            formattedText[0] = '\0';

            messageListItem.num = 17; // Ammo:
            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                if (ammoTypePid != -1) {
                    if (ammoGetQuantity(item) != 0) {
                        const char* ammoName = protoGetName(ammoTypePid);
                        int capacity = ammoGetCapacity(item);
                        int quantity = ammoGetQuantity(item);
                        snprintf(formattedText, sizeof(formattedText), "%s %d/%d %s", messageListItem.text, quantity, capacity, ammoName);
                    } else {
                        int capacity = ammoGetCapacity(item);
                        int quantity = ammoGetQuantity(item);
                        snprintf(formattedText, sizeof(formattedText), "%s %d/%d", messageListItem.text, quantity, capacity);
                    }
                }
            } else {
                int capacity = ammoGetCapacity(item);
                int quantity = ammoGetQuantity(item);
                snprintf(formattedText, sizeof(formattedText), "%s %d/%d", messageListItem.text, quantity, capacity);
            }

            fontDrawText(windowBuffer + offset, formattedText, 140, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
        }

        offset += 2 * INVENTORY_WINDOW_WIDTH * fontGetLineHeight();
    }

    // Total wt:
    messageListItem.num = 20;
    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
        if (PID_TYPE(_stack[0]->pid) == OBJ_TYPE_CRITTER) {
            int carryWeight = critterGetStat(_stack[0], STAT_CARRY_WEIGHT);
            int inventoryWeight = objectGetInventoryWeight(_stack[0]);
            snprintf(formattedText, sizeof(formattedText), "%s %d/%d", messageListItem.text, inventoryWeight, carryWeight);

            int color = _colorTable[992];
            if (critterIsEncumbered(_stack[0])) {
                color = _colorTable[31744];
            }

            fontDrawText(windowBuffer + offset + 15, formattedText, 120, INVENTORY_WINDOW_WIDTH, color);
        } else {
            int inventoryWeight = objectGetInventoryWeight(_stack[0]);
            snprintf(formattedText, sizeof(formattedText), "%s %d", messageListItem.text, inventoryWeight);

            fontDrawText(windowBuffer + offset + 30, formattedText, 80, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
        }
    }

    fontSetCurrent(oldFont);
}

// 0x472B54
static int _inven_from_button(int keyCode, Object** outItem, Object*** outItemSlot, Object** outOwner)
{
    Object** itemSlot;
    Object* owner;
    Object* item;
    int quantity = 0;

    switch (keyCode) {
    case 1006:
        itemSlot = &gInventoryRightHandItem;
        owner = _stack[0];
        item = gInventoryRightHandItem;
        break;
    case 1007:
        itemSlot = &gInventoryLeftHandItem;
        owner = _stack[0];
        item = gInventoryLeftHandItem;
        break;
    case 1008:
        itemSlot = &gInventoryArmor;
        owner = _stack[0];
        item = gInventoryArmor;
        break;
    default:
        itemSlot = nullptr;
        owner = nullptr;
        item = nullptr;

        InventoryItem* inventoryItem;
        if (keyCode < 2000) {
            int index = _stack_offset[_curr_stack] + keyCode - 1000;
            if (index >= _pud->length) {
                break;
            }

            inventoryItem = &(_pud->items[_pud->length - (index + 1)]);
            item = inventoryItem->item;
            owner = _stack[_curr_stack];
        } else if (keyCode < 2300) {
            int index = _target_stack_offset[_target_curr_stack] + keyCode - 2000;
            if (index >= _target_pud->length) {
                break;
            }

            inventoryItem = &(_target_pud->items[_target_pud->length - (index + 1)]);
            item = inventoryItem->item;
            owner = _target_stack[_target_curr_stack];
        } else if (keyCode < 2400) {
            int index = _ptable_offset + keyCode - 2300;
            if (index >= _ptable_pud->length) {
                break;
            }

            inventoryItem = &(_ptable_pud->items[_ptable_pud->length - (index + 1)]);
            item = inventoryItem->item;
            owner = _ptable;
        } else {
            int index = _btable_offset + keyCode - 2400;
            if (index >= _btable_pud->length) {
                break;
            }

            inventoryItem = &(_btable_pud->items[_btable_pud->length - (index + 1)]);
            item = inventoryItem->item;
            owner = _btable;
        }

        quantity = inventoryItem->quantity;
    }

    if (outItemSlot != nullptr) {
        *outItemSlot = itemSlot;
    }

    if (outItem != nullptr) {
        *outItem = item;
    }

    if (outOwner != nullptr) {
        *outOwner = owner;
    }

    if (quantity == 0 && item != nullptr) {
        quantity = 1;
    }

    return quantity;
}

// Displays item description.
//
// The [string] is mutated in the process replacing spaces back and forth
// for word wrapping purposes.
//
// inven_display_msg
// 0x472D24
static void inventoryRenderItemDescription(char* string)
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);
    windowBuffer += INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X;

    char* c = string;
    while (c != nullptr && *c != '\0') {
        _inven_display_msg_line += 1;
        if (_inven_display_msg_line > 17) {
            debugPrint("\nError: inven_display_msg: out of bounds!");
            return;
        }

        char* space = nullptr;
        if (fontGetStringWidth(c) > 152) {
            // Look for next space.
            space = c + 1;
            while (*space != '\0' && *space != ' ') {
                space += 1;
            }

            if (*space == '\0') {
                // This was the last line containing very long word. Text
                // drawing routine will silently truncate it after reaching
                // desired length.
                fontDrawText(windowBuffer + INVENTORY_WINDOW_WIDTH * _inven_display_msg_line * fontGetLineHeight(), c, 152, INVENTORY_WINDOW_WIDTH, _colorTable[992]);
                return;
            }

            char* nextSpace = space + 1;
            while (true) {
                while (*nextSpace != '\0' && *nextSpace != ' ') {
                    nextSpace += 1;
                }

                if (*nextSpace == '\0') {
                    break;
                }

                // Break string and measure it.
                *nextSpace = '\0';
                if (fontGetStringWidth(c) >= 152) {
                    // Next space is too far to fit in one line. Restore next
                    // space's character and stop.
                    *nextSpace = ' ';
                    break;
                }

                space = nextSpace;

                // Restore next space's character and continue looping from the
                // next character.
                *nextSpace = ' ';
                nextSpace += 1;
            }

            if (*space == ' ') {
                *space = '\0';
            }
        }

        if (fontGetStringWidth(c) > 152) {
            debugPrint("\nError: inven_display_msg: word too long!");
            return;
        }

        fontDrawText(windowBuffer + INVENTORY_WINDOW_WIDTH * _inven_display_msg_line * fontGetLineHeight(), c, 152, INVENTORY_WINDOW_WIDTH, _colorTable[992]);

        if (space != nullptr) {
            c = space + 1;
            if (*space == '\0') {
                *space = ' ';
            }
        } else {
            c = nullptr;
        }
    }

    fontSetCurrent(oldFont);
}

// Examines inventory item.
//
// 0x472EB8
static void inventoryExamineItem(Object* critter, Object* item)
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

    // Clear item description area.
    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 48, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            152,
            188,
            INVENTORY_WINDOW_WIDTH,
            windowBuffer + INVENTORY_WINDOW_WIDTH * INVENTORY_SUMMARY_Y + INVENTORY_SUMMARY_X,
            INVENTORY_WINDOW_WIDTH);
    }

    // Reset item description lines counter.
    _inven_display_msg_line = 0;

    // Render item's name.
    char* itemName = objectGetName(item);
    inventoryRenderItemDescription(itemName);

    // Increment line counter to accomodate separator below.
    _inven_display_msg_line += 1;

    int lineHeight = fontGetLineHeight();

    // Draw separator.
    // SFALL: Fix separator position when item name is longer than one line.
    bufferDrawLine(windowBuffer,
        INVENTORY_WINDOW_WIDTH,
        INVENTORY_SUMMARY_X,
        (_inven_display_msg_line - 1) * lineHeight + lineHeight / 2 + 49,
        INVENTORY_SUMMARY_MAX_X,
        (_inven_display_msg_line - 1) * lineHeight + lineHeight / 2 + 49,
        _colorTable[992]);

    // Examine item.
    _obj_examine_func(critter, item, inventoryRenderItemDescription);

    // Add weight if neccessary.
    int weight = itemGetWeight(item);
    if (weight != 0) {
        MessageListItem messageListItem;
        messageListItem.num = 540;

        if (weight == 1) {
            messageListItem.num = 541;
        }

        if (!messageListGetItem(&gProtoMessageList, &messageListItem)) {
            debugPrint("\nError: Couldn't find message!");
        }

        char formattedText[40];
        snprintf(formattedText, sizeof(formattedText), messageListItem.text, weight);
        inventoryRenderItemDescription(formattedText);
    }

    fontSetCurrent(oldFont);
}

// 0x47304C
static void inventoryWindowOpenContextMenu(int keyCode, int inventoryWindowType)
{
    Object* item;
    Object** itemSlot;
    Object* owner;

    int quantity = _inven_from_button(keyCode, &item, &itemSlot, &owner);
    if (quantity == 0) {
        return;
    }

    int itemType = itemGetType(item);

    int mouseState;
    do {
        sharedFpsLimiter.mark();

        inputGetInput();

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
            _display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);
        }

        mouseState = mouseGetEvent();
        if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
            if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
                _obj_look_at_func(_stack[0], item, gInventoryPrintItemDescriptionHandler);
            } else {
                inventoryExamineItem(_stack[0], item);
            }
            windowRefresh(gInventoryWindow);
            return;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseState & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) != MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT);

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_BLANK);

    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

    int x;
    int y;
    mouseGetPosition(&x, &y);

    int actionMenuItemsLength;
    const int* actionMenuItems;
    if (itemType == ITEM_TYPE_WEAPON && weaponCanBeUnloaded(item)) {
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL && objectGetOwner(item) != gDude) {
            actionMenuItemsLength = 3;
            actionMenuItems = _act_weap2;
        } else {
            actionMenuItemsLength = 4;
            actionMenuItems = _act_weap;
        }
    } else {
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
            // SFALL: Fix crash when trying to open bag/backpack on the table
            // in the bartering interface.
            Object* owner = objectGetOwner(item);
            if (owner != gDude) {
                if (itemType == ITEM_TYPE_CONTAINER && (owner == _stack[_curr_stack] || owner == _target_stack[_target_curr_stack])) {
                    actionMenuItemsLength = 3;
                    actionMenuItems = _act_just_use;
                } else {
                    actionMenuItemsLength = 2;
                    actionMenuItems = _act_nothing;
                }
            } else {
                if (itemType == ITEM_TYPE_CONTAINER) {
                    actionMenuItemsLength = 4;
                    actionMenuItems = _act_use;
                } else {
                    actionMenuItemsLength = 3;
                    actionMenuItems = _act_no_use;
                }
            }
        } else {
            if (itemType == ITEM_TYPE_CONTAINER && itemSlot != nullptr) {
                actionMenuItemsLength = 3;
                actionMenuItems = _act_no_use;
            } else {
                if (_obj_action_can_use(item) || _proto_action_can_use_on(item->pid)) {
                    actionMenuItemsLength = 4;
                    actionMenuItems = _act_use;
                } else {
                    actionMenuItemsLength = 3;
                    actionMenuItems = _act_no_use;
                }
            }
        }
    }

    const InventoryWindowDescription* windowDescription = &(gInventoryWindowDescriptions[inventoryWindowType]);

    Rect windowRect;
    windowGetRect(gInventoryWindow, &windowRect);
    int inventoryWindowX = windowRect.left;
    int inventoryWindowY = windowRect.top;

    gameMouseRenderActionMenuItems(x, y, actionMenuItems, actionMenuItemsLength,
        windowDescription->width + inventoryWindowX,
        windowDescription->height + inventoryWindowY);

    InventoryCursorData* cursorData = &(gInventoryCursorData[INVENTORY_WINDOW_CURSOR_MENU]);

    int offsetX;
    int offsetY;
    artGetRotationOffsets(cursorData->frm, 0, &offsetX, &offsetY);

    Rect rect;
    rect.left = x - inventoryWindowX - cursorData->width / 2 + offsetX;
    rect.top = y - inventoryWindowY - cursorData->height + 1 + offsetY;
    rect.right = rect.left + cursorData->width - 1;
    rect.bottom = rect.top + cursorData->height - 1;

    int menuButtonHeight = cursorData->height;
    if (rect.top + menuButtonHeight > windowDescription->height) {
        menuButtonHeight = windowDescription->height - rect.top;
    }

    int btn = buttonCreate(gInventoryWindow,
        rect.left,
        rect.top,
        cursorData->width,
        menuButtonHeight,
        -1,
        -1,
        -1,
        -1,
        cursorData->frmData,
        cursorData->frmData,
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    windowRefreshRect(gInventoryWindow, &rect);

    int menuItemIndex = 0;
    int previousMouseY = y;
    while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_UP) == 0) {
        sharedFpsLimiter.mark();

        inputGetInput();

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL) {
            _display_body(-1, INVENTORY_WINDOW_TYPE_NORMAL);
        }

        int x;
        int y;
        mouseGetPosition(&x, &y);
        if (y - previousMouseY > 10 || previousMouseY - y > 10) {
            if (y >= previousMouseY || menuItemIndex <= 0) {
                if (previousMouseY < y && menuItemIndex < actionMenuItemsLength - 1) {
                    menuItemIndex++;
                }
            } else {
                menuItemIndex--;
            }
            gameMouseHighlightActionMenuItemAtIndex(menuItemIndex);
            windowRefreshRect(gInventoryWindow, &rect);
            previousMouseY = y;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    buttonDestroy(btn);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        unsigned char* src = windowGetBuffer(_barter_back_win);
        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        blitBufferToBuffer(src + pitch * rect.top + rect.left + INVENTORY_TRADE_WINDOW_OFFSET,
            cursorData->width,
            menuButtonHeight,
            pitch,
            windowBuffer + windowDescription->width * rect.top + rect.left,
            windowDescription->width);
    } else {
        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, windowDescription->frmId, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            blitBufferToBuffer(backgroundFrmImage.getData() + windowDescription->width * rect.top + rect.left,
                cursorData->width,
                menuButtonHeight,
                windowDescription->width,
                windowBuffer + windowDescription->width * rect.top + rect.left,
                windowDescription->width);
        }
    }

    _mouse_set_position(x, y);

    _display_inventory(_stack_offset[_curr_stack], -1, inventoryWindowType);

    int actionMenuItem = actionMenuItems[menuItemIndex];
    switch (actionMenuItem) {
    case GAME_MOUSE_ACTION_MENU_ITEM_DROP:
        if (clientViewerActive()) {
            // Drop → wire verb (whole top-level stack; partial drops are later). Skip the
            // local unequip + itemDropStack — the server drops the item and streams the
            // removal (Slice 2 reconcile) plus a world SPAWN. Only the dude's own top-
            // level items (owner == gDude) are addressable by the pid verb; nested-
            // container drops are deferred (contents not streamed, Slice A2).
            if (owner == gDude) {
                clientViewerDrop(item);
            }
            break;
        }
        if (itemSlot != nullptr) {
            if (itemSlot == &gInventoryArmor) {
                _adjust_ac(_stack[0], item, nullptr);
            }
            itemAdd(owner, item, 1);
            quantity = 1;
            *itemSlot = nullptr;
        }

        if (quantity > 1 && !explosiveIsActiveExplosive(item->pid)) {
            quantity = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity);
        }

        if (itemDropStack(owner, item, quantity)) {
            _dropped_explosive = 1;
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_LOOK:
        if (inventoryWindowType != INVENTORY_WINDOW_TYPE_NORMAL) {
            _obj_examine_func(_stack[0], item, gInventoryPrintItemDescriptionHandler);
        } else {
            inventoryExamineItem(_stack[0], item);
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_USE:
        switch (itemType) {
        case ITEM_TYPE_CONTAINER:
            // View-only navigation into the container's contents (blob-loaded); safe.
            _container_enter(keyCode, inventoryWindowType);
            break;
        case ITEM_TYPE_DRUG:
            if (clientViewerActive()) {
                // USE mutates the dude — fire the claim-gated wire verb + skip the
                // local mutation; the server's itemUseDrug streams the result back
                // (Slice 2 reconcile). Explosives are rejected server-side.
                clientViewerUseItem(item->pid);
                break;
            }
            if (itemUseDrug(_stack[0], owner, item, itemSlot == nullptr)) {
                if (itemSlot != nullptr) {
                    *itemSlot = nullptr;
                }
            }
            break;
        case ITEM_TYPE_WEAPON:
        case ITEM_TYPE_MISC:
            if (clientViewerActive()) {
                if (explosiveIsExplosive(item->pid)) {
                    // C4 / dynamite: the plain useitem verb rejects explosives (their
                    // arm opens the blocking SET_TIMER modal, unwired server-side). Run
                    // that timer dial LOCALLY here — a pure UI choice — then send the
                    // chosen countdown via the dedicated arm verb. The server arms the
                    // charge headless; the timed explosion streams back. Skip the local
                    // arm entirely (no local _obj_use_explosive / queue mutation).
                    int seconds = _inven_set_timer(item);
                    if (seconds != -1) {
                        clientViewerArmExplosive(item->pid, seconds);
                    }
                    break;
                }
                clientViewerUseItem(item->pid); // as above (misc/weapon self-use)
                break;
            }
            if (itemUseFromInventory(_stack[0], owner, item, itemSlot == nullptr) == 1) {
                if (itemSlot != nullptr) {
                    *itemSlot = nullptr;
                }
            }
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_UNLOAD:
        if (clientViewerActive()) {
            break; // ammo/charges not streamed yet (Slice A2) — deferred to Slice 3c
        }
        weaponUnloadIntoInventory(owner, item, itemSlot == nullptr);
        break;
    default:
        break;
    }

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_NORMAL && actionMenuItem != GAME_MOUSE_ACTION_MENU_ITEM_LOOK) {
        inventoryRenderSummary();
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_LOOT
        || inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, inventoryWindowType);
    }

    _display_inventory(_stack_offset[_curr_stack], -1, inventoryWindowType);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_TRADE) {
        inventoryWindowRenderInnerInventories(_barter_back_win, _ptable, _btable, -1);
    }

    _adjust_fid();
}

// 0x473904
int inventoryOpenLooting(Object* looter, Object* target)
{
    int arrowFrmIds[INVENTORY_ARROW_FRM_COUNT];
    FrmImage arrowFrmImages[INVENTORY_ARROW_FRM_COUNT];
    MessageListItem messageListItem;

    memcpy(arrowFrmIds, gInventoryArrowFrmIds, sizeof(gInventoryArrowFrmIds));

    if (looter != _inven_dude) {
        return 0;
    }

    ScopedGameMode gm(GameMode::kLoot);

    switch (lootOpenCheck(looter, target, _gIsSteal)) {
    case LOOT_OPEN_NO_STEAL:
        // You can't find anything to take from that.
        messageListItem.num = 50;
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }
        return 0;
    case LOOT_OPEN_BLOCKED:
        return 0;
    default:
        break;
    }

    if (inventoryCommonInit() == -1) {
        return 0;
    }

    _target_pud = &(target->data.inventory);
    _target_curr_stack = 0;
    _target_stack_offset[0] = 0;
    _target_stack[0] = target;

    Object* item1 = nullptr;
    Object* item2 = nullptr;
    Object* armor = nullptr;

    Object* hiddenBox = lootTargetDetach(target, _gIsSteal, &item1, &item2, &armor);
    if (hiddenBox == nullptr) {
        return 0;
    }

    bool isoWasEnabled = _setup_inventory(INVENTORY_WINDOW_TYPE_LOOT);

    Object** critters = nullptr;
    int critterCount = 0;
    int critterIndex = 0;
    if (!_gIsSteal) {
        if (FID_TYPE(target->fid) == OBJ_TYPE_CRITTER) {
            critterCount = objectListCreate(target->tile, target->elevation, OBJ_TYPE_CRITTER, &critters);
            int endIndex = critterCount - 1;
            for (int index = 0; index < critterCount; index++) {
                Object* critter = critters[index];
                if ((critter->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) == 0) {
                    critters[index] = critters[endIndex];
                    critters[endIndex] = critter;
                    critterCount--;
                    index--;
                    endIndex--;
                } else {
                    critterIndex++;
                }
            }

            if (critterCount == 1) {
                objectListFree(critters);
                critterCount = 0;
            }

            if (critterCount > 1) {
                int fid;
                int btn;

                // Setup left arrow button.
                fid = buildFid(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_LEFT_ARROW_UP], 0, 0, 0);
                arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_UP].lock(fid);

                fid = buildFid(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN], 0, 0, 0);
                arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN].lock(fid);

                if (arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_UP].isLocked() && arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN].isLocked()) {
                    btn = buttonCreate(gInventoryWindow,
                        436,
                        162,
                        20,
                        18,
                        -1,
                        -1,
                        KEY_PAGE_UP,
                        -1,
                        arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_UP].getData(),
                        arrowFrmImages[INVENTORY_ARROW_FRM_LEFT_ARROW_DOWN].getData(),
                        nullptr,
                        0);
                    if (btn != -1) {
                        buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
                    }
                }

                // Setup right arrow button.
                fid = buildFid(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP], 0, 0, 0);
                arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP].lock(fid);

                fid = buildFid(OBJ_TYPE_INTERFACE, arrowFrmIds[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN], 0, 0, 0);
                arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN].lock(fid);

                if (arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP].isLocked() && arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN].isLocked()) {
                    btn = buttonCreate(gInventoryWindow,
                        456,
                        162,
                        20,
                        18,
                        -1,
                        -1,
                        KEY_PAGE_DOWN,
                        -1,
                        arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_UP].getData(),
                        arrowFrmImages[INVENTORY_ARROW_FRM_RIGHT_ARROW_DOWN].getData(),
                        nullptr,
                        0);
                    if (btn != -1) {
                        buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
                    }
                }

                for (int index = 0; index < critterCount; index++) {
                    if (target == critters[index]) {
                        critterIndex = index;
                    }
                }
            }
        }
    }

    _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
    _display_body(target->fid, INVENTORY_WINDOW_TYPE_LOOT);
    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);

    bool isCaughtStealing = false;
    int stealingXp = 0;
    int stealingXpBonus = 10;
    for (;;) {
        sharedFpsLimiter.mark();

        // Wire viewer: tell the decoder which container this screen is looting so its
        // inventory delta gets a FULL contents reconcile (add/remove/qty) instead of the
        // equip-flags-only path — set every iteration so it tracks the corpse-cycle
        // (PAGE_UP/DOWN) target. Set BEFORE inputGetInput so the pump inside it reconciles
        // against the right target. Cleared on exit below.
        if (clientViewerActive()) {
            clientViewerSetLootTarget(_target_stack[_target_curr_stack]->netId);
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        if (isCaughtStealing) {
            break;
        }

        int keyCode = inputGetInput();

        // Wire viewer: loot transfers (take/put/takeall) are server-authoritative —
        // the local move is skipped and the truth streams back as an inventory
        // reconcile pumped by the service ticker inside inputGetInput. Repaint BOTH
        // panels when it lands: the dude-inv dirty flag fires when the dude side changes
        // (take/takeall/put), the loot-target dirty flag when the container side is
        // reconciled — consume BOTH (bitwise | so neither is short-circuited away), else
        // an async transfer would not show until the next user event. Clamp both scroll
        // offsets in case items were removed out from under them.
        if (clientViewerActive()
            && (static_cast<int>(clientViewerConsumeDudeInvDirty())
                    | static_cast<int>(clientViewerConsumeLootTargetInvDirty()))
                != 0) {
            if (_stack_offset[_curr_stack] > _pud->length - gInventorySlotsCount) {
                _stack_offset[_curr_stack] = _pud->length - gInventorySlotsCount;
            }
            if (_stack_offset[_curr_stack] < 0) {
                _stack_offset[_curr_stack] = 0;
            }
            if (_target_stack_offset[_target_curr_stack] > _target_pud->length - gInventorySlotsCount) {
                _target_stack_offset[_target_curr_stack] = _target_pud->length - gInventorySlotsCount;
            }
            if (_target_stack_offset[_target_curr_stack] < 0) {
                _target_stack_offset[_target_curr_stack] = 0;
            }
            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
            // BLIT IT — see the identical note in inventoryOpen. Both display calls
            // above draw into the window buffer only, and this block runs off an
            // async reconcile rather than a user event, so nothing else blits. This
            // is why a deposited item showed no sprite on the container side until
            // the mouse moved over it, and why the weight/capacity line (drawn by
            // these same two functions) never updated live either.
            windowRefresh(gInventoryWindow);
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        if (keyCode == KEY_UPPERCASE_A) {
            if (!_gIsSteal) {
                if (clientViewerActive()) {
                    // Wire viewer: "Take All" must be server-authoritative. The local
                    // lootTakeAll would move the container's items into the dude's LOCAL
                    // mirror only — the server never gets them, so they are client-only
                    // phantoms that vanish on the next inventory reconcile (and cannot be
                    // dropped/used). Fire the takeall verb + SKIP the local move; the
                    // result streams back and the reconcile repaints both panels (top of
                    // this loop). The server enforces the carry cap, so no local dialog.
                    clientViewerLootTakeAll(target->netId);
                } else if (lootTakeAll(looter, target)) {
                    _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                } else {
                    // Sorry, you cannot carry that much.
                    messageListItem.num = 31;
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        showDialogBox(messageListItem.text, nullptr, 0, 169, 117, _colorTable[32328], nullptr, _colorTable[32328], 0);
                    }
                }
            }
        } else if (keyCode == KEY_ARROW_UP) {
            if (_stack_offset[_curr_stack] > 0) {
                _stack_offset[_curr_stack] -= 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_PAGE_UP) {
            if (critterCount != 0) {
                if (critterIndex > 0) {
                    critterIndex -= 1;
                } else {
                    critterIndex = critterCount - 1;
                }

                target = critters[critterIndex];
                _target_pud = &(target->data.inventory);
                _target_stack[0] = target;
                _target_curr_stack = 0;
                _target_stack_offset[0] = 0;
                _display_target_inventory(0, -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                _display_body(target->fid, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                _stack_offset[_curr_stack] += 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_PAGE_DOWN) {
            if (critterCount != 0) {
                if (critterIndex < critterCount - 1) {
                    critterIndex += 1;
                } else {
                    critterIndex = 0;
                }

                target = critters[critterIndex];
                _target_pud = &(target->data.inventory);
                _target_stack[0] = target;
                _target_curr_stack = 0;
                _target_stack_offset[0] = 0;
                _display_target_inventory(0, -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                _display_body(target->fid, INVENTORY_WINDOW_TYPE_LOOT);
            }
        } else if (keyCode == KEY_CTRL_ARROW_UP) {
            if (_target_stack_offset[_target_curr_stack] > 0) {
                _target_stack_offset[_target_curr_stack] -= 1;
                _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                windowRefresh(gInventoryWindow);
            }
        } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
            if (_target_stack_offset[_target_curr_stack] + gInventorySlotsCount < _target_pud->length) {
                _target_stack_offset[_target_curr_stack] += 1;
                _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                windowRefresh(gInventoryWindow);
            }
        } else if (keyCode >= 2500 && keyCode <= 2501) {
            _container_exit(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
        } else {
            if ((mouseGetEvent() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_HAND) {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1000 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
                    } else {
                        int slotIndex = keyCode - 1000;
                        if (slotIndex + _stack_offset[_curr_stack] < _pud->length) {
                            _gStealCount += 1;
                            _gStealSize += itemGetSize(_stack[_curr_stack]);

                            InventoryItem* inventoryItem = &(_pud->items[_pud->length - (slotIndex + _stack_offset[_curr_stack] + 1)]);
                            // On the wire viewer _move_inventory runs the full vanilla drag
                            // (pick-up onto the cursor, hold, drop) but its terminal transfer
                            // is rerouted to a wire verb inside the function (server-
                            // authoritative); the reconcile repaints both panels.
                            InventoryMoveResult rc = _move_inventory(inventoryItem->item, slotIndex, _target_stack[_target_curr_stack], true);
                            if (rc == INVENTORY_MOVE_RESULT_CAUGHT_STEALING) {
                                isCaughtStealing = true;
                            } else if (rc == INVENTORY_MOVE_RESULT_SUCCESS) {
                                stealingXp += stealingXpBonus;
                                stealingXpBonus += 10;
                            }

                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }

                        keyCode = -1;
                    }
                } else if (keyCode >= 2000 && keyCode <= 2000 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_LOOT);
                    } else {
                        int slotIndex = keyCode - 2000;
                        if (slotIndex + _target_stack_offset[_target_curr_stack] < _target_pud->length) {
                            _gStealCount += 1;
                            _gStealSize += itemGetSize(_stack[_curr_stack]);

                            InventoryItem* inventoryItem = &(_target_pud->items[_target_pud->length - (slotIndex + _target_stack_offset[_target_curr_stack] + 1)]);
                            // _move_inventory runs the vanilla drag; its terminal transfer is
                            // rerouted to a wire verb inside the function on the viewer.
                            InventoryMoveResult rc = _move_inventory(inventoryItem->item, slotIndex, _target_stack[_target_curr_stack], false);
                            if (rc == INVENTORY_MOVE_RESULT_CAUGHT_STEALING) {
                                isCaughtStealing = true;
                            } else if (rc == INVENTORY_MOVE_RESULT_SUCCESS) {
                                stealingXp += stealingXpBonus;
                                stealingXpBonus += 10;
                            }

                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    }
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_stack_offset[_curr_stack] > 0) {
                            _stack_offset[_curr_stack] -= 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    } else if (wheelY < 0) {
                        if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                            _stack_offset[_curr_stack] += 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_LOOT);
                        }
                    }
                } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_target_stack_offset[_target_curr_stack] > 0) {
                            _target_stack_offset[_target_curr_stack] -= 1;
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            windowRefresh(gInventoryWindow);
                        }
                    } else if (wheelY < 0) {
                        if (_target_stack_offset[_target_curr_stack] + gInventorySlotsCount < _target_pud->length) {
                            _target_stack_offset[_target_curr_stack] += 1;
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
                            windowRefresh(gInventoryWindow);
                        }
                    }
                }
            }
        }

        if (keyCode == KEY_ESCAPE) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (critterCount != 0) {
        objectListFree(critters);
    }

    lootTargetReattach(target, hiddenBox, item1, item2, armor);

    if (_gIsSteal && !isCaughtStealing && stealingXp > 0) {
        // SFALL: Display actual xp received.
        int xpGained;
        if (lootStealExperience(looter, target, stealingXp, &xpGained)) {
            // You gain %d experience points for successfully using your Steal skill.
            messageListItem.num = 29;
            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                char formattedText[200];
                snprintf(formattedText, sizeof(formattedText), messageListItem.text, xpGained);
                presenter()->consoleMessage(formattedText);
            }
        }
    }

    _exit_inventory(isoWasEnabled);

    // NOTE: Uninline.
    inventoryCommonFree();

    // Wire viewer: the loot screen is closed — stop full-reconciling this container
    // (its delta reverts to the equip-flags-only path) and drop any pending dirty.
    if (clientViewerActive()) {
        clientViewerSetLootTarget(0);
    }

    if (_gIsSteal && isCaughtStealing && _gStealCount > 0) {
        lootCaughtStealingReact(looter, target);
    }

    return 0;
}

// 0x4746A0
int inventoryOpenStealing(Object* thief, Object* target)
{
    if (thief == target) {
        return -1;
    }

    _gIsSteal = PID_TYPE(thief->pid) == OBJ_TYPE_CRITTER && critterIsActive(target);
    _gStealCount = 0;
    _gStealSize = 0;

    int rc = inventoryOpenLooting(thief, target);

    _gIsSteal = 0;
    _gStealCount = 0;
    _gStealSize = 0;

    return rc;
}

// 0x474708
static InventoryMoveResult _move_inventory(Object* item, int slotIndex, Object* targetObj, bool isPlanting)
{
    bool needRefresh = true;

    Rect rect;

    int quantity;
    if (isPlanting) {
        rect.left = INVENTORY_LOOT_LEFT_SCROLLER_X;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + INVENTORY_LOOT_LEFT_SCROLLER_Y;

        InventoryItem* inventoryItem = &(_pud->items[_pud->length - (slotIndex + _stack_offset[_curr_stack] + 1)]);
        quantity = inventoryItem->quantity;
        if (quantity > 1) {
            _display_inventory(_stack_offset[_curr_stack], slotIndex, INVENTORY_WINDOW_TYPE_LOOT);
            needRefresh = false;
        }
    } else {
        rect.left = INVENTORY_LOOT_RIGHT_SCROLLER_X;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + INVENTORY_LOOT_RIGHT_SCROLLER_Y;

        InventoryItem* inventoryItem = &(_target_pud->items[_target_pud->length - (slotIndex + _target_stack_offset[_target_curr_stack] + 1)]);
        quantity = inventoryItem->quantity;
        if (quantity > 1) {
            _display_target_inventory(_target_stack_offset[_target_curr_stack], slotIndex, _target_pud, INVENTORY_WINDOW_TYPE_LOOT);
            windowRefresh(gInventoryWindow);
            needRefresh = false;
        }
    }

    if (needRefresh) {
        unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 114, 0, 0, 0);
        if (backgroundFrmImage.lock(backgroundFid)) {
            blitBufferToBuffer(backgroundFrmImage.getData() + INVENTORY_LOOT_WINDOW_WIDTH * rect.top + rect.left,
                INVENTORY_SLOT_WIDTH,
                INVENTORY_SLOT_HEIGHT,
                INVENTORY_LOOT_WINDOW_WIDTH,
                windowBuffer + INVENTORY_LOOT_WINDOW_WIDTH * rect.top + rect.left,
                INVENTORY_LOOT_WINDOW_WIDTH);
        }

        rect.right = rect.left + INVENTORY_SLOT_WIDTH - 1;
        rect.bottom = rect.top + INVENTORY_SLOT_HEIGHT - 1;
        windowRefreshRect(gInventoryWindow, &rect);
    }

    FrmImage itemInventoryFrmImage;
    int itemInventoryFid = itemGetInventoryFid(item);
    if (itemInventoryFrmImage.lock(itemInventoryFid)) {
        int width = itemInventoryFrmImage.getWidth();
        int height = itemInventoryFrmImage.getHeight();
        unsigned char* data = itemInventoryFrmImage.getData();
        mouseSetFrame(data, width, height, width, width / 2, height / 2, 0);
        soundPlayFile("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        inputGetInput();

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (itemInventoryFrmImage.isLocked()) {
        itemInventoryFrmImage.unlock();
        soundPlayFile("iputdown");
    }

    InventoryMoveResult result = INVENTORY_MOVE_RESULT_FAILED;
    MessageListItem messageListItem;

    if (isPlanting) {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_LOOT_RIGHT_SCROLLER_X, INVENTORY_LOOT_RIGHT_SCROLLER_Y, INVENTORY_LOOT_RIGHT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_LOOT_RIGHT_SCROLLER_Y)) {
            int quantityToMove;
            if (quantity > 1) {
                quantityToMove = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity);
            } else {
                quantityToMove = 1;
            }

            if (quantityToMove != -1) {
                if (clientViewerActive()) {
                    // Wire viewer: the drag above ran locally for FEEL; the actual transfer
                    // is server-authoritative. Fire the put verb (dude→container) and report
                    // success optimistically — the server runs the real itemMove and the
                    // reconcile repaints both panels. NO local lootTransferItem (that would
                    // make a client-only phantom the server never has). Read pid now (still
                    // valid — solo, nothing frees it mid-drag; the container-item deferred-
                    // free + item-instance-ids for the co-op case are banked).
                    clientViewerLootPut(targetObj->netId, item->pid, quantityToMove);
                    result = INVENTORY_MOVE_RESULT_SUCCESS;
                } else {
                    // Batch-6 split: transfer rules extracted to core (item.cc).
                    switch (lootTransferItem(_inven_dude, targetObj, item, quantityToMove, true, _gIsSteal != 0)) {
                    case LOOT_TRANSFER_CAUGHT_STEALING:
                        result = INVENTORY_MOVE_RESULT_CAUGHT_STEALING;
                        break;
                    case LOOT_TRANSFER_OK:
                        result = INVENTORY_MOVE_RESULT_SUCCESS;
                        break;
                    case LOOT_TRANSFER_NO_ROOM:
                        // There is no space left for that item.
                        messageListItem.num = 26;
                        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                            presenter()->consoleMessage(messageListItem.text);
                        }
                        break;
                    }
                }
            }
        }
    } else {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_LOOT_LEFT_SCROLLER_X, INVENTORY_LOOT_LEFT_SCROLLER_Y, INVENTORY_LOOT_LEFT_SCROLLER_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_LOOT_LEFT_SCROLLER_Y)) {
            int quantityToMove;
            if (quantity > 1) {
                quantityToMove = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity);
            } else {
                quantityToMove = 1;
            }

            if (quantityToMove != -1) {
                if (clientViewerActive()) {
                    // Wire viewer: drag ran locally for feel; fire the take verb
                    // (container→dude) server-authoritatively (see the plant branch above).
                    clientViewerLootTake(targetObj->netId, item->pid, quantityToMove);
                    result = INVENTORY_MOVE_RESULT_SUCCESS;
                } else {
                    // Batch-6 split: transfer rules extracted to core (item.cc).
                    switch (lootTransferItem(_inven_dude, targetObj, item, quantityToMove, false, _gIsSteal != 0)) {
                    case LOOT_TRANSFER_CAUGHT_STEALING:
                        result = INVENTORY_MOVE_RESULT_CAUGHT_STEALING;
                        break;
                    case LOOT_TRANSFER_OK:
                        result = INVENTORY_MOVE_RESULT_SUCCESS;
                        break;
                    case LOOT_TRANSFER_NO_ROOM:
                        // You cannot pick that up. You are at your maximum weight capacity.
                        messageListItem.num = 25;
                        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                            presenter()->consoleMessage(messageListItem.text);
                        }
                        break;
                    }
                }
            }
        }
    }

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);

    return result;
}

// 0x474DAC
static void _barter_move_inventory(Object* item, int quantity, int slotIndex, int indexOffset, Object* npc, Object* sourceTable, bool fromDude)
{
    Rect rect;
    if (fromDude) {
        rect.left = 23;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + 34;
    } else {
        rect.left = 395;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + 31;
    }

    if (quantity > 1) {
        if (fromDude) {
            _display_inventory(indexOffset, slotIndex, INVENTORY_WINDOW_TYPE_TRADE);
        } else {
            _display_target_inventory(indexOffset, slotIndex, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
        }
    } else {
        unsigned char* dest = windowGetBuffer(gInventoryWindow);
        unsigned char* src = windowGetBuffer(_barter_back_win);

        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        blitBufferToBuffer(src + pitch * rect.top + rect.left + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, INVENTORY_SLOT_HEIGHT, pitch, dest + INVENTORY_TRADE_WINDOW_WIDTH * rect.top + rect.left, INVENTORY_TRADE_WINDOW_WIDTH);

        rect.right = rect.left + INVENTORY_SLOT_WIDTH - 1;
        rect.bottom = rect.top + INVENTORY_SLOT_HEIGHT - 1;
        windowRefreshRect(gInventoryWindow, &rect);
    }

    FrmImage itemInventoryFrmImage;
    int itemInventoryFid = itemGetInventoryFid(item);
    if (itemInventoryFrmImage.lock(itemInventoryFid)) {
        int width = itemInventoryFrmImage.getWidth();
        int height = itemInventoryFrmImage.getHeight();
        unsigned char* data = itemInventoryFrmImage.getData();
        mouseSetFrame(data, width, height, width, width / 2, height / 2, 0);
        soundPlayFile("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        inputGetInput();

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (itemInventoryFrmImage.isLocked()) {
        itemInventoryFrmImage.unlock();
        soundPlayFile("iputdown");
    }

    MessageListItem messageListItem;

    if (fromDude) {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity) : 1;
            if (quantityToMove != -1) {
                if (clientBarterIsDriver()) {
                    // Viewer driver: OFFER is authoritative on the SERVER. Send the
                    // verb; do not mutate the mirror locally -- the server echoes the
                    // result as EVENT_BARTER_STATE, applied at the trade-loop top.
                    // The pickup/drag gesture and quantity dial above are pure vanilla
                    // and ran locally for feel.
                    clientViewerBarterVerb("boffer", item->pid, quantityToMove);
                } else if (itemMoveForce(_inven_dude, sourceTable, item, quantityToMove) == -1) {
                    // There is no space left for that item.
                    messageListItem.num = 26;
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        presenter()->consoleMessage(messageListItem.text);
                    }
                }
            }
        }
    } else {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity) : 1;
            if (quantityToMove != -1) {
                if (clientBarterIsDriver()) {
                    // Viewer driver: TAKE moves a merchant item onto the buy table on
                    // the SERVER (see the boffer note above).
                    clientViewerBarterVerb("btake", item->pid, quantityToMove);
                } else if (itemMoveForce(npc, sourceTable, item, quantityToMove) == -1) {
                    // You cannot pick that up. You are at your maximum weight capacity.
                    messageListItem.num = 25;
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        presenter()->consoleMessage(messageListItem.text);
                    }
                }
            }
        }
    }

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
}

// 0x475070
static void _barter_move_from_table_inventory(Object* item, int quantity, int slotIndex, Object* npc, Object* sourceTable, bool fromDude)
{
    Rect rect;
    if (fromDude) {
        rect.left = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD;
    } else {
        rect.left = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        rect.top = INVENTORY_SLOT_HEIGHT * slotIndex + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD;
    }

    if (quantity > 1) {
        if (fromDude) {
            inventoryWindowRenderInnerInventories(_barter_back_win, sourceTable, nullptr, slotIndex);
        } else {
            inventoryWindowRenderInnerInventories(_barter_back_win, nullptr, sourceTable, slotIndex);
        }
    } else {
        unsigned char* dest = windowGetBuffer(gInventoryWindow);
        unsigned char* src = windowGetBuffer(_barter_back_win);

        int pitch = INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH;
        blitBufferToBuffer(src + pitch * rect.top + rect.left + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, INVENTORY_SLOT_HEIGHT, pitch, dest + INVENTORY_TRADE_WINDOW_WIDTH * rect.top + rect.left, INVENTORY_TRADE_WINDOW_WIDTH);

        rect.right = rect.left + INVENTORY_SLOT_WIDTH - 1;
        rect.bottom = rect.top + INVENTORY_SLOT_HEIGHT - 1;
        windowRefreshRect(gInventoryWindow, &rect);
    }

    FrmImage itemInventoryFrmImage;
    int itemInventoryFid = itemGetInventoryFid(item);
    if (itemInventoryFrmImage.lock(itemInventoryFid)) {
        int width = itemInventoryFrmImage.getWidth();
        int height = itemInventoryFrmImage.getHeight();
        unsigned char* data = itemInventoryFrmImage.getData();
        mouseSetFrame(data, width, height, width, width / 2, height / 2, 0);
        soundPlayFile("ipickup1");
    }

    do {
        sharedFpsLimiter.mark();

        inputGetInput();

        renderPresent();
        sharedFpsLimiter.throttle();
    } while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0);

    if (itemInventoryFrmImage.isLocked()) {
        itemInventoryFrmImage.unlock();
        soundPlayFile("iputdown");
    }

    MessageListItem messageListItem;

    if (fromDude) {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity) : 1;
            if (quantityToMove != -1) {
                if (clientBarterIsDriver()) {
                    // Viewer driver: pulling a good back off the player's offer table.
                    // The server's UNOFFER searches both tables by pid, so one verb
                    // covers either direction (see the boffer note above).
                    clientViewerBarterVerb("bunoffer", item->pid, quantityToMove);
                } else if (itemMoveForce(sourceTable, _inven_dude, item, quantityToMove) == -1) {
                    // There is no space left for that item.
                    messageListItem.num = 26;
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        presenter()->consoleMessage(messageListItem.text);
                    }
                }
            }
        }
    } else {
        if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y)) {
            int quantityToMove = quantity > 1 ? inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity) : 1;
            if (quantityToMove != -1) {
                if (clientBarterIsDriver()) {
                    // Viewer driver: pulling a good back off the merchant's buy table
                    // (see the bunoffer note above -- one verb, both tables).
                    clientViewerBarterVerb("bunoffer", item->pid, quantityToMove);
                } else if (itemMoveForce(sourceTable, npc, item, quantityToMove) == -1) {
                    // You cannot pick that up. You are at your maximum weight capacity.
                    messageListItem.num = 25;
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        presenter()->consoleMessage(messageListItem.text);
                    }
                }
            }
        }
    }

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
}

// 0x475334
static void inventoryWindowRenderInnerInventories(int win, Object* leftTable, Object* rightTable, int draggedSlotIndex)
{
    unsigned char* windowBuffer = windowGetBuffer(gInventoryWindow);

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    char formattedText[80];
    int rectHeight = fontGetLineHeight() + INVENTORY_SLOT_HEIGHT * gInventorySlotsCount;

    if (leftTable != nullptr) {
        unsigned char* src = windowGetBuffer(win);
        blitBufferToBuffer(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, rectHeight + 1, INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH, windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD, INVENTORY_TRADE_WINDOW_WIDTH);

        unsigned char* dest = windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        Inventory* inventory = &(leftTable->data.inventory);
        for (int index = 0; index < gInventorySlotsCount && index + _ptable_offset < inventory->length; index++) {
            InventoryItem* inventoryItem = &(inventory->items[inventory->length - (index + _ptable_offset + 1)]);
            int inventoryFid = itemGetInventoryFid(inventoryItem->item);
            artRender(inventoryFid, dest, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, INVENTORY_TRADE_WINDOW_WIDTH);
            _display_inventory_info(inventoryItem->item, inventoryItem->quantity, dest, INVENTORY_TRADE_WINDOW_WIDTH, index == draggedSlotIndex);

            dest += INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_SLOT_HEIGHT;
        }

        if (gGameDialogSpeakerIsPartyMember) {
            MessageListItem messageListItem;
            messageListItem.num = 30;

            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                int weight = objectGetInventoryWeight(leftTable);
                snprintf(formattedText, sizeof(formattedText), "%s %d", messageListItem.text, weight);
            }
        } else {
            // On a viewer the offer value is SERVER-COMPUTED and streamed (prices
            // are not client-derivable: party best barter skill, a dude==gDude
            // Master Trader check, and a script-mutated reaction all feed it). Off
            // the wire, compute locally as vanilla does.
            int cost = clientBarterActive() ? clientBarterOfferValue() : objectGetCost(leftTable);
            snprintf(formattedText, sizeof(formattedText), "$%d", cost);
        }

        fontDrawText(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD) + INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD, formattedText, 80, INVENTORY_TRADE_WINDOW_WIDTH, _colorTable[32767]);

        Rect rect;
        rect.left = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD;
        rect.top = INVENTORY_TRADE_INNER_LEFT_SCROLLER_Y_PAD;
        // NOTE: Odd math, the only way to get 223 is to subtract 2.
        rect.right = INVENTORY_TRADE_INNER_LEFT_SCROLLER_X_PAD + INVENTORY_SLOT_WIDTH_PAD - 2;
        rect.bottom = rect.top + rectHeight;
        windowRefreshRect(gInventoryWindow, &rect);
    }

    if (rightTable != nullptr) {
        unsigned char* src = windowGetBuffer(win);
        blitBufferToBuffer(src + INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD + INVENTORY_TRADE_WINDOW_OFFSET, INVENTORY_SLOT_WIDTH, rectHeight + 1, INVENTORY_TRADE_BACKGROUND_WINDOW_WIDTH, windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD, INVENTORY_TRADE_WINDOW_WIDTH);

        unsigned char* dest = windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        Inventory* inventory = &(rightTable->data.inventory);
        for (int index = 0; index < gInventorySlotsCount && index + _btable_offset < inventory->length; index++) {
            InventoryItem* inventoryItem = &(inventory->items[inventory->length - (index + _btable_offset + 1)]);
            int inventoryFid = itemGetInventoryFid(inventoryItem->item);
            artRender(inventoryFid, dest, INVENTORY_SLOT_WIDTH_PAD, INVENTORY_SLOT_HEIGHT_PAD, INVENTORY_TRADE_WINDOW_WIDTH);
            _display_inventory_info(inventoryItem->item, inventoryItem->quantity, dest, INVENTORY_TRADE_WINDOW_WIDTH, index == draggedSlotIndex);

            dest += INVENTORY_TRADE_WINDOW_WIDTH * INVENTORY_SLOT_HEIGHT;
        }

        if (gGameDialogSpeakerIsPartyMember) {
            MessageListItem messageListItem;
            messageListItem.num = 30;

            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                int weight = clientBarterActive() ? clientBarterAskingValue()
                                                  : barterComputeValue(gDude, _target_stack[0], _btable, _barter_mod, gGameDialogSpeakerIsPartyMember);
                snprintf(formattedText, sizeof(formattedText), "%s %d", messageListItem.text, weight);
            }
        } else {
            // Viewer: the asking value is server-computed and streamed (see the
            // offer-value note above); barterComputeValue here would read the local
            // actor's own skill/reaction and show a wrong price.
            int cost = clientBarterActive() ? clientBarterAskingValue()
                                            : barterComputeValue(gDude, _target_stack[0], _btable, _barter_mod, gGameDialogSpeakerIsPartyMember);
            snprintf(formattedText, sizeof(formattedText), "$%d", cost);
        }

        fontDrawText(windowBuffer + INVENTORY_TRADE_WINDOW_WIDTH * (INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD) + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD, formattedText, 80, INVENTORY_TRADE_WINDOW_WIDTH, _colorTable[32767]);

        Rect rect;
        rect.left = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD;
        rect.top = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_Y_PAD;
        // NOTE: Odd math, likely should be `INVENTORY_SLOT_WIDTH_PAD`.
        rect.right = INVENTORY_TRADE_INNER_RIGHT_SCROLLER_X_PAD + INVENTORY_SLOT_WIDTH;
        rect.bottom = rect.top + rectHeight;
        windowRefreshRect(gInventoryWindow, &rect);
    }

    fontSetCurrent(oldFont);
}

// 0x4757F0
// Server barter: resolve a proto id to a live item Object* in `owner`'s
// top-level inventory. The dude, the merchant, and the two barter tables are
// all flat inventories here, so no nested-container search is needed. Returns
// the first matching item, or nullptr if absent. Used by the serverLoopActive()
// drain in inventoryOpenTrade to reference items by pid rather than by the
// window's fragile reverse-indexed slot math.
static Object* _barter_find_item_by_pid(Object* owner, int pid)
{
    Inventory* inventory = &(owner->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        if (inventory->items[index].item->pid == pid) {
            return inventory->items[index].item;
        }
    }
    return nullptr;
}

// Installed by f2_server (inventory.h). Null everywhere else, which is what
// leaves the golden fixtures on break-on-empty.
static std::function<bool()> gBarterServerPump;

// Snapshot one table's top-level stacks into the presenter's row form. Top-level
// only, matching every other inventory the wire carries (nested container
// contents are the standing "A2" gap); a barter table never nests in practice.
static int barterSnapshotTable(Object* table, Presenter::BarterStack* out, int cap)
{
    if (table == nullptr) {
        return 0;
    }
    Inventory* inv = &(table->data.inventory);
    int count = 0;
    for (int i = 0; i < inv->length && count < cap; i++) {
        if (inv->items[i].item == nullptr) {
            continue;
        }
        out[count].pid = inv->items[i].item->pid;
        out[count].quantity = inv->items[i].quantity;
        count++;
    }
    return count;
}

// Ship what the trade looks like right now. Called after every accepted move, and
// once at open so a viewer starts from the truth rather than an empty guess.
//
// The two valuations are computed HERE, server-side, because they cannot be
// derived viewer-side (see the presenter.h note): barterComputeValue reads the
// party's best barter skill, a gDude-gated Master Trader check, and a barterMod
// carrying the script-set reaction.
static void barterEmitState(Object* barterer, Object* playerTable, Object* bartererTable,
    int barterMod, bool speakerIsPartyMember, int resultCode = -1)
{
    constexpr int kMaxRows = 64;
    Presenter::BarterStack playerRows[kMaxRows];
    Presenter::BarterStack merchantRows[kMaxRows];

    Presenter::BarterStack driverRows[kMaxRows];
    Presenter::BarterStack merchantInvRows[kMaxRows];

    Presenter::BarterView view;
    // All four, because the tick is parked for the trade's whole life: no mirror
    // on any viewer will update on its own, so anything not in here is frozen at
    // whatever it held when the trade opened.
    view.driverInvCount = barterSnapshotTable(_inven_dude, driverRows, kMaxRows);
    view.driverInv = driverRows;
    view.merchantInvCount = barterSnapshotTable(barterer, merchantInvRows, kMaxRows);
    view.merchantInv = merchantInvRows;
    view.playerTableCount = barterSnapshotTable(playerTable, playerRows, kMaxRows);
    view.playerTable = playerRows;
    view.merchantTableCount = barterSnapshotTable(bartererTable, merchantRows, kMaxRows);
    view.merchantTable = merchantRows;

    // offerValue = what the player is putting up; askingValue = what the merchant
    // wants for the goods on their table. The player's side is valued WITHOUT the
    // barter modifier: the modifier prices the MERCHANT's goods, and vanilla's
    // offer side is the raw worth of what you hand over.
    view.offerValue = objectGetCost(playerTable);
    view.askingValue = barterComputeValue(_inven_dude, barterer, bartererTable,
        barterMod, speakerIsPartyMember);
    view.resultCode = resultCode;

    presenter()->barterState(view);
}

void barterSetServerPump(std::function<bool()> pump)
{
    gBarterServerPump = std::move(pump);
}

void inventoryOpenTrade(int win, Object* barterer, Object* playerTable, Object* bartererTable, int barterMod)
{
    ScopedGameMode gm(GameMode::kBarter);

    _barter_mod = barterMod;

    if (inventoryCommonInit() == -1) {
        return;
    }

    // SIM setup (runs on both paths): strip the merchant's worn armor and
    // held weapon so they aren't offered as barter goods; the teardown re-adds
    // them. This mutates the merchant's inventory, so it must run headless too.
    Object* armor = critterGetArmor(barterer);
    if (armor != nullptr) {
        itemRemove(barterer, armor, 1);
    }

    Object* item1 = nullptr;
    Object* item2 = critterGetItem2(barterer);
    if (item2 != nullptr) {
        itemRemove(barterer, item2, 1);
    } else {
        if (!gGameDialogSpeakerIsPartyMember) {
            item1 = _inven_find_type(barterer, ITEM_TYPE_WEAPON, nullptr);
            if (item1 != nullptr) {
                itemRemove(barterer, item1, 1);
            }
        }
    }

    Object* hiddenBox = nullptr;
    if (objectCreateWithFidPid(&hiddenBox, 0, PROTO_ID_JESSE_CONTAINER) == -1) {
        return;
    }

    bool isoWasEnabled = false;
    if (serverLoopActive()) {
        // Headless: skip the trade window, its slot buttons, and all rendering.
        // The UI setup is NOT headless-safe — _setup_inventory() calls exit(1)
        // when _barter_back_win (== win == the -1 headless dialog window) is -1.
        // The serverLoopActive() drain below references items by pid and mutates
        // via itemMoveForce / barterAttemptTransaction, needing no window or the
        // display-move-helper slot statics (_btable/_ptable/_target_* etc.). It
        // does need _inven_dude — barter is always the player trading — and
        // _pud for parity with the interactive COMMIT path.
        _inven_dude = gDude;
        _pud = &(_inven_dude->data.inventory);

        // ►► DIAGNOSTIC (F2_TRACE_BARTER): whose inventory does this trade mutate?
        // The server barter drain moves items OUT OF _inven_dude on every offer, so
        // if _inven_dude is not the player who is actually driving the trade screen,
        // the wrong player's real inventory is emptied (owner-reported: P1's items
        // vanished when P2 traded). This line pins _inven_dude / gDude against the
        // slot roster so a live run says outright whether the anchor is the driver
        // or a bystander. Trace-only; no behavior change.
        if (getenv("F2_TRACE_BARTER") != nullptr) {
            fprintf(stderr,
                "[barter-trace] OPEN inven_dude=net%d(%s,items=%d) gDude=net%d barterer=net%d | slot0=net%d slot1=net%d slot2=net%d\n",
                _inven_dude != nullptr ? _inven_dude->netId : -1,
                _inven_dude != nullptr ? critterGetName(_inven_dude) : "?",
                _inven_dude != nullptr ? _inven_dude->data.inventory.length : -1,
                gDude != nullptr ? gDude->netId : -1,
                barterer != nullptr ? barterer->netId : -1,
                playerActorAt(0) != nullptr ? playerActorAt(0)->netId : -1,
                playerActorCount() > 1 && playerActorAt(1) != nullptr ? playerActorAt(1)->netId : -1,
                playerActorCount() > 2 && playerActorAt(2) != nullptr ? playerActorAt(2)->netId : -1);
        }
    } else {
        _pud = &(_inven_dude->data.inventory);
        _btable = bartererTable;
        _ptable = playerTable;

        _ptable_offset = 0;
        _btable_offset = 0;

        _ptable_pud = &(playerTable->data.inventory);
        _btable_pud = &(bartererTable->data.inventory);

        _barter_back_win = win;
        _target_curr_stack = 0;
        _target_pud = &(barterer->data.inventory);

        _target_stack[0] = barterer;
        _target_stack_offset[0] = 0;

        isoWasEnabled = _setup_inventory(INVENTORY_WINDOW_TYPE_TRADE);
        _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
        _display_inventory(_stack_offset[0], -1, INVENTORY_WINDOW_TYPE_TRADE);
        _display_body(barterer->fid, INVENTORY_WINDOW_TYPE_TRADE);
        windowRefresh(_barter_back_win);
        inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);

        inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
    }

    int modifier = barterReactionModifier(barterer);

    // Announce the trade before the first barrier wait, so every viewer has a
    // window open (driver editable, spectators read-only) and an accurate opening
    // snapshot rather than an empty guess. Only on the server path: off it, the
    // presenter is the local one and the real UI above is already on screen.
    if (serverLoopActive()) {
        // ►► EMITTED BEFORE barterBegin ON PURPOSE. consoleMessageStyled only
        // BUFFERS; the flush comes from beatEnd, and the tick is about to park in
        // the barter barrier for the whole trade — so a message queued after this
        // point sits in the buffer until the trade ENDS, which is exactly when it
        // stops being useful. barterBegin force-flushes, so anything emitted just
        // before it rides out with it.
        //
        // ►► TELL THE OTHER PLAYERS WHAT IS HAPPENING TO THEM. A trade freezes
        // the whole world (the tick parks in the barter barrier), so from any
        // other player's seat the game simply STOPS -- and an unexplained freeze
        // is indistinguishable from a crash, which is the bugs-list U principle
        // applied to a wait instead of a refusal. One line turns "it hung" into
        // "someone is shopping".
        //
        // Addressed per-actor rather than broadcast, so it can say "not you":
        // the wire has no negative address, so excluding the driver means naming
        // everyone else explicitly.
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* other = playerActorAt(slot);
            if (other == nullptr || other == _inven_dude) {
                continue;
            }
            char line[256];
            snprintf(line, sizeof(line), "%s is trading with %s. Please wait.",
                critterGetName(_inven_dude), objectGetName(barterer));
            presenter()->consoleMessageStyled(other->netId, kMsgChannelSystem, line);
        }

        presenter()->barterBegin(barterer, _inven_dude);
        barterEmitState(barterer, playerTable, bartererTable,
            barterMod + modifier, gGameDialogSpeakerIsPartyMember);
    }

    int keyCode = -1;
    for (;;) {
        if (serverLoopActive()) {
            // Headless: no mouse/keyboard/render/fps loop. Drain the barter
            // intent queue, moving items with itemMoveForce (bypassing the
            // display move-helpers, which reverse-index live window slots) and
            // committing via the same barterAttemptTransaction the 'M' button
            // drives below. An empty queue ends the loop — we cannot block for
            // input (same policy as the server dialog/combat drains).
            _barter_mod = barterMod + modifier;

            BarterIntent intent;
            bool haveIntent = barterIntentPeek(&intent);
            if (gBarterServerPump != nullptr) {
                // Block-and-pump, the barter twin of the dialog barrier
                // (game_dialog.cc _gdProcess): park the server tick inside the
                // open trade and service the control channel until the driver's
                // next move lands, or the pump bails (driver gone / combat /
                // quit). Vanilla freezes the world during barter anyway, so a
                // trade is an accepted input barrier exactly like a dialog node.
                while (!haveIntent) {
                    if (!gBarterServerPump()) {
                        break;
                    }
                    haveIntent = barterIntentPeek(&intent);
                }
            }
            if (!haveIntent) {
                if (gBarterServerPump != nullptr) {
                    // ►► PUMP BAIL — SWEEP THE TABLES OR THE PLAYER LOSES THEM.
                    // Teardown ends at _gdialog_barter_destroy_win, which calls
                    // objectDestroy on both tables WITH THEIR CONTENTS. 'T'/DONE
                    // sweeps first; ESC/CANCEL deliberately does not. A bail is
                    // neither — it is the driver vanishing mid-trade (disconnect,
                    // quit) with goods still on the table, and destroying those is
                    // never the right answer to a dropped connection. So a bail
                    // exits like DONE, not like CANCEL.
                    itemMoveAll(bartererTable, barterer);
                    itemMoveAll(playerTable, gDude);
                    presenter()->barterEnd(); // close every viewer's window too
                }
                // No pump: the ORIGINAL headless behavior, and the one every
                // golden depends on — a fixture pre-queues its moves plus a
                // terminating bdone, so a dry queue means the fixture is done.
                break;
            }
            barterIntentPop();

            if (intent.kind == BARTER_INTENT_DONE) {
                // == the 'T'/Talk button (lines ~4332-4336): return both tables
                // to their owners, then leave barter to resume/close the dialog.
                itemMoveAll(bartererTable, barterer);
                itemMoveAll(playerTable, gDude);
                _barter_end_to_talk_to();
                presenter()->barterEnd();
                break;
            }

            if (intent.kind == BARTER_INTENT_CANCEL) {
                presenter()->barterEnd();
                // == ESC: leave without a table sweep; the teardown below
                // restores the merchant's worn/held items and the hidden box.
                break;
            }

            if (intent.kind == BARTER_INTENT_COMMIT) {
                // == the 'M'/Offer button (lines ~4337-4373), sim half only. The
                // bad-offer/too-heavy message TEXT rendering is UI and dropped, but
                // the RESULT CODE now rides the state emit so a viewer can confirm or
                // deny the Offer button (see barterEmitState's resultCode).
                BarterResult rc = BARTER_RESULT_OK;
                if (playerTable->data.inventory.length != 0 || bartererTable->data.inventory.length != 0) {
                    rc = barterAttemptTransaction(_inven_dude, playerTable, barterer, bartererTable, _barter_mod, gGameDialogSpeakerIsPartyMember);
                    if (getenv("F2_BARTER_TRACE") != nullptr) {
                        debugPrint("[btrace] commit rc=%d ptable_len=%d btable_len=%d mod=%d\n",
                            rc, playerTable->data.inventory.length, bartererTable->data.inventory.length, _barter_mod);
                    }
                }
                // A commit that SUCCEEDS empties both tables; one that is refused
                // (bad offer / too heavy) leaves them exactly as they were. Re-send
                // either way — "nothing moved" is the answer to a refused offer, and
                // a viewer that only heard about successes could never show it.
                barterEmitState(barterer, playerTable, bartererTable,
                    _barter_mod, gGameDialogSpeakerIsPartyMember, (int)rc);
                continue;
            }

            // OFFER/TAKE/UNOFFER: move `quantity` of `pid` between an owner and
            // a table. Mirrors the LMB dude/merchant/table slot handlers.
            Object* item = nullptr;
            Object* from = nullptr;
            Object* to = nullptr;
            switch (intent.kind) {
            case BARTER_INTENT_OFFER_ITEM:
                from = _inven_dude;
                to = playerTable;
                item = _barter_find_item_by_pid(_inven_dude, intent.pid);
                break;
            case BARTER_INTENT_TAKE_ITEM:
                from = barterer;
                to = bartererTable;
                item = _barter_find_item_by_pid(barterer, intent.pid);
                break;
            case BARTER_INTENT_UNOFFER_ITEM:
                item = _barter_find_item_by_pid(playerTable, intent.pid);
                if (item != nullptr) {
                    from = playerTable;
                    to = _inven_dude;
                } else {
                    item = _barter_find_item_by_pid(bartererTable, intent.pid);
                    from = bartererTable;
                    to = barterer;
                }
                break;
            }

            if (item != nullptr) {
                int available = itemGetQuantity(from, item);
                int quantity = intent.quantity;
                if (quantity <= 0 || quantity > available) {
                    quantity = available;
                }
                itemMoveForce(from, to, item, quantity);
                if (getenv("F2_BARTER_TRACE") != nullptr) {
                    debugPrint("[btrace] move kind=%d pid=%d qty=%d/%d\n",
                        intent.kind, intent.pid, quantity, available);
                }
            } else if (getenv("F2_BARTER_TRACE") != nullptr) {
                debugPrint("[btrace] move kind=%d pid=%d NOT FOUND\n", intent.kind, intent.pid);
            }

            // The owner's spec, literally: no animation, no travel — the item is
            // simply on the table now, on every screen at once.
            barterEmitState(barterer, playerTable, bartererTable,
                _barter_mod, gGameDialogSpeakerIsPartyMember);
            continue;
        }

        sharedFpsLimiter.mark();

        if (keyCode == KEY_ESCAPE || _game_user_wants_to_quit != 0) {
            break;
        }

        keyCode = inputGetInput();
        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        _barter_mod = barterMod + modifier;

        if (keyCode == KEY_LOWERCASE_T || modifier <= -30) {
            itemMoveAll(bartererTable, barterer);
            itemMoveAll(playerTable, gDude);
            _barter_end_to_talk_to();
            break;
        } else if (keyCode == KEY_LOWERCASE_M) {
            if (playerTable->data.inventory.length != 0 || _btable->data.inventory.length != 0) {
                BarterResult rc = barterAttemptTransaction(_inven_dude, playerTable, barterer, bartererTable, _barter_mod, gGameDialogSpeakerIsPartyMember);
                if (rc == BARTER_RESULT_OK) {
                    _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                    inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);

                    // Ok, that's a good trade.
                    MessageListItem messageListItem;
                    messageListItem.num = 27;
                    if (!gGameDialogSpeakerIsPartyMember) {
                        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                            gameDialogRenderSupplementaryMessage(messageListItem.text);
                        }
                    }
                } else {
                    MessageListItem messageListItem;
                    // 31: Sorry, you cannot carry that much.
                    // 32: Sorry, that's too much to carry.
                    // 28: No, your offer is not good enough.
                    switch (rc) {
                    case BARTER_RESULT_TOO_HEAVY:
                        messageListItem.num = 31;
                        break;
                    case BARTER_RESULT_NPC_TOO_HEAVY:
                        messageListItem.num = 32;
                        break;
                    default:
                        messageListItem.num = 28;
                        break;
                    }
                    if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                        gameDialogRenderSupplementaryMessage(messageListItem.text);
                    }
                }
            }
        } else if (keyCode == KEY_ARROW_UP) {
            if (_stack_offset[_curr_stack] > 0) {
                _stack_offset[_curr_stack] -= 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
            }
        } else if (keyCode == KEY_PAGE_UP) {
            if (_ptable_offset > 0) {
                _ptable_offset -= 1;
                inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
            }
        } else if (keyCode == KEY_ARROW_DOWN) {
            if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                _stack_offset[_curr_stack] += 1;
                _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
            }
        } else if (keyCode == KEY_PAGE_DOWN) {
            if (_ptable_offset + gInventorySlotsCount < _ptable_pud->length) {
                _ptable_offset += 1;
                inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
            }
        } else if (keyCode == KEY_CTRL_PAGE_DOWN) {
            if (_btable_offset + gInventorySlotsCount < _btable_pud->length) {
                _btable_offset++;
                inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
            }
        } else if (keyCode == KEY_CTRL_PAGE_UP) {
            if (_btable_offset > 0) {
                _btable_offset -= 1;
                inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
            }
        } else if (keyCode == KEY_CTRL_ARROW_UP) {
            if (_target_stack_offset[_target_curr_stack] > 0) {
                _target_stack_offset[_target_curr_stack] -= 1;
                _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                windowRefresh(gInventoryWindow);
            }
        } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
            if (_target_stack_offset[_target_curr_stack] + gInventorySlotsCount < _target_pud->length) {
                _target_stack_offset[_target_curr_stack] += 1;
                _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                windowRefresh(gInventoryWindow);
            }
        } else if (keyCode >= 2500 && keyCode <= 2501) {
            _container_exit(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
        } else {
            if ((mouseGetEvent() & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_HAND) {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);
                } else {
                    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if (keyCode >= 1000 && keyCode <= 1000 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        inventoryWindowRenderInnerInventories(win, playerTable, nullptr, -1);
                    } else {
                        int slotIndex = keyCode - 1000;
                        if (slotIndex + _stack_offset[_curr_stack] < _pud->length) {
                            int offset = _stack_offset[_curr_stack];
                            InventoryItem* inventoryItem = &(_pud->items[_pud->length - (slotIndex + offset + 1)]);
                            _barter_move_inventory(inventoryItem->item, inventoryItem->quantity, slotIndex, offset, barterer, playerTable, true);
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            inventoryWindowRenderInnerInventories(win, playerTable, nullptr, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2000 && keyCode <= 2000 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        inventoryWindowRenderInnerInventories(win, nullptr, bartererTable, -1);
                    } else {
                        int slotIndex = keyCode - 2000;
                        if (slotIndex + _target_stack_offset[_target_curr_stack] < _target_pud->length) {
                            int stackOffset = _target_stack_offset[_target_curr_stack];
                            InventoryItem* inventoryItem = &(_target_pud->items[_target_pud->length - (slotIndex + stackOffset + 1)]);
                            _barter_move_inventory(inventoryItem->item, inventoryItem->quantity, slotIndex, stackOffset, barterer, bartererTable, false);
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            inventoryWindowRenderInnerInventories(win, nullptr, bartererTable, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2300 && keyCode <= 2300 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        inventoryWindowRenderInnerInventories(win, playerTable, nullptr, -1);
                    } else {
                        int slotIndex = keyCode - 2300;
                        if (slotIndex < _ptable_pud->length) {
                            InventoryItem* inventoryItem = &(_ptable_pud->items[_ptable_pud->length - (slotIndex + _ptable_offset + 1)]);
                            _barter_move_from_table_inventory(inventoryItem->item, inventoryItem->quantity, slotIndex, barterer, playerTable, true);
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            inventoryWindowRenderInnerInventories(win, playerTable, nullptr, -1);
                        }
                    }

                    keyCode = -1;
                } else if (keyCode >= 2400 && keyCode <= 2400 + gInventorySlotsCount) {
                    if (gInventoryCursor == INVENTORY_WINDOW_CURSOR_ARROW) {
                        inventoryWindowOpenContextMenu(keyCode, INVENTORY_WINDOW_TYPE_TRADE);
                        inventoryWindowRenderInnerInventories(win, nullptr, bartererTable, -1);
                    } else {
                        int slotIndex = keyCode - 2400;
                        if (slotIndex < _btable_pud->length) {
                            InventoryItem* inventoryItem = &(_btable_pud->items[_btable_pud->length - (slotIndex + _btable_offset + 1)]);
                            _barter_move_from_table_inventory(inventoryItem->item, inventoryItem->quantity, slotIndex, barterer, bartererTable, false);
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                            inventoryWindowRenderInnerInventories(win, nullptr, bartererTable, -1);
                        }
                    }

                    keyCode = -1;
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
                if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_LEFT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_stack_offset[_curr_stack] > 0) {
                            _stack_offset[_curr_stack] -= 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                        }
                    } else if (wheelY < 0) {
                        if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                            _stack_offset[_curr_stack] += 1;
                            _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                        }
                    }
                } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_LEFT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_ptable_offset > 0) {
                            _ptable_offset -= 1;
                            inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
                        }
                    } else if (wheelY < 0) {
                        if (_ptable_offset + gInventorySlotsCount < _ptable_pud->length) {
                            _ptable_offset += 1;
                            inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
                        }
                    }
                } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_RIGHT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_target_stack_offset[_target_curr_stack] > 0) {
                            _target_stack_offset[_target_curr_stack] -= 1;
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            windowRefresh(gInventoryWindow);
                        }
                    } else if (wheelY < 0) {
                        if (_target_stack_offset[_target_curr_stack] + gInventorySlotsCount < _target_pud->length) {
                            _target_stack_offset[_target_curr_stack] += 1;
                            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                            windowRefresh(gInventoryWindow);
                        }
                    }
                } else if (mouseHitTestInWindow(gInventoryWindow, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_X, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y, INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_MAX_X, INVENTORY_SLOT_HEIGHT * gInventorySlotsCount + INVENTORY_TRADE_INNER_RIGHT_SCROLLER_TRACKING_Y)) {
                    int wheelX;
                    int wheelY;
                    mouseGetWheel(&wheelX, &wheelY);
                    if (wheelY > 0) {
                        if (_btable_offset > 0) {
                            _btable_offset -= 1;
                            inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
                        }
                    } else if (wheelY < 0) {
                        if (_btable_offset + gInventorySlotsCount < _btable_pud->length) {
                            _btable_offset++;
                            inventoryWindowRenderInnerInventories(win, playerTable, bartererTable, -1);
                        }
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    itemMoveAll(hiddenBox, barterer);
    objectDestroy(hiddenBox, nullptr);

    if (armor != nullptr) {
        armor->flags |= OBJECT_WORN;
        itemAdd(barterer, armor, 1);
    }

    if (item2 != nullptr) {
        item2->flags |= OBJECT_IN_RIGHT_HAND;
        itemAdd(barterer, item2, 1);
    }

    if (item1 != nullptr) {
        itemAdd(barterer, item1, 1);
    }

    // Headless never created the trade window or disabled iso (see the setup
    // guard above), so skip _exit_inventory (window destroy + iso re-enable).
    if (!serverLoopActive()) {
        _exit_inventory(isoWasEnabled);
    }

    // NOTE: Uninline.
    inventoryCommonFree();
}

// ============================================================================
// VIEWER-SIDE TRADE WINDOW (client_barter.h)
//
// The viewer never runs inventoryOpenTrade above: that function IS the trade, and
// the trade belongs to the server. This is the render half only — the same window
// and the same display calls, driven by the mirrored tables the barter stream
// builds, and exited when the server says the trade is over.
//
// A no-op item-description sink for the viewer's nested quantity dial. During
// barter, _setup_inventory(TRADE) points gInventoryPrintItemDescriptionHandler at
// gameDialogRenderSupplementaryMessage, which draws into the dialog reply window.
// That window is ALIVE on a normal client but TORN DOWN on a viewer (the node
// subwindows come down when the trade screen goes up). inventoryQuantityWindowInit
// ends with inventorySetCursor(ARROW), which -- because the clicked slot is still
// armed (_im_value != -1) -- fires inventoryItemSlotOnMouseEnter ->
// _obj_look_at_func(..., gInventoryPrintItemDescriptionHandler), rendering the
// item description into that dead window and faulting in the font blit. Swapping
// the handler to this sink for the dial's lifetime suppresses the render; the
// description was never visible on the viewer anyway.
static void inventoryTradeViewerDescriptionSink(char* /*string*/)
{
}

// Same shape as client_dialog: bypass the loop that would execute authority
// locally, call the pure-render pieces directly.
//
// ►► DRIVER TRADES VIA THE b* VERBS; SPECTATORS ARE READ-ONLY. Vanilla's slot
// handlers mutate inventories in place, which on a mirror would invent items the
// server does not have, so the driver's clicks are rerouted to the b* verbs (see
// the modal loop below), including the partial-quantity dial. Spectators send
// nothing.
void inventoryOpenTradeViewer(Object* merchant, Object* playerTable, Object* merchantTable)
{
    if (merchant == nullptr || playerTable == nullptr || merchantTable == nullptr) {
        return;
    }

    // ►► REQUIRE A LIVE DIALOG SESSION WINDOW. The trade screen is built on top of
    // the dialog background window (it is the backing window below, and
    // _gdialog_barter_create_win blits from that window's buffer). A viewer that
    // dropped its dialog out of sync with the server — e.g. a spectator that closed
    // it locally — has no such window, and building the trade screen on it faults.
    // Refuse instead: barter reopens on the next EVENT_BARTER_STATE once the session
    // is back, and a missing screen is recoverable where a fault is not.
    if (!gameDialogBackgroundActive()) {
        fprintf(stderr, "client_barter: no dialog session window — refusing the trade screen\n");
        return;
    }

    // ►► SUBJECT SLOTS MUST BE PROTO-RESOLVABLE. Everything the screen puts in a
    // subject slot (_inven_dude, _stack[], _target_stack[]) goes through proto /
    // stat / art resolution at dozens of sites, and protoGetProto on a bad pid
    // hands back a null the caller dereferences — that is the segfault this
    // screen already shipped once. Refuse to open instead: a missing screen is
    // recoverable and reportable, a fault takes the whole viewer down.
    Object* driver = clientBarterDriver();
    Proto* probe = nullptr;
    if (driver == nullptr || protoGetProto(driver->pid, &probe) != 0
        || protoGetProto(merchant->pid, &probe) != 0) {
        fprintf(stderr, "client_barter: subject slot not proto-resolvable — refusing the trade screen\n");
        return;
    }

    ScopedGameMode gm(GameMode::kBarter);

    if (inventoryCommonInit() == -1) {
        return;
    }

    // ►► BUILD THE BARTER BACKGROUND FIRST. game_dialog.cc calls
    // _gdialog_barter_create_win() immediately before inventoryOpenTrade, and
    // skipping it here was a real bug: without it there is no barter.frm behind
    // the screen, so the DIALOG skeleton shows through and the trade chrome
    // (money, the Offer/Talk buttons) draws on top of it, with an unloaded FRM in
    // the corner. The window below is the dialog window on purpose -- that is what
    // vanilla passes as `win` -- but it is the BACKING window, not the background.
    // ►► TWO SUBWINDOWS MUST COME DOWN BEFORE THE TRADE WINDOW GOES UP.
    // (1) The reply/option subwindows: barter REPLACES the option list; leaving them
    // lit was the reported "dialog leftovers blended with the new background".
    // clientBarterActive() is already true here, so this sync reconciles
    // node-subwindows-wanted to false and takes them down — the single owner does it.
    // (2) The NORMAL control subwindow (Barter/Review buttons): gGameDialogWindow is
    // one handle, and _gdialog_barter_create_win below OVERWRITES it. Without this
    // teardown the normal control window leaks and its Barter button stays live over
    // the trade screen (the reported leftover-button), and on exit the flavor bookkeeping
    // is wrong. Destroy it, then build the barter flavor into the same handle. The
    // dialog SESSION (background/head) window stays up — it is what gameDialogGetWindow()
    // returns below as the trade window's backing.
    clientModalWindowsSync();
    gameDialogExitControlWindow();
    gameDialogInitBarterWindows();

    int win = gameDialogGetWindow();

    // ►► RENDER THE DRIVER'S PACK, NOT THE LOCAL PLAYER'S. On a spectator's screen
    // the left panel is whoever is TRADING; showing your own inventory there was
    // the reported "P2 can't see P1's inventory". And even for the driver the real
    // object is the wrong source: the tick is parked for the whole trade, so no
    // inventory delta arrives and the mirror is frozen at open — items moved onto
    // a table would never leave the pack on screen. The snapshot is the only
    // accurate source while a trade runs, for everyone.
    // ►► TWO DIFFERENT THINGS, AND CONFLATING THEM SEGFAULTED. _inven_dude must be
    // a REAL CRITTER: the inventory UI reads it for stats, armour, encumbrance and
    // body art at 60+ sites, so a bare snapshot container (pid -1, no proto) faults
    // the first time one of those resolves a proto. Only the rendered item LIST
    // comes from the snapshot — which is still essential, because the tick is
    // parked and the real mirror is frozen for the whole trade.
    //
    // ►► SAVE THE LOCAL INVENTORY ANCHOR AND RESTORE IT ON EVERY EXIT. _inven_dude /
    // _stack[0] are the PERSISTENT anchor the plain inventory screen reads
    // (inventoryOpen does NOT re-derive them — _setup_inventory seeds _inven_dude
    // FROM _stack[0]). Pointing them at the trade DRIVER here and walking away left
    // a spectator's own inventory screen anchored to the driver afterwards: the
    // driver had no items, so the spectator's pack rendered EMPTY and stayed empty
    // (owner-reported "inventory vanished, doesn't recover"). Restore the local
    // actor on the way out. [[coop-mp-track]] cached-gDude-anchor bug class.
    Object* savedInvenDude = _inven_dude;
    Object* savedStack0 = _stack[0];
    _inven_dude = clientBarterDriver();
    _pud = &(clientBarterDriverInv()->data.inventory);
    _btable = merchantTable;
    _ptable = playerTable;
    _ptable_offset = 0;
    _btable_offset = 0;
    _ptable_pud = &(playerTable->data.inventory);
    _btable_pud = &(merchantTable->data.inventory);
    _barter_back_win = win;
    _target_curr_stack = 0;
    _target_pud = &(clientBarterMerchantInv()->data.inventory); // list: snapshot
    _target_stack[0] = merchant;                                 // subject: real critter
    _target_stack_offset[0] = 0;

    // ►► RECOMPUTE THE BODY FID FOR THE DRIVER. gInventoryWindowDudeFid is set by
    // _adjust_fid inside inventoryCommonInit ABOVE, while _inven_dude still held
    // whatever the last screen left it -- so _display_body would blit a stale body
    // to the top-left panel (the reported unexplained sprite there). Now that
    // _inven_dude is the driver, recompute it before any display call reads it.
    _adjust_fid();

    bool isoWasEnabled = _setup_inventory(INVENTORY_WINDOW_TYPE_TRADE);

    // ►► RE-SEED AFTER _setup_inventory, AND CHECK IT TOOK. _setup_inventory
    // unconditionally re-derives the list slots from the subject slots
    // (`_pud = &(_inven_dude->data.inventory); _stack[0] = _inven_dude;` above),
    // so seeding them before this call silently accomplishes nothing — the panel
    // then renders the FROZEN real mirror instead of the snapshot, which is
    // invisible until something actually moves. That is a general hazard of
    // driving vanilla screens from mirrors, not a one-off, hence the loud check:
    // NDEBUG compiles assert() away, so this has to be a real branch.
    _pud = &(clientBarterDriverInv()->data.inventory);
    _target_pud = &(clientBarterMerchantInv()->data.inventory);
    if (_pud != &(clientBarterDriverInv()->data.inventory)
        || _target_pud != &(clientBarterMerchantInv()->data.inventory)) {
        fprintf(stderr, "client_barter: list slots did not take — refusing the trade screen\n");
        _exit_inventory(isoWasEnabled);
        gameDialogExitBarterWindows();
        // Restore the normal control window we tore down on entry (same reason as the
        // normal-exit path below) so a refused open leaves a valid dialog window.
        if (clientDialogActive()) {
            gameDialogInitControlWindow();
        }
        _inven_dude = savedInvenDude; // restore the local anchor (see save site)
        _stack[0] = savedStack0;
        inventoryCommonFree();
        return;
    }

    inventorySetCursor(INVENTORY_WINDOW_CURSOR_HAND);

    // ►► SUPPRESS THE ITEM-DESCRIPTION RENDER FOR THE WHOLE VIEWER TRADE, not just
    // the quantity dial. _setup_inventory(TRADE) above pointed
    // gInventoryPrintItemDescriptionHandler at gameDialogRenderSupplementaryMessage,
    // which draws into the dialog reply window -- ALIVE on a normal client but TORN
    // DOWN on a viewer. Anything that renders an item description here faults in the
    // font blit: an ARROW-cursor hover over a slot (_process_bk -> _GNW_check_buttons
    // -> inventoryItemSlotOnMouseEnter -> _obj_look_at_func) OR the quantity dial's
    // closing cursor reset. The trade window keeps a HAND cursor so hovers are
    // normally inert, but the dial leaves the cursor ARROW on exit and the next hover
    // then fires the render -- the reported "cancel the dial, then segfault". The
    // viewer never shows this text anyway, so route the handler to a sink for the
    // trade's lifetime and restore it on the way out (all exits are below this point).
    InventoryPrintItemDescriptionHandler* savedDescHandler = gInventoryPrintItemDescriptionHandler;
    gInventoryPrintItemDescriptionHandler = inventoryTradeViewerDescriptionSink;

    // Repaint everything the stream can change. Called on open and on every
    // STATE, which is the whole visual cue: no animation, no travel, the item is
    // simply on the table now.
    auto repaint = [&]() {
        _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
        _display_inventory(_stack_offset[0], -1, INVENTORY_WINDOW_TYPE_TRADE);
        _display_body(merchant->fid, INVENTORY_WINDOW_TYPE_TRADE);
        windowRefresh(_barter_back_win);
        inventoryWindowRenderInnerInventories(win, _ptable, _btable, -1);
    };
    clientBarterApplyPending(); // fold in any snapshot latched before the window opened
    repaint();

    // The modal loop. GameMode::kBarter must be in kViewerModalMask or the wire
    // STALLS here — this loop starves conn.pump() exactly like every other
    // vanilla modal, and the service ticker is what keeps it fed.
    for (;;) {
        sharedFpsLimiter.mark();

        // The SERVER owns the trade's lifetime. When it says the trade is over,
        // this window goes, whoever you are — a spectator cannot dismiss someone
        // else's trade early, and the driver's own exit comes back around the
        // same way (verb -> server -> EVENT_BARTER_END).
        if (!clientBarterActive()) {
            break;
        }

        int keyCode = inputGetInput();

        // A trade may have ENDED during that pump (onBarterEnd only latches; it
        // does not free the mirrors — clientBarterFinalize does, from the main
        // loop). Break BEFORE any code below dereferences a mirror.
        if (!clientBarterActive()) {
            break;
        }

        // ►► ONLY THE DRIVER MAY ACT. Spectators render the same screen but send
        // nothing: the server refuses them anyway ("This isn't your trade"), and
        // a spectator's ESC must not close a window the server still owns. Every
        // branch below is gated on it, so a spectator falls straight through to
        // the repaint.
        bool draggedThisFrame = false;
        if (clientBarterIsDriver()) {
            if (keyCode == KEY_ESCAPE || keyCode == KEY_LOWERCASE_T) {
                // Leave the trade. A REQUEST, never local: the window closes when
                // the server's EVENT_BARTER_END comes back, so every viewer leaves
                // together and the mirrors are never torn down under a live trade.
                clientViewerBarterVerb("bdone", -1, 0);
            } else if (keyCode == KEY_LOWERCASE_M) {
                clientViewerBarterVerb("bcommit", -1, 0); // the Offer button
            } else if (keyCode == KEY_ARROW_UP) {
                if (_stack_offset[_curr_stack] > 0) {
                    _stack_offset[_curr_stack] -= 1;
                    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                    windowRefresh(gInventoryWindow);
                }
            } else if (keyCode == KEY_ARROW_DOWN) {
                if (_stack_offset[_curr_stack] + gInventorySlotsCount < _pud->length) {
                    _stack_offset[_curr_stack] += 1;
                    _display_inventory(_stack_offset[_curr_stack], -1, INVENTORY_WINDOW_TYPE_TRADE);
                    windowRefresh(gInventoryWindow);
                }
            } else if (keyCode == KEY_CTRL_ARROW_UP) {
                if (_target_stack_offset[_target_curr_stack] > 0) {
                    _target_stack_offset[_target_curr_stack] -= 1;
                    _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                    windowRefresh(gInventoryWindow);
                }
            } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
                if (_target_stack_offset[_target_curr_stack] + gInventorySlotsCount < _target_pud->length) {
                    _target_stack_offset[_target_curr_stack] += 1;
                    _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, INVENTORY_WINDOW_TYPE_TRADE);
                    windowRefresh(gInventoryWindow);
                }
            } else if (keyCode == KEY_PAGE_UP) {
                if (_ptable_offset > 0) {
                    _ptable_offset -= 1;
                    inventoryWindowRenderInnerInventories(win, _ptable, nullptr, -1);
                }
            } else if (keyCode == KEY_PAGE_DOWN) {
                if (_ptable_offset + gInventorySlotsCount < _ptable_pud->length) {
                    _ptable_offset += 1;
                    inventoryWindowRenderInnerInventories(win, _ptable, nullptr, -1);
                }
            } else if (keyCode == KEY_CTRL_PAGE_UP) {
                if (_btable_offset > 0) {
                    _btable_offset -= 1;
                    inventoryWindowRenderInnerInventories(win, nullptr, _btable, -1);
                }
            } else if (keyCode == KEY_CTRL_PAGE_DOWN) {
                if (_btable_offset + gInventorySlotsCount < _btable_pud->length) {
                    _btable_offset += 1;
                    inventoryWindowRenderInnerInventories(win, nullptr, _btable, -1);
                }
            } else if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0
                && gInventoryCursor == INVENTORY_WINDOW_CURSOR_HAND) {
                // ►► REAL DRAG/DROP, VANILLA MACHINERY. A left-click on a slot runs
                // the actual vanilla move helper — pickup sprite, hold-to-drag,
                // drop hit-test, quantity dial — exactly as single-player barter.
                // The ONE difference is the authority boundary: on a driver the
                // helper SENDS THE VERB instead of mutating locally (see the 4
                // itemMoveForce sites, now gated on clientBarterIsDriver()), and the
                // server echoes the move back as EVENT_BARTER_STATE, applied at the
                // top of this loop. Resolving the item is the same reverse-indexed
                // row math vanilla uses; the helper takes it from there.
                //
                // The helpers run inner input loops (drag hold, quantity dial) that
                // pump the wire. That is SAFE now only because the reconcile is
                // deferred: a STATE arriving mid-drag merely latches, so no mirror
                // item is freed while the helper holds a pointer to it. Do not
                // reintroduce an apply at decode time.
                //
                // ►► ALWAYS REPAINT AFTER A DRAG (draggedThisFrame). The helper
                // visually ERASES the picked-up slot at pickup, but the driver does
                // NO local move (the move is a verb, applied only when the server's
                // STATE echoes back). So without an immediate repaint the slot stays
                // blank: if the drop sent a verb, until the echo lands a frame or two
                // later; if it landed on a non-target region (dropped back where it
                // was), FOREVER, until some other action repaints — the reported
                // "item no longer visible, a later click brings it back". Repainting
                // now restores the current mirror view; the echo then repaints the
                // moved result. Vanilla does the same unconditional redraw here.
                draggedThisFrame = true;
                if (keyCode >= 1000 && keyCode <= 1000 + gInventorySlotsCount) {
                    int slotIndex = keyCode - 1000;
                    int offset = _stack_offset[_curr_stack];
                    if (slotIndex + offset < _pud->length) {
                        InventoryItem* it = &(_pud->items[_pud->length - (slotIndex + offset + 1)]);
                        _barter_move_inventory(it->item, it->quantity, slotIndex, offset, merchant, _ptable, true);
                    }
                } else if (keyCode >= 2000 && keyCode <= 2000 + gInventorySlotsCount) {
                    int slotIndex = keyCode - 2000;
                    int offset = _target_stack_offset[_target_curr_stack];
                    if (slotIndex + offset < _target_pud->length) {
                        InventoryItem* it = &(_target_pud->items[_target_pud->length - (slotIndex + offset + 1)]);
                        _barter_move_inventory(it->item, it->quantity, slotIndex, offset, merchant, _btable, false);
                    }
                } else if (keyCode >= 2300 && keyCode <= 2300 + gInventorySlotsCount) {
                    int slotIndex = keyCode - 2300;
                    if (slotIndex + _ptable_offset < _ptable_pud->length) {
                        InventoryItem* it = &(_ptable_pud->items[_ptable_pud->length - (slotIndex + _ptable_offset + 1)]);
                        _barter_move_from_table_inventory(it->item, it->quantity, slotIndex, merchant, _ptable, true);
                    }
                } else if (keyCode >= 2400 && keyCode <= 2400 + gInventorySlotsCount) {
                    int slotIndex = keyCode - 2400;
                    if (slotIndex + _btable_offset < _btable_pud->length) {
                        InventoryItem* it = &(_btable_pud->items[_btable_pud->length - (slotIndex + _btable_offset + 1)]);
                        _barter_move_from_table_inventory(it->item, it->quantity, slotIndex, merchant, _btable, false);
                    }
                }
            }
        }
        if (_game_user_wants_to_quit != 0) {
            break;
        }

        // ►► SAFE POINT: apply any latched snapshot here, with no drag helper or
        // quantity dial on the stack, then repaint. Doing the apply here (not at
        // decode time) is the whole reason the vanilla drag helpers above can pump
        // the wire without a use-after-free. Repaint on a STATE change OR after a
        // drag (see draggedThisFrame — the drag leaves the panels mid-edit).
        clientBarterApplyPending();
        if (draggedThisFrame || clientBarterConsumeDirty()) {
            repaint();
        }

        // ►► COMMIT FEEDBACK. The server answers the Offer button only by moving
        // items (or not); a refused offer looks like "nothing happened". Give the
        // driver an audible confirm/deny for the last commit (the visible why is the
        // offer-vs-asking $ totals, now server-correct). Text feedback wants a
        // visible surface the trade screen does not have free — that lands with the
        // <inspect> pass. Driver-only: a spectator never committed.
        if (clientBarterIsDriver()) {
            int rc = clientBarterConsumeResult();
            if (rc == 0) {
                soundPlayFile("ib1p1xx1"); // BARTER_RESULT_OK — the trade went through
            } else if (rc > 0) {
                soundPlayFile("iisxxxx1"); // refused (bad offer / too heavy)
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    _exit_inventory(isoWasEnabled);
    gameDialogExitBarterWindows();
    // ►► RECREATE THE NORMAL CONTROL SUBWINDOW if the conversation is still live —
    // this is the segfault fix. gameDialogExitBarterWindows just set gGameDialogWindow
    // to -1; the main loop's next node render would dereference that dead handle. The
    // barter flavor was destroyed above; swap the normal flavor back into the same
    // handle (and restore _dialogue_state=1) so the dialog resumes with a valid
    // control window. If the trade's exit ALSO ended the conversation (bdone ->
    // _barter_end_to_talk_to can), leave it down: clientModalWindowsSync() sees the
    // session as over and _gdialogExitFromScript cleans the background + the already
    // -gone control, no orphan windows.
    //
    // ►► NODE SUBWINDOWS ARE NOT REBUILT HERE — the main loop's clientModalWindowsSync()
    // owns those (I2). On the next reconcile clientBarterActive() is false, so it
    // rebuilds them IFF the conversation is still live. The old order-of-arrival guess
    // (BARTER_END vs DIALOG_END) is gone: the reconcile reads the final latched state.
    if (clientDialogActive()) {
        gameDialogInitControlWindow();
    }
    // Restore the local inventory anchor (see the save site above) so the plain
    // inventory screen reads the local actor again, not the trade driver.
    _inven_dude = savedInvenDude;
    _stack[0] = savedStack0;
    // Restore the item-description handler we routed to a sink for the trade (the
    // loop only breaks, never returns, so this is the one exit past that swap).
    gInventoryPrintItemDescriptionHandler = savedDescHandler;
    inventoryCommonFree();
}

// 0x47620C
static void _container_enter(int keyCode, int inventoryWindowType)
{
    if (keyCode >= 2000) {
        int index = _target_pud->length - (_target_stack_offset[_target_curr_stack] + keyCode - 2000 + 1);
        if (index < _target_pud->length && _target_curr_stack < 9) {
            InventoryItem* inventoryItem = &(_target_pud->items[index]);
            Object* item = inventoryItem->item;
            if (itemGetType(item) == ITEM_TYPE_CONTAINER) {
                _target_curr_stack += 1;
                _target_stack[_target_curr_stack] = item;
                _target_stack_offset[_target_curr_stack] = 0;

                _target_pud = &(item->data.inventory);

                _display_body(item->fid, inventoryWindowType);
                _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, inventoryWindowType);
                windowRefresh(gInventoryWindow);
            }
        }
    } else {
        int index = _pud->length - (_stack_offset[_curr_stack] + keyCode - 1000 + 1);
        if (index < _pud->length && _curr_stack < 9) {
            InventoryItem* inventoryItem = &(_pud->items[index]);
            Object* item = inventoryItem->item;
            if (itemGetType(item) == ITEM_TYPE_CONTAINER) {
                _curr_stack += 1;

                _stack[_curr_stack] = item;
                _stack_offset[_curr_stack] = 0;

                _inven_dude = _stack[_curr_stack];
                _pud = &(item->data.inventory);

                _adjust_fid();
                _display_body(-1, inventoryWindowType);
                _display_inventory(_stack_offset[_curr_stack], -1, inventoryWindowType);
            }
        }
    }
}

// 0x476394
static void _container_exit(int keyCode, int inventoryWindowType)
{
    if (keyCode == 2500) {
        if (_curr_stack > 0) {
            _curr_stack -= 1;
            _inven_dude = _stack[_curr_stack];
            _pud = &_inven_dude->data.inventory;
            _adjust_fid();
            _display_body(-1, inventoryWindowType);
            _display_inventory(_stack_offset[_curr_stack], -1, inventoryWindowType);
        }
    } else if (keyCode == 2501) {
        if (_target_curr_stack > 0) {
            _target_curr_stack -= 1;
            Object* v5 = _target_stack[_target_curr_stack];
            _target_pud = &(v5->data.inventory);
            _display_body(v5->fid, inventoryWindowType);
            _display_target_inventory(_target_stack_offset[_target_curr_stack], -1, _target_pud, inventoryWindowType);
            windowRefresh(gInventoryWindow);
        }
    }
}

// Drop item inside a container item (bag, backpack, etc.).
// 0x476464
static int _drop_into_container(Object* container, Object* item, int sourceIndex, Object** itemSlot, int quantity)
{
    int quantityToMove;
    if (quantity > 1) {
        quantityToMove = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, item, quantity);
    } else {
        quantityToMove = 1;
    }

    if (quantityToMove == -1) {
        return -1;
    }

    int rc = containerStoreItem(_inven_dude, container, item, quantityToMove, sourceIndex != -1);
    if (rc == 0) {
        if (itemSlot != nullptr) {
            if (itemSlot == &gInventoryArmor) {
                _adjust_ac(_stack[0], gInventoryArmor, nullptr);
            }
            *itemSlot = nullptr;
        }
    }

    return rc;
}

// 0x47650C
static int _drop_ammo_into_weapon(Object* weapon, Object* ammo, Object** ammoItemSlot, int quantity, int keyCode)
{
    if (itemGetType(weapon) != ITEM_TYPE_WEAPON) {
        return -1;
    }

    if (itemGetType(ammo) != ITEM_TYPE_AMMO) {
        return -1;
    }

    if (!weaponCanBeReloadedWith(weapon, ammo)) {
        return -1;
    }

    int quantityToMove;
    if (quantity > 1) {
        quantityToMove = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_MOVE_ITEMS, ammo, quantity);
    } else {
        quantityToMove = 1;
    }

    if (quantityToMove == -1) {
        return -1;
    }

    bool firstPackConsumed;
    int rc = weaponLoadAmmo(_inven_dude, weapon, ammo, quantityToMove, ammoItemSlot == nullptr, &firstPackConsumed);
    if (firstPackConsumed && ammoItemSlot != nullptr) {
        *ammoItemSlot = nullptr;
    }

    return rc;
}

// 0x47664C
static void _draw_amount(int value, int inventoryWindowType)
{
    // BIGNUM.frm
    FrmImage numbersFrmImage;
    int numbersFid = buildFid(OBJ_TYPE_INTERFACE, 170, 0, 0, 0);
    if (!numbersFrmImage.lock(numbersFid)) {
        return;
    }

    Rect rect;

    int windowWidth = windowGetWidth(_mt_wid);
    unsigned char* windowBuffer = windowGetBuffer(_mt_wid);

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        rect.left = 125;
        rect.top = 45;
        rect.right = 195;
        rect.bottom = 69;

        int ranks[5];
        ranks[4] = value % 10;
        ranks[3] = value / 10 % 10;
        ranks[2] = value / 100 % 10;
        ranks[1] = value / 1000 % 10;
        ranks[0] = value / 10000 % 10;

        windowBuffer += rect.top * windowWidth + rect.left;

        for (int index = 0; index < 5; index++) {
            unsigned char* src = numbersFrmImage.getData() + 14 * ranks[index];
            blitBufferToBuffer(src, 14, 24, 336, windowBuffer, windowWidth);
            windowBuffer += 14;
        }
    } else {
        rect.left = 133;
        rect.top = 64;
        rect.right = 189;
        rect.bottom = 88;

        windowBuffer += windowWidth * rect.top + rect.left;
        blitBufferToBuffer(numbersFrmImage.getData() + 14 * (value / 60), 14, 24, 336, windowBuffer, windowWidth);
        blitBufferToBuffer(numbersFrmImage.getData() + 14 * (value % 60 / 10), 14, 24, 336, windowBuffer + 14 * 2, windowWidth);
        blitBufferToBuffer(numbersFrmImage.getData() + 14 * (value % 10), 14, 24, 336, windowBuffer + 14 * 3, windowWidth);
    }

    windowRefreshRect(_mt_wid, &rect);
}

// 0x47688C
static int inventoryQuantitySelect(int inventoryWindowType, Object* item, int max)
{
    ScopedGameMode gm(GameMode::kCounter);

    inventoryQuantityWindowInit(inventoryWindowType, item);

    int value;
    int min;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        value = 1;
        if (max > 99999) {
            max = 99999;
        }
        min = 1;
    } else {
        value = 60;
        min = 10;
    }

    _draw_amount(value, inventoryWindowType);

    bool isTyping = false;
    for (;;) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode == KEY_ESCAPE) {
            inventoryQuantityWindowFree(inventoryWindowType);
            return -1;
        }

        if (keyCode == KEY_RETURN) {
            if (value >= min && value <= max) {
                if (inventoryWindowType != INVENTORY_WINDOW_TYPE_SET_TIMER || value % 10 == 0) {
                    soundPlayFile("ib1p1xx1");
                    break;
                }
            }

            soundPlayFile("iisxxxx1");
        } else if (keyCode == 5000) {
            isTyping = false;
            value = max;
            _draw_amount(value, inventoryWindowType);
        } else if (keyCode == 6000) {
            isTyping = false;
            if (value < max) {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
                    if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
                        getTicks();

                        unsigned int delay = 100;
                        while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
                            sharedFpsLimiter.mark();

                            if (value < max) {
                                value++;
                            }

                            _draw_amount(value, inventoryWindowType);
                            inputGetInput();

                            if (delay > 1) {
                                delay--;
                                inputPauseForTocks(delay);
                            }

                            renderPresent();
                            sharedFpsLimiter.throttle();
                        }
                    } else {
                        if (value < max) {
                            value++;
                        }
                    }
                } else {
                    value += 10;
                }

                _draw_amount(value, inventoryWindowType);
                continue;
            }
        } else if (keyCode == 7000) {
            isTyping = false;
            if (value > min) {
                if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
                    if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
                        getTicks();

                        unsigned int delay = 100;
                        while ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_REPEAT) != 0) {
                            sharedFpsLimiter.mark();

                            if (value > min) {
                                value--;
                            }

                            _draw_amount(value, inventoryWindowType);
                            inputGetInput();

                            if (delay > 1) {
                                delay--;
                                inputPauseForTocks(delay);
                            }

                            renderPresent();
                            sharedFpsLimiter.throttle();
                        }
                    } else {
                        if (value > min) {
                            value--;
                        }
                    }
                } else {
                    value -= 10;
                }

                _draw_amount(value, inventoryWindowType);
                continue;
            }
        }

        if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
            if (keyCode >= KEY_0 && keyCode <= KEY_9) {
                int number = keyCode - KEY_0;
                if (!isTyping) {
                    value = 0;
                }

                value = 10 * value % 100000 + number;
                isTyping = true;

                _draw_amount(value, inventoryWindowType);
                continue;
            } else if (keyCode == KEY_BACKSPACE) {
                if (!isTyping) {
                    value = 0;
                }

                value /= 10;
                isTyping = true;

                _draw_amount(value, inventoryWindowType);
                continue;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    inventoryQuantityWindowFree(inventoryWindowType);

    return value;
}

// Creates move items/set timer interface.
//
// 0x476AB8
static int inventoryQuantityWindowInit(int inventoryWindowType, Object* item)
{
    const int oldFont = fontGetCurrent();
    fontSetCurrent(103);

    const InventoryWindowDescription* windowDescription = &(gInventoryWindowDescriptions[inventoryWindowType]);

    // Maintain original position in original resolution, otherwise center it.
    int quantityWindowX = screenGetWidth() != 640
        ? (screenGetWidth() - windowDescription->width) / 2
        : windowDescription->x;
    int quantityWindowY = screenGetHeight() != 480
        ? (screenGetHeight() - windowDescription->height) / 2
        : windowDescription->y;
    _mt_wid = windowCreate(quantityWindowX, quantityWindowY, windowDescription->width, windowDescription->height, 257, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    unsigned char* windowBuffer = windowGetBuffer(_mt_wid);

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, windowDescription->frmId, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        blitBufferToBuffer(backgroundFrmImage.getData(),
            windowDescription->width,
            windowDescription->height,
            windowDescription->width,
            windowBuffer,
            windowDescription->width);
    }

    MessageListItem messageListItem;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        // MOVE ITEMS
        messageListItem.num = 21;
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            int length = fontGetStringWidth(messageListItem.text);
            fontDrawText(windowBuffer + windowDescription->width * 9 + (windowDescription->width - length) / 2, messageListItem.text, 200, windowDescription->width, _colorTable[21091]);
        }
    } else if (inventoryWindowType == INVENTORY_WINDOW_TYPE_SET_TIMER) {
        // SET TIMER
        messageListItem.num = 23;
        if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
            int length = fontGetStringWidth(messageListItem.text);
            fontDrawText(windowBuffer + windowDescription->width * 9 + (windowDescription->width - length) / 2, messageListItem.text, 200, windowDescription->width, _colorTable[21091]);
        }

        // Timer overlay
        FrmImage overlayFrmImage;
        int overlayFid = buildFid(OBJ_TYPE_INTERFACE, 306, 0, 0, 0);
        if (overlayFrmImage.lock(overlayFid)) {
            blitBufferToBuffer(overlayFrmImage.getData(),
                105,
                81,
                105,
                windowBuffer + 34 * windowDescription->width + 113,
                windowDescription->width);
        }
    }

    int inventoryFid = itemGetInventoryFid(item);
    artRender(inventoryFid, windowBuffer + windowDescription->width * 46 + 16, INVENTORY_LARGE_SLOT_WIDTH, INVENTORY_LARGE_SLOT_HEIGHT, windowDescription->width);

    int x;
    int y;
    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        x = 200;
        y = 46;
    } else {
        x = 194;
        y = 64;
    }

    int fid;
    int btn;

    // Plus button
    fid = buildFid(OBJ_TYPE_INTERFACE, 193, 0, 0, 0);
    _moveFrmImages[0].lock(fid);

    fid = buildFid(OBJ_TYPE_INTERFACE, 194, 0, 0, 0);
    _moveFrmImages[1].lock(fid);

    if (_moveFrmImages[0].isLocked() && _moveFrmImages[1].isLocked()) {
        btn = buttonCreate(_mt_wid,
            x,
            y,
            16,
            12,
            -1,
            -1,
            6000,
            -1,
            _moveFrmImages[0].getData(),
            _moveFrmImages[1].getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
        }
    }

    // Minus button
    fid = buildFid(OBJ_TYPE_INTERFACE, 191, 0, 0, 0);
    _moveFrmImages[2].lock(fid);

    fid = buildFid(OBJ_TYPE_INTERFACE, 192, 0, 0, 0);
    _moveFrmImages[3].lock(fid);

    if (_moveFrmImages[2].isLocked() && _moveFrmImages[3].isLocked()) {
        btn = buttonCreate(_mt_wid,
            x,
            y + 12,
            17,
            12,
            -1,
            -1,
            7000,
            -1,
            _moveFrmImages[2].getData(),
            _moveFrmImages[3].getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
        }
    }

    fid = buildFid(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    _moveFrmImages[4].lock(fid);

    fid = buildFid(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    _moveFrmImages[5].lock(fid);

    if (_moveFrmImages[4].isLocked() && _moveFrmImages[5].isLocked()) {
        // Done
        btn = buttonCreate(_mt_wid,
            98,
            128,
            15,
            16,
            -1,
            -1,
            -1,
            KEY_RETURN,
            _moveFrmImages[4].getData(),
            _moveFrmImages[5].getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
        }

        // Cancel
        btn = buttonCreate(_mt_wid,
            148,
            128,
            15,
            16,
            -1,
            -1,
            -1,
            KEY_ESCAPE,
            _moveFrmImages[4].getData(),
            _moveFrmImages[5].getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
        }
    }

    if (inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS) {
        fid = buildFid(OBJ_TYPE_INTERFACE, 307, 0, 0, 0);
        _moveFrmImages[6].lock(fid);

        fid = buildFid(OBJ_TYPE_INTERFACE, 308, 0, 0, 0);
        _moveFrmImages[7].lock(fid);

        if (_moveFrmImages[6].isLocked() && _moveFrmImages[7].isLocked()) {
            // ALL
            messageListItem.num = 22;
            if (messageListGetItem(&gInventoryMessageList, &messageListItem)) {
                int length = fontGetStringWidth(messageListItem.text);

                // TODO: Where is y? Is it hardcoded in to 376?
                fontDrawText(_moveFrmImages[6].getData() + (94 - length) / 2 + 376, messageListItem.text, 200, 94, _colorTable[21091]);
                fontDrawText(_moveFrmImages[7].getData() + (94 - length) / 2 + 376, messageListItem.text, 200, 94, _colorTable[18977]);

                btn = buttonCreate(_mt_wid,
                    120,
                    80,
                    94,
                    33,
                    -1,
                    -1,
                    -1,
                    5000,
                    _moveFrmImages[6].getData(),
                    _moveFrmImages[7].getData(),
                    nullptr,
                    BUTTON_FLAG_TRANSPARENT);
                if (btn != -1) {
                    buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
                }
            }
        }
    }

    windowRefresh(_mt_wid);
    inventorySetCursor(INVENTORY_WINDOW_CURSOR_ARROW);
    fontSetCurrent(oldFont);

    return 0;
}

// 0x477030
static int inventoryQuantityWindowFree(int inventoryWindowType)
{
    int count = inventoryWindowType == INVENTORY_WINDOW_TYPE_MOVE_ITEMS ? 8 : 6;

    for (int index = 0; index < count; index++) {
        _moveFrmImages[index].unlock();
    }

    windowDestroy(_mt_wid);

    return 0;
}

// 0x477074
int _inven_set_timer(Object* item)
{
    bool isInitialized = _inven_is_initialized;

    if (!isInitialized) {
        if (inventoryCommonInit() == -1) {
            return -1;
        }
    }

    int seconds = inventoryQuantitySelect(INVENTORY_WINDOW_TYPE_SET_TIMER, item, 180);

    if (!isInitialized) {
        // NOTE: Uninline.
        inventoryCommonFree();
    }

    return seconds;
}

Object* inven_get_current_target_obj()
{
    return _target_stack[_target_curr_stack];
}

} // namespace fallout
