#include "trait.h"

#include <stdio.h>

#include "game.h"
#include "message.h"
#include "object.h"
#include "platform_compat.h"
#include "skill.h"
#include "server_players.h" // playerActorSlotOf — per-actor trait rows
#include "stat.h"

namespace fallout {

// Provides metadata about traits.
typedef struct TraitDescription {
    // The name of trait.
    char* name;

    // The description of trait.
    //
    // The description is only used in character editor to inform player about
    // effects of this trait.
    char* description;

    // Identifier of art in [intrface.lst].
    int frmId;
} TraitDescription;

// 0x66BE38
static MessageList gTraitsMessageList;

// List of selected traits.
//
// 0x66BE40
static int gSelectedTraits[TRAITS_MAX_SELECTED_COUNT];

// Traits for EXTRA player actors, slots 1..kMaxPlayerActors-1 (index = slot - 1).
// Slot 0 is gSelectedTraits above — the same aliasing as the sheet's proto row
// and the PC-stat row, for the same degeneracy reason (PLAYER_SHEET_DESIGN.md
// §2): with an empty registry no subject resolves here.
static int gPlayerActorTraits[kMaxPlayerActors - 1][TRAITS_MAX_SELECTED_COUNT];

// The trait row for `subject`. THE resolver — nothing else may index either
// array. nullptr means "no subject in hand" and resolves to gDude, which is
// today's behavior verbatim and why the ~25 unconverted traitIsSelected callers
// keep working.
static int* traitRow(Object* subject)
{
    int slot = playerActorSlotOf(subject != nullptr ? subject : gDude);
    return slot > 0 ? gPlayerActorTraits[slot - 1] : gSelectedTraits;
}

// 0x51DB84
static TraitDescription gTraitDescriptions[TRAIT_COUNT] = {
    { nullptr, nullptr, 55 },
    { nullptr, nullptr, 56 },
    { nullptr, nullptr, 57 },
    { nullptr, nullptr, 58 },
    { nullptr, nullptr, 59 },
    { nullptr, nullptr, 60 },
    { nullptr, nullptr, 61 },
    { nullptr, nullptr, 62 },
    { nullptr, nullptr, 63 },
    { nullptr, nullptr, 64 },
    { nullptr, nullptr, 65 },
    { nullptr, nullptr, 66 },
    { nullptr, nullptr, 67 },
    { nullptr, nullptr, 94 },
    { nullptr, nullptr, 69 },
    { nullptr, nullptr, 70 },
};

// 0x4B39F0
int traitsInit()
{
    if (!messageListInit(&gTraitsMessageList)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "trait.msg");

    if (!messageListLoad(&gTraitsMessageList, path)) {
        return -1;
    }

    for (int trait = 0; trait < TRAIT_COUNT; trait++) {
        MessageListItem messageListItem;

        messageListItem.num = 100 + trait;
        if (messageListGetItem(&gTraitsMessageList, &messageListItem)) {
            gTraitDescriptions[trait].name = messageListItem.text;
        }

        messageListItem.num = 200 + trait;
        if (messageListGetItem(&gTraitsMessageList, &messageListItem)) {
            gTraitDescriptions[trait].description = messageListItem.text;
        }
    }

    // NOTE: Uninline.
    traitsReset();

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_TRAIT, &gTraitsMessageList);

    return true;
}

// 0x4B3ADC
void traitsReset()
{
    for (int index = 0; index < TRAITS_MAX_SELECTED_COUNT; index++) {
        gSelectedTraits[index] = -1;
    }

    // Extras reset with the host — file statics, not malloc'd per run, so a new
    // game would otherwise inherit the previous one's traits (same trap as
    // perkResetRanks and pcStatsReset).
    for (int slot = 0; slot < kMaxPlayerActors - 1; slot++) {
        for (int index = 0; index < TRAITS_MAX_SELECTED_COUNT; index++) {
            gPlayerActorTraits[slot][index] = -1;
        }
    }
}

// 0x4B3AF8
void traitsExit()
{
    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_TRAIT, nullptr);
    messageListFree(&gTraitsMessageList);
}

// Loads trait system state from save game.
//
// 0x4B3B08
int traitsLoad(File* stream)
{
    return fileReadInt32List(stream, gSelectedTraits, TRAITS_MAX_SELECTED_COUNT);
}

// Saves trait system state to save game.
//
// 0x4B3B28
int traitsSave(File* stream)
{
    return fileWriteInt32List(stream, gSelectedTraits, TRAITS_MAX_SELECTED_COUNT);
}

