#ifndef CRITTER_H
#define CRITTER_H

#include "db.h"
#include "obj_types.h"
#include "proto_types.h"

namespace fallout {

typedef enum DudeState {
    DUDE_STATE_SNEAKING = 0,
    DUDE_STATE_LEVEL_UP_AVAILABLE = 3,
    DUDE_STATE_ADDICTED = 4,
} DudeState;

int critterInit();
void critterReset();
void critterExit();
int critterLoad(File* stream);
int critterSave(File* stream);
char* critterGetName(Object* obj);
void critterProtoDataCopy(CritterProtoData* dest, CritterProtoData* src);
int dudeSetName(const char* name);
void dudeResetName();

// Per-actor names (PLAYER_SHEET_DESIGN.md §8). Slot 0 is gDudeName, i.e.
// dudeSetName / dudeResetName above.
//
// The GETTER is the storage accessor, not a convenience wrapper: protoGetName
// must answer a sheet pid without calling critterGetName, or the two recurse.
char* critterGetNameForSlot(int slot);
int critterSetNameForSlot(int slot, const char* name);
void critterPlayerActorSeedNames();
// ONE slot, for spawn-at-login (see ACCOUNT_IDENTITY_DESIGN.md trap 1).
void critterPlayerActorSeedNameSlot(int slot);
int critterPlayerActorNameRowWrite(File* stream, int slot);
int critterPlayerActorNameRowRead(File* stream, int slot);
int critterGetHitPoints(Object* critter);
int critterAdjustHitPoints(Object* critter, int hp);
int critterGetPoison(Object* critter);
int critterAdjustPoison(Object* obj, int amount);
int poisonEventProcess(Object* obj, void* data);
int critterGetRadiation(Object* critter);
int critterAdjustRadiation(Object* obj, int amount);
int _critter_check_rads(Object* critter);
int _clear_rad_damage(Object* obj, void* data);
void _process_rads(Object* obj, int radiationLevel, bool direction);
int radiationEventProcess(Object* obj, void* data);
int radiationEventRead(File* stream, void** dataPtr);
int radiationEventWrite(File* stream, void* data);
int critterGetDamageType(Object* critter);
int killsIncByType(int killType);
int killsGetByType(int killType);
int killsLoad(File* stream);
int killsSave(File* stream);
int critterGetKillType(Object* critter);
char* killTypeGetName(int killType);
char* killTypeGetDescription(int killType);
int _critter_heal_hours(Object* obj, int a2);
void critterKill(Object* critter, int anim, bool a3);
// Minimal inverse of critterKill: revive a dead critter at 1 HP, standing and
// blocking again. No-op (returns false) if the critter is not actually dead.
bool critterRevive(Object* critter);
int critterGetExp(Object* critter);
bool critterIsActive(Object* critter);
bool critterIsDead(Object* critter);
bool critterIsCrippled(Object* critter);
bool _critter_is_prone(Object* critter);
int critterGetBodyType(Object* critter);
int gcdLoad(const char* path);
int protoCritterDataRead(File* stream, CritterProtoData* critterData);
int gcdSave(const char* path);
int protoCritterDataWrite(File* stream, CritterProtoData* critterData);
// ►► THE THREE DUDE_STATE_* FLAGS (sneaking, level-up available, addicted) LIVE IN
// THE PLAYER'S PROTO ROW, and every player actor has its own row — so these take a
// SUBJECT. They used to read gDude->pid unconditionally, which meant an extra
// player's sneak toggle, level-up badge and addiction marker were all written into
// (and read out of) SLOT 0's sheet: the host's indicator bar lit up for someone
// else's chem habit, an extra could never see its own level-up badge, and two
// players sneaking shared one flag.
//
// `subject == nullptr` means gDude, so every vanilla call site keeps its exact
// behaviour and single-player is byte-identical. The row is marked sheet-dirty on
// change, which is what streams the new flags to that actor's own client.
void dudeDisableState(int state, Object* subject = nullptr);
void dudeEnableState(int state, Object* subject = nullptr);
void dudeToggleState(int state, Object* subject = nullptr);
bool dudeHasState(int state, Object* subject = nullptr);
int sneakEventProcess(Object* obj, void* data);
int _critter_sneak_clear(Object* obj, void* data);
// True iff `subject` (default gDude) is sneaking AND its last sneak roll held.
bool dudeIsSneaking(Object* subject = nullptr);
int knockoutEventProcess(Object* obj, void* data);
int _critter_wake_clear(Object* obj, void* data);
int _critter_set_who_hit_me(Object* a1, Object* a2);
bool _critter_can_obj_dude_rest();
int critterGetMovementPointCostAdjustedForCrippledLegs(Object* critter, int a2);
bool critterIsEncumbered(Object* critter);
bool critterIsFleeing(Object* a1);
bool _critter_flag_check(int pid, int flag);
void critter_flag_set(int pid, int flag);
void critter_flag_unset(int pid, int flag);

} // namespace fallout

#endif /* CRITTER_H */
