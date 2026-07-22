#include "combat.h"

#include <limits.h>
#include <string.h>

#include "animation.h"
#include "art.h"
#include "combat_defs.h"
#include "combat_intent.h"
#include "combat_ui.h"
#include "critter.h"
#include "debug.h"
#include "game.h"
#include "game_mouse.h"
#include "input.h"
#include "item.h"
#include "map.h"
#include "message.h"
#include "msg_channel.h"
#include "object.h"
#include "perk.h"
#include "pres_record.h"
#include "presenter.h"
#include "proto.h"
#include "server_loop.h"
#include "server_players.h"
#include "settings.h"
#include "stat.h"
#include "tile.h"

// Headless/authoritative combat drivers, split out of the f2_client combat_ui.cc
// (REWRITE_PLAN P5 cut-list head H3). combat_ui.cc is NOT linked into f2_server,
// so its serverLoopActive() combat bodies were dead there and core combat.cc's
// by-name calls into them (_combat_turn_run/_combat_input/_combat_display/
// _combat_outline_on/off/calledShotSelectHitLocation) resolved to abort stubs.
//
// This TU (f2_core, SDL-free) now OWNS those entry points with the branch that
// the server needs:
//   - _combat_turn_run / _combat_input: the serverLoopActive() headless driver
//     inline; the interactive frame-pump / keyboard loop delegates to the
//     combat*Client() helpers in combat_ui.cc (f2_client — stubbed & never
//     reached on the server, which always takes the headless branch).
//   - _combat_display: pure narration (messageList + presenter()->consoleMessage),
//     already SDL-free, moved WHOLE with its static helpers and hitLocationGetName
//     (the last is still used by the called-shot chrome in combat_ui.cc, so it is
//     no longer static — declared in combat_ui.h).
//   - _combat_outline_on/off: pure critter-highlight presentation, moved whole
//     with a serverLoopActive() no-op guard (headless has no world view; the only
//     client symbols they name — gameMouseGetMode/tileWindowRefresh — were already
//     stubs).
//   - calledShotSelectHitLocation: a thin dispatcher — the server never reaches the
//     modal picker (the intent driver passes an explicit hit location to
//     _combat_attack), so it returns -1 headless; the client modal lives in
//     combat_ui.cc as calledShotSelectHitLocationClient.
//
// The probe (client binary) still exercises the headless branch here, now core-
// located, so the golden event/replay streams are unchanged.

namespace fallout {

static void combatCopyDamageAmountDescription(char* dest, size_t size, Object* critter, int damage);
static void combatAddDamageFlagsDescription(char* dest, int flags, Object* critter);

// 0x500B50
static char _a_1[] = ".";

// 0x4227DC
void _combat_turn_run()
{
    if (serverLoopActive()) {
        // Server loop: drain in-flight combat animations with no fps
        // limiter / renderPresent. The instant scheduler completes all sads in
        // a single _process_bk pass, so _combat_turn_running drops to 0 in ~1
        // iteration; damage-on-completion ordering is preserved by the
        // scheduler contract (SERVER_LOOP_DESIGN.md §3). The drainGuard mirrors
        // _object_animate's backstop: an unterminating spin here is the exact
        // unkillable hang that defeated the prior retrofit attempts.
        int drainGuard = 0;
        while (_combat_turn_running > 0) {
            _process_bk();
            if (++drainGuard > 100000) {
                debugPrint("server: _combat_turn_run drain exceeded guard (turn_running=%d)\n", _combat_turn_running);
                break;
            }
        }
        return;
    }

    combatTurnRunClient();
}

// --- server-loop combat driver (SERVER_LOOP_DESIGN.md §3, combat_intent.h) ---

// The dude's primary hit mode for its equipped weapon (right hand, then left),
// else an unarmed punch. Headless has no interface bar, so interfaceGetCurrent
// HitMode() is meaningless (returns -1); pick from the equipped weapon directly.
static int serverDudeHitMode()
{
    Object* weapon = critterGetItem2(gDude);
    if (weapon != nullptr && itemGetType(weapon) == ITEM_TYPE_WEAPON) {
        return HIT_MODE_RIGHT_WEAPON_PRIMARY;
    }
    weapon = critterGetItem1(gDude);
    if (weapon != nullptr && itemGetType(weapon) == ITEM_TYPE_WEAPON) {
        return HIT_MODE_LEFT_WEAPON_PRIMARY;
    }
    return HIT_MODE_PUNCH;
}

// Resolve an ATTACK intent's target: a specific object id, or (arg < 0) the
// nearest living hostile critter on the dude's elevation. "Hostile" prefers
// critters actively fighting the dude (whoHitMe == gDude) so the dude
// concentrates fire on its attackers (and finishes them off) rather than
// spreading damage across every non-allied bystander; it falls back to the
// nearest enemy-team critter when nobody is currently engaging the dude.
// A crosshair attack targets a CRITTER or a DOOR — nothing else. This mirrors
// vanilla's target selection exactly (game_mouse.cc:1146-1153: resolve a critter
// under the crosshair first, else take any object but only proceed if
// objectIsDoor()). Doors are destructible — you can shoot or blow them open — so
// they are a legitimate attack target; the server previously accepted critters
// ONLY, which turned every door attack into a "no target" failure that ate the
// player's turn. Reading data.critter on a door in the bad-shot/attack path is a
// union alias, but Object is fixed-size so it stays in bounds — the same thing
// vanilla does for _combat_attack_this(door).
static bool serverAttackTargetIsDoor(Object* obj)
{
    if (obj == nullptr || PID_TYPE(obj->pid) != OBJ_TYPE_SCENERY) {
        return false;
    }
    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1 || proto == nullptr) {
        return false;
    }
    return proto->scenery.type == SCENERY_TYPE_DOOR;
}

