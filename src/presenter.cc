#include "presenter.h"

#include <cstdlib>
#include <vector>

#include "object.h"

namespace fallout {

// The base class IS the null presenter (all methods no-op).
static Presenter gNullPresenter;
static Presenter* gPresenter = &gNullPresenter;

Presenter* presenter()
{
    return gPresenter;
}

void presenterSet(Presenter* newPresenter)
{
    gPresenter = newPresenter != nullptr ? newPresenter : &gNullPresenter;
}

static bool gSuppressPresenterEmissions = false;

bool presenterEmissionsSuppressed()
{
    return gSuppressPresenterEmissions;
}

void presenterSetEmissionsSuppressed(bool suppressed)
{
    gSuppressPresenterEmissions = suppressed;
}

// -- TIME-SKIP MOVE COALESCING (see presenter.h for the why) ---------------

namespace {

struct TimeSkipOrigin {
    Object* obj;
    int tile;
    int elevation;
};

// Depth, not a bool: game_time_advance loops day-by-day and any of those days
// may re-enter script code that skips again. Only the outermost End emits.
int gTimeSkipDepth = 0;

// Linear: the set is small (movers during a skip, not objects on the map) and
// bounded by distinct critters, so the scan is cheaper than a map's overhead.
std::vector<TimeSkipOrigin> gTimeSkipOrigins;

} // namespace

void presenterTimeSkipBegin()
{
    // Kill switch for A/B-ing the coalescer live (bisection method,
    // record-purity-ap-asymmetry): F2_NO_TIMESKIP_COALESCE=1 restores the raw
    // per-tile flood. Read once — the env cannot change under a running server.
    static const bool disabled = getenv("F2_NO_TIMESKIP_COALESCE") != nullptr;
    if (disabled) {
        return;
    }

    if (gTimeSkipDepth == 0) {
        gTimeSkipOrigins.clear();
    }
    gTimeSkipDepth++;
}

bool presenterTimeSkipActive()
{
    return gTimeSkipDepth > 0;
}

void presenterTimeSkipRecordMove(Object* obj, int fromTile, int fromElevation)
{
    if (obj == nullptr) {
        return;
    }

    // FIRST move wins: the origin is where the object stood when the skip began.
    for (const TimeSkipOrigin& origin : gTimeSkipOrigins) {
        if (origin.obj == obj) {
            return;
        }
    }

    gTimeSkipOrigins.push_back({ obj, fromTile, fromElevation });
}

void presenterTimeSkipForget(Object* obj)
{
    if (obj == nullptr) {
        return;
    }

    for (size_t index = 0; index < gTimeSkipOrigins.size(); index++) {
        if (gTimeSkipOrigins[index].obj == obj) {
            gTimeSkipOrigins.erase(gTimeSkipOrigins.begin() + index);
            return;
        }
    }
}

void presenterTimeSkipForgetAll()
{
    gTimeSkipOrigins.clear();
}

void presenterTimeSkipEnd()
{
    if (gTimeSkipDepth == 0) {
        return;
    }

    gTimeSkipDepth--;
    if (gTimeSkipDepth > 0) {
        return;
    }

    // Emit AFTER leaving the window so these go out as ordinary moves. Swap the
    // vector out first: an emission that re-enters this file (it should not, but
    // the presenter is an arbitrary consumer) must not iterate a mutating vector.
    std::vector<TimeSkipOrigin> origins;
    origins.swap(gTimeSkipOrigins);

    for (const TimeSkipOrigin& origin : origins) {
        Object* obj = origin.obj;
        if (obj->tile == origin.tile && obj->elevation == origin.elevation) {
            // Wandered and came back — the net move is nothing, and emitting a
            // no-op move would make the viewer re-derive a rotation for it.
            continue;
        }
        presenter()->objectMoved(obj, origin.tile, origin.elevation, obj->tile, obj->elevation);
    }
}

} // namespace fallout
