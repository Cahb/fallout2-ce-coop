#include "object.h"

#include <assert.h>
#include <string.h>

#include <algorithm>

#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat.h"
#include "critter.h"
#include "debug.h"
#include "draw.h"
#include "game.h"
#include "game_mouse.h"
#include "item.h"
#include "light.h"
#include "map.h"
#include "memory.h"
#include "object_render.h"
#include "party_member.h"
#include "presenter.h"
#include "proto.h"
#include "proto_instance.h"
#include "scripts.h"
#include "server_players.h"
#include "settings.h"
#include "svga.h"
#include "text_object.h"
#include "tile.h"
#include "worldmap.h"
#include "server_loop.h"
#include "client_net.h" // clientViewerActive — combat-mirror guards (COMBAT_CLIENT_DESIGN.md §5.1)

namespace fallout {

static int objectLoadAllInternal(File* stream);
static void _object_fix_weapon_ammo(Object* obj);
static int objectWrite(Object* obj, File* stream);
static int _obj_offset_table_init();
static void _obj_offset_table_exit();
static int _obj_order_table_init();
static int _obj_order_comp_func_even(const void* a1, const void* a2);
static int _obj_order_comp_func_odd(const void* a1, const void* a2);
static void _obj_order_table_exit();
static void _obj_light_table_init();
static int _obj_save_obj(File* stream, Object* object);
static int _obj_load_obj(File* stream, Object** objectPtr, int elevation, Object* owner);
static int objectAllocate(Object** objectPtr);
static void objectDeallocate(Object** objectPtr);
static int objectListNodeCreate(ObjectListNode** nodePtr);
static void objectListNodeDestroy(ObjectListNode** nodePtr);
static int objectGetListNode(Object* obj, ObjectListNode** out_node, ObjectListNode** out_prev_node);
static void _obj_insert(ObjectListNode* ptr);
static int _obj_remove(ObjectListNode* a1, ObjectListNode* a2);
static int _obj_connect_to_tile(ObjectListNode* node, int tile_index, int elev, Rect* rect);
static int _obj_adjust_light(Object* obj, int a2, Rect* rect);
static int _obj_preload_sort(const void* a1, const void* a2);

// 0x5195F8
bool gObjectsInitialized = false;

// 0x5195FC
static int gObjectsUpdateAreaHexWidth = 0;

// 0x519600
static int gObjectsUpdateAreaHexHeight = 0;

// 0x519604
int gObjectsUpdateAreaHexSize = 0;

// 0x519608
int* _orderTable[2] = {
    nullptr,
    nullptr,
};

// 0x519610
int* _offsetTable[2] = {
    nullptr,
    nullptr,
};

// 0x519618
int* _offsetDivTable = nullptr;

// 0x51961C
int* _offsetModTable = nullptr;



// Contains objects that are not bounded to tiles.
//
// 0x519628
ObjectListNode* gObjectListHead = nullptr;

// 0x51962C
static int _centerToUpperLeft = 0;

// 0x519630
static int gObjectFindElevation = 0;

// 0x519634
static int gObjectFindTile = 0;

// 0x519638
static ObjectListNode* gObjectFindLastObjectListNode = nullptr;

// 0x51963C
static int* gObjectFids = nullptr;

// 0x519640
static int gObjectFidsLength = 0;

// 0x51964C
static Rect _light_rect[9] = {
    { 0, 0, 96, 42 },
    { 0, 0, 160, 74 },
    { 0, 0, 224, 106 },
    { 0, 0, 288, 138 },
    { 0, 0, 352, 170 },
    { 0, 0, 416, 202 },
    { 0, 0, 480, 234 },
    { 0, 0, 544, 266 },
    { 0, 0, 608, 298 },
};

// 0x5196DC
static int _light_distance[36] = {
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    3,
    4,
    5,
    6,
    7,
    8,
    4,
    5,
    6,
    7,
    8,
    5,
    6,
    7,
    8,
    6,
    7,
    8,
    7,
    8,
    8,
};

// 0x51976C
static int gViolenceLevel = -1;

// 0x519770
static int _obj_last_roof_x = -1;

// 0x519774
static int _obj_last_roof_y = -1;

// 0x519778
static int _obj_last_elev = -1;

// 0x51977C
static bool _obj_last_is_empty = true;






// 0x519794
Object* _moveBlockObj = nullptr;

// 0x519798
static int _objItemOutlineState = 0;

// 0x51979C
static int _cd_order[9] = {
    1,
    0,
    3,
    5,
    4,
    2,
    0,
    0,
    0,
};

// 0x6391D0
static int _light_blocked[6][36];

// 0x639530
static int _light_offsets[2][6][36];

// 0x639BF0
Rect gObjectsWindowRect;


// 0x639D90
static Rect gObjectsUpdateAreaPixelBounds;

// Contains objects that are bounded to tiles.
//
// 0x639DA0
ObjectListNode* gObjectListHeadByTile[HEX_GRID_SIZE];



// 0x6610A0
int gObjectsWindowBufferSize;

// 0x6610A4
unsigned char* gObjectsWindowBuffer;

// 0x6610A8
static int gObjectsWindowHeight;

// Translucent "egg" effect around player.
//
// 0x6610AC
Object* gEgg;

// 0x6610B0
int gObjectsWindowPitch;

// 0x6610B4
static int gObjectsWindowWidth;

// obj_dude
// 0x6610B8
Object* gDude;

// 0x6610BC
static char _obj_seen_check[5001];

// 0x662445
static char _obj_seen[5001];

// obj_init
// 0x488780
int objectsInit(unsigned char* buf, int width, int height, int pitch)
{
    int dudeFid;
    int eggFid;

    memset(_obj_seen, 0, 5001);
    gObjectsUpdateAreaPixelBounds.right = width + 320;
    gObjectsUpdateAreaPixelBounds.left = -320;
    gObjectsUpdateAreaPixelBounds.bottom = height + 240;
    gObjectsUpdateAreaPixelBounds.top = -240;

    gObjectsUpdateAreaHexWidth = (gObjectsUpdateAreaPixelBounds.right + 320 + 1) / 32 + 1;
    gObjectsUpdateAreaHexHeight = (gObjectsUpdateAreaPixelBounds.bottom + 240 + 1) / 12 + 1;
    gObjectsUpdateAreaHexSize = gObjectsUpdateAreaHexWidth * gObjectsUpdateAreaHexHeight;

    memset(gObjectListHeadByTile, 0, sizeof(gObjectListHeadByTile));

    if (_obj_offset_table_init() == -1) {
        return -1;
    }

    if (_obj_order_table_init() == -1) {
        goto err;
    }

    if (_obj_render_table_init() == -1) {
        goto err_2;
    }

    if (lightInit() == -1) {
        goto err_2;
    }

    if (textObjectsInit(buf, width, height) == -1) {
        goto err_2;
    }

    _obj_light_table_init();
    _obj_blend_table_init();

    _centerToUpperLeft = tileFromScreenXY(gObjectsUpdateAreaPixelBounds.left, gObjectsUpdateAreaPixelBounds.top, 0) - gCenterTile;
    gObjectsWindowWidth = width;
    gObjectsWindowHeight = height;
    gObjectsWindowBuffer = buf;

    gObjectsWindowRect.left = 0;
    gObjectsWindowRect.top = 0;
    gObjectsWindowRect.right = width - 1;
    gObjectsWindowRect.bottom = height - 1;

    gObjectsWindowBufferSize = height * width;
    gObjectsWindowPitch = pitch;

    dudeFid = buildFid(OBJ_TYPE_CRITTER, _art_vault_guy_num, 0, 0, 0);
    objectCreateWithFidPid(&gDude, dudeFid, 0x1000000);

    gDude->flags |= OBJECT_NO_REMOVE;
    gDude->flags |= OBJECT_NO_SAVE;
    gDude->flags |= OBJECT_HIDDEN;
    gDude->flags |= OBJECT_LIGHT_THRU;
    objectSetLight(gDude, 4, 0x10000, nullptr);

    if (partyMemberAdd(gDude) == -1) {
        debugPrint("\n  Error: Can't add Player into party!");
        exit(1);
    }

    eggFid = buildFid(OBJ_TYPE_INTERFACE, 2, 0, 0, 0);
    objectCreateWithFidPid(&gEgg, eggFid, -1);
    gEgg->flags |= OBJECT_NO_REMOVE;
    gEgg->flags |= OBJECT_NO_SAVE;
    gEgg->flags |= OBJECT_HIDDEN;
    gEgg->flags |= OBJECT_LIGHT_THRU;

    gObjectsInitialized = true;

    return 0;

err_2:

    // NOTE: Uninline.
    _obj_order_table_exit();

err:

    _obj_offset_table_exit();

    return -1;
}

// 0x488A00
void objectsReset()
{
    if (gObjectsInitialized) {
        textObjectsReset();
        _obj_remove_all();
        memset(_obj_seen, 0, 5001);
        lightReset();
    }
}

// 0x488A30
void objectsExit()
{
    if (gObjectsInitialized) {
        gDude->flags &= ~OBJECT_NO_REMOVE;
        gEgg->flags &= ~OBJECT_NO_REMOVE;

        _obj_remove_all();
        textObjectsFree();

        // NOTE: Uninline.
        _obj_blend_table_exit();

        lightExit();

        // NOTE: Uninline.
        _obj_render_table_exit();

        // NOTE: Uninline.
        _obj_order_table_exit();

        _obj_offset_table_exit();
    }
}

// 0x488AF4
int objectRead(Object* obj, File* stream)
{
    int field_74;

    if (fileReadInt32(stream, &(obj->id)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->tile)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->x)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->y)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->sx)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->sy)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->frame)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->rotation)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->fid)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->flags)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->elevation)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->pid)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->cid)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->lightDistance)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->lightIntensity)) == -1) return -1;
    if (fileReadInt32(stream, &field_74) == -1) return -1;
    if (fileReadInt32(stream, &(obj->sid)) == -1) return -1;
    if (fileReadInt32(stream, &(obj->scriptIndex)) == -1) return -1;

    obj->outline = 0;
    obj->owner = nullptr;

    if (objectDataRead(obj, stream) != 0) {
        return -1;
    }

    // whoHitMe is a runtime-only CACHE of the persistent whoHitMeCid; a serialized
    // pointer is always garbage across a load (a foreign process address, or — the
    // fatal case — a legacy 4-byte value sitting in the 8-byte field on 64-bit, e.g.
    // -1 -> 0xffffffff, a non-null bogus pointer). The many nullptr-only-guarded deref
    // sites (combat.cc:5453 _damage_object, state_dump.cc:98, actions.cc:2449) fault on
    // it. Null it on EVERY deserialization — objectRead is the single chokepoint for map
    // load, savegame, join blob AND _obj_copy (which round-trips through here) — so no
    // path can surface a bogus whoHitMe. This SUBSUMES _map_fix_critter_combat_data
    // (which only covered critters present at map-load time — artemple's script-spawned
    // Temple guards loaded LATER escaped it → the cdamage SIGSEGV). whoHitMeCid is
    // untouched; combat re-resolves the pointer via _find_cid at _combat_begin. See the
    // id-plus-cached-pointer antipattern (do NOT 32-bit-mask the deref — fix the source).
    if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
        obj->data.critter.combat.whoHitMe = nullptr;
    }

    if (isExitGridPid(obj->pid)) {
        if (obj->data.misc.map <= 0) {
            if ((obj->fid & 0xFFF) < 33) {
                obj->fid = buildFid(OBJ_TYPE_MISC, (obj->fid & 0xFFF) + 16, FID_ANIM_TYPE(obj->fid), 0, 0);
            }
        }
    } else {
        if (PID_TYPE(obj->pid) == 0 && !(gMapHeader.flags & 0x01)) {
            _object_fix_weapon_ammo(obj);
        }

        if (PID_TYPE(obj->pid) == OBJ_TYPE_ITEM
            && itemGetType(obj) == ITEM_TYPE_WEAPON
            && obj->data.item.weapon.ammoQuantity < 0) {
            obj->data.item.weapon.ammoQuantity = 0;
        }
    }

    return 0;
}

// 0x488CE4
int objectLoadAll(File* stream)
{
    int rc = objectLoadAllInternal(stream);

    gViolenceLevel = -1;

    return rc;
}

// 0x488CF8
static int objectLoadAllInternal(File* stream)
{
    if (stream == nullptr) {
        return -1;
    }

    bool fixMapInventory = settings.mapper.fix_map_inventory;

    gViolenceLevel = settings.preferences.violence_level;

    int objectCount;
    if (fileReadInt32(stream, &objectCount) == -1) {
        return -1;
    }

    if (gObjectFids != nullptr) {
        internal_free(gObjectFids);
    }

    if (objectCount != 0) {
        gObjectFids = (int*)internal_malloc(sizeof(*gObjectFids) * objectCount);
        memset(gObjectFids, 0, sizeof(*gObjectFids) * objectCount);
        if (gObjectFids == nullptr) {
            return -1;
        }
        gObjectFidsLength = 0;
    }

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        int objectCountAtElevation;
        if (fileReadInt32(stream, &objectCountAtElevation) == -1) {
            return -1;
        }

        for (int objectIndex = 0; objectIndex < objectCountAtElevation; objectIndex++) {
            ObjectListNode* objectListNode;

            // NOTE: Uninline.
            if (objectListNodeCreate(&objectListNode) == -1) {
                return -1;
            }

            if (objectAllocate(&(objectListNode->obj)) == -1) {
                // NOTE: Uninline.
                objectListNodeDestroy(&objectListNode);
                return -1;
            }

            if (objectRead(objectListNode->obj, stream) != 0) {
                // NOTE: Uninline.
                objectDeallocate(&(objectListNode->obj));

                // NOTE: Uninline.
                objectListNodeDestroy(&objectListNode);

                return -1;
            }

            objectListNode->obj->outline = 0;
            gObjectFids[gObjectFidsLength++] = objectListNode->obj->fid;

            if (objectListNode->obj->sid != -1) {
                Script* script;
                if (scriptGetScript(objectListNode->obj->sid, &script) == -1) {
                    objectListNode->obj->sid = -1;
                    debugPrint("\nError connecting object to script!");
                } else {
                    script->owner = objectListNode->obj;
                    objectListNode->obj->scriptIndex = script->index;
                }
            }

            _obj_fix_violence_settings(&(objectListNode->obj->fid));
            objectListNode->obj->elevation = elevation;

            _obj_insert(objectListNode);

            if ((objectListNode->obj->flags & OBJECT_NO_REMOVE) && PID_TYPE(objectListNode->obj->pid) == OBJ_TYPE_CRITTER && objectListNode->obj->pid != 18000) {
                objectListNode->obj->flags &= ~OBJECT_NO_REMOVE;
            }

            Inventory* inventory = &(objectListNode->obj->data.inventory);
            if (inventory->length != 0) {
                // length/capacity come straight from the object stream, which on a
                // rebaseline is a wire/blob-loaded trust boundary. A corrupt or
                // garbage count (observed: capacity ~0xfffffffe on a co-op map-switch
                // rebaseline) otherwise drives an impossible malloc — ASAN aborts with
                // allocation-size-too-big, a stock build crashes or wildly over-
                // allocates — and a length past capacity overruns the item array
                // below. Reject the object rather than trust it. (The corrupt count's
                // upstream cause is the applyBlob teardown/parse corruption; this is
                // the boundary backstop so a bad blob fails the load instead of
                // crashing.)
                if (inventory->length < 0
                    || inventory->capacity < inventory->length
                    || (size_t)(unsigned int)inventory->capacity > SIZE_MAX / sizeof(InventoryItem)) {
                    // Loud, not silent: name the object so a repro points AT the
                    // root (which object's serialization desynced the stream) instead
                    // of the reject hiding it. If pid/fid also look like garbage, the
                    // whole object struct is corrupt → a stream mis-alignment earlier
                    // in objectLoadAll; if they look valid, this specific object's
                    // inventory write/read disagrees.
                    debugPrint("Error loading inventory: bad length=%d capacity=%d "
                               "obj pid=0x%X fid=0x%X — corrupt blob or stream desync\n",
                        inventory->length, inventory->capacity,
                        objectListNode->obj->pid, objectListNode->obj->fid);
                    return -1;
                }
                inventory->items = (InventoryItem*)internal_malloc(sizeof(InventoryItem) * inventory->capacity);
                if (inventory->items == nullptr) {
                    return -1;
                }

                for (int inventoryItemIndex = 0; inventoryItemIndex < inventory->length; inventoryItemIndex++) {
                    InventoryItem* inventoryItem = &(inventory->items[inventoryItemIndex]);
                    if (fileReadInt32(stream, &(inventoryItem->quantity)) != 0) {
                        debugPrint("Error loading inventory\n");
                        return -1;
                    }

                    if (fixMapInventory) {
                        inventoryItem->item = (Object*)internal_malloc(sizeof(Object));
                        if (inventoryItem->item == nullptr) {
                            debugPrint("Error loading inventory\n");
                            return -1;
                        }

                        if (objectRead(inventoryItem->item, stream) != 0) {
                            debugPrint("Error loading inventory\n");
                            return -1;
                        }
                    } else {
                        if (_obj_load_obj(stream, &(inventoryItem->item), elevation, objectListNode->obj) == -1) {
                            return -1;
                        }
                    }
                }
            } else {
                inventory->capacity = 0;
                inventory->items = nullptr;
            }
        }
    }

    _obj_rebuild_all_light();

    return 0;
}

