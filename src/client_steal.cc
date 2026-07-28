#include "client_steal.h"

#include "game.h"
#include "object.h"

namespace fallout {

// See client_steal.h. Two netIds and three flags — the whole viewer state of a
// steal session. Everything visible is either a real mirrored object or an
// ordinary inventory delta applied to one.
static int gStealThiefNetId = 0;
static int gStealTargetNetId = 0;
static bool gStealOpen = false;
static bool gStealEndPending = false;
static bool gStealDirty = false;
// A close has been ASKED FOR and not yet answered. ESC is a request to the server
// (the session is its, not ours), and the window stays up until EVENT_STEAL_END
// comes back — so a player who presses ESC again during that round trip used to
// fire another `sdone`, and the extras landed after the session was already gone.
static bool gStealClosePending = false;

void clientStealOnBegin(int thiefNetId, int targetNetId)
{
    gStealThiefNetId = thiefNetId;
    gStealTargetNetId = targetNetId;
    gStealOpen = true;
    gStealEndPending = false;
    gStealDirty = true;
    gStealClosePending = false;
}

void clientStealOnState()
{
    gStealDirty = true;
}

void clientStealOnEnd()
{
    // LATCH ONLY. This decodes from inside the open screen's own loop (the wire
    // is pumped there), so tearing state down here would pull the ground out from
    // under the frame that is mid-render. The loop breaks on the flag and the
    // main loop finalizes afterwards — the client_barter deferred-reconcile rule.
    gStealEndPending = true;
}

bool clientStealActive()
{
    return gStealOpen && !gStealEndPending;
}

bool clientStealEndPending()
{
    return gStealEndPending;
}

void clientStealFinalize()
{
    if (!gStealEndPending) {
        return;
    }
    gStealEndPending = false;
    gStealOpen = false;
    gStealDirty = false;
    gStealThiefNetId = 0;
    gStealTargetNetId = 0;
    gStealClosePending = false;
}

bool clientStealConsumeCloseRequest()
{
    if (gStealClosePending) {
        return false;
    }
    gStealClosePending = true;
    return true;
}

bool clientStealIsDriver()
{
    return gStealOpen && gDude != nullptr && gDude->netId == gStealThiefNetId;
}

Object* clientStealThief()
{
    return objectFindByNetId(gStealThiefNetId);
}

Object* clientStealTarget()
{
    return objectFindByNetId(gStealTargetNetId);
}

bool clientStealConsumeDirty()
{
    bool dirty = gStealDirty;
    gStealDirty = false;
    return dirty;
}

} // namespace fallout
