---
name: mp-actor-architecture-principle
description: "LOAD-BEARING design invariant for the f2 multiplayer/server roadmap — a player is a first-class ACTOR; party/leash is a removable v1 behavior, never the player's identity. What to avoid + what we refuse to lock in irreversibly."
metadata: 
  node_type: memory
  type: project
  originSessionId: 8b14f578-f7fb-4617-bcaf-af5cc7321c8a
---

The single architectural invariant that keeps the whole MP roadmap ITERATIVE with
minimal throwaway (user's explicit goal, 2026-07-13). Governs [[p5-server-plan]] and
everything after it (freeroam -> parallel -> Lua encounters). Decided after weighing
whether reusing Fallout's party system for v1 co-op would cripple later stages.

════════════════════════════════════════════════════════════════════════════════
THE PRINCIPLE
════════════════════════════════════════════════════════════════════════════════
A connected player is modeled as a FIRST-CLASS ACTOR (its own identity, command
routing, inventory, event stream — the Phase-4 Character/PlayerActor aggregate).
This abstraction is DURABLE: every roadmap stage needs it and it never gets rewritten.

"Party member" is NOT the player's identity. It is a REMOVABLE v1 BEHAVIOR (leash/
follow/leader-mediation) layered ON TOP of the actor. We BORROW party mechanics
(follow-pathing, combat-turn slotting) as v1 conveniences; we do NOT adopt party
membership as what a player IS.

Mental model: `PlayerActor` (durable) <- a swappable "party/leash strategy"
(disposable v1 glue). Freeroam = detach the strategy. The actor survives untouched.

════════════════════════════════════════════════════════════════════════════════
WHAT WE MUST AVOID (the anti-patterns that create irreversible lock-in)
════════════════════════════════════════════════════════════════════════════════
- NEVER branch player logic on party-membership: no `if (isPartyMember(p))` deciding
  what a player can do. Party-ness is a v1 movement/turn strategy, not a capability gate.
- NEVER make the party list the SOURCE OF TRUTH for player state. The actor owns its
  own inventory/stats/position; the party system may reference it, not contain it.
- NEVER make leader-mediation the ONLY path for a player's commands/inventory (i.e.
  don't route player #2's actions exclusively through gDude/the party-trade screen as
  the sole mechanism — that's a v1 convenience, behind the actor's own command path).
- NEVER let player code inherit party.cc's structural assumptions (enumerated below).

════════════════════════════════════════════════════════════════════════════════
THE party.cc ASSUMPTIONS WE REFUSE TO LOCK IN (the concrete lock-in fears)
════════════════════════════════════════════════════════════════════════════════
Fallout's party system carries assumptions that are EXACTLY what freeroam/parallel
must break. If player identity == party member, breaking these later = un-inheriting =
real throwaway + regression risk. So we keep them OUTSIDE the durable actor:
- LEADER-OWNED: party members belong to gDude; no leader -> they break. (Freeroam has
  no single leader.)
- HARD 5-MEMBER CAP.
- LEADER-MEDIATED CONTROL: companions are managed via the leader's party-trade screen;
  they don't self-drive world interaction. (B-plus already pushes past this — #2 loots/
  barters independently — which is WHY #2 must be an actor, not a vanilla companion.)
- PARTY-WIDE SKILL POOLING: partyMemberGetBestSkill (used in the BARTER path!). A party
  pools skills; independent freeroam players must NOT pool — a co-op that shares one
  barter/lockpick skill across bodies is wrong for freeroam.
- SHARED-MAP-BY-DEFINITION: party members are auto-stored/restored on the leader's map
  transition; they cannot be on a different map. (Directly blocks "parallel freeroam on
  different maps".)
- SINGLE-COMBAT-CONTEXT: party members act within the leader's ONE combat. (Blocks
  parallel combats.)