static Object* serverResolveTarget(int arg)
{
    Object* obj = objectFindFirst();
    if (arg >= 0) {
        // Address by netId, the client-facing object identity. obj->id is measured
        // ~53% non-unique (MP_PROTOCOL §7) — matching it would let a `cattack N`
        // hit the wrong critter. netId is the wire's unique handle (§3.f item 1).
        while (obj != nullptr) {
            if (obj->netId == arg
                && (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER || serverAttackTargetIsDoor(obj))) {
                return obj;
            }
            obj = objectFindNext();
        }
        return nullptr;
    }

    Object* bestEngaging = nullptr;
    int bestEngagingDistance = INT_MAX;
    Object* bestEnemy = nullptr;
    int bestEnemyDistance = INT_MAX;
    while (obj != nullptr) {
        if (obj != gDude
            && PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER
            && obj->elevation == gDude->elevation
            && critterIsActive(obj)
            && !critterIsDead(obj)
            && obj->data.critter.combat.team != gDude->data.critter.combat.team) {
            int distance = objectGetDistanceBetween(gDude, obj);
            if (obj->data.critter.combat.whoHitMe == gDude) {
                if (distance < bestEngagingDistance) {
                    bestEngagingDistance = distance;
                    bestEngaging = obj;
                }
            }
            if (distance < bestEnemyDistance) {
                bestEnemyDistance = distance;
                bestEnemy = obj;
            }
        }
        obj = objectFindNext();
    }
    return bestEngaging != nullptr ? bestEngaging : bestEnemy;
}

// Human-readable reason for a rejected attack, streamed to the acting player so a
// denied shot is never silent. Mirrors the cases vanilla _combat_attack_this
// (combat.cc:6040) reports to the message log, plus the "not a critter or door"
// case the server can hit that the vanilla crosshair filters out client-side.
static const char* combatBadShotReason(int badShotCode)
{
    switch (badShotCode) {
    case COMBAT_BAD_SHOT_NO_AMMO:
        return "out of ammo";
    case COMBAT_BAD_SHOT_OUT_OF_RANGE:
        return "target out of range";
    case COMBAT_BAD_SHOT_NOT_ENOUGH_AP:
        return "not enough action points";
    case COMBAT_BAD_SHOT_ALREADY_DEAD:
        return "target already dead";
    case COMBAT_BAD_SHOT_AIM_BLOCKED:
        return "aim is blocked";
    case COMBAT_BAD_SHOT_ARM_CRIPPLED:
        return "cannot use a two-handed weapon with a crippled arm";
    case COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED:
        return "cannot use weapons with both arms crippled";
    default:
        return "attack could not be performed";
    }
}

