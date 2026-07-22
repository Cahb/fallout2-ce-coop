---
name: fonline-prior-art
description: "FOnline sources as prior-art reference for hard MP problems — read for ideas, never copy code; PARKED, per-problem go/no-go"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 5421653f-de17-4c14-80cb-96e53a2f82d9
---

FOnline (ground-up MP Fallout engine) is domain-close to the P5 server track — worth
consulting as PRIOR ART for specific hard problems. PARKED per user 2026-07-17: do NOT
do a general trawl; when we hit a matching design-class problem, PROPOSE a focused read
of just that subsystem and wait for the user's go/no-go, then bring back "how they did
it + what maps to our constraints."

TWO HARD CAVEATS:
- LICENSING: FOnline forks carry their own licenses ≠ fallout2-ce. Read for PATTERNS,
  never paste code (keeps the repo clean). Ideas only.
- DIFFERENT FOUNDATION: FOnline is clean-slate with its own object model — it never had
  our core constraint, the ORIGINAL engine SINGLETONS (gMap/gDude/one object list/one
  combat ctx = our roadmap wall, [[mp-actor-architecture-principle]]). So their CONCRETE
  data structures won't transfer; only the architectural shape of solutions.

HIGHEST-ROI subsystems to peek WHEN we reach them (per-problem, gated on user go/no-go):
1. Multi-map / per-context server (many maps at once) = exactly our banked de-singleton
   wall — the best single study when we tackle co-op de-singletoning.
2. Network sync protocol shape — sync-vs-derive, object/critter/INVENTORY delta framing
   (near-term relevant to the banked ammo/nested-container streaming = A2), interest
   management for N-viewers.
3. Client prediction / interpolation — our authority-leads-presentation glide/teleport
   class (present-tile FSM).
4. Scripting/modding sandbox — the server+client Lua vision (IDEAS.md).
