#include "barter_intent.h"

#include <deque>

namespace fallout {

// See barter_intent.h. A plain FIFO; the server inventoryOpenTrade branch
// drains it.
static std::deque<BarterIntent> gBarterIntents;

void barterIntentPush(int kind, int pid, int quantity)
{
    gBarterIntents.push_back(BarterIntent { kind, pid, quantity });
}

bool barterIntentPeek(BarterIntent* out)
{
    if (gBarterIntents.empty()) {
        return false;
    }
    *out = gBarterIntents.front();
    return true;
}

void barterIntentPop()
{
    if (!gBarterIntents.empty()) {
        gBarterIntents.pop_front();
    }
}

bool barterIntentPending()
{
    return !gBarterIntents.empty();
}

void barterIntentClear()
{
    gBarterIntents.clear();
}

} // namespace fallout
