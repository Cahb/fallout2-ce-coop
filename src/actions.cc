#include "actions.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "animation.h"
#include "art.h"
#include "client_net.h" // clientViewerActive — suppress AI chatter during wire replay (§3.c)
#include "color.h"
#include "combat.h"
#include "combat_ap.h"
#include "server_players.h"
#include "combat_ai.h"
#include "config.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "game_sound.h"
#include "geometry.h"
#include "interface.h"
#include "item.h"
#include "map.h"
#include "memory.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "pres_record.h"
#include "presenter.h"
#include "proto.h"
#include "proto_instance.h"
#include "proto_types.h"
#include "random.h"
#include "scripts.h"
#include "server_loop.h"
#include "settings.h"
#include "sfall_config.h"
#include "skill.h"
#include "stat.h"
#include "text_object.h"
#include "tile.h"
#include "trait.h"

namespace fallout {

#define MAX_KNOCKDOWN_DISTANCE 20

typedef enum ScienceRepairTargetType {
    SCIENCE_REPAIR_TARGET_TYPE_DEFAULT,
    SCIENCE_REPAIR_TARGET_TYPE_DUDE,
    SCIENCE_REPAIR_TARGET_TYPE_ANYONE,
} ScienceRepairTargetType;

// 0x5106D0
static bool _action_in_explode = false;

// 0x5106D4
int rotation;

// 0x5106E0
static const int gNormalDeathAnimations[DAMAGE_TYPE_COUNT] = {
    ANIM_DANCING_AUTOFIRE,
    ANIM_SLICED_IN_HALF,
    ANIM_CHARRED_BODY,
    ANIM_CHARRED_BODY,
    ANIM_ELECTRIFY,
    ANIM_FALL_BACK,
    ANIM_BIG_HOLE,
};

// 0x5106FC
static const int gMaximumBloodDeathAnimations[DAMAGE_TYPE_COUNT] = {
    ANIM_CHUNKS_OF_FLESH,
    ANIM_SLICED_IN_HALF,
    ANIM_FIRE_DANCE,
    ANIM_MELTED_TO_NOTHING,
    ANIM_ELECTRIFIED_TO_NOTHING,
    ANIM_FALL_BACK,
    ANIM_EXPLODED_TO_NOTHING,
};

static int actionKnockdown(Object* obj, int* anim, int maxDistance, int rotation, int delay);
static int _action_blood(Object* obj, int anim, int delay);
static int _pick_death(Object* attacker, Object* defender, Object* weapon, int damage, int attackerAnimation, bool hitFromFront);
static int _check_death(Object* obj, int anim, int minViolenceLevel, bool hitFromFront);
static int _internal_destroy(Object* a1, Object* a2);
static void _show_damage_to_object(Object* defender, int damage, int flags, Object* weapon, bool hitFromFront, int knockbackDistance, int knockbackRotation, int attackerAnimation, Object* attacker, int delay);
static int _show_death(Object* obj, int anim);
static int _show_damage_extras(Attack* attack);
static void _show_damage(Attack* attack, int attackerAnimation, int delay);
static int _action_melee(Attack* attack, int a2);
static int _action_ranged(Attack* attack, int a2);
static int _is_next_to(Object* a1, Object* a2);
static int _action_climb_ladder(Object* a1, Object* a2);
static int _action_use_skill_in_combat_error(Object* critter);
static int _pick_fall(Object* obj, int anim);
static int _report_explosion(Attack* attack, Object* sourceObj);
static int _finished_explosion(Object* a1, Object* a2);
static int _compute_explosion_damage(int min, int max, Object* defender, int* knockbackDistancePtr);
static int _can_talk_to(Object* a1, Object* a2);
static int _talk_to(Object* a1, Object* a2);
static int _report_dmg(Attack* attack, Object* a2);
static int _compute_dmg_damage(int min, int max, Object* obj, int* knockbackDistancePtr, int damageType);

static int hideProjectile(void* a1, void* a2);

// 0x410468
// Computes the tile a knockback of up to `maxDistance` in `rotation` displaces
// `obj` to, stopping one tile short of the first blocking object or exit grid.
// Pure geometry — no sound/animation/side effects — so both the animated client
// path (actionKnockdown) and the headless server path (_combat_knockback_headless)
// share one source of truth for where a knocked-back critter lands. Returns
// obj->tile when the critter cannot be pushed at all.
int _knockback_dest_tile(Object* obj, int maxDistance, int rotation)
{
    // SFALL: Fix to limit the maximum distance for the knockback animation.
    if (maxDistance > MAX_KNOCKDOWN_DISTANCE) {
        maxDistance = MAX_KNOCKDOWN_DISTANCE;
    }

    int distance;
    int tile;
    for (distance = 1; distance <= maxDistance; distance++) {
        tile = tileGetTileInDirection(obj->tile, rotation, distance);
        if (_obj_blocking_at(obj, tile, obj->elevation) != nullptr) {
            distance--;
            break;
        }

        // CE: Fix to prevent critters (including player) cross an exit grid as
        // a result of knockback. Sfall has similar fix done differently and it
        // affects the player only. This approach is better since it also
        // prevents unreachable (=unlootable) corpses on exit grids.
        if (isExitGridAt(tile, obj->elevation)) {
            distance--;
            break;
        }
    }

    // TODO: Check, probably step back because we've started with 1?
    distance--;

    if (distance <= 0) {
        return obj->tile;
    }

    return tileGetTileInDirection(obj->tile, rotation, distance);
}

int actionKnockdown(Object* obj, int* anim, int maxDistance, int rotation, int delay)
{
    if (_critter_flag_check(obj->pid, CRITTER_NO_KNOCKBACK)) {
        return -1;
    }

    if (*anim == ANIM_FALL_FRONT) {
        int fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, *anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (!artExists(fid)) {
            *anim = ANIM_FALL_BACK;
        }
    }

    const char* soundEffectName = sfxBuildCharName(obj, *anim, CHARACTER_SOUND_EFFECT_KNOCKDOWN);
    animationRegisterPlaySoundEffect(obj, soundEffectName, delay);

    int tile = _knockback_dest_tile(obj, maxDistance, rotation);

    if (tile == obj->tile) {
        animationRegisterAnimate(obj, *anim, 0);
    } else {
        animationRegisterMoveToTileStraightAndWaitForComplete(obj, tile, obj->elevation, *anim, 0);
    }

    return tile;
}

// Computes where the knockback that _show_damage_to_object applies via
// actionKnockdown would displace `critter` to, but WITHOUT animation or side
// effects — for the headless server, which skips the whole display sequence.
// `attackerTile` is the push origin (rotation away from it), matching the
// tileGetRotationTo(attacker->tile, victim->tile) the display code computes.
// Returns critter->tile when the critter is not displaced.
//
// Scoped to NON-DEAD critters: a dead critter is finalized flat/OBJECT_NO_BLOCK
// by critterKill, and whether its corpse is knocked back depends on the RNG/art-
// chosen death animation (_pick_death picks FALL_FRONT/FALL_BACK vs explode/melt/
// burn), which is out of v1 scope. A skipped dead-corpse knockback only shifts a
// non-blocking corpse's loot tile, never sim-blocking geometry.
static int _knockback_headless_dest(Object* critter, int flags, int knockbackDistance, int attackerTile)
{
    if (critter == nullptr || knockbackDistance == 0) {
        return critter != nullptr ? critter->tile : -1;
    }

    if ((flags & DAM_DEAD) != 0) {
        return critter->tile;
    }

    if (_critter_flag_check(critter->pid, CRITTER_NO_KNOCKBACK)) {
        return critter->tile;
    }

    // Already-prone critters are not displaced: _show_damage_to_object's whole
    // knockback block is gated on !_critter_is_prone(defender).
    if (_critter_is_prone(critter)) {
        return critter->tile;
    }

    // The fire-dance branch of _show_damage_to_object burns the critter in place
    // and never calls actionKnockdown, so neither do we — otherwise the server
    // would displace a critter the client leaves put.
    if ((flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) == 0
        && (flags & DAM_ON_FIRE) != 0
        && artExists(buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_FIRE_DANCE, (critter->fid & 0xF000) >> 12, critter->rotation + 1))) {
        return critter->tile;
    }

    int rotation = tileGetRotationTo(attackerTile, critter->tile);
    return _knockback_dest_tile(critter, knockbackDistance, rotation);
}

// Applies knockback tile displacement for every non-dead victim of `attack`
// (defender + extras) on the headless server, replacing the actionKnockdown
// moves _show_damage/_show_damage_extras register on the animated path. Runs in
// TWO passes: all destinations are computed against the ORIGINAL board first,
// then applied. This matches the animated path, which REGISTERS every knockback
// move (each actionKnockdown's _obj_blocking_at reads live positions) before any
// move physically executes during animation playback — so a critter that is
// about to be knocked back does not block another victim's path. A single
// apply-as-you-go pass would let victim N's move change what victim N+1 sees.
// The push origin is attack->attacker->tile for all victims (the attacker for
// combat hits, the explosion object's blast-center tile for explosions), matching
// the tileGetRotationTo(attack->attacker->tile, victim->tile) both display sites
// use. Must be called BEFORE _apply_damage / _report_explosion so the prone/knock-
// down guards observe each victim's pre-damage state (see the call sites).
// COMPUTE half: fills victims[]/dests[] (capacity 1 + EXPLOSION_TARGET_COUNT) against
// the ORIGINAL board and returns the count. Reads each victim's PRE-damage prone/knock
// state, so it MUST run before _apply_damage (see the call-site notes). No side effects.
int _combat_compute_knockback(Attack* attack, Object** victims, int* dests)
{
    if (attack->attacker == nullptr) {
        return 0;
    }

    int attackerTile = attack->attacker->tile;
    int count = 0;

    if (attack->defender != nullptr) {
        victims[count] = attack->defender;
        dests[count] = _knockback_headless_dest(attack->defender, attack->defenderFlags, attack->defenderKnockback, attackerTile);
        count++;
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        victims[count] = attack->extras[index];
        dests[count] = _knockback_headless_dest(attack->extras[index], attack->extrasFlags[index], attack->extrasKnockback[index], attackerTile);
        count++;
    }

    return count;
}

// COMMIT half: applies the displacement (the objectSetLocation that emits the EVENT_MOVE).
// Split from compute so the caller can DELAY the emission until AFTER the coupled
// presentation event that reserves the victim (ATTACK_RESULT) — otherwise the knockback
// MOVE reaches the wire before the client has reserved the victim and the client cannot
// hold it, so the recorded slide plays from a stale origin (bug J; pacing design §8.5).
void _combat_commit_knockback(Object** victims, const int* dests, int count)
{
    for (int index = 0; index < count; index++) {
        if (dests[index] != victims[index]->tile) {
            objectSetLocation(victims[index], dests[index], victims[index]->elevation, nullptr);
        }
    }
}

void _combat_apply_knockback(Attack* attack)
{
    // Compute + commit together — the explosion caller (actionExplode) already emits this
    // AFTER its recorded presSeq, so no reorder is needed there. Only the melee/ranged path
    // (_combat_apply_attack_results) splits the two halves around attackResult().
    Object* victims[1 + EXPLOSION_TARGET_COUNT];
    int dests[1 + EXPLOSION_TARGET_COUNT];
    int count = _combat_compute_knockback(attack, victims, dests);
    _combat_commit_knockback(victims, dests, count);
}

// 0x410568
int _action_blood(Object* obj, int anim, int delay)
{
    if (settings.preferences.violence_level == VIOLENCE_LEVEL_NONE) {
        return anim;
    }

    int bloodyAnim;
    if (anim == ANIM_FALL_BACK) {
        bloodyAnim = ANIM_FALL_BACK_BLOOD;
    } else if (anim == ANIM_FALL_FRONT) {
        bloodyAnim = ANIM_FALL_FRONT_BLOOD;
    } else {
        return anim;
    }

    int fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, bloodyAnim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    if (artExists(fid)) {
        animationRegisterAnimate(obj, bloodyAnim, delay);
    } else {
        bloodyAnim = anim;
    }

    return bloodyAnim;
}

