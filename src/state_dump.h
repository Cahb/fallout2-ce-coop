#ifndef FALLOUT_STATE_DUMP_H_
#define FALLOUT_STATE_DUMP_H_

namespace fallout {

// Phase 0 oracle (see REWRITE_PLAN.md item 0.1): writes a canonical,
// deterministic, diffable text snapshot of authoritative world state.
// Render-only fields (sx/sy/frame/outline) are intentionally excluded.
bool stateDumpWrite(const char* path);

// Mid-run tripwire (probe `mark:0` action): records {probe tick, game time,
// RNG fingerprint} at the moment of the call; stateDumpWrite emits the
// collected checkpoints so a failing golden localizes WHERE in the action
// timeline the divergence started, not just that the end state differs.
void stateDumpRecordCheckpoint(int tick);

} // namespace fallout

#endif /* FALLOUT_STATE_DUMP_H_ */
