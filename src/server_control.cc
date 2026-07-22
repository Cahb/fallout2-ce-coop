// f2_server control plane — the FIRST CONTROLLABLE CLIENT (P5-C STEP 6).
// See server_control.h for the model. This is an f2_server-only TU: it is the
// trust boundary between untrusted viewer-wire lines and the authoritative sim.

#include "server_control.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "actions.h" // _action_use_an_object / actionPickUp / actionPush / actionUseSkill
#include "animation.h"
#include "barter_intent.h" // BARTER_INTENT_* / barterIntentPush — the trade verbs
#include "art.h" // buildFid / artExists / ANIM_STAND — server-authoritative equip fid
#include "combat.h"
#include "combat_intent.h" // COMBAT_INTENT_* / combatIntentPush (P3)
#include "critter.h" // critterIsDead
#include "dialog_intent.h" // DIALOG_INTENT_* / dialogIntentPush (A2 dialog streaming)
#include "worldmap_intent.h" // WM_INTENT_* / worldmapIntentPush (worldmap streaming)
#include "server_worldmap.h" // worldmapServerActive — gate for wmmove/wmenter/wmesc
#include "game.h" // gDude / gMiscMessageList
#include "game_movie.h" // gameMovieAck — the movie barrier's release
#include "game_dialog.h" // _gdialogActive — dialog-active gate for dsay/dend
#include "inventory.h" // _inven_wield / _inven_unwield / critterGetArmor / _adjust_ac / HAND_*
#include "item.h" // itemGetType / ITEM_TYPE_CONTAINER / itemDropStack / itemRemove
#include "map.h" // mapGetLoadGeneration — drop latches on a map change
#include "message.h" // messageListGetItem / MessageListItem
#include "msg_channel.h" // kMsgChannel* — the greeting speaks on the system channel
#include "object.h"
#include "platform_compat.h" // compat_stricmp — account names compare case-insensitively
#include "pres_record.h" // presRecord* — record the interaction gesture as a presentation sequence
#include "presenter.h" // presenter()->consoleMessage
#include "proto.h" // protoGetProto / Proto / SCENERY_TYPE_DOOR
#include "proto_instance.h" // _obj_use_door / _obj_examine (INTERACTION verbs)
#include "scripts.h" // scriptsRequestDialog
#include "server_accounts.h" // the account name -> slot table (login verb)
#include "server_anim.h" // serverAnimWalkInFlightFor
#include "player_create.h" // PlayerCreateSpec / playerCreateApply — stage-2 creation
#include "server_boot.h" // serverSpawnPlayerActor / playerActorSeedSheetFromHost
#include "server_loop.h" // serverEmitPlayerRoster / serverSetSlotSessionQuery
#include "server_players.h" // the player-actor registry + ServerActorScope
#include "skill.h" // SKILL_* (skilldex allow-list)
#include "stat.h" // critterGetStat / STAT_GENDER — armor skin is gender-specific
#include "tile.h" // tileIsValid / gHexGridSize
#include "wire_defs.h" // kNoSessionId

namespace {

// Cheap per-session-per-beat flood guard: a wire client cannot inject more than
// this many lines in a single drain (a control client sends a click or two per
// beat, not hundreds). Reset each drain by serverControlBeginDrain.
constexpr int kMaxLinesPerBeat = 32;

} // namespace

namespace fallout {

// Session → player actor bindings, indexed by REGISTRY SLOT (MP_PROPOSAL.md
// Ch 6.1). Replaces the single global claimant: authority is bound to a
// connection, and now to WHICH actor that connection drives.
//
// 0 = unbound. Slots are never added or removed here — membership is fixed at
// boot — so a disconnect, a death or a rebaseline changes a BINDING, never an
// identity. With one registered actor this is the old single-claimant behavior
// exactly: slot 0 is the only bindable slot.
static int gBindings[kMaxPlayerActors] = {};

// Slot this session drives, or -1 if it holds none (a SPECTATOR: it still
// receives the whole stream, it just cannot act).
int serverControlSlotForSession(int sessionId)
{
    if (sessionId == kNoSessionId) {
        return -1;
    }
    for (int slot = 0; slot < playerActorCount(); slot++) {
        if (gBindings[slot] == sessionId) {
            return slot;
        }
    }
    return -1;
}

Object* serverControlActorForSession(int sessionId)
{
    int slot = serverControlSlotForSession(sessionId);
    return slot >= 0 ? playerActorAt(slot) : nullptr;
}

int serverControlSessionForSlot(int slot)
{
    if (slot < 0 || slot >= playerActorCount()) {
        return kNoSessionId;
    }
    return gBindings[slot];
}

bool serverControlAnyBound()
{
    for (int slot = 0; slot < playerActorCount(); slot++) {
        if (gBindings[slot] != kNoSessionId) {
            return true;
        }
    }
    return false;
}

// Is this session driving the HOST actor (slot 0)? The debug CMD port carries no
// session and drives the host, so it answers yes.
//
// OWNER RULING 2026-07-20: the WORLDMAP and DIALOG screens are host-only. Both move
// state that belongs to everyone — travel relocates the whole party, and a
// conversation answers for it — so an extra must not be able to drag the group
// across the world map or speak for them. Same reasoning, and the same act, as the
// host-only map transition (MP_PROPOSAL Ch 14.2): anti-grief and no unconsented
// travel, an intentional asymmetry rather than a v1 shortcut. Extras still SEE both
// screens (every viewer receives the dialog nodes and the travel stream); they just
// cannot drive them. NOTE: this narrows MP_PROPOSAL Ch 11.2, which had the dialog
// driver being whichever actor requested the conversation.
static bool serverControlIsHostSession(int sessionId)
{
    return serverControlSlotForSession(sessionId) <= 0;
}

// ►► A REFUSAL MUST REACH THE PLAYER, NOT JUST THE OPERATOR (bugs list item U).
//
// Every reject path in this file already writes a line to the server's stderr,
// which no player will ever read. From inside the game a refused click is then
// indistinguishable from lag, so "why can't I loot this body" and "the server is
// desyncing, maybe walking around fixes it" look identical — and the rational
// response to the second is to spam-click, which is its own bug (item T). One
// line ends the confusion, and it names the rejecting branch for us too.
//
// TARGETED, never broadcast: the message answers THIS player's click and means
// nothing to anyone else. The address is their own actor's netId, the filter every
// viewer applies to decide a line is theirs (same mechanism as serverGreetClaimant).
//
// The stderr line stays exactly where it is and exactly as it reads. Several are
// gate contracts grepped by scripts/check_*.sh ([[gate-scripts-grep-log-strings]]:
// append after the prefix, never reword), and they carry operator detail — tiles,
// slots, netIds — that is noise to a player. Two audiences, two lines, on purpose.
//
// kMsgChannelRefusal is the grey "nothing happened" style (msg_channel.h): the
// player should register it and move on, not read it as the world speaking.
static void serverControlRefuseV(Object* actor, const char* fmt, va_list args)
{
    if (actor == nullptr) {
        // No actor means no address. The wire has no per-session event channel
        // (one encoder, one broadcast buffer), so an unbound spectator's refusal
        // has nowhere to go — the same limitation serverGreetClaimant documents
        // for the pre-claim window. Their stderr line still lands.
        return;
    }

    char line[256];
    vsnprintf(line, sizeof(line), fmt, args);
    presenter()->consoleMessageStyled(actor->netId, kMsgChannelRefusal, line);
}

// Refuse the session that sent the line. The common case: everything past the
// unbound-session gate in serverControlLine has a bound actor to answer.
static void serverControlRefuse(int sessionId, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    serverControlRefuseV(serverControlActorForSession(sessionId), fmt, args);
    va_end(args);
}

// Refuse by ACTOR, for the executors that were handed one instead of a session
// (serverControlMove, the pending-interaction drain). Same message, same address.
static void serverControlRefuseActor(Object* actor, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    serverControlRefuseV(actor, fmt, args);
    va_end(args);
}

// Dialog DRIVE ownership (server_control.h). Two values, both registry slots:
// the requester is a one-tick handoff from the TALK verb to the dialog entry
// point; the driver is live for the conversation's whole life.
static int gPendingDialogRequesterSlot = -1;
static int gDialogDriverSlot = -1;

void serverControlSetPendingDialogRequester(int slot)
{
    gPendingDialogRequesterSlot = slot;
}

int serverControlBeginDialogDrive()
{
    gDialogDriverSlot = gPendingDialogRequesterSlot;
    gPendingDialogRequesterSlot = -1; // consumed: never attribute the NEXT one
    return gDialogDriverSlot;
}

void serverControlEndDialogDrive()
{
    gDialogDriverSlot = -1;
    gPendingDialogRequesterSlot = -1;
}

bool serverControlDialogDriverPresent()
{
    if (gDialogDriverSlot < 0) {
        return true; // no driven conversation — not this predicate's business
    }
    return serverControlSessionForSlot(gDialogDriverSlot) != kNoSessionId;
}

// May this session answer the live conversation?
//
// Falls back to the HOST rule whenever no driver is recorded: an NPC-initiated
// conversation has no initiator to inherit from, and that is also the state every
// golden and the debug CMD port sit in, so the old behavior is what they keep.
static bool serverControlMayDriveDialog(int sessionId)
{
    if (gDialogDriverSlot < 0) {
        return serverControlIsHostSession(sessionId);
    }
    int slot = serverControlSlotForSession(sessionId);
    return slot < 0 // the debug CMD port carries no session and drives anything
        || slot == gDialogDriverSlot;
}

// Lines seen per session this beat (flood guard). Cleared each drain.
static std::unordered_map<int, int> gLineCounts;

// Client-reported OS string ("Linux"/"Windows"/"macOS"), keyed by SESSION. Sent by
// the client's `platform <name>` line just before login; surfaced in the join
// greeting so everyone sees which platform each player is driving from. Cosmetic
// identity only — cleared when the session drops (serverControlBeginDrain).
static std::unordered_map<int, std::string> gSessionPlatform;

// This server binary's own OS, compile-time. Shown to joiners so a demo makes the
// "drive from anything, serve on anything" point visible on both ends of the wire.
const char* serverPlatformName()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

// Pending player-initiated combat-start request (cstart): the registry SLOT of the
// player who asked, or -1 for none. Set by a claimant out of combat; consumed once
// by the server loop's idle tick. A latch and not a count — repeated cstart before
// the loop polls collapses to one entry, the FIRST one, so the player who actually
// opened the fight keeps the credit rather than being overwritten by a straggler.
static int gPendingCombatStartSlot = -1;

int serverControlConsumePendingCombatStart()
{
    int pending = gPendingCombatStartSlot;
    gPendingCombatStartSlot = -1;
    return pending;
}

// -- Out-of-combat interaction (walk-then-act) latch --------------------------
// INTERACTION_UX_DESIGN.md §2.3. A viewer verb that needs the actor adjacent
// (use/get/skill/talk) registers an approach walk and arms this latch; a poll
// each beat (serverControlAdvancePending, after the walk advance) fires the real
// engine outcome once the actor arrives, or drops with "cannot get there" if the
// walk finished short. Verbs that need no approach (look/push/rot) fire at receipt
// and never arm the latch. One latch (v1 = one claimant); a map<sessionId,...> is
// the named N-player generalization, not built now.

// Verb codes for the latch (distinct from the wire strings so the fire switch is
// closed and the untrusted string never survives past parse).
enum {
    kInteractUse,   // scenery use (door/lever/ladder/stairs) — approach <= 1
    kInteractGet,   // pick up a ground item                   — approach <= 1
    kInteractSkill, // use a skilldex skill on the target      — approach <= 1
    kInteractTalk,  // start dialog with a critter             — approach < 9
    kInteractLoot,  // walk adjacent to a container/corpse     — approach <= 1
                    // (approach-ONLY: no server outcome; the client opens its
                    //  loot modal once the dude arrives — take/put verbs do the
                    //  actual transfer, server-validated)
    kInteractUseItemOn, // use an inventory item ON the target — approach <= 1
                        // (the Temple Key on the locked door, a doctor's bag on
                        //  a critter, …). arg carries the item pid, re-resolved
                        //  on the actor's inventory at fire time.
};

struct PendingInteraction {
    int verb;        // kInteract*
    int targetNetId; // re-looked-up EVERY poll — never a cached Object*
    // Identity snapshot taken at arm. A rebaseline (a second viewer joining — same
    // map!) or a map change re-mints netIds by re-assigning them sequentially
    // (objectAssignAllNetIds), so a stale netId does NOT go dead — it resolves to a
    // DIFFERENT object. The id+pid snapshot (mirroring ServerWalk's ownerId/ownerPid)
    // rejects that at the poll so we never fire an outcome on the wrong object.
    int targetId;
    int targetPid;
    int arg;         // skill id for kInteractSkill; item pid for
                     // kInteractUseItemOn; unused otherwise
    int beatsLeft;   // pathing backstop; hitting 0 drops with "cannot get there"
};

// ~15 s of pathing at 100 ms/beat — a generous cap so a long legitimate approach
// completes, but a wedged/never-arriving walk cannot pin the latch forever.
constexpr int kInteractionBeatsCap = 150;

// One latch PER SESSION (MP_PROPOSAL.md Ch 7.3): each player's walk-then-act
// intent is independent, so P2 approaching a door must not cancel P1's approach
// to a corpse. At most one entry per bound session, so the map is tiny.
// "A fresh action supersedes" is now per-session, which is what it always meant.
static std::unordered_map<int, PendingInteraction> gPendingBySession;

// A pending interaction is being thrown away because the player asked for
// something else. SAY SO — silently dropping it is why "I clicked the item, he
// walked over, nothing happened" was indistinguishable from a genuine failure for
// so long (bugs list U: a rejected/cancelled action must announce itself).
static void serverControlSupersedePending(int sessionId, const char* why)
{
    auto it = gPendingBySession.find(sessionId);
    if (it == gPendingBySession.end()) {
        return;
    }
    fprintf(stderr, "f2_server: interact verb=%d netId=%d SUPERSEDED by %s\n",
        it->second.verb, it->second.targetNetId, why);
    gPendingBySession.erase(it);
}


// Registry SLOTS currently holding an open in-combat inventory screen. Keyed by
// slot, not session, for the usual reason (MP_PROPOSAL Ch 6.1): the slot is the
// durable identity, and a reconnect that re-binds the same slot should not
// inherit a screen it never paid for — which is why the combat-exit sweep and
// the own-turn re-check below both exist rather than trusting this set alone.
static std::unordered_set<int> gInventoryOpenSlots;

bool serverControlInventorySessionOpen(int slot)
{
    return gInventoryOpenSlots.count(slot) != 0;
}

void serverControlClearInventorySessions()
{
    gInventoryOpenSlots.clear();
}

// Vanilla "You cannot get there." (misc msg 2000), the same feedback _is_next_to /
// _can_talk_to stream when an approach can't reach (actions.cc:1112, 2087). Routed
// through the presenter → EVENT_CONSOLE, already rendered by the viewer.
// Addressed to the actor who could not get there: it answers THEIR click and
// means nothing to the other players (nothing happened).
static void interactionCannotGetThere(Object* actor)
{
    MessageListItem messageListItem;
    messageListItem.num = 2000;
    if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
        presenter()->consoleMessageFor(actor != nullptr ? actor->netId : 0, messageListItem.text);
    }
}

// The per-verb adjacency rule. All verbs require the actor adjacent to the target
// (distance <= 1). Talk was previously distance < 9 which fired dialog before the
// approach walk completed — see bugs/001-dialog-range-early-fire.md.
static bool interactionRuleSatisfied(int verb, Object* actor, Object* target)
{
    (void)verb;
    return objectGetDistanceBetween(actor, target) <= 1;
}

// Allow-list the untrusted wire skill id to the eight skilldex skills the vanilla
// USE_SKILL menu offers (game_mouse.cc skilldex switch) before it reaches
// actionUseSkill → the skill tables (INTERACTION_UX_DESIGN.md §2.5). Rejects any
// other index (combat/stat skills, out-of-range) at the trust boundary.
static bool interactionSkillAllowed(int skill)
{
    switch (skill) {
    case SKILL_SNEAK:
    case SKILL_LOCKPICK:
    case SKILL_STEAL:
    case SKILL_TRAPS:
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR:
    case SKILL_SCIENCE:
    case SKILL_REPAIR:
        return true;
    default:
        return false;
    }
}