// 0x41060C
int _pick_death(Object* attacker, Object* defender, Object* weapon, int damage, int attackerAnimation, bool hitFromFront)
{
    int normalViolenceLevelDamageThreshold = 15;
    int maximumBloodViolenceLevelDamageThreshold = 45;

    int damageType = weaponGetDamageType(attacker, weapon);

    if (weapon != nullptr && weapon->pid == PROTO_ID_MOLOTOV_COCKTAIL) {
        normalViolenceLevelDamageThreshold = 5;
        maximumBloodViolenceLevelDamageThreshold = 15;
        damageType = DAMAGE_TYPE_FIRE;
        attackerAnimation = ANIM_FIRE_SINGLE;
    }

    if (playerActorIs(attacker) && perkHasRank(attacker, PERK_PYROMANIAC) && damageType == DAMAGE_TYPE_FIRE) {
        normalViolenceLevelDamageThreshold = 1;
        maximumBloodViolenceLevelDamageThreshold = 1;
    }

    if (weapon != nullptr && weaponGetPerk(weapon) == PERK_WEAPON_FLAMEBOY) {
        normalViolenceLevelDamageThreshold /= 3;
        maximumBloodViolenceLevelDamageThreshold /= 3;
    }

    int violenceLevel = settings.preferences.violence_level;

    if (_critter_flag_check(defender->pid, CRITTER_SPECIAL_DEATH)) {
        return _check_death(defender, ANIM_EXPLODED_TO_NOTHING, VIOLENCE_LEVEL_NORMAL, hitFromFront);
    }

    bool hasBloodyMess = false;
    if (playerActorIs(attacker) && traitIsSelected(TRAIT_BLOODY_MESS, attacker)) {
        hasBloodyMess = true;
    }

    // NOTE: Original code is slightly different. There are lots of jumps and
    // conditions. It's easier to set the default in advance, rather than catch
    // it with bunch of "else" statements.
    int deathAnim = ANIM_FALL_BACK;

    if ((attackerAnimation == ANIM_THROW_PUNCH && damageType == DAMAGE_TYPE_NORMAL)
        || attackerAnimation == ANIM_KICK_LEG
        || attackerAnimation == ANIM_THRUST_ANIM
        || attackerAnimation == ANIM_SWING_ANIM
        || (attackerAnimation == ANIM_THROW_ANIM && damageType != DAMAGE_TYPE_EXPLOSION)) {
        if (violenceLevel == VIOLENCE_LEVEL_MAXIMUM_BLOOD && hasBloodyMess) {
            deathAnim = ANIM_BIG_HOLE;
        }
    } else {
        if (attackerAnimation == ANIM_FIRE_SINGLE && damageType == DAMAGE_TYPE_NORMAL) {
            if (violenceLevel == VIOLENCE_LEVEL_MAXIMUM_BLOOD) {
                if (hasBloodyMess || maximumBloodViolenceLevelDamageThreshold <= damage) {
                    deathAnim = ANIM_BIG_HOLE;
                }
            }
        } else {
            if (violenceLevel > VIOLENCE_LEVEL_MINIMAL && (hasBloodyMess || normalViolenceLevelDamageThreshold <= damage)) {
                if (violenceLevel > VIOLENCE_LEVEL_NORMAL && (hasBloodyMess || maximumBloodViolenceLevelDamageThreshold <= damage)) {
                    deathAnim = gMaximumBloodDeathAnimations[damageType];
                    if (_check_death(defender, deathAnim, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hitFromFront) != deathAnim) {
                        deathAnim = gNormalDeathAnimations[damageType];
                    }
                } else {
                    deathAnim = gNormalDeathAnimations[damageType];
                }
            }
        }
    }

    if (!hitFromFront && deathAnim == ANIM_FALL_BACK) {
        deathAnim = ANIM_FALL_FRONT;
    }

    return _check_death(defender, deathAnim, VIOLENCE_LEVEL_NONE, hitFromFront);
}

// 0x410814
int _check_death(Object* obj, int anim, int minViolenceLevel, bool hitFromFront)
{
    int fid;

    if (settings.preferences.violence_level >= minViolenceLevel) {
        fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (artExists(fid)) {
            return anim;
        }
    }

    if (hitFromFront) {
        return ANIM_FALL_BACK;
    }

    fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, ANIM_FALL_FRONT, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
    if (artExists(fid)) {
        return ANIM_FALL_BACK;
    }

    return ANIM_FALL_FRONT;
}

// 0x4108C8
int _internal_destroy(Object* a1, Object* a2)
{
    return _obj_destroy(a2);
}

// TODO: Check very carefully, lots of conditions and jumps.
//
// 0x4108D0
void _show_damage_to_object(Object* defender, int damage, int flags, Object* weapon, bool hitFromFront, int knockbackDistance, int knockbackRotation, int attackerAnimation, Object* attacker, int delay)
{
    int anim;
    int fid;
    const char* sfx_name;

    if (_critter_flag_check(defender->pid, CRITTER_NO_KNOCKBACK)) {
        knockbackDistance = 0;
    }

    anim = FID_ANIM_TYPE(defender->fid);
    if (!_critter_is_prone(defender)) {
        if ((flags & DAM_DEAD) != 0) {
            fid = buildFid(OBJ_TYPE_MISC, 10, 0, 0, 0);
            if (fid == attacker->fid) {
                anim = _check_death(defender, ANIM_EXPLODED_TO_NOTHING, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hitFromFront);
            } else if (attacker->pid == PROTO_ID_0x20001EB) {
                anim = _check_death(defender, ANIM_ELECTRIFIED_TO_NOTHING, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hitFromFront);
            } else if (attacker->fid == FID_0x20001F5) {
                anim = _check_death(defender, attackerAnimation, VIOLENCE_LEVEL_MAXIMUM_BLOOD, hitFromFront);
            } else {
                anim = _pick_death(attacker, defender, weapon, damage, attackerAnimation, hitFromFront);
            }

            if (anim != ANIM_FIRE_DANCE) {
                if (knockbackDistance != 0 && (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK)) {
                    actionKnockdown(defender, &anim, knockbackDistance, knockbackRotation, delay);
                    anim = _action_blood(defender, anim, -1);
                } else {
                    sfx_name = sfxBuildCharName(defender, anim, CHARACTER_SOUND_EFFECT_DIE);
                    animationRegisterPlaySoundEffect(defender, sfx_name, delay);

                    anim = _pick_fall(defender, anim);
                    animationRegisterAnimate(defender, anim, 0);

                    if (anim == ANIM_FALL_FRONT || anim == ANIM_FALL_BACK) {
                        anim = _action_blood(defender, anim, -1);
                    }
                }
            } else {
                fid = buildFid(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_FIRE_DANCE, (defender->fid & 0xF000) >> 12, defender->rotation + 1);
                if (artExists(fid)) {
                    sfx_name = sfxBuildCharName(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                    animationRegisterPlaySoundEffect(defender, sfx_name, delay);

                    // SFALL
                    if (explosionEmitsLight()) {
                        // 0xFFFF0002:
                        // - distance: 2
                        // - intensity: 65535
                        //
                        // NOTE: Change intensity to 65536 (which is on par with
                        // `anim_set_check_light_fix` Sfall's hack).
                        animationRegisterSetLightIntensity(defender, 2, 65536, 0);
                    }

                    animationRegisterAnimate(defender, anim, 0);

                    // SFALL
                    if (explosionEmitsLight()) {
                        // 0x00010000:
                        // - distance: 0
                        // - intensity: 1
                        //
                        // NOTE: Change intensity to 0. I guess using 1 was a
                        // workaround for `anim_set_check_light_fix` hack which
                        // requires two upper bytes to be non-zero to override
                        // default behaviour.
                        animationRegisterSetLightIntensity(defender, 0, 0, -1);
                    }

                    int randomDistance = randomBetween(2, 5);
                    int randomRotation = randomBetween(0, 5);

                    // CE: Fix to prevent critters (including player) to cross
                    // an exit grid as a result of fire dance animation. See
                    // `actionKnockdown` for notes.
                    int rotation = randomRotation;
                    int distance = randomDistance;
                    while (true) {
                        int tile = tileGetTileInDirection(defender->tile, (rotation + randomRotation) % ROTATION_COUNT, distance);
                        if (!isExitGridAt(tile, defender->elevation)) {
                            Object* obstacle = nullptr;
                            _make_straight_path(defender, defender->tile, tile, nullptr, &obstacle, 4);
                            if (obstacle == nullptr) {
                                animationRegisterRotateToTile(defender, tile);
                                animationRegisterMoveToTileStraight(defender, tile, defender->elevation, anim, 0);
                                break;
                            }
                        }

                        if (distance > 0) {
                            distance--;
                        } else if (rotation < ROTATION_COUNT) {
                            rotation++;
                            distance = randomDistance;
                        } else {
                            break;
                        }
                    }
                }

                anim = ANIM_BURNED_TO_NOTHING;
                sfx_name = sfxBuildCharName(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                animationRegisterPlaySoundEffect(defender, sfx_name, -1);
                animationRegisterAnimate(defender, anim, 0);
            }
        } else {
            if ((flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) {
                anim = hitFromFront ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
                sfx_name = sfxBuildCharName(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                animationRegisterPlaySoundEffect(defender, sfx_name, delay);
                if (knockbackDistance != 0) {
                    actionKnockdown(defender, &anim, knockbackDistance, knockbackRotation, 0);
                } else {
                    anim = _pick_fall(defender, anim);
                    animationRegisterAnimate(defender, anim, 0);
                }
            } else if ((flags & DAM_ON_FIRE) != 0 && artExists(buildFid(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_FIRE_DANCE, (defender->fid & 0xF000) >> 12, defender->rotation + 1))) {
                animationRegisterAnimate(defender, ANIM_FIRE_DANCE, delay);

                fid = buildFid(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_STAND, (defender->fid & 0xF000) >> 12, defender->rotation + 1);
                animationRegisterSetFid(defender, fid, -1);
            } else {
                if (knockbackDistance != 0) {
                    anim = hitFromFront ? ANIM_FALL_BACK : ANIM_FALL_FRONT;
                    actionKnockdown(defender, &anim, knockbackDistance, knockbackRotation, delay);
                    if (anim == ANIM_FALL_BACK) {
                        animationRegisterAnimate(defender, ANIM_BACK_TO_STANDING, -1);
                    } else {
                        animationRegisterAnimate(defender, ANIM_PRONE_TO_STANDING, -1);
                    }
                } else {
                    if (hitFromFront || !artExists(buildFid(OBJ_TYPE_CRITTER, defender->fid & 0xFFF, ANIM_HIT_FROM_BACK, (defender->fid & 0xF000) >> 12, defender->rotation + 1))) {
                        anim = ANIM_HIT_FROM_FRONT;
                    } else {
                        anim = ANIM_HIT_FROM_BACK;
                    }

                    sfx_name = sfxBuildCharName(defender, anim, CHARACTER_SOUND_EFFECT_UNUSED);
                    animationRegisterPlaySoundEffect(defender, sfx_name, delay);

                    animationRegisterAnimate(defender, anim, 0);
                }
            }
        }
    } else {
        if ((flags & DAM_DEAD) != 0 && (defender->data.critter.combat.results & DAM_DEAD) == 0) {
            anim = _action_blood(defender, anim, delay);
        } else {
            return;
        }
    }

    if (weapon != nullptr) {
        if ((flags & DAM_EXPLODE) != 0) {
            animationRegisterCallbackForced(defender, weapon, (AnimationCallback*)_obj_drop, -1);
            fid = buildFid(OBJ_TYPE_MISC, 10, 0, 0, 0);
            animationRegisterSetFid(weapon, fid, 0);
            animationRegisterAnimateAndHide(weapon, ANIM_STAND, 0);

            sfx_name = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_HIT, weapon, HIT_MODE_RIGHT_WEAPON_PRIMARY, defender);
            animationRegisterPlaySoundEffect(weapon, sfx_name, 0);

            animationRegisterHideObjectForced(weapon);
        } else if ((flags & DAM_DESTROY) != 0) {
            animationRegisterCallbackForced(defender, weapon, (AnimationCallback*)_internal_destroy, -1);
        } else if ((flags & DAM_DROP) != 0) {
            animationRegisterCallbackForced(defender, weapon, (AnimationCallback*)_obj_drop, -1);
        }
    }

    if ((flags & DAM_DEAD) != 0) {
        // TODO: Get rid of casts.
        animationRegisterCallbackForced(defender, (void*)anim, (AnimationCallback*)_show_death, -1);
    }
}

// 0x410E24
int _show_death(Object* obj, int anim)
{
    Rect tempRect;
    Rect dirtyRect;
    int fid;

    objectGetRect(obj, &dirtyRect);
    if (anim < 48 && anim > 63) {
        fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, anim + 28, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (objectSetFid(obj, fid, &tempRect) == 0) {
            rectUnion(&dirtyRect, &tempRect, &dirtyRect);
        }

        if (objectSetFrame(obj, 0, &tempRect) == 0) {
            rectUnion(&dirtyRect, &tempRect, &dirtyRect);
        }
    }

    if (!_critter_flag_check(obj->pid, CRITTER_FLAT)) {
        obj->flags |= OBJECT_NO_BLOCK;
        if (_obj_toggle_flat(obj, &tempRect) == 0) {
            rectUnion(&dirtyRect, &tempRect, &dirtyRect);
        }
    }

    if (objectDisableOutline(obj, &tempRect) == 0) {
        rectUnion(&dirtyRect, &tempRect, &dirtyRect);
    }

    // ►► STATE, not presentation — and on the dedicated server _show_death never
    // runs at all (server_anim.cc animationRegisterCallbackForced RECORDS the
    // callback and returns without invoking it). The viewer, which DOES replay it,
    // must therefore not perform the drop: doing so dropped the corpse's inventory
    // into the CLIENT'S world only, producing ground items the server had never
    // heard of. Clicking one sent `get <netId>`, and the server — where those items
    // were still inside the corpse — answered "bad target ... ignored". Nobody could
    // ever loot an annihilated critter.
    //
    // The authoritative drop is applied server-side at the record site instead; see
    // actionDeathDropItems below. pres_record.h:193 always specified this half as
    // "guarded"; this is that guard, finally written.
    if (!clientViewerActive()) {
        actionDeathDropItems(obj, anim);
    }

    presenter()->worldInvalidateRect(&dirtyRect, obj->elevation);

    return 0;
}

// Presentation record/replay defunctionalization seam for _show_death (the one
// RECORD callback in the explosion POC, PRESENTATION_RECORD_REPLAY_SPEC.md §6.4).
// _show_death stays static; these two exports let (a) the server recorder identify
// it by pointer and (b) the viewer register the REAL callback from the op stream.

// Callback-pointer export for the server's state-bearing-callback allowlist
// (server_anim.cc). _talk_to stays static — the server only needs to recognise it.
void* actionTalkToCallbackPtr()
{
    return (void*)(AnimationCallback*)_talk_to;
}

// The STATE half of _show_death, callable from both sides of the record seam.
//
// The range is the two ANNIHILATION deaths (the decompile spelled these 30/31;
// animation.h has had the names all along). Vanilla-correct and deliberate: an
// ordinary corpse KEEPS its inventory and is looted as a container, and only a
// body destroyed outright has to dump its contents on the floor because there is
// no corpse left to open.
void actionDeathDropItems(Object* obj, int anim)
{
    if (anim >= ANIM_ELECTRIFIED_TO_NOTHING && anim <= ANIM_EXPLODED_TO_NOTHING
        && !_critter_flag_check(obj->pid, CRITTER_SPECIAL_DEATH)
        && !_critter_flag_check(obj->pid, CRITTER_NO_DROP)) {
        itemDropAll(obj, obj->tile);
    }
}

// The pointer the recorder compares against (server_anim's animationRegister-
// CallbackForced hands the recorder this same (void*)(AnimationCallback*) value).
void* actionShowDeathCallbackPtr()
{
    return (void*)(AnimationCallback*)_show_death;
}

// Replay a recorded CALL{SHOW_DEATH, obj, anim}: register the real _show_death on
// the viewer's own reg_anim, exactly as actionExplode's animate branch does
// (actions.cc:574) — anim rides as (void*)anim, matching the registration site.
void actionPresReplayShowDeath(Object* obj, int anim)
{
    animationRegisterCallbackForced(obj, (void*)(intptr_t)anim, (AnimationCallback*)_show_death, -1);
}

// 0x410FEC
int _show_damage_extras(Attack* attack)
{
    for (int index = 0; index < attack->extrasLength; index++) {
        Object* obj = attack->extras[index];
        if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
            // NOTE: Uninline.
            bool hitFromFront = _is_hit_from_front(attack->attacker, obj);
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
            _register_priority(1);
            int attackerAnimation = critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
            int knockbackRotation = tileGetRotationTo(attack->attacker->tile, obj->tile);
            _show_damage_to_object(obj, attack->extrasDamage[index], attack->extrasFlags[index], attack->weapon, hitFromFront, attack->extrasKnockback[index], knockbackRotation, attackerAnimation, attack->attacker, 0);
            reg_anim_end();
        }
    }

    return 0;
}

// 0x4110AC
void _show_damage(Attack* attack, int attackerAnimation, int delay)
{
    for (int index = 0; index < attack->extrasLength; index++) {
        Object* object = attack->extras[index];
        if (FID_TYPE(object->fid) == OBJ_TYPE_CRITTER) {
            animationRegisterPing(ANIMATION_REQUEST_RESERVED, delay);
            delay = 0;
        }
    }

    if ((attack->attackerFlags & DAM_HIT) == 0) {
        if ((attack->attackerFlags & DAM_CRITICAL) != 0) {
            _show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, attackerAnimation, attack->attacker, -1);
        } else if ((attack->attackerFlags & DAM_BACKWASH) != 0) {
            _show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, attackerAnimation, attack->attacker, -1);
        }
    } else {
        if (attack->defender != nullptr) {
            // NOTE: Uninline.
            bool hitFromFront = _is_hit_from_front(attack->defender, attack->attacker);

            if (FID_TYPE(attack->defender->fid) == OBJ_TYPE_CRITTER) {
                if (attack->attacker->fid == FID_0x20001F5) {
                    int knockbackRotation = tileGetRotationTo(attack->attacker->tile, attack->defender->tile);
                    _show_damage_to_object(attack->defender, attack->defenderDamage, attack->defenderFlags, attack->weapon, hitFromFront, attack->defenderKnockback, knockbackRotation, attackerAnimation, attack->attacker, delay);
                } else {
                    int weaponAnimation = critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
                    int knockbackRotation = tileGetRotationTo(attack->attacker->tile, attack->defender->tile);
                    _show_damage_to_object(attack->defender, attack->defenderDamage, attack->defenderFlags, attack->weapon, hitFromFront, attack->defenderKnockback, knockbackRotation, weaponAnimation, attack->attacker, delay);
                }
            } else {
                tileGetRotationTo(attack->attacker->tile, attack->defender->tile);
                critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
            }
        }

        if ((attack->attackerFlags & DAM_DUD) != 0) {
            _show_damage_to_object(attack->attacker, attack->attackerDamage, attack->attackerFlags, attack->weapon, 1, 0, 0, attackerAnimation, attack->attacker, -1);
        }
    }
}

