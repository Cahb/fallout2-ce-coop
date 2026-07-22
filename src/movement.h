#ifndef FALLOUT_MOVEMENT_H_
#define FALLOUT_MOVEMENT_H_

#include "obj_types.h"

namespace fallout {

// Movement rules extracted from animation.cc's move callbacks
// (REWRITE_PLAN 2.2). Pure sim: charges a critter's per-tile movement against
// its combat AP and the free-move allowance. SDL-free, lives in f2_core. The
// HUD update (interfaceRenderActionPoints) stays client-side at the call site.

// Charge one tile-step of movement to [critter] during combat: consume the
// free-move allowance first, then combat AP. Returns true if the critter is
// now out of movement (AP + free-move <= 0) and cannot take another step.
bool movementChargeApForStep(Object* critter);

} // namespace fallout

#endif /* FALLOUT_MOVEMENT_H_ */
