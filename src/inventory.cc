#include "inventory.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "actions.h"
#include "animation.h"
#include "art.h"
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
#include "input.h"
#include "interface.h"
#include "inventory_ui.h"
#include "item.h"
#include "kb.h"
#include "light.h"
#include "map.h"
#include "message.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "platform_compat.h"
#include "pres_record.h"
#include "presenter.h"
#include "proto.h"
#include "proto_instance.h"
#include "random.h"
#include "reaction.h"
#include "server_loop.h"
#include "server_players.h"
#include "scripts.h"
#include "skill.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

// 0x519058
Object* _inven_dude = nullptr;

// Probably fid of armor to display in inventory dialog.
//
// 0x51905C
int _inven_pid = -1;

// 0x59E954
Object* gInventoryArmor;

// 0x59E958
Object* gInventoryLeftHandItem;

// item2
// 0x59E968
Object* gInventoryRightHandItem;

// 0x46E724
void _inven_reset_dude()
{
    _inven_dude = gDude;
    _inven_pid = 0x1000000;
}

// This function removes armor bonuses and effects granted by [oldArmor] and
// adds appropriate bonuses and effects granted by [newArmor]. Both [oldArmor]
// and [newArmor] can be NULL.
//
// 0x4715F8
void _adjust_ac(Object* critter, Object* oldArmor, Object* newArmor)
{
    int armorClassBonus = critterGetBonusStat(critter, STAT_ARMOR_CLASS);
    int oldArmorClass = armorGetArmorClass(oldArmor);
    int newArmorClass = armorGetArmorClass(newArmor);
    critterSetBonusStat(critter, STAT_ARMOR_CLASS, armorClassBonus - oldArmorClass + newArmorClass);

    int damageResistanceStat = STAT_DAMAGE_RESISTANCE;
    int damageThresholdStat = STAT_DAMAGE_THRESHOLD;
    for (int damageType = 0; damageType < DAMAGE_TYPE_COUNT; damageType += 1) {
        int damageResistanceBonus = critterGetBonusStat(critter, damageResistanceStat);
        int oldArmorDamageResistance = armorGetDamageResistance(oldArmor, damageType);
        int newArmorDamageResistance = armorGetDamageResistance(newArmor, damageType);
        critterSetBonusStat(critter, damageResistanceStat, damageResistanceBonus - oldArmorDamageResistance + newArmorDamageResistance);

        int damageThresholdBonus = critterGetBonusStat(critter, damageThresholdStat);
        int oldArmorDamageThreshold = armorGetDamageThreshold(oldArmor, damageType);
        int newArmorDamageThreshold = armorGetDamageThreshold(newArmor, damageType);
        critterSetBonusStat(critter, damageThresholdStat, damageThresholdBonus - oldArmorDamageThreshold + newArmorDamageThreshold);

        damageResistanceStat += 1;
        damageThresholdStat += 1;
    }

    if (objectIsPartyMember(critter)) {
        if (oldArmor != nullptr) {
            int perk = armorGetPerk(oldArmor);
            perkRemoveEffect(critter, perk);
        }

        if (newArmor != nullptr) {
            int perk = armorGetPerk(newArmor);
            perkAddEffect(critter, perk);
        }
    }
}

// 0x471B70
Object* critterGetItem2(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (gInventoryRightHandItem != nullptr && critter == _inven_dude) {
        return gInventoryRightHandItem;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_IN_RIGHT_HAND) {
            return item;
        }
    }

    return nullptr;
}

// 0x471BBC
Object* critterGetItem1(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (gInventoryLeftHandItem != nullptr && critter == _inven_dude) {
        return gInventoryLeftHandItem;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_IN_LEFT_HAND) {
            return item;
        }
    }

    return nullptr;
}

// 0x471C08
Object* critterGetArmor(Object* critter)
{
    int i;
    Inventory* inventory;
    Object* item;

    if (gInventoryArmor != nullptr && critter == _inven_dude) {
        return gInventoryArmor;
    }

    inventory = &(critter->data.inventory);
    for (i = 0; i < inventory->length; i++) {
        item = inventory->items[i].item;
        if (item->flags & OBJECT_WORN) {
            return item;
        }
    }

    return nullptr;
}

// 0x471CA0
Object* objectGetCarriedObjectByPid(Object* obj, int pid)
{
    Inventory* inventory = &(obj->data.inventory);

    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (inventoryItem->item->pid == pid) {
            return inventoryItem->item;
        }

        Object* found = objectGetCarriedObjectByPid(inventoryItem->item, pid);
        if (found != nullptr) {
            return found;
        }
    }

    return nullptr;
}