// 0x411224
int _action_attack(Attack* attack)
{
    if (reg_anim_clear(attack->attacker) == -2) {
        return -1;
    }

    if (reg_anim_clear(attack->defender) == -2) {
        return -1;
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        if (reg_anim_clear(attack->extras[index]) == -2) {
            return -1;
        }
    }

    int anim = critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
    if (anim < ANIM_FIRE_SINGLE && anim != ANIM_THROW_ANIM) {
        return _action_melee(attack, anim);
    } else {
        return _action_ranged(attack, anim);
    }
}

// 0x4112B4
int _action_melee(Attack* attack, int anim)
{
    int fid;
    Art* art;
    CacheEntry* cache_entry;
    int delay;
    const char* sfx_name;
    char sfx_name_temp[16];

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    _register_priority(1);

    fid = buildFid(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, anim, (attack->attacker->fid & 0xF000) >> 12, attack->attacker->rotation + 1);
    art = artLock(fid, &cache_entry);
    if (art != nullptr) {
        delay = artGetActionFrame(art);
    } else {
        delay = 0;
    }
    artUnlock(cache_entry);

    tileGetTileInDirection(attack->attacker->tile, attack->attacker->rotation, 1);
    animationRegisterRotateToTile(attack->attacker, attack->defender->tile);

    if (anim != ANIM_THROW_PUNCH && anim != ANIM_KICK_LEG) {
        sfx_name = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_ATTACK, attack->weapon, attack->hitMode, attack->defender);
    } else {
        sfx_name = sfxBuildCharName(attack->attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    }

    strcpy(sfx_name_temp, sfx_name);

    // The viewer replays this sequence from a wire ATTACK_RESULT; the server already
    // streams the real AI taunts as floatText, and the mirror does not faithfully
    // hold the AI packet _combatai_msg reads — so suppress the local chatter (§3.c).
    if (!clientViewerActive()) {
        _combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_ATTACK, 0);
    }

    if (attack->attackerFlags & 0x0300) {
        animationRegisterPlaySoundEffect(attack->attacker, sfx_name_temp, 0);
        if (anim != ANIM_THROW_PUNCH && anim != ANIM_KICK_LEG) {
            sfx_name = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_HIT, attack->weapon, attack->hitMode, attack->defender);
        } else {
            sfx_name = sfxBuildCharName(attack->attacker, anim, CHARACTER_SOUND_EFFECT_CONTACT);
        }

        strcpy(sfx_name_temp, sfx_name);

        animationRegisterAnimate(attack->attacker, anim, 0);
        animationRegisterPlaySoundEffect(attack->attacker, sfx_name_temp, delay);
        _show_damage(attack, anim, 0);
    } else {
        if (attack->defender->data.critter.combat.results & 0x03) {
            animationRegisterPlaySoundEffect(attack->attacker, sfx_name_temp, -1);
            animationRegisterAnimate(attack->attacker, anim, 0);
        } else {
            fid = buildFid(OBJ_TYPE_CRITTER, attack->defender->fid & 0xFFF, ANIM_DODGE_ANIM, (attack->defender->fid & 0xF000) >> 12, attack->defender->rotation + 1);
            art = artLock(fid, &cache_entry);
            if (art != nullptr) {
                int dodgeDelay = artGetActionFrame(art);
                artUnlock(cache_entry);

                if (dodgeDelay <= delay) {
                    animationRegisterPlaySoundEffect(attack->attacker, sfx_name_temp, -1);
                    animationRegisterAnimate(attack->attacker, anim, 0);

                    sfx_name = sfxBuildCharName(attack->defender, ANIM_DODGE_ANIM, CHARACTER_SOUND_EFFECT_UNUSED);
                    animationRegisterPlaySoundEffect(attack->defender, sfx_name, delay - dodgeDelay);
                    animationRegisterAnimate(attack->defender, ANIM_DODGE_ANIM, 0);
                } else {
                    sfx_name = sfxBuildCharName(attack->defender, ANIM_DODGE_ANIM, CHARACTER_SOUND_EFFECT_UNUSED);
                    animationRegisterPlaySoundEffect(attack->defender, sfx_name, -1);
                    animationRegisterAnimate(attack->defender, ANIM_DODGE_ANIM, 0);
                    animationRegisterPlaySoundEffect(attack->attacker, sfx_name_temp, dodgeDelay - delay);
                    animationRegisterAnimate(attack->attacker, anim, 0);
                }
            }
        }
    }

    if (!clientViewerActive()) { // wire replay: server streams the real taunts (§3.c)
        if ((attack->attackerFlags & DAM_HIT) != 0) {
            if ((attack->defenderFlags & DAM_DEAD) == 0) {
                _combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_HIT, -1);
            }
        } else {
            _combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_MISS, -1);
        }
    }

    if (reg_anim_end() == -1) {
        return -1;
    }

    _show_damage_extras(attack);

    return 0;
}

