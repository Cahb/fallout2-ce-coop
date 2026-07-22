#include "sim_clock.h"

namespace fallout {

// The current sim time. Starts at 0; advanced by simClockAdvance in fixed
// kServerTickDelta increments per server tick.
static unsigned int gSimNow = 0;

unsigned int simClockNow()
{
    return gSimNow;
}

void simClockAdvance(unsigned int delta)
{
    gSimNow += delta;
}

void simClockReset()
{
    gSimNow = 0;
}

} // namespace fallout
