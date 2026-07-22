# Presentation pacing / frame-perfect replay — design proposal

Status: **PROPOSAL, revised after a Fable design pass (2026-07-22)** — now a two-heads doc (Opus
draft + Fable review, accepted corrections folded in). Owner sign-off still pending. Anchors are
`file:line` on `rewrite/phase0`. Changes the Fable pass forced are marked **[rev]**.

---

## 1. The invariant (the bar everything is measured against)
The presentation stream is a **deterministic recording; the client is a variable-speed but
NEVER-LOSSY player.** The client must reproduce the entire world — anims, object moves,
throwables, knockback, stand-up, HP — **frame-perfect and in exact order.** Latency/RTT may
**DEFER or COMPRESS** *when* a slice plays; it may **NEVER** change *what* plays: no reorder, no
drop, no approximation, no mid-frame skip.

> **Accuracy is inviolable; timing is elastic.**

## 2. The mental model (one sentence)
**Keyframes carry truth; the client animates between them, compresses to catch up, and only snaps
to recover.**
- A state delta is an **I-frame** (whole ground truth); anim sequences are the **P/B frames**.
- Steady state: animate toward each keyframe, commit its truth at the **action frame** of the
  sequence that earns it (tile commits when the slide lands; HP tweens old→new). **[rev]** commit
  point is the action frame, not the last frame — see Pillar 1.
- The elastic ladder **[rev]**: **defer → compress (play faster, never skip a frame) → SEEK/snap**.
  SEEK is recovery only (a dead/wedged client). Compression is the missing middle rung that makes
  "lossless AND bounded" actually achievable for a live client.

## 3. Current engine (why it misbehaves)
Two lanes, **no shared ordering**: STATE deltas (`EVENT_MOVE`, `EVENT_OBJECT_DELTA`) apply
**immediately on decode** (`client_net.cc:1338`, `:1580`); PRESENTATION sequences (`EVENT_PRES_SEQ`)
are **queued/paced** (`presentationPump :458`). Knockback = bare `objectSetLocation`, `durMs=0`
(`actions.cc:263`) → client SNAPS; stand-up is a *later* delta → snap→pause→standup. Knockdown/fall
is **not on the record channel** (`pres_record.h:60` `[ ]`). Durations = frozen `400/100` table
(`server_anim.cc:187`), not frame-derived, though `artGetFrameCount/FramesPerSecond` exist server-
side (`art.h:135-137`). No backpressure, no client→server feedback. Layer-1 time-skip coalescing
shipped (`presenter.h:595`).

## 4. The design

### 4.0 One ordered FIFO of interleaved entries {event(delta) | sequence(anim)}
```
[event: hp -= 12] → [seq: got-hit] → [seq: knockback slide old→new] → [event: tile = new] → [seq: stand-up]
```
Drained in order; an entry is apply-silently or apply+play. **[rev] Scope of "one queue":**
- **In combat** — a single totally-ordered queue (the turn barrier bounds lag; ordering makes
  uniform-defer self-consistent).
- **Out of combat** — **per-actor lanes**, cross-waiting only where coupled. A single global FIFO
  here head-of-line-blocks unrelated actors (A's gesture stalls B's door/pickup). This matches the
  existing per-actor `gEntries` hold model.

### Pillar 1 — deferred delta commit (fixes ordering / lane collision)
Deltas apply **when the drain reaches them**, i.e. at the **action frame** of the sequence in front
of them (`PRES_OP_CALL`/callback already marks this — vanilla lands damage / hides projectile /
vanishes the picked-up item at the action frame, *mid-anim*, not the last frame; committing at
sequence-end would be a fidelity divergence). Fixes bugs J/K/D2 and the HP-snap by construction.

**[rev] This flips a load-bearing decision, own it:** today "numeric fields are never held —
hp/ap never wait on pixels" (`client_net.cc:1578`); Pillar 1 moves *truth* into the deferred lane,
so every client-side read (DAM_DEAD/outline `:411`, target affordance, LOS, AP gating) now reads
the presentation-time snapshot. In combat this is *self-consistent* (the FIFO guarantees my
turn-start commits after the death that precedes it; I can never target a not-yet-dead corpse once
my turn shows) and the turn barrier bounds the cost to vanilla-ish per-turn latency. Riders:
- Keep the existing **shown-vs-auth display tween** for the local HUD (`_pendingDudeTick`,
  `tickCombatMoveAp` `:428`) — don't route the local player's own HUD through a long queue.
