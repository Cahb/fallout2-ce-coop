#include <stdio.h>
#include <string.h>

// animation.h is included for the ANIM_* enum only (no client symbol is named
// here) — the same way core TUs like combat_ai.cc/actions.cc already use it.
#include "animation.h"
#include "art.h"
#include "combat.h"
#include "game_sound.h"
#include "item.h"
#include "object.h"
#include "platform_compat.h"
#include "proto.h"

namespace fallout {

// Moved here with sfxBuildSceneryName, its only user (was a file-local enum in
// game_sound.cc).
typedef enum SoundEffectActionType {
    SOUND_EFFECT_ACTION_TYPE_ACTIVE,
    SOUND_EFFECT_ACTION_TYPE_PASSIVE,
} SoundEffectActionType;

// The sfx NAME BUILDERS, split out of the client's game_sound.cc playback TU
// ([[p5-cut-list]] tail, category C "relocate": the same H4 art lesson —
// when the closure of a scoped facade is already core-resident, move the whole
// family rather than build a seam).
//
// These six functions are pure string composition over proto/art metadata
// (artCopyFileName/_art_get_code, protoGetProto, weaponGetSoundId/DamageType,
// compat_strupr) — every one of those is resident in f2_core, and none of this
// touches SDL, the audio device, or a window. Only the *playing* of the named
// file is presentation.
//
// They MUST live in core rather than become no-op stubs: the sfx_build_*_name
// script opcodes (interpreter_extra.cc) do `strcpy(dst, sfxBuildXName(...))`,
// so a stub returning nullptr is a crash, and one returning "" would silently
// feed scripts a wrong name. The server composes the real name and hands it to
// clients to play (a future NetworkPresenter sfx event).

// 0x518E60
static char _snd_lookup_weapon_type[WEAPON_SOUND_EFFECT_COUNT] = {
    'R', // Ready
    'A', // Attack
    'O', // Out of ammo
    'F', // Firing
    'H', // Hit
};

// 0x518E65
static char _snd_lookup_scenery_action[SCENERY_SOUND_EFFECT_COUNT] = {
    'O', // Open
    'C', // Close
    'L', // Lock
    'N', // Unlock
    'U', // Use
};

// 0x596FB5
static char _sfx_file_name[13];

// sfx_build_char_name
// 0x451604
char* sfxBuildCharName(Object* a1, int anim, int extra)
{
    char v7[13];
    char v8;
    char v9;

    if (artCopyFileName(FID_TYPE(a1->fid), a1->fid & 0xFFF, v7) == -1) {
        return nullptr;
    }

    if (anim == ANIM_TAKE_OUT) {
        if (_art_get_code(anim, extra, &v8, &v9) == -1) {
            return nullptr;
        }
    } else {
        if (_art_get_code(anim, (a1->fid & 0xF000) >> 12, &v8, &v9) == -1) {
            return nullptr;
        }
    }

    // TODO: Check.
    if (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK) {
        if (extra == CHARACTER_SOUND_EFFECT_PASS_OUT) {
            v8 = 'Y';
        } else if (extra == CHARACTER_SOUND_EFFECT_DIE) {
            v8 = 'Z';
        }
    } else if ((anim == ANIM_THROW_PUNCH || anim == ANIM_KICK_LEG) && extra == CHARACTER_SOUND_EFFECT_CONTACT) {
        v8 = 'Z';
    }

    snprintf(_sfx_file_name, sizeof(_sfx_file_name), "%s%c%c", v7, v8, v9);
    compat_strupr(_sfx_file_name);
    return _sfx_file_name;
}

// sfx_build_ambient_name
// 0x4516F0
char* gameSoundBuildAmbientSoundEffectName(const char* a1)
{
    snprintf(_sfx_file_name, sizeof(_sfx_file_name), "A%6s%1d", a1, 1);
    compat_strupr(_sfx_file_name);
    return _sfx_file_name;
}

// sfx_build_interface_name
// 0x451718
char* gameSoundBuildInterfaceName(const char* a1)
{
    snprintf(_sfx_file_name, sizeof(_sfx_file_name), "N%6s%1d", a1, 1);
    compat_strupr(_sfx_file_name);
    return _sfx_file_name;
}

// sfx_build_weapon_name
// 0x451760
char* sfxBuildWeaponName(int effectType, Object* weapon, int hitMode, Object* target)
{
    int soundVariant;
    char weaponSoundCode;
    char effectTypeCode;
    char materialCode;
    Proto* proto;

    weaponSoundCode = weaponGetSoundId(weapon);
    effectTypeCode = _snd_lookup_weapon_type[effectType];

    if (effectType != WEAPON_SOUND_EFFECT_READY
        && effectType != WEAPON_SOUND_EFFECT_OUT_OF_AMMO) {
        if (hitMode != HIT_MODE_LEFT_WEAPON_PRIMARY
            && hitMode != HIT_MODE_RIGHT_WEAPON_PRIMARY
            && hitMode != HIT_MODE_PUNCH) {
            soundVariant = 2;
        } else {
            soundVariant = 1;
        }
    } else {
        soundVariant = 1;
    }

    int damageType = weaponGetDamageType(nullptr, weapon);

    // SFALL
    if (effectTypeCode != 'H' || target == nullptr || damageType == explosionGetDamageType() || damageType == DAMAGE_TYPE_PLASMA || damageType == DAMAGE_TYPE_EMP) {
        materialCode = 'X';
    } else {
        const int type = FID_TYPE(target->fid);
        int material;
        switch (type) {
        case OBJ_TYPE_ITEM:
            protoGetProto(target->pid, &proto);
            material = proto->item.material;
            break;
        case OBJ_TYPE_SCENERY:
            protoGetProto(target->pid, &proto);
            material = proto->scenery.field_2C;
            break;
        case OBJ_TYPE_WALL:
            protoGetProto(target->pid, &proto);
            material = proto->wall.material;
            break;
        default:
            material = -1;
            break;
        }

        switch (material) {
        case MATERIAL_TYPE_GLASS:
        case MATERIAL_TYPE_METAL:
        case MATERIAL_TYPE_PLASTIC:
            materialCode = 'M';
            break;
        case MATERIAL_TYPE_WOOD:
            materialCode = 'W';
            break;
        case MATERIAL_TYPE_DIRT:
        case MATERIAL_TYPE_STONE:
        case MATERIAL_TYPE_CEMENT:
            materialCode = 'S';
            break;
        default:
            materialCode = 'F';
            break;
        }
    }

    snprintf(_sfx_file_name, sizeof(_sfx_file_name), "W%c%c%1d%cXX%1d", effectTypeCode, weaponSoundCode, soundVariant, materialCode, 1);
    compat_strupr(_sfx_file_name);
    return _sfx_file_name;
}

// sfx_build_scenery_name
// 0x451898
char* sfxBuildSceneryName(int actionType, int action, const char* name)
{
    char actionTypeCode = actionType == SOUND_EFFECT_ACTION_TYPE_PASSIVE ? 'P' : 'A';
    char actionCode = _snd_lookup_scenery_action[action];

    snprintf(_sfx_file_name, sizeof(_sfx_file_name), "S%c%c%4s%1d", actionTypeCode, actionCode, name, 1);
    compat_strupr(_sfx_file_name);

    return _sfx_file_name;
}

// sfx_build_open_name
// 0x4518D
char* sfxBuildOpenName(Object* object, int action)
{
    if (FID_TYPE(object->fid) == OBJ_TYPE_SCENERY) {
        char scenerySoundId;
        Proto* proto;
        if (protoGetProto(object->pid, &proto) != -1) {
            scenerySoundId = proto->scenery.field_34;
        } else {
            scenerySoundId = 'A';
        }
        snprintf(_sfx_file_name, sizeof(_sfx_file_name), "S%cDOORS%c", _snd_lookup_scenery_action[action], scenerySoundId);
    } else {
        Proto* proto;
        protoGetProto(object->pid, &proto);
        snprintf(_sfx_file_name, sizeof(_sfx_file_name), "I%cCNTNR%c", _snd_lookup_scenery_action[action], proto->item.field_80);
    }
    compat_strupr(_sfx_file_name);
    return _sfx_file_name;
}

} // namespace fallout