// Sets selected traits.
//
// 0x4B3B48
void traitsSetSelected(int trait1, int trait2, Object* subject)
{
    int* row = traitRow(subject);
    row[0] = trait1;
    row[1] = trait2;
}

// Copy the host's traits into every extra player actor's row — the fourth member
// of the seeding set (protoPlayerActorSheetsSeed, perkPlayerActorSeedRanks,
// pcPlayerActorSeedStats). Traits are chosen at character creation and co-op v1
// is one authored character, so an extra must carry the host's.
void traitsPlayerActorSeed()
{
    for (int slot = 1; slot < kMaxPlayerActors; slot++) {
        traitsPlayerActorSeedSlot(slot);
    }
}

// ONE slot, for the dynamic spawn-at-login path (ACCOUNT_IDENTITY_DESIGN.md §3).
// ⚠ Never call the bulk seeder above with players live (trap 1).
// ⚠ Takes an ACTOR SLOT (1..kMaxPlayerActors-1); the array is indexed slot-1.
void traitsPlayerActorSeedSlot(int slot)
{
    if (slot < 1 || slot >= kMaxPlayerActors) {
        return;
    }

    for (int index = 0; index < TRAITS_MAX_SELECTED_COUNT; index++) {
        gPlayerActorTraits[slot - 1][index] = gSelectedTraits[index];
    }
}

// One actor's selected traits (PLAYER_SHEET_DESIGN.md §5). Slot 0 is
// gSelectedTraits — the bare global traitsSave already writes.
static int* traitsPlayerActorRow(int slot)
{
    if (slot == 0) {
        return gSelectedTraits;
    }

    if (slot < 1 || slot >= kMaxPlayerActors) {
        return nullptr;
    }

    return gPlayerActorTraits[slot - 1];
}

int traitsPlayerActorRowWrite(File* stream, int slot)
{
    int* row = traitsPlayerActorRow(slot);
    if (row == nullptr) {
        return -1;
    }

    return fileWriteInt32List(stream, row, TRAITS_MAX_SELECTED_COUNT);
}

int traitsPlayerActorRowRead(File* stream, int slot)
{
    int* row = traitsPlayerActorRow(slot);
    if (row == nullptr) {
        return -1;
    }

    return fileReadInt32List(stream, row, TRAITS_MAX_SELECTED_COUNT);
}

// Returns selected traits.
//
// 0x4B3B54
void traitsGetSelected(int* trait1, int* trait2, Object* subject)
{
    int* row = traitRow(subject);
    *trait1 = row[0];
    *trait2 = row[1];
}

// Ledger H-47 (extracted from the character editor's Mutate! perk dialog):
// drop the trait the player chose to lose. `traits` is the caller's working
// pair (-1 = empty slot), `traitCount` the caller's session trait count
// (2 minus leading empty slots, as the dialog computes it), `pickedLine`
// the picked line in the (alphabetically sorted) display list and
// `firstListedTrait` the trait shown on its first line — the last two
// together identify which slot the player picked.
void traitsMutateDrop(int* traits, int traitCount, int pickedLine, int firstListedTrait)
{
    if (pickedLine == 0) {
        if (traitCount == 1) {
            traits[0] = -1;
            traits[1] = -1;
        } else {
            if (firstListedTrait == traits[0]) {
                traits[0] = traits[1];
                traits[1] = -1;
            } else {
                traits[1] = -1;
            }
        }
    } else {
        if (firstListedTrait == traits[0]) {
            traits[1] = -1;
        } else {
            traits[0] = traits[1];
            traits[1] = -1;
        }
    }
}

// Ledger H-47 (extracted from the character editor's Mutate! perk dialog):
// gain the newly picked trait and commit the swap. `traitCount` is the
// session count from before the drop (the dialog does not recompute it).
void traitsMutateGain(int* traits, int traitCount, int newTrait)
{
    if (traitCount != 0) {
        traits[1] = newTrait;
    } else {
        traits[0] = newTrait;
        traits[1] = -1;
    }

    traitsSetSelected(traits[0], traits[1]);
}

// Returns a name of the specified trait, or `NULL` if the specified trait is
// out of range.
//
// 0x4B3B68
char* traitGetName(int trait)
{
    return trait >= 0 && trait < TRAIT_COUNT ? gTraitDescriptions[trait].name : nullptr;
}

// Returns a description of the specified trait, or `NULL` if the specified
// trait is out of range.
//
// 0x4B3B88
char* traitGetDescription(int trait)
{
    return trait >= 0 && trait < TRAIT_COUNT ? gTraitDescriptions[trait].description : nullptr;
}