// 0x471CDC
int objectGetCarriedQuantityByPid(Object* object, int pid)
{
    int quantity = 0;

    Inventory* inventory = &(object->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        if (inventoryItem->item->pid == pid) {
            quantity += inventoryItem->quantity;
        }

        quantity += objectGetCarriedQuantityByPid(inventoryItem->item, pid);
    }

    return quantity;
}

// Finds next item of given [itemType] (can be -1 which means any type of
// item).
//
// The [index] is used to control where to continue the search from, -1 - from
// the beginning.
//
// 0x472698
Object* _inven_find_type(Object* obj, int itemType, int* indexPtr)
{
    int dummy = -1;
    if (indexPtr == nullptr) {
        indexPtr = &dummy;
    }

    *indexPtr += 1;

    Inventory* inventory = &(obj->data.inventory);

    // TODO: Refactor with for loop.
    if (*indexPtr >= inventory->length) {
        return nullptr;
    }

    while (itemType != -1 && itemGetType(inventory->items[*indexPtr].item) != itemType) {
        *indexPtr += 1;

        if (*indexPtr >= inventory->length) {
            return nullptr;
        }
    }

    return inventory->items[*indexPtr].item;
}

// Searches for an item with a given id inside given obj's inventory.
//
// 0x4726EC
Object* _inven_find_id(Object* obj, int id)
{
    if (obj->id == id) {
        return obj;
    }

    Inventory* inventory = &(obj->data.inventory);
    for (int index = 0; index < inventory->length; index++) {
        InventoryItem* inventoryItem = &(inventory->items[index]);
        Object* item = inventoryItem->item;
        if (item->id == id) {
            return item;
        }

        if (itemGetType(item) == ITEM_TYPE_CONTAINER) {
            item = _inven_find_id(item, id);
            if (item != nullptr) {
                return item;
            }
        }
    }

    return nullptr;
}

// Returns inventory item at a given index.
//
// 0x472740
Object* _inven_index_ptr(Object* obj, int index)
{
    Inventory* inventory;

    inventory = &(obj->data.inventory);

    if (index < 0 || index >= inventory->length) {
        return nullptr;
    }

    return inventory->items[index].item;
}

// inven_wield
// 0x472758
int _inven_wield(Object* critter, Object* item, int hand)
{
    return _invenWieldFunc(critter, item, hand, true);
}

