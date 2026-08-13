#include "elevator.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "animation.h" // reg_anim_clear — an elevator ride cancels a walk in progress
#include "config.h"
#include "debug.h"
#include "game.h" // gDude
#include "light.h" // _obj_rebuild_all_light — the closing doors change the lighting
#include "map.h" // gMapHeader / mapSetTransition
#include "object.h"
#include "proto_types.h" // PROTO_ID_* — the three elevator-door scenery pids
#include "proto_instance.h" // _obj_attempt_placement
#include "server_players.h" // playerActorAt/Count/Online — the party rides together
#include "sfall_config.h"
#include "tile.h" // tileDistanceBetween

namespace fallout {

// ─── ELEVATOR DATA, split out of the client's elevator.cc ─────────────────────
// The DEDICATED SERVER has to resolve an elevator ride itself, so the tables and
// the two resolvers live here in f2_core while the picker SCREEN stays in
// elevator.cc (f2_client) — the character_editor_state.cc precedent: move the
// state, not the projector.
//
// ►► THIS SPLIT IS A TRUST BOUNDARY, not tidiness. The client owns the screen and
// therefore knows which BUTTON was pressed; it must never be the thing that says
// which map/elevation/tile that button means. If the destination came off the wire,
// any client could ride to any coordinate in the game. The server takes the level
// index and resolves it against ITS OWN copy of these tables.

// The maximum number of elevator levels.
#define ELEVATOR_LEVEL_MAX (4)

// Max number of elevators that can be loaded from elevators.ini. This limit is
// emposed by Sfall.
#define ELEVATORS_MAX 50

typedef struct ElevatorDescription {
    int map;
    int elevation;
    int tile;
} ElevatorDescription;

// Number of levels for eleveators.
//
// 0x43EA1C
static int gElevatorLevels[ELEVATORS_MAX] = {
    4,
    2,
    3,
    2,
    3,
    2,
    3,
    3,
    3,
    3,
    3,
    2,
    4,
    2,
    3,
    3,
    3,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
};

// 0x43EA7C
static ElevatorDescription gElevatorDescriptions[ELEVATORS_MAX][ELEVATOR_LEVEL_MAX] = {
    {
        { 14, 0, 18940 },
        { 14, 1, 18936 },
        { 15, 0, 21340 },
        { 15, 1, 21340 },
    },
    {
        { 13, 0, 20502 },
        { 14, 0, 14912 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 33, 0, 12498 },
        { 33, 1, 20094 },
        { 34, 0, 17312 },
        { 0, 0, -1 },
    },
    {
        { 34, 0, 16140 },
        { 34, 1, 16140 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 49, 0, 14920 },
        { 49, 1, 15120 },
        { 50, 0, 12944 },
        { 0, 0, -1 },
    },
    {
        { 50, 0, 24520 },
        { 50, 1, 25316 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 42, 0, 22526 },
        { 42, 1, 22526 },
        { 42, 2, 22526 },
        { 0, 0, -1 },
    },
    {
        { 42, 2, 14086 },
        { 43, 0, 14086 },
        { 43, 2, 14086 },
        { 0, 0, -1 },
    },
    {
        { 40, 0, 14104 },
        { 40, 1, 22504 },
        { 40, 2, 17312 },
        { 0, 0, -1 },
    },
    {
        { 9, 0, 13704 },
        { 9, 1, 23302 },
        { 9, 2, 17308 },
        { 0, 0, -1 },
    },
    {
        { 28, 0, 19300 },
        { 28, 1, 19300 },
        { 28, 2, 20110 },
        { 0, 0, -1 },
    },
    {
        { 28, 2, 20118 },
        { 29, 0, 21710 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 28, 0, 20122 },
        { 28, 1, 20124 },
        { 28, 2, 20940 },
        { 29, 0, 22540 },
    },
    {
        { 12, 1, 16052 },
        { 12, 2, 14480 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 6, 0, 14104 },
        { 6, 1, 22504 },
        { 6, 2, 17312 },
        { 0, 0, -1 },
    },
    {
        { 30, 0, 14104 },
        { 30, 1, 22504 },
        { 30, 2, 17312 },
        { 0, 0, -1 },
    },
    {
        { 36, 0, 13704 },
        { 36, 1, 23302 },
        { 36, 2, 17308 },
        { 0, 0, -1 },
    },
    {
        { 39, 0, 17285 },
        { 36, 0, 19472 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 109, 2, 10701 },
        { 109, 1, 10705 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 109, 2, 14697 },
        { 109, 1, 15099 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 109, 2, 23877 },
        { 109, 1, 25481 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 109, 2, 26130 },
        { 109, 1, 24721 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 137, 0, 23953 },
        { 148, 1, 16526 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
    {
        { 62, 0, 13901 },
        { 63, 1, 17923 },
        { 0, 0, -1 },
        { 0, 0, -1 },
    },
};

// The number of buttons this elevator's panel has — the range a level index from a
// client must fall inside. Reads the same (possibly sfall-overridden) table the
// resolvers use, so the check can never disagree with the resolution.
int elevatorLevelCount(int elevator)
{
    if (elevator < 0 || elevator >= ELEVATORS_MAX) {
        return 0;
    }

    return gElevatorLevels[elevator];
}

// Ledger H-57 (extracted from the elevator UI): normalize the player's
// current map/elevation into the elevator's gauge level index, including the
// Sierra Army Depot / Military Base remap math.
int elevatorResolveStartLevel(int elevator, int map, int elevation)
{
    const ElevatorDescription* elevatorDescription = gElevatorDescriptions[elevator];

    int index;
    for (index = 0; index < ELEVATOR_LEVEL_MAX; index++) {
        if (elevatorDescription[index].map == map) {
            break;
        }
    }

    if (index < ELEVATOR_LEVEL_MAX) {
        if (elevatorDescription[elevation + index].tile != -1) {
            elevation += index;
        }
    }

    if (elevator == ELEVATOR_SIERRA_2) {
        if (elevation <= 2) {
            elevation -= 2;
        } else {
            elevation -= 3;
        }
    } else if (elevator == ELEVATOR_MILITARY_BASE_LOWER) {
        if (elevation >= 2) {
            elevation -= 2;
        }
    } else if (elevator == ELEVATOR_MILITARY_BASE_UPPER && elevation == 4) {
        elevation -= 2;
    }

    if (elevation > 3) {
        elevation -= 3;
    }

    return elevation;
}

// Ledger H-57: resolve a chosen elevator level into its {map, elevation,
// tile} destination from the elevator table.
void elevatorResolveDestination(int elevator, int level, int* mapPtr, int* elevationPtr, int* tilePtr)
{
    const ElevatorDescription* description = &(gElevatorDescriptions[elevator][level]);
    *mapPtr = description->map;
    *elevationPtr = description->elevation;
    *tilePtr = description->tile;
}

// The DATA half of the sfall elevators.ini override (elevator.cc keeps the art
// half). Each side reads the file for its own tables: the alternative is one
// reader that reaches across the core/client line, which is the coupling this
// split exists to remove. Init-time, once, and the file is usually absent.
void elevatorsDataInit()
{
    char* elevatorsFileName;
    configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_ELEVATORS_FILE_KEY, &elevatorsFileName);
    if (elevatorsFileName == nullptr || *elevatorsFileName == '\0') {
        return;
    }

    Config elevatorsConfig;
    if (!configInit(&elevatorsConfig)) {
        return;
    }

    if (configRead(&elevatorsConfig, elevatorsFileName, false)) {
        char sectionKey[4];
        char key[32];
        for (int index = 0; index < ELEVATORS_MAX; index++) {
            snprintf(sectionKey, sizeof(sectionKey), "%d", index);

            if (index >= ELEVATOR_COUNT) {
                int levels = 0;
                configGetInt(&elevatorsConfig, sectionKey, "ButtonCount", &levels);
                gElevatorLevels[index] = std::clamp(levels, 2, ELEVATOR_LEVEL_MAX);
            }

            for (int level = 0; level < ELEVATOR_LEVEL_MAX; level++) {
                snprintf(key, sizeof(key), "ID%d", level + 1);
                configGetInt(&elevatorsConfig, sectionKey, key, &(gElevatorDescriptions[index][level].map));

                snprintf(key, sizeof(key), "Elevation%d", level + 1);
                configGetInt(&elevatorsConfig, sectionKey, key, &(gElevatorDescriptions[index][level].elevation));

                snprintf(key, sizeof(key), "Tile%d", level + 1);
                configGetInt(&elevatorsConfig, sectionKey, key, &(gElevatorDescriptions[index][level].tile));
            }
        }

        // The "Image" remap: an elevator can borrow another's panel, and with it
        // that panel's BUTTON COUNT. Applied here for the level count (data) and
        // again in elevator.cc for the art.
        for (int index = 0; index < ELEVATORS_MAX; index++) {
            snprintf(sectionKey, sizeof(sectionKey), "%d", index);

            int type;
            if (configGetInt(&elevatorsConfig, sectionKey, "Image", &type)) {
                type = std::clamp(type, 0, ELEVATORS_MAX - 1);
                if (index != type) {
                    gElevatorLevels[index] = gElevatorLevels[type];
                }
            }
        }
    }

    configFree(&elevatorsConfig);
}

// The elevator doors the rider just stepped through: nearest matching scenery
// within 4 tiles, on the rider's own elevation. Vanilla's search, verbatim.
static Object* elevatorFindDoors(Object* rider)
{
    Object* doors = objectFindFirstAtElevation(rider->elevation);
    while (doors != nullptr) {
        int pid = doors->pid;
        if (PID_TYPE(pid) == OBJ_TYPE_SCENERY
            && (pid == PROTO_ID_0x2000099 || pid == PROTO_ID_0x20001A5 || pid == PROTO_ID_0x20001D6)
            && tileDistanceBetween(doors->tile, rider->tile) <= 4) {
            break;
        }
        doors = objectFindNextAtElevation();
    }

    return doors;
}

static void elevatorCloseDoors(Object* doors)
{
    if (doors == nullptr) {
        debugPrint("\nWarning: Elevator: Couldn't find old elevator doors!");
        return;
    }

    objectSetFrame(doors, 0, nullptr);
    objectSetLocation(doors, doors->tile, doors->elevation, nullptr);
    doors->flags &= ~OBJECT_OPEN_DOOR;
    doors->data.scenery.door.openFlags &= ~0x01;
    _obj_rebuild_all_light();
}

// THE RIDE. Extracted from the SCRIPT_REQUEST_ELEVATOR drain (scripts.cc) so the
// single-player path and the dedicated server run the same code — the server has no
// picker screen, so it reaches this from its own wire verb instead of from the drain.
//
// ►► THE WHOLE PARTY RIDES, not just the rider. Co-op players are an SP-style party
// (the project's standing group-effects ruling), and there is a hard reason here as
// well as a design one: one gMap, one camera elevation, and every combat/AI roster
// filters on gElevation — so leaving one player on another floor puts the server in a
// state nothing else supports. Cross-map rides were already global (mapSetTransition
// moves the world); this makes the same-map case agree with them.
//
// The vanilla animated gauge is deliberately NOT reproduced: the ride is instant, on
// the owner's call ("ease on the animated stuff; just make it a map transition").
void elevatorRideApply(Object* rider, int map, int elevation, int tile)
{
    if (rider == nullptr) {
        return;
    }

    if (map != gMapHeader.index) {
        // Another map: close the doors behind, then let the ordinary transition
        // machinery carry everyone. Placement of the other players is mapLoad's
        // business (it re-plants player actors beside the host), not ours.
        elevatorCloseDoors(elevatorFindDoors(rider));

        MapTransition transition;
        memset(&transition, 0, sizeof(transition));
        transition.map = map;
        transition.elevation = elevation;
        transition.tile = tile;
        transition.rotation = ROTATION_SE;
        mapSetTransitionForActor(&transition, rider);
        return;
    }

    // Same map. Find the doors BEFORE moving anyone — the search is relative to the
    // rider's current tile and elevation, both of which the placement changes.
    Object* doors = elevation != rider->elevation ? elevatorFindDoors(rider) : nullptr;

    reg_anim_clear(rider);
    objectSetRotation(rider, ROTATION_SE, nullptr);
    _obj_attempt_placement(rider, tile, elevation, 0);

    // Everyone else who is online and not the rider, onto free tiles beside it. A
    // radius of 1 lets _obj_attempt_placement itself spread them if the free-tile
    // search comes up empty; co-locating is better than stranding a player on a
    // floor the party has left (map.cc's own rule for reattach).
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr || actor == rider || !playerActorOnline(slot)) {
            continue;
        }

        int near = playerActorFindFreeTileNear(tile, elevation);
        reg_anim_clear(actor);
        objectSetRotation(actor, ROTATION_SE, nullptr);
        _obj_attempt_placement(actor, near != -1 ? near : tile, elevation, 1);
    }

    if (doors != nullptr) {
        elevatorCloseDoors(doors);
    }
}

} // namespace fallout