// Fixes ammo pid and number of charges.
//
// 0x48911C
static void _object_fix_weapon_ammo(Object* obj)
{
    if (PID_TYPE(obj->pid) != OBJ_TYPE_ITEM) {
        return;
    }

    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1) {
        debugPrint("\nError: obj_load: proto_ptr failed on pid");
        exit(1);
    }

    int charges;
    if (itemGetType(obj) == ITEM_TYPE_WEAPON) {
        int ammoTypePid = obj->data.item.weapon.ammoTypePid;
        if (ammoTypePid == 0xCCCCCCCC || ammoTypePid == -1) {
            obj->data.item.weapon.ammoTypePid = proto->item.data.weapon.ammoTypePid;
        }

        charges = obj->data.item.weapon.ammoQuantity;
        if (charges == 0xCCCCCCCC || charges == -1 || charges != proto->item.data.weapon.ammoCapacity) {
            obj->data.item.weapon.ammoQuantity = proto->item.data.weapon.ammoCapacity;
        }
    } else {
        if (PID_TYPE(obj->pid) == OBJ_TYPE_MISC) {
            // FIXME: looks like this code in unreachable
            charges = obj->data.item.misc.charges;
            if (charges == 0xCCCCCCCC) {
                charges = proto->item.data.misc.charges;
                obj->data.item.misc.charges = charges;
                if (charges == 0xCCCCCCCC) {
                    debugPrint("\nError: Misc Item Prototype %s: charges incorrect!", protoGetName(obj->pid));
                    obj->data.item.misc.charges = 0;
                }
            } else {
                if (charges != proto->item.data.misc.charges) {
                    obj->data.item.misc.charges = proto->item.data.misc.charges;
                }
            }
        }
    }
}

// 0x489200
static int objectWrite(Object* obj, File* stream)
{
    if (fileWriteInt32(stream, obj->id) == -1) return -1;
    if (fileWriteInt32(stream, obj->tile) == -1) return -1;
    if (fileWriteInt32(stream, obj->x) == -1) return -1;
    if (fileWriteInt32(stream, obj->y) == -1) return -1;
    if (fileWriteInt32(stream, obj->sx) == -1) return -1;
    if (fileWriteInt32(stream, obj->sy) == -1) return -1;
    if (fileWriteInt32(stream, obj->frame) == -1) return -1;
    if (fileWriteInt32(stream, obj->rotation) == -1) return -1;
    if (fileWriteInt32(stream, obj->fid) == -1) return -1;
    if (fileWriteInt32(stream, obj->flags) == -1) return -1;
    if (fileWriteInt32(stream, obj->elevation) == -1) return -1;
    if (fileWriteInt32(stream, obj->pid) == -1) return -1;
    if (fileWriteInt32(stream, obj->cid) == -1) return -1;
    if (fileWriteInt32(stream, obj->lightDistance) == -1) return -1;
    if (fileWriteInt32(stream, obj->lightIntensity) == -1) return -1;
    if (fileWriteInt32(stream, obj->outline) == -1) return -1;
    if (fileWriteInt32(stream, obj->sid) == -1) return -1;
    if (fileWriteInt32(stream, obj->scriptIndex) == -1) return -1;
    if (objectDataWrite(obj, stream) == -1) return -1;

    return 0;
}

// 0x48935C
int objectSaveAll(File* stream)
{
    if (stream == nullptr) {
        return -1;
    }

    _obj_process_seen();

    int objectCount = 0;

    long objectCountPos = fileTell(stream);
    if (fileWriteInt32(stream, objectCount) == -1) {
        return -1;
    }

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        int objectCountAtElevation = 0;

        long objectCountAtElevationPos = fileTell(stream);
        if (fileWriteInt32(stream, objectCountAtElevation) == -1) {
            return -1;
        }

        for (int tile = 0; tile < HEX_GRID_SIZE; tile++) {
            for (ObjectListNode* objectListNode = gObjectListHeadByTile[tile]; objectListNode != nullptr; objectListNode = objectListNode->next) {
                Object* object = objectListNode->obj;
                if (object->elevation != elevation) {
                    continue;
                }

                if ((object->flags & OBJECT_NO_SAVE) != 0) {
                    continue;
                }

                CritterCombatData* combatData = nullptr;
                Object* whoHitMe = nullptr;
                if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
                    combatData = &(object->data.critter.combat);
                    whoHitMe = combatData->whoHitMe;
                    if (whoHitMe != nullptr) {
                        if (combatData->whoHitMeCid != -1) {
                            combatData->whoHitMeCid = whoHitMe->cid;
                        }
                    } else {
                        combatData->whoHitMeCid = -1;
                    }
                }

                if (objectWrite(object, stream) == -1) {
                    return -1;
                }

                if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
                    combatData->whoHitMe = whoHitMe;
                }

                Inventory* inventory = &(object->data.inventory);
                for (int index = 0; index < inventory->length; index++) {
                    InventoryItem* inventoryItem = &(inventory->items[index]);

                    if (fileWriteInt32(stream, inventoryItem->quantity) == -1) {
                        return -1;
                    }

                    if (_obj_save_obj(stream, inventoryItem->item) == -1) {
                        return -1;
                    }
                }

                objectCountAtElevation++;
            }
        }

        long pos = fileTell(stream);
        fileSeek(stream, objectCountAtElevationPos, SEEK_SET);
        fileWriteInt32(stream, objectCountAtElevation);
        fileSeek(stream, pos, SEEK_SET);

        objectCount += objectCountAtElevation;
    }

    long pos = fileTell(stream);
    fileSeek(stream, objectCountPos, SEEK_SET);
    fileWriteInt32(stream, objectCount);
    fileSeek(stream, pos, SEEK_SET);

    return 0;
}



// 0x489A84
int objectCreateWithFidPid(Object** objectPtr, int fid, int pid)
{
    ObjectListNode* objectListNode;

    // NOTE: Uninline;
    if (objectListNodeCreate(&objectListNode) == -1) {
        return -1;
    }

    if (objectAllocate(&(objectListNode->obj)) == -1) {
        // Uninline.
        objectListNodeDestroy(&objectListNode);
        return -1;
    }

    objectListNode->obj->fid = fid;
    _obj_insert(objectListNode);

    if (objectPtr) {
        *objectPtr = objectListNode->obj;
    }

    objectListNode->obj->pid = pid;
    objectListNode->obj->id = scriptsNewObjectId();

    if (serverLoopActive()) {
        objectListNode->obj->netId = objectNextNetId();
    }

    if (pid == -1 || PID_TYPE(pid) == OBJ_TYPE_TILE) {
        Inventory* inventory = &(objectListNode->obj->data.inventory);
        inventory->length = 0;
        inventory->items = nullptr;
        return 0;
    }

    _proto_update_init(objectListNode->obj);

    Proto* proto = nullptr;
    if (protoGetProto(pid, &proto) == -1) {
        return 0;
    }

    objectSetLight(objectListNode->obj, proto->lightDistance, proto->lightIntensity, nullptr);

    if ((proto->flags & 0x08) != 0) {
        _obj_toggle_flat(objectListNode->obj, nullptr);
    }

    if ((proto->flags & 0x10) != 0) {
        objectListNode->obj->flags |= OBJECT_NO_BLOCK;
    }

    if ((proto->flags & 0x800) != 0) {
        objectListNode->obj->flags |= OBJECT_MULTIHEX;
    }

    if ((proto->flags & 0x8000) != 0) {
        objectListNode->obj->flags |= OBJECT_TRANS_NONE;
    } else {
        if ((proto->flags & 0x10000) != 0) {
            objectListNode->obj->flags |= OBJECT_TRANS_WALL;
        } else if ((proto->flags & 0x20000) != 0) {
            objectListNode->obj->flags |= OBJECT_TRANS_GLASS;
        } else if ((proto->flags & 0x40000) != 0) {
            objectListNode->obj->flags |= OBJECT_TRANS_STEAM;
        } else if ((proto->flags & 0x80000) != 0) {
            objectListNode->obj->flags |= OBJECT_TRANS_ENERGY;
        } else if ((proto->flags & 0x4000) != 0) {
            objectListNode->obj->flags |= OBJECT_TRANS_RED;
        }
    }

    if ((proto->flags & 0x20000000) != 0) {
        objectListNode->obj->flags |= OBJECT_LIGHT_THRU;
    }

    if ((proto->flags & 0x80000000) != 0) {
        objectListNode->obj->flags |= OBJECT_SHOOT_THRU;
    }

    if ((proto->flags & 0x10000000) != 0) {
        objectListNode->obj->flags |= OBJECT_WALL_TRANS_END;
    }

    if ((proto->flags & 0x1000) != 0) {
        objectListNode->obj->flags |= OBJECT_NO_HIGHLIGHT;
    }

    _obj_new_sid(objectListNode->obj, &(objectListNode->obj->sid));

    // MP_PROTOCOL.md §4 spawn: the object is now fully formed in the world.
    // (The pid==-1/TILE early-out above does not emit — tiles are not network
    // entities.)
    presenter()->objectCreated(objectListNode->obj);

    return 0;
}

// 0x489C9C
int objectCreateWithPid(Object** objectPtr, int pid)
{
    Proto* proto;

    *objectPtr = nullptr;

    if (protoGetProto(pid, &proto) == -1) {
        return -1;
    }

    return objectCreateWithFidPid(objectPtr, proto->fid, pid);
}

// 0x489CCC
int _obj_copy(Object** a1, Object* a2)
{
    if (a2 == nullptr) {
        return -1;
    }

    ObjectListNode* objectListNode;

    // NOTE: Uninline.
    if (objectListNodeCreate(&objectListNode) == -1) {
        return -1;
    }

    if (objectAllocate(&(objectListNode->obj)) == -1) {
        // NOTE: Uninline.
        objectListNodeDestroy(&objectListNode);
        return -1;
    }

    objectDataReset(objectListNode->obj);

    memcpy(objectListNode->obj, a2, sizeof(Object));

    if (a1 != nullptr) {
        *a1 = objectListNode->obj;
    }

    _obj_insert(objectListNode);

    objectListNode->obj->id = scriptsNewObjectId();

    if (serverLoopActive()) {
        objectListNode->obj->netId = objectNextNetId();
    }

    if (objectListNode->obj->sid != -1) {
        objectListNode->obj->sid = -1;
        _obj_new_sid(objectListNode->obj, &(objectListNode->obj->sid));
    }

    if (objectSetRotation(objectListNode->obj, a2->rotation, nullptr) == -1) {
        // TODO: Probably leaking object allocated with objectAllocate.
        // NOTE: Uninline.
        objectListNodeDestroy(&objectListNode);
        return -1;
    }

    objectListNode->obj->flags &= ~OBJECT_QUEUED;

    Inventory* newInventory = &(objectListNode->obj->data.inventory);
    newInventory->length = 0;
    newInventory->capacity = 0;
    // The memcpy above copied the SOURCE's items POINTER as well. Clearing only
    // length/capacity leaves the copy aliasing the source's array, and itemAdd's
    // growth test (`length == capacity || items == nullptr`) then takes the
    // realloc branch on that borrowed pointer — reallocating the SOURCE's array
    // and adopting it. Both objects end up sharing one array while the source's
    // pointer may already be dangling: edits to one inventory show up in the
    // other, entries read back as garbage items, and teardown frees freed memory.
    // Latent in vanilla only because every other caller copies an object whose
    // inventory is empty AND nullptr; copying the DUDE (co-op extra actors, which
    // carry the starting inventory) is what first supplies a non-null pointer.
    newInventory->items = nullptr;

    Inventory* oldInventory = &(a2->data.inventory);
    for (int index = 0; index < oldInventory->length; index++) {
        InventoryItem* oldInventoryItem = &(oldInventory->items[index]);

        Object* newItem;
        if (_obj_copy(&newItem, oldInventoryItem->item) == -1) {
            // TODO: Probably leaking object allocated with objectAllocate.
            // NOTE: Uninline.
            objectListNodeDestroy(&objectListNode);
            return -1;
        }

        if (itemAdd(objectListNode->obj, newItem, oldInventoryItem->quantity) == 1) {
            // TODO: Probably leaking object allocated with objectAllocate.
            // NOTE: Uninline.
            objectListNodeDestroy(&objectListNode);
            return -1;
        }

        // The recursive _obj_copy above _obj_insert'ed this item into the WORLD
        // object list, and itemAdd has now also put it in an inventory — the item
        // is in two places at once. Carried items belong in neither the world list
        // nor a tile bucket (see command.cc's give, which _obj_disconnect's right
        // after objectCreateWithPid for exactly this reason).
        //
        // The damage is deferred and looks unrelated: map teardown walks the world
        // list and frees the item, but an owner that survives teardown keeps
        // pointing at it. Observed with co-op extra actors (OBJECT_NO_REMOVE, so
        // they DO survive): after a transition their carried spear read back as
        // pid 0x0500000C — a freed object reinterpreted — rendering as a garbage
        // item. Left in the world list it is also a double free waiting for any
        // owner that is torn down normally.
        _obj_disconnect(newItem, nullptr);
    }

    // MP_PROTOCOL.md §4 spawn: the second create path (duplication / stack
    // split). Recursion above already emitted objectCreated for each copied
    // inventory item, so children precede their parent on the wire.
    presenter()->objectCreated(objectListNode->obj);

    return 0;
}

// 0x489EC4
int _obj_connect(Object* object, int tile, int elevation, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    if (!hexGridTileIsValid(tile)) {
        return -1;
    }

    if (!elevationIsValid(elevation)) {
        return -1;
    }

    ObjectListNode* objectListNode;

    // NOTE: Uninline.
    if (objectListNodeCreate(&objectListNode) == -1) {
        return -1;
    }

    objectListNode->obj = object;

    int rc = _obj_connect_to_tile(objectListNode, tile, elevation, rect);
    if (rc == 0) {
        // MP_PROTOCOL.md §4/§6.2b item<->world lifecycle: the object attached to a
        // world tile (drop/scatter/unload/script obj_connect). This bypasses
        // objectCreateWithFidPid and objectSetLocation, so it is the sole signal an
        // item re-parented from an inventory now appears at a tile. Distinct from
        // objectCreated — the object persists, it is not born here.
        presenter()->objectConnected(object, tile, elevation);
    }
    return rc;
}

// 0x489F34
int _obj_disconnect(Object* obj, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    ObjectListNode* node;
    ObjectListNode* prev_node;
    if (objectGetListNode(obj, &node, &prev_node) != 0) {
        return -1;
    }

    if (_obj_adjust_light(obj, 1, rect) == -1) {
        if (rect != nullptr) {
            objectGetRect(obj, rect);
        }
    }

    if (prev_node != nullptr) {
        prev_node->next = node->next;
    } else {
        int tile = node->obj->tile;
        if (tile == -1) {
            gObjectListHead = gObjectListHead->next;
        } else {
            gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
        }
    }

    if (node != nullptr) {
        internal_free(node);
    }

    obj->tile = -1;

    // MP_PROTOCOL.md §4/§6.2b item<->world lifecycle: the object detached from its
    // world tile (pickup/consume/unload/script obj_disconnect) into an inventory or
    // limbo. Bypasses objectDestroy (the object persists), so this is the sole
    // signal the item left the world. Reached only on the success path; obj->id is
    // intact, obj->tile is now -1.
    presenter()->objectDisconnected(obj);

    return 0;
}

// Put an OFF-MAP object back at a world tile, whichever off-map state it is
// in. There are two: _obj_disconnect freed its list node entirely, while a
// blob/appendix load of a tile==-1 object leaves it with a FLOATING node
// (_obj_load_player_actor inserts at -1 and objectSetLocation(-1) fails
// closed). _obj_connect would mint a SECOND node for the latter — so probe
// membership first and route each state through the primitive that fits.
// The co-op despawn/reattach cycle is the caller (server_control.cc).
int objectReattach(Object* obj, int tile, int elevation)
{
    if (obj == nullptr) {
        return -1;
    }

    ObjectListNode* node;
    ObjectListNode* previousNode;
    if (objectGetListNode(obj, &node, &previousNode) != 0) {
        return _obj_connect(obj, tile, elevation, nullptr);
    }

    return objectSetLocation(obj, tile, elevation, nullptr);
}