// 0x472768
int _invenWieldFunc(Object* critter, Object* item, int handIndex, bool animate)
{
    // PRESENTATION-RECORD (weapon-draw family, PRESENTATION_RECORD_REPLAY_SPEC.md §8):
    // capture the wield's reg_anim leaves (put-away + take-out draw) in a record section
    // and ship them as EVENT_PRES_SEQ instead of the combat-only weaponTakeOut cue below.
    // Unlike explosion/damage there is NO !serverLoopActive() gate to relax here — iso is
    // enabled server-side so this animate branch already runs; the section only CAPTURES
    // its leaves. The inline authoritative state (in-hand flags / armed fid / light) still
    // executes (the section restores only RNG, which these leaves don't draw). Ship only an
    // actual weapon TAKE-OUT (recordedDraw); armor / unarmed swaps abort the section — their
    // fid rides OBJECT_DELTA, so shipping would double-apply on the viewer. The section must
    // wrap reg_anim_begin (it records SEQ_BEGIN), so the mid-function -1 returns abort it.
    // Actor = the wielder, so the viewer glues the draw behind any approach glide. Inert
    // (recording=false) in the client/golden binary → goldens byte-identical.
    bool recording = animate && !isoIsDisabled() && presRecordEnabled();
    bool recordedDraw = false;
    if (recording) {
        presRecordSectionBegin();
    }

    if (animate) {
        if (!isoIsDisabled()) {
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        }
    }

    int itemType = itemGetType(item);
    if (itemType == ITEM_TYPE_ARMOR) {
        Object* armor = critterGetArmor(critter);
        if (armor != nullptr) {
            armor->flags &= ~OBJECT_WORN;
        }

        item->flags |= OBJECT_WORN;

        int baseFrmId;
        if (critterGetStat(critter, STAT_GENDER) == GENDER_FEMALE) {
            baseFrmId = armorGetFemaleFid(item);
        } else {
            baseFrmId = armorGetMaleFid(item);
        }

        if (baseFrmId == -1) {
            baseFrmId = 1;
        }

        if (critter == gDude) {
            if (!isoIsDisabled()) {
                int fid = buildFid(OBJ_TYPE_CRITTER, baseFrmId, 0, (critter->fid & 0xF000) >> 12, critter->rotation + 1);
                animationRegisterSetFid(critter, fid, 0);
            }
        } else {
            _adjust_ac(critter, armor, item);
        }
    } else {
        int hand;
        if (critter == gDude) {
            hand = interfaceGetCurrentHand();
        } else {
            hand = HAND_RIGHT;
        }

        // ►►►► THE GATE BELOW IS A WEAPON QUESTION, AND ONLY A WEAPON HAS THE FIELDS IT
        // READS. weaponGetAnimationCode reads proto->item.data.weapon.animationCode — the
        // FIRST INT OF A UNION. On a misc item that is `misc.powerTypePid`, on a drug it is
        // `drug.stat[0]`; the number that comes back is not an animation code, it is
        // whatever the other arm happens to hold. A shovel (pid 289) has powerTypePid -1,
        // so this built a fid with weapon nibble 0xF, artExists said no, and the wield was
        // REFUSED — you could not put a shovel in your hand. A stimpak reads stat[0] = 7
        // and fails the same way. Vanilla gets away with it because its inventory screen
        // never calls this for a hand drop (_switch_hand just parks the item in the UI
        // slot); our co-op viewer routes every hand equip through the invwield verb, so the
        // server runs the wield for real and the garbage read became a player-visible wall.
        //
        // Vanilla's own code, 90 lines below, is the oracle: it recomputes this exact value
        // as `itemGetType(item) == ITEM_TYPE_WEAPON ? weaponGetAnimationCode(item) : 0`.
        // The guard existed; the gate simply predated it. Non-weapon = unarmed art, which
        // every critter has, so the wield proceeds and the hand slot works as it does in
        // single player. (weaponArtSupportedForCritter already refuses to read a non-weapon
        // proto for the same reason — this is that rule applied where it was missing.)
        bool itemIsWeapon = itemType == ITEM_TYPE_WEAPON;
        int weaponAnimationCode = itemIsWeapon ? weaponGetAnimationCode(item) : 0;
        int hitModeAnimationCode = itemIsWeapon
            ? weaponGetAnimationForHitMode(item, HIT_MODE_RIGHT_WEAPON_PRIMARY)
            : ANIM_STAND;
        int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, hitModeAnimationCode, weaponAnimationCode, critter->rotation + 1);
        if (!artExists(fid)) {
            // ►►►► THIS FAILURE WAS INVISIBLE ON THE DEDICATED SERVER, and it silently
            // degrades combat: the critter does NOT equip the weapon, so it fights
            // UNARMED — no wielded weapon on any client, no draw, and punch-range
            // attacks from something the operator believes is armed. Vanilla only
            // debugPrints it, and f2_server never installs gDebugPrintProc, so the line
            // goes nowhere ([[presentation-backpressure-gap]]'s gotcha).
            //
            // The cause is ART, not logic: this critter's base body has no animation for
            // (hit-mode, weapon) — a tribal/civilian frame set with a rifle, say. Already
            // confirmed once in this codebase: it is why the check.sh combat gates fought
            // unarmed on klatoxcv (@3d5021f).
            debugPrint("\ninven_wield failed!  ERROR ERROR ERROR!");
            fprintf(stderr, "f2_server: inven_wield FAILED (art missing) critter_net=%d pid=%d "
                            "item_pid=%d fid=0x%x baseFrm=%d hitAnim=%d weapAnim=%d — it will "
                            "fight UNARMED\n",
                critter->netId, critter->pid, item->pid, fid, critter->fid & 0xFFF,
                hitModeAnimationCode, weaponAnimationCode);
            if (recording) {
                presRecordSectionAbort();
            }
            return -1;
        }

        Object* v17;
        if (handIndex) {
            v17 = critterGetItem2(critter);
            item->flags |= OBJECT_IN_RIGHT_HAND;
        } else {
            v17 = critterGetItem1(critter);
            item->flags |= OBJECT_IN_LEFT_HAND;
        }
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[wield] critter_net=%d item_pid=%d hand=%d animate=%d\n",
                critter->netId, item->pid, handIndex, animate);
        }

        Rect rect;
        if (v17 != nullptr) {
            v17->flags &= ~OBJECT_IN_ANY_HAND;

            if (v17->pid == PROTO_ID_LIT_FLARE) {
                int lightIntensity;
                int lightDistance;
                if (critter == gDude) {
                    lightIntensity = LIGHT_INTENSITY_MAX;
                    lightDistance = 4;
                } else {
                    Proto* proto;
                    if (protoGetProto(critter->pid, &proto) == -1) {
                        if (recording) {
                            presRecordSectionAbort();
                        }
                        return -1;
                    }

                    lightDistance = proto->lightDistance;
                    lightIntensity = proto->lightIntensity;
                }

                objectSetLight(critter, lightDistance, lightIntensity, &rect);
            }
        }

        if (item->pid == PROTO_ID_LIT_FLARE) {
            int lightDistance = item->lightDistance;
            if (lightDistance < critter->lightDistance) {
                lightDistance = critter->lightDistance;
            }

            int lightIntensity = item->lightIntensity;
            if (lightIntensity < critter->lightIntensity) {
                lightIntensity = critter->lightIntensity;
            }

            objectSetLight(critter, lightDistance, lightIntensity, &rect);
            presenter()->worldInvalidateRect(&rect, gElevation);
        }

        if (itemGetType(item) == ITEM_TYPE_WEAPON) {
            weaponAnimationCode = weaponGetAnimationCode(item);
        } else {
            weaponAnimationCode = 0;
        }

        if (hand == handIndex) {
            // Present the weapon draw to remote viewers when it happens in combat.
            // The wield itself (in-hand flags) rides OBJECT_DELTA_INVENTORY; the
            // draw anim is now always recorded and shipped as EVENT_PRES_SEQ below.
            if (isInCombat() && weaponAnimationCode != 0) {
            }

            // Server-authoritative armed fid (COMBAT_CLIENT_DESIGN.md §6, Fable review).
            // On the server the take-out below is a no-op AND _action_attack is skipped,
            // so without this the critter's fid never reflects its drawn weapon server-
            // side: a mid-fight joiner would see an unarmed pose holding a gun, and
            // put-away/swap would have no authoritative final fid. Set the armed stand
            // fid directly (same fid the vanilla non-animate branch's _dude_stand uses);
            // it rides OBJECT_DELTA, and viewers HOLD it during the draw replay. Server-
            // only (client/goldens use the animate path); combat-scoped to match the emit.
            if (serverLoopActive() && isInCombat()) {
                int armedFid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_STAND, weaponAnimationCode, critter->rotation + 1);
                objectSetFid(critter, armedFid, nullptr);
                objectSetFrame(critter, 0, nullptr);
                if (getenv("F2_TRACE_EVENTS") != nullptr) {
                    fprintf(stderr, "[swield-armfid] net=%d armedFid=0x%x wpnCode=%d\n",
                        critter->netId, armedFid, weaponAnimationCode);
                }
            }

            if ((critter->fid & 0xF000) >> 12 != 0) {
                if (animate) {
                    if (!isoIsDisabled()) {
                        const char* soundEffectName = sfxBuildCharName(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
                        animationRegisterPlaySoundEffect(critter, soundEffectName, 0);
                        animationRegisterAnimate(critter, ANIM_PUT_AWAY, 0);
                    }
                }
            }

            if (animate && !isoIsDisabled()) {
                if (weaponAnimationCode != 0) {
                    animationRegisterTakeOutWeapon(critter, weaponAnimationCode, -1);
                    recordedDraw = true; // a real draw — ship the recorded section below
                } else {
                    int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, 0, critter->rotation + 1);
                    animationRegisterSetFid(critter, fid, -1);
                }
            } else {
                int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, weaponAnimationCode, critter->rotation + 1);
                _dude_stand(critter, critter->rotation, fid);
            }
        }
    }

    if (animate) {
        if (!isoIsDisabled()) {
            int rc = reg_anim_end();
            if (recording) {
                if (recordedDraw) {
                    presRecordSectionEnd();
                    presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount(), critter->netId);
                } else {
                    // Armor / unarmed swap: nothing to replay (fid rides OBJECT_DELTA).
                    presRecordSectionAbort();
                }
            }
            return rc;
        }
    }

    if (recording) {
        // recording implies animate && !isoIsDisabled(), so the branch above returned;
        // this only balances the section if that invariant ever changes.
        presRecordSectionAbort();
    }
    return 0;
}

