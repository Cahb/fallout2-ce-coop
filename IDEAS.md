# Fallout 2 Community Edition - Modding & Feature Ideas

This document outlines architectural strategies and workflows for implementing complex new features and mods within the engine.

## Designing Complex Features (e.g., Hideout Mod)
For non-trivial features (e.g., outpost management, traitor mechanics, building systems), adopt a hybrid architectural approach:

### 1. Hybrid Architecture
- **C++ Engine:** Reserve for generic, low-level, or reusable functionality (e.g., drawing a new UI window, handling specialized container interactions, or new low-level engine hooks).
- **Sfall/Scripts (`.ssl`):** Implement gameplay-specific logic here (e.g., NPC AI behavior, construction timers, event triggers, traitor sabotaging logic).

### 2. Implementation Workflow
1.  **Map Design:** Use the Fallout 2 Map Editor to construct the location and place initial objects.
2.  **Define Prototypes:** Create new `.pro` files (using community tools) to define new building types or interactive objects.
3.  **State Management:** Reserve a block of global variables in `sfall_global_vars.h` to persist the state of your feature (e.g., `Hideout_Construction_Level`, `Traitor_Active`).
4.  **Scripting:** Write `.ssl` scripts to handle the core logic (hiring NPCs, triggering events, managing expansion timers) by reading/writing your global variables and using engine-exposed opcodes.
5.  **UI/Interaction:** Use `window_manager.h` for custom interfaces if existing dialogue or menu systems are insufficient, then expose a simple bridge to the scripting engine to trigger the UI.

### 3. Example: Traitor Mechanic
- **Logic:** Do not hardcode "traitor stealing items" in the engine.
- **Implementation:**
    - Use a `HOOK_MAP_UPDATE` script.
    - Script checks: `If Traitor_Active and current_map == Hideout_Map`.
    - Script action: Randomly move/delete items from containers or trigger dialogue events based on probability.

## Freeplay After Endgame (non-faithful / quirky option)
Vanilla Fallout 2 is terminal on the winning ending: `endgamePlayMovie` ->
`endgameEndingHandleContinuePlaying` -> `_game_user_wants_to_quit = 2` (credits,
back to main menu; no freeplay). The engine already has the latent "Yes/keep
playing" branch (endgame.cc:286, the YES_NO prompt) that simply does NOT set the
quit flag.

Idea: an optional **freeplay mode** where the endgame does not end the game.
- Simplest: take the "Yes/keep playing" branch (skip the `quit = 2` write) so the
  world keeps running after the slideshow/movie.
- Quirky extensions (deliberately non-faithful): on endgame, teleport the party
  somewhere (a post-game sandbox map / a "wasteland after the war" state), unlock
  areas, or seed a new-game+ style continuation.
- Server angle: for the headless server this is the natural persistent-world
  choice -- the endgame becomes a scripted event, not a session terminator.
  (The current headless decouple hard-codes the faithful terminal `quit = 2`;
  freeplay would be a config/flag that selects the "keep playing" branch instead.)

---

# The Big Picture: Client/Server as a Modding & Extension Platform

The dedicated-server rewrite is **not only about multiplayer/co-op**. An authoritative
server + thin client is, structurally, a very flexible **modding and extension
platform**: the server owns the whole simulation and can run logic the client knows
nothing about, so new content and systems can be added server-side without touching
(or even redistributing) the client. This section banks the feature vision AND — more
importantly for the rewrite in progress — the **architectural constraints these
features imply**, so we do not bake in assumptions we will have to tear out.

> Cross-reference: the durable-actor invariant (a player is a first-class actor;
> party/leash is removable v1 glue) is the load-bearing rule that keeps most of the
> below *additive*. See the `mp-actor-architecture-principle` design note.

## A. Server-Side Scripting (client-unaware logic)
- **Server-driven scripts** (Lua favored over `.ssl` for new content) that the client
  is unaware of — they just observe the resulting state/events over the wire.
- Start simple: spawn random enemies / crates / loot on a map; ambient world events.
  Not necessarily loops — can be one-shot "on enter, seed the map" logic.
