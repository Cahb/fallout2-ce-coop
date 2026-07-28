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

## 11. Per-actor action gate (feature A, SHIPPED) + pickup/outcome sync (B, designed)

### A — SHIPPED @0c2c0c8 (2026-07-24, check.sh green, LIVE-VERIFY OWED)
Reproduces vanilla's animation-blocked action pacing without ever blocking the authoritative
single-thread sim. Per-actor **wall-clock busy window** (`serverActorBusyMark/Is`,
`server_players.{h,cc}` — self-expiring, clamped [100,3000]ms, never persisted/on-Object/on-wire).
Enforced two ways from ONE state: **out of combat REJECT** at the verb choke point
(`server_control.cc`, rate-limited refusal, meta verbs bypass, `!isInCombat()` only); **in combat
DEFER** in the intent drain (`combat_drain.cc`, `END_TURN`/`END_COMBAT` bypass — no barrier
deadlock). Duration = **`presRecordCostMs()`** (`pres_record.cc` — per-owner-lane anim cycles, MAX
across lanes; also feeds the once-stubbed outbox `_frameSeqCostMs`). Pilot consumer = **`hand <0|1>`**
verb (per-actor active hand + recorded put-away/take-out; 0 AP; `serverDudeHitMode` reads active
hand, default RIGHT → byte-identical). Client 'B' input-blocks the initiating viewer only, reverts on
refusal. Golden-safe: busy marked only on `serverDedicatedActive()`. Kill switch `F2_SERVER_ACTION_GATE=0`.
►► LIVE-VERIFY: weapon-switch plays an animation; spam (door/lockpick/hand) is rate-limited; `mv`
doesn't storm refusals; a non-host advanced-unarmed move works (also validates per-actor sheet).

### B — pickup / outcome-delta sync (DESIGNED 2026-07-24, NOT built; builds on A's cost helper)
Bug: "item disappears before the pickup animation finishes." Grounded findings:
- The co-op client does **NOT predict movement** (`main.cc:1165/1619/1642` — server-authoritative
  glide owns the sprite) → **no warp/reconcile needed**; a losing peer never runs ahead.
- The cancel-push **already exists** (`interactionCannotGetThere`, `server_control.cc:358`).
- Pickup **reparents** the item (`_obj_pickup`) → the loser's `PendingInteraction` can't satisfy →
  drops → cancel-push. Server race resolution is complete + authoritative.
- The gesture **IS** streamed out of combat (`interactionEmitGesture`, `server_control.cc:597`, on
  arrival at :859) — right before `interactionFire` (:860) the same beat.
- ROOT: the gesture presSeq replays over frames but the **item-removed lifecycle delta applies
  immediately** on the viewer → item pops at gesture-START not gesture-END. Pure emission ordering,
  no race, **no lock**.
