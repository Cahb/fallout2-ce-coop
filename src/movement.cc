#include "movement.h"

#include "combat.h"
#include "critter.h"

namespace fallout {

// Extracted verbatim from the per-tile move step in animation.cc's _object_move
// callback (REWRITE_PLAN 2.2). Behaviour is unchanged: the caller still guards
// with isInCombat()/OBJ_TYPE_CRITTER and renders the AP bar afterwards.
bool movementChargeApForStep(Object* critter)
{
    int actionPointsRequired = critterGetMovementPointCostAdjustedForCrippledLegs(critter, 1);
    if (actionPointsRequired > _combat_free_move) {
        actionPointsRequired -= _combat_free_move;
        _combat_free_move = 0;
        if (actionPointsRequired > critter->data.critter.combat.ap) {
            critter->data.critter.combat.ap = 0;
        } else {
            critter->data.critter.combat.ap -= actionPointsRequired;
        }
    } else {
        _combat_free_move -= actionPointsRequired;
    }

    return (critter->data.critter.combat.ap + _combat_free_move) <= 0;
}

} // namespace fallout
