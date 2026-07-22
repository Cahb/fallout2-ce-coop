#include "fps_limiter.h"

#include <SDL.h>

#include "input.h"

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _ticks(0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
}

void FpsLimiter::throttle() const
{
    // Synthetic clock (headless replay): real-time frame pacing is
    // meaningless and only burns wall time (cf. paletteInit).
    if (getTicksIsSynthetic()) {
        return;
    }

    if (1000 / _fps > SDL_GetTicks() - _ticks) {
        SDL_Delay(1000 / _fps - (SDL_GetTicks() - _ticks));
    }
}

} // namespace fallout