// 0x411600
int _action_ranged(Attack* attack, int anim)
{
    Object* adjacentObjects[ROTATION_COUNT];
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        adjacentObjects[rotation] = nullptr;
    }

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    _register_priority(1);

    Object* projectile = nullptr;
    Object* replacedWeapon = nullptr;
    int weaponFid = -1;

    Proto* weaponProto;
    Object* weapon = attack->weapon;
    protoGetProto(weapon->pid, &weaponProto);

    int fid = buildFid(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, anim, (attack->attacker->fid & 0xF000) >> 12, attack->attacker->rotation + 1);
    CacheEntry* artHandle;
    Art* art = artLock(fid, &artHandle);
    int delay = (art != nullptr) ? artGetActionFrame(art) : 0;
    artUnlock(artHandle);

    weaponGetRange(attack->attacker, attack->hitMode);

    int damageType = weaponGetDamageType(attack->attacker, attack->weapon);

    tileGetTileInDirection(attack->attacker->tile, attack->attacker->rotation, 1);

    animationRegisterRotateToTile(attack->attacker, attack->defender->tile);

    bool isGrenade = false;
    if (anim == ANIM_THROW_ANIM) {
        // SFALL
        if (damageType == explosionGetDamageType() || damageType == DAMAGE_TYPE_PLASMA || damageType == DAMAGE_TYPE_EMP) {
            isGrenade = true;
        }
    } else {
        animationRegisterAnimate(attack->attacker, ANIM_POINT, -1);
    }

    if (!clientViewerActive()) { // wire replay: server streams the real taunts (§3.c)
        _combatai_msg(attack->attacker, attack, AI_MESSAGE_TYPE_ATTACK, 0);
    }

    const char* sfx;
    if (((attack->attacker->fid & 0xF000) >> 12) != 0) {
        sfx = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_ATTACK, weapon, attack->hitMode, attack->defender);
    } else {
        sfx = sfxBuildCharName(attack->attacker, anim, CHARACTER_SOUND_EFFECT_UNUSED);
    }
    animationRegisterPlaySoundEffect(attack->attacker, sfx, -1);

    animationRegisterAnimate(attack->attacker, anim, 0);

    if (anim != ANIM_FIRE_CONTINUOUS) {
        if ((attack->attackerFlags & DAM_HIT) != 0 || (attack->attackerFlags & DAM_CRITICAL) == 0) {
            bool l56 = false;

            int projectilePid = weaponGetProjectilePid(weapon);
            Proto* projectileProto;
            if (protoGetProto(projectilePid, &projectileProto) != -1 && projectileProto->fid != -1) {
                if (anim == ANIM_THROW_ANIM && !presRecordActive()) {
                    // Vanilla animated throw (SP / client): the weapon object IS the
                    // projectile, and the inventory mutation (consume + auto-equip next +
                    // drop into the world) runs inline here. NOT taken on the record server
                    // — see the else branch.
                    projectile = weapon;
                    weaponFid = weapon->fid;
                    int weaponFlags = weapon->flags;

                    int leftItemAction;
                    int rightItemAction;
                    interfaceGetItemActions(&leftItemAction, &rightItemAction);

                    itemRemove(attack->attacker, weapon, 1);
                    replacedWeapon = itemReplace(attack->attacker, weapon, weaponFlags & OBJECT_IN_ANY_HAND);
                    objectSetFid(projectile, projectileProto->fid, nullptr);
                    _cAIPrepWeaponItem(attack->attacker, weapon);

                    if (attack->attacker == gDude) {
                        if (replacedWeapon == nullptr) {
                            if ((weaponFlags & OBJECT_IN_LEFT_HAND) != 0) {
                                leftItemAction = INTERFACE_ITEM_ACTION_DEFAULT;
                            } else if ((weaponFlags & OBJECT_IN_RIGHT_HAND) != 0) {
                                rightItemAction = INTERFACE_ITEM_ACTION_DEFAULT;
                            }
                        }
                        presenter()->hudItems(false, leftItemAction, rightItemAction);
                    }

                    _obj_connect(weapon, attack->attacker->tile, attack->attacker->elevation, nullptr);
                } else {
                    // Non-throw ranged, OR a RECORDED throw. For a recorded throw the
                    // authoritative consumption + landed ground weapon are applied by
                    // actionThrowConsumeHeadless on the STATE arm (combat.cc) — so we must
                    // NOT mutate the attacker's inventory here; we only build the flight
                    // PRESENTATION with a transient projectile, exactly like a non-throw
                    // ranged shot (the common flight/explosion code below then streams it,
                    // and the leak-fix destroys it after the section). weaponFid feeds the
                    // tail's land-as-weapon setFid for a thrown weapon; it stays -1 for
                    // bullets, where the tail never reads it.
                    if (anim == ANIM_THROW_ANIM) {
                        weaponFid = weapon->fid;
                    }
                    objectCreateWithFidPid(&projectile, projectileProto->fid, -1);

                    // PRESENTATION-RECORD: resolveRef runs DURING the record section and
                    // needs NO_SAVE (+ created-this-beat) to classify the projectile as a
                    // transient and ship it as OBJ_CREATE; vanilla sets NO_SAVE only later
                    // in hideProjectile, too late for the recorder. Mark it now so its pose
                    // streams (record-server only; SP is untouched — presRecordActive() is
                    // false there). Destroyed after the section closes (below, leak fix).
                    if (presRecordActive()) {
                        projectile->flags |= OBJECT_NO_SAVE;
                        // A NON-explosive thrown weapon becomes a ground item: the STATE arm
                        // (actionThrowConsumeHeadless) _obj_connects the real weapon by netId,
                        // and the AI later picks it up (disconnect). Map that netId onto this
                        // flight transient so the disconnect removes it on the viewer — else
                        // the spear phantoms and accumulates. Explosive throws (grenades) are
                        // destroyed by the blast, not connected, so they must NOT adopt.
                        if (anim == ANIM_THROW_ANIM) {
                            bool explosive = damageType == explosionGetDamageType()
                                || damageType == DAMAGE_TYPE_PLASMA || damageType == DAMAGE_TYPE_EMP;
                            if (!explosive) {
                                presRecordSetAdoptNetId(projectile, weapon->netId);
                            }
                        }
                    }
                }

                objectHide(projectile, nullptr);

                // SFALL
                if (explosionEmitsLight() && projectile->lightIntensity == 0) {
                    objectSetLight(projectile, projectileProto->item.lightDistance, projectileProto->item.lightIntensity, nullptr);
                } else {
                    objectSetLight(projectile, 9, projectile->lightIntensity, nullptr);
                }

                int projectileOrigin = _combat_bullet_start(attack->attacker, attack->defender);
                objectSetLocation(projectile, projectileOrigin, attack->attacker->elevation, nullptr);

                int projectileRotation = tileGetRotationTo(attack->attacker->tile, attack->defender->tile);
                objectSetRotation(projectile, projectileRotation, nullptr);

                animationRegisterUnsetFlag(projectile, OBJECT_HIDDEN, delay);

                const char* sfx = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_AMMO_FLYING, weapon, attack->hitMode, attack->defender);
                animationRegisterPlaySoundEffect(projectile, sfx, 0);

                int explosionCenterTile;
                if ((attack->attackerFlags & DAM_HIT) != 0) {
                    animationRegisterMoveToTileStraight(projectile, attack->defender->tile, attack->defender->elevation, ANIM_WALK, 0);
                    delay = _make_straight_path(projectile, projectileOrigin, attack->defender->tile, nullptr, nullptr, 32) - 1;
                    explosionCenterTile = attack->defender->tile;
                } else {
                    animationRegisterMoveToTileStraight(projectile, attack->tile, attack->defender->elevation, ANIM_WALK, 0);
                    delay = 0;
                    explosionCenterTile = attack->tile;
                }

                // SFALL
                if (isGrenade || damageType == explosionGetDamageType()) {
                    if ((attack->attackerFlags & DAM_DROP) == 0) {
                        int explosionFrmId;
                        if (isGrenade) {
                            switch (damageType) {
                            case DAMAGE_TYPE_EMP:
                                explosionFrmId = 2;
                                break;
                            case DAMAGE_TYPE_PLASMA:
                                explosionFrmId = 31;
                                break;
                            default:
                                explosionFrmId = 29;
                                break;
                            }
                        } else {
                            explosionFrmId = 10;
                        }

                        // SFALL
                        int explosionFrmIdOverride = explosionGetFrm();
                        if (explosionFrmIdOverride != -1) {
                            explosionFrmId = explosionFrmIdOverride;
                        }

                        if (isGrenade) {
                            animationRegisterSetFid(projectile, weaponFid, -1);
                        }

                        int explosionFid = buildFid(OBJ_TYPE_MISC, explosionFrmId, 0, 0, 0);
                        animationRegisterSetFid(projectile, explosionFid, -1);

                        const char* sfx = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_HIT, weapon, attack->hitMode, attack->defender);
                        animationRegisterPlaySoundEffect(projectile, sfx, 0);

                        // SFALL
                        if (explosionEmitsLight()) {
                            animationRegisterAnimate(projectile, ANIM_STAND, 0);
                            // 0xFFFF0008
                            // - distance: 8
                            // - intensity: 65535
                            animationRegisterSetLightIntensity(projectile, 8, 65536, 0);
                        } else {
                            animationRegisterAnimateAndHide(projectile, ANIM_STAND, 0);
                        }

                        // SFALL
                        int startRotation;
                        int endRotation;
                        explosionGetPattern(&startRotation, &endRotation);

                        for (int rotation = startRotation; rotation < endRotation; rotation++) {
                            if (objectCreateWithFidPid(&(adjacentObjects[rotation]), explosionFid, -1) != -1) {
                                objectHide(adjacentObjects[rotation], nullptr);

                                int adjacentTile = tileGetTileInDirection(explosionCenterTile, rotation, 1);
                                objectSetLocation(adjacentObjects[rotation], adjacentTile, projectile->elevation, nullptr);

                                int delay;
                                if (rotation != ROTATION_NE) {
                                    delay = 0;
                                } else {
                                    if (damageType == DAMAGE_TYPE_PLASMA) {
                                        delay = 4;
                                    } else {
                                        delay = 2;
                                    }
                                }

                                animationRegisterUnsetFlag(adjacentObjects[rotation], OBJECT_HIDDEN, delay);
                                animationRegisterAnimateAndHide(adjacentObjects[rotation], ANIM_STAND, 0);
                            }
                        }

                        l56 = true;
                    }
                } else {
                    if (anim != ANIM_THROW_ANIM) {
                        animationRegisterHideObjectForced(projectile);
                    }
                }

                if (!l56) {
                    const char* sfx = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_HIT, weapon, attack->hitMode, attack->defender);
                    animationRegisterPlaySoundEffect(weapon, sfx, delay);
                }

                delay = 0;
            } else {
                if ((attack->attackerFlags & DAM_HIT) == 0) {
                    Object* defender = attack->defender;
                    if ((defender->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) == 0) {
                        animationRegisterAnimate(defender, ANIM_DODGE_ANIM, delay);
                        l56 = true;
                    }
                }
            }
        }
    }

    _show_damage(attack, anim, delay);

    if (!clientViewerActive()) { // wire replay: server streams the real taunts (§3.c)
        if ((attack->attackerFlags & DAM_HIT) == 0) {
            _combatai_msg(attack->defender, attack, AI_MESSAGE_TYPE_MISS, -1);
        } else {
            if ((attack->defenderFlags & DAM_DEAD) == 0) {
                _combatai_msg(attack->defender, attack, AI_MESSAGE_TYPE_HIT, -1);
            }
        }
    }

    // SFALL
    if (projectile != nullptr && (isGrenade || damageType == explosionGetDamageType())) {
        // CE: Use custom callback to hide projectile instead of relying on
        // `animationRegisterHideObjectForced`. The problem is that completing
        // `ANIM_KIND_HIDE` removes (frees) object entirely. When this happens
        // `attack->weapon` becomes a dangling pointer, but this object is
        // needed to process `damage_p_proc` by scripting engine which can
        // interrogate weapon's properties (leading to crash on some platforms).
        // So instead of removing projectile follow a pattern established in
        // `opDestroyObject` for self-deleting objects (mark it hidden +
        // no-save).
        animationRegisterCallbackForced(attack, projectile, hideProjectile, -1);
    } else if (anim == ANIM_THROW_ANIM && projectile != nullptr) {
        animationRegisterSetFid(projectile, weaponFid, -1);
    }

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        if (adjacentObjects[rotation] != nullptr) {
            animationRegisterHideObjectForced(adjacentObjects[rotation]);
        }
    }

    if ((attack->attackerFlags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_DEAD)) == 0) {
        if (anim == ANIM_THROW_ANIM) {
            bool takeOutAnimationRegistered = false;
            if (replacedWeapon != nullptr) {
                int weaponAnimationCode = weaponGetAnimationCode(replacedWeapon);
                if (weaponAnimationCode != 0) {
                    animationRegisterTakeOutWeapon(attack->attacker, weaponAnimationCode, -1);
                    takeOutAnimationRegistered = true;
                }
            }

            if (!takeOutAnimationRegistered) {
                int fid = buildFid(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, ANIM_STAND, 0, attack->attacker->rotation + 1);
                animationRegisterSetFid(attack->attacker, fid, -1);
            }
        } else {
            animationRegisterAnimate(attack->attacker, ANIM_UNPOINT, -1);
        }
    }

    if (reg_anim_end() == -1) {
        debugPrint("Something went wrong with a ranged attack sequence!\n");
        if (projectile != nullptr && (isGrenade || damageType == DAMAGE_TYPE_EXPLOSION || anim != ANIM_THROW_ANIM)) {
            objectDestroy(projectile, nullptr);
        }

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            if (adjacentObjects[rotation] != nullptr) {
                objectDestroy(adjacentObjects[rotation], nullptr);
            }
        }

        return -1;
    }

    _show_damage_extras(attack);

    // PRESENTATION-RECORD leak fix: under record the animate branch's completion
    // callbacks are DROPped server-side (the viewer destroys the projectile via its own
    // anim completion; the record server has no completion), so the transient projectile
    // + any explosion clouds created above would leak on the record server (no_save
    // count grows → run_record_purity FAIL). Their OBJ_CREATE poses are already captured
    // in the stream, so free them now that the section has recorded. NON-THROW ONLY by
    // construction — a throw is never recorded (combat.cc opens no section for it), so
    // presRecordActive() is false and the real inventory weapon used as a throw
    // projectile is never touched here. Mirrors the reg_anim_end()==-1 cleanup and
    // actionExplode's STATE-block cloud destroy.
    if (presRecordActive()) {
        if (projectile != nullptr) {
            objectDestroy(projectile, nullptr);
        }
        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            if (adjacentObjects[rotation] != nullptr) {
                objectDestroy(adjacentObjects[rotation], nullptr);
            }
        }
    }

    return 0;
}

// Headless mirror of the thrown-weapon CONSUMPTION welded into _action_ranged's throw
// branch (actions.cc:892-917). The animated path runs itemRemove/itemReplace/_obj_connect
// inline while building the throw animation; the dedicated server SKIPS _action_attack, so
// without this a thrown weapon is never removed from inventory (infinite spears/grenades —
// a pre-existing bug, confirmed 2026-07-18). Called authoritatively from the server's
// _combat_apply_attack_results(!animated); the animated path is unchanged, so there is no
// double-consume. Mirrors _action_ranged's exact entry gate + itemRemove/itemReplace/
// _cAIPrepWeaponItem sequence, then applies the thrown object's FINAL resting state directly
// (the animated path reaches it via the projectile move + fid-restore/hide leaves):
//   - non-explosive throw (spear/knife/rock): lands on the ground at the target tile as a
//     lootable item with its weapon fid (animated: MoveToTileStraight -> SetFid weaponFid);
//   - explosive throw (grenade): consumed by the explosion (animated: hideProjectile),
//     the blast damage is already applied by _combat_apply_attack_results/_apply_damage.
void actionThrowConsumeHeadless(Attack* attack)
{
    int anim = critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
    if (anim != ANIM_THROW_ANIM) {
        return;
    }

    // Same entry gate as _action_ranged: the throw branch (and thus consumption) is skipped
    // on a critical miss that did not hit (line 886: DAM_HIT != 0 || DAM_CRITICAL == 0).
    if ((attack->attackerFlags & DAM_HIT) == 0 && (attack->attackerFlags & DAM_CRITICAL) != 0) {
        return;
    }

    Object* weapon = attack->weapon;
    if (weapon == nullptr) {
        return;
    }

    // A throw only consumes when the weapon actually has a projectile proto (matches the
    // _action_ranged guard); otherwise the throw branch was never entered.
    int projectilePid = weaponGetProjectilePid(weapon);
    Proto* projectileProto;
    if (protoGetProto(projectilePid, &projectileProto) == -1 || projectileProto->fid == -1) {
        return;
    }

    int weaponFlags = weapon->flags;
    int damageType = weaponGetDamageType(attack->attacker, weapon);
    // SFALL isGrenade test (actions.cc:864): explosive throwables are consumed by the blast.
    bool explosive = damageType == explosionGetDamageType()
        || damageType == DAMAGE_TYPE_PLASMA
        || damageType == DAMAGE_TYPE_EMP;

    itemRemove(attack->attacker, weapon, 1);
    itemReplace(attack->attacker, weapon, weaponFlags & OBJECT_IN_ANY_HAND);
    _cAIPrepWeaponItem(attack->attacker, weapon);

    // itemRemove detaches `weapon` (owner cleared) but does NOT free it — reconnect it to
    // the world (spear) or destroy it (grenade), else it orphans.
    if (explosive) {
        objectDestroy(weapon, nullptr);
    } else {
        int landTile = (attack->attackerFlags & DAM_HIT) != 0 ? attack->defender->tile : attack->tile;
        weapon->flags &= ~OBJECT_HIDDEN; // the animated path leaves it visible on the ground
        _obj_connect(weapon, landTile, attack->defender->elevation, nullptr);
    }
}