// 0x489FF8
int _obj_offset(Object* obj, int x, int y, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    ObjectListNode* node = nullptr;
    ObjectListNode* previousNode = nullptr;
    if (objectGetListNode(obj, &node, &previousNode) == -1) {
        return -1;
    }

    if (obj == gDude) {
        if (rect != nullptr) {
            Rect eggRect;
            objectGetRect(gEgg, &eggRect);
            rectCopy(rect, &eggRect);

            if (previousNode != nullptr) {
                previousNode->next = node->next;
            } else {
                int tile = node->obj->tile;
                if (tile == -1) {
                    gObjectListHead = gObjectListHead->next;
                } else {
                    gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
                }
            }

            obj->x += x;
            obj->sx += x;

            obj->y += y;
            obj->sy += y;

            _obj_insert(node);

            rectOffset(&eggRect, x, y);

            _obj_offset(gEgg, x, y, nullptr);
            rectUnion(rect, &eggRect, rect);
        } else {
            if (previousNode != nullptr) {
                previousNode->next = node->next;
            } else {
                int tile = node->obj->tile;
                if (tile == -1) {
                    gObjectListHead = gObjectListHead->next;
                } else {
                    gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
                }
            }

            obj->x += x;
            obj->sx += x;

            obj->y += y;
            obj->sy += y;

            _obj_insert(node);

            _obj_offset(gEgg, x, y, nullptr);
        }
    } else {
        if (rect != nullptr) {
            objectGetRect(obj, rect);

            if (previousNode != nullptr) {
                previousNode->next = node->next;
            } else {
                int tile = node->obj->tile;
                if (tile == -1) {
                    gObjectListHead = gObjectListHead->next;
                } else {
                    gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
                }
            }

            obj->x += x;
            obj->sx += x;

            obj->y += y;
            obj->sy += y;

            _obj_insert(node);

            Rect objectRect;
            rectCopy(&objectRect, rect);

            rectOffset(&objectRect, x, y);

            rectUnion(rect, &objectRect, rect);
        } else {
            if (previousNode != nullptr) {
                previousNode->next = node->next;
            } else {
                int tile = node->obj->tile;
                if (tile == -1) {
                    gObjectListHead = gObjectListHead->next;
                } else {
                    gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
                }
            }

            obj->x += x;
            obj->sx += x;

            obj->y += y;
            obj->sy += y;

            _obj_insert(node);
        }
    }

    return 0;
}

// 0x48A324
int _obj_move(Object* a1, int a2, int a3, int elevation, Rect* a5)
{
    if (a1 == nullptr) {
        return -1;
    }

    // TODO: Get rid of initialization.
    ObjectListNode* node = nullptr;
    ObjectListNode* previousNode;
    int v22 = 0;

    int tile = a1->tile;
    if (hexGridTileIsValid(tile)) {
        if (objectGetListNode(a1, &node, &previousNode) == -1) {
            return -1;
        }

        if (_obj_adjust_light(a1, 1, a5) == -1) {
            if (a5 != nullptr) {
                objectGetRect(a1, a5);
            }
        }

        if (previousNode != nullptr) {
            previousNode->next = node->next;
        } else {
            int tile = node->obj->tile;
            if (tile == -1) {
                gObjectListHead = gObjectListHead->next;
            } else {
                gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
            }
        }

        a1->tile = -1;
        a1->elevation = elevation;
        v22 = 1;
    } else {
        if (elevation == a1->elevation) {
            if (a5 != nullptr) {
                objectGetRect(a1, a5);
            }
        } else {
            if (objectGetListNode(a1, &node, &previousNode) == -1) {
                return -1;
            }

            if (a5 != nullptr) {
                objectGetRect(a1, a5);
            }

            if (previousNode != nullptr) {
                previousNode->next = node->next;
            } else {
                int tile = node->obj->tile;
                if (tile != -1) {
                    gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
                } else {
                    gObjectListHead = gObjectListHead->next;
                }
            }

            a1->elevation = elevation;
            v22 = 1;
        }
    }

    CacheEntry* cacheHandle;
    int width;
    int height;
    Art* art = artLock(a1->fid, &cacheHandle);
    if (art != nullptr) {
        artGetSize(art, a1->frame, a1->rotation, &width, &height);
        a1->sx = a2 - width / 2;
        a1->sy = a3 - (height - 1);
        artUnlock(cacheHandle);
    }

    if (v22) {
        _obj_insert(node);
    }

    if (a5 != nullptr) {
        Rect rect;
        objectGetRect(a1, &rect);
        rectUnion(a5, &rect, a5);
    }

    if (a1 == gDude) {
        if (a1 != nullptr) {
            Rect rect;
            _obj_move(gEgg, a2, a3, elevation, &rect);
            rectUnion(a5, &rect, a5);
        } else {
            _obj_move(gEgg, a2, a3, elevation, nullptr);
        }
    }

    return 0;
}

// 0x48A568
int objectSetLocation(Object* obj, int tile, int elevation, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if (!hexGridTileIsValid(tile)) {
        return -1;
    }

    if (!elevationIsValid(elevation)) {
        return -1;
    }

    ObjectListNode* node;
    ObjectListNode* prevNode;
    if (objectGetListNode(obj, &node, &prevNode) == -1) {
        return -1;
    }

    Rect v23;
    int v5 = _obj_adjust_light(obj, 1, rect);
    if (rect != nullptr) {
        if (v5 == -1) {
            objectGetRect(obj, rect);
        }

        rectCopy(&v23, rect);
    }

    int oldElevation = obj->elevation;
    int oldTile = obj->tile;
    if (prevNode != nullptr) {
        prevNode->next = node->next;
    } else {
        int tileIndex = node->obj->tile;
        if (tileIndex == -1) {
            gObjectListHead = gObjectListHead->next;
        } else {
            gObjectListHeadByTile[tileIndex] = gObjectListHeadByTile[tileIndex]->next;
        }
    }

    if (_obj_connect_to_tile(node, tile, elevation, rect) == -1) {
        return -1;
    }

    // MP_PROTOCOL.md §4 move: tile/elevation is now committed. The per-tile
    // stream is coalesced into a path event downstream; suppressed during map
    // load by the consumer (dude placement moves through here). Pixel-offset
    // interpolation (_obj_offset) is client-derived, not synced.
    //
    // During a script game-time skip the per-tile stream is COALESCED instead:
    // the window records this object's origin once and ships a single snap to
    // its final tile when the skip ends (presenter.h TIME-SKIP MOVE COALESCING).
    if (presenterTimeSkipActive()) {
        presenterTimeSkipRecordMove(obj, oldTile, oldElevation);
    } else {
        presenter()->objectMoved(obj, oldTile, oldElevation, tile, elevation);
    }

    // The wire viewer mirrors gCombatState so the vanilla combat HUD lights up,
    // but a decode-driven move must never mutate a synced object. This LOS outline
    // rewrite is such a mutation (v1 defers target outlines entirely —
    // COMBAT_CLIENT_DESIGN.md §5.1/§5.5), so it is guarded off on the viewer.
    if (isInCombat() && !clientViewerActive()) {
        if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
            bool v8 = obj->outline != 0 && (obj->outline & OUTLINE_DISABLED) == 0;
            _combat_update_critter_outline_for_los(obj, v8);
        }
    }

    if (rect != nullptr) {
        rectUnion(rect, &v23, rect);
    }

    if (obj == gDude) {
        ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
        while (objectListNode != nullptr) {
            Object* obj = objectListNode->obj;
            int elev = obj->elevation;
            if (elevation < elev) {
                break;
            }

            if (elevation == elev) {
                if (FID_TYPE(obj->fid) == OBJ_TYPE_MISC) {
                    if (isExitGridPid(obj->pid)) {
                        ObjectData* data = &(obj->data);

                        MapTransition transition;
                        memset(&transition, 0, sizeof(transition));

                        transition.map = data->misc.map;
                        transition.tile = data->misc.tile;
                        transition.elevation = data->misc.elevation;
                        transition.rotation = data->misc.rotation;
                        mapSetTransition(&transition);

                        wmMapMarkMapEntranceState(transition.map, transition.elevation, 1);
                    }
                }
            }

            objectListNode = objectListNode->next;
        }

        // NOTE: Uninline.
        obj_set_seen(tile);

        int roofX = tile % 200 / 2;
        int roofY = tile / 200 / 2;
        if (roofX != _obj_last_roof_x || roofY != _obj_last_roof_y || elevation != _obj_last_elev) {
            int currentSquare = _square[elevation]->field_0[roofX + 100 * roofY];
            int currentSquareFid = buildFid(OBJ_TYPE_TILE, (currentSquare >> 16) & 0xFFF, 0, 0, 0);
            // CE: Add additional checks for -1 to prevent array lookup at index -101.
            int previousSquare = _obj_last_roof_x != -1 && _obj_last_roof_y != -1
                ? _square[elevation]->field_0[_obj_last_roof_x + 100 * _obj_last_roof_y]
                : 0;
            bool isEmpty = buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0) == currentSquareFid;

            if (isEmpty != _obj_last_is_empty || (((currentSquare >> 16) & 0xF000) >> 12) != (((previousSquare >> 16) & 0xF000) >> 12)) {
                if (!_obj_last_is_empty) {
                    tile_fill_roof(_obj_last_roof_x, _obj_last_roof_y, elevation, true);
                }

                if (!isEmpty) {
                    tile_fill_roof(roofX, roofY, elevation, false);
                }

                if (rect != nullptr) {
                    rectUnion(rect, &_scr_size, rect);
                }
            }

            _obj_last_roof_x = roofX;
            _obj_last_roof_y = roofY;
            _obj_last_elev = elevation;
            _obj_last_is_empty = isEmpty;
        }

        if (rect != nullptr) {
            Rect r;
            objectSetLocation(gEgg, tile, elevation, &r);
            rectUnion(rect, &r, rect);
        } else {
            objectSetLocation(gEgg, tile, elevation, nullptr);
        }

        if (elevation != oldElevation) {
            // SFALL: Remove text floaters after moving to another elevation.
            textObjectsReset();

            mapSetElevation(elevation);
            tileSetCenter(tile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
            // On the wire viewer gCombatState is a mirror, not a real local fight;
            // a wire-driven dude elevation change must not trip vanilla's "changed
            // level mid-combat -> quit", which would silently kill mainClientViewer
            // (COMBAT_CLIENT_DESIGN.md §5.1).
            if (isInCombat() && !clientViewerActive()) {
                _game_user_wants_to_quit = 1;
            }
        }
    } else {
        if (elevation != _obj_last_elev && PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
            _combat_delete_critter(obj);
        }
    }

    return 0;
}

// 0x48A9A0
int _obj_reset_roof()
{
    int fid = buildFid(OBJ_TYPE_TILE, (_square[gDude->elevation]->field_0[_obj_last_roof_x + 100 * _obj_last_roof_y] >> 16) & 0xFFF, 0, 0, 0);
    if (fid != buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0)) {
        tile_fill_roof(_obj_last_roof_x, _obj_last_roof_y, gDude->elevation, 1);
    }
    return 0;
}

// Sets object fid.
//
// 0x48AA3C
int objectSetFid(Object* obj, int fid, Rect* dirtyRect)
{
    Rect new_rect;

    if (obj == nullptr) {
        return -1;
    }

    if (dirtyRect != nullptr) {
        objectGetRect(obj, dirtyRect);

        obj->fid = fid;

        objectGetRect(obj, &new_rect);
        rectUnion(dirtyRect, &new_rect, dirtyRect);
    } else {
        obj->fid = fid;
    }

    return 0;
}

// Sets object frame.
//
// 0x48AA84
int objectSetFrame(Object* obj, int frame, Rect* rect)
{
    Rect new_rect;
    Art* art;
    CacheEntry* cache_entry;
    int framesPerDirection;

    if (obj == nullptr) {
        return -1;
    }

    art = artLock(obj->fid, &cache_entry);
    if (art == nullptr) {
        return -1;
    }

    framesPerDirection = art->frameCount;

    artUnlock(cache_entry);

    if (frame >= framesPerDirection) {
        return -1;
    }

    if (rect != nullptr) {
        objectGetRect(obj, rect);
        obj->frame = frame;
        objectGetRect(obj, &new_rect);
        rectUnion(rect, &new_rect, rect);
    } else {
        obj->frame = frame;
    }

    return 0;
}

// 0x48AAF0
int objectSetNextFrame(Object* obj, Rect* dirtyRect)
{
    Art* art;
    CacheEntry* cache_entry;
    int framesPerDirection;
    int nextFrame;

    if (obj == nullptr) {
        return -1;
    }

    art = artLock(obj->fid, &cache_entry);
    if (art == nullptr) {
        return -1;
    }

    framesPerDirection = art->frameCount;

    artUnlock(cache_entry);

    nextFrame = obj->frame + 1;
    if (nextFrame >= framesPerDirection) {
        nextFrame = 0;
    }

    if (dirtyRect != nullptr) {

        objectGetRect(obj, dirtyRect);

        obj->frame = nextFrame;

        Rect updatedRect;
        objectGetRect(obj, &updatedRect);
        rectUnion(dirtyRect, &updatedRect, dirtyRect);
    } else {
        obj->frame = nextFrame;
    }

    return 0;
}

// 0x48AB60
//
int objectSetPrevFrame(Object* obj, Rect* dirtyRect)
{
    Art* art;
    CacheEntry* cache_entry;
    int framesPerDirection;
    int prevFrame;
    Rect newRect;

    if (obj == nullptr) {
        return -1;
    }

    art = artLock(obj->fid, &cache_entry);
    if (art == nullptr) {
        return -1;
    }

    framesPerDirection = art->frameCount;

    artUnlock(cache_entry);

    prevFrame = obj->frame - 1;
    if (prevFrame < 0) {
        prevFrame = framesPerDirection - 1;
    }

    if (dirtyRect != nullptr) {
        objectGetRect(obj, dirtyRect);
        obj->frame = prevFrame;
        objectGetRect(obj, &newRect);
        rectUnion(dirtyRect, &newRect, dirtyRect);
    } else {
        obj->frame = prevFrame;
    }

    return 0;
}

// 0x48ABD4
int objectSetRotation(Object* obj, int direction, Rect* dirtyRect)
{
    if (obj == nullptr) {
        return -1;
    }

    if (direction >= ROTATION_COUNT) {
        return -1;
    }

    if (dirtyRect != nullptr) {
        objectGetRect(obj, dirtyRect);
        obj->rotation = direction;

        Rect newRect;
        objectGetRect(obj, &newRect);
        rectUnion(dirtyRect, &newRect, dirtyRect);
    } else {
        obj->rotation = direction;
    }

    return 0;
}

// 0x48AC20
int objectRotateClockwise(Object* obj, Rect* dirtyRect)
{
    int rotation = obj->rotation + 1;
    if (rotation >= ROTATION_COUNT) {
        rotation = ROTATION_NE;
    }

    return objectSetRotation(obj, rotation, dirtyRect);
}

// 0x48AC38
int objectRotateCounterClockwise(Object* obj, Rect* dirtyRect)
{
    int rotation = obj->rotation - 1;
    if (rotation < 0) {
        rotation = ROTATION_NW;
    }

    return objectSetRotation(obj, rotation, dirtyRect);
}

// 0x48AC54
void _obj_rebuild_all_light()
{
    lightResetTileIntensity();

    for (int tile = 0; tile < HEX_GRID_SIZE; tile++) {
        ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
        while (objectListNode != nullptr) {
            _obj_adjust_light(objectListNode->obj, 0, nullptr);
            objectListNode = objectListNode->next;
        }
    }
}

// 0x48AC90
int objectSetLight(Object* obj, int lightDistance, int lightIntensity, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    int rc = _obj_turn_off_light(obj, rect);
    if (lightIntensity > 0) {
        obj->lightDistance = std::min(lightDistance, 8);
        obj->lightIntensity = lightIntensity;

        if (rect != nullptr) {
            Rect tempRect;
            rc = _obj_turn_on_light(obj, &tempRect);
            rectUnion(rect, &tempRect, rect);
        } else {
            rc = _obj_turn_on_light(obj, nullptr);
        }
    } else {
        obj->lightIntensity = 0;
        obj->lightDistance = 0;
    }

    return rc;
}

// 0x48AD04
int objectGetLightIntensity(Object* obj)
{
    int ambientIntensity = lightGetAmbientIntensity();
    int tileIntensity = lightGetTrueTileIntensity(obj->elevation, obj->tile);

    if (obj == gDude) {
        tileIntensity -= gDude->lightIntensity;
    }

    if (tileIntensity >= ambientIntensity) {
        if (tileIntensity > LIGHT_INTENSITY_MAX) {
            tileIntensity = LIGHT_INTENSITY_MAX;
        }
    } else {
        tileIntensity = ambientIntensity;
    }

    return tileIntensity;
}

