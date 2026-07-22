#include "stat.h"

#include <stdio.h>

#include <algorithm>

#include "art.h"
#include "combat.h"
#include "critter.h"
#include "display_monitor.h"
#include "game.h"
#include "game_sound.h"
#include "interface.h"
#include "item.h"
#include "memory.h"
#include "message.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "platform_compat.h"
#include "presenter.h"
#include "proto.h"
#include "random.h"
#include "scripts.h"
#include "server_players.h" // playerActorSlotOf — per-actor PC-stat rows
#include "skill.h"
#include "svga.h"
#include "tile.h"
#include "trait.h"

namespace fallout {

// Provides metadata about stats.
typedef struct StatDescription {
    char* name;
    char* description;
    int frmId;
    int minimumValue;
    int maximumValue;
    int defaultValue;
} StatDescription;

// 0x51D53C
static StatDescription gStatDescriptions[STAT_COUNT] = {
    { nullptr, nullptr, 0, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 1, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 2, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 3, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 4, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 5, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 6, PRIMARY_STAT_MIN, PRIMARY_STAT_MAX, 5 },
    { nullptr, nullptr, 10, 0, 999, 0 },
    { nullptr, nullptr, 75, 1, 99, 0 },
    { nullptr, nullptr, 18, 0, 999, 0 },
    { nullptr, nullptr, 31, 0, INT_MAX, 0 },
    { nullptr, nullptr, 32, 0, 500, 0 },
    { nullptr, nullptr, 20, 0, 999, 0 },
    { nullptr, nullptr, 24, 0, 60, 0 },
    { nullptr, nullptr, 25, 0, 30, 0 },
    { nullptr, nullptr, 26, 0, 100, 0 },
    { nullptr, nullptr, 94, -60, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 22, 0, 90, 0 },
    { nullptr, nullptr, 0, 0, 90, 0 },
    { nullptr, nullptr, 0, 0, 90, 0 },
    { nullptr, nullptr, 0, 0, 90, 0 },
    { nullptr, nullptr, 0, 0, 90, 0 },
    { nullptr, nullptr, 0, 0, 100, 0 },
    { nullptr, nullptr, 0, 0, 90, 0 },
    { nullptr, nullptr, 83, 0, 95, 0 },
    { nullptr, nullptr, 23, 0, 95, 0 },
    { nullptr, nullptr, 0, 16, 101, 25 },
    { nullptr, nullptr, 0, 0, 1, 0 },
    { nullptr, nullptr, 10, 0, 2000, 0 },
    { nullptr, nullptr, 11, 0, 2000, 0 },
    { nullptr, nullptr, 12, 0, 2000, 0 },
};

// 0x51D8CC
static StatDescription gPcStatDescriptions[PC_STAT_COUNT] = {
    { nullptr, nullptr, 0, 0, INT_MAX, 0 },
    { nullptr, nullptr, 0, 1, PC_LEVEL_MAX, 1 },
    { nullptr, nullptr, 0, 0, INT_MAX, 0 },
    { nullptr, nullptr, 0, -20, 20, 0 },
    { nullptr, nullptr, 0, 0, INT_MAX, 0 },
};

// 0x66817C
static MessageList gStatsMessageList;

// 0x668184
static char* gStatValueDescriptions[PRIMARY_STAT_RANGE];

// 0x6681AC
static int gPcStatValues[PC_STAT_COUNT];

// XP / level / karma / unspent skill points for EXTRA player actors, slots
// 1..kMaxPlayerActors-1 (index = slot - 1). Slot 0 is gPcStatValues above and is
// NOT stored here — same aliasing as the sheet's proto row, for the same reason
// (PLAYER_SHEET_DESIGN.md §2): with an empty registry no subject can resolve to
// this array, so single-player, the client and the golden probe never touch it.
static int gPlayerActorPcStats[kMaxPlayerActors - 1][PC_STAT_COUNT];

// The PC-stat row for `subject`. THE resolver — nothing else may index either
// array (PLAYER_SHEET_DESIGN.md §3).
//
// nullptr means "the caller has no subject in hand", which resolves to gDude and
// therefore to today's behavior verbatim. That default is what lets the ~45
// existing pcGetStat / pcSetStat call sites stay untouched: only the ones that
// genuinely know WHOSE experience this is need to say so.
//
// ⚠ An unregistered critter (a companion, a script handing us any old object)
// also lands on slot 0. That is deliberate and matches §4's disclosed policy for
// ally kills — it is today's behavior, not new guesswork — but it does mean this
// resolver cannot be used to detect "not a player".
static int* pcStatRow(Object* subject)
{
    int slot = playerActorSlotOf(subject != nullptr ? subject : gDude);
    return slot > 0 ? gPlayerActorPcStats[slot - 1] : gPcStatValues;
}

// 0x4AED70
int statsInit()
{
    MessageListItem messageListItem;

    // NOTE: Uninline.
    pcStatsReset();

    if (!messageListInit(&gStatsMessageList)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "stat.msg");

    if (!messageListLoad(&gStatsMessageList, path)) {
        return -1;
    }

    for (int stat = 0; stat < STAT_COUNT; stat++) {
        gStatDescriptions[stat].name = getmsg(&gStatsMessageList, &messageListItem, 100 + stat);
        gStatDescriptions[stat].description = getmsg(&gStatsMessageList, &messageListItem, 200 + stat);
    }

    for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
        gPcStatDescriptions[pcStat].name = getmsg(&gStatsMessageList, &messageListItem, 400 + pcStat);
        gPcStatDescriptions[pcStat].description = getmsg(&gStatsMessageList, &messageListItem, 500 + pcStat);
    }

