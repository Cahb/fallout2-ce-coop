#ifndef SKILL_H
#define SKILL_H

#include "db.h"
#include "obj_types.h"
#include "proto_types.h"
#include "skill_defs.h"

namespace fallout {

extern int _gIsSteal;
extern int _gStealCount;
extern int _gStealSize;

int skillsInit();
void skillsReset();
void skillsExit();
int skillsLoad(File* stream);
int skillsSave(File* stream);
void protoCritterDataResetSkills(CritterProtoData* data);
// The tagged-skill row belongs to ONE actor (PLAYER_SHEET_DESIGN.md §8). A null
// subject means "no subject in hand" and resolves to gDude — today's behavior
// verbatim, which is why the character-editor and debug callers pass nothing.
void skillsSetTagged(int* skills, int count, Object* subject = nullptr);
void skillsGetTagged(int* skills, int count, Object* subject = nullptr);

// Copy the host's tagged skills into every extra actor's row. Part of the
// stage-2 seeding set — an actor missing it is ~20 points short on every skill
// the host tagged.
void skillsPlayerActorSeed();
// ONE slot, for spawn-at-login (see ACCOUNT_IDENTITY_DESIGN.md trap 1).
void skillsPlayerActorSeedSlot(int slot);
int skillsPlayerActorTaggedRowWrite(File* stream, int slot);
int skillsPlayerActorTaggedRowRead(File* stream, int slot);
void skillsTagPerkApply(int* taggedSkills, int skill);
bool skillIsTagged(int skill, Object* subject = nullptr);
int skillGetValue(Object* critter, int skill);
int skillGetDefaultValue(int skill);
int skillGetBaseValue(Object* critter, int skill);
int skillAdd(Object* critter, int skill);
int skillAddForce(Object* critter, int skill);
int skillsGetCost(int a1);
int skillSub(Object* critter, int skill);
int skillSubForce(Object* critter, int skill);
int skillRoll(Object* critter, int skill, int modifier, int* howMuch);
char* skillGetName(int skill);
char* skillGetDescription(int skill);
char* skillGetAttributes(int skill);
int skillGetFrmId(int skill);
int skillUse(Object* obj, Object* target, int skill, int criticalChanceModifier);
int skillsPerformStealing(Object* a1, Object* a2, Object* item, bool isPlanting);
int skillGetGameDifficultyModifier(int skill);
int skillUpdateLastUse(int skill);
// Number of non-expired usage slots recorded for `skill` today (the anti-spam
// cooldown state). Exposed for state-dump visibility.
int skillGetUsesToday(int skill);
int skillsUsageSave(File* stream);
int skillsUsageLoad(File* stream);
char* skillsGetGenericResponse(Object* critter, bool isDude);

// Returns true if skill is valid.
static inline bool skillIsValid(int skill)
{
    return skill >= 0 && skill < SKILL_COUNT;
}

} // namespace fallout

#endif /* SKILL_H */
