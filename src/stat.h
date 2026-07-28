#ifndef STAT_H
#define STAT_H

#include "db.h"
#include "obj_types.h"
#include "proto_types.h"
#include "stat_defs.h"

namespace fallout {

#define STAT_ERR_INVALID_STAT (-5)

int statsInit();
int statsReset();
int statsExit();
int statsLoad(File* stream);
int statsSave(File* stream);
int critterGetStat(Object* critter, int stat);
int critterGetBaseStatWithTraitModifier(Object* critter, int stat);
int critterGetBaseStat(Object* critter, int stat);
int critterGetBonusStat(Object* critter, int stat);
int critterSetBaseStat(Object* critter, int stat, int value);
int critterIncBaseStat(Object* critter, int stat);
int critterDecBaseStat(Object* critter, int stat);
int critterSetBonusStat(Object* critter, int stat, int value);
void protoCritterDataResetStats(CritterProtoData* data);
void critterUpdateDerivedStats(Object* critter);
char* statGetName(int stat);
char* statGetDescription(int stat);
char* statGetValueDescription(int value);
// The PC-STAT API (XP, level, karma, unspent skill points). `subject` is the
// player actor whose sheet this reads or writes; nullptr means gDude, which is
// today's behavior and why the ~45 existing call sites need no change. Pass one
// wherever the caller genuinely knows WHOSE experience it is
// (PLAYER_SHEET_DESIGN.md §4 — the subject comes from the call site, never from
// geometry).
int pcGetStat(int pcStat, Object* subject = nullptr);
int pcSetStat(int pcStat, int value, Object* subject = nullptr);
// The level-up award for ONE actor: skill points for the levels in
// (fromLevel, toLevel], and true when the range crossed the free-perk cadence.
// `subject` follows the usual convention — nullptr means gDude.
//
// Called by the XP funnel for whoever earned the levels; the character screen no
// longer awards (it only reconciles a level raised by some other path).
bool pcLevelUpApply(int fromLevel, int toLevel, Object* subject = nullptr);
void pcStatsReset();

// RE-DERIVE the level-up badge (DUDE_STATE_LEVEL_UP_AVAILABLE, the flashing
// character-screen button) for one actor from what it actually MEANS: unspent skill
// points, or a free perk pick still owed.
//
// It is derived and not event-set because vanilla's only clear path is "the player
// closed the character screen" (character_editor.cc) — and a dedicated server has no
// character screen, so a badge switched on at level-up would light that player's
// indicator bar forever. Call it after anything that spends or grants an entitlement.
// [[no-re-derivation-path-bug-class]]
void pcLevelUpBadgeRefresh(Object* subject = nullptr);

// Copy the host's XP / level / karma / unspent skill points into every extra
// player actor's row. Call WITH protoPlayerActorSheetsSeed and
// perkPlayerActorSeedRanks — the three are one operation.
void pcPlayerActorSeedStats();
// ONE slot, for spawn-at-login (see ACCOUNT_IDENTITY_DESIGN.md trap 1).
void pcPlayerActorSeedStatsSlot(int slot);

// One actor's XP / level / karma / unspent skill points
// (PLAYER_SHEET_DESIGN.md §5). Slot 0 is the host's row.
int pcPlayerActorRowWrite(File* stream, int slot);
int pcPlayerActorRowRead(File* stream, int slot);
int pcGetExperienceForNextLevel(Object* subject = nullptr);
int pcGetExperienceForLevel(int level);
char* pcStatGetName(int pcStat);
char* pcStatGetDescription(int pcStat);
int statGetFrmId(int stat);
int statRoll(Object* critter, int stat, int modifier, int* howMuch);
int pcAddExperience(int xp, int* xpGained = nullptr, Object* subject = nullptr);
int pcAddExperienceWithOptions(int xp, bool a2, int* xpGained = nullptr, Object* subject = nullptr);
int pcSetExperience(int a1, Object* subject = nullptr);

static inline bool statIsValid(int stat)
{
    return stat >= 0 && stat < STAT_COUNT;
}

static inline bool pcStatIsValid(int pcStat)
{
    return pcStat >= 0 && pcStat < PC_STAT_COUNT;
}

} // namespace fallout

#endif /* STAT_H */