    for (int index = 0; index < PRIMARY_STAT_RANGE; index++) {
        gStatValueDescriptions[index] = getmsg(&gStatsMessageList, &messageListItem, 301 + index);
    }

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_STAT, &gStatsMessageList);

    return 0;
}

// 0x4AEEC0
int statsReset()
{
    // NOTE: Uninline.
    pcStatsReset();

    return 0;
}

// 0x4AEEE4
int statsExit()
{
    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_STAT, nullptr);
    messageListFree(&gStatsMessageList);

    return 0;
}

// 0x4AEEF4
int statsLoad(File* stream)
{
    for (int index = 0; index < PC_STAT_COUNT; index++) {
        if (fileReadInt32(stream, &(gPcStatValues[index])) == -1) {
            return -1;
        }
    }

    return 0;
}

// 0x4AEF20
int statsSave(File* stream)
{
    for (int index = 0; index < PC_STAT_COUNT; index++) {
        if (fileWriteInt32(stream, gPcStatValues[index]) == -1) {
            return -1;
        }
    }

    return 0;
}

// 0x4AEF48
int critterGetStat(Object* critter, int stat)
{
    int value;
    if (stat >= 0 && stat < SAVEABLE_STAT_COUNT) {
        value = critterGetBaseStatWithTraitModifier(critter, stat);
        value += critterGetBonusStat(critter, stat);

        switch (stat) {
        case STAT_PERCEPTION:
            if ((critter->data.critter.combat.results & DAM_BLIND) != 0) {
                value -= 5;
            }
            break;
        case STAT_MAXIMUM_ACTION_POINTS:
            if (1) {
                int remainingCarryWeight = critterGetStat(critter, STAT_CARRY_WEIGHT) - objectGetInventoryWeight(critter);
                if (remainingCarryWeight < 0) {
                    value -= -remainingCarryWeight / 40 + 1;
                }
            }
            break;
        case STAT_ARMOR_CLASS:
            if (isInCombat()) {
                if (_combat_whose_turn() != critter) {
                    int actionPointsMultiplier = 1;
                    int hthEvadeBonus = 0;

                    // Every player actor evades with its OWN unarmed skill and
                    // its OWN equipped weapons — the whole block reads `critter`,
                    // not gDude, or an extra's armor class is computed from what
                    // the HOST happens to be holding.
                    if (playerActorIs(critter)) {
                        if (perkHasRank(critter, PERK_HTH_EVADE)) {
                            bool hasWeapon = false;

                            Object* item2 = critterGetItem2(critter);
                            if (item2 != nullptr) {
                                if (itemGetType(item2) == ITEM_TYPE_WEAPON) {
                                    if (weaponGetAnimationCode(item2) != WEAPON_ANIMATION_NONE) {
                                        hasWeapon = true;
                                    }
                                }
                            }

                            if (!hasWeapon) {
                                Object* item1 = critterGetItem1(critter);
                                if (item1 != nullptr) {
                                    if (itemGetType(item1) == ITEM_TYPE_WEAPON) {
                                        if (weaponGetAnimationCode(item1) != WEAPON_ANIMATION_NONE) {
                                            hasWeapon = true;
                                        }
                                    }
                                }
                            }

                            if (!hasWeapon) {
                                actionPointsMultiplier = 2;
                                hthEvadeBonus = skillGetValue(critter, SKILL_UNARMED) / 12;
                            }
                        }
                    }
                    value += hthEvadeBonus;
                    value += critter->data.critter.combat.ap * actionPointsMultiplier;
                }
            }
            break;
        case STAT_AGE:
            value += gameTimeGetTime() / GAME_TIME_TICKS_PER_YEAR;
            break;
        }

        if (critter == gDude) {
            switch (stat) {
            case STAT_STRENGTH:
                if (perkGetRank(critter, PERK_GAIN_STRENGTH)) {
                    value++;
                }

                if (perkGetRank(critter, PERK_ADRENALINE_RUSH)) {
                    if (critterGetStat(critter, STAT_CURRENT_HIT_POINTS) < (critterGetStat(critter, STAT_MAXIMUM_HIT_POINTS) / 2)) {
                        value++;
                    }
                }
                break;
            case STAT_PERCEPTION:
                if (perkGetRank(critter, PERK_GAIN_PERCEPTION)) {
                    value++;
                }
                break;
            case STAT_ENDURANCE:
                if (perkGetRank(critter, PERK_GAIN_ENDURANCE)) {
                    value++;
                }
                break;
            case STAT_CHARISMA:
                if (1) {
                    if (perkGetRank(critter, PERK_GAIN_CHARISMA)) {
                        value++;
                    }

                    bool hasMirrorShades = false;

                    Object* item2 = critterGetItem2(critter);
                    if (item2 != nullptr && item2->pid == PROTO_ID_MIRRORED_SHADES) {
                        hasMirrorShades = true;
                    }

                    Object* item1 = critterGetItem1(critter);
                    if (item1 != nullptr && item1->pid == PROTO_ID_MIRRORED_SHADES) {
                        hasMirrorShades = true;
                    }

                    if (hasMirrorShades) {
                        value++;
                    }
                }
                break;
            case STAT_INTELLIGENCE:
                if (perkGetRank(critter, PERK_GAIN_INTELLIGENCE)) {
                    value++;
                }
                break;
            case STAT_AGILITY:
                if (perkGetRank(critter, PERK_GAIN_AGILITY)) {
                    value++;
                }
                break;
            case STAT_LUCK:
                if (perkGetRank(critter, PERK_GAIN_LUCK)) {
                    value++;
                }
                break;
            case STAT_MAXIMUM_HIT_POINTS:
                if (perkGetRank(critter, PERK_ALCOHOL_RAISED_HIT_POINTS)) {
                    value += 2;
                }

                if (perkGetRank(critter, PERK_ALCOHOL_RAISED_HIT_POINTS_II)) {
                    value += 4;
                }

                if (perkGetRank(critter, PERK_ALCOHOL_LOWERED_HIT_POINTS)) {
                    value -= 2;
                }

                if (perkGetRank(critter, PERK_ALCOHOL_LOWERED_HIT_POINTS_II)) {
                    value -= 4;
                }

                if (perkGetRank(critter, PERK_AUTODOC_RAISED_HIT_POINTS)) {
                    value += 2;
                }

                if (perkGetRank(critter, PERK_AUTODOC_RAISED_HIT_POINTS_II)) {
                    value += 4;
                }

                if (perkGetRank(critter, PERK_AUTODOC_LOWERED_HIT_POINTS)) {
                    value -= 2;
                }

                if (perkGetRank(critter, PERK_AUTODOC_LOWERED_HIT_POINTS_II)) {
                    value -= 4;
                }
                break;
            case STAT_DAMAGE_RESISTANCE:
            case STAT_DAMAGE_RESISTANCE_EXPLOSION:
                if (perkGetRank(critter, PERK_DERMAL_IMPACT_ARMOR)) {
                    value += 5;
                } else if (perkGetRank(critter, PERK_DERMAL_IMPACT_ASSAULT_ENHANCEMENT)) {
                    value += 10;
                }
                break;
            case STAT_DAMAGE_RESISTANCE_LASER:
            case STAT_DAMAGE_RESISTANCE_FIRE:
            case STAT_DAMAGE_RESISTANCE_PLASMA:
                if (perkGetRank(critter, PERK_PHOENIX_ARMOR_IMPLANTS)) {
                    value += 5;
                } else if (perkGetRank(critter, PERK_PHOENIX_ASSAULT_ENHANCEMENT)) {
                    value += 10;
                }
                break;
            case STAT_RADIATION_RESISTANCE:
            case STAT_POISON_RESISTANCE:
                if (perkGetRank(critter, PERK_VAULT_CITY_INOCULATIONS)) {
                    value += 10;
                }
                break;
            }
        }

        value = std::clamp(value, gStatDescriptions[stat].minimumValue, gStatDescriptions[stat].maximumValue);
    } else {
        switch (stat) {
        case STAT_CURRENT_HIT_POINTS:
            value = critterGetHitPoints(critter);
            break;
        case STAT_CURRENT_POISON_LEVEL:
            value = critterGetPoison(critter);
            break;
        case STAT_CURRENT_RADIATION_LEVEL:
            value = critterGetRadiation(critter);
            break;
        default:
            value = 0;
            break;
        }
    }

    return value;
}