// 0x48AD48
int _obj_turn_on_light(Object* obj, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if (obj->lightIntensity <= 0) {
        obj->flags &= ~OBJECT_LIGHTING;
        return -1;
    }

    if ((obj->flags & OBJECT_LIGHTING) == 0) {
        obj->flags |= OBJECT_LIGHTING;

        if (_obj_adjust_light(obj, 0, rect) == -1) {
            if (rect != nullptr) {
                objectGetRect(obj, rect);
            }
        }
    }

    return 0;
}

// 0x48AD9C
int _obj_turn_off_light(Object* obj, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if (obj->lightIntensity <= 0) {
        obj->flags &= ~OBJECT_LIGHTING;
        return -1;
    }

    if ((obj->flags & OBJECT_LIGHTING) != 0) {
        if (_obj_adjust_light(obj, 1, rect) == -1) {
            if (rect != nullptr) {
                objectGetRect(obj, rect);
            }
        }

        obj->flags &= ~OBJECT_LIGHTING;
    }

    return 0;
}

// 0x48ADF0
int objectShow(Object* obj, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if ((obj->flags & OBJECT_HIDDEN) == 0) {
        return -1;
    }

    obj->flags &= ~OBJECT_HIDDEN;
    obj->outline &= ~OUTLINE_DISABLED;

    if (_obj_adjust_light(obj, 0, rect) == -1) {
        if (rect != nullptr) {
            objectGetRect(obj, rect);
        }
    }

    if (obj == gDude) {
        if (rect != nullptr) {
            Rect eggRect;
            objectGetRect(gEgg, &eggRect);
            rectUnion(rect, &eggRect, rect);
        }
    }

    return 0;
}

// 0x48AE68
int objectHide(Object* object, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    if ((object->flags & OBJECT_HIDDEN) != 0) {
        return -1;
    }

    if (_obj_adjust_light(object, 1, rect) == -1) {
        if (rect != nullptr) {
            objectGetRect(object, rect);
        }
    }

    object->flags |= OBJECT_HIDDEN;

    if ((object->outline & OUTLINE_TYPE_MASK) != 0) {
        object->outline |= OUTLINE_DISABLED;
    }

    if (object == gDude) {
        if (rect != nullptr) {
            Rect eggRect;
            objectGetRect(gEgg, &eggRect);
            rectUnion(rect, &eggRect, rect);
        }
    }

    return 0;
}

// 0x48AEE4
int objectEnableOutline(Object* object, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    object->outline &= ~OUTLINE_DISABLED;

    if (rect != nullptr) {
        objectGetRect(object, rect);
    }

    return 0;
}

// 0x48AF00
int objectDisableOutline(Object* object, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    if ((object->outline & OUTLINE_TYPE_MASK) != 0) {
        object->outline |= OUTLINE_DISABLED;
    }

    if (rect != nullptr) {
        objectGetRect(object, rect);
    }

    return 0;
}

// 0x48AF2C
int _obj_toggle_flat(Object* object, Rect* rect)
{
    Rect v1;

    if (object == nullptr) {
        return -1;
    }

    ObjectListNode* node;
    ObjectListNode* previousNode;
    if (objectGetListNode(object, &node, &previousNode) == -1) {
        return -1;
    }

    if (rect != nullptr) {
        objectGetRect(object, rect);

        if (previousNode != nullptr) {
            previousNode->next = node->next;
        } else {
            int tile_index = node->obj->tile;
            if (tile_index == -1) {
                gObjectListHead = gObjectListHead->next;
            } else {
                gObjectListHeadByTile[tile_index] = gObjectListHeadByTile[tile_index]->next;
            }
        }

        object->flags ^= OBJECT_FLAT;

        _obj_insert(node);
        objectGetRect(object, &v1);
        rectUnion(rect, &v1, rect);
    } else {
        if (previousNode != nullptr) {
            previousNode->next = node->next;
        } else {
            int tile = node->obj->tile;
            if (tile == -1) {
                gObjectListHead = gObjectListHead->next;
            } else {
                gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
            }
        }

        object->flags ^= OBJECT_FLAT;

        _obj_insert(node);
    }

    return 0;
}

// 0x48B0FC
int objectDestroy(Object* object, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    // MP_PROTOCOL.md §4 destroy: emit before the free so obj->id is readable.
    // Map teardown (_obj_remove_all) bypasses objectDestroy, so this fires only
    // for gameplay destroys, not the bulk world-clear on map change.
    presenter()->objectDestroyed(object);

    // A time-skip window may be holding this object's pre-skip origin; the
    // pointer is about to become unreadable.
    presenterTimeSkipForget(object);

    _gmouse_remove_item_outline(object);

    ObjectListNode* node;
    ObjectListNode* previousNode;
    if (objectGetListNode(object, &node, &previousNode) == 0) {
        if (_obj_adjust_light(object, 1, rect) == -1) {
            if (rect != nullptr) {
                objectGetRect(object, rect);
            }
        }

        if (_obj_remove(node, previousNode) != 0) {
            return -1;
        }

        return 0;
    }

    // NOTE: Uninline.
    if (objectListNodeCreate(&node) == -1) {
        return -1;
    }

    node->obj = object;

    if (_obj_remove(node, node) == -1) {
        return -1;
    }

    return 0;
}

// 0x48B1B0
int _obj_inven_free(Inventory* inventory)
{
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);

        ObjectListNode* node;
        // NOTE: Uninline.
        objectListNodeCreate(&node);

        node->obj = inventoryItem->item;
        node->obj->flags &= ~OBJECT_NO_REMOVE;
        _obj_remove(node, node);

        inventoryItem->item = nullptr;
    }

    if (inventory->items != nullptr) {
        internal_free(inventory->items);
        inventory->items = nullptr;
        inventory->capacity = 0;
        inventory->length = 0;
    }

    return 0;
}

// 0x48B24C
bool _obj_action_can_use(Object* obj)
{
    int pid = obj->pid;
    // SFALL
    if (pid != PROTO_ID_LIT_FLARE && !explosiveIsActiveExplosive(pid)) {
        return _proto_action_can_use(pid);
    } else {
        return false;
    }
}

// 0x48B278
bool _obj_action_can_talk_to(Object* obj)
{
    return _proto_action_can_talk_to(obj->pid) && (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) && critterIsActive(obj);
}

// 0x48B2A8
bool _obj_portal_is_walk_thru(Object* obj)
{
    if (PID_TYPE(obj->pid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1) {
        return false;
    }

    return (proto->scenery.data.generic.field_0 & 0x04) != 0;
}

// 0x48B2E8
Object* objectFindById(int a1)
{
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (obj->id == a1) {
            return obj;
        }
        obj = objectFindNext();
    }

    return nullptr;
}

// Resolve a wire netId to its live object, or nullptr. netId is the wire's UNIQUE
// object handle (obj->id is ~53% non-unique — MP_PROTOCOL §7 — so the network
// command path must address by netId, never id). netId 0 means "outside the
// syncable domain" (see object.cc netId assignment); a 0 arg never matches a real
// object here. Used by the server control plane to look up a clicked target
// (server_control.cc) — the trust-boundary lookup for interaction verbs.
Object* objectFindByNetId(int netId)
{
    if (netId <= 0) {
        return nullptr;
    }
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (obj->netId == netId) {
            return obj;
        }
        obj = objectFindNext();
    }

    return nullptr;
}

// Returns root owner of given object.
//
// 0x48B304
Object* objectGetOwner(Object* object)
{
    Object* owner = object->owner;
    if (owner == nullptr) {
        return nullptr;
    }

    while (owner->owner != nullptr) {
        owner = owner->owner;
    }

    return owner;
}

// 0x48B318
void _obj_remove_all()
{
    ObjectListNode* node;
    ObjectListNode* prev;
    ObjectListNode* next;

    // Bulk world-clear bypasses objectDestroy, so nothing else drops the
    // time-skip window's held pointers. Every one of them dies here.
    presenterTimeSkipForgetAll();

    _scr_remove_all();

    for (int tile = 0; tile < HEX_GRID_SIZE; tile++) {
        node = gObjectListHeadByTile[tile];
        prev = nullptr;

        while (node != nullptr) {
            next = node->next;
            if (_obj_remove(node, prev) == -1) {
                prev = node;
            }
            node = next;
        }
    }

    node = gObjectListHead;
    prev = nullptr;

    while (node != nullptr) {
        next = node->next;
        if (_obj_remove(node, prev) == -1) {
            prev = node;
        }
        node = next;
    }

    _obj_last_roof_y = -1;
    _obj_last_elev = -1;
    _obj_last_is_empty = true;
    _obj_last_roof_x = -1;
}

// 0x48B3A8
Object* objectFindFirst()
{
    gObjectFindElevation = 0;

    for (gObjectFindTile = 0; gObjectFindTile < HEX_GRID_SIZE; gObjectFindTile++) {
        ObjectListNode* objectListNode = gObjectListHeadByTile[gObjectFindTile];
        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                gObjectFindLastObjectListNode = objectListNode;
                return object;
            }
            objectListNode = objectListNode->next;
        }
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x48B41C
Object* objectFindNext()
{
    if (gObjectFindLastObjectListNode == nullptr) {
        return nullptr;
    }

    ObjectListNode* objectListNode = gObjectFindLastObjectListNode->next;

    while (true) {
        if (objectListNode == nullptr) {
            gObjectFindTile++;
            if (gObjectFindTile >= HEX_GRID_SIZE) {
                break;
            }

            objectListNode = gObjectListHeadByTile[gObjectFindTile];
        }

        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                gObjectFindLastObjectListNode = objectListNode;
                return object;
            }
            objectListNode = objectListNode->next;
        }
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x48B48C
Object* objectFindFirstAtElevation(int elevation)
{
    gObjectFindElevation = elevation;
    gObjectFindTile = 0;

    for (gObjectFindTile = 0; gObjectFindTile < HEX_GRID_SIZE; gObjectFindTile++) {
        ObjectListNode* objectListNode = gObjectListHeadByTile[gObjectFindTile];
        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if (object->elevation == elevation) {
                if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                    gObjectFindLastObjectListNode = objectListNode;
                    return object;
                }
            }
            objectListNode = objectListNode->next;
        }
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x48B510
Object* objectFindNextAtElevation()
{
    if (gObjectFindLastObjectListNode == nullptr) {
        return nullptr;
    }

    ObjectListNode* objectListNode = gObjectFindLastObjectListNode->next;

    while (true) {
        if (objectListNode == nullptr) {
            gObjectFindTile++;
            if (gObjectFindTile >= HEX_GRID_SIZE) {
                break;
            }

            objectListNode = gObjectListHeadByTile[gObjectFindTile];
        }

        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if (object->elevation == gObjectFindElevation) {
                if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                    gObjectFindLastObjectListNode = objectListNode;
                    return object;
                }
            }
            objectListNode = objectListNode->next;
        }
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x48B5A8
Object* objectFindFirstAtLocation(int elevation, int tile)
{
    gObjectFindElevation = elevation;
    gObjectFindTile = tile;

    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        Object* object = objectListNode->obj;
        if (object->elevation == elevation) {
            if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                gObjectFindLastObjectListNode = objectListNode;
                return object;
            }
        }
        objectListNode = objectListNode->next;
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x48B608
Object* objectFindNextAtLocation()
{
    if (gObjectFindLastObjectListNode == nullptr) {
        return nullptr;
    }

    ObjectListNode* objectListNode = gObjectFindLastObjectListNode->next;

    while (objectListNode != nullptr) {
        Object* object = objectListNode->obj;
        if (object->elevation == gObjectFindElevation) {
            if (!artIsObjectTypeHidden(FID_TYPE(object->fid))) {
                gObjectFindLastObjectListNode = objectListNode;
                return object;
            }
        }
        objectListNode = objectListNode->next;
    }

    gObjectFindLastObjectListNode = nullptr;
    return nullptr;
}

// 0x0x48B66C
void objectGetRect(Object* obj, Rect* rect)
{
    if (obj == nullptr) {
        return;
    }

    if (rect == nullptr) {
        return;
    }

    bool isOutlined = false;
    if ((obj->outline & OUTLINE_TYPE_MASK) != 0) {
        isOutlined = true;
    }

    CacheEntry* artHandle;
    Art* art = artLock(obj->fid, &artHandle);
    if (art == nullptr) {
        rect->left = 0;
        rect->top = 0;
        rect->right = 0;
        rect->bottom = 0;
        return;
    }

    int width;
    int height;
    artGetSize(art, obj->frame, obj->rotation, &width, &height);

    if (obj->tile == -1) {
        rect->left = obj->sx;
        rect->top = obj->sy;
        rect->right = obj->sx + width - 1;
        rect->bottom = obj->sy + height - 1;
    } else {
        int tileScreenY;
        int tileScreenX;
        if (tileToScreenXY(obj->tile, &tileScreenX, &tileScreenY, obj->elevation) == 0) {
            tileScreenX += 16;
            tileScreenY += 8;

            tileScreenX += art->xOffsets[obj->rotation];
            tileScreenY += art->yOffsets[obj->rotation];

            tileScreenX += obj->x;
            tileScreenY += obj->y;

            rect->left = tileScreenX - width / 2;
            rect->top = tileScreenY - height + 1;
            rect->right = width + rect->left - 1;
            rect->bottom = tileScreenY;
        } else {
            rect->left = 0;
            rect->top = 0;
            rect->right = 0;
            rect->bottom = 0;
            isOutlined = false;
        }
    }

    artUnlock(artHandle);

    if (isOutlined) {
        rect->left--;
        rect->top--;
        rect->right++;
        rect->bottom++;
    }
}

// 0x48B7F8
bool _obj_occupied(int tile, int elevation)
{
    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        if (objectListNode->obj->elevation == elevation
            && objectListNode->obj != gGameMouseBouncingCursor
            && objectListNode->obj != gGameMouseHexCursor) {
            return true;
        }
        objectListNode = objectListNode->next;
    }

    return false;
}

// 0x48B848
Object* _obj_blocking_at(Object* excludeObj, int tile, int elev)
{
    ObjectListNode* objectListNode;
    Object* obj;
    int type;

    if (!hexGridTileIsValid(tile)) {
        return nullptr;
    }

    objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        obj = objectListNode->obj;
        if (obj->elevation == elev) {
            if ((obj->flags & OBJECT_HIDDEN) == 0 && (obj->flags & OBJECT_NO_BLOCK) == 0 && obj != excludeObj) {
                type = FID_TYPE(obj->fid);
                if (type == OBJ_TYPE_CRITTER
                    || type == OBJ_TYPE_SCENERY
                    || type == OBJ_TYPE_WALL) {
                    return obj;
                }
            }
        }
        objectListNode = objectListNode->next;
    }

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int neighboor = tileGetTileInDirection(tile, rotation, 1);
        if (hexGridTileIsValid(neighboor)) {
            objectListNode = gObjectListHeadByTile[neighboor];
            while (objectListNode != nullptr) {
                obj = objectListNode->obj;
                if ((obj->flags & OBJECT_MULTIHEX) != 0) {
                    if (obj->elevation == elev) {
                        if ((obj->flags & OBJECT_HIDDEN) == 0 && (obj->flags & OBJECT_NO_BLOCK) == 0 && obj != excludeObj) {
                            type = FID_TYPE(obj->fid);
                            if (type == OBJ_TYPE_CRITTER
                                || type == OBJ_TYPE_SCENERY
                                || type == OBJ_TYPE_WALL) {
                                return obj;
                            }
                        }
                    }
                }
                objectListNode = objectListNode->next;
            }
        }
    }

    return nullptr;
}

// 0x48B930
Object* _obj_shoot_blocking_at(Object* excludeObj, int tile, int elev)
{
    if (!hexGridTileIsValid(tile)) {
        return nullptr;
    }

    ObjectListNode* objectListItem = gObjectListHeadByTile[tile];
    while (objectListItem != nullptr) {
        Object* candidate = objectListItem->obj;
        if (candidate->elevation == elev) {
            unsigned int flags = candidate->flags;
            if ((flags & OBJECT_HIDDEN) == 0 && ((flags & OBJECT_NO_BLOCK) == 0 || (flags & OBJECT_SHOOT_THRU) == 0) && candidate != excludeObj) {
                int type = FID_TYPE(candidate->fid);
                // SFALL: Fix to prevent corpses from blocking line of fire.
                if ((type == OBJ_TYPE_CRITTER && !critterIsDead(candidate))
                    || type == OBJ_TYPE_SCENERY
                    || type == OBJ_TYPE_WALL) {
                    return candidate;
                }
            }
        }
        objectListItem = objectListItem->next;
    }

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int adjacentTile = tileGetTileInDirection(tile, rotation, 1);
        if (!hexGridTileIsValid(adjacentTile)) {
            continue;
        }

        ObjectListNode* objectListItem = gObjectListHeadByTile[adjacentTile];
        while (objectListItem != nullptr) {
            Object* candidate = objectListItem->obj;
            unsigned int flags = candidate->flags;
            if ((flags & OBJECT_MULTIHEX) != 0) {
                if (candidate->elevation == elev) {
                    if ((flags & OBJECT_HIDDEN) == 0 && (flags & OBJECT_NO_BLOCK) == 0 && candidate != excludeObj) {
                        int type = FID_TYPE(candidate->fid);
                        // SFALL: Fix to prevent corpses from blocking line of
                        // fire.
                        if ((type == OBJ_TYPE_CRITTER && !critterIsDead(candidate))
                            || type == OBJ_TYPE_SCENERY
                            || type == OBJ_TYPE_WALL) {
                            return candidate;
                        }
                    }
                }
            }
            objectListItem = objectListItem->next;
        }
    }

    return nullptr;
}