►► **OUTBOX `costMs` APPROACH IS INFEASIBLE** (build-agent verified 2026-07-24 — my earlier "release
the delta via the outbox slot" was structurally WRONG; corrected here). Four reasons: (1) the outbox
paces at FRAME granularity — `costMs` defers frame N+1, not content within a frame, and the gesture
presSeq + the item DISCONNECT are emitted the SAME beat → same frame (`server_control.cc` erase→emitGesture
→fire); (2) `costMs` is combat-only (`presenter_network.cc:1318`); (3) outbox pacing off by default
(`F2_SERVER_OUTBOX_PACE`, `server_net.cc:44`); (4) the per-client outbox is a strict FIFO — deferring one
frame delays ALL behind it (forbidden). AND the removal is `EVENT_DISCONNECT`, emitted **synchronously**
from vanilla-shared `_obj_pickup`→`_obj_disconnect` (not scan-derived), applied **immediately at client
decode** (`client_net.cc:1575`), while the gesture rides `_presQueue` drained behind the actor's
`animationIsBusy` (`client_net.cc:571`). **That decouple IS the bug.**
►► **REAL SEAM = the RECEIVE side** (per-viewer + RTT-correct by construction): route the pickup DISCONNECT
through the client's existing `_presQueue`, tagged with the gesture actor's netId, so it drains behind that
actor's gesture via the already-present `animationIsBusy` gate. Needs a wire link actor↔item. **Variant (a),
recommended:** new `EVENT_DISCONNECT_AFTER_GESTURE{itemNetId, actorNetId}`, emitted dedicated-path only —
NO `kPresStreamVersion` bump, existing DISCONNECT bytes unchanged, unknown-event skip-safe on client +
netstream decoders. Variant (b): append `afterActorNetId` to `EVENT_DISCONNECT` (touches shared bytes,
bumps version 4→5). Server: `interactionFire` stamps the gesture actor `interactionEmitGesture` just
recorded. **This is a design-class protocol addition — PENDING owner green-light before building.**
►► **SHIPPED @3a198e9: the message branch** — "Someone grabbed it." to the contested-pickup loser at the
target-gone drop (`kInteractGet` + `objectFindByNetId`→null, since the winner's `_obj_pickup` reparented
the item out of the world tile list). Was a silent drop. Golden-safe (dedicated-only, `consoleMessageFor`
no-op on null presenter); check.sh green, no gate moved.
Owner steer: co-op players are an SP-style party ([[coop-group-effects-like-party]]); this is presentation
consistency for the N parallel peers, NOT server integrity (authoritative FIFO, solved).

---

## 12. Projectiles & throwables — ONE fix for all of them (Fable pass, 2026-07-25)

**Owner ask:** an all-in-one fix covering thrown rocks/knives/spears, thrown explosives, and
launcher projectiles, for players AND NPCs. Owner's discriminator that cracked it: *player
rockets and grenades look fine; NPC spears and rocks do not.*

### 12.1 Root cause — TWO CLOCKS, not a ref-encoding bug
The wire is totally ordered (seq → CONNECT → DISCONNECT), but the client applies it on **two
independent clocks**: state events at DECODE time (`onDisconnect`, client_net.cc:1637), record
sequences at PUMP-DRAIN time (`presentationPump`). "Variable-speed but never-lossy player" must
mean variable speed of **the stream as a whole** — per-lane speed (state instant, record
deferred) **is a reorder**, which §the north-star invariant forbids.

A non-explosive throw's flight transient ADOPTS the real ground-item netId (actions.cc, search
`presRecordSetAdoptNetId`) — so it is the first object whose **lifetime** is written by BOTH
lanes, and the tear shows up as lifetime corruption: its DESTROY (the pickup DISCONNECT) is a
LATER-wire-order event applied BEFORE the EARLIER-wire-order OBJ_CREATE commits. Explosive
throws and launcher projectiles never adopt, so nothing destroys their transient early — exactly
the owner's split.

►► **THE SEESAW EXISTS ONLY BECAUSE CREATION AND DESTRUCTION SIT ON DIFFERENT CLOCKS.**
Decode-minting fixed the phantom spear by moving creation to the fast clock, which re-exposed
destruction outrunning the flight. Any fix that keeps two clocks just shifts the weight. §6.5
already names the cure ("DESTROY rides the FIFO too"); this is that bullet, built.

### 12.2 The design — deferred lifecycle entries + mint at EXECUTE
One rule: **any state-lane event addressing a presentation-entangled netId rides `_presQueue`
in wire order instead of applying at decode.** Client-only, ZERO wire change, ZERO server change.

1. **`_pendingAdopts`** (netId → count of not-yet-executed OBJ_CREATEs adopting it). The DRY pass
   **stops minting**; it only increments. The EXECUTE pass mints, registers `_net`/
   `_adoptTransients`, decrements. Every transient now mints on ONE clock — the decode→execute
   existence window ceases to exist.
2. **`PresKind::kDeferredEvent`** `{evType, payload, netId, capDeadline}` — raw event bytes
   re-parsed at drain by the same handlers.
3. **Entangled predicate** at the top of `onConnect`/`onDisconnect`/`onDestroy`/`onObjectDelta`/
   `onMove`: `_pendingAdopts.count(netId) || _adoptTransients.count(netId)`. Ordering is airtight
   by construction: once one event for netId X defers, every later event for X defers, so
   per-netId wire order is preserved end to end.
4. **Pump gate**: hold while `animationIsBusy(lookup(netId))` — the pickup DISCONNECT waits out
   the SPEAR's own flight/landing anim (not the thrower's) — plus a wall-clock cap (~4s, sibling
   of `kMoveReplayCapMs`) that FORCE-APPLIES. Failure direction stays "play/snap, never freeze",
   and never "drop".

**Seesaw resolved, not traded:** early-destroy becomes structurally impossible (the DISCONNECT
sits behind the seq in ONE FIFO; execute mints the object the DISCONNECT will find); phantom
becomes structurally impossible (deferral is never a drop — the DISCONNECT always drains,
cap-bounded). Client lifetime == wire-order lifetime, shifted uniformly by pump latency.

