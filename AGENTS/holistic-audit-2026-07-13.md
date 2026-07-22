---
name: holistic-audit-2026-07-13
description: "Banked unpinned test-coverage gaps surfaced by the Fable holistic equivalence audit — no bug, no current divergence, just no golden bounds them. Kept so they aren't re-litigated and get a fixture eventually."
metadata:
  node_type: memory
  type: project
  originSessionId: 8ebb6a31-7d9f-49a6-af12-8e31012e7229
  modified: 2026-07-19T05:11:04.304Z
---

A one-shot Fable holistic equivalence audit of the whole [[f2-rewrite-project]] found NO
confirmed bug. What's worth keeping = three genuinely UNPINNED classes (no current divergence —
just no golden bounds them). Kept so the same concerns aren't re-litigated.

1. **Multi-victim knockback.** `_combat_apply_knockback` is TWO-PASS (compute all dests vs the
   ORIGINAL board, then apply); the only fixture (denbus1_knockback) has ONE survivor. If one
   explosion knocks back ≥2 critters whose dests INTERACT (A's new tile blocks B's dest), two-pass
   may differ from the animated register-then-play order. The direction is a defensible modeling
   choice; the code comment asserts "two-pass matches animated" but it's not fixture-proven. Needs
   a multi-victim explosion fixture.

2. **Approach-drop × script distance/LOS reads.** Pickup/climb/use-skill/dialog all skip approach
   locomotion + adjacency + the LOS/talk-range gate (accepted caveat class). A `use_p_proc` /
   `talk_p_proc` / `use_skill_on_p_proc` that reads `tile_distance(dude,self)` / dude tile /
   orientation would run with the dude at INTENT-ISSUE position, not adjacent → could branch
   differently with NO oracle to contradict the blessed baseline. Current golden procs show
   expected deltas + byte-identical RNG/time, so no observed divergence — but a distance/LOS-gated
   proc fixture would pin the class. Real latent risk for FUTURE script coverage.

3. **DAM_KNOCKED_DOWN standup** is shared turn-processing (`_combat_standup`/`_dude_standup` for
   the attacker; the defender clears at its own turn start), NOT welded to the attack animation →
   low risk, but not fixture-pinned (no golden has a surviving knocked-down critter take a later
   turn — same limitation as active-combat goldens).
</content>
