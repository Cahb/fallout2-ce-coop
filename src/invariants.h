#ifndef FALLOUT_INVARIANTS_H_
#define FALLOUT_INVARIANTS_H_

namespace fallout {

// Sim self-check for the headless server/probe. The golden suites pin
// DETERMINISM (byte-identical replays) but not CORRECTNESS — a change can be
// perfectly reproducible and still wrong. invariantsCheck asserts properties
// that must hold REGARDLESS of value (AP never negative, hp <= max hp, a killed
// critter is a non-blocking corpse, inventory entries are well-formed). Run at
// the end of every serverTick; it is a pure read on the success path (no RNG,
// no state mutation, no output) so goldens stay byte-identical, and on a
// violation it prints a diagnostic to stderr and aborts the process non-zero so
// the golden runner reports the case as FAIL with the offending beat.
//
// `beat` is the serverTick index, printed in the diagnostic.
void invariantsCheck(int beat);

} // namespace fallout

#endif /* FALLOUT_INVARIANTS_H_ */
