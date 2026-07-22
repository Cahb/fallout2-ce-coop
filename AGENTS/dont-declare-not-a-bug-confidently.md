---
name: dont-declare-not-a-bug-confidently
description: "Stop giving confident all-clears (\"not a bug / vanilla-faithful / fixed / SEPARATE\"); hedge and verify"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: bef671fa-a252-453b-9a52-c1840c7e1851
---

The user repeatedly pushed back this session (2026-07-17) when I declared things resolved with confidence: "wouldn't be so confident" (combat-glide SEPARATE-from-equip — was wrong), and "complete bs" (#7 guard walk "vanilla-faithful" — disputed). It's a recurring pattern of mine: over-concluding a bug is benign/faithful/already-fixed.

**Why:** confident all-clears that later prove wrong erode trust and steer us down wrong paths; the user has to catch me each time, which wastes the time they're trying to save.

**How to apply:**
- Proving mechanism A is NOT the cause ≠ proving the behavior is CORRECT. Say only what's proven; name the residual unknown explicitly.
- Prefer "narrowed / still need to verify X" over "closed / not a bug / vanilla-faithful."
- Before any all-clear on behavior, verify against a real oracle — vanilla itself, a golden, or a live A/B — not just code reasoning.
- Same for "fixed": presentation work has no headless oracle, so it's not fixed until the user live-verifies.

Links [[anim-decouple-verification]] (adversarial review mandatory; goldens pin determinism not correctness).