// Returns base stat value (accounting for traits if critter is dude).
//
// 0x4AF3E0
int critterGetBaseStatWithTraitModifier(Object* critter, int stat)
{
    int value = critterGetBaseStat(critter, stat);

    // EVERY player actor gets its own trait modifiers, not just the host —
    // playerActorIs IS `critter == gDude` with an empty registry.
    if (playerActorIs(critter)) {
        value += traitGetStatModifier(stat, critter);
    }

    return value;
}

// 0x4AF408
int critterGetBaseStat(Object* critter, int stat)
{
    Proto* proto;

    if (stat >= 0 && stat < SAVEABLE_STAT_COUNT) {
        // MP hardening: a critter's pid must resolve to a proto. It ALWAYS does in
        // single-player, so vanilla dereferences unconditionally — but a co-op
        // viewer can transiently hold an actor whose pid does not resolve during a
        // rebaseline, and the unchecked deref then reads out of bounds (ASAN:
        // heap-buffer-overflow). Degrade to 0 instead of crashing; the next
        // interface update reads the correct value once the world settles.
        //
        // The pid must also be a CRITTER pid. baseStats lives deep inside
        // CritterProto (proto_size 0x1A0); every other proto type is allocated at
        // its own, smaller size (proto.cc _proto_sizes — ItemProto 0x84, SceneryProto
        // 0x38, ...). skillUse (skill.cc:663-664) reads the TARGET's HP stats before
        // it branches on skill, so using Lockpick/Repair/Traps on a door, container
        // or item reaches here with a non-critter target; indexing baseStats[stat]
        // then runs off the end of that smaller allocation. Vanilla has the same
        // unconditional read but got away with it — the OOB bytes landed in adjacent
        // 32-bit heap and were never used for non-healing skills. Return 0: a
        // non-critter has no SPECIAL/derived stats, and the healing math that would
        // consume HP only runs for critter targets.
        if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER
            || protoGetProto(critter->pid, &proto) == -1 || proto == nullptr) {
            return 0;
        }
        return proto->critter.data.baseStats[stat];
    } else {
        switch (stat) {
        case STAT_CURRENT_HIT_POINTS:
            return critterGetHitPoints(critter);
        case STAT_CURRENT_POISON_LEVEL:
            return critterGetPoison(critter);
        case STAT_CURRENT_RADIATION_LEVEL:
            return critterGetRadiation(critter);
        }
    }

    return 0;
}

