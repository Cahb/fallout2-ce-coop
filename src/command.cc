#include "command.h"

#include <limits.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "actions.h"
#include "animation.h"
#include "barter_intent.h"
#include "character_transaction.h"
#include "combat.h"
#include "combat_intent.h"
#include "critter.h"
#include "debug.h"
#include "dialog_intent.h"
#include "game.h"
#include "input.h"
#include "inventory.h"
#include "item.h"
#include "map.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "pipboy.h"
#include "pres_record.h"
#include "proto.h"
#include "proto_instance.h"
#include "savegame.h"
#include "scripts.h"
#include "server_loop.h"
#include "sim_clock.h"
#include "skill.h"
#include "state_dump.h"
#include "stat.h"
#include "trait.h"
#include "worldmap.h"
#include "worldmap_intent.h"

namespace fallout {

// H-49: the character editor's begin/rollback snapshot of committed dude state
// (charsnap = begin, charroll = rollback), held for the span of a probe run so
// a golden can snapshot -> mutate -> roll back. Formerly a captured local in
// mainHeadlessProbe; now dispatcher-owned session state (one process = one run,
// so static zero-init is equivalent to the old `= {}` per-run local).
static CharacterSnapshot gCommandCharSnapshot = {};

void probeApplyAggro(int aggroCount)
{
    std::vector<Object*> critters;
    Object* candidate = objectFindFirst();
    while (candidate != nullptr) {
        if (PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
            && candidate != gDude
            && !critterIsDead(candidate)) {
            critters.push_back(candidate);
        }
        candidate = objectFindNext();
    }

    std::sort(critters.begin(), critters.end(), [](Object* a, Object* b) {
        int distanceA = objectGetDistanceBetween(gDude, a);
        int distanceB = objectGetDistanceBetween(gDude, b);
        if (distanceA != distanceB) return distanceA < distanceB;
        return a->id < b->id;
    });

    int hostiles = 0;
    for (Object* critter : critters) {
        if (hostiles >= aggroCount) {
            break;
        }
        _critter_set_who_hit_me(critter, gDude);
        // Same flag _caiSetupTeamCombat uses: guarantees the critter
        // joins combat even if its script sets a fleeing maneuver.
        critter->data.critter.combat.maneuver |= CRITTER_MANEUVER_ENGAGING;
        hostiles++;
        debugPrint("headless-probe: aggro critter id=%d pid=0x%08X dist=%d\n",
            critter->id, critter->pid, objectGetDistanceBetween(gDude, critter));
    }

    if (hostiles > 0) {
        // Seat the dude in the initiative order explicitly: without an
        // attacker/defender pair he never joins the combatant list and
        // _combat_should_end() dissolves the fight immediately.
        CombatStartData csd;
        memset(&csd, 0, sizeof(csd));
        csd.attacker = critters[0];
        csd.defender = gDude;
        scriptsRequestCombat(&csd);
        debugPrint("headless-probe: combat requested with %d hostiles\n", hostiles);
    }
}

// Nearest live-or-dead critter to the dude on his elevation — the shared target
// scan of the `critwarp`/`critwalk` debug verbs (same shape as loot/steal's).
static Object* commandNearestCritter()
{
    Object* best = nullptr;
    int bestDistance = INT_MAX;
    for (Object* c = objectFindFirst(); c != nullptr; c = objectFindNext()) {
        if (PID_TYPE(c->pid) == OBJ_TYPE_CRITTER && c != gDude
            && c->elevation == gDude->elevation) {
            int distance = objectGetDistanceBetween(gDude, c);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = c;
            }
        }
    }
    return best;
}

