#include "state_dump.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <vector>

#include "critter.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "object.h"
#include "proto_types.h"
#include "queue.h"
#include "random.h"
#include "scripts.h"
#include "server_players.h"
#include "skill.h"
#include "stat.h"
#include "trait.h"
#include "worldmap.h"
#include "server_loop.h"

namespace fallout {

struct StateDumpCheckpoint {
    int tick;
    unsigned int gameTime;
    unsigned int rngFingerprint;
};

static std::vector<StateDumpCheckpoint> gStateDumpCheckpoints;

void stateDumpRecordCheckpoint(int tick)
{
    gStateDumpCheckpoints.push_back({ tick, gameTimeGetTime(), randomStateFingerprint() });
}

static void dumpInventory(FILE* f, Object* owner)
{
    Inventory* inventory = &(owner->data.inventory);
    if (inventory->length == 0) {
        return;
    }

    // Copy and sort by (pid, quantity) for deterministic ordering.
    std::vector<InventoryItem> items(inventory->items, inventory->items + inventory->length);
    std::sort(items.begin(), items.end(), [](const InventoryItem& a, const InventoryItem& b) {
        if (a.item->pid != b.item->pid) return a.item->pid < b.item->pid;
        return a.quantity < b.quantity;
    });

    for (const InventoryItem& entry : items) {
        fprintf(f, "  inv pid=0x%08X qty=%d flags=0x%08X",
            entry.item->pid,
            entry.quantity,
            entry.item->flags);

        int itemType = itemGetType(entry.item);
        if (itemType == ITEM_TYPE_WEAPON) {
            fprintf(f, " charges=%d", entry.item->data.item.weapon.ammoQuantity);
        } else if (itemType == ITEM_TYPE_AMMO) {
            fprintf(f, " charges=%d", ammoGetQuantity(entry.item));
        }

        fprintf(f, "\n");
    }
}

static void dumpObject(FILE* f, Object* obj)
{
    if (getenv("F2_SERVER_LOOP") != nullptr || getenv("F2_SERVER_SERVE") != nullptr
        || getenv("F2_SERVER_BLOB_OUT") != nullptr || getenv("F2_CLIENT_BLOB_IN") != nullptr
        || getenv("F2_CLIENT_STREAM_IN") != nullptr) {
        fprintf(f, "obj id=%d pid=0x%08X tile=%d elev=%d rot=%d flags=0x%08X sid=%d script_idx=%d netid=%d\n",
            obj->id,
            obj->pid,
            obj->tile,
            obj->elevation,
            obj->rotation,
            obj->flags,
            obj->sid,
            obj->scriptIndex,
            obj->netId);
    } else {
        fprintf(f, "obj id=%d pid=0x%08X tile=%d elev=%d rot=%d flags=0x%08X sid=%d script_idx=%d\n",
            obj->id,
            obj->pid,
            obj->tile,
            obj->elevation,
            obj->rotation,
            obj->flags,
            obj->sid,
            obj->scriptIndex);
    }

    if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
        Object* whoHitMe = obj->data.critter.combat.whoHitMe;
        fprintf(f, "  critter hp=%d rad=%d poison=%d ap=%d team=%d ai=%d maneuver=%d results=0x%X who_hit_me=%d\n",
            obj->data.critter.hp,
            obj->data.critter.radiation,
            obj->data.critter.poison,
            obj->data.critter.combat.ap,
            obj->data.critter.combat.team,
            obj->data.critter.combat.aiPacket,
            obj->data.critter.combat.maneuver,
            obj->data.critter.combat.results,
            whoHitMe != nullptr ? whoHitMe->id : -1);
    }

    dumpInventory(f, obj);
}