// 0x4AF474
int critterGetBonusStat(Object* critter, int stat)
{
    if (stat >= 0 && stat < SAVEABLE_STAT_COUNT) {
        Proto* proto;
        // MP hardening + non-critter guard, same as critterGetBaseStat — see the
        // note there. bonusStats is the same CritterProto-only array, so a
        // non-critter target (Lockpick/Repair on a door) would read off the end of
        // its smaller proto allocation.
        if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER
            || protoGetProto(critter->pid, &proto) == -1 || proto == nullptr) {
            return 0;
        }
        return proto->critter.data.bonusStats[stat];
    }

    return 0;
}

// 0x4AF4BC
int critterSetBaseStat(Object* critter, int stat, int value)
{
    Proto* proto;

    if (!statIsValid(stat)) {
        return -5;
    }

    if (stat >= 0 && stat < SAVEABLE_STAT_COUNT) {
        if (stat > STAT_LUCK && stat <= STAT_POISON_RESISTANCE) {
            // Cannot change base value of derived stats.
            return -1;
        }

        if (playerActorIs(critter)) {
            value -= traitGetStatModifier(stat, critter);
        }

        if (value < gStatDescriptions[stat].minimumValue) {
            return -2;
        }

        if (value > gStatDescriptions[stat].maximumValue) {
            return -3;
        }

        protoGetProto(critter->pid, &proto);
        proto->critter.data.baseStats[stat] = value;

        if (stat >= STAT_STRENGTH && stat <= STAT_LUCK) {
            critterUpdateDerivedStats(critter);
        }

        return 0;
    }

    switch (stat) {
    case STAT_CURRENT_HIT_POINTS:
        return critterAdjustHitPoints(critter, value - critterGetHitPoints(critter));
    case STAT_CURRENT_POISON_LEVEL:
        return critterAdjustPoison(critter, value - critterGetPoison(critter));
    case STAT_CURRENT_RADIATION_LEVEL:
        return critterAdjustRadiation(critter, value - critterGetRadiation(critter));
    }

    // Should be unreachable
    return 0;
}