// Drain the dude's queued combat intents for one beat (combat_intent.h). Each
// intent calls the same core entry points the AI uses. The turn-ending reasons —
// END_TURN, AP exhaustion, an unexecutable attack (target dead / out of range /
// no ammo), or a turn-end authority (combatPlayerTurnShouldBreak) — report
// kEndTurn; running out of queued intents reports kQueueDrained. A failed attack
// is a one-shot command: it is discarded (popped) and the turn ends — never held
// to retry on a later turn (the client re-sends; see the attack branch). Success/
// move/end-turn also pop, and each such consumption bumps `consumed`.
//
// CROSS-PIN: this is the shared body of the two dude-turn drivers. _combat_input
// (below, byte-identical legacy path) ends the turn on either stop; the resumable
// session's player-turn pump (combat.cc combatSessionAdvance) treats kQueueDrained
// as "wait for more intents across beats" per the idle-timer/wait policy.
CombatPumpOutcome combatServerPumpIntents(int actorSlot)
{
    // The acting player. gDude is ALREADY this actor (the caller holds a
    // ServerActorScope, see the header) — which is what makes the core entry
    // points below act on them — but the sites that name a critter explicitly use
    // `actor`, so the reader can see whose turn this is without having to trust an
    // invisible global swap. Falls back to gDude if the slot is somehow unbound,
    // which is exactly the single-player value.
    Object* actor = playerActorAt(actorSlot);
    if (actor == nullptr) {
        actor = gDude;
    }

    CombatPumpOutcome outcome { CombatPumpStop::kQueueDrained, 0 };
    CombatIntent intent;
    while (combatIntentPeekForSlot(actorSlot, &intent)) {
        if (combatPlayerTurnShouldBreak() || combatPlayerTurnOutOfAp()) {
            outcome.stop = CombatPumpStop::kEndTurn;
            break;
        }
        if (intent.kind == COMBAT_INTENT_END_TURN) {
            combatIntentPopForSlot(actorSlot);
            outcome.stop = CombatPumpStop::kEndTurn;
            break;
        }
        if (intent.kind == COMBAT_INTENT_END_COMBAT) {
            // Vanilla RETURN semantics: attempt to end combat. combatAttemptEnd()
            // scans the roster and self-refuses (streaming console msg 103) if a
            // hostile is unwilling to stop; on success it arms the end-combat
            // handshake (COMBAT_STATE_0x08) that combatPlayerTurnResolve() reads as
            // "combat over". Pop and continue pumping (the shouldBreak check at the
            // top of the loop, and the caller's resolve, observe the handshake) —
            // unlike END_TURN this is not itself a turn-ending stop.
            combatIntentPopForSlot(actorSlot);
            combatAttemptEnd();
            outcome.consumed++;
            continue;
        }
        if (intent.kind == COMBAT_INTENT_MOVE) {
            bool recording = combatMoveRecorded();
            if (recording) presRecordAmbientBegin();
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
            // Run vs walk is presentation only — same AP either way (charged per hex
            // by movementChargeApForStep); run selects the faster anim + glide pace.
            if (intent.run) {
                animationRegisterRunToTile(actor, intent.arg, actor->elevation, -1, 0);
            } else {
                animationRegisterMoveToTile(actor, intent.arg, actor->elevation, -1, 0);
            }
            reg_anim_end();
            combatMoveRecordClose(recording, actor); // ship the recorded walk + commit state
            _combat_turn_run(); // AP charged per hex by movementChargeApForStep
            combatIntentPopForSlot(actorSlot);
            outcome.consumed++;
            continue;
        }
        if (intent.kind == COMBAT_INTENT_INTERACT) {
            // Use a door/lever, pick an item up, loot a container, use an item on
            // something, use a skill. The body lives in server_control.cc beside
            // the out-of-combat path it shares its outcome switch with; we are
            // inside this actor's ServerActorScope, which is what makes those
            // gDude-shaped engine bodies act on `actor`.
            //
            // ALWAYS pops, whatever the result. Like a failed attack (below) this
            // is a one-shot command: a target that died, moved out of reach, or an
            // approach that ran out of AP means the order is spent, not retried on
            // a later turn — the client re-sends if the player still wants it.
            // Unlike a failed attack it does NOT end the turn: being unable to
            // reach a door says nothing about whether you can still shoot.
            serverRunCombatInteract(actor, intent.interactVerb, intent.arg, intent.interactArg);
            combatIntentPopForSlot(actorSlot);
            outcome.consumed++;
            continue;
        }
        // COMBAT_INTENT_ATTACK. Under the server loop _combat_attack applies
        // the outcome directly (no animation — Phase 2.4 de-coupling), so
        // both melee and ranged resolve here. A shot that cannot execute is
        // discarded and ends the turn (one-shot; see the failure branch below).
        Object* target = serverResolveTarget(intent.arg);
        // Honor the client-selected hit mode (the interface bar's hand + primary/
        // secondary, §3.f item 2); AUTO (debug-port bare cattack, or a viewer whose
        // bar can't report a weapon) falls back to the current-weapon default. The
        // server re-validates either way via _combat_check_bad_shot below, so a
        // bogus mode is rejected rather than trusted.
        int hitMode = intent.hitMode == COMBAT_INTENT_HITMODE_AUTO
            ? serverDudeHitMode()
            : intent.hitMode;
        // An aimed intent costs +1 AP; pass the aiming flag so the bad-shot
        // check validates the aimed AP cost, and the hit location so
        // _combat_attack applies the called shot (it derives `aiming`, the AP
        // charge, the to-hit penalty and the body-part crit table from
        // defenderHitLocation). TORSO is excluded from "aiming" to match
        // _combat_attack's internal rule (it groups TORSO with UNCALLED and
        // charges base AP); treating it as aimed here would make the bad-shot
        // check demand base+1 AP that _combat_attack never charges.
        bool aiming = intent.hitLocation != HIT_LOCATION_UNCALLED
            && intent.hitLocation != HIT_LOCATION_TORSO;
        // Validate and execute exactly as vanilla _combat_attack_this does — and,
        // like it, NEVER end the turn on a rejected shot. A rejected attack is a
        // no-op that costs nothing: vanilla shows the reason and lets the player act
        // again (out of range / out of ammo / aim blocked / not enough AP, or a
        // target that is neither a critter nor a door). Ending the turn here was a
        // server-only divergence that this restores.
        //
        // The intent is still DISCARDED (popped): a one-shot command is spent, never
        // re-queued. THAT is what fixed the dry-fire NO_AMMO softlock — an earlier
        // design held the failed intent, so it re-failed every beat and spun combat
        // at machine speed; popping removes that class entirely. Ending the turn was
        // never what fixed it, and it is the part players hated (a mis-click on a
        // wall burning a whole turn). Popping without ending the turn is equally
        // softlock-free: the queue simply drains and the pump waits for fresh input.
        //
        // The denial is streamed to the acting player on the refusal channel so it is
        // never silent (owner request: "explicit msg — intent denied, invalid:
        // <reason>; no more skip turn").
        const char* denyReason = nullptr;
        int badShot = COMBAT_BAD_SHOT_OK;
        if (target == nullptr) {
            denyReason = "no valid target (not a critter or door)";
        } else if ((badShot = _combat_check_bad_shot(actor, target, hitMode, aiming))
            != COMBAT_BAD_SHOT_OK) {
            denyReason = combatBadShotReason(badShot);
        } else if (_combat_attack(actor, target, hitMode, intent.hitLocation) != 0) {
            denyReason = combatBadShotReason(-1); // generic "could not be performed"
        }
        if (denyReason != nullptr) {
            if (actor != nullptr && actor->netId != 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Attack denied: %s.", denyReason);
                presenter()->consoleMessageStyled(actor->netId, kMsgChannelRefusal, msg);
            }
            fprintf(stderr,
                "f2_server: control cattack DENIED slot=%d target=%d reason=%s (turn kept)\n",
                actorSlot, intent.arg, denyReason);
            combatIntentPopForSlot(actorSlot); // spent one-shot — do NOT end the turn
            continue;
        }
        _combat_turn_run(); // drain any residual animation (none headless)
        combatIntentPopForSlot(actorSlot);
        outcome.consumed++;
    }

    // The turn is over the moment the player can no longer act, even without an
    // END_TURN intent: vanilla auto-ends a turn at 0 AP / 0 free-move. The loop
    // above only reaches the line-170 check while an intent is queued, so the
    // beat that drains the LAST affordable action leaves the queue empty and would
    // report kQueueDrained — which the resumable session (combat.cc
    // combatSessionAdvancePlayerTurn) reads as "wait across beats", hanging the
    // turn open until the idle timer fires (the player looks stuck after spending
    // their last AP). Re-checking here ends the turn in that same beat. Legacy /
    // non-resumable paths end the turn on either stop, so this is behavior-neutral
    // (and golden-invisible) there; only the resumable session's wait is affected.
    //
    // ...UNLESS the actor is standing in their inventory screen. Opening it costs
    // 4 AP, so spending your last AP on it would otherwise auto-end your turn
    // right here — while the screen is still up and the player is mid-decision —
    // and the fight would move on behind an open modal. Vanilla dodges this by
    // blocking the whole game loop while the inventory is open; with N players we
    // cannot, so we block only this actor's TURN-END instead, which produces the
    // same result for them without freezing anyone else. The turn still ends the
    // moment they close the screen (the queue is drained and the AP is gone), and
    // the resumable idle deadline still caps it, so an abandoned screen cannot
    // stall the fight.
    if (outcome.stop == CombatPumpStop::kQueueDrained && combatPlayerTurnOutOfAp()
        && !serverSlotInModal(actorSlot)) {
        outcome.stop = CombatPumpStop::kEndTurn;
    }
    return outcome;
}