// 0x48BA20
Object* _obj_ai_blocking_at(Object* excludeObj, int tile, int elevation)
{
    if (!hexGridTileIsValid(tile)) {
        return nullptr;
    }

    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        Object* object = objectListNode->obj;
        if (object->elevation == elevation) {
            if ((object->flags & OBJECT_HIDDEN) == 0
                && (object->flags & OBJECT_NO_BLOCK) == 0
                && object != excludeObj) {
                int objectType = FID_TYPE(object->fid);
                if (objectType == OBJ_TYPE_CRITTER
                    || objectType == OBJ_TYPE_SCENERY
                    || objectType == OBJ_TYPE_WALL) {
                    if (_moveBlockObj != nullptr || objectType != OBJ_TYPE_CRITTER) {
                        return object;
                    }

                    _moveBlockObj = object;
                }
            }
        }
        objectListNode = objectListNode->next;
    }

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int candidate = tileGetTileInDirection(tile, rotation, 1);
        if (!hexGridTileIsValid(candidate)) {
            continue;
        }

        objectListNode = gObjectListHeadByTile[candidate];
        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if ((object->flags & OBJECT_MULTIHEX) != 0) {
                if (object->elevation == elevation) {
                    if ((object->flags & OBJECT_HIDDEN) == 0
                        && (object->flags & OBJECT_NO_BLOCK) == 0
                        && object != excludeObj) {
                        int objectType = FID_TYPE(object->fid);
                        if (objectType == OBJ_TYPE_CRITTER
                            || objectType == OBJ_TYPE_SCENERY
                            || objectType == OBJ_TYPE_WALL) {
                            if (_moveBlockObj != nullptr || objectType != OBJ_TYPE_CRITTER) {
                                return object;
                            }

                            _moveBlockObj = object;
                        }
                    }
                }
            }
            objectListNode = objectListNode->next;
        }
    }

    return nullptr;
}

// 0x48BB44
int _obj_scroll_blocking_at(int tile, int elev)
{
    // TODO: Might be an error - why tile 0 is excluded?
    if (tile <= 0 || tile >= 40000) {
        return -1;
    }

    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        if (elev < objectListNode->obj->elevation) {
            break;
        }

        if (objectListNode->obj->elevation == elev && objectListNode->obj->pid == 0x500000C) {
            return 0;
        }

        objectListNode = objectListNode->next;
    }

    return -1;
}

// 0x48BB88
Object* _obj_sight_blocking_at(Object* excludeObj, int tile, int elevation)
{
    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        Object* object = objectListNode->obj;
        if (object->elevation == elevation
            && (object->flags & OBJECT_HIDDEN) == 0
            && (object->flags & OBJECT_LIGHT_THRU) == 0
            && object != excludeObj) {
            int objectType = FID_TYPE(object->fid);
            if (objectType == OBJ_TYPE_SCENERY || objectType == OBJ_TYPE_WALL) {
                return object;
            }
        }
        objectListNode = objectListNode->next;
    }

    return nullptr;
}

// 0x48BBD4
int objectGetDistanceBetween(Object* object1, Object* object2)
{
    if (object1 == nullptr || object2 == nullptr) {
        return 0;
    }

    int distance = tileDistanceBetween(object1->tile, object2->tile);

    if ((object1->flags & OBJECT_MULTIHEX) != 0) {
        distance -= 1;
    }

    if ((object2->flags & OBJECT_MULTIHEX) != 0) {
        distance -= 1;
    }

    if (distance < 0) {
        distance = 0;
    }

    return distance;
}

// 0x48BC08
int objectGetDistanceBetweenTiles(Object* object1, int tile1, Object* object2, int tile2)
{
    if (object1 == nullptr || object2 == nullptr) {
        return 0;
    }

    int distance = tileDistanceBetween(tile1, tile2);

    if ((object1->flags & OBJECT_MULTIHEX) != 0) {
        distance -= 1;
    }

    if ((object2->flags & OBJECT_MULTIHEX) != 0) {
        distance -= 1;
    }

    if (distance < 0) {
        distance = 0;
    }

    return distance;
}

// 0x48BC38
int objectListCreate(int tile, int elevation, int objectType, Object*** objectListPtr)
{
    if (objectListPtr == nullptr) {
        return -1;
    }

    int count = 0;
    if (tile == -1) {
        for (int index = 0; index < HEX_GRID_SIZE; index++) {
            ObjectListNode* objectListNode = gObjectListHeadByTile[index];
            while (objectListNode != nullptr) {
                Object* obj = objectListNode->obj;
                if ((obj->flags & OBJECT_HIDDEN) == 0
                    && obj->elevation == elevation
                    && FID_TYPE(obj->fid) == objectType) {
                    count++;
                }
                objectListNode = objectListNode->next;
            }
        }
    } else {
        ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
        while (objectListNode != nullptr) {
            Object* obj = objectListNode->obj;
            if ((obj->flags & OBJECT_HIDDEN) == 0
                && obj->elevation == elevation
                && FID_TYPE(objectListNode->obj->fid) == objectType) {
                count++;
            }
            objectListNode = objectListNode->next;
        }
    }

    if (count == 0) {
        return 0;
    }

    Object** objects = *objectListPtr = (Object**)internal_malloc(sizeof(*objects) * count);
    if (objects == nullptr) {
        return -1;
    }

    if (tile == -1) {
        for (int index = 0; index < HEX_GRID_SIZE; index++) {
            ObjectListNode* objectListNode = gObjectListHeadByTile[index];
            while (objectListNode) {
                Object* obj = objectListNode->obj;
                if ((obj->flags & OBJECT_HIDDEN) == 0
                    && obj->elevation == elevation
                    && FID_TYPE(obj->fid) == objectType) {
                    *objects++ = obj;
                }
                objectListNode = objectListNode->next;
            }
        }
    } else {
        ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
        while (objectListNode != nullptr) {
            Object* obj = objectListNode->obj;
            if ((obj->flags & OBJECT_HIDDEN) == 0
                && obj->elevation == elevation
                && FID_TYPE(obj->fid) == objectType) {
                *objects++ = obj;
            }
            objectListNode = objectListNode->next;
        }
    }

    return count;
}

// 0x48BDCC
void objectListFree(Object** objectList)
{
    if (objectList != nullptr) {
        internal_free(objectList);
    }
}


// 0x48C2B4
int objectSetOutline(Object* obj, int outlineType, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if ((obj->outline & OUTLINE_TYPE_MASK) != 0) {
        return -1;
    }

    if ((obj->flags & OBJECT_NO_HIGHLIGHT) != 0) {
        return -1;
    }

    obj->outline = outlineType;

    if ((obj->flags & OBJECT_HIDDEN) != 0) {
        obj->outline |= OUTLINE_DISABLED;
    }

    if (rect != nullptr) {
        objectGetRect(obj, rect);
    }

    return 0;
}

// 0x48C2F0
int objectClearOutline(Object* object, Rect* rect)
{
    if (object == nullptr) {
        return -1;
    }

    if (rect != nullptr) {
        objectGetRect(object, rect);
    }

    object->outline = 0;

    return 0;
}

// 0x48C340
int _obj_intersects_with(Object* object, int x, int y)
{
    int flags = 0;

    if (object == gEgg || (object->flags & OBJECT_HIDDEN) == 0) {
        CacheEntry* handle;
        Art* art = artLock(object->fid, &handle);
        if (art != nullptr) {
            int width;
            int height;
            artGetSize(art, object->frame, object->rotation, &width, &height);

            int minX;
            int minY;
            int maxX;
            int maxY;
            if (object->tile == -1) {
                minX = object->sx;
                minY = object->sy;
                maxX = minX + width - 1;
                maxY = minY + height - 1;
            } else {
                int tileScreenX;
                int tileScreenY;
                tileToScreenXY(object->tile, &tileScreenX, &tileScreenY, object->elevation);
                tileScreenX += 16;
                tileScreenY += 8;

                tileScreenX += art->xOffsets[object->rotation];
                tileScreenY += art->yOffsets[object->rotation];

                tileScreenX += object->x;
                tileScreenY += object->y;

                minX = tileScreenX - width / 2;
                maxX = minX + width - 1;

                minY = tileScreenY - height + 1;
                maxY = tileScreenY;
            }

            if (x >= minX && x <= maxX && y >= minY && y <= maxY) {
                unsigned char* data = artGetFrameData(art, object->frame, object->rotation);
                if (data != nullptr) {
                    if (data[width * (y - minY) + x - minX] != 0) {
                        flags |= 0x01;

                        if ((object->flags & OBJECT_FLAG_0xFC000) != 0) {
                            if ((object->flags & OBJECT_TRANS_NONE) == 0) {
                                flags &= ~0x03;
                                flags |= 0x02;
                            }
                        } else {
                            int type = FID_TYPE(object->fid);
                            if (type == OBJ_TYPE_SCENERY || type == OBJ_TYPE_WALL) {
                                Proto* proto;
                                protoGetProto(object->pid, &proto);

                                bool v20;
                                int extendedFlags = proto->scenery.extendedFlags;
                                if ((extendedFlags & 0x8000000) != 0 || (extendedFlags & 0x80000000) != 0) {
                                    v20 = tileIsInFrontOf(object->tile, gDude->tile);
                                } else if ((extendedFlags & 0x10000000) != 0) {
                                    // NOTE: Original code uses bitwise or, but given the fact that these functions return
                                    // bools, logical or is more suitable.
                                    v20 = tileIsInFrontOf(object->tile, gDude->tile) || tileIsToRightOf(gDude->tile, object->tile);
                                } else if ((extendedFlags & 0x20000000) != 0) {
                                    v20 = tileIsInFrontOf(object->tile, gDude->tile) && tileIsToRightOf(gDude->tile, object->tile);
                                } else {
                                    v20 = tileIsToRightOf(gDude->tile, object->tile);
                                }

                                if (v20) {
                                    if (_obj_intersects_with(gEgg, x, y) != 0) {
                                        flags |= 0x04;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            artUnlock(handle);
        }
    }

    return flags;
}

// 0x48C5C4
int _obj_create_intersect_list(int x, int y, int elevation, int objectType, ObjectWithFlags** entriesPtr)
{
    int upperLeftTile = tileFromScreenXY(x - 320, y - 240, elevation, true);
    *entriesPtr = nullptr;

    if (gObjectsUpdateAreaHexSize <= 0) {
        return 0;
    }

    int count = 0;

    int parity = gCenterTile & 1;
    for (int index = 0; index < gObjectsUpdateAreaHexSize; index++) {
        int offsetIndex = _orderTable[parity][index];
        if (_offsetDivTable[offsetIndex] < 30 && _offsetModTable[offsetIndex] < 20) {
            int tile = _offsetTable[parity][offsetIndex] + upperLeftTile;
            ObjectListNode* objectListNode = hexGridTileIsValid(tile)
                ? gObjectListHeadByTile[tile]
                : nullptr;
            while (objectListNode != nullptr) {
                Object* object = objectListNode->obj;
                if (object->elevation > elevation) {
                    break;
                }

                if (object->elevation == elevation
                    && (objectType == -1 || FID_TYPE(object->fid) == objectType)
                    && object != gEgg) {
                    int flags = _obj_intersects_with(object, x, y);
                    if (flags != 0) {
                        ObjectWithFlags* entries = (ObjectWithFlags*)internal_realloc(*entriesPtr, sizeof(*entries) * (count + 1));
                        if (entries != nullptr) {
                            *entriesPtr = entries;
                            entries[count].object = object;
                            entries[count].flags = flags;
                            count++;
                        }
                    }
                }

                objectListNode = objectListNode->next;
            }
        }
    }

    return count;
}

// 0x48C74C
void _obj_delete_intersect_list(ObjectWithFlags** entriesPtr)
{
    if (entriesPtr != nullptr && *entriesPtr != nullptr) {
        internal_free(*entriesPtr);
        *entriesPtr = nullptr;
    }
}

// NOTE: Inlined.
//
// 0x48C76C
void obj_set_seen(int tile)
{
    _obj_seen[tile >> 3] |= 1 << (tile & 7);
}

// 0x48C788
void _obj_clear_seen()
{
    memset(_obj_seen, 0, sizeof(_obj_seen));
}

// 0x48C7A0
void _obj_process_seen()
{
    int i;
    int v7;
    int v8;
    int v5;
    int v0;
    int v3;
    ObjectListNode* obj_entry;

    memset(_obj_seen_check, 0, 5001);

    v0 = 400;
    for (i = 0; i < 5001; i++) {
        if (_obj_seen[i] != 0) {
            for (v3 = i - 400; v3 != v0; v3 += 25) {
                if (v3 >= 0 && v3 < 5001) {
                    _obj_seen_check[v3] = -1;
                    if (v3 > 0) {
                        _obj_seen_check[v3 - 1] = -1;
                    }
                    if (v3 < 5000) {
                        _obj_seen_check[v3 + 1] = -1;
                    }
                    if (v3 > 1) {
                        _obj_seen_check[v3 - 2] = -1;
                    }
                    if (v3 < 4999) {
                        _obj_seen_check[v3 + 2] = -1;
                    }
                }
            }
        }
        v0++;
    }

    v7 = 0;
    for (i = 0; i < 5001; i++) {
        if (_obj_seen_check[i] != 0) {
            v8 = 1;
            for (v5 = v7; v5 < v7 + 8; v5++) {
                if (v8 & _obj_seen_check[i]) {
                    if (v5 < 40000) {
                        for (obj_entry = gObjectListHeadByTile[v5]; obj_entry != nullptr; obj_entry = obj_entry->next) {
                            if (obj_entry->obj->elevation == gDude->elevation) {
                                obj_entry->obj->flags |= OBJECT_SEEN;
                            }
                        }
                    }
                }
                v8 *= 2;
            }
        }
        v7 += 8;
    }

    memset(_obj_seen, 0, 5001);
}

// 0x48C8E4
char* objectGetName(Object* obj)
{
    int objectType = FID_TYPE(obj->fid);
    switch (objectType) {
    case OBJ_TYPE_ITEM:
        return itemGetName(obj);
    case OBJ_TYPE_CRITTER:
        return critterGetName(obj);
    default:
        return protoGetName(obj->pid);
    }
}

// 0x48C914
char* objectGetDescription(Object* obj)
{
    if (FID_TYPE(obj->fid) == OBJ_TYPE_ITEM) {
        return itemGetDescription(obj);
    }

    return protoGetDescription(obj->pid);
}

// Warm objects cache?
//
// 0x48C938
void _obj_preload_art_cache(int flags)
{
    if (gObjectFids == nullptr) {
        return;
    }

    unsigned char arr[4096];
    memset(arr, 0, sizeof(arr));

    if ((flags & 0x02) == 0) {
        for (int i = 0; i < SQUARE_GRID_SIZE; i++) {
            int v3 = _square[0]->field_0[i];
            arr[v3 & 0xFFF] = 1;
            arr[(v3 >> 16) & 0xFFF] = 1;
        }
    }

    if ((flags & 0x04) == 0) {
        for (int i = 0; i < SQUARE_GRID_SIZE; i++) {
            int v3 = _square[1]->field_0[i];
            arr[v3 & 0xFFF] = 1;
            arr[(v3 >> 16) & 0xFFF] = 1;
        }
    }

    if ((flags & 0x08) == 0) {
        for (int i = 0; i < SQUARE_GRID_SIZE; i++) {
            int v3 = _square[2]->field_0[i];
            arr[v3 & 0xFFF] = 1;
            arr[(v3 >> 16) & 0xFFF] = 1;
        }
    }

    qsort(gObjectFids, gObjectFidsLength, sizeof(*gObjectFids), _obj_preload_sort);

    int v11 = gObjectFidsLength;
    int v12 = gObjectFidsLength;

    if (FID_TYPE(gObjectFids[v12 - 1]) == OBJ_TYPE_WALL) {
        int objectType = OBJ_TYPE_ITEM;
        do {
            v11--;
            objectType = FID_TYPE(gObjectFids[v12 - 1]);
            v12--;
        } while (objectType == OBJ_TYPE_WALL);
        v11++;
    }

    CacheEntry* cache_handle;
    if (artLock(*gObjectFids, &cache_handle) != nullptr) {
        artUnlock(cache_handle);
    }

    for (int i = 1; i < v11; i++) {
        if (gObjectFids[i - 1] != gObjectFids[i]) {
            if (artLock(gObjectFids[i], &cache_handle) != nullptr) {
                artUnlock(cache_handle);
            }
        }
    }

    for (int i = 0; i < 4096; i++) {
        if (arr[i] != 0) {
            int fid = buildFid(OBJ_TYPE_TILE, i, 0, 0, 0);
            if (artLock(fid, &cache_handle) != nullptr) {
                artUnlock(cache_handle);
            }
        }
    }

    for (int i = v11; i < gObjectFidsLength; i++) {
        if (gObjectFids[i - 1] != gObjectFids[i]) {
            if (artLock(gObjectFids[i], &cache_handle) != nullptr) {
                artUnlock(cache_handle);
            }
        }
    }

    internal_free(gObjectFids);
    gObjectFids = nullptr;

    gObjectFidsLength = 0;
}

// 0x48CB88
static int _obj_offset_table_init()
{
    int i;

    if (_offsetTable[0] != nullptr) {
        return -1;
    }

    if (_offsetTable[1] != nullptr) {
        return -1;
    }

    _offsetTable[0] = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_offsetTable[0] == nullptr) {
        goto err;
    }

    _offsetTable[1] = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_offsetTable[1] == nullptr) {
        goto err;
    }

    for (int parity = 0; parity < 2; parity++) {
        int originTile = tileFromScreenXY(gObjectsUpdateAreaPixelBounds.left, gObjectsUpdateAreaPixelBounds.top, 0);
        if (originTile != -1) {
            int* offsets = _offsetTable[gCenterTile & 1];
            int originTileX;
            int originTileY;
            tileToScreenXY(originTile, &originTileX, &originTileY, 0);

            int parityShift = 16;
            originTileX += 16;
            originTileY += 8;
            if (originTileX > gObjectsUpdateAreaPixelBounds.left) {
                parityShift = -parityShift;
            }

            int tileX = originTileX;
            for (int y = 0; y < gObjectsUpdateAreaHexHeight; y++) {
                for (int x = 0; x < gObjectsUpdateAreaHexWidth; x++) {
                    int tile = tileFromScreenXY(tileX, originTileY, 0);
                    if (tile == -1) {
                        goto err;
                    }

                    tileX += 32;
                    *offsets++ = tile - originTile;
                }

                tileX = parityShift + originTileX;
                originTileY += 12;
                parityShift = -parityShift;
            }
        }

        if (tileSetCenter(gCenterTile + 1, TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS) == -1) {
            goto err;
        }
    }

    _offsetDivTable = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_offsetDivTable == nullptr) {
        goto err;
    }

    for (i = 0; i < gObjectsUpdateAreaHexSize; i++) {
        _offsetDivTable[i] = i / gObjectsUpdateAreaHexWidth;
    }

    _offsetModTable = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_offsetModTable == nullptr) {
        goto err;
    }

    for (i = 0; i < gObjectsUpdateAreaHexSize; i++) {
        _offsetModTable[i] = i % gObjectsUpdateAreaHexWidth;
    }

    return 0;

err:
    _obj_offset_table_exit();

    return -1;
}

// 0x48CDA0
static void _obj_offset_table_exit()
{
    if (_offsetModTable != nullptr) {
        internal_free(_offsetModTable);
        _offsetModTable = nullptr;
    }

    if (_offsetDivTable != nullptr) {
        internal_free(_offsetDivTable);
        _offsetDivTable = nullptr;
    }

    if (_offsetTable[1] != nullptr) {
        internal_free(_offsetTable[1]);
        _offsetTable[1] = nullptr;
    }

    if (_offsetTable[0] != nullptr) {
        internal_free(_offsetTable[0]);
        _offsetTable[0] = nullptr;
    }
}

// 0x48CE10
static int _obj_order_table_init()
{
    if (_orderTable[0] != nullptr || _orderTable[1] != nullptr) {
        return -1;
    }

    _orderTable[0] = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_orderTable[0] == nullptr) {
        goto err;
    }

    _orderTable[1] = (int*)internal_malloc(sizeof(int) * gObjectsUpdateAreaHexSize);
    if (_orderTable[1] == nullptr) {
        goto err;
    }

    for (int index = 0; index < gObjectsUpdateAreaHexSize; index++) {
        _orderTable[0][index] = index;
        _orderTable[1][index] = index;
    }

    qsort(_orderTable[0], gObjectsUpdateAreaHexSize, sizeof(int), _obj_order_comp_func_even);
    qsort(_orderTable[1], gObjectsUpdateAreaHexSize, sizeof(int), _obj_order_comp_func_odd);

    return 0;

err:

    // NOTE: Uninline.
    _obj_order_table_exit();

    return -1;
}

// 0x48CF20
static int _obj_order_comp_func_even(const void* a1, const void* a2)
{
    int v1 = *(int*)a1;
    int v2 = *(int*)a2;
    return _offsetTable[0][v1] - _offsetTable[0][v2];
}

// 0x48CF38
static int _obj_order_comp_func_odd(const void* a1, const void* a2)
{
    int v1 = *(int*)a1;
    int v2 = *(int*)a2;
    return _offsetTable[1][v1] - _offsetTable[1][v2];
}

// NOTE: Inlined.
//
// 0x48CF50
static void _obj_order_table_exit()
{
    if (_orderTable[1] != nullptr) {
        internal_free(_orderTable[1]);
        _orderTable[1] = nullptr;
    }

    if (_orderTable[0] != nullptr) {
        internal_free(_orderTable[0]);
        _orderTable[0] = nullptr;
    }
}


// 0x48D020
static void _obj_light_table_init()
{
    for (int s = 0; s < 2; s++) {
        int v4 = gCenterTile + s;
        for (int i = 0; i < ROTATION_COUNT; i++) {
            int v15 = 8;
            int* p = _light_offsets[v4 & 1][i];
            for (int j = 0; j < 8; j++) {
                int tile = tileGetTileInDirection(v4, (i + 1) % ROTATION_COUNT, j);

                for (int m = 0; m < v15; m++) {
                    *p++ = tileGetTileInDirection(tile, i, m + 1) - v4;
                }

                v15--;
            }
        }
    }
}


// 0x48D348
static int _obj_save_obj(File* stream, Object* object)
{
    if ((object->flags & OBJECT_NO_SAVE) != 0) {
        return 0;
    }

    CritterCombatData* combatData = nullptr;
    Object* whoHitMe = nullptr;
    if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
        combatData = &(object->data.critter.combat);
        whoHitMe = combatData->whoHitMe;
        if (whoHitMe != nullptr) {
            if (combatData->whoHitMeCid != -1) {
                combatData->whoHitMeCid = whoHitMe->cid;
            }
        } else {
            combatData->whoHitMeCid = -1;
        }
    }

    if (objectWrite(object, stream) == -1) {
        return -1;
    }

    if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
        combatData->whoHitMe = whoHitMe;
    }

    Inventory* inventory = &(object->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);

        if (fileWriteInt32(stream, inventoryItem->quantity) == -1) {
            return -1;
        }

        if (_obj_save_obj(stream, inventoryItem->item) == -1) {
            return -1;
        }

        if ((inventoryItem->item->flags & OBJECT_NO_SAVE) != 0) {
            return -1;
        }
    }

    return 0;
}