// 0x4AF5D4
int critterIncBaseStat(Object* critter, int stat)
{
    int value = critterGetBaseStat(critter, stat);

    // EVERY player actor gets its own trait modifiers, not just the host —
    // playerActorIs IS `critter == gDude` with an empty registry.
    if (playerActorIs(critter)) {
        value += traitGetStatModifier(stat, critter);
    }

    return critterSetBaseStat(critter, stat, value + 1);
}

// 0x4AF608
int critterDecBaseStat(Object* critter, int stat)
{
    int value = critterGetBaseStat(critter, stat);

    // EVERY player actor gets its own trait modifiers, not just the host —
    // playerActorIs IS `critter == gDude` with an empty registry.
    if (playerActorIs(critter)) {
        value += traitGetStatModifier(stat, critter);
    }

    return critterSetBaseStat(critter, stat, value - 1);
}

// 0x4AF63C
int critterSetBonusStat(Object* critter, int stat, int value)
{
    if (!statIsValid(stat)) {
        return -5;
    }

    if (stat >= 0 && stat < SAVEABLE_STAT_COUNT) {
        Proto* proto;
        protoGetProto(critter->pid, &proto);
        proto->critter.data.bonusStats[stat] = value;

        if (stat >= STAT_STRENGTH && stat <= STAT_LUCK) {
            critterUpdateDerivedStats(critter);
        }

        return 0;
    } else {
        switch (stat) {
        case STAT_CURRENT_HIT_POINTS:
            return critterAdjustHitPoints(critter, value);
        case STAT_CURRENT_POISON_LEVEL:
            return critterAdjustPoison(critter, value);
        case STAT_CURRENT_RADIATION_LEVEL:
            return critterAdjustRadiation(critter, value);
        }
    }

    // Should be unreachable
    return -1;
}

// 0x4AF6CC
void protoCritterDataResetStats(CritterProtoData* data)
{
    for (int stat = 0; stat < SAVEABLE_STAT_COUNT; stat++) {
        data->baseStats[stat] = gStatDescriptions[stat].defaultValue;
        data->bonusStats[stat] = 0;
    }
}

// 0x4AF6FC
void critterUpdateDerivedStats(Object* critter)
{
    int strength = critterGetStat(critter, STAT_STRENGTH);
    int perception = critterGetStat(critter, STAT_PERCEPTION);
    int endurance = critterGetStat(critter, STAT_ENDURANCE);
    int intelligence = critterGetStat(critter, STAT_INTELLIGENCE);
    int agility = critterGetStat(critter, STAT_AGILITY);
    int luck = critterGetStat(critter, STAT_LUCK);

    Proto* proto;
    protoGetProto(critter->pid, &proto);
    CritterProtoData* data = &(proto->critter.data);

    data->baseStats[STAT_MAXIMUM_HIT_POINTS] = critterGetBaseStatWithTraitModifier(critter, STAT_STRENGTH) + critterGetBaseStatWithTraitModifier(critter, STAT_ENDURANCE) * 2 + 15;
    data->baseStats[STAT_MAXIMUM_ACTION_POINTS] = agility / 2 + 5;
    data->baseStats[STAT_ARMOR_CLASS] = agility;
    data->baseStats[STAT_MELEE_DAMAGE] = std::max(strength - 5, 1);
    data->baseStats[STAT_CARRY_WEIGHT] = 25 * strength + 25;
    data->baseStats[STAT_SEQUENCE] = 2 * perception;
    data->baseStats[STAT_HEALING_RATE] = std::max(endurance / 3, 1);
    data->baseStats[STAT_CRITICAL_CHANCE] = luck;
    data->baseStats[STAT_BETTER_CRITICALS] = 0;
    data->baseStats[STAT_RADIATION_RESISTANCE] = 2 * endurance;
    data->baseStats[STAT_POISON_RESISTANCE] = 5 * endurance;
}

// 0x4AF854
char* statGetName(int stat)
{
    return statIsValid(stat) ? gStatDescriptions[stat].name : nullptr;
}

// 0x4AF898
char* statGetDescription(int stat)
{
    return statIsValid(stat) ? gStatDescriptions[stat].description : nullptr;
}

// 0x4AF8DC
char* statGetValueDescription(int value)
{
    if (value < PRIMARY_STAT_MIN) {
        value = PRIMARY_STAT_MIN;
    } else if (value > PRIMARY_STAT_MAX) {
        value = PRIMARY_STAT_MAX;
    }

    return gStatValueDescriptions[value - PRIMARY_STAT_MIN];
}