int _combat_input()
{
    ScopedGameMode gm(GameMode::kPlayerTurn);

    if (serverLoopActive()) {
        // Server loop: drive the dude's turn from the combat intent queue instead
        // of keyboard/mouse. Drain this beat's intents; with no queued intent (or
        // any turn-ending reason) the turn ends immediately — the old auto-end-turn
        // default. combatPlayerTurnResolve() consumes the end-combat handshake,
        // exactly as the legacy break path. Behavior is identical to the prior
        // inline loop; the drain body was lifted into combatServerPumpIntents so
        // the resumable session (combat.cc) reuses it without duplicating logic.
        combatServerPumpIntents();
        return combatPlayerTurnResolve();
    }

    return combatInputClient();
}

// Print attack description to monitor.
//
// 0x425170
void _combat_display(Attack* attack)
{
    MessageListItem messageListItem;

    if (attack->attacker == gDude) {
        Object* weapon = critterGetWeaponForHitMode(attack->attacker, attack->hitMode);
        int strengthRequired = weaponGetMinStrengthRequired(weapon);

        if (perkGetRank(attack->attacker, PERK_WEAPON_HANDLING) != 0) {
            strengthRequired -= 3;
        }

        if (weapon != nullptr) {
            if (strengthRequired > critterGetStat(gDude, STAT_STRENGTH)) {
                // You are not strong enough to use this weapon properly.
                messageListItem.num = 107;
                if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                    presenter()->consoleMessage(messageListItem.text);
                }
            }
        }
    }

    Object* mainCritter;
    if ((attack->attackerFlags & DAM_HIT) != 0) {
        mainCritter = attack->defender;
    } else {
        mainCritter = attack->attacker;
    }

    char* mainCritterName = _a_1;

    char you[20];
    you[0] = '\0';
    if (critterGetStat(gDude, STAT_GENDER) == GENDER_MALE) {
        // You (male)
        messageListItem.num = 506;
    } else {
        // You (female)
        messageListItem.num = 556;
    }

    if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
        strcpy(you, messageListItem.text);
    }

    int baseMessageId;
    if (mainCritter == gDude) {
        mainCritterName = you;
        if (critterGetStat(gDude, STAT_GENDER) == GENDER_MALE) {
            baseMessageId = 500;
        } else {
            baseMessageId = 550;
        }
    } else if (mainCritter != nullptr) {
        mainCritterName = objectGetName(mainCritter);
        if (critterGetStat(mainCritter, STAT_GENDER) == GENDER_MALE) {
            baseMessageId = 600;
        } else {
            baseMessageId = 700;
        }
    }

    char text[280];
    if (attack->defender != nullptr
        && attack->oops != nullptr
        && attack->defender != attack->oops
        && (attack->attackerFlags & DAM_HIT) != 0) {
        if (FID_TYPE(attack->defender->fid) == OBJ_TYPE_CRITTER) {
            if (attack->oops == gDude) {
                // 608 (male) - Oops! %s was hit instead of you!
                // 708 (female) - Oops! %s was hit instead of you!
                messageListItem.num = baseMessageId + 8;
                if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                    snprintf(text, sizeof(text), messageListItem.text, mainCritterName);
                }
            } else {
                // 509 (male) - Oops! %s were hit instead of %s!
                // 559 (female) - Oops! %s were hit instead of %s!
                const char* name = objectGetName(attack->oops);
                messageListItem.num = baseMessageId + 9;
                if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                    snprintf(text, sizeof(text), messageListItem.text, mainCritterName, name);
                }
            }
        } else {
            if (attack->attacker == gDude) {
                if (critterGetStat(attack->attacker, STAT_GENDER) == GENDER_MALE) {
                    // (male) %s missed
                    messageListItem.num = 515;
                } else {
                    // (female) %s missed
                    messageListItem.num = 565;
                }

                if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                    snprintf(text, sizeof(text), messageListItem.text, you);
                }
            } else {
                const char* name = objectGetName(attack->attacker);
                if (critterGetStat(attack->attacker, STAT_GENDER) == GENDER_MALE) {
                    // (male) %s missed
                    messageListItem.num = 615;
                } else {
                    // (female) %s missed
                    messageListItem.num = 715;
                }

                if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                    snprintf(text, sizeof(text), messageListItem.text, name);
                }
            }
        }

        strcat(text, ".");

        presenter()->consoleMessage(text);
    }

    if ((attack->attackerFlags & DAM_HIT) != 0) {
        Object* v21 = attack->defender;
        if (v21 != nullptr && (v21->data.critter.combat.results & DAM_DEAD) == 0) {
            text[0] = '\0';

            if (FID_TYPE(v21->fid) == OBJ_TYPE_CRITTER) {
                if (attack->defenderHitLocation == HIT_LOCATION_TORSO) {
                    if ((attack->attackerFlags & DAM_CRITICAL) != 0) {
                        switch (attack->defenderDamage) {
                        case 0:
                            // 528 - %s were critically hit for no damage
                            messageListItem.num = baseMessageId + 28;
                            break;
                        case 1:
                            // 524 - %s were critically hit for 1 hit point
                            messageListItem.num = baseMessageId + 24;
                            break;
                        default:
                            // 520 - %s were critically hit for %d hit points
                            messageListItem.num = baseMessageId + 20;
                            break;
                        }

                        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                            if (attack->defenderDamage <= 1) {
                                snprintf(text, sizeof(text), messageListItem.text, mainCritterName);
                            } else {
                                snprintf(text, sizeof(text), messageListItem.text, mainCritterName, attack->defenderDamage);
                            }
                        }
                    } else {
                        combatCopyDamageAmountDescription(text, sizeof(text), v21, attack->defenderDamage);
                    }
                } else {
                    const char* hitLocationName = hitLocationGetName(v21, attack->defenderHitLocation);
                    if (hitLocationName != nullptr) {
                        if ((attack->attackerFlags & DAM_CRITICAL) != 0) {
                            switch (attack->defenderDamage) {
                            case 0:
                                // 525 - %s were critically hit in %s for no damage
                                messageListItem.num = baseMessageId + 25;
                                break;
                            case 1:
                                // 521 - %s were critically hit in %s for 1 damage
                                messageListItem.num = baseMessageId + 21;
                                break;
                            default:
                                // 511 - %s were critically hit in %s for %d hit points
                                messageListItem.num = baseMessageId + 11;
                                break;
                            }
                        } else {
                            switch (attack->defenderDamage) {
                            case 0:
                                // 526 - %s were hit in %s for no damage
                                messageListItem.num = baseMessageId + 26;
                                break;
                            case 1:
                                // 522 - %s were hit in %s for 1 damage
                                messageListItem.num = baseMessageId + 22;
                                break;
                            default:
                                // 512 - %s were hit in %s for %d hit points
                                messageListItem.num = baseMessageId + 12;
                                break;
                            }
                        }

                        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                            if (attack->defenderDamage <= 1) {
                                snprintf(text, sizeof(text), messageListItem.text, mainCritterName, hitLocationName);
                            } else {
                                snprintf(text, sizeof(text), messageListItem.text, mainCritterName, hitLocationName, attack->defenderDamage);
                            }
                        }
                    }
                }

                if (settings.preferences.combat_messages && (attack->attackerFlags & DAM_CRITICAL) != 0 && attack->criticalMessageId != -1) {
                    messageListItem.num = attack->criticalMessageId;
                    if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                        strcat(text, messageListItem.text);
                    }

                    if ((attack->defenderFlags & DAM_DEAD) != 0) {
                        strcat(text, ".");
                        presenter()->consoleMessage(text);

                        if (attack->defender == gDude) {
                            if (critterGetStat(attack->defender, STAT_GENDER) == GENDER_MALE) {
                                // were killed
                                messageListItem.num = 207;
                            } else {
                                // were killed
                                messageListItem.num = 257;
                            }
                        } else {
                            if (critterGetStat(attack->defender, STAT_GENDER) == GENDER_MALE) {
                                // was killed
                                messageListItem.num = 307;
                            } else {
                                // was killed
                                messageListItem.num = 407;
                            }
                        }

                        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                            snprintf(text, sizeof(text), "%s %s", mainCritterName, messageListItem.text);
                        }
                    }
                } else {
                    combatAddDamageFlagsDescription(text, attack->defenderFlags, attack->defender);
                }

                strcat(text, ".");

                presenter()->consoleMessage(text);
            }
        }
    }

    if (attack->attacker != nullptr && (attack->attacker->data.critter.combat.results & DAM_DEAD) == 0) {
        if ((attack->attackerFlags & DAM_HIT) == 0) {
            if ((attack->attackerFlags & DAM_CRITICAL) != 0) {
                switch (attack->attackerDamage) {
                case 0:
                    // 514 - %s critically missed
                    messageListItem.num = baseMessageId + 14;
                    break;
                case 1:
                    // 533 - %s critically missed and took 1 hit point
                    messageListItem.num = baseMessageId + 33;
                    break;
                default:
                    // 534 - %s critically missed and took %d hit points
                    messageListItem.num = baseMessageId + 34;
                    break;
                }
            } else {
                // 515 - %s missed
                messageListItem.num = baseMessageId + 15;
            }

            if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                if (attack->attackerDamage <= 1) {
                    snprintf(text, sizeof(text), messageListItem.text, mainCritterName);
                } else {
                    snprintf(text, sizeof(text), messageListItem.text, mainCritterName, attack->attackerDamage);
                }
            }

            combatAddDamageFlagsDescription(text, attack->attackerFlags, attack->attacker);

            strcat(text, ".");

            presenter()->consoleMessage(text);
        }

        if ((attack->attackerFlags & DAM_HIT) != 0 || (attack->attackerFlags & DAM_CRITICAL) == 0) {
            if (attack->attackerDamage > 0) {
                combatCopyDamageAmountDescription(text, sizeof(text), attack->attacker, attack->attackerDamage);
                combatAddDamageFlagsDescription(text, attack->attackerFlags, attack->attacker);
                strcat(text, ".");
                presenter()->consoleMessage(text);
            }
        }
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* critter = attack->extras[index];
        if ((critter->data.critter.combat.results & DAM_DEAD) == 0) {
            combatCopyDamageAmountDescription(text, sizeof(text), critter, attack->extrasDamage[index]);
            combatAddDamageFlagsDescription(text, attack->extrasFlags[index], critter);
            strcat(text, ".");

            presenter()->consoleMessage(text);
        }
    }
}

