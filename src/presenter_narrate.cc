#include "presenter_narrate.h"

#include <cstdio>

#include "combat_defs.h"
#include "object.h"
#include "presenter.h"
#include "scripts.h"
#include "sim_clock.h"

namespace fallout {

// A watcher for the headless server. Where the null presenter drops every
// presentation call, this one prints the human-meaningful ones to stdout, so a
// server run produces a live, readable play-by-play instead of running silent
// until the final state dump.
//
// SDL-free by construction (it belongs to f2_core): it only formats the seam's
// arguments as text. It is one-way like every presenter — it never returns
// state to the sim. Enabled with F2_NARRATE=1; off by default, so the golden
// gate (which installs the null presenter) is unaffected.
class NarratePresenter : public Presenter {
public:
    // The combat/skill/script log lines: "X was hit for N and lost M hit
    // points.", "X was killed.", etc. This is the richest narration source.
    void consoleMessage(const char* text) override
    {
        emit("log", nullptr, text);
    }

    // Floating text above an object (damage numbers, "Dodge!", reactions).
    void floatText(Object* owner, const char* text, int font, int color, int outlineColor) override
    {
        emit("float", owner, text);
    }

    // A modal error would block a real client; on the server surface it as text.
    void errorBox(const char* text) override
    {
        emit("error", nullptr, text);
    }

    // World-state lifecycle (MP_PROTOCOL.md §2 STATE primitives). Printing these
    // is the observable proof the widened seam fires at the right sim mutation
    // sites, and the seed of the server's telemetry / event stream.
    void objectCreated(Object* obj) override
    {
        if (presenterEmissionsSuppressed()) return;
        const char* name = obj != nullptr ? objectGetName(obj) : nullptr;
        printf("[t=%u] spawn id=%d pid=0x%08X %s tile=%d netid=%d\n", simClockNow(),
            obj != nullptr ? obj->id : -1, obj != nullptr ? obj->pid : -1,
            name != nullptr ? name : "?", obj != nullptr ? obj->tile : -1,
            obj != nullptr ? obj->netId : 0);
        fflush(stdout);
    }

    void objectMoved(Object* obj, int fromTile, int fromElevation, int toTile, int toElevation) override
    {
        if (presenterEmissionsSuppressed()) return;
        const char* name = obj != nullptr ? objectGetName(obj) : nullptr;
        printf("[t=%u] move  id=%d %s %d->%d (elev %d->%d) netid=%d\n", simClockNow(),
            obj != nullptr ? obj->id : -1, name != nullptr ? name : "?",
            fromTile, toTile, fromElevation, toElevation,
            obj != nullptr ? obj->netId : 0);
        fflush(stdout);
    }

    void objectDestroyed(Object* obj) override
    {
        if (presenterEmissionsSuppressed()) return;
        const char* name = obj != nullptr ? objectGetName(obj) : nullptr;
        printf("[t=%u] destroy id=%d pid=0x%08X %s tile=%d netid=%d\n", simClockNow(),
            obj != nullptr ? obj->id : -1, obj != nullptr ? obj->pid : -1,
            name != nullptr ? name : "?", obj != nullptr ? obj->tile : -1,
            obj != nullptr ? obj->netId : 0);
        fflush(stdout);
    }

    // Item<->world lifecycle (object.cc _obj_connect/_obj_disconnect): a persisting
    // object re-parented across the inventory<->world-tile boundary (drop/pickup/
    // unload/script), distinct from create/destroy.
    void objectConnected(Object* obj, int tile, int elevation) override
    {
        if (presenterEmissionsSuppressed()) return;
        const char* name = obj != nullptr ? objectGetName(obj) : nullptr;
        printf("[t=%u] connect id=%d pid=0x%08X %s tile=%d elev=%d netid=%d\n", simClockNow(),
            obj != nullptr ? obj->id : -1, obj != nullptr ? obj->pid : -1,
            name != nullptr ? name : "?", tile, elevation,
            obj != nullptr ? obj->netId : 0);
        fflush(stdout);
    }

    void objectDisconnected(Object* obj) override
    {
        if (presenterEmissionsSuppressed()) return;
        const char* name = obj != nullptr ? objectGetName(obj) : nullptr;
        printf("[t=%u] disconn id=%d pid=0x%08X %s netid=%d\n", simClockNow(),
            obj != nullptr ? obj->id : -1, obj != nullptr ? obj->pid : -1,
            name != nullptr ? name : "?",
            obj != nullptr ? obj->netId : 0);
        fflush(stdout);
    }

    // Global sim-state delta (object_delta.cc). v1 carries the in-game clock.
    void worldDelta(unsigned int changed) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (changed & WORLD_DELTA_GAMETIME) {
            printf("[t=%u] world gametime=%u\n", simClockNow(), gameTimeGetTime());
            fflush(stdout);
        }
    }