// 0x4AF8FC
int pcGetStat(int pcStat, Object* subject)
{
    return pcStatIsValid(pcStat) ? pcStatRow(subject)[pcStat] : 0;
}

// 0x4AF910
int pcSetStat(int pcStat, int value, Object* subject)
{
    int result;

    if (!pcStatIsValid(pcStat)) {
        return -5;
    }

    if (value < gPcStatDescriptions[pcStat].minimumValue) {
        return -2;
    }

    if (value > gPcStatDescriptions[pcStat].maximumValue) {
        return -3;
    }

    int* row = pcStatRow(subject);

    if (pcStat != PC_STAT_EXPERIENCE || value >= row[PC_STAT_EXPERIENCE]) {
        row[pcStat] = value;
        if (pcStat == PC_STAT_EXPERIENCE) {
            result = pcAddExperienceWithOptions(0, true, nullptr, subject);
        } else {
            result = 0;
        }
    } else {
        result = pcSetExperience(value, subject);
    }

    return result;
}

// Ledger H-43 (extracted from the character editor's level handler): the
// level-up award. Per level gained: skill points 5 + INT*2 + Educated-rank*2
// + 5 if Skilled, -5 if Gifted (floored at 0), capped at 99 unspent; plus a
// free perk every 3rd (4th with Skilled) level while under the 37-perk cap.
// Returns true when a free perk was earned.
bool pcLevelUpApply(int fromLevel, int toLevel)
{
    bool freePerk = false;

    for (int nextLevel = fromLevel + 1; nextLevel <= toLevel; nextLevel++) {
        int sp = pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS);
        sp += 5;
        sp += critterGetBaseStatWithTraitModifier(gDude, STAT_INTELLIGENCE) * 2;
        sp += perkGetRank(gDude, PERK_EDUCATED) * 2;
        sp += traitIsSelected(TRAIT_SKILLED) * 5;
        if (traitIsSelected(TRAIT_GIFTED)) {
            sp -= 5;
            if (sp < 0) {
                sp = 0;
            }
        }
        if (sp > 99) {
            sp = 99;
        }

        pcSetStat(PC_STAT_UNSPENT_SKILL_POINTS, sp);

        int selectedPerksCount = 0;
        for (int perk = 0; perk < PERK_COUNT; perk++) {
            if (perkGetRank(gDude, perk) != 0) {
                selectedPerksCount += 1;
                if (selectedPerksCount >= 37) {
                    break;
                }
            }
        }

        if (selectedPerksCount < 37) {
            int progression = 3;
            if (traitIsSelected(TRAIT_SKILLED)) {
                progression += 1;
            }

            if (nextLevel % progression == 0) {
                freePerk = true;
            }
        }
    }

    return freePerk;
}

// Reset stats.
//
// 0x4AF980
void pcStatsReset()
{
    for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
        gPcStatValues[pcStat] = gPcStatDescriptions[pcStat].defaultValue;
    }

    // Extra actors reset with the host — these are file statics, not malloc'd per
    // run, so a new game would otherwise inherit the previous one's XP and level
    // (the same trap perkResetRanks has).
    for (int slot = 0; slot < kMaxPlayerActors - 1; slot++) {
        for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
            gPlayerActorPcStats[slot][pcStat] = gPcStatDescriptions[pcStat].defaultValue;
        }
    }
}

// Seed every extra actor's PC stats from the host's — the XP/level half of
// protoPlayerActorSheetsSeed + perkPlayerActorSeedRanks (PLAYER_SHEET_DESIGN.md
// stage 2), and the third member of that set: seed some and not others and the
// actor is a chimera.
//
// Why not leave extras at the defaults: co-op v1 is one authored character, so an
// extra JOINS AS that character and diverges from there. Starting it at 0 XP /
// level 1 while it carries the host's skills and HP is incoherent — and it stops
// being cosmetic the moment the host is high level, because the level a player
// stands at drives the next level-up's HP award.
void pcPlayerActorSeedStats()
{
    for (int slot = 1; slot < kMaxPlayerActors; slot++) {
        pcPlayerActorSeedStatsSlot(slot);
    }
}

// ONE slot, for the dynamic spawn-at-login path (ACCOUNT_IDENTITY_DESIGN.md §3).
// ⚠ Never call the bulk seeder above with players live — it would reset every
// extra's XP/level/karma to the host's (trap 1), and the level a player stands at
// drives the next level-up's HP award, so this is not cosmetic.
//
// ⚠ Takes an ACTOR SLOT (1..kMaxPlayerActors-1); the array is indexed slot-1. The
// bulk loop above historically ran over ARRAY indices, which is the same set but
// the opposite convention — do not copy that loop's bounds here.
void pcPlayerActorSeedStatsSlot(int slot)
{
    if (slot < 1 || slot >= kMaxPlayerActors) {
        return;
    }

    for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
        gPlayerActorPcStats[slot - 1][pcStat] = gPcStatValues[pcStat];
    }
}

