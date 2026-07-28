#include "sheet_intent.h"

#include <stdio.h>

#include "game.h"
#include "perk.h"
#include "player_sheet.h"
#include "server_players.h"
#include "skill.h"
#include "skill_defs.h"
#include "stat.h"
#include "trait.h"
#include "trait_defs.h"

namespace fallout {

// One open character screen, per player actor. Transient SERVER state: never
// persisted, never on the wire, never on an Object — a reconnect simply opens a
// new one. Slot 0 is included, so the host's own screen obeys the same rules.
struct SheetEditSession {
    bool open;
    // Skill values as they were when the screen opened — the floor "-" may walk
    // back to (see sheet_intent.h #4).
    int skillBaseline[SKILL_COUNT];
    // A perk pick that is IN but owes its follow-up choice, and which perk owes
    // it. Remembered here so a `tagpick`/`mutpick` arriving out of the blue cannot
    // be used to re-tag a skill or swap a trait at will.
    int pendingChoice;
    int pendingPerk;
};

static SheetEditSession gSessions[kMaxPlayerActors];

static SheetEditSession* sessionFor(Object* actor)
{
    if (actor == nullptr) {
        return nullptr;
    }

    int slot = playerActorSlotOf(actor);
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return nullptr;
    }

    return &(gSessions[slot]);
}

void sheetEditSessionOpen(Object* actor)
{
    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr) {
        return;
    }

    // Baselines are read the way the editor's own undo reads them (skillGetValue,
    // modifiers included), so "back to where I opened the screen" means the same
    // number the player is looking at.
    ServerActorScope scope(actor);
    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        session->skillBaseline[skill] = skillGetValue(actor, skill);
    }
    session->open = true;

    // A pending follow-up choice deliberately SURVIVES a reopen: the perk is
    // already applied, so dropping the obligation here would strand an actor with
    // Tag! and three tagged skills.
}

void sheetEditSessionEnsure(Object* actor)
{
    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr || session->open) {
        return;
    }

    sheetEditSessionOpen(actor);
}

void sheetEditSessionClose(Object* actor)
{
    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr) {
        return;
    }

    session->open = false;
}

int sheetEditSkillUp(Object* actor, int skill)
{
    if (actor == nullptr) {
        return kSheetEditNoActor;
    }

    if (!skillIsValid(skill)) {
        return kSheetEditBadIndex;
    }

    // ►► THE SCOPE IS THE FIX. skillAdd opens with `if (obj != gDude) return -5`
    // and reads/writes PC_STAT_UNSPENT_SKILL_POINTS with no subject; rebinding gDude
    // to this actor makes the guard pass and puts the budget in the right row.
    ServerActorScope scope(actor);

    int rc = skillAdd(actor, skill);
    switch (rc) {
    case 0:
        // The skill value lands in the actor's proto row directly, not through a
        // hooked setter — stream it (pcSetStat marked the row too; whole-row emits
        // coalesce, so marking twice costs nothing).
        //
        // Spending the last point puts the level-up badge out. Vanilla's only clear
        // path is closing the character screen, which the authority does not have.
        pcLevelUpBadgeRefresh(actor);
        playerSheetMarkDirty(actor);
        return kSheetEditOk;
    case -3:
        return kSheetEditAtCap;
    case -4:
        return kSheetEditNoPoints;
    default:
        return kSheetEditRefused;
    }
}

int sheetEditSkillDown(Object* actor, int skill)
{
    if (actor == nullptr) {
        return kSheetEditNoActor;
    }

    if (!skillIsValid(skill)) {
        return kSheetEditBadIndex;
    }

    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr || !session->open) {
        return kSheetEditNoSession;
    }

    ServerActorScope scope(actor);

    // ►► THE POINT FOUNTAIN GUARD. Without the baseline a player could walk a
    // starting skill down and pocket refunds for points they were never awarded.
    if (skillGetValue(actor, skill) <= session->skillBaseline[skill]) {
        return kSheetEditAtBaseline;
    }

    int rc = skillSub(actor, skill);
    switch (rc) {
    case 0:
        // A refund re-lights the badge: there is a point to spend again.
        pcLevelUpBadgeRefresh(actor);
        playerSheetMarkDirty(actor);
        return kSheetEditOk;
    case -2:
        return kSheetEditAtBaseline;
    default:
        return kSheetEditRefused;
    }
}

