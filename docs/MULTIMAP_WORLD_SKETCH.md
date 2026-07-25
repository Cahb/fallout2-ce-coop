# Multi-map freeroam: what `struct World` actually is

Design sketch, post-v1.0 vision pass (2026-07-20). Question: 2–3+ players on
DIFFERENT maps simultaneously on one dedicated server, reconciling to a single
save. Representative census, not an audit.

## The shape of the answer

`struct World {}` as literally proposed — one struct threaded through the whole
tick engine — conflates two layers that the engine already separates. The truly
world-scoped state (gvars, game clock, worldmap flags, quest state, the save
slot) is SMALL and already funneled; the state a second live map collides on is
exactly the state the engine ALREADY serializes to a `.SAV` file on every map
transition. The map-parking boundary is the seam, and it is enforced by
construction today: no live cross-map object pointer can exist, because
everything off-map is on disk. So the primitive to multiply is not World — it is
**`MapInstance`** (the .SAV closure plus live-only session state), while World
(gvars/clock/worldmap) stays a singleton per server process. **Recommendation:
Option A below — in-process MapInstance bank-switching, bootstrapped from the
existing map save/load code path — with the process-per-map option banked as the
later scale/crash-isolation upgrade.** Full mechanical threading (a ctx
parameter on every function) is a ~400-global, every-core-module refactor and is
not worth it at this player count.

## Candidate architectures

### A. In-process MapInstance bank-switch (RECOMMENDED)
One process, one World singleton, N `MapInstance` slots. `serverTick` becomes
`for each live map: mapBind(m); tickMap(); mapUnbind(m);` — sequential, no
parallelism needed for 2–3 players. mapBind swaps the per-map global set
(object list, tile grid, script lists + the lvar block, mvars, combat session,
anim registry, per-map queue slice) in and out of the struct; gvars/clock/party
blobs are never swapped. **Bootstrap trick that de-risks the whole thing:** v0
of bind/unbind can literally be the existing `mapSaveToStream`/`mapLoad` run
against a memory buffer — correct by reuse of the code that already proves the
swap-set daily — then optimized to pointer swaps hot-path by hot-path.
Cost: moderate — enumerate the swap-set, split the queue (below), keep viewer
netId scoping per-map. Risk: a missed static = silent cross-map state bleed
(the worst bug class; mitigated by the serialize-v0 bootstrap and by keeping
new session state in single structs). Reconciliation mostly DISSOLVES: one
process, one gvar array, one clock — "sync to one save" is just saving.

### B. Process-per-map shards + reconciling parent
Each live map = an f2_server process; parked maps stay `.SAV` files; a small
parent owns the save dir, gvars, clock, and worldmap, and routes players
between shards (transition = shard serializes .SAV + a "player blob", parent
spawns/hands off to the target shard; viewer handoff = the existing
join/rebaseline flow pointed at a new port). Gvars: per-shard read replica,
writes forwarded to the parent (read-your-writes locally, cross-shard eventual
within a beat). Wins: crash isolation (aligns with the uncrashable-mod
requirement), parallelism for free, zero swap-set risk — misses show up as
protocol errors, not silent bleed. Cost: real infra (orchestration, gvar sync
protocol, player-blob extraction, shard lifecycle) plus the gvar staleness
window needs an owner ruling. Right answer LATER at higher scale; overkill for
2–3 players run by one person.

### C. Full threading (`tickMap(World*)` everywhere)
Every global in object.cc/combat.cc/scripts.cc/tile.cc/map.cc becomes a World
member and every function takes the pointer. ~400+ file-scope globals across 8
core modules, thousands of call sites, months of churn, and the goldens
re-bless at every step. Only worth it if the codebase were being rewritten
anyway; it isn't. Rejected.

## The var layer

- **GVAR** (`gGameGlobalVars`, game.cc): world-scoped, ONE array — and access
  already funnels through `gameGetGlobalVar`/`gameSetGlobalVar` (game.cc:115,
  126). ~62 direct-call sites outside game.cc across 9 files plus the two
  interpreter opcodes. Under Option A this array is NEVER multiplied and needs
  no ctx.
- **MVAR** (`gMapGlobalVars`, map.cc): per-map, loaded at map load, written to
  the .SAV at leave. Already MapInstance-shaped; swaps with the bank.
- **LVAR** (`gMapLocalVars`, map.cc): one flat per-map block; each Script holds
  `localVarsOffset/Count` into it; serialized in the .SAV. Per-SCRIPT-INSTANCE
  within a per-MAP block — also already MapInstance-shaped.