// 0x425A9C
static void combatCopyDamageAmountDescription(char* dest, size_t size, Object* critter, int damage)
{
    MessageListItem messageListItem;
    char text[40];
    char* name;

    int messageId;
    if (critter == gDude) {
        text[0] = '\0';

        if (critterGetStat(gDude, STAT_GENDER) == GENDER_MALE) {
            messageId = 500;
        } else {
            messageId = 550;
        }

        // 506 - You
        messageListItem.num = messageId + 6;
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            strcpy(text, messageListItem.text);
        }

        name = text;
    } else {
        name = objectGetName(critter);

        if (critterGetStat(critter, STAT_GENDER) == GENDER_MALE) {
            messageId = 600;
        } else {
            messageId = 700;
        }
    }

    switch (damage) {
    case 0:
        // 627 - %s was hit for no damage
        messageId += 27;
        break;
    case 1:
        // 623 - %s was hit for 1 hit point
        messageId += 23;
        break;
    default:
        // 613 - %s was hit for %d hit points
        messageId += 13;
        break;
    }

    messageListItem.num = messageId;
    if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
        if (damage <= 1) {
            snprintf(dest, size, messageListItem.text, name);
        } else {
            snprintf(dest, size, messageListItem.text, name, damage);
        }
    }
}

// 0x425BA4
static void combatAddDamageFlagsDescription(char* dest, int flags, Object* critter)
{
    MessageListItem messageListItem;

    int num;
    if (critter == gDude) {
        if (critterGetStat(critter, STAT_GENDER) == GENDER_MALE) {
            num = 200;
        } else {
            num = 250;
        }
    } else {
        if (critterGetStat(critter, STAT_GENDER) == GENDER_MALE) {
            num = 300;
        } else {
            num = 400;
        }
    }

    if (flags == 0) {
        return;
    }

    if ((flags & DAM_DEAD) != 0) {
        // " and "
        messageListItem.num = 108;
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            strcat(dest, messageListItem.text);
        }

        // were killed
        messageListItem.num = num + 7;
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            strcat(dest, messageListItem.text);
        }

        return;
    }

    int bit = 1;
    int flagsListLength = 0;
    int flagsList[32];
    for (int index = 0; index < 32; index++) {
        if (bit != DAM_CRITICAL && bit != DAM_HIT && (bit & flags) != 0) {
            flagsList[flagsListLength++] = index;
        }
        bit <<= 1;
    }

    if (flagsListLength != 0) {
        for (int index = 0; index < flagsListLength - 1; index++) {
            strcat(dest, ", ");

            messageListItem.num = num + flagsList[index];
            if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                strcat(dest, messageListItem.text);
            }
        }

        // " and "
        messageListItem.num = 108;
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            strcat(dest, messageListItem.text);
        }

        messageListItem.num = num + flagsList[flagsListLength - 1];
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            strcat(dest, messageListItem.text);
        }
    }
}

