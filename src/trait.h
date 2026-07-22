#ifndef TRAIT_H
#define TRAIT_H

#include "db.h"
#include "obj_types.h"
#include "trait_defs.h"

namespace fallout {

int traitsInit();
void traitsReset();
void traitsExit();
int traitsLoad(File* stream);
int traitsSave(File* stream);
// The TRAIT API. `subject` is the player actor whose traits these are; nullptr
// means gDude, which is today's behavior and why unconverted callers keep
// working (PLAYER_SHEET_DESIGN.md §3).
void traitsSetSelected(int trait1, int trait2, Object* subject = nullptr);
void traitsGetSelected(int* trait1, int* trait2, Object* subject = nullptr);

// Copy the host's traits into every extra actor's row. Call with the other three
// seeders — see stat.h's pcPlayerActorSeedStats.
void traitsPlayerActorSeed();
// ONE slot, for spawn-at-login (see ACCOUNT_IDENTITY_DESIGN.md trap 1).
void traitsPlayerActorSeedSlot(int slot);

// One actor's selected traits (PLAYER_SHEET_DESIGN.md §5). Slot 0 is the host's
// row.
int traitsPlayerActorRowWrite(File* stream, int slot);
int traitsPlayerActorRowRead(File* stream, int slot);
void traitsMutateDrop(int* traits, int traitCount, int pickedLine, int firstListedTrait);
void traitsMutateGain(int* traits, int traitCount, int newTrait);
char* traitGetName(int trait);
char* traitGetDescription(int trait);
int traitGetFrmId(int trait);
bool traitIsSelected(int trait, Object* subject = nullptr);
int traitGetStatModifier(int stat, Object* subject = nullptr);
int traitGetSkillModifier(int skill, Object* subject = nullptr);

} // namespace fallout

#endif /* TRAIT_H */