// inven_unwield
// 0x472A54
int _inven_unwield(Object* critter_obj, int hand)
{
    return _invenUnwieldFunc(critter_obj, hand, true);
}

// 0x472A64
// Re-derive the WEAPON half of a critter's fid from what is actually in its hands.
//
// Bits 0xF000 of the fid are what the SPRITE is holding, and they are normally written by
// wield/unwield EVENTS. Any path that removes a weapon without going through those events
// leaves the sprite holding a gun that no longer exists — owner-reported after a critical
// destroyed a shotgun: "no weapon equipped, but my character is still holding some kind of
// rifle." Asking "what is in the hands NOW?" cannot go stale the way an event can.
//
// ►► And unlike _invenUnwieldFunc this needs no agreement about which hand is ACTIVE. That
// agreement is exactly what broke the first attempt at this fix: the client's interface said
// LEFT while the server registry's default said RIGHT, so `activeHand == hand` failed and the
// re-derive never ran. A post-condition ("nothing armed, so show nothing") has no such
// dependency. When both hands hold weapons the server registry supplies the active hand;
// elsewhere an already-matching fid is preserved, so this remains a safe post-condition.
void invenRederiveWeaponFid(Object* critter)
{
    if (critter == nullptr || FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    Object* leftItem = critterGetItem1(critter);
    Object* rightItem = critterGetItem2(critter);
    bool leftIsWeapon = leftItem != nullptr && itemGetType(leftItem) == ITEM_TYPE_WEAPON;
    bool rightIsWeapon = rightItem != nullptr && itemGetType(rightItem) == ITEM_TYPE_WEAPON;

    int weaponAnimationCode;
    if (!leftIsWeapon && !rightIsWeapon) {
        weaponAnimationCode = 0;
    } else if (leftIsWeapon != rightIsWeapon) {
        weaponAnimationCode = weaponGetAnimationCode(leftIsWeapon ? leftItem : rightItem);
    } else {
        int currentCode = (critter->fid & 0xF000) >> 12;
        int leftCode = weaponGetAnimationCode(leftItem);
        int rightCode = weaponGetAnimationCode(rightItem);
        if (currentCode == leftCode || currentCode == rightCode) {
            return;
        }
        if (serverDedicatedActive() && playerActorIs(critter)) {
            weaponAnimationCode = serverActorActiveHand(playerActorSlotOf(critter)) == HAND_LEFT
                ? leftCode
                : rightCode;
        } else {
            return;
        }
    }

    if (((critter->fid & 0xF000) >> 12) == weaponAnimationCode) {
        return;
    }

    // objectSetFrame because objectSetFid does NOT reset obj->frame, and a frame index past
    // the new art's count renders NOTHING ([[frame-index-render-gotcha]]).
    int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_STAND, weaponAnimationCode,
        (critter->fid & 0x70000000) >> 28);
    _dude_stand(critter, critter->rotation, fid);
    objectSetFrame(critter, 0, nullptr);
}

