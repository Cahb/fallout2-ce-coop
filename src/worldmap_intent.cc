#include "worldmap_intent.h"

#include <deque>

namespace fallout {

static std::deque<WorldmapIntent> gWorldmapIntents;

void worldmapIntentPush(int kind, int x, int y)
{
    gWorldmapIntents.push_back(WorldmapIntent { kind, x, y });
}

bool worldmapIntentPeek(WorldmapIntent* out)
{
    if (gWorldmapIntents.empty()) {
        return false;
    }
    *out = gWorldmapIntents.front();
    return true;
}

void worldmapIntentPop()
{
    if (!gWorldmapIntents.empty()) {
        gWorldmapIntents.pop_front();
    }
}

bool worldmapIntentPending()
{
    return !gWorldmapIntents.empty();
}

void worldmapIntentClear()
{
    gWorldmapIntents.clear();
}

} // namespace fallout