So `gvar_set(ctx, X, v)`: **ctx is the World, and with one World per server
process, multi-map freeroam needs no ctx threading in the gvar layer at all.**
The ctx parameter only becomes real for (a) hosting multiple independent worlds
in one process (don't; run two servers) or (b) mod-namespaced var stores
(IDEAS.md B2 — a separate keyed store, not a retrofit of the vanilla array).
ctx is definitely NOT the player: per-player state is the sheet/actor work,
already shipped on a different axis. The IDEAS.md wall #2 framing ("touches the
interpreter and every opcode") is therefore MOSTLY DISSOLVED by the two-layer
split: MVAR/LVAR ride the map bank, GVAR stays singular. The remaining
discipline is only: never index `gGameGlobalVars` directly in new code.

## Singleton census (representative; file-scope global counts by grep)

| Module | ~globals | Scope verdict | Hard or tedious |
|---|---|---|---|
| object.cc | 102 | per-map (list + HEX_GRID by-tile heads) | tedious — the .SAV already captures it |
| combat.cc | 114 | per-map (per-fight); session struct exists | tedious-plus — session machine helps; time-freeze semantic is the hard part (below) |
| scripts.cc | 79 | MIXED: gScriptLists per-map; `gGameTime` world; requests/CSD per-map | the real classification work lives here |
| map.cc | 48 | per-map by definition | tedious |
| tile.cc | 46 | per-map derived geometry | tedious |
| party_member.cc | 41 | per-PLAYER once leash is removed (walks with its player) | hard-ish: becomes part of the player blob, not the map |
| critter.cc | 28 | mixed dude/world | classification pass |
| queue.cc | 9 (one list) | **MIXED-SCOPE SINGLETON**: one `gQueueListHead` holds map events (explosion/flare/script timers on map objects) AND world events (drug/poison/radiation on players) | **hard** — needs an explicit split into world-queue + per-map queue; `_queue_leaving_map` (queue.cc:511) already clears the map-typed events, proving the classification exists implicitly |
| worldmap.cc | 9 | world + per-player travel position | per-player worldmap travel is its OWN lift regardless of architecture |
| game.cc | 8 | world (gvars) — stays | none |
| RNG | 1 stream | per-world determinism dies under interleaved maps | accept; goldens stay single-map |

Not verified (flagging, not guessing): the proto cache — critter proto data is
mutable per instance and the cache is process-global; map transitions handle it
today, so the bank-switch must mirror whatever the save/load path does with
protos. Check before committing to pointer-swap v1. Same for `_cur_id`: under
Option A it stays one monotonic mint (no collision, only the known 18000
budget); under Option B it must be range-partitioned.

## Conflict classes for reconciliation

Under Option A most of "reconciliation" evaporates (one process, one array,
sequential ticks). What remains is semantic, not mechanical:

1. **Time-advance verbs** ►► OWNER RULING. Rest and worldmap travel fast-forward
   the ONE world clock. Player A rests 8 hours while player B is mid-fight on
   another map: whose time is real? Options: rest requires all-player consent /
   rest is per-player and merely heals without advancing world time (non-
   faithful) / time advances and other maps sim through it. This is the single
   biggest design decision and it exists in EVERY architecture.
2. **Combat time-freeze** — vanilla freezes world time in combat; ill-defined
   with a second live map. The server loop already ticks sim time during fights
   (SERVER_LOOP_DESIGN §3), so the precedent is set: rule that combat no longer
   freezes the world clock. Low-controversy, but it is a vanilla divergence —
   worth one explicit owner nod.
3. **Cross-map gvar write interleaving** — two maps' scripts writing the same
   quest gvar in the same era. Under A it is just script-execution order (same
   class as vanilla's within-map ordering, plus the known double-fire
   masquerade class); under B it is a real staleness/LWW window ►► ruling
   needed only if B is chosen.
4. **The savegame's single dude position** — the save format stores ONE current
   map + position. N players on N maps needs per-player map/pos in the save
   (natural extension of the per-player sheet work) ►► ruling: extend the
   format vs "saving requires rendezvous on one map" as an interim rule.
5. **Cross-map object motion by scripts** — believed rare-to-absent (vanilla
   scripts fake NPC relocation with gvar-gated dual instances because the
   engine cannot touch an unloaded map). Verify with a script-opcode sweep
   before relying on it; if genuinely absent, the isolation boundary is airtight.
6. **Uniqueness/dup invariants** — avoided BY DESIGN with one ledger rule: a
   map is either live in exactly one MapInstance or parked as exactly one .SAV,
   never both; a player blob lives on exactly one map. Make that rule explicit
   in the transition code and reconciliation stays well-defined.

## What v1.0 must not lock in (do these NOW, all cheap)

1. **Gvar discipline**: all new code goes through `gameGet/SetGlobalVar`; never
   index `gGameGlobalVars` directly. (Existing ~62 outside sites are fine —
   they're already function calls in 7 non-interpreter files.)
2. **Session-state hygiene**: new modal/driver/combat state goes in ONE named
   session struct per subsystem (the resumable-combat pattern), never scattered
   file statics. The future swap-set is then a LIST OF STRUCTS, not an
   archaeology dig. This is the highest-leverage constraint on the list.
3. **Queue scope tagging**: when adding queue event types, decide and mark
   world-scoped vs map-scoped (the `_queue_leaving_map` clear-flag is the
   existing marker). Don't add a type that is secretly both.
4. **netId scoping**: keep netIds and the join/rebaseline flow per-map-instance
   in meaning; never build a wire feature that assumes one global netId space
   for "the whole server forever". Rebaseline-on-transition is exactly the
   shard/bank handoff later.
5. **No cached `Object*` across ticks or transitions** in new code (the
   id+stale-pointer antipattern, already a tracked de-rot class) — both the
   bank-switch and any reconciliation die on stale pointers.
6. **Save-format shape**: keep the save as savegame + N per-map .SAVs (as it
   is). Don't collapse map state into a monolithic blob; the per-map file IS
   the MapInstance serialization format.
7. **Don't deepen the leash**: party membership stays removable v1 glue (the
   standing actor-model invariant); in freeroam, party members become part of a
   player's blob, not the map's — any new party code keyed on "the one party"
   deepens the wrong scope.

## What I did not do
No exhaustive call-site enumeration; counts are grep-representative. Proto
cache mutability and the cross-map script-motion claim (conflict class 5) are
the two facts to verify before implementation planning.
