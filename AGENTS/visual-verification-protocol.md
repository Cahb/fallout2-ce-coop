---
name: visual-verification-protocol
description: Do NOT automate visual verification via screenshot floods — user verifies visuals themselves; keep artifacts out of FO2/; kill real binaries not subshell PIDs
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 98f701d7-5048-4cad-b61d-c9e42b85c08a
---

For VISUAL/presentation changes (viewer animation, rendering), do not build
screenshot-dump-and-parse harnesses. User (2026-07-15): "spawning hundreds of
screenshots on my nvme drive folder root is not ok... I can visually pinpoint
something; it gives me no benefit you be trying to automate this and burn tokens."

**Why:** F2_VIEWER_SHOT_EVERY floods FO2/ with scrNNNNN.bmp; parsing them costs
tokens and proved inconclusive anyway (SDL-dummy screenshots showed a static
scene). The user is happy to run scripts/viewer_live.sh and eyeball it — that IS
the oracle for presentation work ([[anim-decouple-verification]] applies to SIM
correctness, not pixels).

**How to apply:**
- Presentation slice → verify logic via gates + code review, then HAND THE USER
  a one-liner live-demo recipe (viewer_live.sh + nc inject verbs) and ask what
  they see. Ask targeted questions ("does the dude glide or teleport?").
- If a screenshot is genuinely needed, take ONE or TWO, into the scratchpad, and
  clean up after.
- Background-process gotcha: `( cd ... && env ... binary ) & ; kill $!` kills the
  SUBSHELL, orphaning the binary. Use `pkill -9 -x f2_server`/`-x fallout2-ce`
  or exec the binary so $! is the real PID.
- REVIEW AGENTS: user killed an adversarial review agent spawned for a modest
  self-contained presentation slice (2026-07-15, "i wouldn't allow a review agent
  for 15 lines of code change"). The "adversarial review mandatory" rule
  ([[anim-decouple-verification]], [[model-choice-fable-vs-opus]]) is for SIM
  equivalence with no oracle / design-class re-blesses — NOT a blanket per-commit
  gate. Default: self-review the diff, gates green, ship; spawn a reviewer only
  for big design-class changes or when the user asks.
