---
name: anim-decouple-verification
description: How to verify animation-decouple work on the f2 rewrite — the golden gate is structurally insufficient here; adversarial review is load-bearing, plus richer dumps + invariant assertions
metadata:
  node_type: memory
  type: feedback
  originSessionId: 831258ab-1c9c-4d7a-ac01-0d4e221a4ba6
---

Verification discipline for animation-decouple work on [[f2-rewrite-project]]
(the `serverLoopActive()` fast-paths that apply an outcome directly instead of
via a reg_anim completion callback). Learned the hard way 2026-07-13: the
op_critter_damage/op_explosion decouple shipped a real divergence (killed
critter's destroy_p_proc + gDude explosion XP were dropped because the legacy
non-animated branch runs critterKill BEFORE _apply_damage; critterKill strips
the script + sets DAM_DEAD so _apply_damage guards the victim out). The golden
even PINNED the wrong value (dude XP=0) and passed. An adversarial review agent
caught it; fixed in 88f303c by reordering to _combat_apply_attack_results order.

**Why the golden gate is structurally insufficient for anim-decouple specifically:**
A golden pins DETERMINISM (drift from a blessed baseline), NOT CORRECTNESS (was
the baseline right?). We can bless a wrong value and reproduce it forever. Worse,
there is NO HEADLESS ORACLE for the correct answer: the "right" outcome lives on
the animated client path, which is exactly the path that can't run headless (the
reason we're decoupling it). So the check that would validate the bless is
unreachable by the harness that does the blessing.

**How to apply (every anim-decouple / semantic bless):**
1. ADVERSARIAL REVIEW IS MANDATORY before any bless, not optional. It is the only
   guardrail with access to the ground truth (it reads BOTH branches — animated vs
   the forced non-animated — and reasons about what the callback path would have
   done that the direct path skips). This is what caught the XP/destroy-proc bug.
   Prompt it to specifically diff "what does the animated callback apply that the
   forced branch does not (XP, script procs, ordering, knockback, item/ammo, flags)."
2. RICHER STATE DUMPS as a reviewability aid (not an auto-catch): more fields
   (inventory, skills, per-critter kill/XP, gvar/mvar deltas) make every bless diff
   self-documenting so wrongness like "kill happened but XP still 0" is visible on
   eyeball. Dump-format extensions must be additive-only per case before re-bless.
3. INVARIANT ASSERTIONS where a rule is knowable without an oracle (e.g. "dude-
   sourced kill ⇒ XP delta > 0", "DAM_DEAD critter ⇒ FLAT|NO_BLOCK") — catches a
   whole class automatically, independent of any blessed baseline.

KNOWN PATTERN TRAP: reusing a legacy `animate==false` else-branch is NOT
automatically equivalent to the animated path — vanilla's non-animated branches
can have different ordering/side-effects (they were rarely/never exercised). The
correct template is _combat_apply_attack_results: apply damage/report FIRST
(runs XP + destroy proc + sets flags), THEN critterKill for corpse-finalize only.
