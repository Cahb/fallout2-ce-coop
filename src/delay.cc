#include "delay.h"

#include <SDL.h>

#include "input.h"

void delay_ms(int ms)
{
    if (ms <= 0) {
        return;
    }

    // Synthetic clock (headless replay): pacing sleeps only burn wall time.
    // NOTE: movie_lib.cc uses delay_ms for A/V sync against the real clock —
    // under the synthetic clock those loops busy-spin but still terminate;
    // movies are never reached in headless probes.
    if (fallout::getTicksIsSynthetic()) {
        return;
    }

    SDL_Delay(ms);
}
