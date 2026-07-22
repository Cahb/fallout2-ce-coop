---
name: post-demo-hardening
description: "The 3 recurring \"taxes\" that make each viewer slice FEEL like rework (but the core is convergent, not band-aid); each has a banked consolidation. Pay down AFTER Demo v1, when the demo names which one actually hurts."
metadata: 
  node_type: memory
  type: project
  originSessionId: 778c8d95-a359-4170-ac01-4b88e44ca96d
---

User voiced the real fear (2026-07-18): "are we building something right or band-aiding —
each new change legit IS a new one." Grounded answer: the CORE is **convergent, not band-aid**.
Every viewer slice since interaction-verbs is the SAME three moves — server runs real vanilla
logic + stays authoritative; viewer reroutes each mutation leaf behind `clientViewerActive()`
→ a claim-gated wire verb; truth streams back via `OBJECT_DELTA`. Equip/drop, loot, interact,
the coming `useitem` — structurally identical (recon literally kept saying "mirrors the
invwield pattern"). The wire framing (skip-unknown-T, deltas + EVENT_*) absorbs features
ADDITIVELY; it hasn't changed shape. Even dialog's one new mechanism (server block-and-pump)
is the SERVER MIRROR of the client pump-and-bail we already built — the pattern GENERALIZING
to both sides, not fragmenting. Precedent that the project self-corrects: the anim FSM
consolidation (band-aid pile #2/#6/#9 → deliberate Fable refactor 2a/2b, behavior-neutral).
The noticing-a-pile instinct is the immune system, not the disease.

**Why it still FEELS like rework — the 3 recurring TAXES (each has a banked consolidation):**

1. **Leaf-enumeration is error-prone.** Rerouting a vanilla BLOCKING modal means finding
   EVERY mutation leaf + a refresh hook. Loot took 3 live-verify rounds (missed Take-All
   button, missed refresh, stale container mirror). Pattern sound; enumeration bites.
   → Consolidation: a leaf-audit / single choke-point per modal (enumerate up front).

2. **Modal pump-and-bail is re-solved per modal** (inventory, loot, dialog, barter, the C4
   SET_TIMER modal). Same hazard, hand-rolled each time. → Consolidation: a shared modal
   wire-pump wrapper (add-to-mask + ticker + force-ESC-on-combat/rebaseline/disconnect as one
   reusable unit).

3. **Object-lifetime + un-streamed data (the "A2" gap) recurs.** Every feature needing ammo/
   charges/nested-container contents re-hits the SAME known encoder wall (object_delta.cc
   top-level-only); each add/remove path needs its own adversarial object-lifetime review.
   → Consolidation: **item-instance wire IDs** (address by stable id not pid → kills the
   dangling/dedup class) + finish the encoder (nested + ammo/charges). Already banked as the
   co-op MP-identity/lifetime bundle.

**STRATEGY: stay feature-first to Demo v1 (Temple solo pass — dialog is the last real
blocker). Do NOT refactor now — premature abstraction on guesses is its own trap. Let the
demo name which tax actually hurts, THEN pay that one down post-v1 with a real target.**
See [[p5-server-plan]], [[mp-actor-architecture-principle]], [[anim-decouple-verification]].