// 0x48D414
static int _obj_load_obj(File* stream, Object** objectPtr, int elevation, Object* owner)
{
    Object* obj;

    if (objectAllocate(&obj) == -1) {
        *objectPtr = nullptr;
        return -1;
    }

    if (objectRead(obj, stream) != 0) {
        *objectPtr = nullptr;
        return -1;
    }

    if (obj->sid != -1) {
        Script* script;
        if (scriptGetScript(obj->sid, &script) == -1) {
            obj->sid = -1;
        } else {
            script->owner = obj;
        }
    }

    _obj_fix_violence_settings(&(obj->fid));

    if (!_art_fid_valid(obj->fid)) {
        debugPrint("\nError: invalid object art fid: %u\n", obj->fid);
        // NOTE: Uninline.
        objectDeallocate(&obj);
        return -2;
    }

    if (elevation == -1) {
        elevation = obj->elevation;
    } else {
        obj->elevation = elevation;
    }

    obj->owner = owner;

    Inventory* inventory = &(obj->data.inventory);
    if (inventory->length <= 0) {
        inventory->capacity = 0;
        inventory->items = nullptr;
        *objectPtr = obj;
        return 0;
    }

    InventoryItem* inventoryItems = inventory->items = (InventoryItem*)internal_malloc(sizeof(*inventoryItems) * inventory->capacity);
    if (inventoryItems == nullptr) {
        return -1;
    }

    for (int inventoryItemIndex = 0; inventoryItemIndex < inventory->length; inventoryItemIndex++) {
        InventoryItem* inventoryItem = &(inventoryItems[inventoryItemIndex]);
        if (fileReadInt32(stream, &(inventoryItem->quantity)) != 0) {
            return -1;
        }

        if (_obj_load_obj(stream, &(inventoryItem->item), elevation, obj) != 0) {
            return -1;
        }
    }

    *objectPtr = obj;

    return 0;
}

// obj_save_dude
// 0x48D59C
int _obj_save_dude(File* stream)
{
    int field_78 = gDude->sid;

    gDude->flags &= ~OBJECT_NO_SAVE;
    gDude->sid = -1;

    int rc = _obj_save_obj(stream, gDude);

    gDude->sid = field_78;
    gDude->flags |= OBJECT_NO_SAVE;

    if (fileWriteInt32(stream, gCenterTile) == -1) {
        fileClose(stream);
        return -1;
    }

    return rc;
}

// Append ONE extra player actor (registry slot >= 1) to a join blob, exactly as
// _obj_save_dude does for the host: player actors are OBJECT_NO_SAVE, so
// objectSaveAll skips them and they must ride the appendix instead
// (MP_PROPOSAL.md Ch 5.3).
//
// Differences from _obj_save_dude, both deliberate: the trailing gCenterTile
// word is dude-only (it is the CAMERA, of which there is exactly one), and the
// sid dance is a no-op here because extras are spawned scriptless — kept anyway
// so this stays correct if that ever changes.
int _obj_save_player_actor(File* stream, Object* actor)
{
    if (actor == nullptr) {
        return -1;
    }

    int savedSid = actor->sid;
    unsigned int savedFlags = actor->flags;

    actor->flags &= ~OBJECT_NO_SAVE;
    actor->sid = -1;

    int rc = _obj_save_obj(stream, actor);

    actor->sid = savedSid;
    actor->flags = savedFlags;

    return rc;
}

// Apply a flags word that arrived over the wire, PRESERVING this side's own
// lifecycle classification.
//
// OBJECT_NO_REMOVE / OBJECT_NO_SAVE are not world state — they are a statement
// about who owns an object's LIFETIME in THIS process, and the two sides
// legitimately disagree. The server's player actors carry NO_REMOVE so map
// teardown spares them and the registry's raw Object* stay valid; the viewer's
// copies of those same actors must do the OPPOSITE and die with the world on
// every rebaseline (_obj_load_player_actor strips the flag for exactly this
// reason). The wire carries the whole flags word, so a single flags delta would
// otherwise hand the server's NO_REMOVE back and make the viewer's copy
// immortal — _obj_remove_all skips it, the next blob loads a SECOND actor beside
// it, and each rebaseline leaks one more permanent ghost body that no netId
// check notices (its netId is zeroed, so nothing addresses it — it just stands
// there). Stripping at load is not enough; the live channels must honor it too.
void objectApplyWireFlags(Object* obj, unsigned int wireFlags)
{
    if (obj == nullptr) {
        return;
    }

    const unsigned int kLocalLifecycleFlags = OBJECT_NO_REMOVE | OBJECT_NO_SAVE;
    obj->flags = (wireFlags & ~kLocalLifecycleFlags) | (obj->flags & kLocalLifecycleFlags);
}

// Load ONE extra player actor from a join blob and place it in the world. The
// VIEWER side of _obj_save_player_actor (MP_PROPOSAL.md Ch 5.3).
//
// ⚠ NO_REMOVE must be STRIPPED here. The server's actors carry it (that is what
// makes their pointers process-stable across map teardown), and _obj_save_obj
// writes the flag word verbatim — but on the viewer these are ordinary
// blob-loaded objects that MUST die with the world on the next rebaseline.
// Leaving the flag set would leak a duplicate actor into every subsequent
// mapLoad. NO_SAVE is re-asserted for symmetry with the server's classification
// (it keeps objectIsSyncable and the netId walk reading the same on both sides).
int _obj_load_player_actor(File* stream, Object** actorPtr)
{
    Object* obj = nullptr;
    if (_obj_load_obj(stream, &obj, -1, nullptr) != 0 || obj == nullptr) {
        if (actorPtr != nullptr) {
            *actorPtr = nullptr;
        }
        return -1;
    }

    obj->flags &= ~OBJECT_NO_REMOVE;
    obj->flags |= OBJECT_NO_SAVE;

    // ⚠ _obj_load_obj calls objectAllocate, NOT objectListNodeCreate: it hands
    // back a bare Object* that belongs to NO object list. Every list-aware
    // primitive fails CLOSED on such an object — objectSetLocation returns -1 at
    // its objectGetListNode call and does nothing at all, silently — so the actor
    // would render nowhere, never appear in the objectFindFirst walk (so the
    // decoder's netId map never learns it and every MOVE addressed to it is
    // dropped), and be invisible to the whole engine. Give it a node first, the
    // way objectCreateWithFidPid does: insert at tile -1 (the "not placed yet"
    // head list), then let objectSetLocation do the real placement with its
    // lighting/connect work.
    ObjectListNode* node;
    if (objectListNodeCreate(&node) == -1) {
        if (actorPtr != nullptr) {
            *actorPtr = nullptr;
        }
        return -1;
    }

    int tile = obj->tile;
    int elevation = obj->elevation;

    node->obj = obj;
    obj->tile = -1;
    _obj_insert(node);

    objectSetLocation(obj, tile, elevation, nullptr);
    objectSetRotation(obj, obj->rotation, nullptr);

    if (actorPtr != nullptr) {
        *actorPtr = obj;
    }

    return 0;
}

// obj_load_dude
// 0x48D600
int _obj_load_dude(File* stream)
{
    int savedTile = gDude->tile;
    int savedElevation = gDude->elevation;
    int savedRotation = gDude->rotation;
    int savedOid = gDude->id;

    scriptsClearDudeScript();

    Object* temp;
    int rc = _obj_load_obj(stream, &temp, -1, nullptr);

    // Free the current dude's carried inventory BEFORE the memcpy overwrites the
    // data.inventory pointer (APPLYBLOB_TEARDOWN_PLAN step 1). gDude's items live
    // only inside its Inventory (never in tile buckets), so the mapLoad → _obj_remove_all
    // that precedes this on a co-op rebaseline SKIPS them — the memcpy below would then
    // clobber the items[] pointer and leak the whole array (+ every carried item) every
    // rebaseline. No-op on the first load (empty inventory) and on the single-player load
    // path (the in-progress inventory is being replaced by the save's anyway); on the
    // client _net is already cleared this teardown, so freeing these items strands no
    // stale netId map entry.
    _obj_inven_free(&(gDude->data.inventory));

    memcpy(gDude, temp, sizeof(*gDude));

    if (serverLoopActive()) {
        gDude->netId = 1;
    }

    gDude->flags |= OBJECT_NO_SAVE;

    scriptsClearDudeScript();

    gDude->id = savedOid;

    scriptsSetDudeScript();

    int newTile = gDude->tile;
    gDude->tile = savedTile;

    int newElevation = gDude->elevation;
    gDude->elevation = savedElevation;

    int newRotation = gDude->rotation;
    gDude->rotation = newRotation;

    scriptsSetDudeScript();

    if (rc != -1) {
        objectSetLocation(gDude, newTile, newElevation, nullptr);
        objectSetRotation(gDude, newRotation, nullptr);
    }

    // Set ownership of inventory items from temporary instance to dude.
    Inventory* inventory = &(gDude->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        inventoryItem->item->owner = gDude;
    }

    // Dude has claimed ownership of items in temporary instance's inventory.
    // We don't need object's dealloc routine to remove these items from the
    // game, so simply nullify temporary inventory as if nothing was there.
    Inventory* tempInventory = &(temp->data.inventory);
    tempInventory->length = 0;
    tempInventory->capacity = 0;
    tempInventory->items = nullptr;

    temp->flags &= ~OBJECT_NO_REMOVE;

    if (objectDestroy(temp, nullptr) == -1) {
        debugPrint("\nError: obj_load_dude: Can't destroy temp object!\n");
    }

    _inven_reset_dude();

    int tile;
    if (fileReadInt32(stream, &tile) == -1) {
        fileClose(stream);
        return -1;
    }

    tileSetCenter(tile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);

    return rc;
}