// Run the real engine outcome. These are the SAME proven headless bodies the probe
// verbs / serverLoopActive decouples exercise — each applies its outcome directly
// (not via a reg_anim callback, which is a no-op on the server). Runs synchronously
// at the drain site; the caller MUST have consumed the latch first (a re-entrant
// script — combat start, map transition, actor destroy — must not re-fire it).
// Total units of `pid` on `actor`'s TOP-LEVEL inventory. Summed by pid, not read
// off one slot, precisely because a pickup can MERGE into an existing stack and
// destroy the incoming object — the slot changes identity, the total does not.
static int interactionCarriedQty(Object* actor, int pid)
{
    if (actor == nullptr) {
        return 0;
    }
    int total = 0;
    Inventory* inv = &actor->data.inventory;
    for (int i = 0; i < inv->length; i++) {
        if (inv->items[i].item != nullptr && inv->items[i].item->pid == pid) {
            total += inv->items[i].quantity;
        }
    }
    return total;
}

static void interactionFire(int verb, Object* actor, Object* target, int arg)
{
    // ►► The outcome USED TO BE ENTIRELY SILENT — no line on success, none on
    // failure — so "the latch fired and actionPickUp declined" looked exactly like
    // "the latch never fired". Every one of these engine entry points returns a
    // status; log it. Without this the only observable difference between a working
    // and a broken pickup is whether an inventory delta happens to follow.
    int rc = 0;
    switch (verb) {
    case kInteractUse:
        rc = _action_use_an_object(actor, target); // scenery: routes doors/ladders/stairs
        fprintf(stderr, "f2_server: interact FIRE use netId=%d rc=%d actorTile=%d targetTile=%d\n",
            target->netId, rc, actor->tile, target->tile);
        break;
    case kInteractGet: {
        // SNAPSHOT BEFORE THE ACTION. actionPickUp disconnects the item (tile
        // becomes -1) and may MERGE it into an existing stack, destroying the
        // duplicate object — so reading tile/distance afterwards reports -1 and a
        // 9999 distance that describe the aftermath, not the attempt. Logging those
        // post-hoc values already sent one diagnosis down the wrong path.
        //
        // preQty/postQty is the field that matters for "I pressed pickup and
        // nothing happened": picking up a pid you ALREADY carry merges the stacks,
        // so the item COUNT is unchanged and the inventory looks identical — a
        // successful pickup that is invisible unless you read the quantity.
        int preTile = target->tile;
        unsigned int preFlags = target->flags;
        int preDist = objectGetDistanceBetween(actor, target);
        int prePid = target->pid;
        int preQty = interactionCarriedQty(actor, prePid);
        rc = actionPickUp(actor, target);
        fprintf(stderr, "f2_server: interact FIRE get netId=%d pid=%d rc=%d actorTile=%d"
                        " targetTile=%d targetFlags=0x%X dist=%d qty %d->%d\n",
            target->netId, prePid, rc, actor->tile, preTile, preFlags, preDist,
            preQty, interactionCarriedQty(actor, prePid));
        break;
    }
    case kInteractSkill:
        // In combat this is a REFUSAL for seven of the eight skilldex skills —
        // actionUseSkill answers first aid / doctor / lockpick / steal / traps /
        // science / repair with proto msg 902 ("you cannot use that skill in
        // combat") and does nothing (actions.cc:1662ff). That is vanilla, and it
        // is why the skill verb is allowed through in combat rather than dropped
        // at the wire: dropping it silently left the player with no feedback at
        // all. Sneak is the exception and toggles as normal (actions.cc:1760).
        actionUseSkill(actor, target, arg);
        break;
    case kInteractTalk:
        // _talk_to's body: latch a dialog request consumed by scriptsHandleRequests
        // this same tick. actionTalk itself cannot be used — its outcome rides a
        // server-no-op reg_anim callback (actions.cc:2076-2077).
        //
        // Latch WHO asked, beside the request itself, because the conversation that
        // opens from it is theirs to drive. The two travel together: this runs at
        // the arrival of the approach walk the same player started, and the request
        // is drained the same tick.
        //
        // Keyed off the ACTOR rather than the session that sent the verb: this is a
        // deferred outcome, so the actor is the identity that survived the walk (the
        // session could have reconnected under a new id in between), and it is also
        // the identity the driver check ultimately compares.
        serverControlSetPendingDialogRequester(playerActorSlotOf(actor));
        scriptsRequestDialog(target);
        break;
    case kInteractLoot:
        // Open/close the container + record the animation for the viewer.
        // The viewer opens its loot modal once the dude arrives adjacent
        // (viewerPollPendingLoot), and take/put verbs do the transfer.
        //
        // Charge the same 3 AP vanilla does. _action_loot_container hangs
        // _check_scenery_ap_cost off a reg_anim callback (actions.cc:1621) that
        // never fires on the server, so the cost has to be applied in-call here;
        // it self-gates on isInCombat, so this stays free out of combat. A
        // refusal (msg 700, streamed by the check) means the container does not
        // open — otherwise looting a corpse mid-fight would be the one free
        // action in the game.
        if (_check_scenery_ap_cost(actor, target) == -1) {
            break;
        }
        _obj_use_container(actor, target);
        break;
    case kInteractUseItemOn: {
        // arg = the item pid. Re-resolve it on the actor's TOP-LEVEL inventory at
        // FIRE time — never a cached Object* (the inventory can change during the
        // approach; a consumed/dropped item must not fire). Run the real engine
        // action: on serverLoopActive() it dispatches straight to _obj_use_item_on
        // (the reg_anim callback that legacy uses is a server no-op), which runs the
        // item's / target's USE_OBJ_ON script (the Temple Key unlocks its door) or
        // the default use — any world change streams back via OBJECT_DELTA / events.
        Object* item = nullptr;
        Inventory* inv = &actor->data.inventory;
        for (int i = 0; i < inv->length; i++) {
            if (inv->items[i].item != nullptr && inv->items[i].item->pid == arg) {
                item = inv->items[i].item;
                break;
            }
        }
        if (item != nullptr) {
            // Through the AP wrapper, not _action_use_an_item_on_object direct:
            // it gates on and charges the vanilla 2 AP when the use succeeds
            // (actions.cc:2714) and is a plain pass-through out of combat, so
            // the out-of-combat latch that also lands here is unaffected.
            actionUseItemOnObjectWithApCost(actor, target, item);
        }
        break;
    }
    }
}

// The vanilla interaction GESTURE anim (ANIM_MAGIC_HANDS_* crouch/reach the reg_anim
// path plays before the outcome, actions.cc use/pickup/skill). Returns -1 for verbs/
// targets with no gesture (talk, and STAIRS use). GROUND for a prone critter / a
// ground-flagged scenery or container, MIDDLE otherwise.
static int interactionGestureAnim(int verb, Object* target)
{
    int type = FID_TYPE(target->fid);
    Proto* proto = nullptr;
    switch (verb) {
    case kInteractUse:
        if (type == OBJ_TYPE_SCENERY && protoGetProto(target->pid, &proto) == 0) {
            if (proto->scenery.type == SCENERY_TYPE_STAIRS) {
                return -1; // vanilla plays no gesture for stairs
            }
            if ((proto->scenery.extendedFlags & 0x01) != 0) {
                return ANIM_MAGIC_HANDS_GROUND;
            }
        }
        if (type == OBJ_TYPE_CRITTER && _critter_is_prone(target)) {
            return ANIM_MAGIC_HANDS_GROUND;
        }
        return ANIM_MAGIC_HANDS_MIDDLE;
    case kInteractGet:
        // Mirror actionPickUp (actions.cc:1372-1404): a plain item — or a container
        // you carry off whole — is picked up from the GROUND (crouch/reach down);
        // only an OPENED-in-place container consults its ground/middle openFlag. The
        // old code read openFlags on non-container protos (garbage union) → every
        // ground item wrongly played MIDDLE (same as a door).
        if (protoGetProto(target->pid, &proto) != 0) {
            return ANIM_MAGIC_HANDS_GROUND; // proto miss: default to the common case
        }
        if (proto->item.type != ITEM_TYPE_CONTAINER || _proto_action_can_pickup(target->pid)) {
            return ANIM_MAGIC_HANDS_GROUND;
        }
        return (proto->item.data.container.openFlags & 0x01) != 0
            ? ANIM_MAGIC_HANDS_GROUND
            : ANIM_MAGIC_HANDS_MIDDLE;
    case kInteractSkill:
        return (type == OBJ_TYPE_CRITTER && _critter_is_prone(target))
            ? ANIM_MAGIC_HANDS_GROUND
            : ANIM_MAGIC_HANDS_MIDDLE;
    default:
        return -1; // talk (no magic-hands); look/push/rot never reach here
    }
}

// Emit the gesture cue for the actor just before the outcome fires, so the viewer
// animates the crouch/reach (else the door/item just changes with no motion). A
// server-only presenter event — no-op on the headless/golden path.
static void interactionEmitGesture(int verb, Object* actor, Object* target)
{
    int anim = interactionGestureAnim(verb, target);
    if (anim < 0) {
        return;
    }
    presRecordSectionBegin();
    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    animationRegisterAnimate(actor, anim, 0);
    reg_anim_end();
    presRecordSectionEnd();
    presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount(), actor->netId);
}

// ─── In-combat interaction (Stage 4) ────────────────────────────────────────
// The combat twin of serverControlArmInteraction below. Called back from the
// combat pump on the actor's OWN turn, inside its ServerActorScope.
//
// The two differ in exactly one structural way, and it is worth stating because
// it is why this is a separate function rather than a flag on the other one:
// OUT of combat the approach is unbounded and spans beats, so it needs the latch
// (walk now, fire whenever you arrive). IN combat the approach is bounded by the
// actor's remaining AP and resolves inside this call — vanilla passes
// `critter->data.critter.combat.ap` to animationRegisterMoveToObject rather than
// -1 (actions.cc:1488), so the walk stops when the AP runs out. There is nothing
// left to span beats, so there is no latch, no beat cap, and no stale-target
// re-resolution window: the intent is spent here or not at all.
//
// AP for the OUTCOME is charged by the vanilla bodies themselves, not here —
// _check_scenery_ap_cost (3 AP) inside the use/pickup paths, and the 2 AP wrapper
// for use-item-on. Charging it again at this layer would double-bill.
bool serverControlRunCombatInteract(Object* actor, int verb, int targetNetId, int arg)
{
    if (actor == nullptr) {
        return false;
    }

    // Re-resolve the target NOW. It was validated when the intent was queued, but
    // that was potentially several turns ago — an intent waits for its issuer's
    // turn, and the fight moves on meanwhile. The critter may be dead, the item
    // may have been picked up by someone else, the door may already be open.
    Object* target = objectFindByNetId(targetNetId);
    if (target == nullptr || target->elevation != actor->elevation) {
        fprintf(stderr, "f2_server: combat interact verb=%d netId=%d target gone, spent\n",
            verb, targetNetId);
        return false;
    }

    if (!interactionRuleSatisfied(verb, actor, target)) {
        // AP-limited approach. No run variant: vanilla's in-combat branches all
        // register a WALK regardless of distance (actions.cc:1486, 1607, 1368) —
        // running is an out-of-combat affordance, and in combat the pace is the
        // turn's, not the player's.
        bool recording = combatMoveRecorded();
        if (recording) {
            presRecordAmbientBegin();
        }
        reg_anim_begin(ANIMATION_REQUEST_RESERVED);
        animationRegisterMoveToObject(actor, target, actor->data.critter.combat.ap, 0);
        reg_anim_end();
        combatMoveRecordClose(recording, actor); // ship the walk + commit state
        _combat_turn_run(); // charges AP per hex via movementChargeApForStep
    }

    // Re-test rather than trusting the walk: it stops short on running out of AP,
    // on a blocked path, or on the target having moved (a critter you meant to
    // use a skill on takes its own turn between yours). Vanilla says the same
    // thing here through _is_next_to, which streams msg 2000 and cancels the
    // queued outcome.
    if (!interactionRuleSatisfied(verb, actor, target)) {
        interactionCannotGetThere(actor);
        fprintf(stderr, "f2_server: combat interact verb=%d netId=%d short (AP/path)\n",
            verb, targetNetId);
        return false;
    }

    interactionEmitGesture(verb, actor, target);
    interactionFire(verb, actor, target, arg);
    fprintf(stderr, "f2_server: combat interact verb=%d netId=%d fired\n", verb, targetNetId);
    return true;
}

// Arm the walk-then-act latch for an approach verb, or fire immediately if the
// actor already satisfies the rule (preserves slice-1 "adjacent door opens now").
// The target was validated by the caller (netId/type/elevation/args). `actor` is
// the v1 gDude binding (mp-actor: the executor never hardwires it beyond here).
static void serverControlArmInteraction(int sessionId, Object* actor, int verb, Object* target, int arg)
{
    if (actor == nullptr) {
        return;
    }
    // IN COMBAT this does not arm anything and does not act now: the action
    // belongs to the issuer's turn, which may be several turns away. Queue it
    // stamped with their slot and let the pump run it (the callback above). The
    // branch lives here, at the single choke point every approach verb already
    // funnels through, so no individual verb arm can forget it.
    if (isInCombat()) {
        int actorSlot = serverControlSlotForSession(sessionId);
        if (actorSlot < 0) {
            actorSlot = 0; // the debug CMD port has no session and drives the host
        }
        combatIntentPushVerb(COMBAT_INTENT_INTERACT, verb, target->netId, arg, actorSlot);
        fprintf(stderr, "f2_server: control interact verb=%d netId=%d queued (combat, slot=%d)\n",
            verb, target->netId, actorSlot);
        return;
    }
    if (interactionRuleSatisfied(verb, actor, target)) {
        serverControlSupersedePending(sessionId, "a new interact");
        reg_anim_clear(actor); // stop any prior in-flight approach walk (vanilla
                               // cancels the prior sequence before a new action)
        interactionEmitGesture(verb, actor, target);
        interactionFire(verb, actor, target, arg);
        return;
    }
    // Register the approach through the same public entry points mv uses; under
    // F2_SERVER_SMOOTH_WALK this enqueues a stepped walk that stops one tile short
    // and faces the target (server_anim.cc serverAnimMoveToObject). Run when far,
    // mirroring vanilla (actions.cc:1337).
    bool run = objectGetDistanceBetween(actor, target) >= 5;
    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    if (run) {
        animationRegisterRunToObject(actor, target, -1, 0);
    } else {
        animationRegisterMoveToObject(actor, target, -1, 0);
    }
    reg_anim_end();
    // ►► operator[] would OVERWRITE any pending interaction silently, which is the
    // single most confusing thing this subsystem does: click item A, he starts
    // walking, click item B, and A is abandoned with no trace in the log and no
    // word to the player. Announce the replacement first (bugs list U).
    serverControlSupersedePending(sessionId, "a new approach");
    PendingInteraction& pending = gPendingBySession[sessionId];
    pending.verb = verb;
    pending.targetNetId = target->netId;
    pending.targetId = target->id;
    pending.targetPid = target->pid;
    pending.arg = arg;
    pending.beatsLeft = kInteractionBeatsCap;
    fprintf(stderr, "f2_server: control interact verb=%d netId=%d armed (approach, session %d)\n",
        verb, target->netId, sessionId);
}

// Drop a session's latch. Called when its binding is released (the intent dies
// with its owner) — see serverControlBeginDrain.
void serverControlDropPendingFor(int sessionId)
{
    gPendingBySession.erase(sessionId);
}

