---
name: model-choice-fable-vs-opus
description: Model routing for the f2 rewrite — Opus builds + adversarial Opus review for sign-off; Fable freely available for design/review passes (user on a personal Pro subscription; the old "~3% budget" note was FALSE)
metadata: 
  node_type: memory
  type: user
  originSessionId: cee21779-4a8b-4bd0-9fa5-30d88cfb1513
---

Model routing for the f2 rewrite ([[f2-rewrite-project]]). Rule: ledger/design
work on Fable; mechanical file-moves and chores on Opus (fast mode).

**Current state (2026-07-12): BATCH 7 IS COMPLETE on Opus (all 7 UI extractions
+ CMake f2_core/f2_client library split landed, goldens green at 771c84c). The
Opus assignment is DONE. The user was told to switch back to Fable for Phase 2.
If this session is still Opus and the user starts Phase 2 design (animation
de-entangle / AnimationScheduler), remind them to switch to Fable first.**

►► CORRECTION (2026-07-14): the "~3% budget left, resets Jul 15" note was FALSE (no such
Fable-specific quota). The user is on the ENTRY $20 PERSONAL PRO subscription — Fable is
available but overall usage limits are SHALLOW. So: don't ration against a phantom 3%, but DO
stay economical — reserve Fable for genuinely design-class passes and keep each brief tight
(the playbook below already does this). One high-leverage pass, not many. Watch total usage.
DEFAULT stays **Opus builds + an ADVERSARIAL OPUS REVIEW AGENT for equivalence sign-off**
(the standing per-commit gate). Add a FABLE pass when the decision is genuinely design-class /
cross-cutting / hard-to-reverse (seam/architecture strategy, protocol/wire encoding, whole-
rewrite audit) — NOT for mechanical ports or per-commit code review (Opus owns those).
PLAYBOOK THAT WORKED (2026-07-13, keep it): pre-digest a de-noised, SELF-CONTAINED brief
(context + the specific decision + candidate options + the governing constraint, NO raw golden
bytes), hand the Fable agent ONLY that brief with a hard "do not explore the repo" constraint,
ask for terse recommendation/issues-only. Division of labor: Fable = cross-cutting judgment /
gap-finding; Opus = follow-up verification of anything Fable flags "needs-verify". Invoke via
the Agent tool `model: "fable"`. Brief lives at (regenerate per session): scratchpad/*_brief.md.

►► (Older, now WRONG) UPDATE (2026-07-12): "USER CANNOT USE FABLE anymore — everything
runs on Opus." Superseded by the 2026-07-13 note above (Fable is limited, not gone).
Standing process for semantic / design-class / BLESS-sign-off work remains
**Opus builds + an ADVERSARIAL REVIEW AGENT for equivalence sign-off**.
This was VALIDATED across this whole session: all track-A decouples (doors, pickup,
use-object/ladder) were built on Opus and signed off by adversarial equivalence
review agents (verdicts EQUIVALENT / EQUIVALENT-WITH-CAVEATS) before the semantic
commit — the earlier combat build did the same. Do NOT wait for / suggest Fable.
The next session (Build 2 = op_critter_damage/explosion/anim death-finalize +
knockback, which RE-BLESSES combat goldens) uses this Opus+review-agent process.

**Older bidirectional reminder (Fable no longer available — kept for context):**
- Phase 2/semantic work is where replays stop being byte-identical (two-commit
  rule, per-diff sign-off). Formerly Fable; now Opus + review-agent per above.
- Same for: hidden-rule extractions with subtle equivalence arguments,
  semantic/BLESS decisions, H-61-class RNG work, Phase 3 intents/sessions design.