- Grow toward complex: dynamic random encounters, faction behavior, raids (below).
- **UNCRASHABLE is a hard requirement.** A script error must NOT take down the server.
  A misbehaving script is isolated and stopped/terminated cleanly; ideally the server
  gets a chance to **save state** before shedding it. (Sandboxed Lua VM, per-script
  error boundary, resource/time budgets, no direct engine-memory access — scripts act
  through a validated API only.)
- Implication for the engine: a **scripting API surface** that is safe by construction
  (capability-limited, cannot corrupt sim invariants) — distinct from vanilla `.ssl`
  opcodes which assume a trusted single-player context.

## A2. Client-Side Scripting & the Custom-Message Channel
- Scripting is not server-only. **Client-side Lua mods** should be an option too:
  local UI, cosmetic/HUD additions, client-only convenience behavior — things that need
  no server authority.
- **Server-initiated client behavior** via a **mod-defined custom-message channel**:
  a mod defines its own message types; the server can send one, and the client-side mod
  handles it (e.g. pop a custom UI window, play a local effect, prompt the player). So
  the flow is bidirectional — the client can handle server requests AND drive its own UI.
- Combined with C's opaque items: a server-only item "used somewhere" can emit a custom
  message that a client mod turns into a bespoke UI, with the engine never hardcoding
  either end.
- ►► Protocol implication (already half-built): the MP wire uses **skip-unknown-type**
  framing (an unknown event type is structurally skipped, not fatal — MP_PROTOCOL.md).
  That is precisely the substrate for mod-defined message types: a **reserved
  "custom/mod message" envelope** (opaque payload, addressed to a mod id) rides the same
  wire, forward-compatible by construction — a client without the mod safely ignores it,
  a client with it dispatches to the mod's handler. Design the reserved envelope range
  deliberately rather than letting mods squat on engine type ids.

## B. Persistence / Saves on a Dedicated Server
- A dedicated persistent world needs a save model that is NOT the single-player
  savegame-on-demand. Server periodically/eventually persists: world state, per-player
  actor state, AND **mod/script-owned state**.
- **Engine provides the MECHANISM; the mod owns the POLICY.** The engine exposes a
  **save/restore context API** — a mod/script declares what it needs to persist (e.g.
  "these unique enemies I spawned + their per-instance state") and writes/reads it
  through the provided ctx. The engine does NOT know the mod's schema; it guarantees
  the ctx is serialized/restored at the right lifecycle points. Each mod is responsible
  for its own save/load logic; the engine just hands it a durable, scoped store.
- This couples tightly with (A) uncrashable scripts: "give time to save before
  terminating a failing script" means a failing mod gets a chance to flush its
  save-ctx before it is shed. State is a first-class, serializable, per-mod thing.
- Open question banked: save granularity/cadence (snapshot whole world vs per-actor
  vs per-map deltas), and how a joining/returning player is restored.

## B2. Mod Ecosystem (the "API for everything" principle)
- Guiding principle: **bake an API for everything; mods resolve their own concerns
  through it.** The engine is a capability provider, not a policy-maker. Anything a mod
  wants to do (spawn, persist, define content, hook events) goes through a provided,
  validated API — never direct engine-memory access (ties back to the uncrashable
  sandbox in A).
- **Mod-owned responsibilities (engine provides the tools, mod decides):**
  - **Load order** — mods declare/resolve their own ordering.
  - **Dependencies** — a mod declares what it needs; resolution is a mod-ecosystem
    concern layered on the API, not hardcoded engine behavior.
  - **Save/restore context** — see B; each mod manages its own persisted state.
- **OPEN / unresolved: conflicting mods.** e.g. two mods each spawn a "unique" enemy on
  the same tile, or otherwise contend for the same world resource. No mechanism decided
  yet (conflict detection? priority via load order? namespacing of spawns/content ids?).
  Flagged as a distinct future design topic — do not assume it away.

## C. Custom Content (items / weapons / usable objects)
- Two tiers of custom item:
  1. **Client-aware custom item** — client must render/interact with it, so it needs
     the asset knowledge (icon, text, animation, inventory art).
  2. **Server-only / opaque item** — the client just knows "id + display text + which
     icon to load," and using it *somewhere the server cares about* triggers a
     server-side script/effect. The client is a dumb renderer of an opaque handle.
