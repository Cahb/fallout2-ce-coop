#ifndef COMBAT_H
#define COMBAT_H

#include "combat_defs.h"
#include "db.h"
#include "obj_types.h"
#include "proto_types.h"

namespace fallout {

extern int _combatNumTurns;
extern unsigned int gCombatState;

extern int _combat_free_move;

int combatInit();
void combatReset();
void combatExit();
int _find_cid(int a1, int a2, Object** a3, int a4);
int combatLoad(File* stream);
int combatSave(File* stream);
bool _combat_safety_invalidate_weapon(Object* attacker, Object* weapon, int hitMode, Object* defender, int* safeDistancePtr);
bool _combatTestIncidentalHit(Object* attacker, Object* defender, Object* attackerFriend, Object* weapon);
Object* _combat_whose_turn();
void _combat_data_init(Object* obj);
Object* aiInfoGetFriendlyDead(Object* obj);
int aiInfoSetFriendlyDead(Object* a1, Object* a2);
Object* aiInfoGetLastTarget(Object* obj);
int aiInfoSetLastTarget(Object* a1, Object* a2);
Object* aiInfoGetLastItem(Object* obj);
int aiInfoSetLastItem(Object* obj, Object* a2);
void _combat_update_critter_outline_for_los(Object* critter, bool a2);
void _combat_over_from_load();
// `earner` is the player actor the XP belongs to; nullptr means gDude (vanilla
// behavior). Pass one wherever the caller knows who made the kill
// (PLAYER_SHEET_DESIGN.md §4).
void _combat_give_exps(int exp_points, Object* earner = nullptr);
bool combatPlayerTurnShouldBreak();
bool combatPlayerTurnOutOfAp();
int combatPlayerTurnResolve();
void _combat_turn_run();

// Why a single-beat intent drain stopped (P2 resumable combat). kEndTurn = a
// turn-ending authority tripped (combatPlayerTurnShouldBreak/OutOfAp, an
// explicit END_TURN, or an unexecutable attack); kQueueDrained = ran out of
// queued intents with no turn-ending reason (the legacy auto-end-turn case, and
// the point where the resumable session WAITS across beats instead).
enum class CombatPumpStop {
    kEndTurn,
    kQueueDrained,
};

struct CombatPumpOutcome {
    CombatPumpStop stop;
    int consumed; // intents successfully consumed this call (idle-timer reset)
};

// Drain the currently-queued dude combat intents for THIS beat, calling the same
// core entry points the AI uses (combat_intent.h). The extracted body of the
// server branch of _combat_input — shared so the legacy path stays byte-identical
// and the resumable session (combat.cc) reuses it verbatim (do NOT duplicate the
// serverResolveTarget/serverDudeHitMode logic). Legacy _combat_input ignores the
// outcome (empty queue ends the turn); the session inspects it to decide whether
// to wait for more intents across beats.
//
// `actorSlot` names WHOSE turn is being pumped: only that player's intents are
// consumed (MP_PROPOSAL Ch 8.4). The caller must already hold a ServerActorScope
// for that actor — the body reads gDude deep inside the core attack/move entry
// points, and the scope is what makes those reads mean "the acting player".
// Slot 0 (the host) is the default and the only value the legacy path uses.
CombatPumpOutcome combatServerPumpIntents(int actorSlot = 0);

// Resumable server-combat session (P2; F2_SERVER_RESUMABLE_COMBAT, default OFF).
// When enabled, _combat() sets up a session and returns instead of draining the
// fight inside one scriptsHandleRequests call; serverTick advances it one beat at
// a time. combatSessionActive() is false on every gate-off/client/golden path.
bool combatSessionActive();

// Rearm the resumable-combat player-turn idle timer. For player activity that
// does NOT arrive as a combat intent and so never reaches the pump that normally
// rearms it — today: OPENING the in-combat inventory screen, so the player gets
// a full budget to browse in rather than inheriting whatever was left of it.
//
// Called once at open, NOT per action inside the screen: those actions are free,
// so rearming on each would let a player hold the whole fight open indefinitely.
// The budget from the open is the hard bound; when it expires the turn ends and
// the screen is revoked (presenter inventoryRevoke).
void combatSessionRearmIdleTimer();
void combatSessionAdvance();
void _combat(CombatStartData* csd);

// Install the wire-viewer attack-commit hook (COMBAT_CLIENT_DESIGN.md §3.b). The
// SDL viewer registers a function that forwards a fully-selected attack upstream as
// a `cattack` verb; _combat_attack_this calls it at the commit point instead of
// _combat_attack when clientViewerActive(). Null (and thus inert) everywhere else.
void combatSetViewerAttackHook(void (*hook)(Object* target, int hitMode, int hitLocation));
void attackInit(Attack* attack, Object* attacker, Object* defender, int hitMode, int hitLocation);

// True iff an in-combat MOVE bracket should record its walk over the presentation record
// channel (COMBAT_MOVE_RECORD_DESIGN.md). Used by combat_ai/combat_drain to wrap the
// reg_anim move brackets in an ambient record section. False off the record backend / SP /
// out of combat → today's EVENT_MOVE glide path.
bool combatMoveRecorded();

// Close a MOVE record bracket: end the ambient section, ship the actor's presSeq, commit the
// deferred authoritative walk. No-op when !recording. See combat.cc.
void combatMoveRecordClose(bool recording, Object* actor);

int _combat_attack(Object* attacker, Object* defender, int hitMode, int hitLocation);
int _combat_bullet_start(const Object* attacker, const Object* target);
void _compute_explosion_on_extras(Attack* attack, bool isFromAttacker, bool isGrenade, bool noDamage);
int _determine_to_hit(Object* a1, Object* a2, int hitLocation, int hitMode);
int _determine_to_hit_no_range(Object* attacker, Object* defender, int hitLocation, int hitMode, unsigned char* a5);
int _determine_to_hit_from_tile(Object* attacker, int tile, Object* defender, int hitLocation, int hitMode);
void attackComputeDeathFlags(Attack* attack);
void _apply_damage(Attack* attack, bool animated);
void _combat_display(Attack* attack);
void _combat_anim_begin();
void _combat_anim_finished();
int _combat_check_bad_shot(Object* attacker, Object* defender, int hitMode, bool aiming);
bool _combat_to_hit(Object* target, int* accuracy);
void _combat_attack_this(Object* target);
void _combat_outline_on();
void _combat_outline_off();
void _combat_highlight_change();
bool _combat_is_shot_blocked(Object* sourceObj, int from, int to, Object* targetObj, int* numCrittersOnLof);
int _combat_player_knocked_out_by();
int _combat_explode_scenery(Object* a1, Object* a2);
void _combat_delete_critter(Object* obj);
void _combatKillCritterOutsideCombat(Object* critter_obj, char* msg);

int combatGetTargetHighlight();
int criticalsGetValue(int killType, int hitLocation, int effect, int dataMember);
void criticalsSetValue(int killType, int hitLocation, int effect, int dataMember, int value);
void criticalsResetValue(int killType, int hitLocation, int effect, int dataMember);
int unarmedGetDamage(int hitMode, int* minDamagePtr, int* maxDamagePtr);
int unarmedGetBonusCriticalChance(int hitMode);
int unarmedGetActionPointCost(int hitMode);
bool unarmedIsPenetrating(int hitMode);
int unarmedGetPunchHitMode(bool isSecondary);
int unarmedGetKickHitMode(bool isSecondary);
bool unarmedIsPenetrating(int hitMode);
bool damageModGetBonusHthDamageFix();
bool damageModGetDisplayBonusDamage();
int combat_get_hit_location_penalty(int hit_location);
void combat_set_hit_location_penalty(int hit_location, int penalty);
void combat_reset_hit_location_penalty();
Attack* combat_get_data();

static inline bool isInCombat()
{
    return (gCombatState & COMBAT_STATE_0x01) != 0;
}

static inline bool isUnarmedHitMode(int hitMode)
{
    return hitMode == HIT_MODE_PUNCH
        || hitMode == HIT_MODE_KICK
        || (hitMode >= FIRST_ADVANCED_UNARMED_HIT_MODE && hitMode <= LAST_ADVANCED_UNARMED_HIT_MODE);
}

} // namespace fallout

#endif /* COMBAT_H */
