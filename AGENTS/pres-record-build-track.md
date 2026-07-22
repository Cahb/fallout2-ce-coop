---
name: pres-record-build-track
description: "The presentation record/replay channel — durable architecture, the load-bearing BACKEND-SPLIT + TRANSIENT-DISCRIMINATOR + record-purity gotchas, and the remaining scope (progressive cue retirement + bucket-C nested sections). The discrete-action families are migrated."
metadata: 
  node_type: memory
  type: project
  originSessionId: b84e94de-c325-471e-9e07-3e98cf0e21b3
  modified: 2026-07-19T05:12:47.925Z
---

The record/replay channel (server records reg_anim leaves → wire → viewer replays the REAL
reg_anim) is the presentation engine for every DISCRETE action. Gesture, door, hit-react,
weapon-draw, the full attack family (melee/ranged/throw), and in-combat MOVE are migrated; git +
`src/pres_record.h` hold the details. The whole-turn/combat framing lives in
[[combat-full-record-channel]]; the current active pivot is [[dialog-streaming-track]].
Spec = PRESENTATION_RECORD_REPLAY_SPEC.md; recipe = PRESENTATION_RECORD_REPLAY_COOKBOOK.md.

## THE ONE-SCREEN SCOPE MAP lives in src/pres_record.h
A deliberate COVERAGE MANIFEST (family table → composite site → ops/callbacks → the bespoke quirk
it RETIRES → status) + TARGET-ARCHITECTURE block + the PresOp enum (LIVE/SCOPE boundary at
kPresOpLastLive) + PresCallbackTag taxonomy. READ IT FIRST. Key reframe: the op VOCABULARY is
~complete (every server_anim.cc leaf wraps to a record op; the client interpreter
presPlayRecordedSeq already executes ALL of Table A). Widening a family = (W1) a few callback TAGS,
(W2) relax the serverLoopActive() GATE at the composite site to run its animate branch under a
record section, (W3) clear per-family HAZARDS.

## TARGET ARCHITECTURE (converged with owner)
Two entry gates, ONE replay engine:
- IN COMBAT = turn-serial → the barrier already paces it; locomotion folds in here too.
- OUT OF COMBAT = the approach-latch lifecycle: free-roam GLIDE (soft) → ARRIVAL → AUTHORITY SNAP
  (the glide drain, walkSnapToAuthority — not a new teleport) → push recorded SEQUENCE → GLUED
  replay → RELEASE. Self-actions = the zero-approach degenerate case.
THE ONE EXCEPTION (soft by design): out-of-combat tile-crossing walk/run GLIDE stays STATE-lane
(MOVE events + durMs, the client_present glide machinery) — real-time/concurrent/interruptible; its
walk-frame anim stays locally-derived. Everything discrete is snap+sequence.

## BACKEND SPLIT (load-bearing, cost a crash to learn)
fallout2-ce (client + the golden state-dump harness) links animation.cc (REAL reg_anim +
InstantAnimationScheduler). f2_server links server_anim.cc (the RECORDING leaves). They are
MUTUALLY EXCLUSIVE animation backends (both define reg_anim_begin). The recorder ONLY works in
f2_server. Enabling F2_SERVER_PRES_RECORD in fallout2-ce would run the REAL animate branch's
callbacks (e.g. _report_explosion frees `attack`) on top of the state fast-path (also frees it) =
DOUBLE FREE. FIX: server_anim.cc arms the recorder at static-init (presRecordSetBackendActive(true));
presRecordEnabled() = env flag AND armed → inert in fallout2-ce. So record features are OFF on the
golden path AND inert in the client binary → goldens byte-identical.