// 0x42612C
char* hitLocationGetName(Object* critter, int hitLocation)
{
    MessageListItem messageListItem;
    messageListItem.num = 1000 + 10 * _art_alias_num(critter->fid & 0xFFF) + hitLocation;
    if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
        return messageListItem.text;
    }

    return nullptr;
}

// Highlights critters.
//
// 0x426AA8
void _combat_outline_on()
{
    if (serverLoopActive()) {
        // Headless has no world view; critter highlighting is pure presentation.
        return;
    }

    if (settings.preferences.target_highlight == TARGET_HIGHLIGHT_OFF) {
        return;
    }

    if (gameMouseGetMode() != GAME_MOUSE_MODE_CROSSHAIR) {
        return;
    }

    if (isInCombat()) {
        for (int index = 0; index < _list_total; index++) {
            _combat_update_critter_outline_for_los(_combat_list[index], 1);
        }
    } else {
        Object** critterList;
        int critterListLength = objectListCreate(-1, gElevation, OBJ_TYPE_CRITTER, &critterList);
        for (int index = 0; index < critterListLength; index++) {
            Object* critter = critterList[index];
            if (critter != gDude && (critter->data.critter.combat.results & DAM_DEAD) == 0) {
                _combat_update_critter_outline_for_los(critter, 1);
            }
        }

        if (critterListLength != 0) {
            objectListFree(critterList);
        }
    }

    // NOTE: Uninline.
    _combat_update_critters_in_los(true);

    tileWindowRefresh();
}

// 0x426BC0
void _combat_outline_off()
{
    if (serverLoopActive()) {
        return;
    }

    int i;
    int v5;
    Object** v9;

    if (gCombatState & 1) {
        for (i = 0; i < _list_total; i++) {
            objectDisableOutline(_combat_list[i], nullptr);
        }
    } else {
        v5 = objectListCreate(-1, gElevation, 1, &v9);
        for (i = 0; i < v5; i++) {
            objectDisableOutline(v9[i], nullptr);
            objectClearOutline(v9[i], nullptr);
        }
        if (v5) {
            objectListFree(v9);
        }
    }

    tileWindowRefresh();
}

// 0x426218
int calledShotSelectHitLocation(Object* critter, int* hitLocation, int hitMode)
{
    if (serverLoopActive()) {
        // The server drives the dude via the intent queue, which passes an
        // explicit hit location straight to _combat_attack; the interactive modal
        // picker is never reached (and cannot run headless). Report "cancelled".
        *hitLocation = HIT_LOCATION_TORSO;
        return -1;
    }

    return calledShotSelectHitLocationClient(critter, hitLocation, hitMode);
}

} // namespace fallout