// 0x411D68
int _is_next_to(Object* a1, Object* a2)
{
    if (objectGetDistanceBetween(a1, a2) > 1) {
        if (a2 == gDude) {
            MessageListItem messageListItem;
            // You cannot get there.
            messageListItem.num = 2000;
            if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
                presenter()->consoleMessage(messageListItem.text);
            }
        }
        return -1;
    }

    return 0;
}

// 0x411DB4
int _action_climb_ladder(Object* a1, Object* a2)
{
    if (a1 == gDude) {
        int anim = FID_ANIM_TYPE(gDude->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            reg_anim_clear(gDude);
        }
    }

    int animationRequestOptions;
    int actionPoints;
    if (isInCombat()) {
        animationRequestOptions = ANIMATION_REQUEST_RESERVED;
        actionPoints = a1->data.critter.combat.ap;
    } else {
        animationRequestOptions = ANIMATION_REQUEST_UNRESERVED;
        actionPoints = -1;
    }

    if (a1 == gDude) {
        animationRequestOptions = ANIMATION_REQUEST_RESERVED;
    }

    animationRequestOptions |= ANIMATION_REQUEST_NO_STAND;
    reg_anim_begin(animationRequestOptions);

    int tile = tileGetTileInDirection(a2->tile, ROTATION_SE, 1);
    if (actionPoints != -1 || objectGetDistanceBetween(a1, a2) < 5) {
        animationRegisterMoveToTile(a1, tile, a2->elevation, actionPoints, 0);
    } else {
        animationRegisterRunToTile(a1, tile, a2->elevation, actionPoints, 0);
    }

    animationRegisterCallbackForced(a1, a2, (AnimationCallback*)_is_next_to, -1);
    animationRegisterRotateToTile(a1, a2->tile);
    animationRegisterCallbackForced(a1, a2, (AnimationCallback*)_check_scenery_ap_cost, -1);

    int weaponAnimationCode = (a1->fid & 0xF000) >> 12;
    if (weaponAnimationCode != 0) {
        const char* puttingAwaySfx = sfxBuildCharName(a1, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
        animationRegisterPlaySoundEffect(a1, puttingAwaySfx, -1);
        animationRegisterAnimate(a1, ANIM_PUT_AWAY, 0);
    }

    const char* climbingSfx = sfxBuildCharName(a1, ANIM_CLIMB_LADDER, CHARACTER_SOUND_EFFECT_UNUSED);
    animationRegisterPlaySoundEffect(a1, climbingSfx, -1);
    animationRegisterAnimate(a1, ANIM_CLIMB_LADDER, 0);
    animationRegisterCallback(a1, a2, (AnimationCallback*)_obj_use, -1);

    if (weaponAnimationCode != 0) {
        animationRegisterTakeOutWeapon(a1, weaponAnimationCode, -1);
    }

    return reg_anim_end();
}

// 0x411F2C
int _action_use_an_item_on_object(Object* user, Object* targetObj, Object* item)
{
    Proto* proto = nullptr;
    int type = FID_TYPE(targetObj->fid);
    int sceneryType = -1;
    if (type == OBJ_TYPE_SCENERY) {
        if (protoGetProto(targetObj->pid, &proto) == -1) {
            return -1;
        }

        sceneryType = proto->scenery.type;
    }

    if (serverLoopActive()) {
        // Headless: the outcome (_obj_use for use-object/ladder/stairs/door, or
        // _obj_use_item_on for use-item-on-object) is only reached via a
        // reg_anim completion callback that never fires — no animation drives
        // it — so this action was a silent no-op on the server path. Both
        // handlers run synchronously in-call, so invoke them directly; mirrors
        // _obj_use_door / actionPickUp. _obj_use itself dispatches ladders and
        // stairs to useLadder*/useStairs (the SCENERY_TYPE_LADDER_UP special
        // case that the legacy path routes through _action_climb_ladder), and
        // doors to _obj_use_door. _obj_use ignores adjacency, which is the
        // intended headless behavior (no walk-to).
        if (item != nullptr) {
            return _obj_use_item_on(user, targetObj, item);
        }
        // Charge scenery AP as the legacy item==nullptr path does. HONOR the
        // result: a refusal (not enough AP — it streams proto msg 700 itself)
        // means the use does not happen. This return was discarded while the
        // comment here could still say "a no-op out of combat, which the headless
        // dude always is" — true until in-combat interaction became reachable
        // over the wire, at which point ignoring it meant a player with 0 AP
        // still opened the door, for free, on someone else's turn.
        if (_check_scenery_ap_cost(user, targetObj) == -1) {
            return -1;
        }
        return _obj_use(user, targetObj);
    }

    if (sceneryType != SCENERY_TYPE_LADDER_UP || item != nullptr) {
        if (user == gDude) {
            int anim = FID_ANIM_TYPE(gDude->fid);
            if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
                reg_anim_clear(gDude);
            }
        }

        int animationRequestOptions;
        int actionPoints;
        if (isInCombat()) {
            animationRequestOptions = ANIMATION_REQUEST_RESERVED;
            actionPoints = user->data.critter.combat.ap;
        } else {
            animationRequestOptions = ANIMATION_REQUEST_UNRESERVED;
            actionPoints = -1;
        }

        if (user == gDude) {
            animationRequestOptions = ANIMATION_REQUEST_RESERVED;
        }

        reg_anim_begin(animationRequestOptions);

        if (actionPoints != -1 || objectGetDistanceBetween(user, targetObj) < 5) {
            animationRegisterMoveToObject(user, targetObj, actionPoints, 0);
        } else {
            animationRegisterRunToObject(user, targetObj, -1, 0);
        }

        animationRegisterCallbackForced(user, targetObj, (AnimationCallback*)_is_next_to, -1);

        if (item == nullptr) {
            animationRegisterCallback(user, targetObj, (AnimationCallback*)_check_scenery_ap_cost, -1);
        }

        int weaponAnimCode = (user->fid & 0xF000) >> 12;
        if (weaponAnimCode != 0) {
            const char* sfx = sfxBuildCharName(user, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
            animationRegisterPlaySoundEffect(user, sfx, -1);
            animationRegisterAnimate(user, ANIM_PUT_AWAY, 0);
        }

        int anim;
        int objectType = FID_TYPE(targetObj->fid);
        if (objectType == OBJ_TYPE_CRITTER && _critter_is_prone(targetObj)) {
            anim = ANIM_MAGIC_HANDS_GROUND;
        } else if (objectType == OBJ_TYPE_SCENERY && (proto->scenery.extendedFlags & 0x01) != 0) {
            anim = ANIM_MAGIC_HANDS_GROUND;
        } else {
            anim = ANIM_MAGIC_HANDS_MIDDLE;
        }

        if (sceneryType != SCENERY_TYPE_STAIRS && item == nullptr) {
            animationRegisterAnimate(user, anim, -1);
        }

        if (item != nullptr) {
            // TODO: Get rid of cast.
            animationRegisterCallback3(user, targetObj, item, (AnimationCallback3*)_obj_use_item_on, -1);
        } else {
            animationRegisterCallback(user, targetObj, (AnimationCallback*)_obj_use, -1);
        }

        if (weaponAnimCode != 0) {
            animationRegisterTakeOutWeapon(user, weaponAnimCode, -1);
        }

        return reg_anim_end();
    }

    return _action_climb_ladder(user, targetObj);
}

// 0x412114
int _action_use_an_object(Object* user, Object* targetObj)
{
    return _action_use_an_item_on_object(user, targetObj, nullptr);
}

// 0x412134
int actionPickUp(Object* critter, Object* item)
{
    if (FID_TYPE(item->fid) != OBJ_TYPE_ITEM) {
        return -1;
    }

    Proto* itemProto;
    protoGetProto(item->pid, &itemProto);
    bool canPickup = itemProto->item.type != ITEM_TYPE_CONTAINER || _proto_action_can_pickup(item->pid);

    // PRESENTATION-RECORD (pickup gesture): on the dedicated server IN COMBAT, RUN the
    // normally-skipped animate branch — the walk to the item + the crouch-and-grab
    // ANIM_MAGIC_HANDS_GROUND — inside a record section so it streams as EVENT_PRES_SEQ,
    // then apply the pickup outcome authoritatively below (the animate branch's _obj_pickup
    // callback DROPs under record). Ground items only (a container pickup opens a modal not
    // driven here). Flag off / SP / out of combat → the state-only fast path below (goldens
    // unchanged). See COMBAT_MOVE_RECORD_DESIGN.md.
    // The adopt-mapping (throw transient -> real netId) now registers at the seq's DECODE
    // pass (client_net.cc OBJ_CREATE), so the thrown weapon is in _net before this pickup's
    // state-lane DISCONNECT arrives regardless of pump lag — the phantom-spear regression
    // that forced this off is fixed. Ground items only (a container pickup opens a modal not
    // driven here). Flag off / SP / out of combat -> the state-only fast path below.
    bool recording = serverLoopActive() && presRecordEnabled() && isInCombat()
        && !presRecordActive() && canPickup;

    if (serverLoopActive() && !recording) {
        // Headless: the reg_anim completion callbacks that carry the pickup outcome never
        // fire (no animation drives them), so apply the outcome directly. Container pickup
        // opens the loot UI (a modal not driven headless) — decline rather than hang.
        if (canPickup) {
            // ►► The AP result is DELIBERATELY IGNORED here, and must stay that
            // way until the asymmetry below is fixed. See the record-purity note
            // in the `recording` branch: this path does not walk and that one
            // does, so the two do not agree on the critter's AP by the time the
            // pickup is decided. Honoring the refusal on either side alone makes
            // record mode observable in the simulation, which the record-purity
            // melee gate catches immediately (verified 2026-07-20).
            _check_scenery_ap_cost(critter, item);
            return _obj_pickup(critter, item);
        }
        return -1;
    }

    if (recording) {
        presRecordAmbientBegin();
    }

    if (critter == gDude) {
        int animationCode = FID_ANIM_TYPE(gDude->fid);
        if (animationCode == ANIM_WALK || animationCode == ANIM_RUNNING) {
            reg_anim_clear(gDude);
        }
    }

    if (isInCombat()) {
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToObject(critter, item, critter->data.critter.combat.ap, 0);
    } else {
        reg_anim_begin(critter == gDude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);
        if (objectGetDistanceBetween(critter, item) >= 5) {
            animationRegisterRunToObject(critter, item, -1, 0);
        } else {
            animationRegisterMoveToObject(critter, item, -1, 0);
        }
    }

    animationRegisterCallbackForced(critter, item, (AnimationCallback*)_is_next_to, -1);
    animationRegisterCallback(critter, item, (AnimationCallback*)_check_scenery_ap_cost, -1);

    if (canPickup) {
        animationRegisterAnimate(critter, ANIM_MAGIC_HANDS_GROUND, 0);

        int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_MAGIC_HANDS_GROUND, (critter->fid & 0xF000) >> 12, critter->rotation + 1);

        int actionFrame;
        CacheEntry* cacheEntry;
        Art* art = artLock(fid, &cacheEntry);
        if (art != nullptr) {
            actionFrame = artGetActionFrame(art);
        } else {
            actionFrame = -1;
        }

        char sfx[16];
        if (artCopyFileName(FID_TYPE(item->fid), item->fid & 0xFFF, sfx) == 0) {
            // NOTE: looks like they copy sfx one more time, what for?
            animationRegisterPlaySoundEffect(item, sfx, actionFrame);
        }

        animationRegisterCallback(critter, item, (AnimationCallback*)_obj_pickup, actionFrame);
    } else {
        int weaponAnimationCode = (critter->fid & 0xF000) >> 12;
        if (weaponAnimationCode != 0) {
            const char* sfx = sfxBuildCharName(critter, ANIM_PUT_AWAY, CHARACTER_SOUND_EFFECT_UNUSED);
            animationRegisterPlaySoundEffect(critter, sfx, -1);
            animationRegisterAnimate(critter, ANIM_PUT_AWAY, -1);
        }

        // ground vs middle animation
        int anim = (itemProto->item.data.container.openFlags & 0x01) == 0
            ? ANIM_MAGIC_HANDS_MIDDLE
            : ANIM_MAGIC_HANDS_GROUND;
        animationRegisterAnimate(critter, anim, 0);

        int fid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, anim, 0, critter->rotation + 1);

        int actionFrame;
        CacheEntry* cacheEntry;
        Art* art = artLock(fid, &cacheEntry);
        if (art == nullptr) {
            actionFrame = artGetActionFrame(art);
            artUnlock(cacheEntry);
        } else {
            actionFrame = -1;
        }

        if (item->frame != 1) {
            animationRegisterCallback(critter, item, (AnimationCallback*)_obj_use_container, actionFrame);
        }

        if (weaponAnimationCode != 0) {
            animationRegisterTakeOutWeapon(critter, weaponAnimationCode, -1);
        }

        if (item->frame == 0 || item->frame == 1) {
            animationRegisterCallback(critter, item, (AnimationCallback*)scriptsRequestLooting, -1);
        }
    }

    int rc = reg_anim_end();

    if (recording) {
        presRecordAmbientEnd();
        if (presRecordOpCount() > 2) {
            presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount(), critter->netId);
        }
        presRecordCommitDeferred(); // the MoveToObject walk to the item
        // Apply the pickup outcome authoritatively — the recorded _obj_pickup /
        // _check_scenery_ap_cost callbacks DROP under record (the server never runs a
        // recorded reg_anim's callbacks), so the state must be applied here.
        //
        // ►► KNOWN PRE-EXISTING ASYMMETRY, do not "fix" by honoring this return.
        // This path registers an approach walk before charging; the non-record
        // path above charges without walking. The two therefore disagree about
        // the critter's AP at this point. While both sides IGNORE the refusal the
        // divergence stays invisible (the pickup happens either way), which is
        // why the record-purity gate has always passed. Honoring it on either
        // side alone makes record mode observable in the sim and fails that gate
        // (verified 2026-07-20). Making pickup respect AP properly means removing
        // the walk asymmetry first — banked, not done here.
        _check_scenery_ap_cost(critter, item);
        bool traceP = getenv("F2_TRACE_EVENTS") != nullptr;
        if (traceP) fprintf(stderr, "[cpickup] critter=%d item_net=%d item_tile=%d ops=%d\n",
            critter->netId, item->netId, item->tile, presRecordOpCount());
        int pr = _obj_pickup(critter, item);
        if (traceP) fprintf(stderr, "[cpickup] done rc=%d item_tile_now=%d\n", pr, item->tile);
    }

    return rc;
}

