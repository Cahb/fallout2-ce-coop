#ifndef PERK_H
#define PERK_H

#include "db.h"
#include "obj_types.h"
#include "perk_defs.h"

namespace fallout {

// Ledger H-44: follow-up choice a just-committed perk demands from the
// caller (see perkChoiceApply).
typedef enum PerkChoicePending {
    PERK_CHOICE_PENDING_NONE = 0,
    PERK_CHOICE_PENDING_TAG,
    PERK_CHOICE_PENDING_MUTATE,
} PerkChoicePending;

int perksInit();
void perksReset();
void perksExit();
int perksLoad(File* stream);
int perksSave(File* stream);
int perkAdd(Object* critter, int perk);
int perkAddForce(Object* critter, int perk);
int perkRemove(Object* critter, int perk);
int perkChoiceApply(Object* critter, int perk, const int* perksBackup, int* pendingChoicePtr);
int perkGetAvailablePerks(Object* critter, int* perks);
int perkGetRank(Object* critter, int perk);
char* perkGetName(int perk);
char* perkGetDescription(int perk);
int perkGetFrmId(int perk);
void perkAddEffect(Object* critter, int perk);
void perkRemoveEffect(Object* critter, int perk);
int perkGetSkillModifier(Object* critter, int skill);

// Copy the host's perk ranks into every extra player actor's row. Pairs with
// protoPlayerActorSheetsSeed — call both, or an extra gets the host's skills and
// nobody's perks (PLAYER_SHEET_DESIGN.md stage 2).
void perkPlayerActorSeedRanks();
// ONE slot, for spawn-at-login (see ACCOUNT_IDENTITY_DESIGN.md trap 1).
void perkPlayerActorSeedRanksSlot(int slot);
// Zero ONE slot's perks — a CREATED character inherits none of the host's.
void perkPlayerActorClearRanksSlot(int slot);

// One actor's perk ranks (PLAYER_SHEET_DESIGN.md §5). Slot 0 is the host's row.
// Companion rows are perksSave's business and never travel here.
int perkPlayerActorRowWrite(File* stream, int slot);
int perkPlayerActorRowRead(File* stream, int slot);

// Returns true if perk is valid.
static inline bool perkIsValid(int perk)
{
    return perk >= 0 && perk < PERK_COUNT;
}

// Returns true if critter has at least one rank in specified perk.
//
// NOTE: Most perks have only 1 rank, which means dude either have perk, or
// not.
//
// On the other hand, there are several places in editor, where they made two
// consequtive calls to [perkGetRank], first to check for presence, then get
// the actual value for displaying. So a macro could exist, or this very
// function, but due to similarity to [perkGetRank] it could have been
// collapsed by compiler.
static inline bool perkHasRank(Object* critter, int perk)
{
    return perkGetRank(critter, perk) != 0;
}

} // namespace fallout

#endif /* PERK_H */
