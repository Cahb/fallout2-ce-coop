---
name: de-stub-cadence-rule
description: "STANDING cadence for ALL P5 work (not just de-stub) — batch hard, NO per-commit adversarial review of small diffs; defer review to END-OF-SLICE / one massive pass"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 06ce1636-a84f-4482-a3d1-0a45d8ad7ce5
---

User steer (2026-07-14, during P5 H6): STOP spending 100-200k tokens ceremony-reviewing
SHIT changes (relocate a few fns, benign-stub a few) with a full gate + adversarial review +
milestone writeup EACH. It's not about wall-clock — it's the token cost of flagging a
"milestone!" for +15/-15 of trivial work.

**Why:** the P5 de-stub work is mostly homogeneous, low-risk, mechanical. Per-change ritual
burns budget for near-zero information. The valuable output is the accumulating MAP of
"don't-care (benign stub) vs must-reimpl-properly (behind a seam)", not a review of each step.

**How to apply:**
- BATCH aggressively — edit across many functions/heads before running anything. Aim for a
  big lump (~dozens of syms) per gate, not one function per gate.
- TRIVIAL/BENIGN changes need NO individual validation: a stub flip abort→constant
  (`return false/0/-1;`, "yeah this is stubbed") is INVISIBLE to goldens anyway (goldens run the
  CLIENT probe binary with the real fns; server_stubs.cc bodies never execute in them). Just do them.
- Pure RELOCATION of already-reviewed bodies (client→core/seam TU) = low risk, no per-move review.
- Reserve the FULL gate (check.sh + replay + narrate) + adversarial review for: (a) a genuine
  BEHAVIOR restructure (e.g. isoEnable/isoDisable presenter split — tickers under null presenter),
  and even then batch several before ONE gate; (b) the eventual MASSIVE milestone review of a big
  accumulated diff (+500/-500), done ONCE when "we're there", not incrementally.
- Commits are still fine/cheap (bisect hygiene) — keep messages LEAN, skip the milestone novels.
- ►► NEVER run golden harnesses concurrently (background check.sh + direct run_golden.sh race over
  shared build/output → spurious "binary exited non-zero" FAILs that look like a regression).

►► 2026-07-15 REINFORCEMENT (user, sharply): this rule is NOT de-stub-only — it governs ALL P5
work. Do NOT spawn a per-commit adversarial review agent for a small diff (a 40-line object.cc
edit + a golden re-bless burned ~50k tokens + 10min on a "no bug, ship it" — exactly the ceremony
this memory forbids). The "standing per-commit adversarial-review gate" phrasing in [[model-choice-fable-vs-opus]]
and [[p5-server-plan]] is WRONG/overweighted: review is BATCHED to the END OF A SLICE (or a real
milestone), not fired per commit. Gates (check.sh etc.) are cheap+automated → run them freely; a
SUBAGENT REVIEW is the expensive thing to defer. When unsure whether a change is "big enough" to
review now, the answer is almost always NO — park it, note it, review the accumulated slice once.

See [[p5-server-plan]], [[p5-cut-list]], [[model-choice-fable-vs-opus]] (economy).