    // Per-beat batched field delta (object_delta.cc shadow-diff). Prints only
    // the changed fields' current values.
    void objectDelta(Object* obj, unsigned int changed) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (obj == nullptr) {
            return;
        }
        const char* name = objectGetName(obj);
        printf("[t=%u] delta id=%d %s:", simClockNow(), obj->id, name != nullptr ? name : "?");
        if (changed & OBJECT_DELTA_FID) printf(" fid=0x%08X", obj->fid);
        if (changed & OBJECT_DELTA_ROTATION) printf(" rot=%d", obj->rotation);
        if (changed & OBJECT_DELTA_FLAGS) printf(" flags=0x%08X", obj->flags);
        if (changed & OBJECT_DELTA_HP) printf(" hp=%d", obj->data.critter.hp);
        if (changed & OBJECT_DELTA_RADIATION) printf(" rad=%d", obj->data.critter.radiation);
        if (changed & OBJECT_DELTA_POISON) printf(" poison=%d", obj->data.critter.poison);
        if (changed & OBJECT_DELTA_AP) printf(" ap=%d", obj->data.critter.combat.ap);
        if (changed & OBJECT_DELTA_COMBAT_RESULTS) printf(" results=0x%X", obj->data.critter.combat.results);
        if (changed & OBJECT_DELTA_INVENTORY) printf(" inv=%d", obj->data.inventory.length);
        printf("\n");
        fflush(stdout);
    }

    // Tactical map fully loaded (map.cc mapLoad).
    void mapTransition(int mapIndex, int elevation) override
    {
        printf("[t=%u] maptrans map=%d elev=%d\n", simClockNow(),
            mapIndex, elevation);
        fflush(stdout);
    }

    // Join baseline snapshot (server_loop.cc serverInstall). One line per object
    // already present at t=0, carrying pid/tile/elev/netid — the ground truth the
    // file replayer (tools/replay.py) seeds its world from before applying events.
    // Emitted only into the FULL replay capture (run_golden_replay.sh keeps it via
    // the `snapshot` channel); deliberately absent from the compact narrate
    // regression goldens' STRUCT filter, so a ~1800-object map does not bloat them.
    void snapshotObject(Object* obj) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (obj == nullptr) return;
        const char* name = objectGetName(obj);
        printf("[t=%u] snapshot id=%d pid=0x%08X %s tile=%d elev=%d netid=%d\n", simClockNow(),
            obj->id, obj->pid, name != nullptr ? name : "?",
            obj->tile, obj->elevation, obj->netId);
        fflush(stdout);
    }

    // Combat control (combat.cc).
    void combatEnter(Object* initiator) override
    {
        printf("[t=%u] combatEnter initiator=%d\n", simClockNow(),
            initiator != nullptr ? initiator->id : -1);
        fflush(stdout);
    }

    void combatExit() override
    {
        printf("[t=%u] combatExit\n", simClockNow());
        fflush(stdout);
    }

    void turnStart(Object* critter, bool isPlayer, int apAvailable, int deadlineMs) override
    {
        const char* name = critter != nullptr ? objectGetName(critter) : nullptr;
        printf("[t=%u] turnStart id=%d %s%s ap=%d\n", simClockNow(),
            critter != nullptr ? critter->id : -1, name != nullptr ? name : "?",
            isPlayer ? " (player)" : "", apAvailable);
        fflush(stdout);
    }

    void attackResult(const Attack* attack) override
    {
        if (attack == nullptr) {
            return;
        }
        printf("[t=%u] attackResult attacker=%d defender=%d hitMode=%d loc=%d dmg=%d flags=0x%X extras=%d\n",
            simClockNow(),
            attack->attacker != nullptr ? attack->attacker->id : -1,
            attack->defender != nullptr ? attack->defender->id : -1,
            attack->hitMode, attack->defenderHitLocation,
            attack->defenderDamage, attack->defenderFlags, attack->extrasLength);
        fflush(stdout);
    }

private:
    static void emit(const char* channel, Object* owner, const char* text)
    {
        if (owner != nullptr) {
            const char* name = objectGetName(owner);
            printf("[t=%u] %-5s %s: %s\n", simClockNow(), channel,
                name != nullptr ? name : "?", text != nullptr ? text : "");
        } else {
            printf("[t=%u] %-5s %s\n", simClockNow(), channel,
                text != nullptr ? text : "");
        }
        fflush(stdout);
    }
};

static NarratePresenter gNarratePresenter;

void presenterInstallNarrate()
{
    presenterSet(&gNarratePresenter);
}

} // namespace fallout