## TRANSIENT-DISCRIMINATOR RULE (airtight; was a clouds regression)
resolveRef transient = `netId==0 || (netId >= gBeatNetIdWatermark && (flags & OBJECT_NO_SAVE))`.
Watermark snapshotted at BEAT start (serverTick top, presRecordBeatBegin), NOT SectionBegin —
composites create clouds/synthetic-attacker BEFORE opening their section, so a section watermark
ships them by an unresolvable netId. BOTH signals required: netId>=wm = dead-by-pump-time; NO_SAVE =
not-replicated. Excludes gDude (NO_SAVE but old — it's vanilla, saves via a special path) AND
same-beat-spawned persistent critters (new but replicated). Full rule in
[[presenter-object-ref-model]] / the cookbook.

## RECORD-PURITY GATE (the safety net)
Record mode is NOT a pure spectator (it runs the animate branch server-side: creates transients,
draws cosmetic RNG). Purity is proven by tests/golden/run_record_purity.sh (check.sh gate 2b): run
f2_server twice per case on a DETERMINISTIC map, flag off vs on, require (1) the state dump
byte-identical AND (2) the NO_SAVE object count identical (the dump SKIPS NO_SAVE, so a leaked
cloud is invisible to (1) — the leak count covers that blind spot). DIFFERENTIAL = identity → NO
baseline, NO rebless; migrate a family = add a CASE (e.g. `cattack`, artemple aggro for throw).
Knobs: F2_SERVER_ACTIONS="tick:verb:arg,..." + F2_SERVER_DUMP=<path> + F2_SERVER_LEAKPROBE=1.

## ►► REMAINING SCOPE
- **Progressive cue retirement** (owner: "don't retire anything until trusted"): the attack
  decoder-mirror is retired (record = sole attack presenter — the grenade/multi-victim forcing
  function). EVENT_DOOR_STATE / _ACTION_ANIM and the other bespoke cues stay the DEFAULT (flag off)
  until each recorded path is trusted.
- **Bucket C** (script-driven cutscenes) = "records by running"; the remaining delta is wiring the
  script-opcode entry gate (op_reg_anim_begin/end in the interpreter) so script-initiated seqs open
  a record section. "Large" part is only the open-ended test surface (arbitrary mod choreography).
  ►► KEY HAZARD = RE-ENTRANT/NESTED record sections: a script op_reg_anim_begin can pump leaves then
  call an opcode (op_attack/op_use_obj/explosion/door) that routes into a composite which ITSELF
  opens a section. Today "nesting unsupported: 2nd Begin ignored w/ stderr note" is safe ONLY
  because composites are the sole entry + the engine never nests reg_anim. FIX = DEPTH-COUNTED
  re-entrant sections: Begin{depth++; if depth==1 snapshot RNG+clear buf}; End{depth--; if depth==0
  restore RNG+emit}. A counter, NOT a flag (the flag can't tell inner from outer end → premature
  emit). THREE consequences: (1) EMIT OWNERSHIP moves INTO End() firing only at depth==0 (no call
  site emits on its own); (2) ACTOR ATTRIBUTION at the outer boundary — presSeq carries ONE
  actorNetId, a script driving several actors is the real open question; (3) keep section-lifecycle
  separate from composite STATE-apply (runs every time regardless of depth). Cheap tripwire: assert
  depth==0 at beat boundary + before each send. NOT a blocker for the migrated families (never
  exceed depth 1).
- Callback TAGS still scope-only: PRES_CB_HIDE_PROJ (may fold into HIDE_FORCED); the DROP/EXEC/
  LOGIC/LOCAL callbacks need no tag.

## GOTCHAS
- Live-verify recipe: `F2_SERVER_PRES_RECORD=1 F2_TRACE_EVENTS=1 VIEWERS=1 PACE=100
  MAP=denbus2.map scripts/viewer_live.sh` (the flag must reach the f2_server process). Trace:
  `[presseq] SEND ops=N actor=M` (server) / `RECV` (viewer).
- The client interpreter presPlayRecordedSeq ALREADY executes every Table A op — do NOT re-add op
  handlers when migrating a family; only the SERVER record site + maybe a callback tag are new.
- No headless oracle for the VIEWER replay (only the SERVER sim is gated) → live-verify each family
  per [[visual-verification-protocol]] [[anim-decouple-verification]].
- Hazard when migrating: the attack THROW branch mutates inventory during the animate build → any
  new family whose animate branch mutates AUTHORITATIVE state inline must hoist that to the STATE
  arm before migrating (RNG snapshot/restore does NOT undo an itemRemove).
</content>