void commandDispatch(const Command& command)
{
    const int i = command.tick;
    debugPrint("headless-probe: action %s:%d at tick %d\n", command.name, command.arg, i);
    if (strcmp(command.name, "savegame") == 0) {
        // Drive the save/load DRIVER (not the slot-picker screen) from the probe,
        // so the client binary — with every real subsystem present instead of the
        // server's stubs — can be walked through the same save→load→save cycle as
        // f2_server. That comparison is the only oracle for whether a load-path
        // effect is ours or the engine's; there was none before this verb.
        // `arg` is the 1-based slot the directory names use.
        savegameRefreshPatchesPath();
        savegameSetSlot(command.arg - 1);
        savegameSetPreviewBuffer(nullptr); // no thumbnail: writes the block blank
        int rc = lsgPerformSaveGame();
        debugPrint("probe-savegame: save slot %d rc=%d\n", command.arg, rc);
    } else if (strcmp(command.name, "loadgame") == 0) {
        savegameRefreshPatchesPath();
        savegameSetSlot(command.arg - 1);
        int rc = lsgLoadGameInSlot(command.arg - 1);
        debugPrint("probe-savegame: load slot %d rc=%d\n", command.arg, rc);
    } else if (strcmp(command.name, "xp") == 0) {
        pcAddExperience(command.arg);
    } else if (strcmp(command.name, "rad") == 0) {
        critterAdjustRadiation(gDude, command.arg);
    } else if (strcmp(command.name, "poison") == 0) {
        critterAdjustPoison(gDude, command.arg);
    } else if (strcmp(command.name, "drug") == 0) {
        Object* item = nullptr;
        if (objectCreateWithPid(&item, command.arg) == 0 && item != nullptr) {
            if (_item_d_take_drug(gDude, item) == 1) {
                _obj_destroy(item);
            }
        }
    } else if (strcmp(command.name, "hurt") == 0) {
        critterAdjustHitPoints(gDude, -command.arg);
    } else if (strcmp(command.name, "useskill") == 0) {
        skillUse(gDude, gDude, command.arg, 0);
    } else if (strcmp(command.name, "useskillon") == 0) {
        // Use skill `arg` on the nearest wounded living critter
        // (excluding the dude) on the dude's elevation via
        // actionUseSkill — exercising the serverLoopActive() decouple
        // in actionUseSkill that applies the outcome (_obj_use_skill_on
        // -> skillUse) directly instead of via a reg_anim callback.
        // Prefer a wounded target so first-aid/doctor have HP to heal.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (candidate != gDude
                && PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                && candidate->elevation == gDude->elevation
                && (candidate->flags & (OBJECT_HIDDEN | OBJECT_NO_BLOCK)) == 0
                && (candidate->data.critter.combat.results & DAM_DEAD) == 0
                && critterGetHitPoints(candidate) < critterGetStat(candidate, STAT_MAXIMUM_HIT_POINTS)) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: useskillon skill=%d id=%d pid=0x%08X hp_before=%d/%d dist=%d\n",
                command.arg, best->id, best->pid,
                critterGetHitPoints(best), critterGetStat(best, STAT_MAXIMUM_HIT_POINTS), bestDistance);
            actionUseSkill(gDude, best, command.arg);
            debugPrint("headless-probe: useskillon post hp=%d uses=%d\n",
                critterGetHitPoints(best), skillGetUsesToday(command.arg));
        } else {
            debugPrint("headless-probe: no wounded critter found for useskillon\n");
        }
    } else if (strcmp(command.name, "give") == 0) {
        // give:PID adds one; give:PID:N adds N (for stackables like
        // caps/ammo, so a barter fixture can stock the dude in one
        // verb). arg2 == -1 (unspecified) means one.
        int count = command.arg2 > 0 ? command.arg2 : 1;
        Object* item = nullptr;
        if (objectCreateWithPid(&item, command.arg) == 0 && item != nullptr) {
            _obj_disconnect(item, nullptr);
            itemAdd(gDude, item, count);
        }
    } else if (strcmp(command.name, "drop") == 0
        || strcmp(command.name, "usedrug") == 0
        || strcmp(command.name, "useitem") == 0
        || strcmp(command.name, "unload") == 0
        || strcmp(command.name, "reload") == 0
        || strcmp(command.name, "stow") == 0) {
        // Find the dude's stack of the given pid (top level only).
        Inventory* inventory = &(gDude->data.inventory);
        Object* item = nullptr;
        int stackQuantity = 0;
        for (int index = 0; index < inventory->length; index++) {
            if (inventory->items[index].item->pid == command.arg) {
                item = inventory->items[index].item;
                stackQuantity = inventory->items[index].quantity;
                break;
            }
        }
        if (item == nullptr) {
            debugPrint("headless-probe: no item with pid %d\n", command.arg);
        } else if (strcmp(command.name, "drop") == 0) {
            itemDropStack(gDude, item, stackQuantity);
        } else if (strcmp(command.name, "usedrug") == 0) {
            itemUseDrug(gDude, gDude, item, true);
        } else if (strcmp(command.name, "useitem") == 0) {
            itemUseFromInventory(gDude, gDude, item, true);
        } else if (strcmp(command.name, "reload") == 0) {
            // Self-contained: give a fresh pack of the weapon's
            // default ammo type, then load it.
            int ammoPid = weaponGetAmmoTypePid(item);
            Object* ammoPack = nullptr;
            if (ammoPid != -1 && objectCreateWithPid(&ammoPack, ammoPid) == 0 && ammoPack != nullptr) {
                _obj_disconnect(ammoPack, nullptr);
                itemAdd(gDude, ammoPack, 1);
                weaponLoadAmmo(gDude, item, ammoPack, 1, true, nullptr);
            }
        } else if (strcmp(command.name, "stow") == 0) {
            // Store the item into the first carried container.
            Object* container = nullptr;
            for (int index = 0; index < inventory->length; index++) {
                if (itemGetType(inventory->items[index].item) == ITEM_TYPE_CONTAINER) {
                    container = inventory->items[index].item;
                    break;
                }
            }
            if (container != nullptr) {
                containerStoreItem(gDude, container, item, 1, true);
            } else {
                debugPrint("headless-probe: no container to stow into\n");
            }
        } else {
            weaponUnloadIntoInventory(gDude, item, true);
        }
    } else if (strcmp(command.name, "mark") == 0) {
        stateDumpRecordCheckpoint(i);
    } else if (strcmp(command.name, "aggro") == 0) {
        probeApplyAggro(command.arg);
    } else if (strcmp(command.name, "warp") == 0) {
        // Debug/demo: shift the dude by `arg` hex tiles (drive the authoritative
        // actor so a connected viewer sees unmistakable in-view motion). Emits an
        // objectMoved event; the viewer moves gDude by netId. arg negative moves
        // the other way; the dude stays central in the viewer's camera.
        int dest = gDude->tile + command.arg;
        if (dest < 0) {
            dest = 0;
        }
        objectSetLocation(gDude, dest, gDude->elevation, nullptr);
    } else if (strcmp(command.name, "critwarp") == 0) {
        // Debug/demo: shift the critter NEAREST the dude by `arg` hex tiles. Moves
        // a visible NPC (e.g. artemple's spear guard) so a connected viewer sees an
        // unmistakable in-view MOVE that is not the dude.
        Object* best = commandNearestCritter();
        if (best == nullptr) {
            debugPrint("headless-probe: critwarp — no critter found\n");
        } else {
            int dest = best->tile + command.arg;
            if (dest < 0) {
                dest = 0;
            }
            debugPrint("headless-probe: critwarp id=%d pid=0x%X %d -> %d\n",
                best->id, best->pid, best->tile, dest);
            objectSetLocation(best, dest, best->elevation, nullptr);
        }
    } else if (strcmp(command.name, "walk") == 0) {
        // Debug/demo: like `warp`, but WALK the dude ±arg tiles through the
        // register-move path — under F2_SERVER_SMOOTH_WALK (server_anim.cc) the
        // motion steps one tile per beat so a connected viewer sees it animate;
        // without the gate it applies within the beat, like `walkto`. arg2=1 runs.
        int dest = gDude->tile + command.arg;
        if (dest < 0) {
            dest = 0;
        }
        debugPrint("headless-probe: walk %d -> %d run=%d\n",
            gDude->tile, dest, command.arg2 == 1 ? 1 : 0);
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        if (command.arg2 == 1) {
            animationRegisterRunToTile(gDude, dest, gDude->elevation, -1, 0);
        } else {
            animationRegisterMoveToTile(gDude, dest, gDude->elevation, -1, 0);
        }
        reg_anim_end();
    } else if (strcmp(command.name, "critwalk") == 0) {
        // Debug/demo: `walk` for the critter nearest the dude — the walking
        // counterpart of critwarp. arg2=1 runs.
        Object* best = commandNearestCritter();
        if (best == nullptr) {
            debugPrint("headless-probe: critwalk — no critter found\n");
        } else {
            int dest = best->tile + command.arg;
            if (dest < 0) {
                dest = 0;
            }
            debugPrint("headless-probe: critwalk id=%d pid=0x%X %d -> %d\n",
                best->id, best->pid, best->tile, dest);
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
            if (command.arg2 == 1) {
                animationRegisterRunToTile(best, dest, best->elevation, -1, 0);
            } else {
                animationRegisterMoveToTile(best, dest, best->elevation, -1, 0);
            }
            reg_anim_end();
        }
    } else if (strcmp(command.name, "levelup") == 0) {
        // H-43 award: grant per-level skill points (and perk
        // cadence) for levels arg+1 .. current, like the editor.
        bool freePerk = pcLevelUpApply(command.arg, pcGetStat(PC_STAT_LEVEL));
        debugPrint("headless-probe: levelup free_perk=%d sp=%d\n",
            freePerk ? 1 : 0, pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS));
    } else if (strcmp(command.name, "perk") == 0) {
        // H-44: commit perk arg like the perk dialog does —
        // snapshot current ranks (the editor's perks backup taken
        // when the dialog opens), apply, and report any pending
        // follow-up choice (Tag!/Mutate! picks are then driven
        // via the tag4/mutate actions).
        int perksBackup[PERK_COUNT];
        for (int perk = 0; perk < PERK_COUNT; perk++) {
            perksBackup[perk] = perkGetRank(gDude, perk);
        }
        int pendingChoice;
        int rc = perkChoiceApply(gDude, command.arg, perksBackup, &pendingChoice);
        debugPrint("headless-probe: perk %d rc=%d pending=%d hp=%d sp=%d\n",
            command.arg, rc, pendingChoice,
            critterGetHitPoints(gDude), pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS));
    } else if (strcmp(command.name, "tag4") == 0) {
        // H-48: commit skill arg as the Tag! perk's 4th tag
        // skill, against the current tagged set.
        int taggedSkills[NUM_TAGGED_SKILLS];
        skillsGetTagged(taggedSkills, NUM_TAGGED_SKILLS);
        skillsTagPerkApply(taggedSkills, command.arg);
        debugPrint("headless-probe: tag4 %d\n", command.arg);
    } else if (strcmp(command.name, "mutate") == 0) {
        // H-47: drive the extracted Mutate! trait swap like the
        // perk dialog session: compute the dialog's trait count
        // (2 minus leading empty slots), lose the slot-0 trait
        // (as the first-listed line-0 pick) when one is selected,
        // then gain trait arg and commit via traitsSetSelected.
        int traits[TRAITS_MAX_SELECTED_COUNT];
        traitsGetSelected(&traits[0], &traits[1]);

        int remaining = TRAITS_MAX_SELECTED_COUNT - 1;
        int traitIndex = 0;
        while (remaining >= 0) {
            if (traits[traitIndex] != -1) {
                break;
            }
            remaining--;
            traitIndex++;
        }
        int traitCount = TRAITS_MAX_SELECTED_COUNT - traitIndex;

        if (traitCount >= 1) {
            traitsMutateDrop(traits, traitCount, 0, traits[0]);
        }
        traitsMutateGain(traits, traitCount, command.arg);
        debugPrint("headless-probe: mutate %d -> traits %d %d\n",
            command.arg, traits[0], traits[1]);
    } else if (strcmp(command.name, "lootall") == 0 || strcmp(command.name, "stealall") == 0) {
        // Drive a headless loot/steal session against the nearest
        // container item (loot) or critter (steal) via the
        // extracted session primitives: open gates -> detach ->
        // take all -> reattach.
        bool isSteal = command.name[0] == 's';
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            bool suitable;
            if (isSteal) {
                suitable = PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                    && candidate != gDude
                    && candidate->elevation == gDude->elevation;
            } else {
                suitable = PID_TYPE(candidate->pid) == OBJ_TYPE_ITEM
                    && itemGetType(candidate) == ITEM_TYPE_CONTAINER
                    && candidate->elevation == gDude->elevation;
            }
            if (suitable) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best == nullptr) {
            debugPrint("headless-probe: no %s target found\n", command.name);
        } else {
            debugPrint("headless-probe: %s target id=%d pid=0x%X dist=%d\n", command.name, best->id, best->pid, bestDistance);
            if (lootOpenCheck(gDude, best, isSteal) == LOOT_OPEN_OK) {
                Object* item1;
                Object* item2;
                Object* armor;
                Object* hiddenBox = lootTargetDetach(best, isSteal, &item1, &item2, &armor);
                if (hiddenBox != nullptr) {
                    bool took = lootTakeAll(gDude, best);
                    debugPrint("headless-probe: lootTakeAll -> %d\n", took ? 1 : 0);
                    lootTargetReattach(best, hiddenBox, item1, item2, armor);
                }
            } else {
                debugPrint("headless-probe: loot open refused\n");
            }
        }
    } else if (strcmp(command.name, "wmtravel") == 0) {
        // H-13: drive the extracted worldmap travel tick headlessly.
        // Packed arg is destX * 10000 + destY (world pixel coords).
        // Mirrors the wmWorldMapFunc loop shape: step -> rest-heal ->
        // mark-visited -> clock tick -> encounter check, gated on
        // wmPartyIsWalking() like the original isWalking guard.
        int destX = command.arg / 10000;
        int destY = command.arg % 10000;
        int wmX;
        int wmY;
        wmGetPartyWorldPos(&wmX, &wmY);
        debugPrint("headless-probe: wmtravel from %d,%d to %d,%d\n", wmX, wmY, destX, destY);
        wmPartyInitWalking(destX, destY);
        unsigned int healTime = 0;
        int steps = 0;
        int heals = 0;
        bool encounter = false;
        while (wmPartyIsWalking() && steps < 600) {
            // Under the server loop each travel step consumes one
            // server tick's worth of sim time. Advancing the sim
            // clock here (instead of reading getTicks() as a
            // call-counter) makes the rest-heal (1000ms) and
            // encounter (1500ms) cadences a function of STEPS WALKED,
            // matching how wmWorldMapFunc paces them per frame against
            // the wall clock — and riding the same sim time base as
            // wmRndEncounterOccurred (worldmap.cc) and tickersExecute
            // (input.cc). The whole walk still runs inside this single
            // intent drain, so local ambient sim (_process_bk) does
            // not interleave with travel steps, mirroring the worldmap
            // suspending the local map. This retires the
            // getTicks-as-counter dependency in the SERVER driver;
            // F2_FAKE_CLOCK stays set on the gate only for other latent
            // getTicks readers. The legacy F2_PROBE_ACTIONS pump keeps
            // getTicks() so its golden stays byte-identical.
            unsigned int now;
            if (serverLoopActive()) {
                simClockAdvance(kServerTickDelta);
                now = simClockNow();
            } else {
                now = getTicks();
            }
            worldmapTravelStep(destX, destY);
            if (worldmapTravelRestHeal(now, healTime)) {
                healTime = now;
                heals++;
            }
            worldmapTravelMarkVisited();
            worldmapTravelClockTick();
            steps++;
            if (wmPartyIsWalking() && worldmapTravelEncounterCheck()) {
                encounter = true;
                break;
            }
        }
        int areaIdx;
        wmGetPartyWorldPos(&wmX, &wmY);
        wmGetPartyCurArea(&areaIdx);
        debugPrint("headless-probe: wmtravel steps=%d heals=%d pos=%d,%d area=%d encounter=%d gas=%d time=%u\n",
            steps, heals, wmX, wmY, areaIdx, encounter ? 1 : 0, wmCarGasAmount(), gameTimeGetTime());
    } else if (strcmp(command.name, "rest") == 0 || strcmp(command.name, "restopt") == 0) {
        // H-40/H-42: drive the REAL pipboy rest loop headlessly via
        // the server driver (pipboyRestHeadless -> pipboyRest with
        // UI/real-time pacing guarded out under serverLoopActive()),
        // instead of reimplementing its control flow here. rest:N
        // rests for N game minutes (decoded to hours:minutes, kind 0);
        // restopt:N picks rest-menu option N via the extracted intent
        // decoder (until-hour wall-clock math + the until-healed
        // kinds, whose two-pass 24h/3h chunking now runs inside the
        // real pipboyRest). The probe owns the H-42
        // _critter_can_obj_dude_rest gate the pipboy checks before
        // offering the rest menu.
        if (!_critter_can_obj_dude_rest()) {
            debugPrint("headless-probe: cannot rest here\n");
        } else {
            // Session start: the pipboy resets the heal-cadence
            // accumulator when it opens (pipboyWindowInit).
            restHealReset();

            int hours = 0;
            int minutes = 0;
            int kind = 0;
            if (strcmp(command.name, "rest") == 0) {
                hours = command.arg / 60;
                minutes = command.arg % 60;
            } else {
                restOptionDecode(command.arg, &hours, &minutes, &kind);
            }

            bool interrupted = pipboyRestHeadless(hours, minutes, kind);

            debugPrint("headless-probe: rest %d:%02d kind=%d interrupted=%d hp=%d time=%u\n",
                hours, minutes, kind, interrupted ? 1 : 0,
                critterGetHitPoints(gDude), gameTimeGetTime());
        }
    } else if (strcmp(command.name, "usedoor") == 0) {
        // Use the nearest door on the dude's elevation.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (PID_TYPE(candidate->pid) == OBJ_TYPE_SCENERY
                && candidate->elevation == gDude->elevation) {
                Proto* proto;
                if (protoGetProto(candidate->pid, &proto) == 0
                    && proto->scenery.type == SCENERY_TYPE_DOOR) {
                    int distance = objectGetDistanceBetween(gDude, candidate);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        best = candidate;
                    }
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: using door id=%d dist=%d\n", best->id, bestDistance);
            _obj_use_door(gDude, best);
        } else {
            debugPrint("headless-probe: no door found\n");
        }
    } else if (strcmp(command.name, "pickup") == 0) {
        // Pick up the nearest ground item on the dude's elevation
        // (excludes containers — those open the loot modal). Drives
        // the actionPickUp serverLoopActive() decouple.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (PID_TYPE(candidate->pid) == OBJ_TYPE_ITEM
                && candidate->elevation == gDude->elevation
                && itemGetType(candidate) != ITEM_TYPE_CONTAINER) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: picking up id=%d pid=0x%08X dist=%d\n",
                best->id, best->pid, bestDistance);
            actionPickUp(gDude, best);
        } else {
            debugPrint("headless-probe: no ground item found\n");
        }
    } else if (strcmp(command.name, "climb") == 0) {
        // Climb the nearest ladder/stairs on the dude's elevation.
        // Drives the _action_use_an_item_on_object serverLoopActive()
        // decouple (a same-map climb flips gElevation + dude elev,
        // map unchanged).
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (PID_TYPE(candidate->pid) == OBJ_TYPE_SCENERY
                && candidate->elevation == gDude->elevation) {
                Proto* proto;
                if (protoGetProto(candidate->pid, &proto) == 0
                    && (proto->scenery.type == SCENERY_TYPE_LADDER_UP
                        || proto->scenery.type == SCENERY_TYPE_LADDER_DOWN
                        || proto->scenery.type == SCENERY_TYPE_STAIRS)) {
                    int distance = objectGetDistanceBetween(gDude, candidate);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        best = candidate;
                    }
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: climbing id=%d pid=0x%08X dist=%d\n",
                best->id, best->pid, bestDistance);
            _action_use_an_object(gDude, best);
        } else {
            debugPrint("headless-probe: no ladder/stairs found\n");
        }
    } else if (strcmp(command.name, "push") == 0) {
        // Shove the nearest pushable critter one hex. actionCheckPush
        // requires the target to carry a compiled push_p_proc, so this
        // is the FIRST probe to dispatch a real .int script proc
        // (SCRIPT_PROC_PUSH). The rotate+one-hex displacement completes
        // headless via the instant scheduler (same path as walkto); no
        // actionPush change is needed. If the target's push_p_proc sets
        // script_overrides, actionPush returns early with no move (the
        // critter only reacts) — the dump then captures the script's
        // side effects, still exercising the dispatch.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (candidate != gDude
                && PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                && candidate->elevation == gDude->elevation
                && actionCheckPush(gDude, candidate)) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: push id=%d pid=0x%08X sid=%d tile_before=%d dist=%d\n",
                best->id, best->pid, best->sid, best->tile, bestDistance);
            int rc = actionPush(gDude, best);
            debugPrint("headless-probe: push rc=%d tile_after=%d\n", rc, best->tile);
        } else {
            debugPrint("headless-probe: no pushable critter found\n");
        }
    } else if (strcmp(command.name, "dtalk") == 0) {
        // Request dialog with the nearest scripted critter whose
        // scriptIndex == arg (mirrors actionTalk ->
        // scriptsRequestDialog). scriptIndex (the SCRIPTS.LST line)
        // is used rather than obj->id because obj->id is NOT unique
        // for map-placed objects, whereas a singleton talk NPC has a
        // unique script (e.g. denbus1 Story Teller = 940). The
        // request is consumed by scriptsHandleRequests later in this
        // same server tick, which calls gameDialogEnter -> the
        // target's SCRIPT_PROC_TALK; under the server loop that runs
        // the whole conversation inline, draining any dsay intents
        // already queued. Queue the dsay choices on an earlier tick.
        Object* target = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (candidate != gDude
                && PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                && candidate->sid != -1
                && candidate->scriptIndex == command.arg) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    target = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (target != nullptr) {
            debugPrint("headless-probe: dtalk script_idx=%d id=%d pid=0x%08X sid=%d tile=%d\n",
                command.arg, target->id, target->pid, target->sid, target->tile);
            scriptsRequestDialog(target);
        } else {
            debugPrint("headless-probe: dtalk no scripted critter with script_idx=%d\n", command.arg);
        }
    } else if (strcmp(command.name, "dsay") == 0) {
        // Queue a dialog reply-option selection (0-based index) for
        // the server _gdProcess driver to apply when the next dtalk
        // conversation runs. Push these before the dtalk verb.
        dialogIntentPush(DIALOG_INTENT_SELECT, command.arg);
        debugPrint("headless-probe: dsay select option %d\n", command.arg);
    } else if (strcmp(command.name, "dend") == 0) {
        // Queue an explicit end-conversation intent (bail out of a
        // node that keeps offering options).
        dialogIntentPush(DIALOG_INTENT_END, 0);
        debugPrint("headless-probe: dend\n");
    } else if (strcmp(command.name, "boffer") == 0) {
        // Queue "put arg (a proto id) from the dude onto the player
        // offer table". Drained by the server inventoryOpenTrade
        // branch when a dialog option's proc opens barter. arg2
        // (parsed into command.arg2, defaulting to a full stack) is
        // the quantity; boffer:PID or boffer:PID:QTY.
        barterIntentPush(BARTER_INTENT_OFFER_ITEM, command.arg, command.arg2);
        debugPrint("headless-probe: boffer pid=%d qty=%d\n", command.arg, command.arg2);
    } else if (strcmp(command.name, "btake") == 0) {
        // Queue "put arg (a proto id) from the merchant onto the
        // barterer table" (the goods the player wants in return).
        barterIntentPush(BARTER_INTENT_TAKE_ITEM, command.arg, command.arg2);
        debugPrint("headless-probe: btake pid=%d qty=%d\n", command.arg, command.arg2);
    } else if (strcmp(command.name, "bunoffer") == 0) {
        // Queue "take arg (a proto id) back off a table to its owner".
        barterIntentPush(BARTER_INTENT_UNOFFER_ITEM, command.arg, command.arg2);
        debugPrint("headless-probe: bunoffer pid=%d qty=%d\n", command.arg, command.arg2);
    } else if (strcmp(command.name, "bcommit") == 0) {
        // Queue the Offer/commit ('M') — valuate the offer and, if
        // good, transfer both tables. Arg-less (dummy arg required).
        barterIntentPush(BARTER_INTENT_COMMIT, 0, 0);
        debugPrint("headless-probe: bcommit\n");
    } else if (strcmp(command.name, "bdone") == 0) {
        // Queue the Talk/Done ('T') — return both tables to owners
        // and leave barter. Arg-less (dummy arg required).
        barterIntentPush(BARTER_INTENT_DONE, 0, 0);
        debugPrint("headless-probe: bdone\n");
    } else if (strcmp(command.name, "bcancel") == 0) {
        // Queue the ESC/cancel — leave barter without a table sweep.
        barterIntentPush(BARTER_INTENT_CANCEL, 0, 0);
        debugPrint("headless-probe: bcancel\n");
    } else if (strcmp(command.name, "charsnap") == 0) {
        // H-49: begin the character-editor transaction — snapshot
        // the committed dude sim state via the extracted core
        // take steps (the editor's savePlayer captures).
        characterSnapshotTake(&gCommandCharSnapshot);
        characterSnapshotTakeSkillPoints(&gCommandCharSnapshot);
        debugPrint("headless-probe: charsnap hp=%d sp=%d name=%s\n",
            gCommandCharSnapshot.hitPoints, gCommandCharSnapshot.unspentSkillPoints,
            gCommandCharSnapshot.name);
    } else if (strcmp(command.name, "charroll") == 0) {
        // H-49: roll the transaction back — restore the committed
        // dude sim state via the extracted core restore steps (the
        // editor's Cancel path, minus the editor-only lines).
        characterSnapshotRestore(&gCommandCharSnapshot);
        characterSnapshotRestoreSkillPoints(&gCommandCharSnapshot);
        characterSnapshotRestoreHitPoints(&gCommandCharSnapshot);
        debugPrint("headless-probe: charroll hp=%d sp=%d\n",
            critterGetHitPoints(gDude), pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS));
    } else if (strcmp(command.name, "walkto") == 0) {
        // Walk the dude to an explicit destination tile via the
        // same core path a move-click drives (_dude_move), minus
        // the mouse-cursor dependency. Replaces the input-replay
        // walk trace under the server loop; the instant scheduler
        // completes the move within the tick.
        int fromTile = gDude->tile;
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToTile(gDude, command.arg, gDude->elevation, -1, 0);
        int rc = reg_anim_end();
        debugPrint("headless-probe: walkto tile=%d rc=%d from=%d\n",
            command.arg, rc, fromTile);
    } else if (strcmp(command.name, "cattack") == 0) {
        // Queue N "attack nearest hostile" dude combat intents
        // (combat_intent.h). Drained by the server-loop _combat_input
        // during the dude's turns. Push these before combat starts.
        for (int n = 0; n < command.arg; n++) {
            combatIntentPush(COMBAT_INTENT_ATTACK, -1);
        }
        debugPrint("headless-probe: queued %d attack intents\n", command.arg);
    } else if (strcmp(command.name, "caim") == 0) {
        // Queue ONE aimed "attack nearest hostile" combat intent with
        // hit location = arg (HIT_LOCATION_HEAD=0..GROIN=7; EYES=6;
        // UNCALLED=8). Drives the called-shot path headless: the server
        // _combat_input passes intent.hitLocation into _combat_attack,
        // which applies the +1 aimed AP cost, the to-hit penalty and the
        // body-part crit table (DAM_BLIND / DAM_CRIP_* in the target's
        // results). Validate the index: hit_location_penalty[] and
        // gCriticalHitTables[][] are indexed by it, so an out-of-range
        // arg would read out of bounds.
        if (command.arg < 0 || command.arg > HIT_LOCATION_UNCALLED) {
            debugPrint("headless-probe: caim invalid hitLocation=%d (want 0..%d)\n",
                command.arg, HIT_LOCATION_UNCALLED);
        } else {
            combatIntentPush(COMBAT_INTENT_ATTACK, -1, command.arg);
            debugPrint("headless-probe: queued aimed attack intent hitLocation=%d\n", command.arg);
        }
    } else if (strcmp(command.name, "cmove") == 0) {
        // Queue a combat move-to-tile intent.
        combatIntentPush(COMBAT_INTENT_MOVE, command.arg);
        debugPrint("headless-probe: queued cmove tile=%d\n", command.arg);
    } else if (strcmp(command.name, "cendturn") == 0) {
        combatIntentPush(COMBAT_INTENT_END_TURN, 0);
        debugPrint("headless-probe: queued end-turn intent\n");
    } else if (strcmp(command.name, "wield") == 0) {
        // Wield the first weapon in the dude's inventory into hand
        // `arg` (0 = left, 1 = right). The premade dude carries
        // weapons but wields none headless (the character selector's
        // wield flow is skipped). animate=false skips the take-out
        // animation (needs an open reg_anim context + drain we don't
        // have here); the hand-slot assignment is driven by the hand
        // arg directly, so the equip sticks.
        Inventory* inventory = &(gDude->data.inventory);
        Object* weapon = nullptr;
        for (int index = 0; index < inventory->length; index++) {
            if (itemGetType(inventory->items[index].item) == ITEM_TYPE_WEAPON) {
                weapon = inventory->items[index].item;
                break;
            }
        }
        // arg2==1 -> animate=true: exercise the reg_anim take-out draw (the presentation
        // record/replay weapon-draw path). Default (arg2=-1) keeps the animate=false probe.
        bool wieldAnimate = (command.arg2 == 1);
        if (weapon != nullptr) {
            int rc = _invenWieldFunc(gDude, weapon, command.arg, wieldAnimate);
            debugPrint("headless-probe: wield pid=0x%X hand=%d animate=%d rc=%d\n",
                weapon->pid, command.arg, wieldAnimate, rc);
        } else {
            debugPrint("headless-probe: no weapon in inventory to wield\n");
        }
    } else if (strcmp(command.name, "cdamage") == 0) {
        // Damage the nearest living critter (excluding the dude) on
        // the dude's elevation by `arg` HP via actionDamage with
        // animated=true — exercising the serverLoopActive() decouple
        // that forces the direct outcome (op_critter_damage's engine
        // path). A lethal hit finalizes the corpse via critterKill.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (candidate != gDude
                && PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                && candidate->elevation == gDude->elevation
                && (candidate->flags & (OBJECT_HIDDEN | OBJECT_NO_BLOCK)) == 0
                && (candidate->data.critter.combat.results & DAM_DEAD) == 0) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: cdamage id=%d pid=0x%08X amount=%d hp_before=%d\n",
                best->id, best->pid, command.arg, critterGetHitPoints(best));
            actionDamage(best->tile, best->elevation, command.arg, command.arg, 0, true, false);
            debugPrint("headless-probe: cdamage post hp=%d dead=%d\n",
                critterGetHitPoints(best),
                (best->data.critter.combat.results & DAM_DEAD) != 0);
        } else {
            debugPrint("headless-probe: no critter found to damage\n");
        }
    } else if (strcmp(command.name, "explode") == 0) {
        // Detonate an explosion (min=max=`arg` damage) centred on the
        // nearest living critter's tile via actionExplode with
        // animate=true — exercising the serverLoopActive() decouple
        // that forces the direct outcome (op_explosion / EXPLOSION
        // queue-event engine path): radius damage, deaths, corpse
        // finalize, scenery destruction.
        Object* best = nullptr;
        int bestDistance = INT_MAX;
        Object* candidate = objectFindFirst();
        while (candidate != nullptr) {
            if (candidate != gDude
                && PID_TYPE(candidate->pid) == OBJ_TYPE_CRITTER
                && candidate->elevation == gDude->elevation
                && (candidate->flags & (OBJECT_HIDDEN | OBJECT_NO_BLOCK)) == 0
                && (candidate->data.critter.combat.results & DAM_DEAD) == 0) {
                int distance = objectGetDistanceBetween(gDude, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = candidate;
                }
            }
            candidate = objectFindNext();
        }
        if (best != nullptr) {
            debugPrint("headless-probe: explode at tile=%d elev=%d amount=%d target=%d hp_before=%d\n",
                best->tile, best->elevation, command.arg, best->id, critterGetHitPoints(best));
            int rc = actionExplode(best->tile, best->elevation, command.arg, command.arg, gDude, true);
            debugPrint("headless-probe: explode rc=%d target hp=%d dead=%d\n",
                rc, critterGetHitPoints(best),
                (best->data.critter.combat.results & DAM_DEAD) != 0);
        } else {
            debugPrint("headless-probe: no critter found to explode\n");
        }
    } else if (strcmp(command.name, "endgame") == 0) {
        // Request the endgame (op_endgame_slideshow's deferred
        // path). scriptsHandleRequests -- run every serverTick --
        // drains SCRIPT_REQUEST_ENDGAME into endgamePlaySlideshow()
        // + endgamePlayMovie(), both guarded to early-return
        // headless: the slideshow reads gvars only, and the movie
        // sets _game_user_wants_to_quit = 2 (the terminal quit) in
        // place of the blocking playback. Without those guards this
        // run HANGS (creditsOpen / UINT_MAX static-scene loop), so a
        // clean completion is the smoke assertion for the decouple.
        debugPrint("headless-probe: endgame requested (quit before=%d)\n",
            _game_user_wants_to_quit);
        scriptsRequestEndgame();
    } else if (strcmp(command.name, "wmenter") == 0
        || strcmp(command.name, "wmmove") == 0
        || strcmp(command.name, "wmesc") == 0) {
        // Debug-console twins of the wire verbs (server_control.cc): push a
        // worldmap intent from the cmd port so the server-side travel driver can
        // be driven with NO viewer at all. Same queue, no claimant gate — the cmd
        // port is server-local by construction.
        if (strcmp(command.name, "wmenter") == 0) {
            worldmapIntentPush(WM_INTENT_ENTER, 0, 0);
        } else if (strcmp(command.name, "wmesc") == 0) {
            worldmapIntentPush(WM_INTENT_ESCAPE, 0, 0);
        } else {
            worldmapIntentPush(WM_INTENT_MOVE, command.arg, command.arg2);
        }
        debugPrint("headless-probe: %s %d %d\n", command.name, command.arg, command.arg2);
    } else if (strcmp(command.name, "entermap") == 0) {
        // Drive a REAL in-game map transition so the mapTransition presenter
        // event (map.cc mapLoad choke) is exercised — and pinned — under the
        // server loop. No other probe verb reaches mapLoadById: the initial
        // load happens before the narrate presenter is installed, and wmtravel
        // simulates worldmap steps without arriving. entermap:MAP[:ELEV] stages
        // a MapTransition; mapHandleTransition (run every serverTick) performs
        // the load on the next beat, wholesale-replacing the object set (the
        // dump then reflects the destination map). tile=-1 uses the map's
        // default entering location. arg2 unspecified (-1) => elevation 0.
        MapTransition transition;
        memset(&transition, 0, sizeof(transition));
        transition.map = command.arg;
        transition.elevation = command.arg2 > 0 ? command.arg2 : 0;
        transition.tile = -1;
        transition.rotation = ROTATION_SE;
        debugPrint("headless-probe: entermap %d elev=%d\n",
            transition.map, transition.elevation);
        mapSetTransition(&transition);
    } else {
        debugPrint("headless-probe: unknown action '%s'\n", command.name);
    }
}

} // namespace fallout