int sheetEditPerkPick(Object* actor, int perk, int* pendingChoice)
{
    if (pendingChoice != nullptr) {
        *pendingChoice = PERK_CHOICE_PENDING_NONE;
    }

    if (actor == nullptr) {
        return kSheetEditNoActor;
    }

    if (!perkIsValid(perk)) {
        return kSheetEditBadIndex;
    }

    SheetEditSession* session = sessionFor(actor);
    if (session != nullptr && session->pendingChoice != PERK_CHOICE_PENDING_NONE) {
        return kSheetEditChoiceOwed;
    }

    // ►► ENTITLEMENT, checked before anything mutates. perkAdd/perkCanAdd know
    // about prerequisites and rank caps but nothing about being OWED a pick.
    if (!perkOwedPickGet(actor)) {
        return kSheetEditNoOwedPick;
    }

    // perkCanAdd's level requirement is written `if (critter == gDude)`, and
    // perkChoiceApply's Educated bonus reads the unspent-point row with no
    // subject — both correct only inside the scope.
    ServerActorScope scope(actor);

    // perkChoiceApply detects Tag!/Mutate!/Lifegiver/Educated by comparing against
    // the ranks as they were before the pick, exactly as the perk dialog does.
    int ranksBefore[PERK_COUNT];
    for (int index = 0; index < PERK_COUNT; index++) {
        ranksBefore[index] = perkGetRank(actor, index);
    }

    int pending = PERK_CHOICE_PENDING_NONE;
    if (perkChoiceApply(actor, perk, ranksBefore, &pending) == -1) {
        return kSheetEditPrereq;
    }

    perkOwedPickAdd(actor, -1); // spend ONE — the actor may still owe more
    // The pick is spent — and Educated may have just added points, so re-derive
    // rather than assume the badge goes out.
    pcLevelUpBadgeRefresh(actor);
    playerSheetMarkDirty(actor);

    if (session != nullptr) {
        session->pendingChoice = pending;
        session->pendingPerk = perk;
    }

    if (pendingChoice != nullptr) {
        *pendingChoice = pending;
    }

    return kSheetEditOk;
}

// Cancelling an owed follow-up takes the perk back off — vanilla's own behaviour
// when the Tag!/Mutate! dialog is escaped (character_editor.cc). We also hand the
// PICK back, which vanilla does not: it cleared the owed flag the moment the perk
// applied, so escaping the follow-up lost the level's perk outright. On a
// dedicated server that is a support ticket, not a quirk.
static int sheetEditChoiceCancel(Object* actor, SheetEditSession* session, int perk)
{
    ServerActorScope scope(actor);
    perkRemove(actor, perk);
    perkOwedPickAdd(actor, 1); // hand the cancelled pick back, without erasing others
    pcLevelUpBadgeRefresh(actor);
    session->pendingChoice = PERK_CHOICE_PENDING_NONE;
    session->pendingPerk = -1;
    playerSheetMarkDirty(actor);
    return kSheetEditOk;
}