**Case matrix:** rock/knife/spear = the fixed path; grenade/dynamite and bullet/rocket are
unchanged but now UNIFORM (all transients mint at execute); local player / remote player / NPC
are indistinguishable because the mechanism keys on **netId entanglement, never on who acted**.
Miss, victim-dies-mid-flight, thrower-freed-before-execute and version-dropped-seq all covered
(the last degrades to exact state with no flight visual via the CONNECT item self-heal).

### 12.3 Why it is safe
Never-lossy (defers WHEN, never WHAT; the cap is a sanctioned snap, not a drop) · sim never
blocks · no new client authority (pure receive-side flow control) · no vanilla rewrite
(`_action_ranged`, reg_anim, `_obj_pickup`, `actionThrowConsumeHeadless` untouched) · NO_SAVE
/blob purity intact · **GOLDEN-INERT: `onPresSeq` returns before the dry pass when
`!clientViewerActive()`, so headless never feeds `_pendingAdopts` and every lifecycle event
applies inline exactly as today; no wire change → no gate moves, NO re-bless anywhere.**

►► It is the first concrete installment of Pillar 1's deferred-commit model, and `kDeferredEvent`
is the SAME vehicle §11 B (pickup-sync) needs — this bug feeds it from a client-derived key
(adopt), §11 B later feeds it from an owner-gated wire tag. **One mechanism, two feeders.**

### 12.4 Rejected alternatives
1. **Tombstone the adopt entry** (keep decode-minting, mark "destroy after execute") — patches only
   the decode→execute window; a DISCONNECT arriving after execute while the pump is backlogged
   still vanishes the spear, and throw→pickup→re-throw in one burst leaves the dry pass skipping
   the second mint. The seesaw with a third state bolted on.
2. **Server-side deferral / wire tag now** — §11 B's own analysis kills the outbox variant four
   ways, and the client would STILL apply at decode unless it defers, so you build the FIFO entry
   anyway and add a design-class protocol change the adopt key makes unnecessary.
3. **Promote projectiles to real replicated objects** — flight position would ride immediate
   deltas (the exact disease Pillar 1 cures), violates NO_SAVE/blob purity, burns the
   process-lifetime id budget per shot, and rewrites vanilla lifetime handling.

### 12.5 Build order (no step re-blesses a golden)
1. ✅ **BUILT** — `kDeferredEvent` plumbing, **no feeder** (enum arm, pump gate + cap, drain
   dispatch, teardown clears at applyBlob/onMapTransition, overflow exemption). Dead path → suite
   byte-identical, confirmed.