void serverControlAdvancePending()
{
    // ►► MAP-GENERATION BOOKKEEPING RUNS FIRST, BEFORE THE EMPTY EARLY-RETURN.
    //
    // This used to sit below `if (empty()) return;`, which meant lastGeneration was
    // only ever refreshed on beats where a latch already existed. It therefore sat
    // at its initial 0 for the entire life of a server that had not yet seen an
    // interaction — and then the FIRST interaction anyone armed after a map load
    // was destroyed by a "the map changed" guard that had never actually observed a
    // map change. One sacrificial click per map, silently, with no log on any path.
    //
    // That is the long-running "I click the item / door ONCE and nothing happens,
    // I click again and it works" report: the second click worked only because the
    // first had already paid off the stale generation.
    //
    // Tracking it unconditionally keeps the guard honest — it now fires only on a
    // real generation CHANGE, which is what it was always meant to detect.
    static unsigned int lastGeneration = mapGetLoadGeneration();
    unsigned int generation = mapGetLoadGeneration();
    bool mapChanged = (generation != lastGeneration);
    lastGeneration = generation;

    if (gPendingBySession.empty()) {
        return;
    }

    // Combat entry cancels EVERY session's intent (vanilla animationStop clears
    // walks on combat start, combat.cc). The in-combat verbs take over from here.
    if (isInCombat()) {
        fprintf(stderr, "f2_server: interact latches CLEARED (%zu) — combat started\n",
            gPendingBySession.size());
        gPendingBySession.clear();
        return;
    }

    // A map change invalidates every latch: the target ids belong to a world that
    // no longer exists (MP_PROPOSAL Ch 14.2). The per-latch id+pid re-validation
    // below would drop them one beat later anyway; dropping them here means no
    // latch ever gets one beat to act on a freshly loaded map.
    if (mapChanged) {
        fprintf(stderr, "f2_server: interact latches CLEARED (%zu) — map generation %u->%u\n",
            gPendingBySession.size(), lastGeneration, generation);
        gPendingBySession.clear();
        return;
    }

    // Collect first, mutate after: interactionFire runs real engine outcomes that
    // can re-enter this plane (a script may arm or drop a latch), and mutating the
    // map mid-iteration would invalidate the iterator under us.
    std::vector<int> sessions;
    sessions.reserve(gPendingBySession.size());
    for (const auto& entry : gPendingBySession) {
        sessions.push_back(entry.first);
    }

    for (int sessionId : sessions) {
        auto it = gPendingBySession.find(sessionId);
        if (it == gPendingBySession.end()) {
            continue; // dropped by an earlier entry's fire
        }
        PendingInteraction pending = it->second; // by value: the map may move

        // Re-resolve the actor every poll, never cache it: the session may have
        // dropped (binding released) since the latch was armed.
        Object* actor = serverControlActorForSession(sessionId);
        Object* target = objectFindByNetId(pending.targetNetId);
        if (actor == nullptr || critterIsDead(actor)
            || target == nullptr || target->elevation != actor->elevation
            || target->id != pending.targetId || target->pid != pending.targetPid) {
            // Drop if: the session unbound; the actor died; the target is gone; it
            // moved elevation; or the netId now resolves to a DIFFERENT object (a
            // rebaseline/map-change re-minted netIds — the id+pid snapshot mismatch
            // catches the reshuffle so we never fire on the wrong object).
            //
            // NO LONGER SILENT. This used to erase without a word, which made it
            // indistinguishable from "the latch fired and the outcome did nothing" —
            // the exact ambiguity behind the "walks to the item and doesn't pick it
            // up, second press works" report. Say WHICH condition tripped.
            fprintf(stderr,
                "f2_server: interact verb=%d netId=%d DROPPED silently-invalid"
                " (actor=%s dead=%d target=%s elevMismatch=%d idNow=%d/%d idWas=%d/%d)\n",
                pending.verb, pending.targetNetId,
                actor != nullptr ? "ok" : "gone",
                actor != nullptr && critterIsDead(actor) ? 1 : 0,
                target != nullptr ? "ok" : "gone",
                (actor != nullptr && target != nullptr && target->elevation != actor->elevation) ? 1 : 0,
                target != nullptr ? target->id : -1, target != nullptr ? target->pid : -1,
                pending.targetId, pending.targetPid);
            gPendingBySession.erase(sessionId);
            continue;
        }

        // The outcome must run AS the acting player: interactionFire reaches
        // _obj_use_door / actionPickUp / skill use, whose callee trees read gDude
        // for source_obj, PC branches and message voice (Ch 7.2).
        ServerActorScope scope(actor);

        // ►► PER-BEAT LATCH TRACE. The latch has twice now armed and then produced
        // NO outcome and NONE of the three drop logs below — which is only possible
        // if this loop is not reaching the decision, or the decision keeps saying
        // "wait". Print the actual inputs every beat so the next repro names it
        // instead of costing another session. F2_TRACE_EVENTS-gated: an armed latch
        // lives at most kInteractionBeatsCap beats, so this is bounded.
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "f2_server: [latch] verb=%d netId=%d actorTile=%d targetTile=%d"
                            " dist=%d walkInFlight=%d beatsLeft=%d\n",
                pending.verb, pending.targetNetId, actor->tile, target->tile,
                objectGetDistanceBetween(actor, target),
                serverAnimWalkInFlightFor(actor) ? 1 : 0, pending.beatsLeft);
        }

        if (interactionRuleSatisfied(pending.verb, actor, target)) {
            gPendingBySession.erase(sessionId); // CONSUME before firing (re-entrancy, §2.5)
            interactionEmitGesture(pending.verb, actor, target);
            interactionFire(pending.verb, actor, target, pending.arg);
            continue;
        }
        if (!serverAnimWalkInFlightFor(actor)) {
            // Walk finished (or never pathed) without reaching the target.
            //
            // This is the "press pickup, he walks over, nothing happens, press
            // again and it works" case: the approach ended at distance > 1, the
            // latch drops, and the SECOND press fires immediately because the
            // actor is now close enough. Log the geometry so the next occurrence
            // says WHY — stopped one tile too far, never pathed at all (steps=0
            // from _make_path, e.g. an unreachable tile), or arrived a beat after
            // the walk was already reported finished.
            fprintf(stderr,
                "f2_server: interact verb=%d netId=%d DROPPED walk-ended-short"
                " (actor tile=%d, target tile=%d, dist=%d, beatsLeft=%d)\n",
                pending.verb, pending.targetNetId, actor->tile, target->tile,
                objectGetDistanceBetween(actor, target), pending.beatsLeft);
            gPendingBySession.erase(sessionId);
            interactionCannotGetThere(actor);
            continue;
        }
        if (--it->second.beatsLeft <= 0) {
            fprintf(stderr,
                "f2_server: interact verb=%d netId=%d DROPPED beats-exhausted"
                " (actor tile=%d, target tile=%d, dist=%d)\n",
                pending.verb, pending.targetNetId, actor->tile, target->tile,
                objectGetDistanceBetween(actor, target));
            gPendingBySession.erase(sessionId);
            interactionCannotGetThere(actor);
        }
    }
}

// Move the authoritative actor to an absolute tile. Actor-parameterized on
// purpose: the *binding* resolves to gDude for v1, but the executor never
// hardwires it. Mirrors command.cc's walkto/walk body exactly — the same public
// animationRegister* entry points, so the smooth-walk engine (server_anim.cc)
// and the instant scheduler both drive it unchanged. Out-of-combat only.
static void serverControlMove(Object* actor, int tile, bool run)
{
    if (actor == nullptr) {
        return;
    }
    if (isInCombat()) {
        fprintf(stderr, "f2_server: control mv dropped (in combat)\n");
        // ►► THE MOST VALUABLE REFUSAL ON THE LIST. This exact drop, x20, IS the
        // 62s combat-state desync's visible half: the client believed combat was
        // over and kept sending out-of-combat moves while the server sat on the
        // dude's turn, and the player saw only a character who would not walk.
        // Saying it out loud turns a silent stalemate into a report — and tells
        // the player which of the two views of the world they are looking at.
        serverControlRefuseActor(actor, "You are in combat.");
        return;
    }

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    if (run) {
        animationRegisterRunToTile(actor, tile, actor->elevation, -1, 0);
    } else {
        animationRegisterMoveToTile(actor, tile, actor->elevation, -1, 0);
    }
    reg_anim_end();
    fprintf(stderr, "f2_server: control mv tile=%d run=%d\n", tile, run ? 1 : 0);
}

bool serverControlHasClaimant()
{
    return serverControlAnyBound();
}

// Slots whose owning session dropped and whose BODY is waiting to be parked
// off-map. Enqueued where the disconnect is detected (serverControlBeginDrain
// — which the modal pumps also call), drained only from the serve loop's main
// phase (serverControlDrainPresence): a body must not leave the world while a
// dialog/barter barrier holds raw pointers, nor mid-combat (the roster
// iterates it). An EXPLICIT queue, not a "unbound → park" scan, on purpose:
// pre-spawned F2_SERVER_PLAYERS bodies are unbound-by-birth and must keep
// standing (claimable premades, and the control/wire gates claim them).
static std::vector<int> gPendingDespawns;

void serverControlBeginDrain(const std::function<bool(int)>& liveSession)
{
    // Release bindings whose owner has dropped. Done here (not in serverControlLine)
    // because pollInbound's onLine must not observe the client set — and doing it
    // BEFORE this beat's lines means a session that dropped last beat frees its
    // slot in time for a new claimant this beat.
    bool changed = false;
    for (int slot = 0; slot < playerActorCount(); slot++) {
        int sessionId = gBindings[slot];
        if (sessionId != kNoSessionId && liveSession && !liveSession(sessionId)) {
            fprintf(stderr, "f2_server: control slot %d released (session %d gone)\n",
                slot, sessionId);

            // Announce the departure to everyone still here. The counterpart of the
            // "%s joined the game." broadcast at the tail of a successful claim —
            // joins were announced and leaves were not, so from inside the game
            // people only ever arrived. Read the name BEFORE clearing the binding;
            // the ACTOR outlives the session (only the binding is released), so the
            // body is still there and still named.
            Object* leaver = playerActorAt(slot);
            if (leaver != nullptr) {
                char line[128];
                const char* who = critterGetName(leaver);
                snprintf(line, sizeof(line), "%s left the game.", who != nullptr ? who : "A player");
                presenter()->consoleMessageStyled(0, kMsgChannelSystem, line);
            }

            gBindings[slot] = kNoSessionId;
            gSessionPlatform.erase(sessionId); // the reported OS dies with the session
            serverControlDropPendingFor(sessionId); // the intent dies with its owner
            // Same for anything they queued for their combat turn. Their body's
            // turn is about to start auto-ending (the barrier waits per SLOT, and
            // this slot just went unbound); executing a dropped player's stale
            // orders when it does would be acting for someone who left.
            combatIntentDropForSlot(slot);
            gInventoryOpenSlots.erase(slot); // and any screen they had paid to open
            changed = true;

            // Queue the BODY for despawn — parked off-map at the next safe main
            // phase (serverControlDrainPresence), NOT here: this function also
            // runs inside the modal pumps. Slot 0 never despawns: the host body
            // anchors the passive sim (host-transfer is its own future feature).
            if (slot != 0) {
                bool queued = false;
                for (int pendingSlot : gPendingDespawns) {
                    if (pendingSlot == slot) {
                        queued = true;
                        break;
                    }
                }
                if (!queued) {
                    gPendingDespawns.push_back(slot);
                }
            }
        }
    }

    // Sweep inventory sessions that are no longer legitimate, and tell the viewer
    // to close the screen when one dies under it. Done here, once a beat, rather
    // than from a turn-end or combat-exit hook: a turn ends on several paths (the
    // idle deadline, the actor dying, a script terminating combat) and combat
    // ends on several more, and polling the resulting STATE catches all of them
    // without a bridge into f2_core for each.
    //
    // Holding the turn open while the screen is up (serverSlotInModal) is what
    // makes this rare — it is the backstop for the turn ending anyway, not the
    // normal close path, which is the player pressing ESC and sending invclose.
    if (!gInventoryOpenSlots.empty()) {
        if (!isInCombat()) {
            // Out of combat the screen is free and the verbs are ungated, so a
            // surviving session is just a leftover from the fight that ended. No
            // revoke: the player may keep browsing, it costs nothing now.
            gInventoryOpenSlots.clear();
        } else {
            for (auto it = gInventoryOpenSlots.begin(); it != gInventoryOpenSlots.end();) {
                Object* slotActor = playerActorAt(*it);
                if (slotActor != nullptr && _combat_whose_turn() == slotActor) {
                    ++it; // still their turn — the screen is legitimately open
                    continue;
                }
                if (slotActor != nullptr) {
                    fprintf(stderr, "f2_server: inventory session revoked (slot %d turn ended)\n", *it);
                    presenter()->inventoryRevoke(slotActor->netId);
                }
                it = gInventoryOpenSlots.erase(it);
            }
        }
    }

    // The ORPHANED BODY stays in the world exactly where it was — standing,
    // mid-combat, whatever. It just stops receiving intents; in combat its turns
    // auto-end (no bound session for that slot → the barrier does not wait), and
    // it remains a valid target and a claimable slot. A reconnecting player gets
    // a NEW sessionId (they are never reused) and resumes the same body, with its
    // inventory and HP intact, via `claim <slot>`.
    if (changed) {
        serverEmitPlayerRoster(); // viewers re-derive their binding immediately
    }

    gLineCounts.clear();
}

// The server's name, for anything it says as itself. F2_SERVER_NAME, read once —
// an operator renaming the box mid-session would only confuse the log. Unset is
// normal (solo, the goldens, every existing invocation), and the generic default
// keeps the greeting from reading like a misconfiguration.
static const char* serverDisplayName()
{
    static const char* name = [] {
        const char* env = getenv("F2_SERVER_NAME");
        return (env != nullptr && env[0] != '\0') ? env : "Fallout 2 dedicated server";
    }();
    return name;
}

// Greet the session that just claimed `slot`, and tell everyone else they have
// company. Server chrome, not world narration — hence kMsgChannelSystem.
//
// Addressed by the claimant's ACTOR netId rather than their sessionId because the
// wire has no per-session event channel (one encoder, one broadcast buffer); the
// address is the filter every viewer applies to decide the line is theirs. That is
// why this runs on a successful claim and not on accept: before a claim there is
// no actor, so there is no way to address anybody.
// What a successful claim/login DID to the player's character — surfaced to the
// joining client as a plain system line, so "which character did I get, and why"
// is never a mystery (the confusion that a returning account silently resuming its
// old body, or an ESC'd creation screen landing on the default premade, both look
// identical from the client). Client transparency only, not a wire contract.
enum class ClaimDisposition {
    kResumedExisting, // known account rebound to its saved body
    kCreatedNew,      // new account, a rolled `create` spec was applied
    kNewDefault,      // new account, NO spec (creation skipped) → default premade
    kClaimed,         // legacy bare `claim`, no account identity to explain
};

static void serverGreetClaimant(int slot, ClaimDisposition disposition)
{
    Object* actor = playerActorAt(slot);
    if (actor == nullptr) {
        return;
    }

    int netId = actor->netId;
    const char* who = critterGetName(actor);

    int online = 0;
    for (int i = 0; i < playerActorCount(); i++) {
        if (gBindings[i] != kNoSessionId) {
            online++;
        }
    }

    // This player's reported OS, as a " (Linux)"-style suffix (empty if unknown).
    char platSuffix[32];
    platSuffix[0] = '\0';
    int mySession = serverControlSessionForSlot(slot);
    auto platIt = mySession != kNoSessionId ? gSessionPlatform.find(mySession) : gSessionPlatform.end();
    if (platIt != gSessionPlatform.end() && !platIt->second.empty()) {
        snprintf(platSuffix, sizeof(platSuffix), " (%s)", platIt->second.c_str());
    }

    char line[512];

    snprintf(line, sizeof(line), "%s (server: %s)", serverDisplayName(), serverPlatformName());
    presenter()->consoleMessageStyled(netId, kMsgChannelSystem, line);

    snprintf(line, sizeof(line), "You are %s%s, slot %d. %d/%d players online.",
        who, platSuffix, slot, online, playerActorCount());
    presenter()->consoleMessageStyled(netId, kMsgChannelSystem, line);

    // Say WHERE this character came from — the whole point of the transparency ask.
    const char* sourceLine = nullptr;
    switch (disposition) {
    case ClaimDisposition::kResumedExisting:
        sourceLine = "Resumed your existing character.";
        break;
    case ClaimDisposition::kCreatedNew:
        sourceLine = "Created your new character from the stats you rolled.";
        break;
    case ClaimDisposition::kNewDefault:
        sourceLine = "No character was rolled - you joined as the default. "
                     "Reconnect under a new name (and finish the creation screen) to make your own.";
        break;
    case ClaimDisposition::kClaimed:
        break; // bare claim carries no account identity to explain
    }
    if (sourceLine != nullptr) {
        presenter()->consoleMessageStyled(netId, kMsgChannelSystem, sourceLine);
    }

    // The roster, one line, truncated rather than wrapped across several — the log
    // is 80 columns and a full lobby would otherwise push the greeting off the top.
    int used = snprintf(line, sizeof(line), "Online:");
    for (int i = 0; i < playerActorCount() && used < (int)sizeof(line) - 1; i++) {
        if (gBindings[i] == kNoSessionId) {
            continue;
        }
        Object* other = playerActorAt(i);
        used += snprintf(line + used, sizeof(line) - used, " %s%s",
            other != nullptr ? critterGetName(other) : "?",
            i == slot ? " (you)" : "");
    }
    presenter()->consoleMessageStyled(netId, kMsgChannelSystem, line);

    // And announce the arrival to the players already here. Broadcast (netId 0) —
    // the joiner sees their own greeting above and does not need this too, but the
    // wire cannot exclude one viewer, so accept the duplicate rather than invent a
    // negative address for one cosmetic line.
    if (online > 1) {
        snprintf(line, sizeof(line), "%s joined the game%s.", who, platSuffix);
        presenter()->consoleMessageStyled(0, kMsgChannelSystem, line);
    }
}