// 0x48D778
static int objectAllocate(Object** objectPtr)
{
    if (objectPtr == nullptr) {
        return -1;
    }

    Object* object = *objectPtr = (Object*)internal_malloc(sizeof(Object));
    if (object == nullptr) {
        return -1;
    }

    memset(object, 0, sizeof(Object));

    object->id = -1;
    object->tile = -1;
    object->cid = -1;
    object->outline = 0;
    object->pid = -1;
    object->sid = -1;
    object->owner = nullptr;
    object->scriptIndex = -1;
    object->netId = 0;

    return 0;
}

// NOTE: Inlined.
//
// 0x48D7F8
static void objectDeallocate(Object** objectPtr)
{
    if (objectPtr == nullptr) {
        return;
    }

    if (*objectPtr == nullptr) {
        return;
    }

    internal_free(*objectPtr);

    *objectPtr = nullptr;
}

// NOTE: Inlined.
//
// 0x48D818
static int objectListNodeCreate(ObjectListNode** nodePtr)
{
    if (nodePtr == nullptr) {
        return -1;
    }

    ObjectListNode* node = *nodePtr = (ObjectListNode*)internal_malloc(sizeof(*node));
    if (node == nullptr) {
        return -1;
    }

    node->obj = nullptr;
    node->next = nullptr;

    return 0;
}

// NOTE: Inlined.
//
// 0x48D84C
static void objectListNodeDestroy(ObjectListNode** nodePtr)
{
    if (nodePtr == nullptr) {
        return;
    }

    if (*nodePtr == nullptr) {
        return;
    }

    internal_free(*nodePtr);

    *nodePtr = nullptr;
}

// 0x48D86C
static int objectGetListNode(Object* object, ObjectListNode** nodePtr, ObjectListNode** previousNodePtr)
{
    if (object == nullptr) {
        return -1;
    }

    if (nodePtr == nullptr) {
        return -1;
    }

    int tile = object->tile;
    if (tile != -1) {
        *nodePtr = gObjectListHeadByTile[tile];
    } else {
        *nodePtr = gObjectListHead;
    }

    if (previousNodePtr != nullptr) {
        *previousNodePtr = nullptr;
        while (*nodePtr != nullptr) {
            if (object == (*nodePtr)->obj) {
                break;
            }

            *previousNodePtr = *nodePtr;

            *nodePtr = (*nodePtr)->next;
        }
    } else {
        while (*nodePtr != nullptr) {
            if (object == (*nodePtr)->obj) {
                break;
            }

            *nodePtr = (*nodePtr)->next;
        }
    }

    if (*nodePtr != nullptr) {
        return 0;
    }

    return -1;
}

// 0x48D8E8
static void _obj_insert(ObjectListNode* objectListNode)
{
    ObjectListNode** objectListNodePtr;

    if (objectListNode == nullptr) {
        return;
    }

    if (objectListNode->obj->tile == -1) {
        objectListNodePtr = &gObjectListHead;
    } else {
        Art* art = nullptr;
        CacheEntry* cacheHandle = nullptr;

        objectListNodePtr = &(gObjectListHeadByTile[objectListNode->obj->tile]);

        while (*objectListNodePtr != nullptr) {
            Object* obj = (*objectListNodePtr)->obj;
            if (obj->elevation > objectListNode->obj->elevation) {
                break;
            }

            if (obj->elevation == objectListNode->obj->elevation) {
                if ((obj->flags & OBJECT_FLAT) == 0 && (objectListNode->obj->flags & OBJECT_FLAT) != 0) {
                    break;
                }

                if ((obj->flags & OBJECT_FLAT) == (objectListNode->obj->flags & OBJECT_FLAT)) {
                    bool v11 = false;
                    CacheEntry* a2;
                    Art* v12 = artLock(obj->fid, &a2);
                    if (v12 != nullptr) {

                        if (art == nullptr) {
                            art = artLock(objectListNode->obj->fid, &cacheHandle);
                        }

                        // TODO: Incomplete.

                        artUnlock(a2);

                        if (v11) {
                            break;
                        }
                    }
                }
            }

            objectListNodePtr = &((*objectListNodePtr)->next);
        }

        if (art != nullptr) {
            artUnlock(cacheHandle);
        }
    }

    objectListNode->next = *objectListNodePtr;
    *objectListNodePtr = objectListNode;
}

// 0x48DA58
static int _obj_remove(ObjectListNode* a1, ObjectListNode* a2)
{
    if (a1->obj == nullptr) {
        return -1;
    }

    if ((a1->obj->flags & OBJECT_NO_REMOVE) != 0) {
        return -1;
    }

    _obj_inven_free(&(a1->obj->data.inventory));

    if (a1->obj->sid != -1) {
        scriptExecProc(a1->obj->sid, SCRIPT_PROC_DESTROY);
        scriptRemove(a1->obj->sid);
    }

    if (a1 != a2) {
        if (a2 != nullptr) {
            a2->next = a1->next;
        } else {
            int tile = a1->obj->tile;
            if (tile == -1) {
                gObjectListHead = gObjectListHead->next;
            } else {
                gObjectListHeadByTile[tile] = gObjectListHeadByTile[tile]->next;
            }
        }
    }

    // NOTE: Uninline.
    objectDeallocate(&(a1->obj));

    // NOTE: Uninline.
    objectListNodeDestroy(&a1);

    return 0;
}

// 0x48DB28
static int _obj_connect_to_tile(ObjectListNode* node, int tile, int elevation, Rect* rect)
{
    if (node == nullptr) {
        return -1;
    }

    if (!hexGridTileIsValid(tile)) {
        return -1;
    }

    if (!elevationIsValid(elevation)) {
        return -1;
    }

    node->obj->tile = tile;
    node->obj->elevation = elevation;
    node->obj->x = 0;
    node->obj->y = 0;
    node->obj->owner = nullptr;

    _obj_insert(node);

    if (_obj_adjust_light(node->obj, 0, rect) == -1) {
        if (rect != nullptr) {
            objectGetRect(node->obj, rect);
        }
    }

    return 0;
}

// 0x48DC28
static int _obj_adjust_light(Object* obj, int a2, Rect* rect)
{
    if (obj == nullptr) {
        return -1;
    }

    if (obj->lightIntensity <= 0) {
        return -1;
    }

    if ((obj->flags & OBJECT_HIDDEN) != 0) {
        return -1;
    }

    if ((obj->flags & OBJECT_LIGHTING) == 0) {
        return -1;
    }

    if (!hexGridTileIsValid(obj->tile)) {
        return -1;
    }

    AdjustLightIntensityProc* adjustLightIntensity = a2 ? lightDecreaseTileIntensity : lightIncreaseTileIntensity;
    adjustLightIntensity(obj->elevation, obj->tile, obj->lightIntensity);

    Rect objectRect;
    objectGetRect(obj, &objectRect);

    if (obj->lightDistance > 8) {
        obj->lightDistance = 8;
    }

    if (obj->lightIntensity > 65536) {
        obj->lightIntensity = 65536;
    }

    int(*v70)[36] = _light_offsets[obj->tile & 1];
    int v7 = (obj->lightIntensity - 655) / (obj->lightDistance + 1);
    int v28[36];
    v28[0] = obj->lightIntensity - v7;
    v28[1] = v28[0] - v7;
    v28[8] = v28[0] - v7;
    v28[2] = v28[0] - v7 - v7;
    v28[9] = v28[2];
    v28[15] = v28[0] - v7 - v7;
    v28[3] = v28[2] - v7;
    v28[10] = v28[2] - v7;
    v28[16] = v28[2] - v7;
    v28[21] = v28[2] - v7;
    v28[4] = v28[2] - v7 - v7;
    v28[11] = v28[4];
    v28[17] = v28[2] - v7 - v7;
    v28[22] = v28[2] - v7 - v7;
    v28[26] = v28[2] - v7 - v7;
    v28[5] = v28[4] - v7;
    v28[12] = v28[4] - v7;
    v28[18] = v28[4] - v7;
    v28[23] = v28[4] - v7;
    v28[27] = v28[4] - v7;
    v28[30] = v28[4] - v7;
    v28[6] = v28[4] - v7 - v7;
    v28[13] = v28[6];
    v28[19] = v28[4] - v7 - v7;
    v28[24] = v28[4] - v7 - v7;
    v28[28] = v28[4] - v7 - v7;
    v28[31] = v28[4] - v7 - v7;
    v28[33] = v28[4] - v7 - v7;
    v28[7] = v28[6] - v7;
    v28[14] = v28[6] - v7;
    v28[20] = v28[6] - v7;
    v28[25] = v28[6] - v7;
    v28[29] = v28[6] - v7;
    v28[32] = v28[6] - v7;
    v28[34] = v28[6] - v7;
    v28[35] = v28[6] - v7;

    for (int index = 0; index < 36; index++) {
        if (obj->lightDistance >= _light_distance[index]) {
            for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                int v14;
                int nextRotation = (rotation + 1) % ROTATION_COUNT;
                int eax;
                int edx;
                int ebx;
                int esi;
                int edi;
                switch (index) {
                case 0:
                    v14 = 0;
                    break;
                case 1:
                    v14 = _light_blocked[rotation][0];
                    break;
                case 2:
                    v14 = _light_blocked[rotation][1];
                    break;
                case 3:
                    v14 = _light_blocked[rotation][2];
                    break;
                case 4:
                    v14 = _light_blocked[rotation][3];
                    break;
                case 5:
                    v14 = _light_blocked[rotation][4];
                    break;
                case 6:
                    v14 = _light_blocked[rotation][5];
                    break;
                case 7:
                    v14 = _light_blocked[rotation][6];
                    break;
                case 8:
                    v14 = _light_blocked[rotation][0] & _light_blocked[nextRotation][0];
                    break;
                case 9:
                    v14 = _light_blocked[rotation][1] & _light_blocked[rotation][8];
                    break;
                case 10:
                    v14 = _light_blocked[rotation][2] & _light_blocked[rotation][9];
                    break;
                case 11:
                    v14 = _light_blocked[rotation][3] & _light_blocked[rotation][10];
                    break;
                case 12:
                    v14 = _light_blocked[rotation][4] & _light_blocked[rotation][11];
                    break;
                case 13:
                    v14 = _light_blocked[rotation][5] & _light_blocked[rotation][12];
                    break;
                case 14:
                    v14 = _light_blocked[rotation][6] & _light_blocked[rotation][13];
                    break;
                case 15:
                    v14 = _light_blocked[rotation][8] & _light_blocked[nextRotation][1];
                    break;
                case 16:
                    v14 = _light_blocked[rotation][8] | (_light_blocked[rotation][9] & _light_blocked[rotation][15]);
                    break;
                case 17:
                    edx = _light_blocked[rotation][9];
                    edx |= _light_blocked[rotation][10];
                    ebx = _light_blocked[rotation][8];
                    esi = _light_blocked[rotation][16];
                    ebx &= edx;
                    edx &= esi;
                    edi = _light_blocked[rotation][15];
                    ebx |= edx;
                    edx = _light_blocked[rotation][10];
                    eax = _light_blocked[rotation][9];
                    edx |= edi;
                    eax &= edx;
                    v14 = ebx | eax;
                    break;
                case 18:
                    edx = _light_blocked[rotation][0];
                    ebx = _light_blocked[rotation][9];
                    esi = _light_blocked[rotation][10];
                    edx |= ebx;
                    edi = _light_blocked[rotation][11];
                    edx |= esi;
                    ebx = _light_blocked[rotation][17];
                    edx |= edi;
                    ebx &= edx;
                    edx = esi;
                    esi = _light_blocked[rotation][16];
                    edi = _light_blocked[rotation][9];
                    edx &= esi;
                    edx |= edi;
                    edx |= ebx;
                    v14 = edx;
                    break;
                case 19:
                    edx = _light_blocked[rotation][17];
                    edi = _light_blocked[rotation][18];
                    ebx = _light_blocked[rotation][11];
                    edx |= edi;
                    esi = _light_blocked[rotation][10];
                    ebx &= edx;
                    edx = _light_blocked[rotation][9];
                    edx |= esi;
                    ebx |= edx;
                    edx = _light_blocked[rotation][12];
                    edx &= edi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 20:
                    edx = _light_blocked[rotation][2];
                    esi = _light_blocked[rotation][11];
                    edi = _light_blocked[rotation][12];
                    ebx = _light_blocked[rotation][8];
                    edx |= esi;
                    esi = _light_blocked[rotation][9];
                    edx |= edi;
                    edi = _light_blocked[rotation][10];
                    ebx &= edx;
                    edx &= esi;
                    esi = _light_blocked[rotation][17];
                    ebx |= edx;
                    edx = _light_blocked[rotation][16];
                    ebx |= edi;
                    edi = _light_blocked[rotation][18];
                    edx |= esi;
                    esi = _light_blocked[rotation][19];
                    edx |= edi;
                    eax = _light_blocked[rotation][11];
                    edx |= esi;
                    eax &= edx;
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 21:
                    v14 = (_light_blocked[rotation][8] & _light_blocked[nextRotation][1])
                        | (_light_blocked[rotation][15] & _light_blocked[nextRotation][2]);
                    break;
                case 22:
                    edx = _light_blocked[nextRotation][1];
                    ebx = _light_blocked[rotation][15];
                    esi = _light_blocked[rotation][21];
                    edx |= ebx;
                    ebx = _light_blocked[rotation][8];
                    edx |= esi;
                    ebx &= edx;
                    edx = _light_blocked[rotation][9];
                    edi = esi;
                    edx |= esi;
                    esi = _light_blocked[rotation][15];
                    edx &= esi;
                    ebx |= edx;
                    edx = esi;
                    esi = _light_blocked[rotation][16];
                    edx |= edi;
                    edx &= esi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 23:
                    edx = _light_blocked[rotation][3];
                    ebx = _light_blocked[rotation][16];
                    esi = _light_blocked[rotation][15];
                    ebx |= edx;
                    edx = _light_blocked[rotation][9];
                    edx &= esi;
                    edi = _light_blocked[rotation][22];
                    ebx |= edx;
                    edx = _light_blocked[rotation][17];
                    edx &= edi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 24:
                    edx = _light_blocked[rotation][0];
                    edi = _light_blocked[rotation][9];
                    ebx = _light_blocked[rotation][10];
                    edx |= edi;
                    esi = _light_blocked[rotation][17];
                    edx |= ebx;
                    edi = _light_blocked[rotation][18];
                    edx |= esi;
                    ebx = _light_blocked[rotation][16];
                    edx |= edi;
                    esi = _light_blocked[rotation][16];
                    ebx &= edx;
                    edx = _light_blocked[rotation][15];
                    edi = _light_blocked[rotation][23];
                    edx |= esi;
                    esi = _light_blocked[rotation][9];
                    edx |= edi;
                    edi = _light_blocked[rotation][8];
                    edx &= esi;
                    edx |= edi;
                    esi = _light_blocked[rotation][22];
                    ebx |= edx;
                    edx = _light_blocked[rotation][15];
                    edi = _light_blocked[rotation][23];
                    edx |= esi;
                    esi = _light_blocked[rotation][17];
                    edx |= edi;
                    edx &= esi;
                    ebx |= edx;
                    edx = _light_blocked[rotation][18];
                    edx &= edi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 25:
                    edx = _light_blocked[rotation][8];
                    edi = _light_blocked[rotation][15];
                    ebx = _light_blocked[rotation][16];
                    edx |= edi;
                    esi = _light_blocked[rotation][23];
                    edx |= ebx;
                    edi = _light_blocked[rotation][24];
                    edx |= esi;
                    ebx = _light_blocked[rotation][9];
                    edx |= edi;
                    esi = _light_blocked[rotation][1];
                    ebx &= edx;
                    edx = _light_blocked[rotation][8];
                    edx &= esi;
                    edi = _light_blocked[rotation][16];
                    ebx |= edx;
                    edx = _light_blocked[rotation][8];
                    esi = _light_blocked[rotation][17];
                    edx |= edi;
                    edi = _light_blocked[rotation][24];
                    esi |= edx;
                    esi |= edi;
                    esi &= _light_blocked[rotation][10];
                    edi = _light_blocked[rotation][23];
                    ebx |= esi;
                    esi = _light_blocked[rotation][17];
                    edx |= edi;
                    ebx |= esi;
                    esi = _light_blocked[rotation][24];
                    edi = _light_blocked[rotation][18];
                    edx |= esi;
                    edx &= edi;
                    esi = _light_blocked[rotation][19];
                    ebx |= edx;
                    edx = _light_blocked[rotation][0];
                    eax = _light_blocked[rotation][24];
                    edx |= esi;
                    eax &= edx;
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 26:
                    ebx = _light_blocked[rotation][8];
                    esi = _light_blocked[nextRotation][1];
                    edi = _light_blocked[nextRotation][2];
                    esi &= ebx;
                    ebx = _light_blocked[rotation][15];
                    ebx &= edi;
                    eax = _light_blocked[rotation][21];
                    ebx |= esi;
                    eax &= _light_blocked[nextRotation][3];
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 27:
                    edx = _light_blocked[nextRotation][0];
                    edi = _light_blocked[rotation][15];
                    esi = _light_blocked[rotation][21];
                    edx |= edi;
                    edi = _light_blocked[rotation][26];
                    edx |= esi;
                    esi = _light_blocked[rotation][22];
                    edx |= edi;
                    edi = _light_blocked[nextRotation][1];
                    esi &= edx;
                    edx = _light_blocked[rotation][8];
                    ebx = _light_blocked[rotation][15];
                    edx &= edi;
                    edx |= ebx;
                    edi = _light_blocked[rotation][16];
                    esi |= edx;
                    edx = _light_blocked[rotation][8];
                    eax = _light_blocked[rotation][21];
                    edx |= edi;
                    eax &= edx;
                    esi |= eax;
                    v14 = esi;
                    break;
                case 28:
                    ebx = _light_blocked[rotation][9];
                    edi = _light_blocked[rotation][16];
                    esi = _light_blocked[rotation][23];
                    edx = _light_blocked[nextRotation][0];
                    ebx |= edi;
                    edi = _light_blocked[rotation][15];
                    ebx |= esi;
                    esi = _light_blocked[rotation][8];
                    ebx &= edi;
                    edi = _light_blocked[rotation][21];
                    ebx |= esi;
                    esi = _light_blocked[rotation][22];
                    edx |= edi;
                    edi = _light_blocked[rotation][27];
                    edx |= esi;
                    esi = _light_blocked[rotation][16];
                    edx |= edi;
                    edx &= esi;
                    edi = _light_blocked[rotation][17];
                    ebx |= edx;
                    edx = _light_blocked[rotation][9];
                    esi = _light_blocked[rotation][23];
                    edx |= edi;
                    edi = _light_blocked[rotation][22];
                    edx |= esi;
                    edx &= edi;
                    ebx |= edx;
                    edx = esi;
                    edx &= _light_blocked[rotation][27];
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 29:
                    edx = _light_blocked[rotation][8];
                    edi = _light_blocked[rotation][16];
                    ebx = _light_blocked[rotation][23];
                    edx |= edi;
                    esi = _light_blocked[rotation][15];
                    ebx |= edx;
                    edx = _light_blocked[rotation][9];
                    edx &= esi;
                    edi = _light_blocked[rotation][22];
                    ebx |= edx;
                    edx = _light_blocked[rotation][17];
                    edx &= edi;
                    esi = _light_blocked[rotation][28];
                    ebx |= edx;
                    edx = _light_blocked[rotation][24];
                    edx &= esi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 30:
                    ebx = _light_blocked[rotation][8];
                    esi = _light_blocked[nextRotation][1];
                    edi = _light_blocked[nextRotation][2];
                    esi &= ebx;
                    ebx = _light_blocked[rotation][15];
                    ebx &= edi;
                    edi = _light_blocked[nextRotation][3];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][21];
                    ebx &= edi;
                    eax = _light_blocked[rotation][26];
                    ebx |= esi;
                    eax &= _light_blocked[nextRotation][4];
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 31:
                    edx = _light_blocked[rotation][8];
                    esi = _light_blocked[nextRotation][1];
                    edi = _light_blocked[rotation][15];
                    edx &= esi;
                    ebx = _light_blocked[rotation][21];
                    edx |= edi;
                    esi = _light_blocked[rotation][22];
                    ebx |= edx;
                    edx = _light_blocked[rotation][8];
                    edi = _light_blocked[rotation][27];
                    edx |= esi;
                    esi = _light_blocked[rotation][26];
                    edx |= edi;
                    edx &= esi;
                    ebx |= edx;
                    edx = edi;
                    edx &= _light_blocked[rotation][30];
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 32:
                    ebx = _light_blocked[rotation][8];
                    edi = _light_blocked[rotation][9];
                    esi = _light_blocked[rotation][16];
                    ebx |= edi;
                    edi = _light_blocked[rotation][23];
                    ebx |= esi;
                    esi = _light_blocked[rotation][28];
                    ebx |= edi;
                    ebx |= esi;
                    esi = _light_blocked[rotation][15];
                    esi &= ebx;
                    edx = _light_blocked[rotation][8];
                    edx &= _light_blocked[nextRotation][1];
                    ebx = _light_blocked[rotation][16];
                    esi |= edx;
                    edx = _light_blocked[rotation][8];
                    edx |= ebx;
                    ebx = _light_blocked[rotation][28];
                    edi = _light_blocked[rotation][21];
                    ebx |= edx;
                    ebx &= edi;
                    edi = _light_blocked[rotation][23];
                    ebx |= esi;
                    esi = _light_blocked[rotation][22];
                    edx |= edi;
                    ebx |= esi;
                    esi = _light_blocked[rotation][28];
                    edi = _light_blocked[rotation][27];
                    edx |= esi;
                    edx &= edi;
                    esi = _light_blocked[rotation][31];
                    ebx |= edx;
                    edx = _light_blocked[rotation][0];
                    edi = _light_blocked[rotation][28];
                    edx |= esi;
                    edx &= edi;
                    ebx |= edx;
                    v14 = ebx;
                    break;
                case 33:
                    esi = _light_blocked[rotation][8];
                    edi = _light_blocked[nextRotation][1];
                    ebx = _light_blocked[rotation][15];
                    esi &= edi;
                    ebx &= _light_blocked[nextRotation][2];
                    edi = _light_blocked[nextRotation][3];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][21];
                    ebx &= edi;
                    edi = _light_blocked[nextRotation][4];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][26];
                    ebx &= edi;
                    eax = _light_blocked[rotation][30];
                    ebx |= esi;
                    eax &= _light_blocked[nextRotation][5];
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 34:
                    edx = _light_blocked[nextRotation][2];
                    edi = _light_blocked[rotation][26];
                    ebx = _light_blocked[rotation][30];
                    edx |= edi;
                    esi = _light_blocked[rotation][15];
                    edx |= ebx;
                    ebx = _light_blocked[rotation][8];
                    edi = _light_blocked[rotation][21];
                    ebx &= edx;
                    edx &= esi;
                    esi = _light_blocked[rotation][22];
                    ebx |= edx;
                    edx = _light_blocked[rotation][16];
                    ebx |= edi;
                    edi = _light_blocked[rotation][27];
                    edx |= esi;
                    esi = _light_blocked[rotation][31];
                    edx |= edi;
                    eax = _light_blocked[rotation][26];
                    edx |= esi;
                    eax &= edx;
                    ebx |= eax;
                    v14 = ebx;
                    break;
                case 35:
                    ebx = _light_blocked[rotation][8];
                    esi = _light_blocked[nextRotation][1];
                    edi = _light_blocked[nextRotation][2];
                    esi &= ebx;
                    ebx = _light_blocked[rotation][15];
                    ebx &= edi;
                    edi = _light_blocked[nextRotation][3];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][21];
                    ebx &= edi;
                    edi = _light_blocked[nextRotation][4];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][26];
                    ebx &= edi;
                    edi = _light_blocked[nextRotation][5];
                    esi |= ebx;
                    ebx = _light_blocked[rotation][30];
                    ebx &= edi;
                    eax = _light_blocked[rotation][33];
                    ebx |= esi;
                    eax &= _light_blocked[nextRotation][6];
                    ebx |= eax;
                    v14 = ebx;
                    break;
                default:
                    assert(false && "Should be unreachable");
                }

                if (v14 == 0) {
                    // TODO: Check.
                    int tile = obj->tile + v70[rotation][index];
                    if (hexGridTileIsValid(tile)) {
                        bool v12 = true;

                        ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
                        while (objectListNode != nullptr) {
                            if ((objectListNode->obj->flags & OBJECT_HIDDEN) == 0) {
                                if (objectListNode->obj->elevation > obj->elevation) {
                                    break;
                                }

                                if (objectListNode->obj->elevation == obj->elevation) {
                                    Rect v29;
                                    objectGetRect(objectListNode->obj, &v29);
                                    rectUnion(&objectRect, &v29, &objectRect);

                                    v14 = (objectListNode->obj->flags & OBJECT_LIGHT_THRU) == 0;

                                    if (FID_TYPE(objectListNode->obj->fid) == OBJ_TYPE_WALL) {
                                        if ((objectListNode->obj->flags & OBJECT_FLAT) == 0) {
                                            Proto* proto;
                                            protoGetProto(objectListNode->obj->pid, &proto);
                                            if ((proto->wall.extendedFlags & 0x8000000) != 0 || (proto->wall.extendedFlags & 0x40000000) != 0) {
                                                if (rotation != ROTATION_W
                                                    && rotation != ROTATION_NW
                                                    && (rotation != ROTATION_NE || index >= 8)
                                                    && (rotation != ROTATION_SW || index <= 15)) {
                                                    v12 = false;
                                                }
                                            } else if ((proto->wall.extendedFlags & 0x10000000) != 0) {
                                                if (rotation != ROTATION_NE && rotation != ROTATION_NW) {
                                                    v12 = false;
                                                }
                                            } else if ((proto->wall.extendedFlags & 0x20000000) != 0) {
                                                if (rotation != ROTATION_NE
                                                    && rotation != ROTATION_E
                                                    && rotation != ROTATION_W
                                                    && rotation != ROTATION_NW
                                                    && (rotation != ROTATION_SW || index <= 15)) {
                                                    v12 = false;
                                                }
                                            } else {
                                                if (rotation != ROTATION_NE
                                                    && rotation != ROTATION_E
                                                    && (rotation != ROTATION_NW || index <= 7)) {
                                                    v12 = false;
                                                }
                                            }
                                        }
                                    } else {
                                        if (v14 && rotation >= ROTATION_E && rotation <= ROTATION_SW) {
                                            v12 = false;
                                        }
                                    }

                                    if (v14) {
                                        break;
                                    }
                                }
                            }
                            objectListNode = objectListNode->next;
                        }

                        if (v12) {
                            adjustLightIntensity(obj->elevation, tile, v28[index]);
                        }
                    }
                }

                _light_blocked[rotation][index] = v14;
            }
        }
    }

    if (rect != nullptr) {
        Rect* lightDistanceRect = &(_light_rect[obj->lightDistance]);
        memcpy(rect, lightDistanceRect, sizeof(*lightDistanceRect));

        int x;
        int y;
        tileToScreenXY(obj->tile, &x, &y, obj->elevation);
        x += 16;
        y += 8;

        x -= rect->right / 2;
        y -= rect->bottom / 2;

        rectOffset(rect, x, y);
        rectUnion(rect, &objectRect, rect);
    }

    return 0;
}



