---
name: combat-full-record-channel
description: "In-combat record/replay channel — the load-bearing MENTAL MODEL (concurrent paced presentation over synchronous global engine state) plus the remaining cosmetic tails. The whole channel is built + committed; combat presentation is in good shape."
metadata: 
  node_type: memory
  type: project
  originSessionId: fb4f8441-a7eb-4d1a-ad2a-9f99d42a0341
  modified: 2026-07-19T05:09:12.156Z
---

The in-combat record/replay channel (MOVE + wield-draw + throw-serialization + live-HP +
thrown-spear phantom-removal + equip-appearance) is BUILT, committed, and owner-live-verified.
Combat presentation is in genuinely good shape. What remains are known cosmetic tails.
See [[pres-record-build-track]] (the build track) and [[dialog-streaming-track]] (the current
active pivot OFF combat presentation).

## ►► THE MENTAL MODEL (owner-aligned, LOAD-BEARING for the tail)
The bugs are NOT random / sloppy / engine quirks. Fallout presents ONE animation at a time,
SYNCHRONOUSLY, over GLOBAL engine state; we drive it from a CONCURRENT, PACED wire stream.
Every vanilla global / actor-scoped assumption touched during presentation is a landmine. The
"race" is an ORDERING race between TWO LANES (immediate authoritative STATE vs paced
PRESENTATION) + between multiple seqs mutating one actor's engine state — NOT a thread race.
FIXES ARE CONVERGENT, not whack-a-mole: each = take a global/actor-scoped assumption → give it
per-actor/per-seq scope. Owner's "why not hold ALL state until presentation drains?" ANSWER: we
DO, but SELECTIVELY (held-delta family). GLOBAL hold is wrong — state is
authoritative+mandatory+droppable-presentation-independent (a stalled anim would freeze the
WHOLE world; a viewer skipping presentation must still apply state; MP late-joiners/other
clients/input all need immediate viewer-independent authority). Rule: hold ONLY the specific
fields a presentation owns (pos/AP/fid, next = lifecycle), reconcile on drain, keep it NARROW.

## ►► OPEN TAILS (all cosmetic, all one root — the deferred-lifecycle / held-delta family)
- **equip-while-GLIDING appearance** (workaround: equip while STANDING). While gliding obj->fid
  is the WALK fid; the equip is parked in pendingFid, applied only at glide-end. TWO writers
  fight over gDude->fid: client inv-UI (local) vs server delta, through the walk deferral. THE
  CLEAN FIX (~15 lines, NOT another patch): the local player's real appearance changes must NOT
  route through clientAnimDeferFid at all — the server never sends WALK-FRAME fids for gDude, so
  EVERY fid delta gDude gets is a real change → apply it IMMEDIATELY (write-through), pick ONE
  authority (server delta, not local inv-UI). Combat path unaffected (clientCombatAnimDeferDelta
  returns held=true there). LESSON (owner-flagged): over-patched this 8 fixes deep chasing
  symptoms; "standing works / gliding fails, BOTH armor & weapon" was the tell it's ONE
  architectural seam, not N bugs.
- **throw-retrieve projectile** (BUG 6 in [[presentation-viewer-bugs]]): a thrown weapon
  RETRIEVED in the same compressed AI turn is DISCONNECTED (pickup) before its flight seq drains
  → onDisconnect destroys the adopt transient → flight ops no-op → invisible flight + phantom
  accumulation. FIX = extend the held-delta family to OBJECT LIFECYCLE: hold the adopt
  transient's DISCONNECT/destroy until its throw/flight seq has PLAYED (a "held disconnect"),
  same shape as holding position, then destroy. Design-class but a KNOWN shape.
- **pickup GESTURE** is disabled (behind a `false &&` in actionPickUp): enabling it regressed the
  turn-serial pump + reintroduced the phantom, because the throw's adopt-mapping happens at
  presSeq PLAY time (pump) but the pickup's DISCONNECT fires at decode. FIX = move the
  adopt-mapping from PLAY time to DECODE time (create+register the adopt transient in the DRY
  pass at onPresSeq decode) so `_net[netId]` is set when the throw presSeq DECODES; then
  re-enable. A6 note: distance-3 zero-AP pickup gesture is pre-existing server sim semantics (not
  our regression) — gate recorded _obj_pickup on adjacency later.

## ►► LIVE RECIPE
`F2_SERVER_PRES_RECORD=1 VIEWERS=1 F2_TRACE_EVENTS=1 PACE=100 MAP=artemple.map
scripts/viewer_live.sh`, then `aggro` via the cmd port, skip turns. Kept diagnostic traces
(F2_TRACE_EVENTS-gated, cmove-* style): `[cmove-rec]`/`[cmove-play]` (server/viewer move),
`[cmove-hold]` (held position), `[cmove-drift]` (should be SILENT), `[cpickup]`/`[disc]`
(pickup), `[busy] STUCK` (pump-wedge watchdog), `[satk]`/`[dude-equip]`/`[dude-fid]`/`[ctakeout]`
(equip/draw). Design = COMBAT_MOVE_RECORD_DESIGN.md. See [[presenter-object-ref-model]],
[[visual-verification-protocol]], [[dont-declare-not-a-bug-confidently]].
</content>