// F2_SERVER_HOST=<account name>: RESERVE slot 0 for that account.
//
// Slot 0 is the host body and host-only screens are gated on it, so with no
// reservation the answer to "who can travel the worldmap" is "whoever finished
// character creation first" — a race between humans, different every run. Naming
// the host makes it deterministic without committing to anything: the real fix is
// a transferable host ROLE, which is owed anyway for "the host dies and nobody can
// travel" ([[coop-v1-elephants]]).
//
// Unset = first-come-first-served, the private-box default.
static const char* serverHostAccountName()
{
    static const char* name = getenv("F2_SERVER_HOST");
    return name != nullptr && name[0] != '\0' ? name : nullptr;
}

// ---- SPAWN-AT-LOGIN (ACCOUNT_IDENTITY_DESIGN.md §3) ----
//
// A login by an unknown name with no free pre-spawned slot needs a NEW actor,
// which is a registry mutation. The verb therefore only LATCHES the intent here;
// the spawn happens in serverControlDrainPendingLogins, which the serve loop
// calls from its MAIN-phase intent drain only.
//
// ►► That call-site restriction IS the barrier guard. The dialog / movie / barter
// pumps also service inbound lines (they call serverControlLine), but they never
// call the drain — so a login arriving mid-conversation latches and waits instead
// of re-minting netIds under a barrier that is holding raw pointers
// (gGameDialogSpeaker is the documented casualty). Combat is refused explicitly
// below for the same class of reason.
struct PendingLogin {
    int sessionId;
    char name[kAccountNameMaxLength];
    char token[kAccountTokenMaxLength];
    bool hasCreateSpec;
    PlayerCreateSpec createSpec;
};

static std::vector<PendingLogin> gPendingLogins;

// Character-creation specs supplied by `create`, held per SESSION until that
// session's `login` spawns a NEW account (ACCOUNT_IDENTITY_DESIGN.md stage 2).
// Kept separate from the login latch because the two verbs arrive as separate
// lines: `create ...` states who you want to be, `login <name>` commits it.
//
// An EXISTING account ignores any spec — you do not re-roll a character by
// reconnecting, and silently overwriting a saved character would be the worst
// possible default.
static std::unordered_map<int, PlayerCreateSpec> gPendingCreateSpecs;

// Slots spawned in a PREVIOUS beat that are still owed their greeting. The
// greeting is addressed by the actor's netId, and a freshly spawned actor does
// not have its final netId until the rebaseline at that beat's TAIL re-mints the
// world — so greeting in the same breath as the spawn would address an id that is
// about to change. One beat late, correctly addressed.
static std::vector<std::pair<int, ClaimDisposition>> gPendingGreets;

void serverControlDrainPendingLogins()
{
    // Greets owed from an earlier beat: the rebaseline has happened, netIds are
    // final, so these can now be addressed.
    for (const auto& pending : gPendingGreets) {
        serverGreetClaimant(pending.first, pending.second);
    }
    gPendingGreets.clear();

    if (gPendingLogins.empty()) {
        return;
    }

    // Never grow the world mid-fight: the combat machine holds a turn order and
    // an initiative roster that a new body would not be in, and a rebaseline
    // during combat already costs every client its combat mirror (server_loop.cc).
    // The intent keeps waiting — the player joins when the fight ends.
    if (isInCombat()) {
        return;
    }

    std::vector<PendingLogin> pending;
    pending.swap(gPendingLogins);

    for (const PendingLogin& p : pending) {
        // The session may have dropped while the intent waited.
        if (serverControlSlotForSession(p.sessionId) >= 0) {
            continue; // already bound by some other path
        }

        // Re-check by name: two logins for the SAME new name could latch in one
        // beat, and a second spawn would fork the account across two slots.
        int slot = accountSlotForName(p.name);
        if (slot >= 0 && playerActorAt(slot) == nullptr) {
            slot = -1; // table/registry disagree — see the same guard in `login`
        }
        if (slot >= 0) {
            if (gBindings[slot] == kNoSessionId) {
                gBindings[slot] = p.sessionId;
                fprintf(stderr, "f2_server: control claimed by session %d (slot %d)\n",
                    p.sessionId, slot);
                gPendingGreets.push_back({ slot,
                    p.hasCreateSpec ? ClaimDisposition::kCreatedNew : ClaimDisposition::kNewDefault });
            }
            continue;
        }

        slot = playerActorCount();
        if (slot >= kMaxPlayerActors) {
            fprintf(stderr, "f2_server: login '%s' from session %d denied (registry full, %d max)\n",
                p.name, p.sessionId, kMaxPlayerActors);
            continue;
        }

        // ⚠ PER-SLOT seed, never the bulk seeders — those rewrite EVERY extra's
        // sheet from the host and would silently reset the progression of every
        // player already connected (trap 1). Seeded BEFORE the body exists so no
        // window opens where an actor's sheet pid resolves to an unseeded row.
        playerActorSeedSheetFromHost(slot);

        if (serverSpawnPlayerActor(slot) != slot) {
            fprintf(stderr, "f2_server: login '%s' from session %d FAILED (spawn error)\n",
                p.name, p.sessionId);
            continue;
        }

        // A CREATED character replaces the host-seeded sheet with its own SPECIAL
        // / tags / traits. Applied after the body exists (it needs the actor to
        // resolve its row and to recompute derived stats + HP) and before the
        // rebaseline, so the first blob every viewer loads already carries the
        // real character rather than a clone that changes a beat later.
        //
        // No spec = today's behaviour, a clone of the host. That is the owner's
        // ruling that creation is explicit intent and never a default.
        if (p.hasCreateSpec && playerCreateApply(slot, &p.createSpec) != 0) {
            // The sheet is now a half-reset row, so this actor must not go live.
            fprintf(stderr, "f2_server: login '%s' — character creation FAILED, actor dropped\n",
                p.name);
            continue;
        }

        accountAssign(slot, p.name, p.token[0] != '\0' ? p.token : nullptr);
        critterSetNameForSlot(slot, p.name);
        gBindings[slot] = p.sessionId;

        // Bound BEFORE the rebaseline is requested, so the roster the baseline
        // emits at this beat's tail (serverEmitBaseline -> serverEmitPlayerRoster)
        // already carries this binding against the freshly minted netIds. The
        // greeting waits for the next beat (gPendingGreets).
        serverRequestRebaseline();
        gPendingGreets.push_back({ slot,
            p.hasCreateSpec ? ClaimDisposition::kCreatedNew : ClaimDisposition::kNewDefault });

        fprintf(stderr, "f2_server: account '%s' -> slot %d SPAWNED (registry now %d)\n",
            p.name, slot, playerActorCount());
        // "control claimed by session" is the greppable prefix the gate scripts
        // match — emit it on this path too.
        fprintf(stderr, "f2_server: control claimed by session %d (slot %d)\n",
            p.sessionId, slot);
    }
}

// Reconcile body PRESENCE with session bindings, at the same safe point the
// login drain runs (main phase only — see gPendingDespawns for why). Two
// directions:
//   park:     a queued slot still unbound → _obj_disconnect the body. It stays
//             valid off-map (tile -1, sheet + inventory intact, registry
//             untouched — slots never shrink) until its account returns.
//   reattach: a slot that is bound but offline (the owner logged back in, or
//             a save taken while they were away was just restored and they
//             claimed) → put the body back beside the host.
// Either direction requests a rebaseline: the baseline skips offline bodies,
// so the rebaseline IS the viewer-facing add/remove announcement — no new wire
// event, no netId special-casing.
void serverControlDrainPresence()
{
    // Never mutate the world mid-fight (the combat roster iterates player
    // bodies) or while a map transition is latched for this beat's tail. The
    // queue and the bound-but-offline state both just wait for the next beat.
    if (isInCombat() || mapTransitionPending()) {
        return;
    }

    bool changed = false;

    if (!gPendingDespawns.empty()) {
        std::vector<int> pending;
        pending.swap(gPendingDespawns);
        for (int slot : pending) {
            if (gBindings[slot] != kNoSessionId) {
                continue; // reconnected while the despawn waited — body stays
            }
            Object* actor = playerActorAt(slot);
            if (actor == nullptr || !playerActorOnline(slot)) {
                continue;
            }
            _obj_disconnect(actor, nullptr);
            playerActorSetOnline(slot, false);
            changed = true;
            fprintf(stderr, "f2_server: slot %d body parked (owner disconnected)\n", slot);
        }
    }

    for (int slot = 1; slot < playerActorCount(); slot++) {
        if (gBindings[slot] == kNoSessionId || playerActorOnline(slot)) {
            continue;
        }
        Object* actor = playerActorAt(slot);
        if (actor == nullptr || gDude == nullptr) {
            continue;
        }

        int tile = playerActorFindFreeTileNear(gDude->tile, gDude->elevation);
        if (tile == -1) {
            tile = gDude->tile; // co-locate rather than strand (map.cc's rule)
        }
        // objectReattach handles both off-map list states (freshly parked vs
        // restored-from-save) — see its comment in object.cc.
        if (objectReattach(actor, tile, gDude->elevation) != 0) {
            fprintf(stderr, "f2_server: slot %d body reattach FAILED (tile %d)\n", slot, tile);
            continue;
        }
        objectSetRotation(actor, gDude->rotation, nullptr);
        playerActorSetOnline(slot, true);
        changed = true;
        fprintf(stderr, "f2_server: slot %d body reattached (owner returned)\n", slot);
    }

    if (changed) {
        serverRequestRebaseline();
    }
}