int _invenUnwieldFunc(Object* critter, int hand, bool animate)
{
    int activeHand;
    Object* item;
    int fid;

    // The ACTIVE hand decides whether this unwield touches the fid at all — only the active
    // hand's weapon is on the SPRITE (fid bits 0xF000). Three answers, in authority order:
    //
    // ►► A PLAYER ACTOR ON A DEDICATED SERVER: the per-seat registry value (`hand` verb).
    // interfaceGetCurrentHand() is a STUB there — `return HAND_LEFT`, server_stubs.cc, and
    // already noted as a banked gap in that file — so asking it made every RIGHT-hand
    // unwield fail `activeHand == hand` and skip the fid re-derive below. Owner-reported
    // symptom: "no weapon equipped, but my character is still holding some kind of rifle."
    // It is a shared-path defect, not a co-op one; a caller working around it (destroying a
    // weapon and then re-deriving the fid itself) would just fork this function.
    //
    // gDude ON A CLIENT keeps its own interface, so single-player and the goldens are
    // untouched; anyone else keeps vanilla's NPCs-only-use-the-right-slot assumption.
    if (serverDedicatedActive() && playerActorIs(critter)) {
        activeHand = serverActorActiveHand(playerActorSlotOf(critter));
    } else if (critter == gDude) {
        activeHand = interfaceGetCurrentHand();
    } else {
        activeHand = HAND_RIGHT; // NPC's only ever use right slot
    }

    if (hand) {
        item = critterGetItem2(critter);
    } else {
        item = critterGetItem1(critter);
    }

    if (item) {
        item->flags &= ~OBJECT_IN_ANY_HAND;
    }

    if (activeHand == hand && ((critter->fid & 0xF000) >> 12) != 0) {
        if (animate && !isoIsDisabled()) {
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);

            const char* sfx = sfxBuildCharName(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
            animationRegisterPlaySoundEffect(critter, sfx, 0);

            animationRegisterAnimate(critter, ANIM_PUT_AWAY, 0);

            fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, 0, critter->rotation + 1);
            animationRegisterSetFid(critter, fid, -1);

            return reg_anim_end();
        }

        fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, 0, 0, critter->rotation + 1);
        _dude_stand(critter, critter->rotation, fid);
    }

    return 0;
}

} // namespace fallout
