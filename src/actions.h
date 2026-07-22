#ifndef ACTIONS_H
#define ACTIONS_H

#include "combat_defs.h"
#include "obj_types.h"

namespace fallout {

extern int rotation;

int _action_attack(Attack* attack);
int _action_use_an_item_on_object(Object* user, Object* targetObj, Object* item);
int actionUseItemOnObjectWithApCost(Object* user, Object* targetObj, Object* item);
void actionResolveDroppedExplosive(Object* dropper);
int _action_use_an_object(Object* user, Object* targetObj);
int actionPickUp(Object* critter, Object* item);
int _action_loot_container(Object* critter, Object* container);
int _action_skill_use(int skill);
int actionUseSkill(Object* user, Object* target, int skill);
bool _is_hit_from_front(Object* attacker, Object* defender);
bool _can_see(Object* a1, Object* a2);
bool _action_explode_running();
int actionExplode(int tile, int elevation, int minDamage, int maxDamage, Object* sourceObj, bool animate);
void actionExplodeReplay(int tile, int elevation, Attack* attack);
// Presentation record/replay seam for the _show_death callback (POC,
// PRESENTATION_RECORD_REPLAY_SPEC.md). ...CallbackPtr = the pointer the server
// recorder matches to defunctionalize the registration; ...ReplayShowDeath = the
// viewer registering the real _show_death from a decoded CALL{SHOW_DEATH} op.
// The STATE half of _show_death (drop inventory to the floor for the two
// ANNIHILATION deaths, anim 30/31). Exported because the seam splits it from the
// presentation half: the viewer replays _show_death but must NOT run this, and
// the dedicated server never runs _show_death at all, so it calls this directly.
void actionDeathDropItems(Object* obj, int anim);

void* actionShowDeathCallbackPtr();

// See server_anim.cc's state-bearing-callback allowlist.
void* actionTalkToCallbackPtr();
void actionPresReplayShowDeath(Object* obj, int anim);
// Recorder-match pointer for the ranged projectile-hide callback, folded into
// PRES_OP_HIDE_FORCED (no dedicated tag) — see actions.cc.
void* actionHideProjectileCallbackPtr();
// Headless mirror of _action_ranged's inline thrown-weapon consumption, applied
// authoritatively from the server's _combat_apply_attack_results — see actions.cc.
void actionThrowConsumeHeadless(Attack* attack);
int actionTalk(Object* a1, Object* a2);
void actionDamage(int tile, int elevation, int minDamage, int maxDamage, int damageType, bool animated, bool bypassArmor);
bool actionCheckPush(Object* a1, Object* a2);
int actionPush(Object* a1, Object* a2);
int _action_can_talk_to(Object* a1, Object* a2);
int _knockback_dest_tile(Object* obj, int maxDistance, int rotation);
void _combat_apply_knockback(Attack* attack);
// Split halves for the melee/ranged reorder (pacing design §8.5): compute the dests
// against the pre-damage board BEFORE _apply_damage, commit (emit the MOVE) AFTER
// attackResult so the knockback delta rides after the event that reserves the victim.
// victims/dests must have capacity 1 + EXPLOSION_TARGET_COUNT.
int _combat_compute_knockback(Attack* attack, Object** victims, int* dests);
void _combat_commit_knockback(Object** victims, const int* dests, int count);

} // namespace fallout

#endif /* ACTIONS_H */