int sheetEditTagPick(Object* actor, int skill)
{
    if (actor == nullptr) {
        return kSheetEditNoActor;
    }

    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr || session->pendingChoice != PERK_CHOICE_PENDING_TAG) {
        return kSheetEditNoChoiceOwed;
    }

    if (skill == -1) {
        return sheetEditChoiceCancel(actor, session, PERK_TAG);
    }

    if (!skillIsValid(skill)) {
        return kSheetEditBadIndex;
    }

    ServerActorScope scope(actor);

    int tagged[NUM_TAGGED_SKILLS];
    skillsGetTagged(tagged, NUM_TAGGED_SKILLS, actor);
    for (int index = 0; index < NUM_TAGGED_SKILLS; index++) {
        if (tagged[index] == skill) {
            return kSheetEditAlreadyTagged;
        }
    }

    // Writes slot 3 and commits. skillsSetTagged underneath takes no subject here,
    // so the scope is what puts the tag on this actor's row rather than the host's.
    skillsTagPerkApply(tagged, skill);

    session->pendingChoice = PERK_CHOICE_PENDING_NONE;
    session->pendingPerk = -1;
    playerSheetMarkDirty(actor);

    return kSheetEditOk;
}

int sheetEditMutatePick(Object* actor, int dropTrait, int gainTrait)
{
    if (actor == nullptr) {
        return kSheetEditNoActor;
    }

    SheetEditSession* session = sessionFor(actor);
    if (session == nullptr || session->pendingChoice != PERK_CHOICE_PENDING_MUTATE) {
        return kSheetEditNoChoiceOwed;
    }

    if (dropTrait == -1 && gainTrait == -1) {
        return sheetEditChoiceCancel(actor, session, PERK_MUTATE);
    }

    ServerActorScope scope(actor);

    int traits[TRAITS_MAX_SELECTED_COUNT];
    traitsGetSelected(&(traits[0]), &(traits[1]), actor);

    // Addressed by TRAIT ID, not by the dialog's line number: the perk dialog sorts
    // its list alphabetically, so a line index would make the protocol depend on the
    // client's rendering order (and on the message-file language).
    if (dropTrait != -1) {
        if (dropTrait < 0 || dropTrait >= TRAIT_COUNT) {
            return kSheetEditBadIndex;
        }
        if (traits[0] == dropTrait) {
            traits[0] = traits[1];
            traits[1] = -1;
        } else if (traits[1] == dropTrait) {
            traits[1] = -1;
        } else {
            return kSheetEditBadIndex; // not one of this actor's traits
        }
    }

    if (gainTrait != -1) {
        if (gainTrait < 0 || gainTrait >= TRAIT_COUNT) {
            return kSheetEditBadIndex;
        }
        if (traits[0] == gainTrait || traits[1] == gainTrait) {
            return kSheetEditBadIndex; // already has it
        }
        if (traits[0] == -1) {
            traits[0] = gainTrait;
        } else if (traits[1] == -1) {
            traits[1] = gainTrait;
        } else {
            return kSheetEditRefused; // both slots full and nothing dropped
        }
    }

    // Trait modifiers are applied at READ time (traitGetStatModifier), so there is
    // no derived-stat table to recompute here — the next critterGetStat sees them.
    traitsSetSelected(traits[0], traits[1], actor);

    session->pendingChoice = PERK_CHOICE_PENDING_NONE;
    session->pendingPerk = -1;
    playerSheetMarkDirty(actor);

    return kSheetEditOk;
}

const char* sheetEditReason(int result)
{
    switch (result) {
    case kSheetEditOk:
        return "Done.";
    case kSheetEditNoActor:
        return "You have no character to edit.";
    case kSheetEditBadIndex:
        return "That is not something you can pick.";
    case kSheetEditNoPoints:
        return "Not enough skill points available.";
    case kSheetEditAtCap:
        return "That skill is at its maximum.";
    case kSheetEditNoOwedPick:
        return "You have no perk to pick right now.";
    case kSheetEditPrereq:
        return "You don't meet the requirements for that perk.";
    case kSheetEditAtBaseline:
        return "You can't lower it any further.";
    case kSheetEditNoSession:
        return "Open your character sheet first.";
    case kSheetEditChoiceOwed:
        return "Finish choosing first.";
    case kSheetEditNoChoiceOwed:
        return "Nothing is waiting on that choice.";
    case kSheetEditAlreadyTagged:
        return "That skill is already tagged.";
    default:
        return "You can't do that.";
    }
}

} // namespace fallout