════════════════════════════════════════════════════════════════════════════════
WHAT IS *NOT* THE RISK (so we don't over-engineer / chase day-one-perfect)
════════════════════════════════════════════════════════════════════════════════
The real hard walls are the ENGINE SINGLETONS: one gMap, one active object list, one
global combat context. They bite at "parallel freeroam / different maps / parallel
combats". BUT: they are already singletons in VANILLA; no v1 decision creates them; the
party-member reuse does NOT make them worse (it rides the same single context anything
would); and NO v1 choice makes multiplying them cheaper (it's an inherent from-scratch
lift whenever reached). So: do NOT contort v1 to pre-solve the singletons, and do NOT
fear party-member as if it causes them. They are orthogonal, roadmap-inherent cost.

════════════════════════════════════════════════════════════════════════════════
UI-DRIVING COROLLARY (user 2026-07-16, raised during combat-presentation design)
════════════════════════════════════════════════════════════════════════════════
Driving the client's VANILLA UI from local state (the P3 combat-presentation model,
[[p5-server-plan]] / COMBAT_CLIENT_DESIGN.md) surfaces the gDude-singleton wall in the
presentation layer. The resolution:
- gDude is a per-client ROLE = "the actor THIS client controls", filled by a DIFFERENT
  networked object on each client (its ORIGINAL meaning: "the local player character").
  The SAME server actor is gDude on its owner's client (drives that client's dude UI/HP/
  AP) and a plain remote OBJ_TYPE_CRITTER everywhere else. De-singletoning = make WHICH
  netId fills gDude a per-client JOIN BINDING (localized change, additive slice — NOT a
  rewrite). V1 today: every viewer binds gDude = netId 1 (host) = the current single-
  actor limitation (risk b).
- The dude/critter DUALITY is FREE from the data model: HP/AP/damage live on the critter
  OBJECT (critter.hp, critter.combat.ap) — per-actor by construction — NOT in gvars. The
  same hp field is "my HP bar" on the owner client and "that critter's state" elsewhere.
  So combat-presentation creates NO gvar-scoping lock-in. gvars/lvars are a SEPARATE axis
  (world/quest flags; per-player scoping = gvar_set(ctx,X,v), banked in IDEAS.md).
- GUARDRAILS to hold NOW (cheap, no co-op built): (1) key "my turn" on
  turnStart.netId == my actor's netId, NEVER on isPlayer alone (a player TURN_START for
  another netId = another player's turn); (2) new combat-UI hooks read the local actor
  THROUGH the gDude role, never assume gDude is the only player-capable critter. Don't
  pre-solve; just don't hardwire "one player" where the wire already carries netIds.

════════════════════════════════════════════════════════════════════════════════
SCRIPT-SIDE gDude HARDCODING (user's concern 2026-07-17, GROUNDED) — the real chokepoint
════════════════════════════════════════════════════════════════════════════════
User's fear (valid): FO2 SCRIPTS themselves hardcode "the dude", so de-singletoning
isn't just an engine-ptr swap. CONFIRMED: `dude_obj` is a script opcode — opGetDude
(interpreter_extra.cc:1124) literally `programStackPushPointer(program, gDude)`. Scripts
do `if (source == dude_obj)`, award XP to dude_obj, set quest gvars off it. ~36 gDude
refs in interpreter_extra.cc (opcode layer); combat.cc has ~119 (mostly UI/turn/camera
edge, some logic). WHY IT'S STILL BOUNDED, NOT open-ended: (a) engine core (combat/
actions/movement) is ALREADY actor-parameterized — takes Object* actor, not gDude — so
the executor path is fine (the scripted-door USE ran with source = the acting actor, not
hardwired gDude; per-player-correct if bound right). (b) gDude-the-singleton survives only
at the per-client BINDING edge (UI/turn/camera) + the dude_obj opcode + a small set of
dude-hardcoded opcodes. (c) The opcode table is a CLOSED WORLD (~400 entries) → the
dude-hardcoded set is enumerable and finite, not a needle hunt. FIX SHAPE (deferred v1
wall, do NOT build now): make dude_obj resolve to the ACTING/CONTEXT player (script exec
already has scriptSetObjects(sid, source, target) — the "who triggered this" is available
to thread), same family as the gvar_set(ctx,X,v) per-context namespacing in IDEAS.md. So:
legitimate fear, but a bounded closed-set rewrite over a known chokepoint, and the actor-
parameterized engine + clean seam keep it additive. Keep NOT locking in party-branching
or gDude-hardwiring in NEW code; that's the whole insurance. See [[p5-server-plan]].

