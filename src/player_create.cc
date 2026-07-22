#include "player_create.h"

#include <stdio.h>
#include <stdlib.h>

#include "critter.h"
#include "debug.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "server_players.h"
#include "skill.h"
#include "stat.h"
#include "trait.h"

namespace fallout {

// The seven SPECIAL stats are contiguous from STAT_STRENGTH; the spec stores them
// in that order, so the index IS the stat id offset.
static constexpr int kSpecialCount = 7;

void playerCreateSpecDefaults(PlayerCreateSpec* spec)
{
    if (spec == nullptr) {
        return;
    }

    for (int i = 0; i < kSpecialCount; i++) {
        spec->special[i] = 5; // vanilla's starting value for every SPECIAL
    }

    for (int i = 0; i < NUM_TAGGED_SKILLS; i++) {
        spec->tagged[i] = -1;
    }

    for (int i = 0; i < TRAITS_MAX_SELECTED_COUNT; i++) {
        spec->traits[i] = -1;
    }
}

bool playerCreateSpecValidate(const PlayerCreateSpec* spec)
{
    if (spec == nullptr) {
        return false;
    }

    for (int i = 0; i < kSpecialCount; i++) {
        if (spec->special[i] < PRIMARY_STAT_MIN || spec->special[i] > PRIMARY_STAT_MAX) {
            debugPrint("player_create: SPECIAL[%d]=%d out of range %d..%d\n",
                i, spec->special[i], PRIMARY_STAT_MIN, PRIMARY_STAT_MAX);
            return false;
        }
    }

    for (int i = 0; i < NUM_TAGGED_SKILLS; i++) {
        int skill = spec->tagged[i];
        if (skill == -1) {
            continue;
        }

        if (skill < 0 || skill >= SKILL_COUNT) {
            debugPrint("player_create: tagged skill %d out of range\n", skill);
            return false;
        }

        for (int j = 0; j < i; j++) {
            if (spec->tagged[j] == skill) {
                debugPrint("player_create: duplicate tagged skill %d\n", skill);
                return false;
            }
        }
    }

    for (int i = 0; i < TRAITS_MAX_SELECTED_COUNT; i++) {
        int trait = spec->traits[i];
        if (trait == -1) {
            continue;
        }

        if (trait < 0 || trait >= TRAIT_COUNT) {
            debugPrint("player_create: trait %d out of range\n", trait);
            return false;
        }

        for (int j = 0; j < i; j++) {
            if (spec->traits[j] == trait) {
                debugPrint("player_create: duplicate trait %d\n", trait);
                return false;
            }
        }
    }

    return true;
}

int playerCreateApply(int slot, const PlayerCreateSpec* spec)
{
    // Slot 0 IS allowed. The first player to log in by name binds the host slot
    // (nobody can drive the worldmap otherwise — host-only screens), so "create my
    // character" has to be able to land on the host body. The pid check below is
    // what keeps this honest: it still refuses to write a row that is not this
    // slot's. What protects an ESTABLISHED host character is one level up — an
    // existing account never carries a creation spec (server_control.cc).
    if (spec == nullptr || slot < 0 || slot >= kMaxPlayerActors) {
        return -1;
    }

    if (!playerCreateSpecValidate(spec)) {
        return -1;
    }

    Object* actor = playerActorAt(slot);
    if (actor == nullptr) {
        debugPrint("player_create: no actor in slot %d\n", slot);
        return -1;
    }

    // ⚠ Resolve the row through the PID, never a cached pointer, and check that
    // the row we got is this slot's — slot 0's row IS gDudeProto, so applying a
    // creation spec to the wrong row would rewrite the HOST's live character
    // (PLAYER_SHEET_DESIGN.md §7 risk 1).
    if (actor->pid != playerActorSheetPid(slot)) {
        debugPrint("player_create: slot %d actor pid 0x%X is not the slot's sheet pid 0x%X\n",
            slot, actor->pid, playerActorSheetPid(slot));
        return -1;
    }

    Proto* proto = nullptr;
    if (protoGetProto(actor->pid, &proto) == -1 || proto == nullptr) {
        return -1;
    }

    // Take the HOST's character back off the row. The spawn path seeded this slot
    // from the host so the non-sheet fields (fid, messageId, flags, AI packet) are
    // correct; everything the host EARNED has to go before the new character's own
    // numbers land, or a created character silently starts with the host's perks,
    // invested skill points and XP.
    protoCritterDataResetStats(&(proto->critter.data));
    protoCritterDataResetSkills(&(proto->critter.data));
    perkPlayerActorClearRanksSlot(slot);

    pcSetStat(PC_STAT_LEVEL, 1, actor);
    pcSetStat(PC_STAT_EXPERIENCE, 0, actor);
    pcSetStat(PC_STAT_KARMA, 0, actor);
    pcSetStat(PC_STAT_REPUTATION, 0, actor);
    pcSetStat(PC_STAT_UNSPENT_SKILL_POINTS, 0, actor);

    // ►► TRAITS FIRST, BEFORE THE SPECIAL WRITES. critterSetBaseStat subtracts the
    // subject's CURRENT trait modifier from the value it is given (stat.cc: a
    // displayed 10 with Gifted is stored as base 9) and then REFUSES anything that
    // falls outside the stat's min/max instead of clamping. With the host's seeded
    // traits still in place, the new character's numbers are silently distorted by
    // the HOST's build: against a Gifted host a requested 10 stored 9, and a
    // requested 1 became 0, was rejected outright, and left the reset default 5.
    // Establishing this character's own traits first makes the subtraction the
    // vanilla one the character editor performs.
    traitsSetSelected(spec->traits[0], spec->traits[1], actor);

    for (int i = 0; i < kSpecialCount; i++) {
        int stat = STAT_STRENGTH + i;
        int rc = critterSetBaseStat(actor, stat, spec->special[i]);
        if (rc != 0) {
            // ⚠ Never let this pass quietly: the stat keeps whatever the reset left
            // it at, so the player silently gets a different character than the one
            // they asked for. Validation upstream should make this unreachable.
            debugPrint("player_create: slot %d stat %d = %d REJECTED (rc=%d)\n",
                slot, stat, spec->special[i], rc);
            return -1;
        }
    }

    // Tags take the subject explicitly, so they land on this actor's row rather
    // than the host's globals.
    int tagged[NUM_TAGGED_SKILLS];
    for (int i = 0; i < NUM_TAGGED_SKILLS; i++) {
        tagged[i] = spec->tagged[i];
    }
    skillsSetTagged(tagged, NUM_TAGGED_SKILLS, actor);

    // Finish exactly the way _proto_dude_init finishes a freshly loaded character:
    // derived stats follow from the SPECIAL just written, and the heal takes
    // current HP up to the NEW maximum (the seeded value was the host's).
    critterUpdateDerivedStats(actor);
    critterAdjustHitPoints(actor, 10000);

    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        // READ BACK what actually landed. The applier writes through resolvers
        // that dispatch on the actor's pid, so "did the write reach THIS slot's
        // row" is the question worth answering out loud — inferring it from HP
        // downstream is how a silently-ignored write gets blessed.
        fprintf(stderr, "[create] slot=%d SPECIAL:", slot);
        for (int i = 0; i < kSpecialCount; i++) {
            fprintf(stderr, " %d", critterGetStat(actor, STAT_STRENGTH + i));
        }
        fprintf(stderr, " maxhp=%d hp=%d\n",
            critterGetStat(actor, STAT_MAXIMUM_HIT_POINTS), critterGetHitPoints(actor));
    }

    return 0;
}

} // namespace fallout