// TODO: Looks like the name is a little misleading, container can only be a
// critter, which is enforced by this function as well as at the call sites.
// Used to loot corpses, so probably should be something like actionLootCorpse.
// Check if it can be called with a living critter.
//
// 0x4123E8
int _action_loot_container(Object* critter, Object* container)
{
    if (FID_TYPE(container->fid) != OBJ_TYPE_CRITTER) {
        return -1;
    }

    // SFALL: Fix for trying to loot corpses with the "NoSteal" flag.
    if (_critter_flag_check(container->pid, CRITTER_NO_STEAL)) {
        return -1;
    }

    if (critter == gDude) {
        int anim = FID_ANIM_TYPE(gDude->fid);
        if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
            reg_anim_clear(gDude);
        }
    }

    if (isInCombat()) {
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToObject(critter, container, critter->data.critter.combat.ap, 0);
    } else {
        reg_anim_begin(critter == gDude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);

        if (objectGetDistanceBetween(critter, container) < 5) {
            animationRegisterMoveToObject(critter, container, -1, 0);
        } else {
            animationRegisterRunToObject(critter, container, -1, 0);
        }
    }

    animationRegisterCallbackForced(critter, container, (AnimationCallback*)_is_next_to, -1);
    animationRegisterCallback(critter, container, (AnimationCallback*)_check_scenery_ap_cost, -1);
    animationRegisterCallback(critter, container, (AnimationCallback*)scriptsRequestLooting, -1);
    return reg_anim_end();
}

// 0x4124E0
int _action_skill_use(int skill)
{
    if (skill == SKILL_SNEAK) {
        reg_anim_clear(gDude);
        dudeToggleState(DUDE_STATE_SNEAKING);
        return 0;
    }

    return -1;
}

// NOTE: Inlined.
//
// 0x412500
static int _action_use_skill_in_combat_error(Object* critter)
{
    MessageListItem messageListItem;

    // Any player actor, addressed to them — see _check_scenery_ap_cost for why
    // the gDude test had to be generalized and why a refusal is not broadcast.
    if (playerActorIs(critter)) {
        messageListItem.num = 902;
        if (messageListGetItem(&gProtoMessageList, &messageListItem) == 1) {
            presenter()->consoleMessageFor(critter->netId, messageListItem.text);
        }
    }

    return -1;
}

// skill_use
// 0x41255C
int actionUseSkill(Object* user, Object* target, int skill)
{
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR:
        if (isInCombat()) {
            // NOTE: Uninline.
            return _action_use_skill_in_combat_error(user);
        }

        if (PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
            return -1;
        }
        break;
    case SKILL_LOCKPICK:
        if (isInCombat()) {
            // NOTE: Uninline.
            return _action_use_skill_in_combat_error(user);
        }

        if (PID_TYPE(target->pid) != OBJ_TYPE_ITEM && PID_TYPE(target->pid) != OBJ_TYPE_SCENERY) {
            return -1;
        }

        break;
    case SKILL_STEAL:
        if (isInCombat()) {
            // NOTE: Uninline.
            return _action_use_skill_in_combat_error(user);
        }

        if (PID_TYPE(target->pid) != OBJ_TYPE_ITEM && PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
            return -1;
        }

        if (target == user) {
            return -1;
        }

        break;
    case SKILL_TRAPS:
        if (isInCombat()) {
            // NOTE: Uninline.
            return _action_use_skill_in_combat_error(user);
        }

        if (PID_TYPE(target->pid) == OBJ_TYPE_CRITTER) {
            return -1;
        }

        break;
    case SKILL_SCIENCE:
    case SKILL_REPAIR:
        if (isInCombat()) {
            // NOTE: Uninline.
            return _action_use_skill_in_combat_error(user);
        }

        if (PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
            break;
        }

        if (critterGetKillType(target) == KILL_TYPE_ROBOT) {
            break;
        }

        if (critterGetKillType(target) == KILL_TYPE_BRAHMIN
            && skill == SKILL_SCIENCE) {
            break;
        }

        // SFALL: Science on critters patch.
        if (1) {
            int targetType = SCIENCE_REPAIR_TARGET_TYPE_DEFAULT;
            configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_SCIENCE_REPAIR_TARGET_TYPE_KEY, &targetType);
            if (targetType == SCIENCE_REPAIR_TARGET_TYPE_DUDE) {
                if (target == gDude) {
                    break;
                }
            } else if (targetType == SCIENCE_REPAIR_TARGET_TYPE_ANYONE) {
                break;
            }
        }

        return -1;
    case SKILL_SNEAK:
        dudeToggleState(DUDE_STATE_SNEAKING);
        return 0;
    default:
        debugPrint("\nskill_use: invalid skill used.");
    }

    // Performer is either dude, or party member who's best at the specified
    // skill in entire party, and this skill is his/her own best.
    Object* performer = gDude;

    if (user == gDude) {
        Object* partyMember = partyMemberGetBestInSkill(skill);

        if (partyMember == gDude) {
            partyMember = nullptr;
        }

        // Only dude can perform stealing.
        if (skill == SKILL_STEAL) {
            partyMember = nullptr;
        }

        if (partyMember != nullptr) {
            if (partyMemberGetBestSkill(partyMember) != skill) {
                partyMember = nullptr;
            }
        }

        if (partyMember != nullptr) {
            performer = partyMember;
            int anim = FID_ANIM_TYPE(partyMember->fid);
            if (anim != ANIM_WALK && anim != ANIM_RUNNING) {
                if (anim != ANIM_STAND) {
                    performer = gDude;
                    partyMember = nullptr;
                }
            } else {
                reg_anim_clear(partyMember);
            }
        }

        if (partyMember != nullptr) {
            bool isDude = false;
            if (objectGetDistanceBetween(gDude, target) <= 1) {
                isDude = true;
            }

            char* msg = skillsGetGenericResponse(partyMember, isDude);

            presenter()->floatText(partyMember, msg, 101, _colorTable[32747], _colorTable[0]);

            if (isDude) {
                performer = gDude;
                partyMember = nullptr;
            }
        }

        if (partyMember == nullptr) {
            int anim = FID_ANIM_TYPE(performer->fid);
            if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
                reg_anim_clear(performer);
            }
        }
    }

    if (serverLoopActive()) {
        // Headless: the skill outcome (_obj_use_skill_on) is welded to a reg_anim
        // completion callback (animationRegisterCallback3 at the tail of this
        // function) that never fires with no animation to drive it, so on the
        // server path actionUseSkill was a silent no-op. Apply the outcome
        // directly with the selected performer, mirroring the doors/pickup/
        // use-object fast-paths and _combat_apply_attack_results. _obj_use_skill_on
        // runs the target's SCRIPT_PROC_USE_SKILL_ON (lockpick/science/traps lock +
        // gvar effects) and then skillUse (first-aid/doctor/repair HP + cripple
        // clears) in the correct order, so it is the single call to make. Adjacency
        // (_is_next_to) is intentionally not gated, matching actionPickUp / _obj_use.
        return _obj_use_skill_on(performer, target, skill);
    }

    if (isInCombat()) {
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToObject(performer, target, performer->data.critter.combat.ap, 0);
    } else {
        reg_anim_begin(user == gDude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);
        if (target != gDude) {
            if (objectGetDistanceBetween(performer, target) >= 5) {
                animationRegisterRunToObject(performer, target, -1, 0);
            } else {
                animationRegisterMoveToObject(performer, target, -1, 0);
            }
        }
    }

    animationRegisterCallbackForced(performer, target, (AnimationCallback*)_is_next_to, -1);

    int anim = (FID_TYPE(target->fid) == OBJ_TYPE_CRITTER && _critter_is_prone(target)) ? ANIM_MAGIC_HANDS_GROUND : ANIM_MAGIC_HANDS_MIDDLE;
    int fid = buildFid(OBJ_TYPE_CRITTER, performer->fid & 0xFFF, anim, 0, performer->rotation + 1);

    CacheEntry* artHandle;
    Art* art = artLock(fid, &artHandle);
    if (art != nullptr) {
        artGetActionFrame(art);
        artUnlock(artHandle);
    }

    animationRegisterAnimate(performer, anim, -1);
    // TODO: Get rid of casts.
    animationRegisterCallback3(performer, target, (void*)skill, (AnimationCallback3*)_obj_use_skill_on, -1);
    return reg_anim_end();
}

// 0x412BC4
bool _is_hit_from_front(Object* a1, Object* a2)
{
    int diff = a1->rotation - a2->rotation;
    if (diff < 0) {
        diff = -diff;
    }

    return diff != 0 && diff != 1 && diff != 5;
}

// 0x412BEC
bool _can_see(Object* a1, Object* a2)
{
    int diff;

    diff = a1->rotation - tileGetRotationTo(a1->tile, a2->tile);
    if (diff < 0) {
        diff = -diff;
    }

    return diff == 0 || diff == 1 || diff == 5;
}

// looks like it tries to change fall animation depending on object's current rotation
// 0x412C1C
int _pick_fall(Object* obj, int anim)
{
    int i;
    int rotation;
    int tile_num;
    int fid;

    if (anim == ANIM_FALL_FRONT) {
        rotation = obj->rotation;
        for (i = 1; i < 3; i++) {
            tile_num = tileGetTileInDirection(obj->tile, rotation, i);
            if (_obj_blocking_at(obj, tile_num, obj->elevation) != nullptr) {
                anim = ANIM_FALL_BACK;
                break;
            }
        }
    } else if (anim == ANIM_FALL_BACK) {
        rotation = (obj->rotation + 3) % ROTATION_COUNT;
        for (i = 1; i < 3; i++) {
            tile_num = tileGetTileInDirection(obj->tile, rotation, i);
            if (_obj_blocking_at(obj, tile_num, obj->elevation) != nullptr) {
                anim = ANIM_FALL_FRONT;
                break;
            }
        }
    }

    if (anim == ANIM_FALL_FRONT) {
        fid = buildFid(OBJ_TYPE_CRITTER, obj->fid & 0xFFF, ANIM_FALL_FRONT, (obj->fid & 0xF000) >> 12, obj->rotation + 1);
        if (!artExists(fid)) {
            anim = ANIM_FALL_BACK;
        }
    }

    return anim;
}

// 0x412CE4
bool _action_explode_running()
{
    return _action_in_explode;
}

