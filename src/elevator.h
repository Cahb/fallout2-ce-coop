#ifndef ELEVATOR_H
#define ELEVATOR_H

#include "obj_types.h"

namespace fallout {

typedef enum Elevator {
    ELEVATOR_BROTHERHOOD_OF_STEEL_MAIN,
    ELEVATOR_BROTHERHOOD_OF_STEEL_SURFACE,
    ELEVATOR_MASTER_UPPER,
    ELEVATOR_MASTER_LOWER,
    ELEVATOR_MILITARY_BASE_UPPER,
    ELEVATOR_MILITARY_BASE_LOWER,
    ELEVATOR_GLOW_UPPER,
    ELEVATOR_GLOW_LOWER,
    ELEVATOR_VAULT_13,
    ELEVATOR_NECROPOLIS,
    ELEVATOR_SIERRA_1,
    ELEVATOR_SIERRA_2,
    ELEVATOR_SIERRA_SERVICE,
    ELEVATOR_KLAMATH_TOXIC_CAVES,
    ELEVATOR_14,
    ELEVATOR_VAULT_CITY,
    ELEVATOR_VAULT_15_MAIN,
    ELEVATOR_VAULT_15_SURFACE,
    ELEVATOR_NAVARRO_NORTHERN,
    ELEVATOR_NAVARRO_CENTER,
    ELEVATOR_NAVARRO_LAB,
    ELEVATOR_NAVARRO_CANTEEN,
    ELEVATOR_SAN_FRANCISCO_SHI_TEMPLE,
    ELEVATOR_REDDING_WANAMINGO_MINE,
    ELEVATOR_COUNT,
} Elevator;

// ── DATA (elevator_data.cc, f2_core) ─────────────────────────────────────────
// ►► The server resolves a ride from these tables and NEVER from the wire: the
// client says which BUTTON was pressed, the server says what that button means.
// A destination that arrived from a client would be a teleport-anywhere primitive.
int elevatorResolveStartLevel(int elevator, int map, int elevation);
void elevatorResolveDestination(int elevator, int level, int* mapPtr, int* elevationPtr, int* tilePtr);
// Button count — the range a level index from a client must fall inside.
int elevatorLevelCount(int elevator);
void elevatorsDataInit();

// THE RIDE, shared by single-player's script-request drain and the dedicated
// server's wire verb. Resolve the destination first (elevatorResolveDestination),
// then call this. Instant — the animated gauge is the picker screen's business and
// is deliberately not reproduced.
//
// ►► THE WHOLE PARTY RIDES. Cross-map already moved everyone (a transition moves the
// world); this makes the same-map case agree, which also keeps the server out of the
// split-elevation state that one gMap / one camera elevation cannot represent.
void elevatorRideApply(Object* rider, int map, int elevation, int tile);

// ── SCREEN (elevator.cc, f2_client) ──────────────────────────────────────────
int elevatorSelectLevel(int elevator, int* mapPtr, int* elevationPtr, int* tilePtr);
// Which BUTTON the player pressed, and nothing more — the co-op path resolves the
// destination on the SERVER from the level index this returns. -1 = escaped,
// -2 = the screen could not open. `startLevel` is where the gauge starts
// (elevatorResolveStartLevel).
int elevatorPickLevel(int elevator, int startLevel);
// Calls elevatorsDataInit() too, so the one existing call site initialises both.
void elevatorsInit();

} // namespace fallout

#endif /* ELEVATOR_H */