// One actor's XP / level / karma / unspent skill points (PLAYER_SHEET_DESIGN.md
// §5). Slot 0 is gPcStatValues — the bare global statsSave already writes, so
// the host's row travels through the identical path as an extra's.
static int* pcPlayerActorRow(int slot)
{
    if (slot == 0) {
        return gPcStatValues;
    }

    if (slot < 1 || slot >= kMaxPlayerActors) {
        return nullptr;
    }

    return gPlayerActorPcStats[slot - 1];
}

int pcPlayerActorRowWrite(File* stream, int slot)
{
    int* row = pcPlayerActorRow(slot);
    if (row == nullptr) {
        return -1;
    }

    return fileWriteInt32List(stream, row, PC_STAT_COUNT);
}

int pcPlayerActorRowRead(File* stream, int slot)
{
    int* row = pcPlayerActorRow(slot);
    if (row == nullptr) {
        return -1;
    }

    return fileReadInt32List(stream, row, PC_STAT_COUNT);
}

// Returns experience to reach next level.
//
// 0x4AF9A0
int pcGetExperienceForNextLevel(Object* subject)
{
    return pcGetExperienceForLevel(pcStatRow(subject)[PC_STAT_LEVEL] + 1);
}

// Returns exp to reach given level.
//
// 0x4AF9A8
int pcGetExperienceForLevel(int level)
{
    if (level >= PC_LEVEL_MAX) {
        return -1;
    }

    int v1 = level / 2;
    if ((level & 1) != 0) {
        return 1000 * v1 * level;
    } else {
        return 1000 * v1 * (level - 1);
    }
}

// 0x4AF9F4
char* pcStatGetName(int pcStat)
{
    return pcStat >= 0 && pcStat < PC_STAT_COUNT ? gPcStatDescriptions[pcStat].name : nullptr;
}

// 0x4AFA14
char* pcStatGetDescription(int pcStat)
{
    return pcStat >= 0 && pcStat < PC_STAT_COUNT ? gPcStatDescriptions[pcStat].description : nullptr;
}

// 0x4AFA34
int statGetFrmId(int stat)
{
    return statIsValid(stat) ? gStatDescriptions[stat].frmId : 0;
}

// Roll D10 against specified stat.
//
// This function is intended to be used with one of SPECIAL stats (which are
// capped at 10, hence d10), not with artitrary stat, but does not enforce it.
//
// An optional [modifier] can be supplied as a bonus (or penalty) to the stat's
// value.
//
// Upon return [howMuch] will be set to difference between stat's value
// (accounting for given [modifier]) and d10 roll, which can be positive (or
// zero) when roll succeeds, or negative when roll fails. Set [howMuch] to
// `NULL` if you're not interested in this value.
//
// 0x4AFA78
int statRoll(Object* critter, int stat, int modifier, int* howMuch)
{
    int value = critterGetStat(critter, stat) + modifier;
    int chance = randomBetween(PRIMARY_STAT_MIN, PRIMARY_STAT_MAX);

    if (howMuch != nullptr) {
        *howMuch = value - chance;
    }

    if (chance <= value) {
        return ROLL_SUCCESS;
    }

    return ROLL_FAILURE;
}

// 0x4AFAA8
int pcAddExperience(int xp, int* xpGained, Object* subject)
{
    return pcAddExperienceWithOptions(xp, true, xpGained, subject);
}