void serverControlLine(int sessionId, const char* line)
{
    if (++gLineCounts[sessionId] > kMaxLinesPerBeat) {
        return; // flood guard: silently ignore lines past the per-beat cap
    }

    char verb[32];
    verb[0] = '\0';
    int arg = 0;
    int arg2 = 0;
    int arg3 = 0;
    int n = sscanf(line, "%31s %d %d %d", verb, &arg, &arg2, &arg3);
    if (n < 1 || verb[0] == '\0') {
        return;
    }

    if (strcmp(verb, "movdone") == 0) {
        // A viewer finished or skipped the movie it was shown. FIRST ACK RELEASES
        // EVERYONE (game_movie.h) — so this is answered before the claimant gate on
        // purpose: a SPECTATOR watching the cutscene may also end it, and more to
        // the point, gating it on a claim would let an unclaimed viewer's ack vanish
        // and leave the barrier waiting on someone who already pressed escape.
        gameMovieAck();
        return;
    }

    if (strcmp(verb, "platform") == 0) {
        // `platform <name>` — the client's OS, remembered per session for the join
        // greeting. One word; anything odd is just ignored.
        char plat[24];
        plat[0] = '\0';
        if (sscanf(line, "%*s %23s", plat) == 1 && plat[0] != '\0') {
            gSessionPlatform[sessionId] = plat;
        }
        return;
    }

    if (strcmp(verb, "create") == 0) {
        // `create <S> <P> <E> <C> <I> <A> <L> [tag1 tag2 tag3] [trait1 trait2]`
        // — the character this session wants to BE. Held until `login <name>`
        // commits it, and only honoured when that name is NEW (below).
        //
        // Parsed here rather than via the top-level sscanf, which only captures
        // three ints. Missing trailing fields keep their defaults, so a client
        // may send SPECIAL alone.
        PlayerCreateSpec spec;
        playerCreateSpecDefaults(&spec);

        int v[12];
        for (int i = 0; i < 12; i++) {
            v[i] = -1;
        }

        int n2 = sscanf(line, "%*s %d %d %d %d %d %d %d %d %d %d %d %d",
            &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
            &v[7], &v[8], &v[9], &v[10], &v[11]);
        if (n2 < 7) {
            fprintf(stderr, "f2_server: create from session %d rejected "
                            "(need 7 SPECIAL values, got %d)\n",
                sessionId, n2 < 0 ? 0 : n2);
            return;
        }

        for (int i = 0; i < 7; i++) {
            spec.special[i] = v[i];
        }
        for (int i = 0; i < 3 && 7 + i < n2; i++) {
            spec.tagged[i] = v[7 + i];
        }
        for (int i = 0; i < TRAITS_MAX_SELECTED_COUNT && 10 + i < n2; i++) {
            spec.traits[i] = v[10 + i];
        }

        // Validate NOW so the client is told immediately, rather than discovering
        // at spawn time that its character was silently rejected.
        if (!playerCreateSpecValidate(&spec)) {
            fprintf(stderr, "f2_server: create from session %d rejected (invalid spec)\n", sessionId);
            return;
        }

        gPendingCreateSpecs[sessionId] = spec;
        fprintf(stderr, "f2_server: create spec held for session %d "
                        "(S%d P%d E%d C%d I%d A%d L%d)\n",
            sessionId, spec.special[0], spec.special[1], spec.special[2],
            spec.special[3], spec.special[4], spec.special[5], spec.special[6]);
        return;
    }

    if (strcmp(verb, "login") == 0) {
        // `login <name> [token]` — bind by ACCOUNT NAME instead of slot index
        // (ACCOUNT_IDENTITY_DESIGN.md). The name is the DURABLE identity a
        // returning player re-binds by; the slot is an internal handle. Kept a
        // SEPARATE verb from `claim` on purpose: the top-level sscanf grabs args
        // as ints, so `claim bob` would silently alias to bare `claim` and bind
        // an arbitrary slot while looking successful (trap 5).
        //
        // STAGE 0: binds over the PRE-SPAWNED slots (F2_SERVER_PLAYERS). A new
        // name takes the next unowned/unbound slot; dynamic spawn-at-login is
        // stage 1. Never slot 0 — that is the host.
        char name[kAccountNameMaxLength];
        char token[kAccountTokenMaxLength];
        name[0] = '\0';
        token[0] = '\0';
        // %*s skips the verb; the args are strings the top-level parse missed.
        if (sscanf(line, "%*s %31s %63s", name, token) < 1 || name[0] == '\0') {
            fprintf(stderr, "f2_server: login from session %d rejected (no name)\n", sessionId);
            return;
        }

        int held = serverControlSlotForSession(sessionId);
        if (held >= 0) {
            fprintf(stderr, "f2_server: session %d already holds slot %d\n", sessionId, held);
            return;
        }

        int slot = accountSlotForName(name);
        // A name owning a slot the registry cannot resolve means the account table
        // and the registry disagree (they are written and restored together, so
        // this should be impossible). Treat it as unknown rather than binding a
        // session to a null actor, which reads as "all my verbs are ignored".
        if (slot >= 0 && playerActorAt(slot) == nullptr) {
            fprintf(stderr, "f2_server: login '%s' — account owns slot %d but no actor there\n",
                name, slot);
            slot = -1;
        }
        const bool newAccount = slot < 0;
        ClaimDisposition disp = ClaimDisposition::kResumedExisting;

        if (!newAccount) {
            // Token check is COMPARISON-gated by policy: enforced only under
            // F2_REQUIRE_TOKEN (default OFF = first-claimer-wins, the private-box
            // default — ACCOUNT_IDENTITY_DESIGN.md §4).
            static const bool requireToken = getenv("F2_REQUIRE_TOKEN") != nullptr;
            if (requireToken && !accountTokenMatches(slot, token)) {
                fprintf(stderr, "f2_server: login '%s' from session %d rejected (bad token)\n",
                    name, sessionId);
                return;
            }
            if (gBindings[slot] != kNoSessionId) {
                fprintf(stderr, "f2_server: login '%s' from session %d denied (slot %d already driven)\n",
                    name, sessionId, slot);
                return;
            }

            // An account's slot is PERMANENT — the sheet pid encodes it, so slots are
            // never reordered or recycled. F2_SERVER_HOST therefore cannot move an
            // account that already owns one, which matters for worlds saved before
            // slot 0 became bindable by name: the named host is back on the slot it
            // had, and slot 0 stays an unpiloted premade body with nobody able to
            // travel. Say it out loud, because the symptom (worldmap refuses) is a
            // long way from the cause (this save predates the rule).
            const char* hostName = serverHostAccountName();
            if (hostName != nullptr && slot != 0 && compat_stricmp(name, hostName) == 0) {
                fprintf(stderr, "f2_server: F2_SERVER_HOST='%s' but that account already owns slot %d"
                                " (slots are permanent) — slot 0 stays unbound; start a fresh world"
                                " if you want this account hosting\n",
                    name, slot);
            }
        } else {
            // ►► SLOT 0 IS IN THE SCAN, and that is the point. Slot 0 is the host
            // body, and host-only screens (worldmap travel above all) are gated on
            // it — with nobody bound there, NO ONE can travel the map. So the first
            // player to log in takes the host slot and the registry stays exactly as
            // large as the player count; only the second player onward spawns a new
            // actor. Slot order is preference order, same as bare `claim`.
            //
            // ...UNLESS a host account is named, in which case slot 0 is RESERVED
            // for it. Otherwise "who is host" — i.e. who can drive the worldmap — is
            // settled by a race between humans at a character-creation screen, and
            // shifts from run to run. A skipped slot 0 sends this player down the
            // spawn path instead, which is exactly the intended wait.
            disp = ClaimDisposition::kNewDefault;
            const char* hostName = serverHostAccountName();
            const bool wantsHostSlot = hostName != nullptr && compat_stricmp(name, hostName) == 0;

            slot = -1;
            for (int s = 0; s < playerActorCount(); s++) {
                if (s == 0 && hostName != nullptr && !wantsHostSlot) {
                    continue;
                }
                if (!accountSlotOwned(s) && gBindings[s] == kNoSessionId) {
                    slot = s;
                    break;
                }
            }

            // The reservation is a PREFERENCE, not a guarantee: a restored save may
            // already have handed slot 0 to somebody else, and refusing the login
            // over it would lock the named host out of their own server. Say so and
            // carry on — the operator can see who actually holds the host slot in
            // the greeting roster.
            if (wantsHostSlot && slot != 0) {
                fprintf(stderr, "f2_server: host account '%s' wanted slot 0 but it is taken"
                                " — taking %s instead\n",
                    name, slot > 0 ? "another slot" : "a newly spawned slot");
            }
            if (slot < 0) {
                // No pre-spawned slot free → this name needs a NEW actor. LATCH it;
                // the spawn runs in the main-phase drain, never here (a login can
                // arrive inside the dialog/movie/barter pump, where re-minting
                // netIds would pull the rug out from under a live barrier).
                if (playerActorCount() >= kMaxPlayerActors) {
                    fprintf(stderr, "f2_server: login '%s' from session %d denied (registry full, %d max)\n",
                        name, sessionId, kMaxPlayerActors);
                    return;
                }

                for (const PendingLogin& p : gPendingLogins) {
                    if (p.sessionId == sessionId) {
                        return; // already queued; a resend must not spawn twice
                    }
                }

                PendingLogin p;
                p.sessionId = sessionId;
                strncpy(p.name, name, kAccountNameMaxLength - 1);
                p.name[kAccountNameMaxLength - 1] = '\0';
                strncpy(p.token, token, kAccountTokenMaxLength - 1);
                p.token[kAccountTokenMaxLength - 1] = '\0';

                // Carry this session's `create` spec, if it sent one. Consumed
                // here so a second login cannot re-apply it to another character.
                auto it = gPendingCreateSpecs.find(sessionId);
                p.hasCreateSpec = it != gPendingCreateSpecs.end();
                if (p.hasCreateSpec) {
                    p.createSpec = it->second;
                    gPendingCreateSpecs.erase(it);
                }

                gPendingLogins.push_back(p);

                fprintf(stderr, "f2_server: login '%s' from session %d queued (spawn pending)\n",
                    name, sessionId);
                return;
            }
            accountAssign(slot, name, token[0] != '\0' ? token : nullptr);
            // The character in-game name starts as the account name — otherwise the
            // clone shell greets and fights as the HOST's name. They may diverge
            // once the creation screen (stage 2) supplies a distinct one.
            critterSetNameForSlot(slot, name);

            // A new account taking an EXISTING body still gets the character it
            // asked for. The spawn path in the drain applies this too; without it
            // here, the very first player — who now lands on slot 0, an existing
            // body — would have its `create` spec silently dropped and join as the
            // premade. Consumed either way so a second login cannot re-apply it.
            //
            // Safe to run inline (unlike the SPAWN, which is why that one latches):
            // this writes a proto row, it does not mutate the registry or re-mint a
            // netId, so no barrier is holding a pointer this could invalidate.
            auto it = gPendingCreateSpecs.find(sessionId);
            if (it != gPendingCreateSpecs.end()) {
                PlayerCreateSpec spec = it->second;
                gPendingCreateSpecs.erase(it);
                if (playerCreateApply(slot, &spec) != 0) {
                    fprintf(stderr, "f2_server: login '%s' — character creation FAILED (slot %d)\n",
                        name, slot);
                } else {
                    // The sheet rides the join blob, so every viewer's mirror —
                    // including this one's, loaded a moment ago — is now stale.
                    serverRequestRebaseline();
                    disp = ClaimDisposition::kCreatedNew;
                }
            }

            fprintf(stderr, "f2_server: account '%s' -> slot %d/%d (new)\n",
                name, slot, playerActorCount() - 1);
        }

        gBindings[slot] = sessionId;
        // "control claimed by session" is grepped by scripts/check_control.sh and
        // scripts/check_wire_combat.sh — keep that prefix verbatim on this path too.
        fprintf(stderr, "f2_server: control claimed by session %d (slot %d)\n", sessionId, slot);
        serverEmitPlayerRoster();
        serverGreetClaimant(slot, disp);
        return;
    }

    if (strcmp(verb, "claim") == 0) {
        // (greeting is emitted at the tail of a SUCCESSFUL claim, below)
        // `claim`        — take the first free slot, host slot preferred (slot
        //                  order IS preference order). Idempotent: a session that
        //                  already holds a slot just re-reports it.
        // `claim <slot>` — take that specific slot if free. This is the RECONNECT
        //                  affordance: same human, new sessionId, same body.
        //
        // A dead actor's slot stays claimable — what a dead actor may DO is the
        // death-policy seam's business (Ch 9), not the binding layer's.
        int held = serverControlSlotForSession(sessionId);
        if (held >= 0) {
            fprintf(stderr, "f2_server: session %d already holds slot %d\n", sessionId, held);
            return;
        }

        int want = -1;
        if (n >= 2) {
            if (arg < 0 || arg >= playerActorCount()) {
                fprintf(stderr, "f2_server: control claim slot %d out of range (0..%d)\n",
                    arg, playerActorCount() - 1);
                return;
            }
            want = gBindings[arg] == kNoSessionId ? arg : -1;
        } else {
            for (int slot = 0; slot < playerActorCount(); slot++) {
                if (gBindings[slot] == kNoSessionId) {
                    want = slot;
                    break;
                }
            }
        }

        if (want < 0) {
            // Denied: the session stays a spectator. It keeps receiving the whole
            // stream and may claim later when a slot frees.
            fprintf(stderr, "f2_server: control claim from session %d denied (no free actor)\n",
                sessionId);
            return;
        }

        gBindings[want] = sessionId;
        // "control claimed by session" is grepped by scripts/check_control.sh
        // and scripts/check_wire_combat.sh — keep that prefix verbatim.
        fprintf(stderr, "f2_server: control claimed by session %d (slot %d)\n", sessionId, want);
        serverEmitPlayerRoster(); // every viewer re-derives which actor is its own
        serverGreetClaimant(want, ClaimDisposition::kClaimed);
        return;
    }

    // Everything past this point acts on the SESSION'S actor. An unbound session
    // (spectator, or one whose claim was denied) is rejected once, here, replacing
    // the per-verb `sessionId != gClaimant` gates.
    Object* actor = serverControlActorForSession(sessionId);
    if (actor == nullptr) {
        fprintf(stderr, "f2_server: control %s from unbound session %d ignored\n", verb, sessionId);
        return;
    }

    // A DEAD player actor takes no actions (owner ruling 2026-07-20). Until now a
    // corpse could be walked around, made to attack, and emptied by its own owner —
    // and a body strolling away from where it died is not just absurd, it is a
    // desync engine: everyone else's mirror has a corpse, and corpses are loot
    // targets whose position other players' adjacency checks depend on.
    //
    // VIEWING stays allowed on purpose. `invopen` and `look` are read-only, and a
    // dead player watching the fight through their own inventory is harmless (and
    // is how you see what you are about to lose). Everything that MUTATES — moving,
    // fighting, equipping, dropping, using, looting, interacting — is refused here,
    // at the one choke point every verb passes, rather than per-verb where the next
    // verb added would forget it.
    //
    // NOTE this is deliberately NOT `!critterIsActive` — knocked-out is temporary
    // and vanilla lets you act again on standing up; only death is permanent.
    if (critterIsDead(actor)) {
        bool readOnlyVerb = strcmp(verb, "invopen") == 0
            || strcmp(verb, "invclose") == 0
            || strcmp(verb, "look") == 0
            || strcmp(verb, "cancel") == 0; // aborting your own pending walk is not an action
        // (`claim` needs no entry — it is answered above, before an actor is resolved.)
        if (!readOnlyVerb) {
            fprintf(stderr, "f2_server: control %s dropped (actor is dead, session %d)\n",
                verb, sessionId);
            serverControlRefuse(sessionId, "You are dead.");
            return;
        }
    }

    // gDude := actor for the whole verb. The handlers below still read gDude in
    // places, and so does the entire callee tree beneath them (skill branches,
    // item-use script source_obj, "You…" message selection) — the scope is what
    // makes those reads mean "the acting player" (MP_PROPOSAL.md Ch 7.2).
    ServerActorScope scope(actor);

    if (strcmp(verb, "mv") == 0) {
        // tileFromScreenXY on the client can yield -1 out of bounds; reject any
        // tile the walk path would not tolerate (same bound walkto assumes).
        if (n < 2 || !tileIsValid(arg)) {
            fprintf(stderr, "f2_server: control mv bad tile=%d (grid=%d) ignored\n",
                n >= 2 ? arg : -1, gHexGridSize);
            serverControlRefuse(sessionId, "You can't go there.");
            return;
        }
        // A bare move cancels any pending interaction — "walk away" drops the
        // walk-then-act intent (INTERACTION_UX_DESIGN.md §2.3). serverControlMove
        // re-registers a walk to the clicked tile, which also replaces the approach
        // walk in the registry by destTile.
        serverControlSupersedePending(sessionId, "a move order");
        serverControlMove(actor, arg, arg2 != 0);
        return;
    }

    // -- Out-of-combat interaction verbs (INTERACTION_VERBS_PLAN.md) --------
    // The viewer action menu routes a picked verb + clicked-object netId here.
    // Same execution model as mv: claimant-gated, out-of-combat, run the REAL
    // engine action DIRECTLY at this pre-advance drain site (the safe site mv
    // uses), and let the authoritative result ride OBJECT_DELTA / presentation
    // events. Targets are addressed by wire netId (obj->id is non-unique). v0
    // does NOT walk-to-then-act (no adjacency/locomotion) — the accepted
    // equivalence class for the serverLoopActive action decouples (doors/pickup/
    // climb). In-combat interaction (enqueue an intent like the combat verbs) is
    // a later slice.
    // Break the current walk-then-act intent: drop the pending outcome AND stop the
    // in-flight approach where it stands (reg_anim_clear cancels the stepped walk,
    // server_anim.cc). This is the explicit "abort before I reach the door" the
    // last-writer-wins mv/verb replacement gives implicitly — a first-class cancel
    // so the viewer can bind it to a key/right-click without having to fake a move.
    if (strcmp(verb, "cancel") == 0) {
        serverControlSupersedePending(sessionId, "an explicit cancel");
        if (actor != nullptr && !isInCombat()) {
            reg_anim_clear(actor);
        }
        fprintf(stderr, "f2_server: control cancel\n");
        return;
    }

    // -- Dialog choice verbs (A2, DIALOG_STREAMING_PLAN) --------------------
    // dsay <index> / dend route the viewer's picked reply through the trust
    // boundary into the dialog intent queue, which the server-side _gdProcess
    // block-and-pump barrier drains (game_dialog.cc). Three gates:
    //   * claimant  — only the controlling client may drive.
    //   * dialog-active — _gdialogActive() is false unless a conversation is
    //     actually running (pump installed via F2_DIALOG_STREAM, or a golden
    //     pre-queued one), so in normal play (no pump) this is a hard no-op.
    //   * owner — v1 has a single gDude actor so the claimant IS the dialog
    //     owner; when P2 lands, compare the dialog driver's actor to this
    //     claimant's actor (mp-actor-architecture-principle: gate on ownership,
    //     never hardcode "viewer 0 drives"). A spectator's dsay must no-op.
    // Fully DORMANT today: no viewer sends these yet (TALK→LOOK until A3) and no
    // pump runs by default, so the gate short-circuits at _gdialogActive().
    if (strcmp(verb, "dsay") == 0 || strcmp(verb, "dend") == 0 || strcmp(verb, "dbarter") == 0) {
        // WHOEVER STARTED IT ANSWERS IT (owner ruling 2026-07-21). The stderr text
        // keeps the "host-only screen" wording it has always had — scripts grep it,
        // and it is still accurate for the NPC-initiated fallback.
        if (!serverControlMayDriveDialog(sessionId)) {
            fprintf(stderr, "f2_server: control %s dropped (host-only screen)\n", verb);
            serverControlRefuse(sessionId, "This isn't your conversation.");
            return;
        }
        // ►► gameDialogServerNodeActive() is the load-bearing half of this test;
        // _gdialogActive() is kept only so the pre-existing engine-TALK path keeps
        // its exact old behaviour. Gating on _gdialogActive() ALONE was a deadlock:
        // a script-driven conversation (gsay_start/gsay_end -> _gdialogGo ->
        // _gdProcess, no gameDialogEnter) leaves that flag 0, so every dsay/dend
        // was thrown away while the server sat parked in the barrier waiting for
        // one. See the note on gameDialogServerNodeActive() in game_dialog.h.
        if (!_gdialogActive() && !gameDialogServerNodeActive()) {
            // REVERTED (2026-07-21): this branch briefly emitted presenter()->
            // dialogEnd() here to unstick the viewer's waiting-for-response latch.
            // That was WRONG and caused a worse bug — "_gdialogActive() is false"
            // does NOT mean _gdProcess has returned, so it told the viewer to tear
            // down while the server was still parked in the dialog barrier. The
            // viewer then stops sending dsay, the barrier never drains, and the
            // world is frozen while mv/rot/invdrop are still accepted by the pump:
            // the observed "limbo". Fix the stuck SIDE, never lie to the other one.
            //
            // The real defect is upstream in game_dialog.cc: dialogEmitNode() ships
            // a node to the client and the pump may then bail on the very next
            // statement, abandoning the conversation the client is now displaying.
            // Instrumented below (F2_DIALOG_TRACE) — do not paper over it here.
            fprintf(stderr, "f2_server: control %s ignored (no active dialog)\n", verb);
            // Reaching here means the viewer is SHOWING a conversation the server
            // does not think is running — the denbus2 softlock's exact signature.
            // Do not tear the viewer's screen down (the REVERTED note above); just
            // say the reply went nowhere, so the divergence is reported instead of
            // sat through.
            serverControlRefuse(sessionId, "That conversation is no longer open.");
            return;
        }
        if (strcmp(verb, "dbarter") == 0) {
            // The on-screen Barter button, routed. Whether this speaker WILL
            // barter is the server's call (CRITTER_BARTER), made in the drain.
            dialogIntentPush(DIALOG_INTENT_BARTER, 0);
            fprintf(stderr, "f2_server: control dbarter\n");
        } else if (strcmp(verb, "dend") == 0) {
            dialogIntentPush(DIALOG_INTENT_END, 0);
            fprintf(stderr, "f2_server: control dend\n");
        } else {
            if (n < 2 || arg < 0) {
                fprintf(stderr, "f2_server: control dsay bad index=%d ignored\n", n >= 2 ? arg : -1);
                serverControlRefuse(sessionId, "That reply is no longer available.");
                return;
            }
            // Bounds against the live option count are re-checked at drain time
            // (game_dialog.cc vs gGameDialogOptionEntriesLength), so a stale index
            // can never select out of range even if the node changed underneath.
            dialogIntentPush(DIALOG_INTENT_SELECT, arg);
            fprintf(stderr, "f2_server: control dsay index=%d\n", arg);
        }
        return;
    }

    // -- Barter verbs (the trade screen, entered from inside a conversation) --
    // The debug port's vocabulary VERBATIM (command.cc): boffer/btake/bunoffer
    // move `qty` of a pid between an owner and a table, bcommit is the 'M'/Offer
    // button, bdone the 'T'/Talk button, bcancel ESC. They feed the SAME
    // barter_intent queue inventoryOpenTrade's headless drain already consumes,
    // so this adds a wire ROUTE to a proven mechanism rather than a mechanism.
    //
    // DRIVE FOLLOWS THE CONVERSATION. Barter is reached by picking a dialog
    // option, so the player who owns the conversation owns the trade — the same
    // predicate, deliberately not a second notion of ownership to keep in sync.
    if (strcmp(verb, "boffer") == 0 || strcmp(verb, "btake") == 0
        || strcmp(verb, "bunoffer") == 0 || strcmp(verb, "bcommit") == 0
        || strcmp(verb, "bdone") == 0 || strcmp(verb, "bcancel") == 0) {
        if (!serverControlMayDriveDialog(sessionId)) {
            fprintf(stderr, "f2_server: control %s dropped (not the barter driver)\n", verb);
            serverControlRefuse(sessionId, "This isn't your trade.");
            return;
        }
        // An intent pushed while no trade is open would SIT IN THE QUEUE and be
        // applied to the next one — a stale offer surfacing inside someone
        // else's trade, with real items moving. The queue carries no trade
        // identity, so the liveness check has to happen here, at the boundary.
        if (!GameMode::isInGameMode(GameMode::kBarter)) {
            fprintf(stderr, "f2_server: control %s ignored (no active barter)\n", verb);
            serverControlRefuse(sessionId, "You aren't trading.");
            return;
        }

        // arg = item pid, arg2 = quantity (<=0 means the whole stack, clamped
        // against what the source actually holds at DRAIN time — the trade is
        // live and another verb may have moved it since).
        int pid = n >= 2 ? arg : -1;
        int qty = n >= 3 ? arg2 : 0;

        if (strcmp(verb, "boffer") == 0) {
            barterIntentPush(BARTER_INTENT_OFFER_ITEM, pid, qty);
        } else if (strcmp(verb, "btake") == 0) {
            barterIntentPush(BARTER_INTENT_TAKE_ITEM, pid, qty);
        } else if (strcmp(verb, "bunoffer") == 0) {
            barterIntentPush(BARTER_INTENT_UNOFFER_ITEM, pid, qty);
        } else if (strcmp(verb, "bcommit") == 0) {
            barterIntentPush(BARTER_INTENT_COMMIT, 0, 0);
        } else if (strcmp(verb, "bdone") == 0) {
            barterIntentPush(BARTER_INTENT_DONE, 0, 0);
        } else {
            barterIntentPush(BARTER_INTENT_CANCEL, 0, 0);
        }
        fprintf(stderr, "f2_server: control %s pid=%d qty=%d\n", verb, pid, qty);
        return;
    }

    // -- Worldmap travel verbs -----------------------------------------------
    // wmmove <x> <y> / wmenter / wmesc route the viewer's travel intents
    // through the trust boundary into the worldmap intent queue, which the
    // server-side worldmap driver drains. Three gates:
    //   * claimant — only the controlling client may drive.
    //   * worldmap-active — worldmapServerActive() is false unless the driver
    //     is actually running.
    if (strcmp(verb, "wmmove") == 0 || strcmp(verb, "wmenter") == 0 || strcmp(verb, "wmesc") == 0) {
        if (!serverControlIsHostSession(sessionId)) {
            fprintf(stderr, "f2_server: control %s dropped (host-only screen)\n", verb);
            // Same design rule as dialog: travel relocates everyone, so only the
            // host drives it. An extra clicking the map is not broken, just not
            // in charge — and has no way to know that unless we say so.
            serverControlRefuse(sessionId, "Only the host can travel the world map.");
            return;
        }
        if (!worldmapServerActive()) {
            fprintf(stderr, "f2_server: control %s ignored (no active worldmap)\n", verb);
            serverControlRefuse(sessionId, "The world map is not open.");
            return;
        }
        if (strcmp(verb, "wmesc") == 0) {
            worldmapIntentPush(WM_INTENT_ESCAPE, 0, 0);
            fprintf(stderr, "f2_server: control wmesc\n");
        } else if (strcmp(verb, "wmenter") == 0) {
            worldmapIntentPush(WM_INTENT_ENTER, 0, 0);
            fprintf(stderr, "f2_server: control wmenter\n");
        } else {
            if (n < 3) {
                fprintf(stderr, "f2_server: control wmmove missing coords ignored\n");
                return;
            }
            worldmapIntentPush(WM_INTENT_MOVE, arg, arg2);
            fprintf(stderr, "f2_server: control wmmove %d %d\n", arg, arg2);
        }
        return;
    }

    // All interaction verbs share the claimant + out-of-combat gates and target
    // resolution; dispatch below on the specific verb.
    bool isInteractVerb = strcmp(verb, "use") == 0
        || strcmp(verb, "usedoor") == 0
        || strcmp(verb, "get") == 0
        || strcmp(verb, "look") == 0
        || strcmp(verb, "push") == 0
        || strcmp(verb, "rot") == 0
        || strcmp(verb, "skill") == 0
        || strcmp(verb, "talk") == 0
        || strcmp(verb, "loot") == 0
        || strcmp(verb, "useitemon") == 0;
    if (isInteractVerb) {
        // LOOK (examine) is free in combat — no AP, no turn, no barrier; vanilla
        // lets you examine mid-fight. It runs inline below like it always has.
        //
        // The ACTING verbs need the turn barrier and their AP costs, so in combat
        // they are QUEUED as intents stamped with the issuer's slot and executed
        // by the pump on that actor's own turn (serverControlRunCombatInteract).
        // They used to be dropped here — Stage 4 is what un-drops them.
        //
        // TALK is the exception that stays dropped: starting a conversation
        // mid-fight has no vanilla behavior to be faithful to, and dialog is
        // host-only anyway (@c3243a1). PUSH and ROT keep their existing inline
        // no-approach handling below and are free in combat, as in vanilla.
        //
        // The queue/execute split is made inside serverControlArmInteraction, not
        // here, so every approach verb below reaches it through its ONE existing
        // call and no dispatch arm has to remember to branch on combat.
        if (isInCombat() && strcmp(verb, "talk") == 0) {
            fprintf(stderr, "f2_server: control talk dropped (in combat)\n");
            serverControlRefuse(sessionId, "You can't talk to anyone in combat.");
            return;
        }
        if (gDude == nullptr) {
            return;
        }

        // rot: no target, no approach — rotate the actor one step and stream it back.
        if (strcmp(verb, "rot") == 0) {
            serverControlSupersedePending(sessionId, "a fresh action");
            reg_anim_clear(gDude); // and stops any in-flight approach walk
            Rect rect;
            objectRotateClockwise(gDude, &rect);
            fprintf(stderr, "f2_server: control rot\n");
            return;
        }

        // Every other verb takes a target netId. Validate: exists + same elevation
        // (never hand an outcome function a stray/gone/hostile netId).
        int netId = n >= 2 ? arg : -1;
        Object* target = objectFindByNetId(netId);
        if (target == nullptr || target->elevation != gDude->elevation) {
            fprintf(stderr, "f2_server: control %s bad target netId=%d ignored\n", verb, netId);
            // Usually a STALE netId: the baseline re-minted while the viewer was
            // holding one (objectAssignAllNetIds). The player sees an object they
            // clicked simply not respond, which reads as a freeze rather than a
            // stale reference — worth naming.
            serverControlRefuse(sessionId, "That is no longer there.");
            return;
        }

        if (strcmp(verb, "useitemon") == 0) {
            // Use an inventory item ON a world target: `useitemon <netId> <itemPid>`.
            // The viewer picks the item locally (inventoryOpenUseItemOn modal) and
            // sends its pid; find that item on the dude's TOP-LEVEL inventory (same
            // pid-addressing as the useitem/equip verbs — obj->id is non-unique on the
            // wire). Walk-then-act (approach <= 1); the outcome runs at arrival. The
            // target may be any type (scenery/critter/item) — _action_use_an_item_on_
            // object routes it. This is THE Temple Key → locked-door path.
            int itemPid = n >= 3 ? arg2 : -1;
            Object* item = nullptr;
            Inventory* inv = &gDude->data.inventory;
            for (int i = 0; i < inv->length; i++) {
                if (inv->items[i].item != nullptr && inv->items[i].item->pid == itemPid) {
                    item = inv->items[i].item;
                    break;
                }
            }
            if (item == nullptr) {
                fprintf(stderr, "f2_server: control useitemon no dude item pid=%d ignored\n", itemPid);
                serverControlRefuse(sessionId, "You don't have that item.");
                return;
            }
            // Explosives have their own arm-and-drop semantics (useitem_armexplosive);
            // keep this verb to plain use-on and reject them at the trust boundary.
            if (explosiveIsExplosive(item->pid)) {
                fprintf(stderr, "f2_server: control useitemon pid=%d rejected (explosive; use arm path)\n", itemPid);
                serverControlRefuse(sessionId, "Arm the explosive and drop it instead.");
                return;
            }
            serverControlArmInteraction(sessionId, actor, kInteractUseItemOn, target, itemPid);
            return;
        }

        if (strcmp(verb, "use") == 0 || strcmp(verb, "usedoor") == 0) {
            // Scenery use (doors/levers/ladders/stairs). `usedoor` keeps the slice-1
            // door-only validation (probe/older-client compat); `use` accepts any
            // scenery and lets _obj_use route it. Walk-then-act (approach <= 1).
            if (PID_TYPE(target->pid) != OBJ_TYPE_SCENERY) {
                fprintf(stderr, "f2_server: control %s target netId=%d not scenery\n", verb, netId);
                serverControlRefuse(sessionId, "You can't use that.");
                return;
            }
            if (strcmp(verb, "usedoor") == 0) {
                Proto* proto = nullptr;
                if (protoGetProto(target->pid, &proto) != 0
                    || proto->scenery.type != SCENERY_TYPE_DOOR) {
                    fprintf(stderr, "f2_server: control usedoor target netId=%d not a door\n", netId);
                    serverControlRefuse(sessionId, "That isn't a door.");
                    return;
                }
            }
            serverControlArmInteraction(sessionId, actor, kInteractUse, target, 0);
            return;
        }

        if (strcmp(verb, "get") == 0) {
            // Ground item only — containers open the (not-yet-streamed) loot modal.
            if (PID_TYPE(target->pid) != OBJ_TYPE_ITEM || itemGetType(target) == ITEM_TYPE_CONTAINER) {
                fprintf(stderr, "f2_server: control get target netId=%d not a ground item\n", netId);
                serverControlRefuse(sessionId, "You can't pick that up.");
                return;
            }
            serverControlArmInteraction(sessionId, actor, kInteractGet, target, 0);
            return;
        }

        if (strcmp(verb, "look") == 0) {
            // No approach — examine streams the description (EVENT_CONSOLE). Mirror
            // vanilla's fallback: if _obj_examine has no description to give (-1), fall
            // back to _obj_look_at (game_mouse.cc LOOK case). Allowed in combat too;
            // only clear a pending approach walk OUT of combat (in combat there is no
            // interaction approach in flight, and reg_anim_clear must not touch a live
            // combat animation).
            serverControlSupersedePending(sessionId, "an examine");
            if (!isInCombat()) {
                reg_anim_clear(gDude); // supersede any in-flight approach walk
            }
            if (_obj_examine(gDude, target) == -1) {
                _obj_look_at(gDude, target);
            }
            fprintf(stderr, "f2_server: control look netId=%d\n", netId);
            return;
        }

        if (strcmp(verb, "push") == 0) {
            // No approach — actionPush displaces the TARGET, not the actor (vanilla).
            if (PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
                fprintf(stderr, "f2_server: control push target netId=%d not a critter\n", netId);
                serverControlRefuse(sessionId, "There is nobody there to push.");
                return;
            }
            serverControlSupersedePending(sessionId, "a push");
            reg_anim_clear(actor); // supersede any in-flight approach walk
            actionPush(actor, target);
            fprintf(stderr, "f2_server: control push netId=%d\n", netId);
            return;
        }

        if (strcmp(verb, "skill") == 0) {
            // `skill <netId> <skillId>` — allow-list the untrusted skill id. Walk-
            // then-act (approach <= 1).
            int skill = n >= 3 ? arg2 : -1;
            if (!interactionSkillAllowed(skill)) {
                fprintf(stderr, "f2_server: control skill bad skill=%d netId=%d ignored\n", skill, netId);
                serverControlRefuse(sessionId, "You can't use that skill here.");
                return;
            }
            serverControlArmInteraction(sessionId, actor, kInteractSkill, target, skill);
            return;
        }

        // loot: a container item or a downed critter. Walk-then-open — approach-ONLY
        // (kInteractLoot fires no server outcome); the viewer opens its loot modal on
        // arrival and drives take/put from there. A live critter is steal (separate).
        if (strcmp(verb, "loot") == 0) {
            bool isContainer = PID_TYPE(target->pid) == OBJ_TYPE_ITEM
                && itemGetType(target) == ITEM_TYPE_CONTAINER;
            bool isCorpse = PID_TYPE(target->pid) == OBJ_TYPE_CRITTER && critterIsDead(target);
            if (!isContainer && !isCorpse) {
                fprintf(stderr, "f2_server: control loot target netId=%d not a container/corpse\n", netId);
                serverControlRefuse(sessionId, "There is nothing there to loot.");
                return;
            }
            serverControlArmInteraction(sessionId, actor, kInteractLoot, target, 0);
            return;
        }

        // talk: critter only (the unguarded fall-through). Walk-then-act (approach <
        // 9). Verb ships; the viewer menu does not wire it until dialog-options
        // streaming (§5).
        if (PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
            fprintf(stderr, "f2_server: control talk target netId=%d not a critter\n", netId);
            serverControlRefuse(sessionId, "There is nobody there to talk to.");
            return;
        }
        serverControlArmInteraction(sessionId, actor, kInteractTalk, target, 0);
        return;
    }

    if (strcmp(verb, "cstart") == 0) {
        // Player-initiated combat start (vanilla 'A' toggle). Claimant-only, and
        // meaningful only OUT of combat — in combat you already have a turn (use the
        // in-combat verbs). Latch the request; the server loop honors it on its idle
        // tick by calling _combat(nullptr). We do NOT call _combat here: this runs
        // inside the inbound drain, well before the tick's combat-advance point, and
        // combat entry must happen at that one safe site (server_loop.cc).
        if (isInCombat()) {
            fprintf(stderr, "f2_server: control cstart dropped (already in combat)\n");
            serverControlRefuse(sessionId, "You are already in combat.");
            return;
        }
        // WHO started it, because the initiator takes the first turn (below in
        // server_loop, via the CombatStartData attacker). Without this every fight
        // opened on the host's turn no matter who pressed A — owner-reported: P2
        // enters combat and P1 moves first.
        if (gPendingCombatStartSlot < 0) {
            gPendingCombatStartSlot = serverControlSlotForSession(sessionId);
            if (gPendingCombatStartSlot < 0) {
                gPendingCombatStartSlot = 0; // debug CMD port has no session: the host
            }
        }
        fprintf(stderr, "f2_server: control cstart (combat-start requested) slot=%d\n",
            gPendingCombatStartSlot);
        return;
    }

    // invopen / invclose: the in-combat inventory SESSION (Stage 4).
    //
    // Out of combat the viewer opens its inventory locally and never sends these —
    // a screen that costs nothing needs no permission, and a round trip on every
    // 'I' press would be latency for its own sake. IN COMBAT the open is the
    // PRICED act (vanilla charges 4 AP, 2 with Quick Pockets, at open and nothing
    // for the actions inside — item.cc inventoryApCostApply, and inventory_ui.cc
    // :3513 confirms using an item leaves the screen up). Only the authority can
    // decide whether that was affordable, so in combat the viewer asks.
    if (strcmp(verb, "invopen") == 0 || strcmp(verb, "invclose") == 0) {
        int slot = serverControlSlotForSession(sessionId);
        if (slot < 0) {
            slot = 0; // the debug CMD port has no session and drives the host
        }
        if (strcmp(verb, "invclose") == 0) {
            gInventoryOpenSlots.erase(slot);
            fprintf(stderr, "f2_server: control invclose slot=%d\n", slot);
            return;
        }
        if (actor == nullptr) {
            return;
        }
        if (!isInCombat()) {
            // Free, and no session to track: out of combat the inventory verbs are
            // ungated anyway. Answer the grant so a viewer that asked (the debug
            // port, or a client that raced combat ending) still opens its screen.
            presenter()->inventoryGrant(actor->netId);
            fprintf(stderr, "f2_server: control invopen slot=%d (free, out of combat)\n", slot);
            return;
        }
        // Vanilla refuses outright when it is not your turn (inventory_ui.cc:554
        // checks _combat_whose_turn() != _inven_dude and simply returns). No
        // message there and none here — the viewer never offers the key on
        // someone else's turn, so reaching this is a race, not a player error.
        if (_combat_whose_turn() != actor) {
            fprintf(stderr, "f2_server: control invopen dropped (not slot %d's turn)\n", slot);
            return;
        }
        if (!inventoryApCostApply(actor)) {
            // "You don't have enough action points." — proto msg 700, streamed
            // here because the viewer never opened a screen to show it in.
            //
            // Vanilla says the inventory-specific msg 19 ("...to use inventory")
            // at this exact point (inventory_ui.cc:571). We cannot: that string
            // lives in gInventoryMessageList, a static inside inventory_ui.cc,
            // which is f2_client and not linked into f2_server. Msg 700 is the
            // same refusal from gProtoMessageList — the list the server already
            // speaks, and the one _check_scenery_ap_cost uses for the identical
            // refusal on doors and pickups, so the player gets one consistent
            // wording for "not enough AP" across the whole interaction suite.
            MessageListItem messageListItem;
            messageListItem.num = 700;
            if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
                presenter()->consoleMessageFor(actor->netId, messageListItem.text);
            }
            fprintf(stderr, "f2_server: control invopen refused (no AP) slot=%d\n", slot);
            return;
        }
        gInventoryOpenSlots.insert(slot);
        combatSessionRearmIdleTimer(); // browsing a pack is not being idle
        presenter()->inventoryGrant(actor->netId);
        fprintf(stderr, "f2_server: control invopen granted slot=%d ap=%d\n",
            slot, actor->data.critter.combat.ap);
        return;
    }

    // -- Dude inventory verbs (player-UI Slice 3b: equip / unequip / drop) ---
    // The viewer's inventory screen reroutes its drag-drop resolution + ctx-menu
    // DROP to these instead of mutating the local mirror (which would fight the
    // server-authoritative inventory). Claimant-gated; in combat they additionally
    // require the paid-for inventory session above. invwield/invdrop address a specific
    // object by netId over the dude's TOP-LEVEL inventory; useitem still takes a
    // pid, where any stack of that kind will do. ("First match by pid is faithful
    // enough" was the old rule and it was WRONG the moment a player owned two of
    // one thing — see the resolution block below. Nested container contents are
    // still not streamed.) The mutation rides OBJECT_DELTA_
    // INVENTORY back to every viewer (Slice 2 reconcile); a dropped item also
    // arrives as a world SPAWN. Mirrors the debug-port item fns (command.cc).
    bool isInvVerb = strcmp(verb, "invwield") == 0
        || strcmp(verb, "invunwield") == 0
        || strcmp(verb, "invdrop") == 0
        || strcmp(verb, "useitem") == 0
        || strcmp(verb, "useitem_armexplosive") == 0;
    if (isInvVerb) {
        if (gDude == nullptr) {
            return;
        }
        if (isInCombat()) {
            // Free — but only inside a session this slot PAID for, and only while
            // it is still their turn. The own-turn re-check is what makes a stale
            // session harmless: if their turn ended while the screen was open, the
            // set still says "open" and this still refuses, so the flag can never
            // buy an action on someone else's turn.
            int slot = serverControlSlotForSession(sessionId);
            if (slot < 0) {
                slot = 0; // debug CMD port drives the host
            }
            Object* invActor = actor != nullptr ? actor : gDude;
            if (!serverControlInventorySessionOpen(slot)) {
                fprintf(stderr, "f2_server: control %s dropped (no inventory session, slot=%d)\n",
                    verb, slot);
                serverControlRefuse(sessionId, "Open your inventory first.");
                return;
            }
            if (_combat_whose_turn() != invActor) {
                // Unlike the invopen race above, this one is REACHABLE by an honest
                // player: their turn ended while the screen was still open, so the
                // drag they are in the middle of is refused with the UI still up.
                fprintf(stderr, "f2_server: control %s dropped (not slot %d's turn)\n", verb, slot);
                serverControlRefuse(sessionId, "It isn't your turn.");
                return;
            }
            // Deliberately does NOT rearm the idle timer. Actions inside the
            // screen are FREE, so rearming on each one would let a player hold
            // the fight open forever by fiddling with their equipment — every
            // other way of extending a turn costs AP and therefore runs out. The
            // one rearm at invopen gives a full idle budget to browse in, and
            // that budget is the hard bound.
        }

        // invunwield <hand>: 0 = left, 1 = right, 2 = armor. No pid — the slot
        // identifies the item. _inven_unwield covers the hands (clears the in-hand
        // flag + puts the weapon away); armor has no engine unwield fn, so mirror
        // the inline unequip the inventory UI does (clear WORN + recompute AC).
        if (strcmp(verb, "invunwield") == 0) {
            int hand = n >= 2 ? arg : -1;
            if (hand == HAND_LEFT || hand == HAND_RIGHT) {
                Object* held = hand == HAND_LEFT ? critterGetItem1(gDude) : critterGetItem2(gDude);
                _inven_unwield(gDude, hand);
                // RE-STACK. The wield path peels one unit out of its stack into a slot
                // of its own (it must — a stack marked in-hand is undroppable), so an
                // unwield hands the unit back as a SEPARATE slot and the inventory shows
                // "1 spear" beside "2 spears" forever. itemAdd is the engine's merger, so
                // take the unit out and put it back: identical unflagged stacks fold into
                // one, and if none matches this is a no-op move to the end of the list.
                // Only bother when a plausible merge target exists, so the common case
                // keeps its slot order. (_item_identical is private to item.cc; this
                // approximation only decides whether to ASK — itemAdd still rules.)
                if (held != nullptr && (held->flags & OBJECT_EQUIPPED) == 0
                    && held->data.inventory.length == 0) {
                    Inventory* inv = &gDude->data.inventory;
                    bool mergeable = false;
                    for (int i = 0; i < inv->length && !mergeable; i++) {
                        Object* other = inv->items[i].item;
                        mergeable = other != nullptr && other != held
                            && other->pid == held->pid
                            && (other->flags & (OBJECT_EQUIPPED | OBJECT_QUEUED)) == 0;
                    }
                    if (mergeable && itemRemove(gDude, held, 1) == 0) {
                        itemAdd(gDude, held, 1);
                    }
                }
                fprintf(stderr, "f2_server: control invunwield hand=%d\n", hand);
            } else if (hand == HAND_COUNT) { // 2 == armor slot (no Hand enum value)
                Object* armor = critterGetArmor(gDude);
                if (armor != nullptr) {
                    armor->flags &= ~OBJECT_WORN;
                    _adjust_ac(gDude, armor, nullptr);
                    // Back to the bare body, mirroring the equip side (and vanilla's
                    // own unequip, proto_instance.cc's remove-from-inven armor branch,
                    // which reads the same naked base out of the dude proto). Without
                    // it the server keeps rendering the armor it no longer believes
                    // you are wearing.
                    Proto* dudeProto;
                    int baseFrmId = 1;
                    if (protoGetProto(0x1000000, &dudeProto) != -1) {
                        baseFrmId = dudeProto->fid & 0xFFF;
                    }
                    int bareFid = buildFid(OBJ_TYPE_CRITTER, baseFrmId, ANIM_STAND,
                        (gDude->fid & 0xF000) >> 12, gDude->rotation + 1);
                    if (artExists(bareFid) && gDude->fid != bareFid) {
                        objectSetFid(gDude, bareFid, nullptr);
                        objectSetFrame(gDude, 0, nullptr);
                    }
                }
                fprintf(stderr, "f2_server: control invunwield armor\n");
            } else {
                fprintf(stderr, "f2_server: control invunwield bad hand=%d ignored\n", hand);
            }
            return;
        }

        // invwield / invdrop address a specific carried object by netId; useitem still
        // takes a pid (any stack of a drug/stimpak will do — no identity involved).
        //
        // IDENTITY, NOT KIND. This used to resolve everything by pid, taking the FIRST
        // matching slot. pid says what kind of thing an item is, so every stack of one
        // pid was interchangeable: with a spear in each hand plus loose ones there are
        // three slots of pid 7, and the verb acted on whichever came first. Live result
        // — dropping "the loose ones" dropped the wielded spear too, or wielding split
        // the wrong slot. Carried items already carry netIds (the server numbers
        // inventory recursively and the wire ships one per item), so the acting object
        // can simply be named.
        const bool byNetId = strcmp(verb, "invwield") == 0 || strcmp(verb, "invdrop") == 0;
        int pid = -1;
        Object* item = nullptr;
        int stackQty = 0;
        {
            Inventory* inv = &gDude->data.inventory;
            for (int i = 0; i < inv->length; i++) {
                Object* candidate = inv->items[i].item;
                if (candidate == nullptr) {
                    continue;
                }
                const bool hit = byNetId ? (candidate->netId == arg && arg != 0)
                                         : (candidate->pid == arg);
                if (hit) {
                    item = candidate;
                    stackQty = inv->items[i].quantity;
                    break;
                }
            }
        }
        if (item == nullptr) {
            fprintf(stderr, "f2_server: control %s no dude item %s=%d ignored\n",
                verb, byNetId ? "netId" : "pid", n >= 2 ? arg : -1);
            serverControlRefuse(sessionId, "You don't have that item.");
            return;
        }
        pid = item->pid; // logging / type branches below still speak pid

        if (strcmp(verb, "useitem") == 0) {
            // USE / apply an inventory item on the dude: drugs (quaff), or misc/
            // weapon self-use (stimpak, healing powder, books, flares). Mirrors the
            // inventory ctx-menu USE leaf's type branch (inventory_ui.cc:3502) —
            // ITEM_TYPE_DRUG through itemUseDrug, everything else through
            // itemUseFromInventory; inventoryResident=true (a plain inventory item,
            // not a hand slot). The consume/heal/skill-up rides OBJECT_DELTA_INVENTORY
            // + presentation back to every viewer, like equip/drop.
            //
            // GUARD: reject explosives. itemUseFromInventory → _obj_use_explosive →
            // _inven_set_timer → inventoryQuantitySelect(SET_TIMER) is a BLOCKING
            // local modal with no headless representation (it would spin inputGetInput
            // on the server). Arming C4/dynamite over the wire is a deferred increment
            // (viewer runs the timer modal locally + sends the seconds → a headless arm
            // that skips _inven_set_timer). See DIALOG_STREAMING_PLAN's sibling item-use
            // notes / p5-server-plan item-usage gap.
            if (explosiveIsExplosive(item->pid)) {
                fprintf(stderr, "f2_server: control useitem pid=%d rejected (explosive timer modal not wired)\n", pid);
                serverControlRefuse(sessionId, "Set the timer from the inventory to arm that.");
                return;
            }
            if (itemGetType(item) == ITEM_TYPE_DRUG) {
                itemUseDrug(gDude, gDude, item, true);
            } else {
                itemUseFromInventory(gDude, gDude, item, true);
            }
            fprintf(stderr, "f2_server: control useitem pid=%d\n", pid);
            return;
        }

        if (strcmp(verb, "useitem_armexplosive") == 0) {
            // Sub-action of USE for C4 / dynamite. The plain `useitem` verb rejects
            // explosives because vanilla arming (_obj_use_explosive) opens the blocking
            // _inven_set_timer dial (inventoryQuantitySelect SET_TIMER) with no headless
            // representation. Instead the VIEWER runs that timer dial LOCALLY to pick the
            // countdown and sends the chosen seconds here; the server arms the charge
            // headless via the shared _obj_arm_explosive helper (identical post-modal
            // logic: explosiveActivate + Demolition/Traps roll + queueAddEvent), skipping
            // only the modal. The timed EVENT_TYPE_EXPLOSION later resolves through
            // actionExplode's serverLoopActive() branch (already decoupled), and a door /
            // scenery destroyed by the blast streams out as EVENT_DESTROY. The armed
            // charge is normally dropped next to the target via `invdrop` before it blows
            // (so _queue_do_explosion_ uses the ground tile, not the dude's).
            if (!explosiveIsExplosive(item->pid)) {
                fprintf(stderr, "f2_server: control useitem_armexplosive pid=%d not an explosive, ignored\n", pid);
                serverControlRefuse(sessionId, "That isn't an explosive.");
                return;
            }
            if ((item->flags & OBJECT_QUEUED) != 0) {
                // Already ticking — vanilla no-op (proto message 590). Silence here
                // is genuinely dangerous rather than merely confusing: the player is
                // holding a live charge and has just been told nothing about it.
                fprintf(stderr, "f2_server: control useitem_armexplosive pid=%d already armed, ignored\n", pid);
                serverControlRefuse(sessionId, "That one is already ticking.");
                return;
            }
            // Trust-boundary clamp: the vanilla dial enforces 0..180 stepped by 10.
            int seconds = n >= 3 ? arg2 : 0;
            if (seconds < 0) {
                seconds = 0;
            }
            if (seconds > 180) {
                seconds = 180;
            }
            seconds -= seconds % 10;
            _obj_arm_explosive(item, seconds);
            fprintf(stderr, "f2_server: control useitem_armexplosive pid=%d seconds=%d\n", pid, seconds);
            return;
        }

        if (strcmp(verb, "invwield") == 0) {
            // arg2 = target hand (0/1); ignored for armor (_inven_wield branches on
            // ITEM_TYPE_ARMOR internally). Default to the right hand.
            int hand = n >= 3 ? arg2 : HAND_RIGHT;
            if (hand != HAND_LEFT && hand != HAND_RIGHT) {
                hand = HAND_RIGHT;
            }
            // Clear the item's in-hand flags FIRST. A slot→slot move re-wields an item
            // that already carries its OLD hand flag, and _inven_wield only ADDS the new
            // one — so without this the item keeps BOTH IN_LEFT_HAND|IN_RIGHT_HAND, and the
            // viewer's equipmentDetach reconstructs it as a two-slot (two-handed) item,
            // rendering the sprite ghosted across both slots. Vanilla never hits this (its
            // UI moves the hand STATICS directly instead of re-wielding).
            // SPLIT THE STACK FIRST. A stack of N identical items is ONE Object
            // with quantity N, not N objects — so wielding the slot's object marks
            // the whole stack as equipped, and a later drop of "the loose ones"
            // takes all N and the wielded one with them (owner-reported: 3 spears,
            // equip 1, drop -> all 3 leave and the hand empties). Peel ONE unit off
            // and wield that.
            //
            // itemRemove IS the peeler: on a partial removal it _obj_copy's a fresh
            // object into the slot (quantity-1) and hands the ORIGINAL back, un-owned
            // and with OBJECT_EQUIPPED cleared. So no _obj_copy of our own is needed —
            // an earlier version made one and leaked an object per wield.
            //
            // The stamp before itemAdd is the load-bearing part: _item_identical treats
            // only an OBJECT_EQUIPPED item as unstackable, and _inven_wield sets that
            // flag AFTER we add — so an unstamped peel merges straight back into the
            // stack it came from (itemAdd's merge branch keeps ONE object and bumps the
            // quantity), the split silently no-ops, and the whole stack ends up flagged
            // in-hand. itemDropStack then refuses to drop ANY of it (owner-reported:
            // equip one spear of two, and neither can be dropped until you unequip).
            // Clear the stamp again immediately: _inven_wield owns the real equip flags,
            // and leaving WORN set would make its armor branch un-wear the peel itself
            // instead of the armor actually being replaced.
            if (stackQty > 1) {
                const int splitFlag = itemGetType(item) == ITEM_TYPE_ARMOR
                    ? OBJECT_WORN
                    : (hand == HAND_LEFT ? OBJECT_IN_LEFT_HAND : OBJECT_IN_RIGHT_HAND);
                if (itemRemove(gDude, item, 1) == 0) {
                    item->flags |= splitFlag;
                    itemAdd(gDude, item, 1);
                    item->flags &= ~splitFlag;
                } else {
                    fprintf(stderr, "f2_server: control invwield stack split failed pid=%d\n", pid);
                }
            }
            item->flags &= ~OBJECT_IN_ANY_HAND;
            // For the DUDE, _inven_wield applies the WORN flag + paperdoll fid but does
            // NOT recompute AC/DR/DT (that lives in the inventory UI, which the viewer
            // skips) — so equip the armor's protection here, mirroring the UI's
            // _adjust_ac(critter, oldArmor, newArmor). Harmless for non-armor (guarded).
            Object* oldArmor = critterGetArmor(gDude);
            _inven_wield(gDude, item, hand);
            // A genuine two-handed weapon occupies BOTH hand slots (the UI represents that
            // as leftHand==rightHand); _inven_wield only sets one flag, so pair them here.
            if (itemGetType(item) == ITEM_TYPE_WEAPON && weaponIsTwoHanded(item)) {
                item->flags |= (OBJECT_IN_LEFT_HAND | OBJECT_IN_RIGHT_HAND);
            }
            if (itemGetType(item) == ITEM_TYPE_ARMOR) {
                _adjust_ac(gDude, oldArmor, item);
                // Server-authoritative APPEARANCE, the armor twin of the weapon block
                // below — and the same root cause. _inven_wield applies the armored
                // SKIN (the fid's low 12 bits, its base frm id) through
                // animationRegisterSetFid, an animation leaf that does not hold on the
                // dedicated server: measured, gDude's base stayed the premade's 62
                // across equip, attack and throw while the viewer showed the armored
                // body it had applied locally.
                //
                // That divergence is not cosmetic, because the base is what every
                // later server-computed fid PRESERVES (`fid & 0xFFF`) while changing
                // the animation and weapon codes around it. So the first such fid to
                // reach the viewer ships the UNARMORED body with it: throw a grenade
                // and the throw's stand-fid leaf (actions.cc, `attacker->fid & 0xFFF`)
                // replays as "stand, no weapon, NAKED base" — the armor visibly
                // reverts to the default body and the critter snaps to a stand pose.
                // Owner-reported. Fixing it here rather than at the throw keeps ONE
                // authority for appearance: whatever the server believes it is
                // wearing, it looks like.
                int baseFrmId = critterGetStat(gDude, STAT_GENDER) == GENDER_FEMALE
                    ? armorGetFemaleFid(item)
                    : armorGetMaleFid(item);
                if (baseFrmId == -1) {
                    baseFrmId = 1; // _inven_wield's own fallback
                }
                // Keep the weapon code — equipping armor must not un-draw a held gun.
                int armoredFid = buildFid(OBJ_TYPE_CRITTER, baseFrmId, ANIM_STAND,
                    (gDude->fid & 0xF000) >> 12, gDude->rotation + 1);
                if (artExists(armoredFid) && gDude->fid != armoredFid) {
                    objectSetFid(gDude, armoredFid, nullptr);
                    objectSetFrame(gDude, 0, nullptr); // stale frame >= new art's count renders nothing
                }
            }
            // Server-authoritative APPEARANCE: reflect the equipped weapon in gDude's FID.
            // _invenWieldFunc only updates the world fid via interface-/combat-gated paths
            // (interfaceGetCurrentHand, isInCombat, !isoIsDisabled) that don't hold on the
            // dedicated server out of combat — so a wielded weapon sits in-hand (rhandPid set,
            // the server even attacks WITH it) while gDude's fid keeps weapon-code 0. Result:
            // no fid delta ships → the client renders you unarmed → your first shot is a
            // zero-frame nothing (owner-found: rocket launcher). Recompute the STAND fid with
            // the weapon's animation code; the base (0xFFF) already carries the worn-armor skin
            // (_inven_wield's armor branch sets it). Weapons only — armor changed the base, not
            // the code. Guard on artExists so a base/weapon combo without art is a no-op, not a
            // blank sprite. The in-combat draw path (inventory.cc:469) still owns combat wields.
            if (itemGetType(item) == ITEM_TYPE_WEAPON) {
                int weaponCode = weaponGetAnimationCode(item);
                int standFid = buildFid(OBJ_TYPE_CRITTER, gDude->fid & 0xFFF, ANIM_STAND, weaponCode, gDude->rotation + 1);
                if (artExists(standFid) && gDude->fid != standFid) {
                    objectSetFid(gDude, standFid, nullptr);
                    objectSetFrame(gDude, 0, nullptr);
                }
            }
            fprintf(stderr, "f2_server: control invwield pid=%d hand=%d\n", pid, hand);
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                Object* h2 = critterGetItem2(gDude);
                fprintf(stderr, "[dude-equip] pid=%d type=%d hand=%d dudeFid=0x%x rhandPid=%d inCombat=%d\n",
                    pid, itemGetType(item), hand, gDude->fid, h2 ? h2->pid : -1, isInCombat() ? 1 : 0);
            }
        } else { // invdrop — drop the whole top-level stack (partial drops = later)
            // Dropping WORN armor must also strip its AC bonus (itemDropStack only moves
            // the object) — same _adjust_ac the invunwield-armor path uses.
            if (itemGetType(item) == ITEM_TYPE_ARMOR && (item->flags & OBJECT_WORN) != 0) {
                _adjust_ac(gDude, item, nullptr);
            }
            itemDropStack(gDude, item, stackQty);
            fprintf(stderr, "f2_server: control invdrop pid=%d qty=%d\n", pid, stackQty);
        }
        return;
    }

    // -- Loot-container verbs (player-UI loot slice: take / put / takeall) --
    // `take <containerNetId> <pid>` moves a pid stack from the target into the
    // dude; `put <containerNetId> <pid>` moves it the other way; `takeall
    // <containerNetId>` moves every stack the dude can carry into the dude (the
    // loot "Take All" button). The viewer's loot screen reroutes ALL of its
    // transfers here instead of mutating the local mirror (which would fight
    // server-authoritative inventory — a local move makes client-only phantom
    // items the server never has, so they vanish on the next reconcile). Both the
    // dude and the container have their top-level inventory streamed, so the
    // move rides OBJECT_DELTA_INVENTORY back to every viewer (Slice-2 reconcile).
    //
    // Claimant-gated. NO per-container lock — the single-threaded server
    // serializes concurrent take/put from multiple viewers looting the SAME
    // container (two players sharing a loot pile is intended, not a race). The
    // whole matched top-level stack moves (partial quantities = later); items
    // are addressed by pid (obj->id is non-unique on the wire, nested container
    // contents are not streamed yet). itemMove enforces the weight/size caps and
    // returns -1 when the destination cannot hold the stack — a faithful refusal,
    // the item simply stays put and the mirror reflects reality.
    bool isLootVerb = strcmp(verb, "take") == 0 || strcmp(verb, "put") == 0
        || strcmp(verb, "takeall") == 0;
    if (isLootVerb) {
        // Out-of-combat only (trust-boundary defense-in-depth, review L1/L2). The loot
        // modal force-closes on combat entry so this is not UI-reachable, but a racing/
        // malicious client must not transfer items mid-fight — and the loot-target full-
        // reconcile's "no attack replay references these items" safety rests on exactly
        // this gate, not on the object itself.
        if (isInCombat()) {
            fprintf(stderr, "f2_server: control %s dropped (in combat)\n", verb);
            serverControlRefuse(sessionId, "You can't move items around in combat.");
            return;
        }
        if (gDude == nullptr) {
            return;
        }
        int netId = n >= 2 ? arg : -1;
        int pid = n >= 3 ? arg2 : -1;
        Object* container = objectFindByNetId(netId);
        // Validate at the trust boundary: the target must exist, be on the dude's
        // elevation, be adjacent (vanilla only opens a loot screen after walking
        // the dude next to it), and be an actual loot target — a container ITEM or
        // a downed critter (a live critter is pickpocket/steal, a separate path).
        if (container == nullptr || container->elevation != gDude->elevation) {
            fprintf(stderr, "f2_server: control %s bad target netId=%d ignored\n", verb, netId);
            serverControlRefuse(sessionId, "That is no longer there.");
            return;
        }
        bool isContainer = PID_TYPE(container->pid) == OBJ_TYPE_ITEM
            && itemGetType(container) == ITEM_TYPE_CONTAINER;
        bool isCorpse = PID_TYPE(container->pid) == OBJ_TYPE_CRITTER && critterIsDead(container);
        if (!isContainer && !isCorpse) {
            fprintf(stderr, "f2_server: control %s target netId=%d not a loot container/corpse\n", verb, netId);
            serverControlRefuse(sessionId, "There is nothing there to loot.");
            return;
        }
        if (objectGetDistanceBetween(gDude, container) > 1) {
            // Print BOTH tiles and the distance, not just the verdict. "not adjacent"
            // alone cannot distinguish the two causes that produce it: the player
            // genuinely walked away, or the viewer is DRAWING the target somewhere it
            // is not (a stale glide offset renders a sprite up to a tile off, so the
            // player stands where the body looks and the server disagrees). Owner hit
            // the second case — loot screen opened, put refused, moving elsewhere then
            // worked — and neither log could tell them apart. The client-side twin of
            // this line is in main.cc viewerPollPendingLoot.
            fprintf(stderr,
                "f2_server: control %s target netId=%d not adjacent"
                " (actor tile=%d elev=%d, target tile=%d elev=%d, dist=%d)\n",
                verb, netId, gDude->tile, gDude->elevation,
                container->tile, container->elevation,
                objectGetDistanceBetween(gDude, container));
            // ►► THE OWNER'S OWN EXAMPLE of why this whole item exists: the loot
            // screen opened, the drag was refused, and moving elsewhere then worked
            // — with nothing on screen to distinguish "you walked away" from "your
            // viewer is drawing the body a tile off its real position" (the stale
            // glide offset, #10). The player at least now knows WHICH rule stopped
            // them, and that walking closer is the thing to try.
            serverControlRefuse(sessionId, "You are too far away.");
            return;
        }

        if (strcmp(verb, "takeall") == 0) {
            // Move every stack the dude can carry from the container into the dude.
            // itemMove removes the moved stack from the container (its inventory
            // shrinks), so DON'T advance the index on success; on a cap failure the
            // stack stays, so advance past it. Terminates: each pass either shrinks
            // the container or advances the index.
            Inventory* inv = &container->data.inventory;
            int moved = 0;
            int i = 0;
            while (i < inv->length) {
                Object* it = inv->items[i].item;
                int qty = inv->items[i].quantity;
                if (it != nullptr && itemMove(container, gDude, it, qty) == 0) {
                    moved++;
                } else {
                    i++;
                }
            }
            fprintf(stderr, "f2_server: control takeall netId=%d moved=%d left=%d\n", netId, moved, inv->length);
            if (inv->length > 0) {
                // itemMove refused these on the weight/size caps. Vanilla's TAKE-ALL
                // leaves them behind just as silently, but here the player is looking
                // at a REMOTE mirror of the container and a leftover stack reads as a
                // sync failure rather than a full pack.
                serverControlRefuse(sessionId, "You can't carry it all.");
            }
            return;
        }

        Object* from = strcmp(verb, "take") == 0 ? container : gDude;
        Object* to = strcmp(verb, "take") == 0 ? gDude : container;
        Object* item = nullptr;
        int stackQty = 0;
        {
            Inventory* inv = &from->data.inventory;
            for (int i = 0; i < inv->length; i++) {
                if (inv->items[i].item != nullptr && inv->items[i].item->pid == pid) {
                    item = inv->items[i].item;
                    stackQty = inv->items[i].quantity;
                    break;
                }
            }
        }
        if (item == nullptr) {
            fprintf(stderr, "f2_server: control %s no pid=%d in source netId=%d ignored\n", verb, pid, netId);
            // The literal log line quoted in bugs-list U. Its live cause is usually
            // another player having already taken the stack out of the same pile.
            serverControlRefuse(sessionId, "That item is gone.");
            return;
        }
        // arg3 = quantity from the viewer's drag quantity-select; default (or an
        // out-of-range value) moves the whole matched stack. Clamp to [1, stackQty].
        int qty = n >= 4 ? arg3 : stackQty;
        if (qty <= 0 || qty > stackQty) {
            qty = stackQty;
        }
        int rc = itemMove(from, to, item, qty);
        fprintf(stderr, "f2_server: control %s netId=%d pid=%d qty=%d rc=%d\n", verb, netId, pid, qty, rc);
        if (rc != 0) {
            // itemMove enforces the weight/size caps and returns -1 when the
            // destination cannot hold the stack. The item stays put and the next
            // reconcile snaps the viewer's drag back — which, unannounced, looks
            // exactly like the drop was lost in transit.
            serverControlRefuse(sessionId, strcmp(verb, "take") == 0
                    ? "You can't carry any more."
                    : "It won't fit in there.");
        }
        return;
    }

    // -- Combat verbs (P3) --------------------------------------------------
    // In-combat counterparts of mv. Rather than driving the sim directly, these
    // ENQUEUE a dude combat intent (combat_intent.h); the resumable-combat player
    // barrier (combat.cc combatSessionAdvance) drains the queue on the dude's turn
    // via combatServerPumpIntents — the exact same entry points the debug CMD port
    // and the AI use. They are claimant-only and in-combat-only (out of combat the
    // click path uses mv); an intent queued during an AI turn simply waits for the
    // dude's barrier (pre-input, like the debug harness). This is the whole of the
    // wire→barrier binding: the barrier already waits on serverControlHasClaimant.
    bool isCombatVerb = strcmp(verb, "cattack") == 0
        || strcmp(verb, "cmove") == 0
        || strcmp(verb, "cendturn") == 0
        || strcmp(verb, "cendcombat") == 0;
    if (isCombatVerb) {
        if (!isInCombat()) {
            fprintf(stderr, "f2_server: control %s dropped (not in combat)\n", verb);
            // The MIRROR IMAGE of the mv refusal above, and the other half of the
            // same desync: a client that believes it is still fighting sends combat
            // verbs at a server that has left combat. Both directions now speak.
            serverControlRefuse(sessionId, "You are not in combat.");
            return;
        }

        // WHOSE order this is. The queue is shared by every player and the turn
        // barrier consumes only the acting slot's entries, so without this stamp
        // one player's attack is spent on whoever's turn happens to be open
        // (MP_PROPOSAL Ch 8.4). The debug CMD port has no session and drives the
        // host, which is slot 0 — the same default every existing caller gets.
        int actorSlot = serverControlSlotForSession(sessionId);
        if (actorSlot < 0) {
            actorSlot = 0;
        }

        if (strcmp(verb, "cattack") == 0) {
            // `cattack <netId> [hitMode] [hitLocation]`. arg = target netId, or
            // (absent) -1 = nearest hostile (debug-port convenience) — resolved by
            // serverResolveTarget in combat_drain.cc. arg2 = hit mode (the viewer's
            // interface-bar hand/aim selection), default AUTO = server picks. arg3 =
            // aimed hit location, default UNCALLED = normal shot. The server always
            // re-validates the mode/location via _combat_check_bad_shot.
            int target = n >= 2 ? arg : -1;
            int hitMode = n >= 3 ? arg2 : COMBAT_INTENT_HITMODE_AUTO;
            int hitLocation = n >= 4 ? arg3 : HIT_LOCATION_UNCALLED;
            // SECURITY: hitLocation is an UNTRUSTED wire value that ends up indexing
            // fixed [HIT_LOCATION_COUNT] tables inside _combat_attack (hit_location_
            // penalty, gCriticalHitTables) — _combat_check_bad_shot validates the
            // aiming AP cost but NOT the location. An out-of-range value would read
            // out of bounds on the one authoritative sim (session-wide crash). Reject
            // anything outside [HEAD, UNCALLED] down to a normal unaimed shot; hitMode
            // needs no such clamp (item.cc switches default to null → unarmed).
            if (hitLocation < 0 || hitLocation >= HIT_LOCATION_COUNT) {
                fprintf(stderr, "f2_server: control cattack bad hitLoc=%d → UNCALLED\n", hitLocation);
                hitLocation = HIT_LOCATION_UNCALLED;
            }
            combatIntentPush(COMBAT_INTENT_ATTACK, target, hitLocation, false, hitMode, actorSlot);
            fprintf(stderr, "f2_server: control cattack target=%d hitMode=%d hitLoc=%d slot=%d\n",
                target, hitMode, hitLocation, actorSlot);
        } else if (strcmp(verb, "cmove") == 0) {
            if (n < 2 || !tileIsValid(arg)) {
                fprintf(stderr, "f2_server: control cmove bad tile=%d (grid=%d) ignored\n",
                    n >= 2 ? arg : -1, gHexGridSize);
                return;
            }
            // arg2 = run flag (like mv). Same AP either way; run is a faster anim.
            combatIntentPush(COMBAT_INTENT_MOVE, arg, HIT_LOCATION_UNCALLED, arg2 != 0,
                COMBAT_INTENT_HITMODE_AUTO, actorSlot);
            fprintf(stderr, "f2_server: control cmove tile=%d run=%d slot=%d\n",
                arg, arg2 != 0 ? 1 : 0, actorSlot);
        } else if (strcmp(verb, "cendcombat") == 0) {
            // Vanilla RETURN: attempt to end combat (drain calls combatAttemptEnd,
            // which refuses with a streamed console message if hostiles remain).
            combatIntentPush(COMBAT_INTENT_END_COMBAT, 0, HIT_LOCATION_UNCALLED, false,
                COMBAT_INTENT_HITMODE_AUTO, actorSlot);
            fprintf(stderr, "f2_server: control cendcombat slot=%d\n", actorSlot);
        } else { // cendturn
            combatIntentPush(COMBAT_INTENT_END_TURN, 0, HIT_LOCATION_UNCALLED, false,
                COMBAT_INTENT_HITMODE_AUTO, actorSlot);
            fprintf(stderr, "f2_server: control cendturn slot=%d\n", actorSlot);
        }
        return;
    }

    fprintf(stderr, "f2_server: control unknown verb '%s' from session %d ignored\n",
        verb, sessionId);
}

} // namespace fallout
