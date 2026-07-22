#include "animation_scheduler.h"

namespace fallout {

// The base class IS the real-time client scheduler (NOT a null/instant default):
// with nothing installed the engine keeps its original one-frame-per-pump
// behavior, so introducing the seam is replay-identical.
static AnimationScheduler gRealtimeScheduler;
static AnimationScheduler* gScheduler = &gRealtimeScheduler;

AnimationScheduler* animationScheduler()
{
    return gScheduler;
}

void animationSchedulerSet(AnimationScheduler* newScheduler)
{
    gScheduler = newScheduler != nullptr ? newScheduler : &gRealtimeScheduler;
}

} // namespace fallout