// 0x4AFAB8
int pcAddExperienceWithOptions(int xp, bool a2, int* xpGained, Object* subject)
{
    // Resolve ONCE, up front. Everything below that used to read gDude reads this
    // instead, and the row it indexes must be the same actor's — an earner whose
    // XP lands in one row while the level-up spends another's is risk #2, and it
    // is silent (§7).
    Object* earner = subject != nullptr ? subject : gDude;
    int* row = pcStatRow(earner);

    int oldXp = row[PC_STAT_EXPERIENCE];

    int newXp = row[PC_STAT_EXPERIENCE];
    newXp += xp;
    newXp += perkGetRank(earner, PERK_SWIFT_LEARNER) * 5 * xp / 100;

    if (newXp < gPcStatDescriptions[PC_STAT_EXPERIENCE].minimumValue) {
        newXp = gPcStatDescriptions[PC_STAT_EXPERIENCE].minimumValue;
    }

    if (newXp > gPcStatDescriptions[PC_STAT_EXPERIENCE].maximumValue) {
        newXp = gPcStatDescriptions[PC_STAT_EXPERIENCE].maximumValue;
    }

    row[PC_STAT_EXPERIENCE] = newXp;

    // The level-up award is the EARNER's: hit points land on the actor that did
    // the killing, not on whoever happens to be gDude.
    bool isHost = earner == gDude;

    while (row[PC_STAT_LEVEL] < PC_LEVEL_MAX) {
        if (newXp < pcGetExperienceForNextLevel(earner)) {
            break;
        }

        if (pcSetStat(PC_STAT_LEVEL, row[PC_STAT_LEVEL] + 1, earner) == 0) {
            int maxHpBefore = critterGetStat(earner, STAT_MAXIMUM_HIT_POINTS);

            // Presentation is the HOST's screen only, until per-client HUD
            // routing exists: a console line, a levelup sting and a HUD repaint
            // fired for an extra would appear on P1's display, attributed to the
            // wrong character. The award below is NOT gated — an extra levels
            // silently but really.
            if (isHost) {
                // You have gone up a level.
                MessageListItem messageListItem;
                messageListItem.num = 600;
                if (messageListGetItem(&gStatsMessageList, &messageListItem)) {
                    presenter()->consoleMessage(messageListItem.text);
                }

                dudeEnableState(DUDE_STATE_LEVEL_UP_AVAILABLE);

                presenter()->sfxPlay("levelup");
            }

            // NOTE: Uninline.
            int endurance = critterGetBaseStatWithTraitModifier(earner, STAT_ENDURANCE);

            int hpPerLevel = endurance / 2 + 2;
            hpPerLevel += perkGetRank(earner, PERK_LIFEGIVER) * 4;

            int bonusHp = critterGetBonusStat(earner, STAT_MAXIMUM_HIT_POINTS);
            critterSetBonusStat(earner, STAT_MAXIMUM_HIT_POINTS, bonusHp + hpPerLevel);

            int maxHpAfter = critterGetStat(earner, STAT_MAXIMUM_HIT_POINTS);
            critterAdjustHitPoints(earner, maxHpAfter - maxHpBefore);

            if (isHost) {
                presenter()->hudHitPoints(false);

                // SFALL: Update unarmed attack after leveling up.
                int leftItemAction;
                int rightItemAction;
                interfaceGetItemActions(&leftItemAction, &rightItemAction);
                presenter()->hudItems(false, leftItemAction, rightItemAction);
            }

            // Companions follow the HOST's level only. They are one shared party
            // (MP_PROPOSAL Ch 14), so letting each player's level-up advance them
            // would compound their growth by the player count.
            if (a2 && isHost) {
                _partyMemberIncLevels();
            }
        }
    }

    if (xpGained != nullptr) {
        *xpGained = newXp - oldXp;
    }

    return 0;
}

// 0x4AFC38
int pcSetExperience(int xp, Object* subject)
{
    Object* earner = subject != nullptr ? subject : gDude;
    int* row = pcStatRow(earner);

    int oldLevel = row[PC_STAT_LEVEL];
    row[PC_STAT_EXPERIENCE] = xp;

    int level = 1;
    do {
        level += 1;
    } while (xp >= pcGetExperienceForLevel(level) && level < PC_LEVEL_MAX);

    int newLevel = level - 1;

    pcSetStat(PC_STAT_LEVEL, newLevel, earner);

    // The DEMOTION path (XP set backwards). Mirror of the award in
    // pcAddExperienceWithOptions, and gated the same way: the HP is taken off the
    // earner whoever it is, the screen only reacts for the host.
    bool isHost = earner == gDude;
    if (isHost) {
        dudeDisableState(DUDE_STATE_LEVEL_UP_AVAILABLE);
    }

    // NOTE: Uninline.
    int endurance = critterGetBaseStatWithTraitModifier(earner, STAT_ENDURANCE);

    int hpPerLevel = endurance / 2 + 2;
    hpPerLevel += perkGetRank(earner, PERK_LIFEGIVER) * 4;

    int deltaHp = (oldLevel - newLevel) * hpPerLevel;
    critterAdjustHitPoints(earner, -deltaHp);

    int bonusHp = critterGetBonusStat(earner, STAT_MAXIMUM_HIT_POINTS);

    critterSetBonusStat(earner, STAT_MAXIMUM_HIT_POINTS, bonusHp - deltaHp);

    if (isHost) {
        presenter()->hudHitPoints(false);
    }

    return 0;
}

} // namespace fallout