- Implies a **content-descriptor channel**: the server tells the client "here is item
  id N: name, icon ref, flags" so the client can display items it has no built-in
  proto for. (A generalization of the proto system into a server-authoritative,
  streamable registry.)
- Same pattern extends to usable objects, custom weapons, etc.

## D. Player Systems (freeroam-era, additive on the actor model)
- **Per-player cars** (obvious once freeroam exists). Car as an actor-owned entity.
  - **Car inventory** — store items in the car (may exceed vanilla car-trunk behavior).
  - **Car upgrades / improvements, repair.**
- **Hideouts** — FONLINE-style: deploy a tent on a random map tile → creates a hideout
  (reuses the existing "enter tile → load a map" mechanic, but the destination is a
  persistent player-owned instance). Bases/outposts grow from here.
- **Survival mechanics:** food / water / sleep as tracked needs.
  - Debuffs: vision impairment when unslept; Strength (etc.) penalties when low on
    food/water.
- **Scavenging & crafting:** junk scavenging on maps; wood/food/water gathering;
  food from cactuses / corn fields; cooking & crafting chains.

## E. World Systems (faction / conflict)
- **Enemy raids on hideouts** and **raids on cities** (server-scheduled dynamic events).
- **Wanted system** + a proper **faction relation system** (reputation as structured,
  queryable, per-faction state rather than scattered gvars).

---

# ►► Architectural Implications the Rewrite Must Respect NOW

These are the parts of the vision that constrain *current* decisions — flagged so we
avoid lock-in. Most features above are additive on the actor + event-stream model; the
two hard walls below are inherent and worth naming early.

## 1. Multi-map parallel simulation
Freeroam / parallel play means the server must eventually hold **multiple maps in
memory at once** and advance ticks against each (parallel or sequential — doesn't
matter which), because player 1 may be on simulated map X while player 2 has freeroamed
to map Y. This directly collides with the engine **singletons**: one `gMap`, one active
object list, one global combat context. Multiplying those is the big inherent lift
(not created by any v1 decision, but real). **Do not deepen the single-world assumption**
in new server code — keep "the current world" addressable rather than implicitly global
wherever cheap to do so.

## 2. Script-var state must be namespaced / serialized per-context
Vanilla's global/local/map vars (GVAR/LVAR/MVAR) are a flat, globally-shared,
script-scattered namespace — fundamentally single-world. A persistent multi-player,
multi-map world needs these **heavily serialized and context-scoped**: not
`gvar[X] = 1`, but conceptually `gvar_set(context/player, X, 1)` — i.e. a var read/write
is relative to *which world instance / which player* it belongs to. This is a large,
cross-cutting change (touches the interpreter, every script opcode that reads/writes
vars, and the save format). Bank the shape now; it is a Phase-4+ item, but it informs
how we treat the var subsystem in the meantime (avoid new code that assumes the single
global var table is the whole truth).

## 3. Run a design-vs-vision audit (FABLE) at a checkpoint
At some point, run a thorough pass (FABLE-tier) of **this IDEAS vision against the
current design + implementation** to classify each item as: (a) additive / already
compatible, (b) needs a simple localized rewrite, or (c) fundamentally incompatible with
a current assumption (→ surface the assumption before it hardens). Priorities: the two
walls above, the scripting-API safety model, and the save/persistence model.

## Respec ("rebuild character") — owner idea 2026-07-22
Anytime-usable respec: a potion / item / NPC conversation / UI button that drops
stats, SPECIAL, tags, traits and perks, PARKS earned level + XP, and sends the
player back through character creation. Cheapest implementation rides the
existing create chain: prompt the player to relog, serve the creation screen
(`create` spec → `playerCreateApply`), then restore banked level/XP and re-level.
⚠ The tricky part: items/implants that grant stat bonuses (+1 ST armor, boxing
implants, …) live as modifiers against the sheet — the rebuild must strip
equipment-derived modifiers before snapshotting "earned" state and re-derive
them from equipped items after, or a respec permanently bakes (or loses) the
bonus. Same hazard class as the critterSetBaseStat traits-before-SPECIAL trap.