bool stateDumpWrite(const char* path)
{
    FILE* f = fopen(path, "w");
    if (f == nullptr) {
        return false;
    }

    fprintf(f, "== F2CE STATE DUMP v1 ==\n");
    fprintf(f, "game_time %u\n", gameTimeGetTime());
    // RNG tripwire: any extra or missing roll anywhere in the run shifts this
    // fingerprint, exposing silent-divergence bugs (H-5/H-34 class) before
    // they manifest in world state.
    fprintf(f, "rng_state 0x%08X\n", randomStateFingerprint());
    for (const StateDumpCheckpoint& checkpoint : gStateDumpCheckpoints) {
        fprintf(f, "checkpoint tick=%d time=%u rng=0x%08X\n",
            checkpoint.tick, checkpoint.gameTime, checkpoint.rngFingerprint);
    }
    gStateDumpCheckpoints.clear();
    fprintf(f, "map %d\n", mapGetCurrentMap());
    fprintf(f, "map_name %s\n", gMapHeader.name);
    fprintf(f, "elevation %d\n", gElevation);

    // Worldmap party state (H-13 travel-sim coverage): world position, matched
    // area, walking flag, car fuel and the visited state of the party's
    // current subtile (H-16 exploration marking).
    {
        int wmX;
        int wmY;
        wmGetPartyWorldPos(&wmX, &wmY);
        int wmArea;
        wmGetPartyCurArea(&wmArea);
        int wmSubtileState;
        wmSubTileGetVisitedState(wmX, wmY, &wmSubtileState);
        fprintf(f, "worldmap pos=%d,%d area=%d walking=%d gas=%d subtile_state=%d\n",
            wmX, wmY, wmArea, wmPartyIsWalking() ? 1 : 0, wmCarGasAmount(), wmSubtileState);
    }

    // Global game variables (GVARs) — only non-zero entries.
    fprintf(f, "gvars_len %d\n", gGameGlobalVarsLength);
    for (int i = 0; i < gGameGlobalVarsLength; i++) {
        if (gGameGlobalVars[i] != 0) {
            fprintf(f, "gvar %d %d\n", i, gGameGlobalVars[i]);
        }
    }

    // Map-global variables (MVARs) — only non-zero entries.
    fprintf(f, "mvars_len %d\n", gMapGlobalVarsLength);
    for (int i = 0; i < gMapGlobalVarsLength; i++) {
        if (gMapGlobalVars[i] != 0) {
            fprintf(f, "mvar %d %d\n", i, gMapGlobalVars[i]);
        }
    }

    // Map-local (script) variables.
    fprintf(f, "lvars_len %d\n", gMapLocalVarsLength);
    for (int i = 0; i < gMapLocalVarsLength; i++) {
        if (gMapLocalVars[i] != 0) {
            fprintf(f, "lvar %d %d\n", i, gMapLocalVars[i]);
        }
    }

    // Player character.
    if (getenv("F2_SERVER_LOOP") != nullptr || getenv("F2_SERVER_SERVE") != nullptr
        || getenv("F2_SERVER_BLOB_OUT") != nullptr || getenv("F2_CLIENT_BLOB_IN") != nullptr
        || getenv("F2_CLIENT_STREAM_IN") != nullptr) {
        fprintf(f, "dude id=%d tile=%d elev=%d hp=%d rad=%d poison=%d netid=%d\n",
            gDude->id,
            gDude->tile,
            gDude->elevation,
            critterGetHitPoints(gDude),
            critterGetRadiation(gDude),
            critterGetPoison(gDude),
            gDude->netId);
    } else {
        fprintf(f, "dude id=%d tile=%d elev=%d hp=%d rad=%d poison=%d\n",
            gDude->id,
            gDude->tile,
            gDude->elevation,
            critterGetHitPoints(gDude),
            critterGetRadiation(gDude),
            critterGetPoison(gDude));
    }
    // Extra player actors (MP_PROPOSAL.md Ch 5.2): they are OBJECT_NO_SAVE, so
    // the object walk below skips them exactly as it skips the dude — without
    // this they would be absent from the oracle while present on the wire, and
    // the replay gates compare against THIS file. Emitted as their own line kind
    // and only when there are any, so a single-actor dump is unchanged byte for
    // byte.
    for (int slot = 1; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            continue;
        }
        fprintf(f, "player_actor %d id=%d tile=%d elev=%d hp=%d rad=%d poison=%d netid=%d\n",
            slot,
            actor->id,
            actor->tile,
            actor->elevation,
            critterGetHitPoints(actor),
            critterGetRadiation(actor),
            critterGetPoison(actor),
            actor->netId);
    }
    for (int stat = 0; stat < SPECIAL_STAT_COUNT; stat++) {
        fprintf(f, "dude_stat %d %d\n", stat, critterGetStat(gDude, stat));
    }
    for (int pcStat = 0; pcStat < PC_STAT_COUNT; pcStat++) {
        fprintf(f, "dude_pcstat %d %d\n", pcStat, pcGetStat(pcStat));
    }

    // Skill values (v1): the SPECIAL stats above do not reflect skills, so
    // skilldex use / book / doctor / first-aid effects and skill-point spends
    // were previously invisible in the dump. Dumped for the dude only.
    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        fprintf(f, "dude_skill %d %d\n", skill, skillGetValue(gDude, skill));
    }

    // Kill counters by critter type (v1): a kill is otherwise only visible as a
    // corpse flag flip on the specific object, which is easy to miss. This makes
    // damage / explosion / combat kills explicit and self-documenting in the
    // diff — the correctness signal whose absence let a scripted-kill divergence
    // (dropped XP / destroy proc) get blessed. Non-zero entries only.
    for (int killType = 0; killType < KILL_TYPE_COUNT; killType++) {
        int killCount = killsGetByType(killType);
        if (killCount != 0) {
            fprintf(f, "kills %d %d\n", killType, killCount);
        }
    }

    // Per-skill uses-today counter (v1): the anti-spam cooldown state (first aid
    // / doctor limited to N uses/day). A skill-use decouple that applies the
    // effect but forgets skillUpdateLastUse would leave this at 0 -> the skill
    // stays spammable with no cooldown; dumped so that divergence is visible.
    // Non-zero entries only.
    for (int skill = 0; skill < SKILL_COUNT; skill++) {
        int uses = skillGetUsesToday(skill);
        if (uses != 0) {
            fprintf(f, "skill_uses_today %d %d\n", skill, uses);
        }
    }

    // Tagged skills and selected traits (H-47/H-48 coverage): the Tag!/
    // Mutate! perk commits are otherwise invisible in the dump.
    {
        int taggedSkills[NUM_TAGGED_SKILLS];
        skillsGetTagged(taggedSkills, NUM_TAGGED_SKILLS);
        fprintf(f, "dude_tags %d %d %d %d\n", taggedSkills[0], taggedSkills[1], taggedSkills[2], taggedSkills[3]);

        int trait1;
        int trait2;
        traitsGetSelected(&trait1, &trait2);
        fprintf(f, "dude_traits %d %d\n", trait1, trait2);
    }

    dumpInventory(f, gDude);

    // All world objects, sorted canonically. NOTE v0: inventories dumped one
    // level deep (no nested containers yet). OBJECT_NO_SAVE objects (mouse
    // cursor objects, egg, dude) are skipped except dude which is dumped above.
    std::vector<Object*> objects;
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if ((obj->flags & OBJECT_NO_SAVE) == 0) {
            objects.push_back(obj);
        }
        obj = objectFindNext();
    }

    std::sort(objects.begin(), objects.end(), [](Object* a, Object* b) {
        if (a->elevation != b->elevation) return a->elevation < b->elevation;
        if (a->tile != b->tile) return a->tile < b->tile;
        if (a->pid != b->pid) return a->pid < b->pid;
        return a->id < b->id;
    });

    fprintf(f, "objects %zu\n", objects.size());
    for (Object* object : objects) {
        dumpObject(f, object);
    }

    // Event queue summary (v0: emptiness + next event time only).
    fprintf(f, "queue_empty %d\n", queueIsEmpty() ? 1 : 0);
    if (!queueIsEmpty()) {
        fprintf(f, "queue_next_time %u\n", queueGetNextEventTime());
    }

    fclose(f);
    return true;
}

} // namespace fallout