- **Replay-cap + stall backstops become TESTED invariants**, not band-aids: a wedged replay now
  freezes hp/death/turn/input, not just pixels, so the 62s-class stall must be caught by test.
- Spam-click amplification lengthens click→result, so **item U (surface rejections) is a
  prerequisite**, not a nicety.

Seam: extends the existing per-actor hold/reconcile FIFO (`client_present.cc resolveHeld :420`,
`holdFrames :186`, defer hooks `:1358-1437`), today wired for fid/flags/rot + recorded-walk
position/ap; we generalize "recorded walk" → "any coupled position/hp keyframe." `resolveHeld`'s
"any replay exit snaps to authority" stays the state-never-lost backstop.

### Pillar 2 — frame-timed, metered, per-client drain
- **Frame-timed:** sequence duration from the art's real frames. **[rev]** it is the **MAX over
  concurrent legs**, not the sum (attacker swing overlaps defender hit-react within one section);
  sum over-slows the world. `ANIMATE_FOREVER`/held anims need an explicit rule (one cycle, or
  zero-cost and let the *next* entry's release gate). Kills the 400/100 table and the 25%-fast run.
- **Metered (outbox, item AA):** sequences enter a **per-client** outbox stamped with release times
  from their durations; the wire drains on schedule. The **sim never blocks** — only emission is
  paced. Generalizes `serverAddPresentationCostMs` (`server_loop.cc:138`) to per-entry.
- **Compression rung [rev]:** a slow-but-alive client (0.9x render / hitchy) diverges linearly
  forever; defer grows and SEEK loses content, so neither bounds it. Catch-up **compression** (play
  the backlog at up to ~1.1x, never skipping a frame) is invariant-legal (timing is elastic) and is
  what makes bounded-lossless real for every *live* client.
- **SEEK is a real mode [rev]**, not a rare violation: for a dead/detached spectator, compaction
  yields nothing (everything's coupled in a fight) so SEEK-with-a-backlog-seconds threshold is the
  *routine* bound. Design it as item-AA's spectator-snap mode, engineered, not an afterthought.
- **Delta seq-stamping [rev]:** every FIFO entry — including deltas, which today carry no seq id —
  needs a total-order id. This is a small protocol change and a **prerequisite for the outbox and
  the hash-ack both**.

### Pillar 2b — divergence detection via a state-hash ack (a COMBAT oracle) **[rev]**
The ack carries "applied through seq N" + a **hash of the combatants at seq N** (committed keyframe
state, netId order). Server keeps a checkpoint of the world at N and compares. Match =
deferred-but-correct (leave it); mismatch = genuinely lost content → the **trigger for SEEK**.
Fixes forced by the review:
- **Mask the flags** to the syncable set — the server diffs the whole flags word incl. client-local
  bits (`object_delta.cc:161`) and the client deliberately strips lifetime bits
  (`objectApplyWireFlags`, `client_net.cc:1596`); hashing the raw word false-alarms on every player
  actor forever.
- **Include `obj->frame`** (or frame-in-range) — the frame-index gotcha (stale frame ≥ new art's
  count → renders NOTHING; the S5 corpse bug) is the known-worst *visual* divergence and tile/fid/
  rot/hp/flags all match while the body is invisible.
- It is a **combat** oracle, not "the runtime oracle" — non-combatant divergence (ground loot item
  W, container/door item I) is out of v1 scope.
- Requires the seq-stamping above. Hash committed keyframe state, never the interpolated glide
  pixels. Checkpoint retention (pinned to lowest acked N) is a memory cost — see §6.5.

## 5. DECIDED — record-as-you-mutate (A), not splice (B) **[rev — reverses the draft]**
The end-of-beat shadow diff (`object_delta.cc:193`) **collapses a whole turn into one final-value
delta** (hp 30→18→0 becomes one 30→0; the intermediate values are gone). No splice heuristic can
reattach that to the right sequence — it can't order what the diff already destroyed — and a
mis-spliced must-precede delta can **wedge the blocking pump** (`client_net.cc:489-519`), i.e. a
62s-class freeze now carrying truth. Splice is also literally the item-X anti-pattern (re-derive
vanilla ordering from a downstream artifact). And **(A) already half-exists**: the deferred-commit
hook (`pres_record.h:249`, `serverAnimCommitDeferredWalk` `server_anim.cc:129`) is record-as-you-
mutate for *position* — extend that pattern to hp/results at `_combat_apply_attack_results`, field
by field, at sites the record channel already brackets. **Keep the shadow diff as a backstop
oracle:** assert its residual is zero for fields now carried in-stream, so a missed emit site names
itself instead of silently double-applying.

## 6. Invariants that must not break
- **Golden determinism / record-purity.** All pacing is emission-side, inert when disabled
  (`server_anim.cc:161`); `run_record_purity.sh` stays green.
- **Headless probe has no frame clock** — applies inline, never queues (`client_net.cc:304`); all
  duration/defer/compress/ack logic no-ops when `!clientViewerActive()`.
- **Sim never blocks on a client** (per-client outbox; ack is advisory).
- **State-never-lost** — every replay exit reconciles to authority (`resolveHeld`).
- **[rev] Replay-cap + stall backstops are now tested invariants** (Pillar 1 moved truth behind
  the queue).

## 6.5 Cross-cutting scenarios the design must handle **[rev — new]**
- **Mid-fight joiner double-apply:** the join blob is sim-now, but the outbox holds paced content
  describing changes already in the blob. Stamp outbox entries with sim-seq; a joiner's stream
  starts strictly after the blob's seq.
- **Map transition mid-drain:** a lagging client holds a FIFO of old-map netIds when the rebaseline
  lands (and applyBlob teardown is the known-fragile seam). Transition = **sanctioned flush +
  rebaseline (an explicit SEEK)**; stamp entries with map generation.
- **Deferred destroy ordering:** DESTROY rides the FIFO too (good — objects outlive sequences that
  reference them); confirm gDude/host-dude free-protection (`client_net.cc:1397`) and the
  adopt-netId transient connect/disconnect survive the reorder.
- **Ack ≠ gate:** "bounds the outbox" must mean the **per-client** outbox, never a shared one (a
  shared bound gates the sim on the slowest client).

## 7. RESOLVED — the local actor **[rev]**
Uniform-defer **in combat** (the ordered FIFO + turn barrier make it self-consistent; no deadlock),
with the Pillar-1 riders. **Out of combat**, per-actor lanes — do not defer an actor behind an
unrelated actor's sequence. Local HUD keeps the shown-vs-auth tween either way.

## 8. Phased roadmap **[rev — reordered]** (each phase independently landable + golden-safe)
1. **Delta seq-stamping** — total-order id on every entry. Prereq for the outbox + hash.
2. **Record the knockdown/fall/stand-up family** on the channel (`pres_record.h:60` `[ ]`→built);
   record-as-you-mutate (A) for hp/results at `_combat_apply_attack_results`.
3. **Defer knockback position + hp** via the hold FIFO, committing at the **action frame**; add the
   shadow-diff zero-residual assertion. (in-combat)
4. **Server per-client outbox** decoupling emission from the beat quantum, **then** frame-true
   durations (MAX-over-legs). ⚠ frame-true MUST NOT precede the outbox — the stepped-walk emitter
   welds durMs to `kServerTickDelta`, so a non-100-multiple duration snaps (CAP-ERASE,
   `server_anim.cc:181-186`). This is why the draft's phase-1 ordering was wrong.
5. **Compression rung + advisory ack + state-hash** (masked flags + frame); SEEK/spectator as a
   real mode with a backlog-seconds threshold.
6. **Out-of-combat per-actor lanes**; audit the `[anim-cb]` allowlist + throwables against the
   invariant (item-X no-op gaps become hard violations).

## 8.5 Implementation log + findings (Opus, 2026-07-22 — build underway)
Branch `pacing/phase1-seq-stamp`. Owner said GO; cadence = commit-as-I-go, batch verify.
- **@aaca1f3 — HP counter roll (a slice of Pillar 1's local-HUD rider).** Viewer-only:
  `rollDudeHp()` in `presentationPump` eases `gDude->hp` (the display value
  `interfaceRenderHitPoints` reads) toward `_dudeHpAuth`; `onObjectDelta` no longer hard-writes/
  renders the local dude's hp; `tickDudeHp` neutered. Uses `interfaceRenderHitPoints(false)` (the
  `true` variant blocks its own loop — the reason it was hard-set). SMOOTHING only: the roll still
  STARTS at decode, not the action frame. Golden-inert (`clientViewerActive()`-gated).
- **@987aae1 — Phase 1 delta seq-stamping (wire v4).** Frame header +u32 `entryBase`; `entryId =
  entryBase + e`, dense over events. On the WIRE so a joiner agrees with the server (avoids a 2nd
  header bump for the hash-ack). Decoder tracks `_lastEntryId` (only stamped, nothing consumes yet).
  Touched every stream reader (encoder + client walker + replay.py + control/wire_combat/resumable
  probes; header 14→18B). netstream golden green.

### ►► LOAD-BEARING FINDING — the knockback position commit is SERVER+CLIENT, not client-only
Intended to make bug J fall out of Pillar 1 by holding the `durMs<=0` knockback MOVE on the client
(no replay entry → snap as today = bounded blast radius) and committing it at `resolveHeld` (the
slide's completion = action frame), parallel to the existing `dHasFid`/`dHasRot` held state in
`PresEntry`. `resolveHeld` runs at EVERY replay exit (completion/cap/reserve-stall/forget), so it is
the natural commit point and the state-never-lost backstop already holds.

BLOCKER: **emission order.** MOVE is emitted via the LIVE `objectMoved` hook (buffered into the frame
in call order), NOT the end-of-beat shadow scan (`object_delta.cc` shadows only fid/flags/hp/rad/
poison/ap/results/inventory — never tile). In `_combat_apply_attack_results` the call order is
`_combat_apply_knockback` (buffers MOVE) → `_apply_damage` → `attackResult()` (buffers ATTACK_RESULT,
the event that RESERVES attacker/defender/extras at decode, `client_net.cc:2294`). So within the
frame the knockback MOVE decodes BEFORE the defender is reserved → the client-side hold hook finds no
entry → no-ops. This is exactly the "delta ahead of the sequence that explains it" anti-pattern (§3).

THE FORK (owner decision — pick before building phase 2/3):
  (A) **Server reorder** — apply the knockback tile (the `objectSetLocation`) AFTER `attackResult()`
      (and after the recorded presSeq). The two-pass structure already separates knockback COMPUTE
      (dests, reads pre-damage prone state) from APPLY; only the apply moves late. Cheapest; keeps the
      two-lane model but fixes the ordering for this one delta. Risk: any beat-order assumption
      downstream of the tile move.
  (B) **Client retro-hold** — when ATTACK_RESULT reserves a participant, un-snap any same-frame MOVE
      that already landed for it and re-defer to `resolveHeld`. Keeps combat.cc untouched; adds a
      "did a MOVE for this netId land earlier this frame?" ledger. Uglier; the anti-pattern lives on.
  (C) **Record-channel coupling (the "right" end state)** — the knockback rides the recorded
      sequence for ALL attack types (migrate the attack family, `pres_record.h` FAMILY TABLE
      Attack/fire `[ ]`, the LAST/most-tuned family) so position + anim are ONE ordered thing by
      construction. Biggest; this is where §4.0's single FIFO actually lands. (A)/(B) are the
      stepping stones; (C) subsumes them.
Recommendation: **(A) now** (unblocks a live-testable knockback fix cheaply), **(C) as the north
star** (the general mechanism the owner asked for). (B) only if (A)'s reorder proves unsafe.

## 8.6 Phase 4 outbox — architecture ruling (Fable pass, 2026-07-22) + increment plan
RULING **(A′)**: a per-client **outbound queue of frame-granular, metadata-carrying, SHARED**
buffers at the socket sink (`server_net.cc`). Queue element = one `shared_ptr<const vector<u8>>`
(the encoded frame, one copy shared by N clients) + sidecar `WireFrameMeta{seq, entryBase,
eventCount, simTs, costMs, mapGeneration}`. NOT raw bytes (the ack/SEEK need frame boundaries +
entry ids back), NOT entry-level at the presenter (that IS the rejected per-client framing → breaks
joiner seed, tee gate, replay.py, shared entryIds).
- **Invariant preserved, stated precisely:** one encoder → one canonical totally-ordered byte
  sequence; every client gets a suffix-contiguous UNMODIFIED copy. Per-client scheduling changes
  *when* a client gets the identical bytes, never *what*/*order*. seq stays global+dense (joiner
  seeds from the rebaseline frame header), gap-detect still fails loud (never drop/reorder a queued
  frame). Golden-inert by construction: all behavior in `server_net.cc` (f2_server-only, socket-
  only); headless golden uses the FILE sink + applies inline, never sees a queue.
- **Release time (per-client, wall-anchored, cost-chains SUCCESSORS not self):**
  `releaseAt(f,c) = max(enqueueWallMs(f), c.lastReleaseAt + c.lastCostMs)`; then advance
  `c.lastReleaseAt/​lastCostMs`. Anchor on WALL clock at enqueue (sim already runs 1:1 real time),
  NOT a simTs→wall map — a script time-skip jumps simTs by hours and would park the queue. A frame
  releases immediately; its cost defers the NEXT frame. Zero-cost frames chain through = today's
  behavior. Lagging client: frames sit queued past releaseAt (sim never blocks, O(1) enqueue),
  drain back-to-back when the socket recovers — defer, never drop. `costMs` = **MAX over actor
  lanes** in the frame (attacker swing ∥ defender react must not sum).
- **Two welds (refines §8.4):** (a) in-combat BURST (whole AI turn drains in one beat) — the outbox
  fixes this; frame-true combat-glide durMs safe post-outbox. (b) out-of-combat STEPPED walk —
  `serverWalkBeatsPerStep = durMs/kServerTickDelta` is sim-MUTATION cadence, not emission; the
  outbox can't fix it (deferring delivery diverges the live view from the live sim linearly). Stays
  quantized (400/100) until the ~25ms finer-tick project. So frame-true durations in Phase-4's tail
  apply ONLY to combat-glide/recorded paths, NOT the stepped registry.
- **Increments (each independently landable + golden-safe):**
  1. Frame-metadata seam (core, wire-INERT): `ByteSink::writeFrame(header,payload,WireFrameMeta)`
     virtual, default body = today's two `write()`s (FileByteSink untouched); `flushFrame` calls it +
     populates meta; passive per-frame cost accumulation in NetworkPresenter (objectMoved → mover
     lane durMs; presSeq → `presenterSetNextSeqCostMs`; frame cost = max over lanes).
  2. Per-client queues + NON-BLOCKING pump, everything due immediately (no scheduling). Fixes the
     5s dead-client `writeAll` stall for free. Pump at EVERY barrier pump site (deadlock guard) +
     bounded drain before closeAll (tee-vs-socket at shutdown).
  3. Release scheduling, env-gated `F2_SERVER_OUTBOX_PACE=1` (default off = increment-2 behavior).
  4. (optional) exact turn-barrier budget: `backlogMs(session)` supersedes the global estimate at
     combat.cc:3878.
  5. Frame-true durations (in-combat/burst ONLY; verify no gate sets F2_SERVER_RESUMABLE_COMBAT).
- **Traps:** joiner queue starts at rebaseline (preamble stays OUT of the queue, written at accept);
  map-transition mid-drain = ride the queue in order (do NOT byte-flush → fatal seq gap; sanctioned
  SEEK is phase 5, `mapGeneration` banked for it); dead spectator = drop WHOLE client at a cap,
  never trim; ack (phase 5) maps entryId→frame in O(log n) via the sidecar.

## 9. Overall verdict (Fable)
Architecture is **sound** — one ordered stream, defer-not-drop, emission-side pacing — and matches
machinery already half-built. Not buildable as the *original* phasing: fork flipped to (A), phase 1
reordered behind the outbox, compression rung + seq-stamping added before phases 4/5; Pillars 2/2b
took spec fixes (max-not-sum, action-frame commit, masked-flags + frame) but no structural rework.

## 10. Related
`[[presentation-backpressure-gap]]`, item AA/J/K/S/X in `drafts/COOP_LIVEPLAY_BUGS_2026-07-21.md`,
`PRESENTATION_RECORD_REPLAY_SPEC.md`, `COMBAT_MOVE_RECORD_DESIGN.md`, `src/pres_record.h`,
`APPLYBLOB_TEARDOWN_PLAN.md` (the transition/rebaseline seam §6.5 leans on).