════════════════════════════════════════════════════════════════════════════════
THE GUARDRAIL (the entire insurance policy = ONE clean seam)
════════════════════════════════════════════════════════════════════════════════
Exactly one seam between the DURABLE actor and the DISPOSABLE v1 behavior. Party
mechanics (follow-pathing, combat-turn slotting) are attached to the actor via a
strategy object / flag and are deletable when freeroam lifts the leash. This is the
same discipline already used elsewhere (two-commit gates, byte-identical anchors,
adversarial review): isolate the throwaway to a thin, clearly-labeled boundary. Not
day-one-perfect — the leash IS throwaway (~a few hundred lines of glue), and that's
fine — but the 95% that's expensive (actor, command routing, event stream, interaction
generalization) is forward-carried into every stage.

════════════════════════════════════════════════════════════════════════════════
ROADMAP TRAJECTORY (why iterative / additive holds)
════════════════════════════════════════════════════════════════════════════════
UI/core split (DONE) -> MP sync (authoritative server + event stream + command
dispatcher + presenter widening for object events) -> PlayerActor + B-plus (leashed
follow, independent loot/barter/use-skill on nearby targets via bounded auto-approach)
-> same-map freeroam (DROP the leash + build the gDude->N generalization of spatial
triggers/aggro — ADDITIVE, not throwaway) -> parallel / different-maps (multiply the
world+combat singletons — the big inherent lift) -> Lua-driven encounters/bases/raids
(orthogonal, layered on the actor+event model). Each stage ADDS; the only deliberate
throwaway is the leash glue.

════════════════════════════════════════════════════════════════════════════════
LOCKED DECISIONS (2026-07-13, user)
════════════════════════════════════════════════════════════════════════════════
- #1 ACTOR MODEL: a player is a first-class actor. Party is a behavior, not an identity.
- #2 MOVEMENT: v1 = B-plus. Leash = restricts INDEPENDENT LONG-RANGE LOCOMOTION only
  (no wandering off, no independent spatial-trigger/encounter). It does NOT forbid
  target interaction: #2 can loot/barter/use-skill on NEARBY targets (bounded auto-
  approach) and manage own gear freely. Designed so the leash can later be disabled.
- Interaction paths (loot/barter/use-skill) get generalized from gDude-centric to any
  player actor (bounded work; the first bite of the gDude->N generalization).
- #5 SERVER CADENCE: LOCKED (see [[p5-server-plan]] for the full resolution) — logical-
  time-only beat-paced server, client self-paces, catch-up = apply-state/skip-animation,
  snapshot = join+resync (one mechanism). 
- #6 MP COMBAT INITIATIVE: LOCKED (user 2026-07-15, resolves REWRITE_PLAN open q #2,
  full text recorded there): sequential vanilla-skeleton initiative + connected players
  as ONE contiguous chain anchored at gDude's slot (host, +1, +2, then roster). Host
  initiates ⇒ whole player party before defenders. Deliberate vanilla divergence:
  players grouped after host, NOT interleaved by Sequence stat. Machine consequence:
  one player input-barrier window per round; player-turn slot iterates a LIST of player
  actors (v1 = {gDude}) — never hardwire "exactly one player turn".
- STILL OPEN: #3 progression ownership = user "don't care, decide later". Next design
  item = the event CATALOG (#4 detail — which event types the protocol carries).