// Updates fid according to current violence level.
//
// 0x48FA14
void _obj_fix_violence_settings(int* fid)
{
    if (FID_TYPE(*fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    bool shouldResetViolenceLevel = false;
    if (gViolenceLevel == -1) {
        gViolenceLevel = settings.preferences.violence_level;
        shouldResetViolenceLevel = true;
    }

    int start;
    int end;

    switch (gViolenceLevel) {
    case VIOLENCE_LEVEL_NONE:
        start = ANIM_BIG_HOLE_SF;
        end = ANIM_FALL_FRONT_BLOOD_SF;
        break;
    case VIOLENCE_LEVEL_MINIMAL:
        start = ANIM_BIG_HOLE_SF;
        end = ANIM_FIRE_DANCE_SF;
        break;
    case VIOLENCE_LEVEL_NORMAL:
        start = ANIM_BIG_HOLE_SF;
        end = ANIM_SLICED_IN_HALF_SF;
        break;
    default:
        // Do not replace anything.
        start = ANIM_COUNT + 1;
        end = ANIM_COUNT + 1;
        break;
    }

    int anim = FID_ANIM_TYPE(*fid);
    if (anim >= start && anim <= end) {
        anim = (anim == ANIM_FALL_BACK_BLOOD_SF)
            ? ANIM_FALL_BACK_SF
            : ANIM_FALL_FRONT_SF;
        *fid = buildFid(OBJ_TYPE_CRITTER, *fid & 0xFFF, anim, (*fid & 0xF000) >> 12, (*fid & 0x70000000) >> 28);
    }

    if (shouldResetViolenceLevel) {
        gViolenceLevel = -1;
    }
}

// 0x48FB08
static int _obj_preload_sort(const void* a1, const void* a2)
{
    int v1 = *(int*)a1;
    int v2 = *(int*)a2;

    int v3 = _cd_order[FID_TYPE(v1)];
    int v4 = _cd_order[FID_TYPE(v2)];

    int cmp = v3 - v4;
    if (cmp != 0) {
        return cmp;
    }

    cmp = (v1 & 0xFFF) - (v2 & 0xFFF);
    if (cmp != 0) {
        return cmp;
    }

    cmp = ((v1 & 0xF000) >> 12) - (((v2 & 0xF000) >> 12));
    if (cmp != 0) {
        return cmp;
    }

    cmp = ((v1 & 0xFF0000) >> 16) - (((v2 & 0xFF0000) >> 16));
    return cmp;
}

Object* objectTypedFindById(int id, int type)
{
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (obj->id == id && PID_TYPE(obj->pid) == type) {
            return obj;
        }
        obj = objectFindNext();
    }

    return nullptr;
}

bool isExitGridAt(int tile, int elevation)
{
    ObjectListNode* objectListNode = gObjectListHeadByTile[tile];
    while (objectListNode != nullptr) {
        Object* obj = objectListNode->obj;
        if (obj->elevation == elevation) {
            if ((obj->flags & OBJECT_HIDDEN) == 0) {
                if (isExitGridPid(obj->pid)) {
                    return true;
                }
            }
        }
        objectListNode = objectListNode->next;
    }

    return false;
}

static int gNextNetId = 1;

int objectNextNetId()
{
    return gNextNetId++;
}

int objectGetNextNetId()
{
    return gNextNetId;
}

void objectSetNextNetId(int netId)
{
    gNextNetId = netId;
}

static void objectAssignInventoryNetIds(Object* owner)
{
    Inventory* inv = &(owner->data.inventory);
    for (int i = 0; i < inv->length; ++i) {
        Object* item = inv->items[i].item;
        item->netId = objectNextNetId();
        objectAssignInventoryNetIds(item);
    }
}

// Recursively clear the netIds of a skipped (NO_SAVE) object's inventory so no
// stale value survives a rebaseline to collide with the syncable netId range.
static void objectZeroInventoryNetIds(Object* owner)
{
    Inventory* inv = &(owner->data.inventory);
    for (int i = 0; i < inv->length; ++i) {
        Object* item = inv->items[i].item;
        item->netId = 0;
        objectZeroInventoryNetIds(item);
    }
}

void objectAssignAllNetIds()
{
    objectSetNextNetId(1);

    // Player actors are numbered FIRST, in registry slot order, ahead of the
    // tile walk (MP_PROPOSAL.md Ch 5.1). With nothing registered this is exactly
    // the old "gDude then the walk" shape — playerActorCount() is 1 and slot 0
    // resolves to gDude.
    //
    // ⚠ Slot k does NOT get netId k+1: each actor's inventory is numbered
    // immediately after it. Never infer an actor's netId from its slot — read
    // the roster (EVENT_PLAYER_ROSTER), which is re-announced after every
    // baseline for exactly this reason.
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor != nullptr) {
            actor->netId = objectNextNetId();
            objectAssignInventoryNetIds(actor);
        }
    }

    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        // Domain alignment (CLIENT_JOIN_DESIGN.md §C): the netId walk, the join
        // blob (objectSaveAll, object.cc:674), and the delta layer (objectIsSyncable,
        // object_delta.cc) must number the SAME object set, or a slot mismatch
        // shifts every netId after it (the client cannot then reproduce the walk).
        // OBJECT_NO_SAVE objects (cursors, the egg, recruited party members) are
        // absent from the blob AND excluded from deltas, so they take no netId slot.
        // Player actors are NO_SAVE but ARE primary actors: numbered first,
        // above, and skipped here so they take exactly one slot each.
        if (!playerActorIs(obj) && (obj->flags & OBJECT_NO_SAVE) == 0) {
            obj->netId = objectNextNetId();
            objectAssignInventoryNetIds(obj);
        } else if (!playerActorIs(obj)) {
            // Skipped NO_SAVE object: zero its netId so exactly the syncable set
            // carries a nonzero netId after the walk. A NO_SAVE object that
            // SURVIVES a rebaseline (e.g. gEgg, which follows the dude across
            // maps) would otherwise retain a stale netId that could collide with
            // a syncable object's freshly-renumbered value (1..N) — the wire
            // would then LIE about which object a delta/move addresses. netId 0 =
            // "no object" (presenter_network.cc:97), so such an object's lifecycle
            // events are harmlessly skipped by the decoder. NO_SAVE objects that
            // spawn LATER still get a fresh wire netId at creation (object.cc:761).
            obj->netId = 0;
            objectZeroInventoryNetIds(obj);
        }
        obj = objectFindNext();
    }
}

} // namespace fallout