// action_explode
// 0x412CF4
int actionExplode(int tile, int elevation, int minDamage, int maxDamage, Object* sourceObj, bool animate)
{
    if (animate && _action_in_explode) {
        return -2;
    }

    Attack* attack = (Attack*)internal_malloc(sizeof(*attack));
    if (attack == nullptr) {
        return -1;
    }

    Object* explosion;
    int fid = buildFid(OBJ_TYPE_MISC, 10, 0, 0, 0);
    if (objectCreateWithFidPid(&explosion, fid, -1) == -1) {
        internal_free(attack);
        return -1;
    }

    objectHide(explosion, nullptr);
    explosion->flags |= OBJECT_NO_SAVE;

    objectSetLocation(explosion, tile, elevation, nullptr);

    Object* adjacentExplosions[ROTATION_COUNT];
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        int fid = buildFid(OBJ_TYPE_MISC, 10, 0, 0, 0);
        if (objectCreateWithFidPid(&(adjacentExplosions[rotation]), fid, -1) == -1) {
            while (--rotation >= 0) {
                objectDestroy(adjacentExplosions[rotation], nullptr);
            }

            objectDestroy(explosion, nullptr);
            internal_free(attack);
            return -1;
        }

        objectHide(adjacentExplosions[rotation], nullptr);
        adjacentExplosions[rotation]->flags |= OBJECT_NO_SAVE;

        int adjacentTile = tileGetTileInDirection(tile, rotation, 1);
        objectSetLocation(adjacentExplosions[rotation], adjacentTile, elevation, nullptr);
    }

    Object* critter = _obj_blocking_at(nullptr, tile, elevation);
    if (critter != nullptr) {
        if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER || (critter->data.critter.combat.results & DAM_DEAD) != 0) {
            critter = nullptr;
        }
    }

    attackInit(attack, explosion, critter, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);

    attack->tile = tile;
    attack->attackerFlags = DAM_HIT;

    gameUiDisable(1);

    if (critter != nullptr) {
        if (reg_anim_clear(critter) == -2) {
            debugPrint("Cannot clear target's animation for action_explode!\n");
        }
        attack->defenderDamage = _compute_explosion_damage(minDamage, maxDamage, critter, &(attack->defenderKnockback));
    }

    _compute_explosion_on_extras(attack, 0, 0, 1);

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* critter = attack->extras[index];
        if (reg_anim_clear(critter) == -2) {
            debugPrint("Cannot clear extra's animation for action_explode!\n");
        }

        attack->extrasDamage[index] = _compute_explosion_damage(minDamage, maxDamage, critter, &(attack->extrasKnockback[index]));
    }

    attackComputeDeathFlags(attack);

    // Headless: the animated branch welds damage/death/XP/scenery-destruction to
    // reg_anim callbacks (_report_explosion / _finished_explosion) whose sequence
    // is rejected by reg_anim_end under the server loop, dropping the whole
    // outcome. Force the non-animated branch, which applies the identical outcome
    // directly. Covers both the op_explosion opcode and the EXPLOSION queue event
    // (both funnel through here). Mirror of the actionDamage gate below.
    //
    // PRESENTATION-RECORD POC (PRESENTATION_RECORD_REPLAY_SPEC.md §11): on the
    // dedicated server with F2_SERVER_PRES_RECORD set, RUN this animate branch
    // (normally skipped by !serverLoopActive) inside a record section so its LEAVES
    // record a command stream (server_anim.cc + pres_record.cc), then ship it as
    // EVENT_PRES_SEQ instead of the bespoke explosionFx cue. The state branch below
    // still applies the outcome (the callbacks here are DROPped by the recorder).
    // Gate OFF (or any non-server build) → this is false → today's path exactly.
    bool recording = serverLoopActive() && presRecordEnabled();
    if (animate && (!serverLoopActive() || recording)) {
        if (recording) {
            presRecordSectionBegin();
        }
        _action_in_explode = true;

        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        _register_priority(1);
        animationRegisterPlaySoundEffect(explosion, "whn1xxx1", 0);
        animationRegisterUnsetFlag(explosion, OBJECT_HIDDEN, 0);
        animationRegisterAnimateAndHide(explosion, ANIM_STAND, 0);
        _show_damage(attack, 0, 1);

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            animationRegisterUnsetFlag(adjacentExplosions[rotation], OBJECT_HIDDEN, 0);
            animationRegisterAnimateAndHide(adjacentExplosions[rotation], ANIM_STAND, 0);
        }

        animationRegisterCallbackForced(explosion, nullptr, (AnimationCallback*)_combat_explode_scenery, -1);
        animationRegisterHideObjectForced(explosion);

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            animationRegisterHideObjectForced(adjacentExplosions[rotation]);
        }

        animationRegisterCallbackForced(attack, sourceObj, (AnimationCallback*)_report_explosion, -1);
        animationRegisterCallbackForced(nullptr, nullptr, (AnimationCallback*)_finished_explosion, -1);
        if (reg_anim_end() == -1) {
            _action_in_explode = false;
            if (recording) {
                presRecordSectionAbort();
            }

            objectDestroy(explosion, nullptr);

            for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                objectDestroy(adjacentExplosions[rotation], nullptr);
            }

            internal_free(attack);

            gameUiEnable();
            return -1;
        }

        _show_damage_extras(attack);

        if (recording) {
            // Close the section (restore RNG so the sim stream is untouched) and
            // ship the recorded stream. Clear the _action_in_explode latch the
            // DROPped _finished_explosion would have cleared. Then fall through to
            // the STATE block, which applies the outcome server-authoritatively.
            presRecordSectionEnd();
            _action_in_explode = false;
            presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount());
        } else {
            // SP client: the reg_anim callbacks (run for real) applied the outcome
            // and destroyed the transients — done, do not run the STATE block.
            return 0;
        }
    }

    if (serverLoopActive()) {
        // Server equivalent of the animated path: report/apply FIRST so XP is
        // awarded (mainTargetWasDead/extrasWasDead are false pre-kill) and each
        // victim's destroy_p_proc runs, THEN finalize corpses with critterKill
        // (the _show_death animation's job). Running critterKill first (as the
        // legacy branch does) sets DAM_DEAD + strips the script before
        // _apply_damage, dropping both the XP and the destroy procs. Mirrors
        // the _combat_apply_attack_results ordering. (_report_explosion frees
        // attack, so capture the dead targets first; max 1 defender + 6 extras.)
        Object* deadTargets[7];
        int deadTargetsLength = 0;
        if (critter != nullptr && (attack->defenderFlags & DAM_DEAD) != 0) {
            deadTargets[deadTargetsLength++] = critter;
        }
        for (int index = 0; index < attack->extrasLength; index++) {
            if ((attack->extrasFlags[index] & DAM_DEAD) != 0) {
                deadTargets[deadTargetsLength++] = attack->extras[index];
            }
        }

        // Displace surviving critters (away from the blast center = the
        // explosion object's tile) BEFORE _report_explosion, which runs
        // _apply_damage. The animated path completes the actionKnockdown tile
        // moves before _apply_damage, so the knockback/prone decision must see
        // each critter's PRE-damage state; running it after _report_explosion
        // would let the freshly-ORed DAM_KNOCKED_DOWN make _critter_is_prone
        // suppress the knockback. Death flags were set by attackComputeDeathFlags
        // above; _combat_apply_knockback skips DAM_DEAD victims itself.
        // Mirror of the _combat_apply_attack_results knockback block.
        _combat_apply_knockback(attack);

        _report_explosion(attack, sourceObj);

        for (int index = 0; index < deadTargetsLength; index++) {
            critterKill(deadTargets[index], -1, false);
        }
    } else {
        if (critter != nullptr) {
            if ((attack->defenderFlags & DAM_DEAD) != 0) {
                critterKill(critter, -1, false);
            }
        }

        for (int index = 0; index < attack->extrasLength; index++) {
            if ((attack->extrasFlags[index] & DAM_DEAD) != 0) {
                critterKill(attack->extras[index], -1, false);
            }
        }

        _report_explosion(attack, sourceObj);
    }

    // Common cloud tail (was the outer else-branch tail): destroy the transient
    // blast objects + run scenery damage. Runs for every state path (record and
    // non-record server, and the non-animate path); the SP animate branch returned
    // above (its reg_anim callbacks own teardown).
    _combat_explode_scenery(explosion, nullptr);

    objectDestroy(explosion, nullptr);

    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        objectDestroy(adjacentExplosions[rotation], nullptr);
    }

    return 0;
}

// Viewer replay of the explosion PRESENTATION (boom cloud + each caught critter's gib/
// knockdown), driven by EVENT_EXPLOSION_FX. This mirrors actionExplode's animate branch
// (the serverLoopActive() path skips it) MINUS the state callbacks (_report_explosion /
// _combat_explode_scenery / _finished_explosion — the server ran those and streamed the
// results). It creates the 7 transient cloud objects, points attack->attacker at the
// center cloud (so _show_damage_to_object's fid check picks ANIM_EXPLODED_TO_NOTHING),
// and registers the REAL _show_damage / _show_damage_extras for the victims. The caller
// fills attack->defender/extras + damage/flags from the wire with knockbacks ZEROED (the
// server applied them; they ride MOVE — replaying would fight the authoritative tile,
// same rule as the combat attack replay). Pure anim registration, ticked by the viewer's
// presAdvance -> _object_animate; out-of-combat safe (an explosion can detonate outside
// combat — the C4'd Temple door). See PRESENTATION_SEAM_DESIGN.md Stage 1/1b.
void actionExplodeReplay(int tile, int elevation, Attack* attack)
{
    int fid = buildFid(OBJ_TYPE_MISC, 10, 0, 0, 0);

    Object* explosion = nullptr;
    if (objectCreateWithFidPid(&explosion, fid, -1) == -1) {
        return;
    }
    objectHide(explosion, nullptr);
    explosion->flags |= OBJECT_NO_SAVE;
    objectSetLocation(explosion, tile, elevation, nullptr);

    Object* adjacentExplosions[ROTATION_COUNT];
    int adjacentCount = 0;
    for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
        Object* obj = nullptr;
        if (objectCreateWithFidPid(&obj, fid, -1) == -1) {
            break;
        }
        objectHide(obj, nullptr);
        obj->flags |= OBJECT_NO_SAVE;
        objectSetLocation(obj, tileGetTileInDirection(tile, rotation, 1), elevation, nullptr);
        adjacentExplosions[adjacentCount++] = obj;
    }

    // The center cloud is the attacker (its explosion fid drives the gib-anim choice).
    attack->attacker = explosion;
    attack->tile = tile;

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    _register_priority(1);
    animationRegisterPlaySoundEffect(explosion, "whn1xxx1", 0);
    animationRegisterUnsetFlag(explosion, OBJECT_HIDDEN, 0);
    animationRegisterAnimateAndHide(explosion, ANIM_STAND, 0);
    _show_damage(attack, 0, 1); // defender reaction (no-op when attack->defender == null)

    for (int rotation = 0; rotation < adjacentCount; rotation++) {
        animationRegisterUnsetFlag(adjacentExplosions[rotation], OBJECT_HIDDEN, 0);
        animationRegisterAnimateAndHide(adjacentExplosions[rotation], ANIM_STAND, 0);
    }

    animationRegisterHideObjectForced(explosion);
    for (int rotation = 0; rotation < adjacentCount; rotation++) {
        animationRegisterHideObjectForced(adjacentExplosions[rotation]);
    }
    if (reg_anim_end() == -1) {
        // Rejected (should not happen for brand-new objects) — tear the transients down
        // directly so nothing leaks, and skip the extras (their own sequences).
        objectDestroy(explosion, nullptr);
        for (int rotation = 0; rotation < adjacentCount; rotation++) {
            objectDestroy(adjacentExplosions[rotation], nullptr);
        }
        return;
    }

    _show_damage_extras(attack); // ring victims (each its own reg_anim sequence)
}

// 0x413144
int _report_explosion(Attack* attack, Object* sourceObj)
{
    bool mainTargetWasDead;
    if (attack->defender != nullptr) {
        mainTargetWasDead = (attack->defender->data.critter.combat.results & DAM_DEAD) != 0;
    } else {
        mainTargetWasDead = false;
    }

    bool extrasWasDead[6];
    for (int index = 0; index < attack->extrasLength; index++) {
        extrasWasDead[index] = (attack->extras[index]->data.critter.combat.results & DAM_DEAD) != 0;
    }

    attackComputeDeathFlags(attack);
    _combat_display(attack);
    _apply_damage(attack, false);

    Object* anyDefender = nullptr;
    int xp = 0;
    if (sourceObj != nullptr) {
        if (attack->defender != nullptr && attack->defender != sourceObj) {
            if ((attack->defender->data.critter.combat.results & DAM_DEAD) != 0) {
                if (sourceObj == gDude && !mainTargetWasDead) {
                    xp += critterGetExp(attack->defender);
                }
            } else {
                _critter_set_who_hit_me(attack->defender, sourceObj);
                anyDefender = attack->defender;
            }
        }

        for (int index = 0; index < attack->extrasLength; index++) {
            Object* critter = attack->extras[index];
            if (critter != sourceObj) {
                if ((critter->data.critter.combat.results & DAM_DEAD) != 0) {
                    if (sourceObj == gDude && !extrasWasDead[index]) {
                        xp += critterGetExp(critter);
                    }
                } else {
                    _critter_set_who_hit_me(critter, sourceObj);

                    if (anyDefender == nullptr) {
                        anyDefender = critter;
                    }
                }
            }
        }

        if (anyDefender != nullptr) {
            if (!isInCombat()) {
                CombatStartData combat;
                combat.attacker = anyDefender;
                combat.defender = sourceObj;
                combat.actionPointsBonus = 0;
                combat.accuracyBonus = 0;
                combat.damageBonus = 0;
                combat.minDamage = 0;
                combat.maxDamage = INT_MAX;
                combat.overrideAttackResults = 0;
                scriptsRequestCombat(&combat);
            }
        }
    }

    internal_free(attack);
    gameUiEnable();

    // Any player actor earns here, not just the host — and it earns as ITSELF.
    // playerActorIs IS `sourceObj == gDude` with an empty registry.
    if (playerActorIs(sourceObj)) {
        _combat_give_exps(xp, sourceObj);
    }

    return 0;
}

// 0x4132C0
int _finished_explosion(Object* a1, Object* a2)
{
    _action_in_explode = false;
    return 0;
}

// calculate explosion damage applying threshold and resistances
// 0x4132CC
int _compute_explosion_damage(int min, int max, Object* defender, int* knockbackDistancePtr)
{
    int damage = randomBetween(min, max) - critterGetStat(defender, STAT_DAMAGE_THRESHOLD_EXPLOSION);
    if (damage > 0) {
        damage -= critterGetStat(defender, STAT_DAMAGE_RESISTANCE_EXPLOSION) * damage / 100;
    }

    if (damage < 0) {
        damage = 0;
    }

    if (knockbackDistancePtr != nullptr) {
        if ((defender->flags & OBJECT_MULTIHEX) == 0) {
            *knockbackDistancePtr = damage / 10;
        }
    }

    return damage;
}

// 0x413330
int actionTalk(Object* a1, Object* a2)
{
    if (a1 != gDude) {
        return -1;
    }

    if (FID_TYPE(a2->fid) != OBJ_TYPE_CRITTER) {
        return -1;
    }

    int anim = FID_ANIM_TYPE(gDude->fid);
    if (anim == ANIM_WALK || anim == ANIM_RUNNING) {
        reg_anim_clear(gDude);
    }

    if (isInCombat()) {
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToObject(a1, a2, a1->data.critter.combat.ap, 0);
    } else {
        reg_anim_begin(a1 == gDude ? ANIMATION_REQUEST_RESERVED : ANIMATION_REQUEST_UNRESERVED);

        if (objectGetDistanceBetween(a1, a2) >= 9 || _combat_is_shot_blocked(a1, a1->tile, a2->tile, a2, nullptr)) {
            animationRegisterRunToObject(a1, a2, -1, 0);
        }
    }

    animationRegisterCallbackForced(a1, a2, (AnimationCallback*)_can_talk_to, -1);
    animationRegisterCallback(a1, a2, (AnimationCallback*)_talk_to, -1);
    return reg_anim_end();
}