// Return an art ID of the specified trait, or `0` if the specified trait is
// out of range.
//
// 0x4B3BA8
int traitGetFrmId(int trait)
{
    return trait >= 0 && trait < TRAIT_COUNT ? gTraitDescriptions[trait].frmId : 0;
}

// Returns `true` if the specified trait is selected.
//
// 0x4B3BC8
bool traitIsSelected(int trait, Object* subject)
{
    int* row = traitRow(subject);
    return row[0] == trait || row[1] == trait;
}

// Subject-first spelling of traitIsSelected, for the two modifier switches
// below: they test ~21 traits between them, and reading the actor before the
// trait keeps "whose trait" impossible to lose in the noise.
static bool traitHas(Object* subject, int trait)
{
    return traitIsSelected(trait, subject);
}

// Returns stat modifier depending on selected traits.
//
// 0x4B3C7C
int traitGetStatModifier(int stat, Object* subject)
{
    int modifier = 0;

    switch (stat) {
    case STAT_STRENGTH:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        if (traitHas(subject, TRAIT_BRUISER)) {
            modifier += 2;
        }
        break;
    case STAT_PERCEPTION:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        break;
    case STAT_ENDURANCE:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        break;
    case STAT_CHARISMA:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        break;
    case STAT_INTELLIGENCE:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        break;
    case STAT_AGILITY:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        if (traitHas(subject, TRAIT_SMALL_FRAME)) {
            modifier += 1;
        }
        break;
    case STAT_LUCK:
        if (traitHas(subject, TRAIT_GIFTED)) {
            modifier += 1;
        }
        break;
    case STAT_MAXIMUM_ACTION_POINTS:
        if (traitHas(subject, TRAIT_BRUISER)) {
            modifier -= 2;
        }
        break;
    case STAT_ARMOR_CLASS:
        if (traitHas(subject, TRAIT_KAMIKAZE)) {
            modifier -= critterGetBaseStat(gDude, STAT_ARMOR_CLASS);
        }
        break;
    case STAT_MELEE_DAMAGE:
        if (traitHas(subject, TRAIT_HEAVY_HANDED)) {
            modifier += 4;
        }
        break;
    case STAT_CARRY_WEIGHT:
        if (traitHas(subject, TRAIT_SMALL_FRAME)) {
            modifier -= 10 * critterGetBaseStat(gDude, STAT_STRENGTH);
        }
        break;
    case STAT_SEQUENCE:
        if (traitHas(subject, TRAIT_KAMIKAZE)) {
            modifier += 5;
        }
        break;
    case STAT_HEALING_RATE:
        if (traitHas(subject, TRAIT_FAST_METABOLISM)) {
            modifier += 2;
        }
        break;
    case STAT_CRITICAL_CHANCE:
        if (traitHas(subject, TRAIT_FINESSE)) {
            modifier += 10;
        }
        break;
    case STAT_BETTER_CRITICALS:
        if (traitHas(subject, TRAIT_HEAVY_HANDED)) {
            modifier -= 30;
        }
        break;
    case STAT_RADIATION_RESISTANCE:
        if (traitHas(subject, TRAIT_FAST_METABOLISM)) {
            modifier -= critterGetBaseStat(gDude, STAT_RADIATION_RESISTANCE);
        }
        break;
    case STAT_POISON_RESISTANCE:
        if (traitHas(subject, TRAIT_FAST_METABOLISM)) {
            modifier -= critterGetBaseStat(gDude, STAT_POISON_RESISTANCE);
        }
        break;
    }

    return modifier;
}

// Returns skill modifier depending on selected traits.
//
// 0x4B40FC
int traitGetSkillModifier(int skill, Object* subject)
{
    int modifier = 0;

    if (traitHas(subject, TRAIT_GIFTED)) {
        modifier -= 10;
    }

    if (traitHas(subject, TRAIT_GOOD_NATURED)) {
        switch (skill) {
        case SKILL_SMALL_GUNS:
        case SKILL_BIG_GUNS:
        case SKILL_ENERGY_WEAPONS:
        case SKILL_UNARMED:
        case SKILL_MELEE_WEAPONS:
        case SKILL_THROWING:
            modifier -= 10;
            break;
        case SKILL_FIRST_AID:
        case SKILL_DOCTOR:
        case SKILL_SPEECH:
        case SKILL_BARTER:
            modifier += 15;
            break;
        }
    }

    return modifier;
}

} // namespace fallout