2. ✅ **BUILT + MECHANISM MEASURED; the EYEBALL arm still owed.** Execute-time minting + the
   entangled feeder. ►► **`scripts/throw_smoke.sh` reproduces the whole thing on demand** (real
   dedicated server + real dummy-video viewer, spear on artemple — the throw path CANNOT be
   exercised headlessly, see step 2's note in §12.3). Measured A/B on the same netId, same op
   counts, one commit apart:

   | throw seq (ops=15) | `[disc]` order | |
   |---|---|---|
   | `refsOk=6 refsDROPPED=5 transientsMinted=0` | **before** the seq plays | step 1 — spear never flies |
   | `refsOk=11 refsDROPPED=0 transientsMinted=1` | after the flight, `found=1` | step 2 |

   Every other `[preplay]` line was identical across the two runs (blast radius confirmed
   empirically, not just by argument). The fixture also lands the tightest possible version of the
   race: `PARK type=5 net=381 ... live=0` — the pickup DISCONNECT arrives before the transient
   even exists. **Still owed, and only the owner can call it:** does the flight LOOK right, does
   the AI re-pickup remove it as the AI arrives, are grenade + rocket unchanged.
3. Re-throw + stacked-throwable soak under induced pump backlog — extend `throw_smoke.sh`. ►► The
   `[adopt]` trace shipped with step 2 (`PARK` at the feeder, `MINT` at execute, both under
   `F2_TRACE_EVENTS`), so trap 4's netId-identity assertion is now a read, not a build: compare
   `MINT net=` against `PARK net=`.
   ►► **BANKED, PRE-EXISTING (identical in both A/B runs, so NOT from this work):** a 4-op
   sequence right after the pickup reports `refsOk=1 refsDROPPED=1` — one unresolvable ref in a
   post-pickup sequence. Under the never-lossy bar that is a real (cosmetic) tail worth chasing;
   do not attribute it to §12.
4. Backstop: force-wedge a flight anim → cap force-applies; map transition mid-flight → clean
   rebaseline (walks the applyBlob seam → run the ASAN build).
5. *(Later, owner-gated)* §11 B pickup-sync rides the same path via the wire tag.

### 12.6 Traps (Fable, codebase-specific)
1. ►►►► **`enqueue()` DROPS THE OLDEST DROPPABLE ENTRY ON BACKLOG** (client_net.cc:753-776).
   Fine for captions, **fatal for a deferred DISCONNECT — a permanent phantom, i.e. the exact bug
   this design exists to fix.** `kDeferredEvent` must be overflow-EXEMPT *and* apply-inline-now on
   overflow (merely skipping starves the cap).
2. `onBlobEnd`'s in-combat branch preserves only `kExit` — clear `_pendingAdopts` in the SAME
   block, or later events defer against a count that never decrements and force-apply 4s late
   forever.
3. Order the guards: `onDestroy`'s gDude protection and the loot-modal deferred-free branch must
   run BEFORE the entangled check.
4. Stacked throwables: `itemRemove` peels a fresh object per unit ([[drop-count-divergence]]);
   adopt captures `weapon->netId` at record time while `actionThrowConsumeHeadless` connects
   `weapon` itself. Trace-assert netId identity before trusting it.
5. Gate the deferred DISCONNECT on the TRANSIENT's `animationIsBusy`, not the thrower's. A null
   `lookup(netId)` at the gate can only mean "already gone" under FIFO order → apply, don't hold.
6. Keep `reserveSeqRef` / move-hold arming at DECODE — only OBJ_CREATE minting moves to execute,
   or the same-beat corpse-fid leak regresses for every attack.
7. Never bind the in-world flight transient into the dude's inventory mirror (single-membership is
   a teardown double-free invariant).
8. If a transient still lingers in testing, the answer is a missing deferred event draining — NOT
   a client-side cleanup timer. The cap is the only sanctioned unilateral client action, and it
   APPLIES the server's event, never invents one.

### 12.7 Where the build differs from §12.2, and what it costs (Opus, step 2)
Three deltas from the sketch. Each is load-bearing — do not "simplify" them back.
1. **The feeder lives at the `event()` DISPATCHER, not inside the five handlers.** All five
   deferrable events carry `netId` as their FIRST wire field, so one peek covers them; five
   in-handler checks would also have to re-serialize a payload each handler had already consumed.
   Trap 3 is satisfied differently than written but fully: the handler guards are not *ordered
   before* the entangled check, they are *postponed with it* — the drain re-dispatches through
   `event()`, so `onDestroy`'s gDude/loot-modal branches run unchanged, just later. (Neither can
   fire on an adopt netId anyway: adopts are items, those guards protect critters/containers.)
2. **A dropped `kRecordedSeq` must RELEASE the mints it promised.** §12.2 has the dry pass promise
   and the execute pass fulfil, but `enqueue()` can drop the sequence *between* the two — then the
   netId is entangled forever with nothing left to un-entangle it, which is trap 2 reached by a
   second route. The queued entry therefore carries its own `seqAdopts` list and the drop path
   decrements it. State still converges (the CONNECT self-heal materializes the real item); only
   the flight visual is lost, which is what dropping a sequence has always meant.
3. **The drain needs a re-entry guard** (`_applyingDeferredEvent`). Re-dispatching through
   `event()` lands back on the feeder, and the netId is *still* entangled at that instant —
   draining the event is precisely what un-entangles it — so without the guard a parked event
   re-parks itself forever. Saved/restored, not blind-cleared: an overflow inline-apply can nest
   inside a drain.

**Known cost, accepted:** the cap is stamped when an event reaches the FRONT, so N events parked
behind one genuinely wedged animation serialize their caps (N × 4 s) rather than sharing one. Real
throws park ~2–3 events per item, and the first force-apply is the DISCONNECT — after which
`lookup` returns null and the rest apply immediately. Revisit only if a live wedge shows it.

**Blast radius, measured:** `presRecordSetAdoptNetId` has exactly ONE caller (`actions.cc:1013`,
non-explosive `ANIM_THROW_ANIM`), so `presEntangled` can only ever be true for a thrown weapon's
netId during its flight. Bullets, rockets, grenades, dynamite and every non-throw event decode
byte-identically to before.