// 0x413420
int _can_talk_to(Object* a1, Object* a2)
{
    if (_combat_is_shot_blocked(a1, a1->tile, a2->tile, a2, nullptr) || objectGetDistanceBetween(a1, a2) >= 9) {
        if (a1 == gDude) {
            // You cannot get there. (used in actions.c)
            MessageListItem messageListItem;
            messageListItem.num = 2000;
            if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
                presenter()->consoleMessage(messageListItem.text);
            }
        }

        return -1;
    }

    return 0;
}

// 0x413488
int _talk_to(Object* a1, Object* a2)
{
    scriptsRequestDialog(a2);
    return 0;
}

// 0x413494
void actionDamage(int tile, int elevation, int minDamage, int maxDamage, int damageType, bool animated, bool bypassArmor)
{
    Attack* attack = (Attack*)internal_malloc(sizeof(*attack));
    if (attack == nullptr) {
        return;
    }

    Object* attacker;
    if (objectCreateWithFidPid(&attacker, FID_0x20001F5, -1) == -1) {
        internal_free(attack);
        return;
    }

    objectHide(attacker, nullptr);

    attacker->flags |= OBJECT_NO_SAVE;

    objectSetLocation(attacker, tile, elevation, nullptr);

    Object* defender = _obj_blocking_at(nullptr, tile, elevation);
    attackInit(attack, attacker, defender, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
    attack->tile = tile;
    attack->attackerFlags = DAM_HIT;
    gameUiDisable(1);

    if (defender != nullptr) {
        reg_anim_clear(defender);

        int damage;
        if (bypassArmor) {
            damage = maxDamage;
        } else {
            damage = _compute_dmg_damage(minDamage, maxDamage, defender, &(attack->defenderKnockback), damageType);
        }

        attack->defenderDamage = damage;
    }

    attackComputeDeathFlags(attack);

    // Headless: the animated path welds _apply_damage/critterKill to the
    // _report_dmg reg_anim callback, which never fires under the server loop
    // (the sequence's reg_anim_end is rejected). Force the non-animated branch,
    // which applies the identical outcome directly (mirror of
    // _combat_apply_attack_results / actionExplode(animate=false)).
    //
    // PRESENTATION-RECORD (PRESENTATION_RECORD_REPLAY_SPEC.md §8, receive-damage /
    // hit-react): on the dedicated server with F2_SERVER_PRES_RECORD set, RUN this
    // animate branch (normally skipped by !serverLoopActive) inside a record section
    // so its LEAVES record a command stream, then ship it as EVENT_PRES_SEQ. The
    // STATE block below still applies the outcome (the _report_dmg callback here is
    // DROPped by the recorder). Gate OFF (or the client/golden binary) → recording
    // is false → today's path exactly. Mirror of the actionExplode gate above.
    //
    // The visible reaction (SFX + ANIM_HIT_FROM_* / death anim) rides the DEFENDER,
    // a persistent object the viewer resolves by netId. The synthetic `attacker`
    // (FID_0x20001F5, NO_SAVE, created THIS beat) is classified transient by
    // resolveRef (created-this-beat && NO_SAVE) → ships as OBJ_CREATE, so its
    // whc1xxx1 SFX + hide replay from a viewer-minted local transient. (This is the
    // first family to exercise OBJ_CREATE end-to-end for a synthetic attacker.)
    bool recording = serverLoopActive() && presRecordEnabled();
    if (animated && (!serverLoopActive() || recording)) {
        if (recording) {
            presRecordSectionBegin();
        }

        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterPlaySoundEffect(attacker, "whc1xxx1", 0);
        _show_damage(attack, gMaximumBloodDeathAnimations[damageType], 0);
        animationRegisterCallbackForced(attack, nullptr, (AnimationCallback*)_report_dmg, 0);
        animationRegisterHideObjectForced(attacker);

        if (reg_anim_end() == -1) {
            if (recording) {
                presRecordSectionAbort();
            }
            objectDestroy(attacker, nullptr);
            internal_free(attack);
            return;
        }

        if (!recording) {
            // SP client: the reg_anim callbacks (_report_dmg) apply the outcome and
            // free `attack`; the attacker is hidden by the sequence. Done — do NOT
            // run the STATE block below.
            gameUiEnable();
            return;
        }

        // Recording: close the section (restore RNG so the sim stream is untouched)
        // and ship the recorded stream, then fall through to the STATE block, which
        // applies the outcome server-authoritatively.
        presRecordSectionEnd();
        presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount());
    }

    if (serverLoopActive()) {
        // Server equivalent of the animated path: apply damage FIRST (via
        // _report_dmg -> _apply_damage) so the victim's destroy_p_proc runs and
        // all death consequences fire, THEN finalize the corpse physically with
        // critterKill (the _show_death animation's job). The legacy branch below
        // runs critterKill first, which sets DAM_DEAD + strips the script before
        // _apply_damage, silently dropping the destroy proc. Mirrors the
        // _combat_apply_attack_results ordering. (_report_dmg frees attack, so
        // capture the death decision first.)
        Object* deadDefender = (defender != nullptr && (attack->defenderFlags & DAM_DEAD) != 0) ? defender : nullptr;

        _report_dmg(attack, nullptr);

        if (deadDefender != nullptr) {
            critterKill(deadDefender, -1, 1);
        }

        objectDestroy(attacker, nullptr);
    } else {
        if (defender != nullptr) {
            if ((attack->defenderFlags & DAM_DEAD) != 0) {
                critterKill(defender, -1, 1);
            }
        }

        // NOTE: Uninline.
        _report_dmg(attack, nullptr);

        objectDestroy(attacker, nullptr);
    }

    gameUiEnable();
}

// 0x41363C
int _report_dmg(Attack* attack, Object* a2)
{
    _combat_display(attack);
    _apply_damage(attack, false);
    internal_free(attack);
    gameUiEnable();
    return 0;
}

// Calculate damage by applying threshold and resistances.
//
// 0x413660
int _compute_dmg_damage(int min, int max, Object* obj, int* knockbackDistancePtr, int damageType)
{
    if (!_critter_flag_check(obj->pid, CRITTER_NO_KNOCKBACK)) {
        knockbackDistancePtr = nullptr;
    }

    int damage = randomBetween(min, max) - critterGetStat(obj, STAT_DAMAGE_THRESHOLD + damageType);
    if (damage > 0) {
        damage -= critterGetStat(obj, STAT_DAMAGE_RESISTANCE + damageType) * damage / 100;
    }

    if (damage < 0) {
        damage = 0;
    }

    if (knockbackDistancePtr != nullptr) {
        if ((obj->flags & OBJECT_MULTIHEX) == 0 && damageType != DAMAGE_TYPE_ELECTRICAL) {
            *knockbackDistancePtr = damage / 10;
        }
    }

    return damage;
}

// 0x4136EC
bool actionCheckPush(Object* a1, Object* a2)
{
    // Cannot push anything but critters.
    if (FID_TYPE(a2->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    // Cannot push itself.
    if (a1 == a2) {
        return false;
    }

    // Cannot push inactive critters.
    if (!critterIsActive(a2)) {
        return false;
    }

    if (_action_can_talk_to(a1, a2) != 0) {
        return false;
    }

    // Can only push critters that have push handler.
    if (!scriptHasProc(a2->sid, SCRIPT_PROC_PUSH)) {
        return false;
    }

    if (isInCombat()) {
        if (a2->data.critter.combat.team == a1->data.critter.combat.team
            && a2 == a1->data.critter.combat.whoHitMe) {
            return false;
        }

        // TODO: Check.
        Object* whoHitMe = a2->data.critter.combat.whoHitMe;
        if (whoHitMe != nullptr
            && whoHitMe->data.critter.combat.team == a1->data.critter.combat.team) {
            return false;
        }
    }

    return true;
}

// 0x413790
int actionPush(Object* a1, Object* a2)
{
    if (!actionCheckPush(a1, a2)) {
        return -1;
    }

    int sid;
    if (_obj_sid(a2, &sid) == 0) {
        scriptSetObjects(sid, a1, a2);
        scriptExecProc(sid, SCRIPT_PROC_PUSH);

        bool scriptOverrides = false;

        Script* scr;
        if (scriptGetScript(sid, &scr) != -1) {
            scriptOverrides = scr->scriptOverrides;
        }

        if (scriptOverrides) {
            return -1;
        }
    }

    int rotation = tileGetRotationTo(a1->tile, a2->tile);
    int tile;
    do {
        tile = tileGetTileInDirection(a2->tile, rotation, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        tile = tileGetTileInDirection(a2->tile, (rotation + 1) % ROTATION_COUNT, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        tile = tileGetTileInDirection(a2->tile, (rotation + 5) % ROTATION_COUNT, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        tile = tileGetTileInDirection(a2->tile, (rotation + 2) % ROTATION_COUNT, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        tile = tileGetTileInDirection(a2->tile, (rotation + 4) % ROTATION_COUNT, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        tile = tileGetTileInDirection(a2->tile, (rotation + 3) % ROTATION_COUNT, 1);
        if (_obj_blocking_at(a2, tile, a2->elevation) == nullptr) {
            break;
        }

        return -1;
    } while (0);

    int actionPoints;
    if (isInCombat()) {
        actionPoints = a2->data.critter.combat.ap;
    } else {
        actionPoints = -1;
    }

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    animationRegisterRotateToTile(a2, tile);
    animationRegisterMoveToTile(a2, tile, a2->elevation, actionPoints, 0);
    return reg_anim_end();
}

// Returns -1 if can't see there (can't find a path there)
// Returns -2 if it's too far (> 12 tiles).
//
// 0x413970
// Ledger H-5 (extracted from the inventory UI's _exit_inventory): resolving a
// live explosive dropped on the ground — nearby hostile critters roll
// perception (CONSUMES RNG), notice the dropper, and combat starts if anyone
// noticed. Must run on the sim authority, never in a UI teardown path.
void actionResolveDroppedExplosive(Object* dropper)
{
    Attack attack;
    attackInit(&attack, dropper, nullptr, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);
    attack.attackerFlags = DAM_HIT;
    attack.tile = dropper->tile;
    _compute_explosion_on_extras(&attack, 0, 0, 1);

    Object* watcher = nullptr;
    for (int index = 0; index < attack.extrasLength; index++) {
        Object* critter = attack.extras[index];
        if (critter != dropper
            && critter->data.critter.combat.team != dropper->data.critter.combat.team
            && statRoll(critter, STAT_PERCEPTION, 0, nullptr) >= ROLL_SUCCESS) {
            _critter_set_who_hit_me(critter, dropper);

            if (watcher == nullptr) {
                watcher = critter;
            }
        }
    }

    if (watcher != nullptr) {
        if (!isInCombat()) {
            CombatStartData combat;
            combat.attacker = watcher;
            combat.defender = dropper;
            combat.actionPointsBonus = 0;
            combat.accuracyBonus = 0;
            combat.damageBonus = 0;
            combat.minDamage = 0;
            combat.maxDamage = INT_MAX;
            combat.overrideAttackResults = 0;
            scriptsRequestCombat(&combat);
        }
    }
}

// Ledger H-2 (extracted from the inventory UI's inventoryOpenUseItemOn):
// combat wrapper for using an inventory item on a target — gates on and
// charges the 2 AP cost when the use succeeds. Outside combat it is a plain
// pass-through.
int actionUseItemOnObjectWithApCost(Object* user, Object* targetObj, Object* item)
{
    // Policy switch, combat_ap.h: charging off → fall through to the untaxed
    // pass-through below. One of the three interaction charge points.
    if (isInCombat() && kCombatApChargeEnabled) {
        if (user->data.critter.combat.ap < 2) {
            return -1;
        }

        int rc = _action_use_an_item_on_object(user, targetObj, item);
        if (rc != -1) {
            int actionPoints = user->data.critter.combat.ap;
            if (actionPoints < 2) {
                user->data.critter.combat.ap = 0;
            } else {
                user->data.critter.combat.ap = actionPoints - 2;
            }

            presenter()->hudActionPoints(user->data.critter.combat.ap, _combat_free_move);
        }

        return rc;
    }

    return _action_use_an_item_on_object(user, targetObj, item);
}

int _action_can_talk_to(Object* a1, Object* a2)
{
    if (pathfinderFindPath(a1, a1->tile, a2->tile, nullptr, 0, _obj_sight_blocking_at) == 0) {
        return -1;
    }

    if (tileDistanceBetween(a1->tile, a2->tile) > 12) {
        return -2;
    }

    return 0;
}

static int hideProjectile(void* a1, void* a2)
{
    Object* projectile = reinterpret_cast<Object*>(a2);

    Rect rect;
    if (objectHide(projectile, &rect) == 0) {
        presenter()->worldInvalidateRect(&rect, projectile->elevation);
    }

    projectile->flags |= OBJECT_NO_SAVE;

    return 0;
}

// PRESENTATION-RECORD: the pointer the recorder compares against to fold the ranged
// projectile-hide callback into PRES_OP_HIDE_FORCED (mirror of actionShowDeathCallback-
// Ptr). hideProjectile stays static; the recorder identifies it by pointer, then the
// viewer replays the hide via its own reg_anim (HIDE_FORCED already interpreted client-
// side). No new callback tag is spent — the fold reuses the leaf op at the same sequence
// position, so the action-frame timing survives (PRESENTATION_RECORD_REPLAY_SPEC §4.2).
void* actionHideProjectileCallbackPtr()
{
    return (void*)(AnimationCallback*)hideProjectile;
}

} // namespace fallout
