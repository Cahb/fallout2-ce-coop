#include "steal_intent.h"

#include <deque>

namespace fallout {

// See steal_intent.h. A plain FIFO; the server steal session drains it.
static std::deque<StealIntent> gStealIntents;

void stealIntentPush(int kind, int pid, int quantity)
{
    gStealIntents.push_back(StealIntent { kind, pid, quantity });
}

bool stealIntentPeek(StealIntent* out)
{
    if (gStealIntents.empty()) {
        return false;
    }
    *out = gStealIntents.front();
    return true;
}

void stealIntentPop()
{
    if (!gStealIntents.empty()) {
        gStealIntents.pop_front();
    }
}

bool stealIntentPending()
{
    return !gStealIntents.empty();
}

void stealIntentClear()
{
    gStealIntents.clear();
}

} // namespace fallout
