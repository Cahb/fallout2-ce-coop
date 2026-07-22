---
name: sweep-before-recon-lesson
description: Prefer a broad EXECUTION sweep over static recon when de-stubbing — run the thing on all inputs and let the aborts name the work
metadata: 
  node_type: memory
  type: feedback
  originSessionId: e667ddd5-0a88-48f5-ac28-dc9be643ee4c
---

Established 2026-07-15 (P5 STEP 1 completion, de6b8ef). When the question is
"what does the running core still reach?", a BROAD EXECUTION SWEEP beats static
recon and beats theorizing.

`scripts/srv_sweep.sh` boots f2_server on all 155 maps in master.dat; each abort
names the next client symbol. In one pass it produced a better work list than 4
background Explore recon agents had: it found stubs recon never listed
(_gmouse_remove_item_outline, gameDialogEnable/Disable, endgameSetupDeathEnding),
corrected recon's scope (the sfx builder family is 6 fns, not the 1 listed), and
surfaced two things static reading could not have: a pre-existing engine segfault
(cowbomb.map) and a pre-existing infinite-ish combat (rnduvilg/rndholy2).

**Why:** static recon guesses at reachability and under-counts closures; the
linker and the live abort trail are ground truth. The prior session's failure
mode was the opposite — "denbus1 boot-fails, probably needs elevatorsInit or a
per-map init" was a plausible theory that was simply WRONG (it was one chrome
call from a map_enter script). Cost of the sweep: minutes.

**How to apply:**
- Broaden the input set EARLY (all maps, not the 5 golden ones). Cheap, parallel-
  free, and each failure is self-labeling.
- ►► When a target fails, first ask "does the CLIENT do this too?" — run the same
  case through the probe (scripts/server_run_case.sh). That one question
  separated 3 pre-existing engine bugs from P5 regressions in this session and is
  the single highest-value diagnostic move. See [[p5-server-plan]].
- ►► Any sweep/harness script MUST abort on build failure — otherwise it runs the
  STALE binary and reports green for code that does not compile. This bit me once.
- ►► Run the binary from the FO2/ game-data dir (cwd-relative master.dat), else
  gameDbInit fails and it looks like a boot bug.
- Keep these harnesses in scripts/ (committed), NOT session scratch — the earlier
  srv_step.sh was lost at session exit. /scratch/ is gitignored for logs only.

See [[de-stub-cadence-rule]] (batch hard, one gate at the end), [[p5-cut-list]].
