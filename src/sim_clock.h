#ifndef FALLOUT_SIM_CLOCK_H_
#define FALLOUT_SIM_CLOCK_H_

namespace fallout {

// Fixed-step simulation clock for the future headless server loop
// (SERVER_LOOP_DESIGN.md §1). This is the eventual replacement for the
// getTicks-derived sim time: instead of sim time tracking wall-clock via
// getTicks() call-count, it advances by a fixed delta per server tick, so the
// simulation is fully decoupled from animation/render/frame timing.
//
// NOT yet wired into tickersExecute (input.cc) — the one-line feed change that
// makes tickersExecute stamp from gSimNow instead of getTicks() is part of the
// interlocked server-loop activation, not this scaffold. This file just
// provides the clock the activation will read.

// bk-ms advanced per server tick (SERVER_LOOP_DESIGN.md §1). 100ms/tick makes
// the >=100 game-clock gate fire every tick (gGameTime +1/tick out of combat).
constexpr unsigned int kServerTickDelta = 100;

// Current sim time (a file-static gSimNow, starts at 0).
unsigned int simClockNow();

// Advance the sim clock: gSimNow += delta.
void simClockAdvance(unsigned int delta);

// Reset the sim clock: gSimNow = 0.
void simClockReset();

} // namespace fallout

#endif /* FALLOUT_SIM_CLOCK_H_ */
