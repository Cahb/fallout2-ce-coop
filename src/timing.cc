// Timing + ticker infrastructure (P5-C Step 1, [[p5-server-plan]]).
//
// Moved verbatim out of the SDL-coupled f2_client input.cc into f2_core so the
// dedicated server (f2_server, which links f2_core WITHOUT f2_client) has a real
// clock and background-ticker list without pulling SDL. This is the SEAM
// STRATEGY for de-SDL: pure infrastructure MOVES to the durable core; the one
// genuinely platform-specific bit (the real wall-clock source) is inverted into
// a REGISTERED BACKEND (clockProviderSet) that the client binds to SDL_GetTicks
// at init. Core never names SDL; the only dependency direction is client -> core.
//
// Behaviour is byte-identical to the previous input.cc definitions: under
// F2_FAKE_CLOCK (which every golden sets) getTicks() takes the synthetic-counter
// branch and never consults the provider, so relocating the SDL tail cannot
// perturb any golden regardless of when the client registers its provider.

#include "input.h"

#include <stdlib.h>

#include <chrono>
#include <climits>

#include "memory.h"
#include "server_loop.h"
#include "sim_clock.h"

namespace fallout {

typedef struct TickerListNode {
    int flags;
    TickerProc* proc;
    struct TickerListNode* next;
} TickerListNode;

// 0x6AC780
static bool gRunLoopDisabled;

// 0x6AC784
static TickerListNode* gTickerListHead;

// 0x6AC788
static unsigned int gTickerLastTimestamp;

// The real wall-clock source, bound by the client (clockProviderSet) to a
// SDL_GetTicks wrapper. Null on the server (and before client registration),
// in which case getTicks() falls back to a core-only monotonic ms counter.
static unsigned int (*gClockProvider)() = nullptr;

// Reset the ticker list to its initial (empty, enabled) state. Called by the
// client's inputInit(); the server relies on static zero-init instead.
void tickersReset()
{
    gRunLoopDisabled = 0;
    gTickerListHead = nullptr;
}

// Free the whole ticker list. Called by the client's inputExit().
void tickersFree()
{
    TickerListNode* curr = gTickerListHead;
    while (curr != nullptr) {
        TickerListNode* next = curr->next;
        internal_free(curr);
        curr = next;
    }
    gTickerListHead = nullptr;
}

void clockProviderSet(unsigned int (*provider)())
{
    gClockProvider = provider;
}

// 0x4C8D1C
void tickersExecute()
{
    if (gRunLoopDisabled) {
        return;
    }

    // The one bridge that feeds the sim time base. Under the server loop the
    // sim clock (fixed kServerTickDelta per tick) replaces the getTicks()
    // call-counter, so animation/render/frame work no longer perturbs sim time
    // (SERVER_LOOP_DESIGN.md §1). Every downstream _get_bk_time() consumer
    // rides along unchanged.
    gTickerLastTimestamp = serverLoopActive() ? simClockNow() : getTicks();

    TickerListNode* curr = gTickerListHead;
    TickerListNode** currPtr = &(gTickerListHead);

    while (curr != nullptr) {
        TickerListNode* next = curr->next;
        if (curr->flags & 1) {
            *currPtr = next;

            internal_free(curr);
        } else {
            curr->proc();
            currPtr = &(curr->next);
        }
        curr = next;
    }
}

// 0x4C8D74
void tickersAdd(TickerProc* proc)
{
    TickerListNode* curr = gTickerListHead;
    while (curr != nullptr) {
        if (curr->proc == proc) {
            if ((curr->flags & 0x01) != 0) {
                curr->flags &= ~0x01;
                return;
            }
        }
        curr = curr->next;
    }

    curr = (TickerListNode*)internal_malloc(sizeof(*curr));
    curr->flags = 0;
    curr->proc = proc;
    curr->next = gTickerListHead;
    gTickerListHead = curr;
}

// 0x4C8DC4
void tickersRemove(TickerProc* proc)
{
    TickerListNode* curr = gTickerListHead;
    while (curr != nullptr) {
        if (curr->proc == proc) {
            curr->flags |= 0x01;
            return;
        }
        curr = curr->next;
    }
}

// 0x4C8DE4
void tickersEnable()
{
    gRunLoopDisabled = false;
}

// 0x4C8DF0
void tickersDisable()
{
    gRunLoopDisabled = true;
}

// Phase 0 (REWRITE_PLAN.md item 0.2): synthetic deterministic clock.
// When F2_FAKE_CLOCK is set, "time" advances 1ms per query, making all
// elapsed-time logic (game clock, timed scripts, animation pacing, fades)
// a pure function of the execution path instead of the wall clock —
// deterministic across runs and much faster than real time.
bool getTicksIsSynthetic()
{
    static bool useFakeClock = getenv("F2_FAKE_CLOCK") != nullptr;
    return useFakeClock;
}

// 0x4C9370
unsigned int getTicks()
{
    if (getTicksIsSynthetic()) {
        static unsigned int fakeTicks = 0;
        return ++fakeTicks;
    }

    if (gClockProvider != nullptr) {
        return gClockProvider();
    }

    // Core fallback (no SDL): monotonic milliseconds since first call. Used by
    // the dedicated server (registers no provider) and by any client call that
    // precedes inputInit's provider registration.
    static auto origin = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return (unsigned int)std::chrono::duration_cast<std::chrono::milliseconds>(now - origin).count();
}

// 0x4C93E0
unsigned int getTicksSince(unsigned int start)
{
    unsigned int end = getTicks();

    // NOTE: Uninline.
    return getTicksBetween(end, start);
}

// 0x4C9400
unsigned int getTicksBetween(unsigned int end, unsigned int start)
{
    if (start > end) {
        return INT_MAX;
    } else {
        return end - start;
    }
}

// 0x4C9410
unsigned int _get_bk_time()
{
    return gTickerLastTimestamp;
}

} // namespace fallout
