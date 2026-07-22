#ifndef FALLOUT_INPUT_REPLAY_H_
#define FALLOUT_INPUT_REPLAY_H_

#include "dinput.h"

namespace fallout {

// Phase 0 input trace record/replay (REWRITE_PLAN.md item 0.3).
//
// Events are keyed by "pump iteration" — the number of times the engine has
// run _GNW95_process_message(). Under the synthetic clock (F2_FAKE_CLOCK)
// the iteration count at any point of the boot/sim path is deterministic,
// so a trace replays bit-exactly.
//
// Trace file format (text, one event per line, sorted by iteration):
//   # comment
//   M <iteration> <dx> <dy> <buttonMask>   mouse: one-shot deltas, sticky
//                                          button state (bit0 = left,
//                                          bit1 = right)
//   K <iteration> <scancode> <down>        keyboard: SDL scancode, 1/0
//
// Env vars: F2_INPUT_REPLAY=<path> to replay, F2_INPUT_RECORD=<path> to
// record (mutually exclusive; replay wins).

// Called once at the top of every _GNW95_process_message: advances the
// iteration counter and injects due keyboard events.
void inputReplayPumpTick();

// Current pump iteration (for diagnostics / trace authoring).
unsigned int inputReplayGetIteration();

// Replay hook for mouseDeviceGetData. Returns true if replay is active, in
// which case *data has been filled from the trace (zero deltas and last
// sticky button state when no event is due this iteration).
bool inputReplayOverrideMouse(MouseData* data);

// Record hooks. No-ops unless recording.
void inputReplayRecordMouse(const MouseData* data);
void inputReplayRecordKey(int scancode, bool down);

} // namespace fallout

#endif /* FALLOUT_INPUT_REPLAY_H_ */
