# MP_PROPOSAL.md — Same-map N-player co-op (Demo 0.2): the implementation specification

Status: **BUILDING** (2026-07-19). Stage status: **M0 landed**; M1-M6 pending (Ch 17).
This document SUPERSEDES the earlier short `MP_PROPOSAL.md` and
`bugs/008-multiplayer-coop.md` (both ~90% duplicates of each other;
`bugs/008-multiplayer-coop.md` is DELETED as of the M0 commit). It is written to a
different
standard than those drafts: an implementing agent with NO prior context should be able to
work top-to-bottom from this document without inventing anything. Where something could
NOT be verified in the code, it is marked loudly rather than papered over.

Read alongside (authoritative substrate, all in repo root unless noted):
`AGENTS/mp-actor-architecture-principle.md` (the load-bearing invariant), `MP_PROTOCOL.md`
(wire), `CLIENT_JOIN_DESIGN.md` (join/rebaseline), `SERVER_LOOP_DESIGN.md` (beat loop),
`COMBAT_CLIENT_DESIGN.md` (combat presentation), `DIALOG_STREAMING_PLAN.md` (dialog
driver/spectator), `BARTER_STREAMING_PLAN.md`, `TEMPLE_DEMO_ROADMAP.md` (north star),
`SCRIPT_OPCODE_MAP.md` (the 181-opcode spine used by Chapter 10), `IDEAS.md`
(vision constraints), `AGENTS/p5-server-plan.md` (open bugs).

## Verification legend (used throughout)

- **[V file:line]** — VERIFIED by reading the source at that location on 2026-07-19.
  Line numbers are against branch `rewrite/phase0` at commit `2cde848` (+ uncommitted
  working-tree edits present that day); re-grep the cited symbol if lines have drifted.
- **[U]** — UNVERIFIED: stated from design docs/memory, not re-proven in source. Verify
  before building on it.
- **[OPEN-Q #n]** — a genuine decision the owner must make; collected in Chapter 19.
- House rule: the gate is `scripts/check.sh` → must print "ALL GATES PASS". NEVER run
  golden harnesses concurrently or while a live `viewer_live.sh` server is up
  (`pkill -x f2_server; pkill -x fallout2-ce` first).

--------------------------------------------------------------------------------
## Table of contents

- **Ch 1 — Goal, scope, non-goals**
- **Ch 2 — Architecture inventory: what exists today (verified shapes)**
- **Ch 3 — The core design: the Player-Actor Registry (generalize "the dude is special" to a set)**
- **Ch 4 — Spawning additional player actors**
- **Ch 5 — Identity on the wire: netIds, join blob, roster, per-viewer binding**
- **Ch 6 — Session lifecycle: connect, claim, disconnect, reconnect, spectator**
- **Ch 7 — The verb layer: per-session actor binding**
- **Ch 8 — Combat with N player actors**
- **Ch 9 — Player death and server survival (CRITICAL: the server never dies because a player did)**
- **Ch 10 — The passive-sim boundary and `dude_obj` (the hardest problem; owner-flagged)**
- **Ch 11 — Dialog under N players (driver + spectators)**
- **Ch 12 — Barter ruling**
- **Ch 13 — Out-of-combat parity per player (movement, interaction latch, inventory, loot)**
- **Ch 14 — Coupled map transitions and elevation**
- **Ch 15 — XP, loot, and progression attribution ruling**
- **Ch 16 — Save/load ruling**
- **Ch 17 — Staging plan (M0…M6) with per-stage file changes, gates, rollback**
- **Ch 18 — Consolidated acceptance criteria (Demo 0.2)**
- **Ch 19 — Open-questions register**
- **Ch 20 — Risk register**
- **Appendix A — gDude reference census (per-file counts + classification)**
- **Appendix B — Server-terminal site census (every way the server can die)**
- **Appendix C — Opcode classification tables for Chapter 10**

--------------------------------------------------------------------------------
# Ch 1 — Goal, scope, non-goals

**Goal.** N independently-controlled players (design for N; never hardcode 2) on the SAME
map served by the dedicated server (`f2_server`), each with full feature parity:
free-roam movement, inventory/equip/drop, skills, out-of-combat interaction (use / get /
look / loot / use-item-on), combat (attack / move / end-turn / end-combat, own turns),
use-item, and dialog spectating (one driver, N−1 read-only spectators). The Temple of
Trials passable together.

**⚠ What "full feature parity" does and does NOT mean — read this before Ch 2.** Every
player gets full parity of *actions*: each drives its own body, inventory, turns, and
verbs, independently. What players do NOT get in v1 is separate *characters*. The
limitations are each derived and justified in place (Ch 4.3, Ch 10.7, Ch 15), but they
are scattered across four chapters and a reader who stops after this one will form the
wrong picture, so they are collected here once:

| Shared, not per-player | Consequence | Where |
|---|---|---|
| One `gDudeProto` (all actors share pid `0x1000000`) | identical stats/skills for everyone; every actor reads back with the HOST'S NAME; a script `set_critter_stat` on any player mutates all | Ch 4.3 |
| One PC sheet (`pcAddExperience` has no critter arg) | all XP pools onto the host; extras never level; P2's skill successes train P1 | Ch 15 |
| `dude_obj` in scope-less procs | until O3 lands, passive NPC scripts notice only the anchor player: aggro, floaters, notice-checks | Ch 10.3/10.4 |
| Sneak state, PC skill points, kill counts | host-only | Ch 10.6 |

So Demo 0.2 co-op is honestly described as **N bodies driving one character sheet**, not
N characters. That is a legitimate demo and a deliberate v1 cut — per-actor progression
needs per-actor PC-data blocks, and per-actor identity needs per-context vars
(`gvar_set(ctx,X,v)`, IDEAS.md §2) — but it should be said once, here, rather than
discovered in Ch 15.

**Server-authoritative.** The server runs the one sim, all dice, all validation. Clients
request through claim-gated verbs; the authoritative result rides the S2C stream back.
Nothing in this spec ever moves authority client-side.

**The load-bearing architectural change** is de-singletoning `gDude` into a per-session
actor binding (`sessionId → actor`), per `AGENTS/mp-actor-architecture-principle.md`:
`gDude` is a per-client ROLE ("the actor THIS client controls"); on the server it
degrades to "the HOST-slot actor" plus a scoped binding during verb execution. This is
Chapter 3, and everything else hangs off it. It is treated as the core of the work, not a
trick.

**Non-goals (out of scope for Demo 0.2), each with the reason:**

| Out of scope | Why |
|---|---|
| Separate maps per player | The true engine-singleton wall (one `gMap`, one object list, one combat ctx). Vanilla-inherent; do not contort v1 (`IDEAS.md` §1). |
| Two concurrent combats (even on two elevations of one map) | Single global combat context (`gCombatState`, `_combat_list`, `_combat_elev`, `gCombatSession`). Bounded refactor, banked (`TEMPLE_DEMO_ROADMAP.md` "Elevation architecture"). |
| Character creation / per-player builds | Premade actors only (owner requirement). |
| Player-death POLICY (respawn/permadeath/spectate rules) | Explicitly DEFERRED by the owner. This spec builds the SEAM (`playerActorDied`, Ch 9) with a documented no-op default so policy is a one-function change later. |
| Per-player XP/level progression | PC progression state is a global singleton (Ch 15). v1: the one PC sheet absorbs all XP. Documented, not hidden. |
| Barter driver/spectator | Separate track (`BARTER_STREAMING_PLAN.md`); ruling in Ch 12. |
| Save/load of an N-player session | Ruling in Ch 16 (out of scope; the join-blob format already carries the shape a future save needs). |
| Player-to-player direct trade UI | Not in vanilla between two PCs; v1 workaround = drop/pick up on the ground (the loot layer already serializes shared access). |
| gvar/lvar per-player namespacing (`gvar_set(ctx,X,v)`) | Phase-4+ (`IDEAS.md` §2). Ch 10 depends on NOT having it and says what that costs. |

--------------------------------------------------------------------------------
# Ch 2 — Architecture inventory: what exists today (verified shapes)

This chapter is the "current code shape" baseline every later chapter diffs against. All
items verified 2026-07-19 unless marked.

## 2.1 Processes and binaries

- `f2_server` — core-only dedicated server (`src/server_main.cc` [V], `src/server_boot.cc`
  [V]). Boots the sim (`serverBoot`: subsystem init + `_proto_dude_init("premade\\combat.gcd")`
  + map load [V server_boot.cc:237-272]), then `serverServe` (beat loop).
- `fallout2-ce` with `F2_CLIENT_CONNECT=host:port` — the SDL wire viewer
  (`mainClientViewer`, [V src/main.cc:939]). Puppet client: scripts disabled, no local sim
  authority, renders the mirror, sends verbs.
- Debug channel: `F2_SERVER_CMD=<port>` → unrestricted `commandDispatch` (not the trust
  boundary). Viewer wire lines → `serverControlLine` (the trust boundary)
  [V server_main.cc:157-173, 261-269].

## 2.2 The beat loop

`serverTick` [V src/server_loop.cc:262-368], in order: `intentsDrain` (socket accept →
rebaseline request; control-line drain; scripted actions; `serverAnimAdvanceWalks`;
`serverControlAdvancePending`) → `simClockAdvance(100)` → `_process_bk()` (tickers:
`_doBkProcesses` = script AI + timed events) → `scriptsHandleRequests()` → cstart
combat-entry latch [V :292-297] → `combatSessionAdvance()` if active [V :304-306] →
`mapHandleTransition()` → `objectDeltaScan()` → `invariantsCheck` → baseline re-emit on
map-generation change [V :325-341] → mid-combat-rebaseline re-assert [V :357-363] →
`presenter()->beatEnd(tick)`.

The serve loop's continue-predicate stops on `gameTerminalQuitRequested()`
[V server_main.cc:302-310] — the exact seam Chapter 9 protects — and when all viewers
disconnect [V :314-316].

## 2.3 The wire

Binary S2C stream (`src/presenter_network.cc`): frame = beat; events
[V presenter_network.cc:59-131]: SPAWN=1, MOVE=2, DESTROY=3, CONNECT=4, DISCONNECT=5,
OBJECT_DELTA=6, WORLD_DELTA=7, SNAPSHOT_OBJECT=8, SNAPSHOT_BEGIN/END=9/10,
MAP_TRANSITION=11, COMBAT_ENTER/EXIT=12/13, TURN_START=14, ATTACK_RESULT=15,
CONSOLE..MUSIC_STOP=16-23, SNAPSHOT_BLOB_BEGIN/CHUNK/END=24/25/26, WEAPON_TAKE_OUT=27,
DOOR_STATE=28, ACTION_ANIM=29, EXPLOSION_FX=30, PRES_SEQ=31, DIALOG_NODE=32,
DIALOG_END=33. **Next free event id = 34.** Events are length-prefixed
(skip-unknown-type safe), so appending fields to existing events is compatible in both
directions.

There is **ONE encoder and ONE broadcast byte stream — NO per-client S2C framing**
[V server_net.cc:203-229 `SocketByteSink::write` broadcasts the same buffer to every
client]. The only per-client bytes ever sent are the 6-byte accept preamble
`"F2NS" + LE u16 version=1` [V server_net.cc:171-180]. Chapter 5 exploits exactly this
seam for per-session identity.

C2S is newline-delimited text `verb arg arg2 arg3` on the same socket, tagged with a
stable, never-reused `sessionId` minted at accept (`_nextSessionId++`, starts at 1; 0 is
the debug-channel sentinel) [V server_net.cc:134,182; server_net.h:113-127].

## 2.4 Join / rebaseline

- Join blob = save pipeline: `mapSaveToStream(stream, &mapBodyLen)` writes the map body
  (`_map_save_file`) **then appends the dude via `_obj_save_dude`** (the dude is
  `OBJECT_NO_SAVE`, so `objectSaveAll` skips him) [V map.cc:1301-1318]. Emitted by
  `serverEmitJoinBlob` [V server_loop.cc:148-205]; `SNAPSHOT_BLOB_BEGIN` carries
  `mapIndex, elevation, dudeNetId, gameTime, mapSaveVersion, mapBlobLen, dudeBlobLen,
  crc32` [V server_loop.cc:191-194, client_net.cc:592-608].
- netId alignment = **rebaseline-at-join**: both sides run the same deterministic
  `objectAssignAllNetIds()` walk — dude first (netId 1) + his inventory, then the tile
  walk skipping `OBJECT_NO_SAVE` (skipped objects get netId zeroed)
  [V object.cc:4548-4584]. Any rebaseline broadcasts blob + baseline to ALL clients
  (`serverRequestRebaseline` consumed at beat tail [V server_loop.cc:334-341]).
- Viewer apply: `applyBlob` [V client_net.cc:631-736] — `presReset` →
  `gameTimeSetTime(blobGameTime)` → viewer-mode `mapLoad(stream)` → `_obj_load_dude(rd)`
  (memcpy's the blob dude INTO the local `gDude` object [V object.cc:3393-3450]) →
  `_map_fix_critter_combat_data` → `objectAssignAllNetIds` → `scriptsDisable` →
  `seedNetMap` → dudeNetId cross-check [V client_net.cc:681-684].

## 2.5 Control plane / claims (the single-player shape being replaced)

`src/server_control.cc`: ONE global claimant `static int gClaimant` [V :48]. `claim`
grants to the first asker [V :415-424]; every verb handler gates on
`sessionId != gClaimant` and acts on **`gDude` directly** (47 references in this file).
`serverControlBeginDrain(liveSession)` releases the claim when its session died and kills
the (single, global) pending walk-then-act latch `gPending`/`gPendingActive`
[V :91-110, 385-397]. It says nothing about the actor — there is only one, `gDude`.
Verb families [V :399-1097]: `claim`, `mv`, `cancel`, `dsay`/`dend`,
interaction (`use usedoor get look push rot skill talk loot useitemon`), `cstart`,
inventory (`invwield invunwield invdrop useitem useitem_armexplosive`),
loot (`take put takeall`), combat (`cattack cmove cendturn cendcombat`).

## 2.6 Combat (the resumable session machine)

- `_combat()` under `F2_SERVER_RESUMABLE_COMBAT` sets up `gCombatSession` and returns;
  `combatSessionAdvance` runs one beat at a time [V combat.cc:3938-3960, 3854-3936].
- **The player-actor list is already list-shaped with one entry**:
  `combatSessionIsPlayerActor(obj)` iterates `Object* players[] = { gDude }`
  [V combat.cc:3549-3562]. This is the designed extension point (locked decision #6:
  players = one contiguous chain; never "exactly one player turn").
- Turn order: `_combat_sequence_init` places attacker first, defender second, **dude
  third** [V combat.cc:2938-2993]; per-round `_combat_sequence` re-sorts by Sequence stat
  [V :2998-3047].
- Player barrier: `combatSessionAdvancePlayerTurn` [V :3709-3779] — BEGIN once
  (`combatSessionPlayerTurnBegin`, a faithful copy of `_combat_turn`'s dude branch
  [V :3628-3694]), then per-beat `combatServerPumpIntents()`
  [V combat_drain.cc:170-279], waiting while `combatSessionShouldWait()`
  (= `serverClaimantConnected() || combatIntentPending() || F2_SERVER_TURN_WAIT`)
  [V combat.cc:3568-3573] with a sim-ms idle budget.
- Turn-end authorities: `combatPlayerTurnShouldBreak` (reads `gDude` results +
  `_game_user_wants_to_quit != 0`!) [V :3099-3118], `combatPlayerTurnOutOfAp` (gDude AP)
  [V :3121-3124], `combatPlayerTurnResolve` (consumes the 0x08 end-combat handshake,
  clears quit==1) [V :3129-3148].
- Combat-end conditions: `_combat_should_end` — roster ≤1, **or gDude not in roster**,
  or no non-dude-team hostiles left [V :3289-3325]. `_combat_turn` returns −1 (ends
  combat) **whenever `gDude` is DAM_DEAD** [V :3276-3278] and when the acting dude
  changed elevation [V :3280-3285].
- Intents: `CombatIntent {kind, arg, hitLocation, hitMode, run}` — **no actor field**
  [V src/combat_intent.h]. The pump acts on `gDude` explicitly
  [V combat_drain.cc:204-240].
- TURN_START carries `(netId, isPlayer, ap, deadlineMs)`; viewer myTurn is already keyed
  `isPlayer && netId == gDude->netId` — never isPlayer alone
  [V client_net.cc:1494-1507].

## 2.7 Dialog (A3 state, working tree)

Server drives the real conversation via the block-and-pump barrier; each node is emitted
as `dialogNode(speaker, driver=gDude, …)` [V game_dialog.cc:1957] and `dialogEnd(gDude)`
[V :2251-2252]; the pump installs from `server_main.cc` behind `F2_DIALOG_STREAM`
[V server_main.cc:193-237]. Viewer renders the node (`src/client_dialog.cc`); editability
is gated `gDude->netId == driverNetId` [V client_dialog.h:8-12, client_dialog.cc:103].
`dsay`/`dend` are claimant-gated + `_gdialogActive()`-gated in `serverControlLine`
[V server_control.cc:490-514]. Dialog entry: `talk` verb → walk-then-act latch →
`scriptsRequestDialog(target)` (stores ONLY the speaker; **no actor**)
[V server_control.cc:172-177, scripts.cc:1223-1226] → `scriptsHandleRequests` →
`ServerScriptRequestHandler::dialogEnter` → `gameDialogEnter(speaker, 0)`
[V script_request_handler_server.cc:22, game_dialog.cc:693].

## 2.8 The dude object itself (what "gDude" physically is)

Created once at `objectsInit`: `objectCreateWithFidPid(&gDude, dudeFid, 0x1000000)` with
flags `OBJECT_NO_REMOVE | OBJECT_NO_SAVE | OBJECT_HIDDEN | OBJECT_LIGHT_THRU`, light set,
and **`partyMemberAdd(gDude)`** [V object.cc:312-324]. Pid `0x1000000` resolves to the
one mutable `gDudeProto` [V proto.cc:90, 2133-2137]; `_proto_dude_init(path)` loads a
premade `.gcd` into it [V proto.cc:885, server_boot.cc:246-251]. `protoGetName(0x1000000)`
→ `critterGetName(gDude)` → the global `gDudeName` [V proto.cc:356-362,
critter.cc:236-267]. Map teardown spares him (`OBJECT_NO_REMOVE`), map load re-places him
and re-asserts `OBJECT_NO_SAVE` [V map.cc:1492-1506]. His script is bound via
`scriptsSetDudeScript` [V scripts.cc:1467-1510].

The "special-dude predicate" recurs at these load-bearing seams — this list IS the
generalization surface for Chapter 3:

| Seam | Site | Shape today |
|---|---|---|
| Delta sync domain | `objectIsSyncable` [V object_delta.cc:129-132] | `(flags & NO_SAVE)==0 \|\| obj == gDude` |
| netId walk | `objectAssignAllNetIds` [V object.cc:4548-4584] | dude numbered first, then non-NO_SAVE walk |
| Baseline snapshot | `serverEmitBaseline` [V server_loop.cc:224-231] | `snapshotObject(gDude)` + non-NO_SAVE walk |
| Join blob | `mapSaveToStream` [V map.cc:1301-1318] | map body + `_obj_save_dude` appendix |
| Blob header | `snapshotBlobBegin(dudeNetId…)` [V server_loop.cc:191-194] | one dudeNetId |
| Combat player list | `combatSessionIsPlayerActor` [V combat.cc:3549-3562] | `{ gDude }` |
| Combat end/turn rules | `_combat_should_end` :3297, `_combat_turn` :3276/:3280, `combatPlayerTurnShouldBreak/OutOfAp` :3105/:3123 | gDude hardwired |
| Death→endgame | `critterKill` [V critter.cc:912-915] | `critter == gDude` → quit=2 |
| Verb executor binding | `serverControlArmInteraction` etc. [V server_control.cc:277] | `Object* actor = gDude` |
| Dialog driver stamp | [V game_dialog.cc:1957] | `dialogNode(speaker, gDude, …)` |
| Walk-registry liveness | `serverWalkOwnerAlive` [V server_anim.cc:275-286] | `owner == gDude` fast-true |
| Presentation-cost tally | [V server_anim.cc:405-415] | `owner != gDude` = "AI mover" |
| State dump | `state_dump.cc` (16 refs, [U] detail) | dumps the dude explicitly |

--------------------------------------------------------------------------------
# Ch 3 — The core design: the Player-Actor Registry

## 3.1 The idea

Every seam in the §2.8 table asks the same question — "is this object THE player?" — and
answers it with pointer-equality against one global. The whole co-op change is: **replace
the predicate `obj == gDude` with membership in a small ordered registry of player
actors, of which the HOST slot (index 0) is `gDude`.** Nothing about the dude's special
mechanics (NO_SAVE lifecycle, blob appendix, explicit numbering, camera-role on the
client) is discarded; each is applied to N objects instead of 1.

Default state of the registry = `{ gDude }`. Every current code path is then IDENTICAL by
construction (the loops degenerate to today's single checks) — this is the
golden-safety argument for the whole track, and it must be true at every stage boundary.

## 3.2 New TU: `src/server_players.{h,cc}` (f2_core)

It must live in **f2_core**, not f2_server, because core TUs consult it every beat
(combat.cc, object_delta.cc, object.cc, critter.cc, server_loop.cc). Same precedent as
`combat_intent.{h,cc}` (data-only core TU) and the claim-query bridge
[V server_loop.cc:57-69].

```c
// server_players.h — the player-actor registry (mp-actor-architecture-principle).
// Slot 0 is ALWAYS gDude (the host actor). Slots are stable for the process
// lifetime; actors never leave the registry (death/disconnect change BINDING,
// not membership). Registry mutation happens only at boot (spawn) — the beat
// loop never adds/removes slots.
#ifndef FALLOUT_SERVER_PLAYERS_H_
#define FALLOUT_SERVER_PLAYERS_H_

namespace fallout {

struct Object;

constexpr int kMaxPlayerActors = 8; // arbitrary sane cap; N is env-driven

// Number of player actors (1 = today's behavior in every respect).
int playerActorCount();

// The actor at slot i (0 <= i < playerActorCount()). Slot 0 == gDude.
Object* playerActorAt(int slot);

// TRUE iff obj is a player actor. THE generalized "obj == gDude" predicate.
// O(N) over <= kMaxPlayerActors — cheap enough for per-beat/per-event use.
bool playerActorIs(Object* obj);

// Slot for obj, or -1. (Registry is keyed by Object*; player actors are
// NO_SAVE + NO_REMOVE like the dude, so the pointers are process-stable —
// see Ch 4/Ch 14. If that lifetime guarantee is ever weakened, re-key on
// id+pid snapshots like ServerWalk does.)
int playerActorSlotOf(Object* obj);

// Boot-time registration (server_boot.cc only). Slot 0 auto-registers gDude.
void playerActorRegister(Object* actor);

// True iff at least one player actor is alive (not DAM_DEAD).
bool playerActorAnyAlive();

// ---- The player-death POLICY SEAM (Ch 9). Default body: no-op. ----
void playerActorDied(Object* actor);

} // namespace fallout
#endif
```

`playerActorRegister` is called from `serverBoot` after `_proto_dude_init` (slot 0) and
after each extra spawn (Ch 4). On every non-server path (client, probe, goldens) nothing
registers extras → count stays 1 → all generalized loops degenerate to the old code.

**Registry lifetime rule (load-bearing):** membership is fixed at boot. A disconnect, a
death, or a rebaseline never removes a slot — they change the *binding* (Ch 6) or the
actor's *state*, never its identity. This keeps every consumer free of add/remove races
and keeps slot indices meaningful in the roster event (Ch 5.4).

## 3.3 What "gDude" MEANS after this change

- **On the server:** `gDude` remains a real global pointing at the SLOT-0 (host) actor
  forever. It is additionally *scope-swapped* to the acting session's actor during verb
  execution (Ch 7) and during a player-context script execution if Option O2 of Ch 10 is
  adopted. Outside those scopes, all passive-sim reads of `gDude` see the host actor —
  Chapter 10 is the honest analysis of what that means.
- **On each viewer:** `gDude` = "the actor I control" — repointed at join to the actor
  the roster assigns this session (Ch 5.6). The same server object is `gDude` on exactly
  one viewer and a plain remote critter everywhere else.
- **In goldens/probe/single-player:** unchanged in every observable way.

## 3.4 Alternatives considered (and why rejected)

1. **Pure parameter-threading (no gDude swap, no registry; pass `actor` everywhere).**
   Correct long-term, but the callee tree below the verb layer reads `gDude` internally
   at dozens of sites (e.g. `skill.cc` PC-vs-NPC branches ×18 [V grep], `proto_instance.cc`
   ×44, item use paths). Threading a parameter through all of them is the post-v1 rename-
   class refactor, not a demo deliverable. The registry + scoped swap gets per-actor
   CORRECTNESS at the boundaries that matter now without rewriting the tree.
2. **Make P2 a party member.** Violates the locked principle (party = removable behavior,
   never identity; leader-owned, 5-cap, leader-mediated — all the things co-op must not
   inherit). Rejected permanently.
3. **N full engine instances / per-player worlds.** The singleton wall; out of scope by
   definition.

--------------------------------------------------------------------------------
# Ch 4 — Spawning additional player actors

## 4.1 Requirements

- `F2_SERVER_PLAYERS=N` (default 1). Server boots with N controllable actors; N−1 extras
  spawned near the host's entry tile. N=1 must be bit-for-bit today's behavior.
- Premade parity: extras play like the host's premade (combat.gcd = the "combat" premade
  build loaded by `_proto_dude_init` [V server_boot.cc:246-251]).
- Extras must: (a) be visible/addressable on the wire, (b) survive map transitions,
  (c) not duplicate into map `.SAV`s, (d) be valid combat/AI targets, (e) carry their own
  inventory.

## 4.2 The lifecycle-class decision (the deep fork; DECIDED)

Two possible object classes for extras:

- **Option A — "dude-like" actors: `OBJECT_NO_SAVE | OBJECT_NO_REMOVE`, explicitly
  carried.** Same class as gDude: excluded from `objectSaveAll` (no `.SAV` duplication),
  spared by map teardown (`_obj_remove_all` respects NO_REMOVE — the dude demonstrably
  survives every transition [V object.cc:315, map.cc:1492-1506]), therefore
  **process-stable pointers** (what the registry keys on). Cost: every "…except the dude"
  seam (§2.8 table) must except them too: netId walk, syncable predicate, baseline,
  blob appendix.
- **Option B — plain world critters (no NO_SAVE).** Free ride on the existing wire
  (objectSaveAll/netId walk/deltas cover them with zero seam edits). FATAL flaws:
  (1) map transition frees them with the world (`mapLoadById` → teardown) — they would
  need bespoke save/restore juggling across every transition; (2) the engine SAVES the
  outgoing map on exit in normal play, so their bodies would persist INTO the map `.SAV`
  and duplicate on re-entry (the exact problem NO_SAVE exists to prevent for the dude);
  (3) pointers die on every transition, poisoning the registry.

**DECISION: Option A.** It is the N-actor generalization of the proven dude mechanism;
every edit it needs lands on a seam already enumerated in §2.8, and Chapter 14
(transitions) becomes "run the dude's placement loop over the registry" instead of a
serialization subsystem.

## 4.3 Spawn mechanism (exact)

Use `_obj_copy(&extra, gDude)` [V object.cc:866-945]. What it verifiably does: allocates
+ memcpy's the object, inserts it, mints a fresh `obj->id` (`scriptsNewObjectId`),
assigns a wire netId when `serverLoopActive()` [V :896-898], **re-creates the sid from
the proto** (`_obj_new_sid`) [V :902-905], deep-copies the whole inventory (children emit
`objectCreated` before the parent) [V :915-944], clears `OBJECT_QUEUED`.

Required fix-ups after `_obj_copy` (each justified):

```c
// server_boot.cc — after serverLoadMap() succeeds, before scriptsEnable():
static int serverSpawnExtraActors()
{
    int want = 1;
    if (const char* v = getenv("F2_SERVER_PLAYERS")) want = atoi(v);
    if (want < 1) want = 1;
    if (want > kMaxPlayerActors) want = kMaxPlayerActors;

    playerActorRegister(gDude); // slot 0 — do this even when want == 1

    for (int slot = 1; slot < want; slot++) {
        Object* extra = nullptr;
        if (_obj_copy(&extra, gDude) == -1 || extra == nullptr) return -1;

        // (1) The copy inherited gDude's flags via memcpy — including HIDDEN
        //     cleared already by serverLoadMap (objectShow) — but be explicit:
        //     dude-class lifecycle ON, hidden OFF.
        extra->flags |= (OBJECT_NO_SAVE | OBJECT_NO_REMOVE);
        extra->flags &= ~OBJECT_HIDDEN;

        // (2) _obj_copy minted a fresh sid from the proto. The dude proto's
        //     script slot is the PC script bound via scriptsSetDudeScript —
        //     a second live instance of it is an untested aliasing hazard.
        //     Extras are scriptless (like most critters with sid == -1).
        if (extra->sid != -1) { scriptRemove(extra->sid); extra->sid = -1; }

        // (3) Place adjacent to the host: first unblocked tile in a growing
        //     ring (same idiom as vanilla placement fallbacks).
        int tile = serverFindFreeTileNear(gDude->tile, gDude->elevation);
        if (tile == -1) return -1;
        objectSetLocation(extra, tile, gDude->elevation, nullptr);
        objectSetRotation(extra, gDude->rotation, nullptr);

        playerActorRegister(extra);
    }
    return 0;
}
```

`serverFindFreeTileNear`: loop `dist = 1..6`, `rot = 0..5`,
`t = tileGetTileInDirection(center, rot, dist)`, first `t` with
`_obj_blocking_at(nullptr, t, elev) == nullptr` wins; −1 if none. (Same primitives the
AI flee path uses [V combat_ai.cc:1192-1206].)

**Consequences of sharing pid `0x1000000` (documented, accepted for the demo):**

- Stats/skills read the shared `gDudeProto` → identical premade build for every actor
  (that is the requirement). A script `set_critter_stat` on ANY player mutates the shared
  proto → affects all players. Rare; accepted. [V proto.cc:2133-2137]
- `critterGetName(extra)` falls through `scriptIndex==-1`/`sid==-1` to
  `protoGetName(0x1000000)` → `gDudeName` → every player shows the host's name
  [V critter.cc:236-267, proto.cc:356-362]. Cosmetic; per-actor names ride the roster
  event later (Ch 5.4 reserves the field).
- The PC-vs-NPC branches (`critter == gDude` pointer tests) treat extras as NPCs: proto
  skills, no PC skill points, no sneak state, no perk bonuses beyond proto. For premade
  parity this is approximately right; the measurable gaps are enumerated in Ch 10.6
  (poison/rad immunity is the serious one) — **[OPEN-Q #3]** whether to relax
  the poison/radiation dude-gates for player actors in v1.
- Scripts testing `obj_pid(x) == 16777216` (the dude pid) will match extras — for
  co-op content that is usually the desired reading. [U — no such test enumerated in
  shipped scripts; noted as behavior, not verified frequency.]

**Rejected spawn alternatives:** fresh proto per player (needs new .pro assets + a
proto-streaming story — post-v1); `objectCreateWithPid(0x1000000)` (no inventory copy —
would need a manual kit; `_obj_copy` gives Narg's kit for free); reusing
`premadeCharacters`/gcd loaders for extras (they write PC globals — single-slot).

## 4.4 What spawning must NOT do

- Do NOT `partyMemberAdd(extra)` (mp-actor principle; also party membership changes AI
  disposition paths [V combat_ai.cc:1551]).
- Do NOT give extras the dude script or any script.
- Do NOT spawn before the map load (serverLoadMap references gDude and the map places
  him [V server_boot.cc:221-235]); spawn AFTER `serverLoadMap`, BEFORE `scriptsEnable()`
  so map-enter scripts have already run and cannot observe a half-built registry…
  **CORRECTION [V server_boot.cc:256-268]: `scriptsEnable()` runs after `serverLoadMap`,
  and map-enter procs run INSIDE mapLoad regardless** — so extras spawn after map-enter
  procs have run either way. Map-enter scripts therefore never see extras on first load;
  acceptable (they don't see the dude's final position either), noted for Ch 10.

--------------------------------------------------------------------------------
# Ch 5 — Identity on the wire: netIds, join blob, roster, per-viewer binding

This chapter makes the N actors (a) addressable by every existing wire event, (b) present
in the join blob, and (c) bindable — each viewer learns WHICH actor is its own. Point (c)
is the only genuinely new wire machinery in the whole spec.

## 5.1 netId walk extension

Current shape [V object.cc:4548-4584]: dude first (netId 1) + his inventory recursively,
then the tile walk numbering non-NO_SAVE objects and zeroing skipped ones.

Target shape — number ALL registry actors first, in slot order:

```c
void objectAssignAllNetIds()
{
    objectSetNextNetId(1);
    for (int slot = 0; slot < playerActorCount(); slot++) {   // slot 0 == gDude
        Object* actor = playerActorAt(slot);
        actor->netId = objectNextNetId();
        objectAssignInventoryNetIds(actor);
    }
    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (!playerActorIs(obj) && (obj->flags & OBJECT_NO_SAVE) == 0) {
            obj->netId = objectNextNetId();
            objectAssignInventoryNetIds(obj);
        } else if (!playerActorIs(obj)) {
            obj->netId = 0;
            objectZeroInventoryNetIds(obj);
        }
        obj = objectFindNext();
    }
}
```

Domain-alignment invariant (CLIENT_JOIN_DESIGN §C) is preserved because the CLIENT
reproduces the identical walk — which requires the client to know the actors and their
order at walk time. That is why the actors ride the blob as an ORDERED appendix (§5.3):
the client registers them in appendix order into its own (client-side) registry before
running the walk. **Ordering rule: registry slot order is the wire order everywhere.**

⚠ Note actor netIds are STABLE ONLY within one baseline generation, like every netId
(re-minted on every rebaseline — [V AGENTS/p5-server-plan.md "netId RE-MINT"]). Nothing
may persist an actor netId across a rebaseline; persistent identity = registry slot.
The roster event (§5.4) re-announces slot→netId after every baseline for exactly this
reason. (With the §5.1 walk, actor slot k deterministically gets netId… NOT k+1 — actor
inventories are numbered in between [V object.cc:4526-4533]. Do not assume netId==slot+1;
always read the roster.)

## 5.2 Syncable-domain extension

`objectIsSyncable` [V object_delta.cc:129-132]:

```c
// was: return (obj->flags & OBJECT_NO_SAVE) == 0 || obj == gDude;
return (obj->flags & OBJECT_NO_SAVE) == 0 || playerActorIs(obj);
```

And the baseline walk `serverEmitBaseline` [V server_loop.cc:224-231]:

```c
presenter()->snapshotBegin(mapGetCurrentMap(), gElevation);
for (int slot = 0; slot < playerActorCount(); slot++)
    presenter()->snapshotObject(playerActorAt(slot));
for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext())
    if ((obj->flags & OBJECT_NO_SAVE) == 0 && !playerActorIs(obj))   // (guard is new)
        presenter()->snapshotObject(obj);
presenter()->snapshotEnd();
```

(The `!playerActorIs` guard in the tile walk is belt-and-braces, not load-bearing: player
actors are NO_SAVE (Ch 4.2 Option A), so the existing flag test already skips them — the
same way it skips the dude today [V server_loop.cc:227]. Keep the guard anyway so the
"actors are emitted exactly once, from the slot loop" invariant is stated in code rather
than inferred from a flag.)

`state_dump.cc` mirrors the same set (it dumps the dude explicitly [U detail]); extend it
identically so the replay/netstream gates keep their oracle symmetry. **Gate impact:**
with `F2_SERVER_PLAYERS` unset the registry is `{gDude}` and every emitted byte is
identical — run `scripts/check.sh` to prove it at each stage.

## 5.3 Join blob: N-actor appendix

Current: map body + `_obj_save_dude` [V map.cc:1301-1318]; header fields
`(mapIndex, elevation, dudeNetId, gameTime, mapSaveVersion, mapBlobLen, dudeBlobLen, crc32)`
[V server_loop.cc:191-194].

Target:

- `mapSaveToStream` appends, after `_obj_save_dude`, each EXTRA actor in slot order via
  the same idiom `_obj_save_dude` uses internally (`_obj_save_obj` with the NO_SAVE flag
  dance [V object.cc:3371-3389]). New helper in object.cc:

  ```c
  // Append a player actor to a blob stream exactly as _obj_save_dude does for
  // the dude (temporarily clear NO_SAVE so _obj_save_obj serializes it; the
  // trailing gCenterTile word is dude-only and NOT written for extras).
  int _obj_save_player_actor(File* stream, Object* actor);
  ```

- `SNAPSHOT_BLOB_BEGIN` payload: **append `u16 actorCount`** (the TOTAL number of
  actors, so 1 = every pre-co-op blob). Length-prefixed events make the append
  backward/forward-safe: old viewers skip the extra bytes, new viewers read a short
  payload as actorCount=1. crc32 covers the whole byte run as today
  [V server_loop.cc:129-140, 191-194 for the emit shape to extend].

  ⚠ **AS BUILT, deviating from this chapter's original design:** the per-extra
  `u32 actorBlobLen` fields are NOT emitted, and `dudeBlobLen` now bounds the WHOLE
  appendix (dude + extras) rather than the slot-0 section alone. Reason: each section is
  one self-delimiting `_obj_save_obj` tree, exactly like the dude's, so nothing ever
  reads a per-section length — they would be a second, unverified description of the
  same bytes, free to drift out of agreement with them and lie. The total-length guard
  and the crc32 both still cover every byte, and `actorCount` tells the reader how many
  trees to consume.
- ⚠ **`OBJECT_NO_REMOVE` MUST BE STRIPPED on the viewer's load.** `_obj_save_obj` writes
  the flag word verbatim, so the server's dude-class flags ride the blob — and a
  viewer-side actor that keeps NO_REMOVE survives `mapLoad`'s teardown and is then
  loaded AGAIN from the next blob. That leaks one permanent duplicate actor per
  rebaseline, standing in the world forever, and nothing in the netId walk or the
  tripwire counts would flag it (the duplicate is NO_SAVE, so it takes no netId slot and
  appears in no baseline). Re-assert NO_SAVE at the same time, so both sides classify
  actors identically for `objectIsSyncable` and the walk.
- Client `applyBlob` [V client_net.cc:631-736], after `_obj_load_dude(rd)`: for each
  extra section, `_obj_load_obj(rd, &actor, gElevation??, …)` — **use the same
  `_obj_load_obj` call shape `_obj_load_dude` uses ([V object.cc:3402-3403]:
  `_obj_load_obj(stream, &temp, -1, nullptr)`), then `objectSetLocation` it at its
  serialized tile** and register it into the CLIENT-side registry (slot order). Then
  `objectAssignAllNetIds()` reproduces the server walk (which consults the same registry
  order), then `seedNetMap()`.
- The client registry re-seeds from the blob on EVERY apply (rebaseline reload frees the
  old extra objects with the world — extras on the CLIENT are ordinary loaded objects,
  not NO_REMOVE; only the special local gDude object survives mapLoad
  [V object.cc:312-324 vs blob-loaded objects]). `presReset`/`seedNetMap` already handle
  the analogous invalidation for `_net` [V client_net.cc:653, 738-750].

⚠ **Sequencing hazard (must-follow):** on the CLIENT, `gDude` may have been repointed to
a non-slot-0 actor by §5.6. `_obj_load_dude` memcpy's INTO `*gDude`
[V object.cc:3405] — running it while `gDude` aims at a freed/foreign object corrupts
memory. **applyBlob MUST restore `gDude = <the process's original dude object>` BEFORE
`mapLoad`, and re-run the §5.6 rebind AFTER `seedNetMap`.** Keep the original pointer in
a file-static `gClientHostDude` captured at first connect (it is the `objectsInit`-made
NO_REMOVE object and survives every mapLoad [V object.cc:312-324]).

## 5.4 New wire event: PLAYER_ROSTER (EVENT id 38, STATE)

⚠ Id **38**, not 34: worldmap streaming claimed 34-37 (`EVENT_WORLDMAP_BEGIN/END/STATE/
SUBTILES`) after this chapter was written. Re-read the enum in `presenter_network.cc`
before appending any further event; "next free" ages badly.

Purpose: tell every viewer, after every baseline, which sessions own which actors.

```
EVENT_PLAYER_ROSTER = 34   (EVENT_FLAG_STATE)
payload: u16 rowCount, then rowCount x {
    i32 slot            // registry slot (stable forever)
    i32 actorNetId      // this generation's netId for the actor (0 = none, defensive)
    i32 sessionId       // owning session, 0 = unbound (unclaimed / disconnected)
    u8  alive           // !DAM_DEAD at emit time (courtesy; deltas are authoritative)
    u16 nameLen; bytes name[nameLen]  // reserved; v1 emits nameLen=0
}
```

Emitted by a new no-op base virtual `Presenter::playerRoster(...)` (same recipe as every
prior event: base no-op → NetworkPresenter override → byte-identical elsewhere
[V AGENTS/p5-server-plan.md presenter recipe]). Emit sites:

1. `serverEmitBaseline` tail (after `snapshotEnd`, before the frame closes) — every
   baseline re-announces the fresh netIds. [server_loop.cc]
2. On any binding change (claim granted, claim released by disconnect) — Ch 6. The
   binding lives in f2_server's control plane, which cannot call the emit directly from
   core… it CAN: `presenter()` is reachable from anywhere in core, and server_control.cc
   already calls `presenter()->consoleMessage` [V server_control.cc:117-123]. Emit from
   `serverControlLine`/`serverControlBeginDrain` on change.

The decoder records the roster verbatim (slot/netId/sessionId rows) and derives its OWN
binding by matching `sessionId == mySessionId` (§5.5).

## 5.5 Telling a viewer its own sessionId (preamble extension)

The ONLY per-client bytes are the accept preamble [V server_net.cc:171-180]. Extend it:

```
old: "F2NS" | u16 version=1                       (6 bytes)
new: "F2NS" | u16 version=2 | i32 sessionId       (10 bytes)
```

- Bump `kWireVersion` to 2 in ONE constant shared by encoder + both preamble writers:
  `presenter_network.cc begin()` [V "F2NS"+LE16 at presenter_network.cc, mirrored
  constant in server_net.cc:175 kPreamble] — the comment there already warns the bytes
  MUST match; make them literally share a header constant
  (`src/wire_defs.h`, new) so they cannot drift.
- `acceptClients` (connect-at-start path) must ALSO send the preamble+sessionId — today
  the boot-time client gets the preamble from the NetworkPresenter's `begin()` on the
  shared stream [V server_loop.cc:406-409 install order, presenter_network.cc begin()].
  **Change:** `begin()` stops writing the preamble to the SOCKET sink (it keeps writing
  it to the F2_NETSTREAM file sink, where there is no per-client concept); the socket
  path sends it per-client at accept in both `acceptClients` and `acceptPending`, with
  the accepted client's sessionId. This unifies the two join paths and is where the
  per-client identity byte belongs. ⚠ This is a small ordering change on the boot path —
  verify with `scripts/check_netsocket.sh`-family gates ([U exact gate name; run
  `check.sh` and let it name failures]).
- Viewer: `ClientConnection::connect` parses version 2 + sessionId; stores
  `_mySessionId`. Version 1 peers are rejected (server and viewer ship together; no
  compat shim needed — [OPEN-Q #7 if mixed-version support is ever wanted]).

## 5.6 Viewer-side binding: repointing gDude

After every `applyBlob` (and roster decode), the viewer resolves its actor:

```c
// client_net.cc, end of applyBlob() after seedNetMap():
int myNetId = rosterNetIdForSession(_mySessionId); // 0 = spectator / not yet known
if (myNetId != 0 && myNetId != (gClientHostDude ? gClientHostDude->netId : 0)) {
    Object* mine = lookup(myNetId);
    if (mine != nullptr) gDude = mine;      // the per-client ROLE binding
} // else: gDude stays the blob-loaded host actor (P1 viewer, or spectator)
```

Ordering within applyBlob (supersedes the §2.4 list where they overlap):

1. `gDude = gClientHostDude` (undo any prior rebind — BEFORE mapLoad, see §5.3 hazard)
2. `presReset()` → `gameTimeSetTime` → `mapLoad(stream)` → `_obj_load_dude(rd)`
3. load extra-actor appendix sections → client registry (slot order)
4. `_map_fix_critter_combat_data()` → `objectAssignAllNetIds()` → `scriptsDisable()`
5. `seedNetMap()`
6. roster may arrive in the same frame AFTER the blob — the rebind therefore also runs in
   the roster decoder (`onPlayerRoster`): re-resolve + repoint + `tileSetCenter(gDude->tile,
   TILE_SET_CENTER_REFRESH_WINDOW)` + `mapSetElevation(gDude->elevation)` if it differs.

What the repoint buys FOR FREE (all already keyed on gDude):
HP/AP bars + interface HUD, inventory/skilldex/char screens, crosshair + to-hit preview,
myTurn keying (`netId == gDude->netId` [V client_net.cc:1494-1507]), dialog editability
gate (`gDude->netId == driverNetId` [V client_dialog.cc:103]), camera centering, combat
outlines (self-relative [V client_net.cc:120-137]), the DESTROY-guard for the local dude
[V client_net.cc:848-858 — note this guard tests `obj == gDude`; after a repoint it
correctly protects the BOUND actor; the host-actor object additionally must never be
freed by wire DESTROY — extend the guard to `obj == gDude || obj == gClientHostDude`].

Known follow-ups the repoint does NOT cover (accepted v1, enumerate so nobody hunts them
as bugs): the egg/transparency follows `gDude` moves via `objectSetLocation`'s dude
branch on the CLIENT — [U which exact site keys the egg; verify visually: if the
transparency circle sticks to the host actor on P2's viewer, the egg-follow site needs
the same repoint treatment]; pipboy/character screens show the shared premade + host
name; `_dude_fidget` is already removed on viewers.

## 5.7 Spectators

A session with no roster row (all actors bound, or it never claimed) is a SPECTATOR:
`gDude` stays the blob's host actor → renders P1's view exactly as every extra viewer
does today. It sends no verbs (Ch 6/7 reject unbound sessions), sees dialog read-only.
This is current behavior formalized — zero new code beyond the reject path.

--------------------------------------------------------------------------------
# Ch 6 — Session lifecycle: connect, claim, disconnect, reconnect, spectator

## 6.1 The binding table (replaces `gClaimant`)

In `server_control.cc`, replace [V :48]:

```c
// was: static int gClaimant = 0;
struct ActorBinding {
    int sessionId; // 0 = unbound
};
static ActorBinding gBindings[kMaxPlayerActors]; // indexed by registry slot
```

Helpers (server_control.h additions):

```c
// Slot bound to this session, or -1. O(N).
int serverControlSlotForSession(int sessionId);
// The session's actor (registry lookup through the binding), or nullptr.
Object* serverControlActorForSession(int sessionId);
// True iff ANY session holds a binding (replaces serverControlHasClaimant for
// the combat barrier's coarse "someone is driving" test; Ch 8 refines this
// to per-actor).
bool serverControlAnyBound();
// Session bound to this slot (0 = none) — for the roster emit + turn barrier.
int serverControlSessionForSlot(int slot);
```

`serverSetClaimantQuery(serverControlHasClaimant)` [V server_main.cc:63] repoints to
`serverControlAnyBound` (name kept via a shim or updated at the install site — prefer
updating the install site; keep `serverControlHasClaimant` as a deprecated alias only if
other callers exist — [V grep: callers are server_main.cc:63,228 and the barrier through
the query pointer; update both]).

## 6.2 Connect + claim

- On connect, the session is UNBOUND (spectates; receives blob + roster like everyone).
- `claim` (no arg): bind the first slot with `sessionId == 0`, **preferring slot 0 if the
  host slot is free** (slot order IS preference order). Idempotent for a session that
  already holds a slot (re-`claim` returns its current slot). All slots taken → denied →
  session stays spectator; log
  `"control claim from session %d denied (no free actor)"`.
- `claim <slot>`: bind that specific slot if free (reconnect affordance + tooling).
  Validate `0 <= slot < playerActorCount()`.
- On ANY binding change: emit `playerRoster` (Ch 5.4) so every viewer re-derives.
- The viewer sends `claim` once after its first blob, exactly as today
  [V main.cc:1080-1082]; no viewer change needed for the default path.

**Dead-actor claim rule:** a slot whose actor is DAM_DEAD is still claimable/bindable —
what a dead actor can DO is Chapter 9's policy seam, not the binding layer's business.

## 6.3 Disconnect (the orphaned body)

Current `serverControlBeginDrain` releases the single claim + kills the single pending
latch [V server_control.cc:385-397]. Target:

```c
void serverControlBeginDrain(const std::function<bool(int)>& liveSession)
{
    bool changed = false;
    for (int slot = 0; slot < playerActorCount(); slot++) {
        int sid = gBindings[slot].sessionId;
        if (sid != 0 && liveSession && !liveSession(sid)) {
            fprintf(stderr, "f2_server: control slot %d released (session %d gone)\n", slot, sid);
            gBindings[slot].sessionId = 0;
            serverControlDropPendingFor(sid);   // per-session latch dies with it (Ch 7.3)
            combatIntentDropForSlot(slot);      // queued combat intents die with it (Ch 8.4)
            changed = true;
        }
    }
    if (changed) serverControlEmitRoster();
    gLineCounts.clear();
}
```

**What happens to the orphaned body (specified, v1):** NOTHING, immediately. The actor
object stays in the world exactly where it was (standing, mid-combat, whatever), stops
receiving intents, and:

- Out of combat: it just stands there. Any in-flight approach walk finishes or is
  cancelled with the latch (`serverControlDropPendingFor` mirrors today's
  `gPendingActive = false` [V :394]).
- In combat: its turns auto-end immediately — the barrier's wait test is per-actor
  (Ch 8.3): no bound live session for the current actor → no wait → the legacy
  auto-end-turn path runs [V combat.cc:3564-3573 semantics, generalized]. Combat
  proceeds; the body remains a valid target. This is the same behavior a claimant-less
  server exhibits today, scoped to one actor.
- It remains claimable: a reconnecting player (same human, NEW sessionId — sessionIds
  are never reused [V server_net.h:113-127]) sends `claim <slot>` (or bare `claim`,
  which finds the freed slot) and resumes the same body with its inventory/HP intact.
  **There is no auth/identity on the wire (v1 dev server, trust boundary is permissive
  by design [V server_main.cc:250-253]); first claimer wins the orphan.** [OPEN-Q #8:
  reserve-by-token for reconnect, post-demo.]

## 6.4 Mid-dialog / mid-combat disconnect edges

- Mid-dialog: the block-and-pump barrier already bails when its driver vanishes — the
  pump body tests claimant liveness each iteration and `dialogEnd` is emitted on every
  exit [V server_main.cc:210-230, game_dialog.cc:2251-2252]. Generalize the bail test to
  "the DRIVER session is still bound" (Ch 11.3).
- Mid-combat: covered above; additionally the idle timer (base 60 s
  [V combat.cc:3502-3510]) is a backstop, not the mechanism — the no-wait path ends the
  turn the same beat the drain releases the binding.
- The serve loop keeps its "all viewers gone → stop serving" rule
  [V server_main.cc:314-316]; that is a whole-server shutdown decision, orthogonal to
  per-session bindings. With `F2_SERVER_PLAYERS>1`, one remaining viewer keeps the
  server up.

## 6.5 Late join while all actors bound

Covered by §5.7 + §6.2: joins as spectator (gets blob + roster; roster has no row with
its sessionId), may `claim` later when a slot frees. The join still triggers the C.4
rebaseline broadcast (world-reload hitch for everyone — existing accepted cost
[V CLIENT_JOIN_DESIGN.md §C.4, server_main.cc:258-259]).

--------------------------------------------------------------------------------
# Ch 7 — The verb layer: per-session actor binding

## 7.1 The dispatch change (mechanical, broad — every handler)

`serverControlLine(sessionId, line)` [V server_control.cc:399-1097] currently: parse
verb → per-family `sessionId != gClaimant` gate → act on `gDude`.

Target pattern:

```c
void serverControlLine(int sessionId, const char* line)
{
    // flood guard unchanged [V :401-403]
    // parse unchanged [V :405-413]

    if (strcmp(verb, "claim") == 0) { /* Ch 6.2 */ return; }

    Object* actor = serverControlActorForSession(sessionId);
    if (actor == nullptr) {
        fprintf(stderr, "f2_server: control %s from unbound session %d ignored\n", verb, sessionId);
        return;
    }
    ServerActorScope scope(actor);   // §7.2 — gDude := actor for this verb
    // ... existing handlers, with `gDude` reads replaced by `actor`
    //     (equivalent under the scope; prefer the explicit parameter in
    //      touched lines so the code stops growing new gDude reads) ...
}
```

Per-family gate replacement: every `sessionId != gClaimant` check [V :427, :464, :492,
:529, :703, :732, :937, :1041] becomes the single unbound-session check above (done once,
top of function, after `claim` handling). The per-verb combat/out-of-combat gates,
validation, and trust-boundary clamps stay exactly as they are.

## 7.2 `ServerActorScope` — the scoped gDude swap (and its honest limits)

```c
// server_players.h
// Scoped binding of the acting player for a verb/dialog/script-context span.
// Sets gDude to `actor`; on destruction restores THE PREVIOUS CONTEXT ACTOR
// (stack discipline), so the outermost scope restores slot 0 and the invariant
// "gDude == the host actor outside all scopes" still holds.
//
// ⚠ Scopes DO nest, in exactly one place: the dialog barrier holds a
// conversation-long scope (Ch 11.2) while its pump services OTHER sessions'
// verbs, each of which opens its own. Restoring slot 0 instead of the previous
// actor would silently re-anchor the rest of the conversation — including every
// node proc's dude_obj — onto the host. DEBUG-assert a depth counter <= 2
// rather than non-reentrancy.
class ServerActorScope {
public:
    explicit ServerActorScope(Object* actor);
    ~ServerActorScope();
};
```

WHY the swap is needed even though handlers take `actor`: the callee tree below the
verb layer reads `gDude` internally, and those reads are exactly what makes the verb
behave "as the acting player". Verified examples reachable from today's handlers:

- `_action_use_an_item_on_object` path: `scriptSetObjects(item->sid, gDude, item)`
  [V proto_instance.cc:866, 1027] — the USE script's `source_obj` must be the actor.
- Skill use: `skill.cc` PC-branches + XP award `pcAddExperience` on success
  [V skill.cc:520-543] and "You…" message selection by `obj == gDude` (many sites,
  Appendix A).
- `_obj_examine(gDude, target)` / `_obj_look_at` message voice [V server_control.cc:637-639].
- Book/drug use paths branching `user == gDude` [V proto_instance.cc:784-840].

WHAT THE SWAP DOES NOT FIX (documented residue — do not chase as bugs):

1. PC-GLOBAL stores hit regardless of swap: `pcAddExperience` (one XP pool, Ch 15),
   PC skill points/tag skills, `DUDE_STATE_SNEAKING`, kill counts `killsIncByType`
   [V combat.cc:5536], `gDudeName`. Acting as P2 credits P1's sheet.
2. Poison/radiation remain pointer-gated `!= gDude` deep inside critter.cc
   [V critter.cc:332, 381, 417, 490] — the swap makes a P2 verb-scoped poison work,
   but QUEUED poison events fire outside any scope (Ch 10.6).
3. HUD presenter calls keyed `== gDude` fire for whichever actor holds the scope —
   harmless: `hud*` events are not on the wire (dropped client-local chrome
   [V MP_PROTOCOL.md §7d]).

## 7.3 Per-session pending interaction (walk-then-act latch)

Current: ONE global latch `gPending`/`gPendingActive`; the comment already names the
generalization ("a map<sessionId,...> is the named N-player generalization")
[V server_control.cc:76-110].

Target: `static std::unordered_map<int, PendingInteraction> gPendingBySession;`
(sessionId → latch; ≤ N entries).

- `serverControlArmInteraction(verb, target, arg)` gains the session/actor:
  `serverControlArmInteraction(int sessionId, Object* actor, int verb, Object* target, int arg)`
  — replaces `Object* actor = gDude` [V :277] with the parameter; arms
  `gPendingBySession[sessionId]`; the last-writer-wins "a fresh action supersedes"
  semantics become per-session.
- `serverControlAdvancePending()` [V :312-353] iterates the map each beat; per entry the
  body is IDENTICAL to today with `actor = serverControlActorForSession(sessionId)`
  (re-resolved each poll; entry dropped if the session unbound — Ch 6.3's
  `serverControlDropPendingFor`). Keep the id+pid re-validation of the TARGET exactly as
  is [V :324-334]. The combat-entry cancel [V :317-321] drops ALL entries (combat
  starting cancels everyone's approach, mirroring vanilla animationStop-on-combat).
- `interactionCannotGetThere()` [V :117-123] streams through `presenter()->consoleMessage`,
  which BROADCASTS to every viewer — with N players that misattributes feedback.
  **Accepted v1 wart** (console text is broadcast by design today; per-client console
  needs per-client framing, banked). Note it in the code comment.

## 7.4 Per-verb notes (only where the mechanical swap is NOT sufficient)

- `mv` [V :426-445]: `serverControlMove(actor, tile, run)` is already
  actor-parameterized [V :360-378] — pass the session's actor. The walk registry keys
  per-owner (`ServerWalk.owner`), so N concurrent walks already coexist
  [V server_anim.cc — `serverAnimWalkInFlightFor(actor)` is per-owner :256-265].
- `cancel` [V :463-474]: cancel THIS session's latch + `reg_anim_clear(actor)`.
- `rot` [V :547-554]: `objectRotateClockwise(actor, …)`.
- `look`/`push` [V :626-655]: swap-only.
- Inventory family [V :726-913]: swap-only — every read is `gDude->data.inventory` or
  `critterGetArmor(gDude)`/fid rebuild on `gDude`; all become `actor`. The
  server-authoritative equip-fid rebuild [V :889-896] operates on `actor` — verify live
  that a P2 wield re-fids P2 (eyeball, `viewer_live.sh`).
- Loot family [V :934-1025]: swap-only; the adjacency check `objectGetDistanceBetween(gDude,
  container)` [V :971] becomes `actor`. NO per-container locks (single-threaded server
  serializes concurrent take/put — deliberate, keep the comment [V :926-933]).
- `useitem_armexplosive` [V :814-848]: swap-only.
- `cstart` [V :695-713] + the loop-side consumer [V server_loop.cc:292-297]: the latch is
  a bool; with N players the consumer's dead-actor guard reads `gDude` (host). Change the
  latch to carry the REQUESTER's slot (`static int gPendingCombatStartSlot = -1`), and
  the loop-side guard tests THAT actor alive:
  `Object* starter = playerActorAt(slot); … !critterIsDead(starter)`. `_combat(nullptr)`
  itself is actor-agnostic (roster from proximity/team; no CSD attacker).
- Combat verbs [V :1036-1093]: intents gain the actor — Ch 8.4.
- `dsay`/`dend` [V :490-514]: owner gate becomes real — Ch 11.
- **`talk`** [V :684-692 + :172-177]: the dialog request must carry the REQUESTER —
  Ch 11.2 (new `scriptsRequestDialogAs(target, actor)` or an actor field beside
  `gScriptsRequestedDialogWith` [V scripts.cc:242, 1223-1226]).

## 7.5 Session flood guard

`gLineCounts` is already per-session [V server_control.cc:51, 401-403]; unchanged.

--------------------------------------------------------------------------------
# Ch 8 — Combat with N player actors

## 8.1 Team assignment

All player actors share the host's team by construction: `_obj_copy` memcpy's
`data.critter.combat.team` from gDude (Ch 4.3). No further team work is needed for
outlines (`green if same team as gDude` [V combat.cc:2661]), `combatAttemptEnd`'s
hostile scan (keyed on `dudeTeam` [V :3052-3061]), or `_combat_should_end` (team ==
gDude's team counts as "our side" [V :3306-3317]). Hostiles keep their proto teams.

**Verify at M3 gate:** dude team value on the object — [U] the numeric team id of the
premade dude (believed 0, not read from source). Print it once
(`F2_TRACE_EVENTS` or debug port `inspect`) before relying on it.

## 8.2 Roster placement + turn order (locked decision #6)

Rule: sequential vanilla-skeleton initiative; player actors form ONE contiguous chain
anchored at the host's slot (host, +1, +2, then everyone else by Sequence).

- `_combat_sequence_init(attacker, defender)` [V combat.cc:2938-2993]: after the existing
  attacker/defender/dude placement swaps, add a loop placing every OTHER registry actor
  next (slot order), same swap idiom:

  ```c
  // Place remaining player actors contiguously after the dude
  // (mp-actor locked decision #6: host, +1, +2, then roster).
  for (int slot = 0; slot < playerActorCount(); slot++) {
      Object* p = playerActorAt(slot);
      if (p == attacker || p == defender || p == gDude) continue;
      for (int index = 0; index < _list_total; index++) {
          if (_combat_list[index] == p) {
              Object* temp = _combat_list[next];
              _combat_list[index] = temp;
              _combat_list[next] = p;
              next += 1;
              break;
          }
      }
  }
  ```

  Note the existing dude block only runs when the dude is neither attacker nor defender
  [V :2969-2981]; the new loop runs unconditionally for non-dude players (skipping any
  that are attacker/defender, already placed).
- `_combat_sequence` (per-round re-sort by Sequence stat [V :2998-3047]) will INTERLEAVE
  players with NPCs from round 2 on — all combatants share `_compare_faster`
  [V :2911-2933]. **Divergence from decision #6's "one contiguous chain per round"**:
  keeping the chain contiguous every round requires a post-sort stable partition
  ("players first in slot order, then the rest in Sequence order") applied to
  `_combat_list[0.._list_com)`. Spec: add exactly that partition at the tail of
  `_combat_sequence`, gated `serverLoopActive() && playerActorCount() > 1` (golden-safe:
  N==1 keeps vanilla order since the dude is one element; still gate it to be
  bulletproof). One input-barrier WINDOW per round then falls out: the machine hits
  consecutive kPlayerTurn states.
- Same-pid Sequence ties: all players share the premade's Sequence stat →
  `_compare_faster` ties → qsort order is implementation-defined. The partition above
  makes player order deterministic (slot order); NPC ties keep vanilla quirks.

## 8.3 The session machine, generalized

All in combat.cc; each item names current → target.

1. `combatSessionIsPlayerActor` [V :3549-3562]: `players[] = {gDude}` → registry:

   ```c
   static bool combatSessionIsPlayerActor(Object* obj) { return playerActorIs(obj); }
   ```

2. **Per-actor barrier state.** `CombatSession.playerTurnBegun/idleBudgetMs/lastActionSimTs`
   [V :3537-3539] stay ONE set of fields — only one player turn is ever live at a time
   (sequential chain), and the state resets at each turn's BEGIN
   [V :3711-3718]. No struct change needed; add `int playerTurnSlot` (the current
   player's registry slot) set when kTurns detects a player actor [V :3876-3879], so the
   pump and wait test know whose turn it is.
3. `combatSessionPlayerTurnBegin` [V :3628-3694]: replace `Object* obj = gDude` with
   `Object* obj = playerActorAt(s.playerTurnSlot)`, and hold a `ServerActorScope` for the
   duration of the player's turn processing each beat (BEGIN + pump + resolve) so the
   deep dude-reads inside (`perkGetRank(gDude, PERK_BONUS_MOVE)` [V :3644],
   `_combat_attack_this(_gcsd->defender)` [V :3674-3676], scripts run from
   SCRIPT_PROC_COMBAT) act on the turn's actor. ⚠ The `_gcsd` initial-attack replay
   [V :3674] fires on whichever player's turn comes first — vanilla semantics assume the
   initiator; guard it `if (_gcsd != nullptr && obj == s.csd.attacker)` — the csd
   attacker is by-value stable [V :3527-3540].
4. **Wait policy per actor** — `combatSessionShouldWait` [V :3568-3573] becomes
   slot-aware:

   ```c
   static bool combatSessionShouldWait(const CombatSession& s)
   {
       int sid = serverControlSessionForSlot(s.playerTurnSlot);   // via the claim-query
       return (sid != 0) || combatIntentPendingForSlot(s.playerTurnSlot)
           || combatResumableTurnWaitForced();
   }
   ```

   Bridge shape: `serverSetClaimantQuery` today carries a zero-arg bool
   [V server_loop.cc:57-69]; add a second installed pointer
   `serverSetSlotBoundQuery(bool (*)(int slot))` beside it (same idiom, null on
   client/probe → false → auto-end, byte-identical). An unbound/disconnected actor's
   turn thus ends immediately (Ch 6.3), and a bound-but-idle player gets the idle budget
   [V :3575-3578] exactly as today.
5. Finalize/turn-end tails: `combatSessionDudeFinalize` [V :3587-3614] and the resolve
   path [V :3763-3778] read `gDude` 6× — all become the turn actor via the held scope
   (they already run inside the player-turn processing). **EXCEPT** the two rules that
   must aggregate over ALL players — Ch 9.4: `(gDude->…DAM_DEAD) → return -1` [V :3603,
   :3276] and `_combat_elev == gDude->elevation` [V :3608, :3280].
6. `combatPlayerTurnShouldBreak`/`OutOfAp` [V :3099-3124]: read the turn actor —
   under the scope they already do (gDude == turn actor inside the scope). Leave the
   text as `gDude`; add a comment pinning the scope requirement. The
   `_game_user_wants_to_quit != 0` test inside shouldBreak [V :3109-3111] is the
   combat-break signal path — see Ch 9.3 (unchanged mechanics).
7. Watchdog, round bookkeeping, teardown: untouched (actor-agnostic)
   [V :3384-3456].

## 8.4 Per-actor intents

`CombatIntent` [V src/combat_intent.h] gains a routing field:

```c
struct CombatIntent {
    int kind;
    int arg;
    int hitLocation = HIT_LOCATION_UNCALLED;
    int hitMode = COMBAT_INTENT_HITMODE_AUTO;
    bool run = false;
    int actorSlot = 0;   // NEW: registry slot of the issuing player (0 = host,
                         // the default keeps every existing caller/golden intact)
};
void combatIntentPush(int kind, int arg, int hitLocation = HIT_LOCATION_UNCALLED,
    bool run = false, int hitMode = COMBAT_INTENT_HITMODE_AUTO, int actorSlot = 0);
// NEW:
bool combatIntentPeekForSlot(int slot, CombatIntent* out); // first intent for slot
void combatIntentPopForSlot(int slot);                     // pop that one
bool combatIntentPendingForSlot(int slot);
void combatIntentDropForSlot(int slot);                    // Ch 6.3 disconnect
```

Queue semantics: the FIFO holds a MIX of slots; the pump consumes only the CURRENT
turn's slot (in queue order among that slot's entries); other slots' intents wait for
their turn. This matches today's "an intent queued during an AI turn waits for the
barrier" [V server_control.cc:1027-1035 comment]. A disconnect drops that slot's
queue.

- `serverControlLine` combat verbs [V :1050-1091] pass
  `actorSlot = serverControlSlotForSession(sessionId)`.
- `combatServerPumpIntents` [V combat_drain.cc:170-279]: takes the acting slot (or reads
  `gCombatSession.playerTurnSlot` via a getter); every `combatIntentPeek/Pop` becomes the
  ForSlot variant; every `gDude` in the body (move register :204-206, bad-shot+attack
  :239-240, hit-mode default :94-105 `serverDudeHitMode` → `serverActorHitMode(actor)`,
  target-resolution "hostile relative to" :134-141) becomes the turn actor (under the
  scope they already resolve; prefer explicit `actor` in touched lines).
- `serverResolveTarget` [V :113-155]: nearest-hostile fallback (`arg<0`, debug-port
  convenience) keys distance/team/whoHitMe off the ACTING actor.
- Legacy `_combat_input()` [V :281-298] (non-resumable path): pumps slot 0 only —
  the legacy path never runs with N>1 (resumable combat is mandatory for the demo;
  assert `playerActorCount()==1 || combatResumableEnabled()` at combat entry,
  fail-loud).

## 8.5 Per-viewer turn cueing

Already correct by construction: `turnStart` carries the actor + `isPlayer`
[V combat.cc:3177, 3633]; `isPlayer` must become `playerActorIs(critter)`:

- `_combat_turn`'s emit: `presenter()->turnStart(obj, obj == gDude, …)` [V :3177] →
  `playerActorIs(obj)`.
- `combatSessionPlayerTurnBegin`'s emit passes `true` already [V :3633] — correct.
- Loop-side mid-combat re-assert after a rebaseline: `cur == gDude` [V server_loop.cc:361]
  → `playerActorIs(cur)`.

The viewer needs NO change: myTurn = `isPlayer && netId == gDude->netId`
[V client_net.cc:1494-1507] — with the Ch 5.6 repoint, P2's viewer flips myTurn only on
P2's TURN_START. Another player's turn is `isPlayer=1, netId != mine` → red lights, watch
cursor (the guardrail held).

## 8.6 Combat entry pulling everyone in

`_combat_begin`/`_caiSetupTeamCombat` build the roster from critters on the combat
elevation [V combat_ai.cc:1718-1729, combat.cc roster build ~:2540-2560 [U exact
lines]]; teammates enter via `_combat_add_noncoms`/`_combatai_notify_friends(gDude)`
[V combat.cc:2874-2906]. Because all players share the team and stand on the same map,
P2 lands in `_combat_list` like any teammate critter; the §8.2 placement makes it a
TURN-taking combatant (in `_list_com`) when… ⚠ **VERIFY**: a teammate who has not acted
may sit in the noncom partition until `_combatai_want_to_join` admits it
[V :2878-2904] — an idle P2 might not get a turn in round 1 if the initial partition
classifies it noncom. The `_combat_sequence_init` placement loop (§8.2) runs on
`_list_total` and bumps `next` INTO `_list_com` (`_list_com = next` [V :2983]) — placing
players there FORCES them into the combatant partition from round 1. State this as the
intended mechanism; verify at the M3 gate ("P2 gets a turn in the first round without
having attacked").

Also generalize `_combatai_notify_friends(gDude)` at combat end [V :2876] and the
combat-exit knockout-wake block (`_critter_is_prone(gDude) …` [V :2815-2818]) over the
registry (loop the registry; identical body per actor).

## 8.7 Friendly-fire toggle

Env `F2_FRIENDLY_FIRE` (default **1** = vanilla-faithful ON; `0` disables player↔player
damage). Requirement source: bugs/008 item 7 ("if easy as a toggle").

Analysis of the insertion point (owner asked for `_combat_apply_attack_results`; the
honest options):

- (a) **Attack-commit gate (RECOMMENDED):** in `_combat_attack_this`'s server drain
  and/or `combatServerPumpIntents`' attack branch [V combat_drain.cc:238-240]: if
  `playerActorIs(actor) && playerActorIs(target) && !friendlyFireEnabled()` → reject the
  intent with a console message ("You cannot attack your ally.") and end nothing (treat
  like a bad-shot refusal: pop + turn continues… note the existing failure branch ENDS
  the turn [V :241-258]; a friendly-fire refusal should NOT end the turn — pop +
  `continue`, keep pumping). PROS: no damage math ever runs; no half-applied Attack
  struct; AoE (explosion extras) is untouched (a burst that CATCHES an ally still hurts
  them — vanilla-faithful splash). CONS: direct attacks only.
- (b) Damage-application gate inside `_combat_apply_attack_results`/`_apply_damage`
  (zero out damage/flags for player-actor victims): touches the Attack struct after
  computation (flags like DAM_DEAD already computed by `attackComputeDeathFlags`
  upstream [V combat.cc:5619-5624 comment]) — fragile, double-book-keeping (ammo spent,
  XP paths), and the wire's ATTACK_RESULT would still carry the computed damage.
  REJECTED for v1.

Spec: implement (a) only. Splash/burst friendly damage remains possible with the toggle
off — document in the demo notes. [OPEN-Q #9: whether "FF off" should also zero AoE
extras; needs (b)-class surgery, adversarial review mandatory.]

## 8.8 Combat-end rules with N players

`_combat_should_end` [V :3289-3325] currently ends when the DUDE is out of the roster.
Target: end when NO player actor remains in `_combat_list[0.._list_com)`:

```c
// was: linear scan for gDude; end if absent
bool anyPlayer = false;
for (index = 0; index < _list_com; index++)
    if (playerActorIs(_combat_list[index])) { anyPlayer = true; break; }
if (!anyPlayer) return true;
```

The team-hostility tail keys off `gDude->data.critter.combat.team` [V :3306] — all
players share it; unchanged.

Dead-player and elevation rules move to Ch 9.4 (they are death-semantics, not
combat plumbing).

--------------------------------------------------------------------------------
# Ch 9 — Player death and server survival (CRITICAL)

**The requirement, verbatim: the dedicated server must NEVER terminate because a player
died.** A bug in exactly this family killed the server mid-fight on 2026-07-19 (commit
"Temple warrior crash…": the serve predicate tested `_game_user_wants_to_quit != 0` and
op_terminate_combat's transient `=1` shut the server down
[V server_main.cc:302-310 comment; combat.cc:3884-3898 comment]).

## 9.1 The quit-flag semantics (pin this before touching anything)

`_game_user_wants_to_quit` [V game.cc:95]:

- `1` = **in-band combat-break signal, NOT a quit.** Set by `opTerminateCombat`
  [V interpreter_extra.cc:4772-4785] and `mapSetTransition` while in combat
  [V map.cc:1084]. CONSUMED by `combatTeardown` (`==1 → =0` [V combat.cc:3453-3455]) and
  `combatPlayerTurnResolve` [V :3131-3134].
- `2`/`3` = terminal quit. `gameTerminalQuitRequested()` = `> 1` [V game.cc:98-101] is
  the ONLY test server code may use for shutdown decisions
  [V server_main.cc:302-310, 196-201 — both already correct].

## 9.2 The complete terminal-site census (Appendix B holds the table; these are the
server-reachable ones and what each becomes)

**S1. `critterKill` dude branch — THE site** [V critter.cc:911-915]:

```c
    if (critter == gDude) {
        endgameSetupDeathEnding(ENDGAME_DEATH_ENDING_REASON_DEATH);
        _game_user_wants_to_quit = 2;
    }
```

Reached on the server for EVERY combat death via
`_combat_apply_attack_results(false)` → `critterKill` finalization
[V combat.cc:5640-5652] and for non-combat HP-zero via `critterAdjustHitPoints`
[V critter.cc:304-306] (poison DoT, scripted damage) and radiation stat-collapse
[V critter.cc:604-611]. **Today a player death = server termination.** Target:

```c
    if (playerActorIs(critter)) {
        if (serverLoopActive()) {
            playerActorDied(critter);        // the POLICY SEAM (§9.5) — no quit,
                                             // no endgame; the world keeps running
        } else {
            endgameSetupDeathEnding(ENDGAME_DEATH_ENDING_REASON_DEATH);
            _game_user_wants_to_quit = 2;    // vanilla single-player, byte-identical
        }
    }
```

(`endgameSetupDeathEnding` is a no-op stub on f2_server anyway
[V server_stubs.cc:300-302] — but do not rely on the stub: the QUIT write is the
kill-shot and it is core code. The `serverLoopActive()` fork is the same golden-safe
idiom as every other decouple.)

**S2. 13-year game-clock timeout** [V scripts.cc:363-375 `gameTimeAddTicks`]:
`year >= 13 → endgameSetupDeathEnding(TIMEOUT) + quit=2`. Server-reachable (sim clock
advances every beat). Target: same fork — `serverLoopActive()` → log loudly
(`debugPrint("server: 13-year limit reached — vanilla endgame suppressed")`) + do
nothing. [OPEN-Q #10: a freeplay policy hook could ride playerActorDied's sibling
`worldEndgameReached()`; v1 = suppress+log.]

**S3. `opMetarule METARULE_SIGNAL_END_GAME`** [V interpreter_extra.cc:3205-3210]:
script-driven endgame (`quit=2`). Server-reachable through any script. Target: same
`serverLoopActive()` fork — log + no-op. (Vanilla path untouched.)

**S4. `endgamePlayMovie` headless branch** [V endgame.cc:253-259]: deliberately
replicates the terminal "No" (`quit=2`) on the server. Reached via
`opEndgameMovie`/`opEndgameSlideshow` (MODAL rows, SCRIPT_OPCODE_MAP). This is the
FAITHFUL winning-ending terminal — NOT player-death. Leave as is for the demo
(IDEAS.md freeplay is banked); it cannot fire from a player death.

**S5. Client-only sites (no server change; listed so nobody "fixes" them):**
main-loop dude-death poll [V main.cc:400-403], quit dialog [V game_ui.cc:680-686],
load-failure paths [V loadsave.cc:939, 1256], endgame keep-playing prompt
[V endgame.cc:311-318]. The VIEWER is a puppet (none of these run in
`mainClientViewer`'s loop — it has its own loop [V main.cc:1104+]; the dude-death poll
at :400 is `mainLoop`, not the viewer). [V]

**S6. The serve predicate + dialog pump** [V server_main.cc:302-310, 196-201]: already
correct (`gameTerminalQuitRequested`). After S1-S3 land, `quit==2` can only arise from
S4 (true endgame) — the predicate keeps meaning "the game ended", never "someone died".

## 9.3 The `==1` combat-break family (verified safe, keep as is)

`opTerminateCombat` and `mapSetTransition` set `1`; `combatTeardown`/
`combatPlayerTurnResolve` consume it; the resumable machine tears down SAME-BEAT so the
flag never leaks across `serverTick` [V combat.cc:3884-3898 — the load-bearing comment].
`combatPlayerTurnShouldBreak`'s `!= 0` test [V :3109-3111] correctly treats it as
"stop pumping input". NO changes. Any future code testing this flag MUST use
`gameTerminalQuitRequested()` for shutdown semantics — add that one-liner to the S1-S3
commit's review checklist.

## 9.4 Combat semantics when a player dies (mechanics, not policy)

Enumerated dude-dead reads in the combat machine and their N-player replacements:

1. `_combat_turn` epilogue: `if ((gDude->…results & DAM_DEAD) != 0) return -1;`
   [V combat.cc:3276-3278] — ends the whole fight when the dude dies, even during an AI
   turn. Target: `if (serverLoopActive() ? !playerActorAnyAlive() : dudeDead) return -1;`
   — the fight ends only when ALL player actors are dead (or the vanilla rule off-server).
   Same change in `combatSessionDudeFinalize` [V :3603-3605].
2. Elevation epilogue `_combat_elev == gDude->elevation` [V :3280-3285, :3608-3613]:
   keep keyed on the TURN actor under the scope (a player who left the combat elevation
   ends the fight — single-elevation combat is a v1 constraint; with N players use "any
   LIVE player still on _combat_elev" → stay, else end). Implement as a helper
   `combatAnyLivePlayerOnCombatElevation()`; gate `serverLoopActive()`.
3. A dead player's turn: `_combat_turn`/BEGIN already skip DAM_DEAD actors (results
   test → LOSE_TURN strip + skip [V :3181-3182, :3637-3639]) and `_combat_sequence`
   removes dead critters from the roster each round [V :3004-3017] — a dead player
   simply stops getting turns. No change.
4. `_combat_give_exps` early-outs when the DUDE is dead [V :2844-2846] — leave (XP
   policy is Ch 15; the dude-dead guard is vanilla).
5. The mid-fight victim presentation ("player-DEATH sequence races",
   [V COMBAT_CLIENT_DESIGN.md §6 banked]) is unchanged by this spec — banked.

**What the dead player's VIEWER shows (v1, no policy):** the corpse (authoritative fid/
flags rode OBJECT_DELTA), the world keeps streaming, input verbs are refused server-side
(next section). The viewer is NOT kicked. Spectate-your-corpse is the v1 experience —
the policy seam upgrades it later.

## 9.5 The `playerActorDied` seam (exact contract)

```c
// server_players.cc
// POLICY SEAM — invoked exactly once per player-actor death transition, from
// critterKill's finalization (critter.cc), server loop only. The DEFAULT BODY
// IS A NO-OP by owner decision (2026-07-19): respawn / permadeath / spectator
// rules are deliberately NOT designed yet. Replacing this body is the whole
// policy change.
//
// GUARANTEED STATE when this fires (from the critterKill body that precedes
// the call [V critter.cc:819-916]):
//   - actor->data.critter.hp == 0; combat.results has DAM_DEAD;
//   - corpse fid applied (or fall-back), OBJECT_NO_BLOCK set unless CRITTER_FLAT,
//     light off; actor->sid removed (== -1  — note: the dude's sid IS removed by
//     critterKill:898-901 like any critter's);
//   - the actor is STILL in the object list, still in _combat_list if fighting
//     (the round-end sweep partitions it out later [V combat.cc:3004-3017]);
//   - queued DRUG events cleared [V :903-904]; other queue events NOT;
//   - the registry binding (sessionId) is UNTOUCHED — the session stays bound
//     to its corpse (claim/binding is Ch 6's layer).
// MAY fire mid-beat inside combat resolution or a queue drain — the body must
// not re-enter combat, must not mutate the roster, must not emit blocking UI.
// Safe operations: presenter() emits, registry/binding reads, latching state
// for the loop tail to act on.
void playerActorDied(Object* actor)
{
    (void)actor; // v1: deliberate no-op. See MP_PROPOSAL.md Ch 9.5.
}
```

Call sites (complete):

1. `critterKill` [V critter.cc:911-915] — replaces the endgame branch per §9.2-S1. This
   is the SOLE finalizer on the server path for combat deaths [V combat.cc:5640-5652 +
   MP_PROTOCOL.md §4 death note], HP-zero deaths [V critter.cc:304-306], and
   radiation-collapse deaths [V critter.cc:604-611] — all funnel through critterKill.
2. **No other call site.** The animated client death path (`_show_death`,
   actions.cc) never runs under `serverLoopActive()` [V MP_PROTOCOL.md §4 — client-path
   split is post-v1]; if a thin-client death path ever lands, it must add the hook there.

Verb-layer behavior for a dead actor (mechanics, needed so the no-op policy is
coherent): `serverControlLine` refuses action verbs when
`critterIsDead(actor)` — add the test beside the unbound check (Ch 7.1);
allow `look` (harmless read) and `claim`. The walk-then-act poll already drops on
actor death [V server_control.cc:325-334 `critterIsDead(actor)`]. Combat turns
self-skip (§9.4-3). cstart's dead-guard: Ch 7.4.

## 9.6 Gate for this chapter (run it before ANY other MP stage ships)

`F2_SERVER_PLAYERS=1` (registry = {gDude}), viewer_live.sh, enter combat with the Temple
ants, let the dude die. OBSERVE: server process stays up ("f2_server: served … to
completion" never fires early; the serve loop keeps beating); the viewer renders the
corpse; hostiles exit combat when `_combat_should_end` clears; a reconnect/`claim` still
answers. Then `scripts/check.sh` → ALL GATES PASS (every S1-S3 fork is
`serverLoopActive()`-gated → byte-identical off-server).

--------------------------------------------------------------------------------
# Ch 10 — The passive-sim boundary and `dude_obj` (the hardest problem)

The earlier drafts claimed the per-verb `gDude` swap makes gDude references
"transparent". **That claim is wrong as stated, and this chapter replaces it.** Scripts,
AI, timed events and tickers run inside `serverTick` OUTSIDE verb dispatch, where `gDude`
is the host actor (Ch 7.2's scope restores slot 0). Everything in the passive sim that
says "the player" therefore means P1 unless this chapter changes it.

## 10.1 What actually runs outside verb scope (the passive-sim inventory, verified)

Per beat [V server_loop.cc:262-368]:

| Driver | Path | Player-reads it makes |
|---|---|---|
| Critter heartbeat | `_process_bk` → `_doBkProcesses` → `_script_chk_critters` → one critter's `SCRIPT_PROC_CRITTER` per pump [V scripts.cc:696-745] | whatever the script does — `dude_obj` reads dominate (distance/LOS/aggro/floaters) |
| Timed events | `_script_chk_timed_events` → `queueProcessEvents` [V scripts.cc:750-785] | per-event; poison/radiation events are DUDE-GATED (§10.6); script timer events run their proc (dude_obj again) |
| Midnight sweep | `gameTimeEventProcess` → `_critter_check_rads(gDude)` [V scripts.cc:407-437] | rad processing for the HOST ONLY |
| Script requests | `scriptsHandleRequests` — elevator branch repositions `gDude` explicitly [V scripts.cc:936-1065]; dialog request (Ch 11) | host-anchored |
| Combat AI | `combatSessionAdvance` → `_combat_ai` | §10.5 — mostly team/whoHitMe-driven (fine), a few dude anchors |
| Map transition | `mapHandleTransition` moves `gDude` [V map.cc:1125-1130] | Ch 14 |
| Spatial procs | `scriptsExecSpatialProc(object, tile, elev)` — **source = the stepping object** [V scripts.cc:2544-2561]; currently NOT fired by server walks at all (the known traps gap [V AGENTS/p5-server-plan.md]) | source is right; the script BODY still reads dude_obj |

## 10.2 How a script learns who it is dealing with (the resolution model, verified)

Three distinct channels, confirmed in source:

1. **`self_obj`** — the script's owner (`scriptGetSelf`) [V interpreter_extra.cc:1080-1084].
2. **`source_obj` / `target_obj`** — `script->source/target`, set per-proc by
   `scriptSetObjects(sid, source, target)` at the INTERACTION sites: use
   [V proto_instance.cc:1482, 1768], use-item-on [V :1309-1332], pickup [V :205, 263],
   damage/destroy [V combat.cc:5365-5421, 5521], push [V actions.cc:2600], loot
   [V item.cc:3562, 3646], spatial [V scripts.cc:2544-2561]. **NOT set for
   `SCRIPT_PROC_TALK`** (gameDialogEnter calls `scriptExecProc(sid, SCRIPT_PROC_TALK)`
   with no preceding scriptSetObjects [V game_dialog.cc:765-768]) and **NOT set for
   `SCRIPT_PROC_CRITTER`** (`_script_chk_critters` execs bare [V scripts.cc:739-742]) —
   in those procs `source_obj` is STALE (whatever last set it).
3. **`dude_obj`** — `opGetDude` pushes the global `gDude`, unconditionally
   [V interpreter_extra.cc:1122-1127]. **This one opcode is the singleton chokepoint:**
   FO2 content overwhelmingly writes `dude_obj` (not source_obj) for "the player" —
   `if (source_obj == dude_obj)`, `distance(self_obj, dude_obj)`, `attack(dude_obj)`,
   `give_exp`… so whatever `opGetDude` returns IS the game's notion of the player.

Corollary (load-bearing): because dude-ness flows through ONE pointer produced at ONE
opcode, the mechanism space is exactly "what does opGetDude push in a given execution
context" — there is no per-opcode fix-up that can substitute (see §10.4's broadcast
analysis).

## 10.3 Consequence analysis if NOTHING is done (dude_obj == P1 always)

Concrete, Temple-relevant:

- **P2 is invisible to game content.** `critter_p_proc` aggro/floaters key on
  `dude_obj` distance/LOS → hostiles never notice P2 out of combat; town-hostility,
  karma floats, "get away from my stuff" checks — all P1-only. P2 walking alone through
  a hostile room is SAFE (wrongly).
- **Traps half-fire for P2.** Once the spatial-proc walk gap is fixed, the proc runs
  with `source_obj = P2` — but trap scripts typically then do
  `if (source_obj == dude_obj)` before applying damage → false for P2 → no trap. (Trap
  script bodies [U — not decompiled here; the pattern is the documented FO2 idiom.
  Verify with the Temple plates once spatial procs fire at all.])
- **Dialog-side effects hit P1.** Node procs run inside P1-anchored context unless
  Ch 11 scopes the driver: e.g. Cameron's strip-and-fight would strip P1's inventory
  even when P2 initiated the conversation (if Ch 11.4's scope is omitted).
- **Timed script events** (e.g. a healing-powder timer script the actor used) read
  dude_obj → P1.
- XP/quest gvars: correct-ish by accident (they are global anyway — Ch 15).

This is NOT acceptable even for the demo where it breaks the co-op Temple pass
(Cameron/strip, trap damage, aggro). Hence:

## 10.4 Mechanism options (analyzed), and the DECISION

**O1 — status quo (dude_obj = host always).** Zero work. Consequences above. Rejected as
the ONLY mechanism; it remains the fallback for contexts with no better answer.

**O2 — CONTEXT-DUDE (adopted): dude_obj resolves to the acting player when a
player-actor context exists.** Mechanism:

```c
// opGetDude, interpreter_extra.cc:1122 — was: push gDude
static void opGetDude(Program* program)
{
    programStackPushPointer(program, scriptContextDude());
}
// server_players.h:
// The "player" a script should see: the innermost ServerActorScope's actor
// when one is held (verb execution, dialog-driver span, player-sourced proc
// execution), else gDude (the host actor — vanilla value). On non-server
// paths this is ALWAYS gDude (scopes only exist under serverLoopActive()).
Object* scriptContextDude();
```

Because Ch 7.2's `ServerActorScope` already brackets every verb (and Ch 11.4 brackets
the dialog conversation), O2's engine cost is: (a) the one-line opGetDude change,
(b) extending the scope to the two passive dispatch points where a player actor IS
identifiable:

- **Spatial procs:** in `scriptsExecSpatialProc` [V scripts.cc:2544-2561], if
  `playerActorIs(object)` hold a `ServerActorScope(object)` around the
  `scriptExecProc(script->sid, SCRIPT_PROC_SPATIAL)` call. → traps damage the player
  who stepped on them.
- **Interaction-driven procs outside the verb drain:** already covered — use/damage/
  pickup procs run synchronously INSIDE the verb scope (the verb handler calls the
  action function which runs the proc before returning) for out-of-combat verbs;
  in-combat damage procs run inside the turn scope (Ch 8.3-3). Deaths from QUEUED
  events (poison timer) run scope-less → dude_obj = host; accepted (see §10.6).

What O2 leaves at the host anchor (explicit, accepted for the demo):
`SCRIPT_PROC_CRITTER` heartbeats, `SCRIPT_PROC_MAP_UPDATE`/map-enter, timer-event procs,
AI-turn scripts (SCRIPT_PROC_COMBAT for NPCs runs in the AI part of the beat — no
scope → host anchor for its dude_obj reads). So: **out-of-combat NPC aggro/notice
remains P1-keyed in v1.** P2 hides behind P1's aggro shadow. Consequence accepted and
demo-visible; the full fix is O3 or per-context vars (Phase 4+).

**O3 — nearest-player anchor for passive procs. (ADOPTED — see the DECISION below;
the "DEFERRED" verdict in this paragraph is superseded and kept only for its analysis.)** `opGetDude` in a scope-less context
returns the player actor nearest to `scriptGetSelf(program)`. Well-defined within one
proc execution (sim state cannot advance mid-proc — the interpreter runs synchronously
inside the beat [V scripts.cc:689-692 `_updatePrograms` single-threaded]). PROS: aggro,
floaters, notice-checks become per-nearest-player — the natural co-op reading. CONS:
(a) cross-proc flapping: an lvar state machine that latched "dude approached" against P1
can be advanced by P2 (double-fire class); (b) `self_obj`-less programs (map scripts)
need a fallback anchor (host); (c) every proc read becomes position-dependent →
nondeterministic vs. the goldens' expectations if ever run headless-with-registry>1
(goldens run registry==1 → safe). **DEFERRED, not rejected** [OPEN-Q #1]: adopt after
the demo IF P2-invisibility proves unacceptable in play. The seam (scriptContextDude)
makes it a one-function change.

**O4 — broadcast/multiply (run player-keyed effects for every player).** REJECTED with
analysis, since the owner asked: the proc is the unit of execution and runs ONCE;
whether a given mutation inside it is "per-player" (damage dude_obj) or "world-once"
(set gvar, unlock door, spawn loot) is a RUNTIME DATAFLOW property of where the
dude_obj pointer flowed, not a static property of the opcode. Re-running whole procs
per player double-fires every world-once mutation (gvar increments, XP, spawns);
tagging pointers through the interpreter to selectively multiply mutations is a VM
rewrite. No bounded version of O4 exists. (This is also why Appendix C classifies
opcodes by SUBJECT rather than by "broadcastable".)

**DECISION (owner ruling 2026-07-19, SUPERSEDES this chapter's earlier "O2 now, O3
banked" text): O2 + O3 TOGETHER from the start.** `scriptContextDude()` resolves in this
order:

1. the innermost `ServerActorScope`'s actor, if a scope is held (verb / dialog-driver /
   spatial span) — this is O2;
2. else, under `serverLoopActive() && playerActorCount() > 1`, the registered player
   actor NEAREST to `scriptGetSelf(program)` — this is O3, and it is why
   `scriptContextDude` must take the `Program*` (see the signature note below);
3. else `gDude` — the vanilla value, and the ONLY branch reachable off-server or with a
   single registered actor (so goldens are byte-identical by construction).

O3's rationale, accepted limitations, and the rejected per-tick-rotation alternative live
in **OPEN-Q #1** (Ch 19) — read that row before implementing; it is the ruling of record.
Signature consequence: `Object* scriptContextDude(Program* program)` (nullable `program`
→ skip step 2), NOT the zero-arg form sketched in §10.4-O2 above. `opGetDude` receives
`program` and `scriptGetSelf` takes exactly that
[V interpreter_extra.cc:1124, scripts.cc:574].

⚠ What O3 does NOT change: it only re-anchors procs that run scope-less. Everything §10.7
lists as host-bound (sneak, PC progression, dialogue INT-gating) stays host-bound, and a
zone that should affect ALL present players still affects only the nearest one per proc
tick.

## 10.5 Combat AI under N players (mostly already correct — with named anchors)

The targeting core is team/whoHitMe-driven and ALREADY multi-player-correct: attacks by
P2 set `whoHitMe = P2` (`_critter_set_who_hit_me` at attack commit
[V combat.cc:4205-4209]) and `_ai_danger_source` returns `whoHitMe` / attackers-of-me
first [V combat_ai.cc:1661-1714] — NPCs retaliate against whichever player hit them.
Player-team hostility works because players share the team (Ch 8.1).

Remaining gDude anchors in AI, each with its ruling [V combat_ai.cc]:

| Site | What it does | Ruling |
|---|---|---|
| `_ai_run_away(a1, a2==nullptr → a2=gDude)` :1177-1179 | flee direction defaults to "away from the host" | ACCEPT v1 (cosmetic direction); optional: nearest player |
| ATTACK_WHO_WHOMEVER_ATTACKING_ME validity `aiInfoGetLastTarget(critter) != gDude` :1589, :1614 | party-member AI targeting "who attacks the dude" | party-member-only branch [V :1551] — players are not party members; unreachable for our actors. ACCEPT |
| distance-to-dude flee/cover heuristics :2376-2379, :3007-3010 | positioning relative to the host | ACCEPT v1 (behavioral nuance only) |
| `nearestTeammate = gDude` :3168 | fallback rally point | ACCEPT v1 |
| team checks vs `gDude->…team` :3286, combat.cc:5143/:5218 | "is this thing on the player side" | correct under shared team; unchanged |
| `_combatai_msg` dude-relative perception/messaging :3536-3559 | taunt audibility | ACCEPT (presentation) |

Net: NO AI code changes required for the demo; the table is the audit trail.

## 10.6 Dude-gated SUBSYSTEMS (deeper than dude_obj — pointer-gated engine code)

These reject non-dude critters outright; the gDude swap does NOT make them per-actor
because their EVENTS fire outside any scope:

1. **Poison**: `critterAdjustPoison` — `if (critter != gDude) return -1`
   [V critter.cc:328-345]; `poisonEventProcess` — `if (obj != gDude) return 0`
   [V :379-386]. → P2 CANNOT be poisoned (Temple scorpions/spears!).
2. **Radiation**: `critterAdjustRadiation` — `if (obj != gDude) return -1`
   [V :413-419]; `_critter_check_rads` — same [V :488-492]; the midnight sweep only
   checks gDude [V scripts.cc:426]. → P2 immune to rads. (Also note the RADIATED flag
   lives on the shared dude PROTO [V :429] — with shared-pid actors, per-actor rad
   status would need object-side state; another reason this is not a v1 relaxation.)
3. **Sneak**: `DUDE_STATE_SNEAKING` is a dude-global [V interpreter_extra.cc:588-592;
   critter.cc dudeHasState family [U exact lines]] → only the host can sneak.
4. **PC progression stores**: XP/level/skill-points/kill-counts (Ch 15).

RULING for demo 0.2 (owner, 2026-07-19 — OPEN-Q #3): **relax BOTH poison and radiation**
to `playerActorIs(...)`; sneak (3) and PC progression (4) stay host-bound.

⚠ **The gates to relax are the `!= gDude` REJECT returns, not the `== gDude` cosmetic
branches.** Re-verified against the tree 2026-07-19 — the four sites are:

| Site | Function | Gate today |
|---|---|---|
| critter.cc:332 | `critterAdjustPoison` | `if (critter != gDude) return -1;` |
| critter.cc:381 | `poisonEventProcess` | `if (obj != gDude) return 0;` |
| critter.cc:417 | `critterAdjustRadiation` | `if (obj != gDude) return -1;` |
| critter.cc:490 | `_critter_check_rads` | `if (obj != gDude) return 0;` |

Plus the midnight sweep, which only ever passes the host:
`gameTimeEventProcess` → `_critter_check_rads(gDude)` [V scripts.cc:426] → loop the
registry.

The `== gDude` sites at critter.cc:238 / :480 / :578 / :616 / :748 are `gDudeName`, the
HUD indicator, two message-text branches and `critterGetKillType`'s gender lookup — they
are NOT the gates, and relaxing them alone leaves every player but the host immune while
looking like the change landed. (An earlier draft of OPEN-Q #3 cited exactly that wrong
set; corrected here.)

Why this is low-risk: the STATE is already per-object (`critterGetPoison` reads
`obj->data.critter.poison` for any critter [V critter.cc:315]); the gates only decide who
receives effects/penalties/UI. The queued poison event is `queueAddEvent(..., gDude, ...)`
[V :352 — change the hardcoded gDude to `critter`].

⚠ Residual for RADIATION specifically: the RADIATED flag lives on the SHARED dude proto
[V critter.cc:429]. With shared-pid actors that flag is common to every player — relaxing
the gates makes each player accrue its own `data.critter.radiation`, but proto-flag-keyed
behavior stays shared. Verify at the M6 gate; if it misbehaves, fall back to poison-only
and re-open OPEN-Q #3.

## 10.7 What Chapter 10 does NOT fix, one paragraph, for the record

With O2+poison landed, the passive world still *notices only P1* (heartbeat procs),
sneak/rad/progression remain host-bound, dialogue INT-gating uses the shared premade INT
(same build → same options; moot until per-player builds), and any script latching lvar
state about "the dude" can be confused by two bodies claiming dude-ness across different
scopes. These are the knowingly-accepted costs of shipping same-map co-op on a
single-PC content engine; the durable fixes are O3 and per-context vars
(`gvar_set(ctx,X,v)`, IDEAS.md §2), both post-demo.

--------------------------------------------------------------------------------
# Ch 11 — Dialog under N players (driver + spectators)

## 11.1 What already works (A3, verified in working tree)

Node broadcast + spectator gating are BUILT: every viewer receives
`EVENT_DIALOG_NODE(speakerNetId, driverNetId, …)`; editability =
`gDude->netId == driverNetId` [V client_dialog.cc:20-66, 103; client_dialog.h:8-12];
`EVENT_DIALOG_END` tears down every viewer's window; `dsay`/`dend` route through the
trust boundary [V server_control.cc:490-514]. With Ch 5.6's repoint, the per-viewer
comparison is ALREADY per-actor-correct. The gaps are all server-side driver identity:

## 11.2 The driver must be the REQUESTING actor (plumbing)

Today the request path carries only the speaker: `talk` verb →
`scriptsRequestDialog(target)` [V server_control.cc:172-177, scripts.cc:1223-1226] →
`gameDialogEnter(speaker, 0)` [V script_request_handler_server.cc:22] → emits stamp
`driver = gDude` [V game_dialog.cc:1957, 2251-2252]. Changes:

1. `scripts.cc`: beside `gScriptsRequestedDialogWith` [V :242] add
   `static Object* gScriptsRequestedDialogBy;` and
   `void scriptsRequestDialogAs(Object* speaker, Object* driver)` (old
   `scriptsRequestDialog(obj)` forwards with `driver = gDude` — every existing caller
   unchanged [V callers: server_control.cc:176, command.cc dtalk [U line], actions.cc
   `_talk_to` [V actions.cc:2373-2400 region]]).
2. The interaction latch fires it with the session's actor: `interactionFire`'s
   `kInteractTalk` case [V server_control.cc:172-177] →
   `scriptsRequestDialogAs(target, actor)` (actor from Ch 7's dispatch).
3. `scriptsHandleRequests`'s dialog branch [V scripts.cc:1005-1015 region] wraps the
   whole `dialogEnter` in `ServerActorScope scope(gScriptsRequestedDialogBy)` — this
   makes (a) the emitted `driver = gDude` stamps correct WITHOUT touching
   game_dialog.cc, (b) `gameDialogEnter`'s dude reads (`gGameDialogOldDudeTile = gDude->tile`
   [V game_dialog.cc:749-751], range gate — server-skipped anyway [V :710-714]) read
   the requester, and (c) **all node PROCS run with `dude_obj` = the driver** (the O2
   scope; Cameron strips the actor who talked to him — Ch 10.3's fix).
4. **Scope duration:** the conversation is a synchronous block-and-pump inside
   `scriptsHandleRequests` (the barrier never returns to serverTick mid-conversation
   [V game_dialog.cc:2004-2027, DIALOG_STREAMING_PLAN "block-and-pump"]). One scope
   around the `dialogEnter` call therefore covers the entire conversation, including
   every `_gdProcessChoice` proc. ⚠ The pump SERVICES OTHER SESSIONS' verbs mid-dialog
   (`serverControlLine` from the pump body [V server_main.cc:217-221]) — those handlers
   open their own inner `ServerActorScope`. This is the one place scopes nest, and it is
   why Ch 7.2's class restores the PREVIOUS context actor rather than slot 0 — restoring
   slot 0 would silently re-anchor the rest of the conversation, node procs included,
   onto the host. That contract is now stated correctly at the class definition itself;
   it was originally specified there as "restores slot 0 deliberately" and repaired only
   here, so an implementer working from Ch 7.2 alone would have written the broken one.
   — Also note the A2 review item stands: non-dialog verbs mid-dialog act on a frozen
   world [V server_main.cc:186-192 comment]; the reviewed pump should gate them. That
   review is a dialog-track obligation, not this spec's.

## 11.3 Owner gating + lifecycle

- `dsay`/`dend` gate [V server_control.cc:490-514]: replace the claimant test with
  "the sending session's bound actor == the active dialog's driver":
  `serverControlActorForSession(sessionId) == gameDialogDriverActor()` where
  `gameDialogDriverActor()` is a new getter exposing the driver captured at
  conversation start (store it beside `gGameDialogSpeaker` in game_dialog.cc, set from
  the scope at `gameDialogEnter` entry; clear at exit). A spectator's `dsay` no-ops
  with a log line. ESC/`dend` from a spectator: also no-op (spectators cannot end the
  driver's conversation — the A3 client already suppresses sending
  [V client_net.h:154-161 spectatorGuard]; the server gate is defense-in-depth).
- Driver disconnect mid-dialog: the pump's liveness bail generalizes from "claimant
  gone" to "driver's session unbound" (`serverControlSessionForSlot(driverSlot)==0`)
  [V server_main.cc:210-230] → barrier bails → `dialogEnd` broadcast → world unfreezes.
- **Dialog initiation with N players:** ANY bound actor may initiate (the `talk` verb
  is per-session now). No first-actor privilege, no `setdriver` command — the earlier
  drafts' "only P1 initiates / setdriver" idea is DROPPED as unnecessary once the
  driver is the requester. While a dialog is active, a second session's `talk` is
  rejected by the barrier being singular: `scriptsRequestDialogAs` while
  `_gdialogActive()` → refuse + console "…is busy talking." [new guard in the talk
  fire; _gdialogActive is real on f2_server since A0 [V DIALOG_STREAMING_PLAN §A0]].
- **The world FREEZES for everyone during any dialog** (the barrier blocks serverTick —
  v1 cost already accepted in DIALOG_STREAMING_PLAN "block-and-pump, NOT a resumable
  machine"). Spectators watch the conversation; non-participants' verbs are serviced
  but act on a frozen world (see 11.2's review note). Resumable dialog = banked Stage 4
  of that plan.

## 11.4 Gate

Two viewers, two actors. P2 walks to the Temple story-teller, talks: P2's viewer gets
the editable window, P1's viewer shows the same node read-only and cannot select; P1's
`dsay` lines log as ignored. Node procs' effects (e.g. any inventory/gvar change the
conversation causes) land on P2. Driver disconnect mid-node ends the dialog on P1's
viewer. `check.sh` green (all changes server-side-gated / additive events).

--------------------------------------------------------------------------------
# Ch 12 — Barter ruling

**Out of scope for Demo 0.2.** Grounds [V BARTER_STREAMING_PLAN.md]: the barter VIEWER
track (B0-B3) is itself unbuilt (table objects have no netIds; `inventoryOpenTrade`
never entered on the viewer; no wire verbs); building N-player semantics on top of an
unbuilt single-player modal is ordering-backwards. Rulings this spec DOES make so the
barter track lands MP-ready:

1. Barter inherits the dialog model wholesale: driver = the conversation's driver
   (Ch 11.2's stored driver actor); spectators see nothing new in v1 (the barter window
   is NOT broadcast — unlike dialog nodes there is no spectator rendering requirement;
   plan B1's table-state events may later enable it).
2. `boffer/btake/bunoffer/bcommit/bdone` (plan B2) MUST be gated exactly like `dsay`:
   session's actor == driver. The plan's "claim-gated" wording predates per-session
   binding; update it when B2 lands.
3. The dude-side table (`_peon_table_obj`) and `inventoryOpenTrade(gDude…)` calls bind
   to the DRIVER through the Ch 11.2 scope (the barter drain runs inside the same
   conversation barrier [V game_dialog.cc:2029-2057 region per plan]).
4. Player↔player barter: NOT a thing in v1 (vanilla barter needs a talk_p_proc merchant
   script). Item transfer between players = drop/loot ground exchange, already
   serialized by the loot layer [V server_control.cc:926-933].

--------------------------------------------------------------------------------
# Ch 13 — Out-of-combat parity per player (movement, interaction, inventory, loot)

Mostly falls out of Ch 7; this chapter enumerates the per-subsystem residue.

- **Movement**: `mv` per actor (Ch 7.4). The stepped-walk engine keys walks by owner and
  epoch-guards script re-entrancy [V server_anim.cc:275-300, serverWalkEnqueue]; N
  concurrent walks are already supported (AI walks coexist with the dude's today).
  Two liveness/cost details to touch:
  - `serverWalkOwnerAlive`'s `owner == gDude` fast-path [V server_anim.cc:275-278] →
    `playerActorIs(owner)` (player actors are NO_REMOVE-stable, same guarantee).
  - The idle-deadline presentation-cost tally `owner != gDude` [V :405-415] →
    `!playerActorIs(owner)` (the comment there already flags it as "a v1 player-actor
    test").
- **Spatial procs on walk steps** (the Temple traps gap): when the per-step
  `scriptsExecSpatialProc` hook lands (hook point = the stepped-walk per-step loop
  [V AGENTS/p5-server-plan.md]), it must fire for EVERY mover — player actors included —
  with the Ch 10.4 spatial scope. Not an MP change per se; MP inherits it.
- **Inventory/equip/skills/useitem**: swap-only per Ch 7.4; per-viewer UI already reads
  the repointed gDude (Ch 5.6). Skill XP + messages credit the shared PC sheet (Ch 15).
- **Loot**: concurrent access to one container by two players is serialized by the
  single-threaded drain; `itemMove` refuses over-capacity faithfully
  [V server_control.cc:926-1025]. The loot-open gesture/animation
  (`_obj_use_container` via kInteractLoot [V :178-183]) is per-actor under the scope.
- **Interaction feedback misattribution** (console broadcast) — accepted wart, Ch 7.3.
- **Per-hex AP / aimed-shot 'N'/'B' / other banked viewer nits** — unchanged by MP
  [V AGENTS/p5-server-plan.md BANKED VISUAL NITS].

--------------------------------------------------------------------------------
# Ch 14 — Coupled map transitions and elevation

## 14.1 Facts (verified)

- Map transition: `mapSetTransition` latches; `mapHandleTransition` (beat tail) does
  `mapLoadById` then places THE DUDE at the transition tile + `mapSetElevation` +
  camera [V map.cc:1067-1145]. In-combat, the transition sets quit=1 to break combat
  first [V :1084].
- Map teardown spares NO_REMOVE objects — the dude survives; with Ch 4's Option A,
  EXTRA ACTORS SURVIVE TOO (same flags). `_map_place_dude_and_mouse` re-stands the dude
  and re-asserts NO_SAVE [V map.cc:1490-1508].
- Intra-map stairs/ladders with `destinationMap == 0` are a plain
  `objectSetLocation(user, tile, elevation)` [V proto_instance.cc:1540-1632] — which
  EMITS `objectMoved` (the §4 choke [V object.cc:1294-1300]) → EVENT_MOVE carries
  from/to elevation [V presenter_network.cc:271, client_net.cc:770-777]. ⚠ This
  CORRECTS the p5-plan memory's "(2) NO event fires on an intra-map elevation change" —
  a MOVE does fire; the true gaps are (1) `gElevation` is never assigned viewer-side and
  (2) [U] whether the viewer's decode/render handles a cross-elevation move gracefully
  (glide code assumes same-elevation adjacency; `clientAnimOnMove` receives both
  elevations [V client_net.cc:811]).

## 14.2 Coupled map transition (all players move together)

Change in `mapHandleTransition`'s normal branch [V map.cc:1113-1141]:

```c
if (gMapTransition.tile != -1 && gMapTransition.tile != 0
    && gMapHeader.index != MAP_MODOC_BEDNBREAKFAST && gMapHeader.index != MAP_THE_SQUAT_A
    && elevationIsValid(gMapTransition.elevation)) {
    objectSetLocation(gDude, gMapTransition.tile, gMapTransition.elevation, nullptr);
    mapSetElevation(gMapTransition.elevation);
    objectSetRotation(gDude, gMapTransition.rotation, nullptr);
    // NEW: bring every other player actor along, adjacent to the entry tile.
    if (serverLoopActive()) {
        for (int slot = 1; slot < playerActorCount(); slot++) {
            Object* p = playerActorAt(slot);
            int t = serverFindFreeTileNear(gDude->tile, gDude->elevation); // Ch 4.3 helper
            if (t == -1) t = gDude->tile; // degenerate fallback: co-locate
            objectSetLocation(p, t, gMapTransition.elevation, nullptr);
            objectSetRotation(p, gMapTransition.rotation, nullptr);
        }
    }
}
```

Also mirror in `_map_place_dude_and_mouse` [V map.cc:1490-1508]: the per-actor
"stand + fix fid + place-if-tile-1 + re-assert NO_SAVE" block loops the registry
(each extra actor needs `OBJECT_NO_SAVE` re-asserted and a STAND fid fix exactly like
the dude — the memcpy'd flags survive, but symmetry keeps the invariant explicit).

Mid-walk players: `mapLoadById`'s teardown clears animations wholesale; the stepped-walk
registry must drop all walks on map generation change — [U whether server_anim already
keys on `mapGetLoadGeneration`; VERIFY and, if not, clear `gServerWalks` in the
transition path]. Pending interaction latches: drop ALL on transition
(`gPendingBySession.clear()` — the target id+pid re-validation would drop them anyway,
one beat later [V server_control.cc:325-334]; clearing is cleaner).

Wire side: nothing new — `mapTransition` + the generation-keyed rebaseline broadcast
already rebuild every viewer [V server_loop.cc:325-341], the blob carries all actors
(Ch 5.3), the roster re-emit follows the baseline (Ch 5.4), each viewer rebinds + its
camera centers on ITS actor (Ch 5.6 step 6).

**Who triggers transitions — OWNER RULING 2026-07-19, supersedes the symmetric reading
below:** ONLY THE HOST ACTOR (registry slot 0) triggers a map transition. An extra
player walking onto an exit grid does NOTHING — no transition, no message, it is inert
for them. When the host takes the exit, EVERY actor is carried to the new map (the
coupled placement loop of §14.2 is unchanged; that part was never in question).

Rationale (owner): the party travels as one, and letting any body drag the whole group
between maps is both a griefing surface and a source of transitions nobody consented to.
Asymmetry here is a FEATURE, not a v1 simplification to revisit.

Implementation seam: the trigger is a script (`op_load_map` in an exit grid's
spatial/use proc → `mapSetTransition` [V SCRIPT_OPCODE_MAP 0x80E4 + cross-cut note]), so
the gate belongs where the acting actor is known — the Ch 10.4 spatial scope. Do NOT gate
inside `mapSetTransition` itself (by then the caller is anonymous, and legitimate
script-driven transitions with no walking actor would be caught too). ⚠ This makes the
spatial-proc-on-walk gap a prerequisite for the RULING, not just for M6 — until per-step
`scriptsExecSpatialProc` fires there is no scoped actor to test.

►► **Express it as a POLICY PREDICATE, not a slot test** — same shape as the death seam
(Ch 12): one function, `playerActorMayTransit(Object* actor)`, whose v1 body is
`return actor == playerActorAt(0);` and whose CALLERS never mention slot 0. "Slot 0 is
the one who travels" is true only of the world we can run today (one map, one group,
everyone co-resident). The moment there are multiple live maps, per-map groups, or a
client hot-joining a map the host is not on, the question stops being "is this the host"
and becomes "is this actor the transit authority for ITS group/context" — which is a new
body for the same predicate, not a hunt through call sites. The rule generalizes to every
"P1 is special" temptation in this spec: name the ROLE, put it behind one predicate, and
let v1 answer it with a slot comparison. [[mp-actor-architecture-principle]]: a player is
a first-class actor; leash/party/host-primacy are removable BEHAVIORS, never identity.

Prior text (now superseded, kept so the change of mind is legible): "any player walking
onto an exit grid can trigger the exit; everyone is carried — that matches the owner's B4
('P1 drives… P2 moves with it' — and symmetrically)."

⚠ OPEN, do not assume it follows: this ruling is about MAP transitions. Same-map
ELEVATION is a different mechanism (no teardown, all floors resident and simulated) and
the locked leash decision GRANTS same-map any-elevation free-roam (§14.3) — i.e. today an
extra taking stairs/an elevator is legal and just puts them on another floor, with each
viewer's camera following its own actor. Whether elevators should ALSO be host-only +
coupled is unresolved; ask before implementing either way.

In-combat: vanilla defers via quit=1 combat-break [V map.cc:1084]; unchanged.

## 14.3 Intra-map elevation follow (per-viewer camera)

Server: nothing — stairs already move the actor and the MOVE event carries elevation
(§14.1). Player actors do NOT follow each other across elevations (same-map
any-elevation free-roam is GRANTED per the locked leash decision
[V TEMPLE_DEMO_ROADMAP.md "P2 leash decision"]). Combat remains single-elevation
(entry guard [V combat.cc:3940-3942]).

### 14.3a ⚠ What "two players on two floors" actually breaks (audited 2026-07-19)

Answering the owner's questions directly, from the code rather than from the design:

**"Will NPCs find you through floors?" — No.** Every combat/AI roster is
elevation-filtered: the fight's roster is `objectListCreate(-1, _combat_elev, …)`
[V combat.cc:2544], AI ground-item search [V combat_ai.cc:2195], the critter scans in
[V critter.cc:1336] and [V combat_drain.cc:770,805] likewise. Nothing targets or paths
across a floor boundary. The ONE cross-floor critter movement in the engine is party
members, who are TELEPORTED to the dude's elevation by `_partyMemberSyncPosition`
[V party_member.cc:797-820, called from map.cc:306] — party-only, and party is a
removable v1 behavior.

**But they filter on the WRONG elevation.** Those lists read `gElevation` — the CAMERA
selection, not the acting critter's floor. With one player the two are always equal, so
this is invisible today; with players on two floors `gElevation` is whichever floor was
selected last, and AI on the other floor builds its world-view from the wrong list. Same
bug CLASS as the worldmap one (a client/UI concept read as sim geometry
[[worldmap-streaming-track]]). Fix direction: these take the elevation from the SUBJECT
(`critter->elevation`), and `_combat_elev` becomes "the elevation of THIS fight" rather
than `= gElevation` [V combat.cc:2538]. Cheap, mechanical, and it must land before any
two-floor play — it is not part of the combat refactor below.

**"Two combats on two floors?" — cannot happen today; it is the one real wall.** The
combat context is a set of file-statics (`_combat_list`, `_list_total`, `_combat_elev`,
`gCombatState` [V combat.cc:151,1887,1909]) — exactly ONE fight exists process-wide. The
resumable session is likewise a singleton and says so out loud: a second `_combat()` while
one runs is ignored with "resumable combat — ignoring re-entrant _combat() (session
already active)" [V combat.cc:3948-3953]. So a fight starting on floor 2 during a fight on
floor 1 does not run in parallel — it is DROPPED. Per the elevation architecture finding,
making this concurrent is a BOUNDED refactor (per-context struct + scoping the freeze),
tractable because the resumable state machine already exists — but it is a real project,
not a v1 line.

**The nearer problem is the FREEZE, not the second fight.** One fight freezes the WHOLE
world: out-of-combat stepped walks are gated `!isInCombat()` [V server_anim.cc:576,651],
and the entry guard needs attacker or defender on `gElevation` [V combat.cc:3940-3942].
So P1 fighting on floor 1 leaves P2 on floor 2 frozen in place, unable to join (roster is
elevation-scoped) and unable to walk away — with no indication why. That lands the moment
free-roam elevation and combat meet, ahead of any concurrency work, and it is the thing to
decide policy on first: freeze only the fight's elevation, or accept the global freeze and
tell P2 what is happening.

**Also gDude-keyed, so extras get none of it:** the roof reveal, `gEgg` follow, camera
centering and the mid-combat "changed level → quit" all hang off `obj == gDude`
[V object.cc:1320-1405]. On the server gDude is the HOST actor, so an extra changing
elevation silently skips all of it. Per the UI-driving corollary these must resolve
through the per-client gDude ROLE, not the singleton
[[mp-actor-architecture-principle]].

Viewer (client_net.cc `onMove` [V :770-846]): after `objectSetLocation`, add:

```c
if (obj == gDude && toElev != gElevation) {
    mapSetElevation(toElev);              // camera follows MY actor only
    tileSetCenter(obj->tile, TILE_SET_CENTER_REFRESH_WINDOW);
}
```

Also in the rebind path (Ch 5.6 step 6) for the post-blob case. Other actors' cross-
elevation moves need no camera action (they simply vanish from this floor's render —
correct). ⚠ [U] the glide layer's behavior on a cross-elevation hop — force the snap
path (`durMs=0` semantics) when `fromElev != toElev` (one-line guard in
`clientAnimOnMove` if not already present).

**Temple relevance:** whether artemple.map is multi-elevation is STILL UNVERIFIED
(owner-disputed [V TEMPLE_DEMO_ROADMAP.md warning]) — the elevation-follow work is
specified regardless; its Temple-criticality is [OPEN-Q #6] until someone loads the map
and looks.

--------------------------------------------------------------------------------
# Ch 15 — XP, loot, and progression attribution ruling

**Facts [V]:** kill XP accrues to `_combat_exps` when the victim's killer is the dude OR
dude-team [V combat.cc:5520-5539], and is paid at combat end via
`pcAddExperience` — a GLOBAL PC pool with no critter argument
[V combat.cc:2832-2871, stat.cc:778-786]. Skill-use XP: `pcAddExperience` on success
[V skill.cc:543]. Script XP: `opGiveExpPoints → pcAddExperience`
[V interpreter_extra.cc:464-472]. Kill counts: global `killsIncByType`
[V combat.cc:5536, critter.cc:703]. Level-ups mutate the one PC sheet.

**RULING (v1): shared pool, host sheet.** All XP lands on the single PC record
regardless of which player earned it; extras never level (their stats are the shared
premade proto, Ch 4.3). Consequences: P2's lockpick successes "train" P1's sheet — and
since extras READ the proto, not the PC sheet, P1's viewer sees the benefit, P2's play
does not change. Accepted and disclosed; per-actor progression = post-v1 (needs
per-actor PC-data blocks — same banked item as the join-blob PC-data gap
[V COMBAT_CLIENT_DESIGN.md §5 risk 4]).

**Friendly-kill XP quirk (documented):** if P1 kills P2 (FF on), the killer is
dude/dude-team and the victim is `a1 != gDude` → the XP branch runs with
`critterGetExp(P2)` = the dude proto's experience field [V combat.cc:5525-5539] —
value unknown [U, likely 0]. Verify once; if nonzero, guard the XP branch with
`!playerActorIs(a1)` (one line, harmless).

**Loot:** free-for-all by design (shared containers already serialize, Ch 13); corpse
loot of a dead PLAYER is allowed (it is a corpse container) — this is the mechanic that
makes death recoverable pre-policy (your partner carries your gear to you… to your
successor body, once policy exists). No per-player ownership tags in v1.

--------------------------------------------------------------------------------
# Ch 16 — Save/load ruling

**Out of scope for Demo 0.2** — the dedicated server currently has no save/load at all
(the serve loop runs a fresh boot each time; `serverShutdown` is minimal
[V server_boot.cc:274-281]). What this spec contributes so N-actor save is cheap later:
the join blob (map body + per-actor appendix, Ch 5.3) IS the serialized form of an
N-player session minus (a) game GVARs (already a known wire gap
[V AGENTS/p5-server-plan.md KNOWN GAPS]), (b) the binding table (sessionIds are
transport-ephemeral and must NOT be persisted; persist slot count only), (c) queue/
combat state (vanilla save policy: no mid-combat save — keep it). A future
`F2_SERVER_SAVE` = write the blob bytes + gvars to disk; load = boot + apply like a
joining client. [OPEN-Q #11 records this as the designated approach, unbuilt.]

--------------------------------------------------------------------------------
# Ch 17 — Staging plan

Rules of the road: every stage ends `scripts/check.sh` → "ALL GATES PASS" (never run
concurrently with a live viewer; `pkill -x f2_server; pkill -x fallout2-ce` first).
`F2_SERVER_PLAYERS` unset/1 must be byte-identical at EVERY stage boundary — that plus
the goldens is the rollback story: each stage is revertible by itself, and the feature
is dark until the env var is set. Live verification follows
`AGENTS/visual-verification-protocol.md` (hand the owner a recipe + targeted questions;
no screenshot floods). Adversarial review is MANDATORY where marked (object-lifetime /
trust-boundary / no-headless-oracle changes, per the standing cadence rule).

### M0 — Server survival hardening (Ch 9) — DO FIRST, independently shippable
Files: `src/critter.cc` (S1 fork at :911-915), `src/scripts.cc` (S2 fork in
`gameTimeAddTicks`), `src/interpreter_extra.cc` (S3 fork in `opMetarule`
METARULE_SIGNAL_END_GAME), new `src/server_players.{h,cc}` (registry with default
`{gDude}` + no-op `playerActorDied`), `src/server_boot.cc` (`playerActorRegister(gDude)`),
CMakeLists (new TU into f2_core).
Ordering: registry TU first (S1's fork calls `playerActorIs`/`playerActorDied`).
Gate: Ch 9.6's live dude-death run + check.sh. Review: YES (terminal-semantics class).
Rollback: revert the three forks; registry TU is inert.

### M1 — Registry + spawn + wire identity (Ch 4, Ch 5.1-5.4)
Files: `src/server_boot.cc` (`serverSpawnExtraActors` + free-tile helper),
`src/object.cc` (`objectAssignAllNetIds` registry walk; `_obj_save_player_actor`),
`src/object_delta.cc` (`objectIsSyncable`), `src/server_loop.cc` (baseline loop; blob
header actorCount emit), `src/map.cc` (`mapSaveToStream` appendix),
`src/presenter.h` + `src/presenter_network.cc` (playerRoster event 34),
`src/state_dump.cc` (dump extras like the dude).
Gate A (regression): `F2_SERVER_PLAYERS` unset → check.sh byte-identical.
Gate B (feature): `F2_SERVER_PLAYERS=2 scripts/viewer_live.sh` → ONE viewer connects,
SEES two premade critters standing adjacent; `nc` the debug port for a state sanity dump
if wired. Rollback: env-gated; revert cleanly.
Review: YES for the blob/netId-walk edits (silent-corruption class — the §C domain
alignment is the most fragile invariant in the wire).

### M2 — Sessions, claims, verb binding, viewer rebind (Ch 5.5-5.7, 6, 7)
Files: `src/server_net.cc`/`.h` + `src/presenter_network.cc` + new `src/wire_defs.h`
(preamble v2 + sessionId; begin() split), `src/server_control.cc` (binding table, drain,
dispatch, per-session latch, ServerActorScope adoption), `src/server_players.{h,cc}`
(scope class, scriptContextDude stub returning gDude for now), `src/client_net.cc`
(preamble parse, roster decode, applyBlob actor sections + gDude restore/rebind,
`gClientHostDude`), `src/main.cc` (no change expected — claim already sent
[V main.cc:1082]).
Gate: TWO viewers: each claims; roster shows two rows; P1 and P2 walk INDEPENDENTLY;
P2 opens inventory and sees Narg-kit on ITS actor; equip on P2 re-fids P2 on both
viewers; disconnect P2 → roster row unbinds → reconnect → `claim 1` resumes the same
body. Free-roam only (combat still host-only — cattack from P2 is dropped by the
not-your-turn barrier; acceptable mid-stage).
Rollback: viewer changes are version-2-preamble-gated; server reverts cleanly.
Review: YES (trust boundary + lifetime of rebound gDude on the viewer — the §5.3
sequencing hazard).

### M3 — Combat N-player (Ch 8)
⚠ **ORDERING:** fix the 62s COMBAT-STATE DESYNC before starting M3. It lives in exactly
the machine this stage generalizes (the player-turn barrier / COMBAT_EXIT path), the
`[cbtstate]` trace is already committed (ceafe9b), and it needs only one live repro to
pin the site [V AGENTS/p5-server-plan.md open bugs]. Debugging it through a
slot-generalized barrier with two viewers attached is strictly harder than debugging it
now, solo.

Files: `src/combat.cc` (sequence placement + partition, session machine slot plumbing,
should_end/turn-end generalizations, turnStart isPlayer), `src/combat_intent.{h,cc}`
(actorSlot + ForSlot API), `src/combat_drain.cc` (pump per slot, resolve-target per
actor, hit-mode per actor), `src/server_control.cc` (combat verbs pass slot; cstart
slot), `src/server_loop.cc` (slot-bound query bridge + re-assert isPlayer).
Gate: P1 cstarts a fight with the ants → P2 is IN the roster round 1 (gets a turn
without having acted); both attack/move/end-turn on their own turns; killing all
hostiles exits combat for both; kill P2's actor with FF on → fight CONTINUES for P1;
kill both → combat ends, server stays up (M0's guarantee); idle P2 (no input) → its
turn auto-ends at the idle budget; disconnected P2 → its turns end instantly.
Friendly-fire toggle: `F2_FRIENDLY_FIRE=0` → P1's crosshair attack on P2 is refused
with a console line and does NOT end the turn.
Rollback: all `playerActorCount()>1`-degenerate; goldens pin N==1.
Review: YES (combat machine; no headless oracle for the N>1 half — eyeball protocol).

### M4 — Death seam completion + dead-actor verb policy (Ch 9.4-9.5)
Files: `src/combat.cc` (any-live-player rules), `src/server_control.cc` (dead-actor verb
refusal), `src/server_players.cc` (seam body stays no-op).
Gate: P2 dies mid-fight → P1 finishes the fight; dead P2's clicks are refused
server-side with logs; P1 loots P2's corpse; both die → combat ends, world idles,
server alive, both viewers still streaming.
Review: fold into M3's review if landed together.

### M5 — Coupled transitions + elevation follow (Ch 14)
Files: `src/map.cc` (transition loop + place loop), `src/client_net.cc` (onMove
elevation follow + rebind camera), `src/server_control.cc`/`src/server_anim.cc`
(latch/walk clears on transition if not generation-keyed already — verify first).
Gate: P1 walks the exit grid → both viewers transition together, P2 standing adjacent
to P1 on the new map, roster/bindings intact (netIds re-minted, slots stable); **P2
walks the exit grid → NOTHING HAPPENS on either viewer** (the host-only trigger ruling,
§14.2 — and its prerequisite: the exit-grid spatial proc must actually be firing per
step, or this passes vacuously for the wrong reason); P2 takes the stairs (if artemple
has any — else test on a known multi-elevation map, e.g. vault13 [U structure; pick
empirically]) → P2's viewer follows floors, P1's does not.
Also assert the actor-leak tripwire ("client_net: ACTOR LEAK") stays silent across the
transition: a transition is a rebaseline, which is precisely when leaked bodies breed.
Review: standard.

### M6 — Dialog driver + context-dude (O2+O3) + spatial scope (Ch 10.4, 11)
⚠ **PREREQUISITE, not owned by this spec:** the spatial-proc-on-walk gap
(`scriptsExecSpatialProc` is never fired by server stepped-walks — hook point = the
per-step loop in `server_anim.cc` [V AGENTS/p5-server-plan.md open bugs]). Until it
lands, the Ch 10.4 spatial scope has nothing to bracket and acceptance criterion 8's
trap clause is untestable. Land the gap fix (solo, N==1) BEFORE M6 or accept dropping
traps from the demo.

Files: `src/scripts.cc` (RequestDialogAs + handler scope + spatial scope + the
registry-looping rad midnight sweep, Ch 10.6), `src/game_dialog.cc` (driver getter),
`src/server_control.cc` (dsay/dend owner gate, talk busy-guard),
`src/interpreter_extra.cc` (opGetDude → `scriptContextDude(program)`),
`src/server_players.{h,cc}` (scope stack discipline; `scriptContextDude` real body =
scope → nearest-registered-actor-to-`scriptGetSelf` → gDude; a
`playerActorNearestTo(Object* origin)` helper),
`src/critter.cc` (poison + radiation gate relax at :332/:381/:417/:490 per OPEN-Q #3 —
NOT the cosmetic `== gDude` sites; see Ch 10.6's table).
Gate: Ch 11.4's dialog gate + a trap check once spatial procs fire (P2 steps a Temple
plate → P2 takes the damage) + a poison check (P2 stung by a Temple scorpion actually
accrues poison). denbus1_dialog golden + all gates byte-identical (registry==1 → step 2
of `scriptContextDude` is unreachable → ≡ gDude).
Review: MANDATORY (interpreter-visible change + the nested-scope discipline + O3's
geometry-dependent resolution).

Estimated effort mirrors the owner's earlier sizing: M0 ~0.5 session, M1-M2 ~2-3
sessions, M3-M4 ~1.5, M5 ~1, M6 ~1. (Estimates only; gates, not clocks, decide.)

--------------------------------------------------------------------------------
# Ch 18 — Consolidated acceptance criteria (Demo 0.2)

1. `F2_SERVER_PLAYERS=2 scripts/viewer_live.sh` (+ a second `fallout2-ce` with
   `F2_CLIENT_CONNECT`) → two viewers, two distinct premade actors, roster on both.
2. Independent free-roam: walk/run, action menu, inventory/equip/drop, skills, useitem,
   loot — each viewer on ITS actor, results visible on both.
3. P1 starts combat → P2 is a round-1 combatant; per-viewer myTurn correct; both play
   full turns; combat ends for both; turn barrier honors disconnect/idle per actor.
4. Friendly-fire toggle works as specced (direct attacks refused when off).
5. A player death NEVER terminates or endgames the server (M0 gate re-run at demo).
   Dead player spectates own corpse; partner can finish the fight and loot the corpse.
6. Dialog: either player initiates; driver edits, other spectates read-only; node procs
   act on the driver; driver disconnect ends cleanly.
7. Map exit carries all players together; per-viewer elevation follows own actor.
8. Temple of Trials passable start-to-exit with both players participating (combat,
   traps if spatial procs landed, C4 door, Cameron via the driver, exit to arvillag).
9. `scripts/check.sh` → ALL GATES PASS with the feature dark (unset) at every stage and
   at final; no golden re-bless attributable to MP.
10. ~~`bugs/008-multiplayer-coop.md` deleted; this doc updated to Status: BUILDING~~ —
    DONE at M0. Keep the stage status in the header current as each stage lands.

--------------------------------------------------------------------------------
# Ch 19 — Open-questions register

| # | Question | Blocking? | Default if unanswered |
|---|---|---|---|
| 1 | Ch 10.4: sign off O2+O1 (context-dude + host anchor); define the trigger for O3 (nearest-player) | M6 | ~~Ship O2~~ **OWNER RULING 2026-07-19: adopt O3 (nearest-player) from the start.** Implement by making `opGetDude` return the registered player nearest to `scriptGetSelf(program)` when there is no scoped/interacting actor (verified: opGetDude receives `program`, scriptGetSelf takes exactly that [V interpreter_extra.cc:1124, scripts.cc:574]). Because the heartbeat idiom mints ONE pointer via opGetDude and keys both the query and the effect off it, hijacking the single mint point is sufficient — no need to touch tile_distance/effect ops. **Accepted limitation:** "nearest," so a zone that should hit ALL players present hits only the closest per proc tick (the script has no loop to hit all); as players move, the target shifts. Faithful "all players" needs the script itself to loop and is out of v1 scope. Caveat: scripts that cache dude_obj across beats freeze on "nearest at cache time." **Rejected alternative — per-tick gDude rotation** (gDude=P1 on beat N, P2 on N+1, …): fails because the engine ALREADY round-robins critter scripts one-per-beat (`_script_chk_critters`, `_count_++ % scriptsCount` [V scripts.cc:707-714]). A second rotation over gDude interferes with the first: a given NPC only fires every `scriptsCount` beats, so it sees a FIXED phase of the dude rotation → correctness becomes `gcd(critterCount, playerCount)`-dependent (even critter count + 2 players → half the NPCs never see P2). Nearest sidesteps this by resolving from geometry when each proc fires. Rotation remains viable NARROWLY, at a single "must reach every player over time" call site, never as the global policy. Cadence for reference: one critter proc per beat (~100ms); full sweep = scriptsCount beats. |
| 2 | "build 304" in the old drafts — meaning unknown (premade art#? gcd?). `_proto_dude_init("premade\\combat.gcd")` is what the server actually loads [V server_boot.cc:246-251] | No | Treat combat.gcd premade as THE template; drop the phrase |
| 3 | Relax poison (and rad?) dude-gates to playerActorIs for player actors (Ch 10.6) | M3-ish (Temple scorpions) | **OWNER RULING 2026-07-19: relax BOTH poison and radiation.** Swap identity for membership (`!= gDude` → `!playerActorIs(...)`). ⚠ **CORRECTED 2026-07-19:** the ruling originally cited critter.cc:238/480/578/616/748 — those are the `== gDude` COSMETIC branches (name, HUD, message text, kill-type gender), NOT the gates. The real gates are the `!= gDude` reject returns at **critter.cc:332, 381, 417, 490**, plus the host-only midnight sweep `_critter_check_rads(gDude)` [V scripts.cc:426]. Full table + the shared-proto RADIATED caveat: Ch 10.6. Low risk: per-object state already exists; the gates only decide who receives effects. No "nearest" ambiguity here, unlike #1. |
| 4 | Does the AI noncom partition delay an idle P2's first turn despite §8.2's placement? (verify at M3 gate) | M3 | The placement forces _list_com membership; verify live |
| 5 | Egg/transparency follow on a repointed viewer gDude (Ch 5.6) — which site keys it? | M2 polish | Verify visually; fix the one site if it sticks to host |
| 6 | artemple.map: multi-elevation or not (owner-disputed, never verified) | M5 scope | Spec ships elevation-follow regardless |
| 7 | Wire version-2 compatibility with version-1 peers | No | None — server+viewer ship together |
| 8 | Reconnect authentication (orphan claim is first-come) | No (dev server) | Accept; bank a claim-token |
| 9 | Friendly-fire OFF for AoE/burst extras (needs damage-side surgery) | No | FF-off covers direct attacks only |
| 10 | 13-year timeout / SIGNAL_END_GAME server policy beyond suppress+log (freeplay hook?) | No | Suppress+log |
| 11 | N-actor save/load (Ch 16 approach sign-off) | No | Banked as specced |
| 12 | Per-client console attribution (feedback broadcast wart, Ch 7.3) | No | Accept; needs per-client framing epoch |
| 13 | `_gcsd` initial-attack replay guard (Ch 8.3-3): confirm csd.attacker comparison is the right condition on a script-initiated fight | M3 review | Guard as specced |

--------------------------------------------------------------------------------
# Ch 20 — Risk register

| Sev | Risk | Mitigation in this spec |
|---|---|---|
| HIGH | netId/blob domain misalignment after the multi-actor walk+appendix (silently corrupts EVERY delta — the "stream lies" failure class, twice-bitten historically) | Ch 5.1/5.3 keep the single-walk-both-sides invariant + slot-ordered appendix; SNAPSHOT_OBJECT tripwire counts stay the oracle [V client_net.cc:592-597]; M1 adversarial review; midjoin gate |
| HIGH | Viewer gDude repoint lifetime (memcpy into gDude, modal statics, DESTROY guard) — crash class | Ch 5.3 hazard box: restore-before-mapLoad, gClientHostDude, guard extension; blob-defer machinery already shields modals [V client_net.cc:617-629]; M2 review |
| HIGH | Passive-sim asymmetries misread as bugs (P2 unnoticed by NPCs, poison immunity, host-anchored procs) | Ch 10 is the disclosure document; demo notes must link it; OPEN-Q #1/#3 force explicit owner acceptance |
| HIGH | Combat machine regressions from the slot generalization (idle-timer, 0x08 handshake, _gcsd, ending rules — subtle, no headless N>1 oracle) | Degenerate-to-N==1 construction + goldens; M3 eyeball gate list is deliberately adversarial (disconnect/idle/death mid-fight); mandatory review |
| MED | Scope-restore discipline (Ch 11.2's nesting fix) — a mis-restored gDude leaks a foreign actor into the passive sim for the rest of a beat | Stack-discipline scope + depth assert; grep-gate: no bare `gDude =` assignments outside object.cc/proto.cc/scope class |
| MED | Preamble/begin() split breaks the boot-path join framing | Shared wire_defs.h constant; netsocket/midjoin gates |
| MED | Transition carry: walk registry / latches / combat state straddling mapLoadById | Ch 14.2 clears; combat already broken by quit=1; verify generation-keying [U] |
| MED | Shared dude-proto mutation (set_critter_stat on any player hits all) | Documented; rare in Temple content [U]; bank per-actor protos post-v1 |
| LOW | Roster/rebind race (roster frame vs blob frame ordering) | Rebind runs in BOTH decoders (Ch 5.6 step 6), idempotent |
| LOW | Console misattribution, shared name, host-anchored flee direction | Documented warts (Ch 7.3, 4.3, 10.5) |

--------------------------------------------------------------------------------
# Appendix A — gDude reference census (grep -c, 2026-07-19) + classification

Classes: **UI/CAM** (client presentation/camera — per-viewer-correct after the Ch 5.6
repoint), **VERB** (server verb layer — Ch 7 scope/parameter), **SIM-CORE** (combat/
engine authority — Ch 8/9 generalizations), **SCRIPT** (opcode layer — Ch 10),
**PC-GLOBAL** (progression/dude-state singletons — Ch 15/10.6), **BOOT/SAVE**
(lifecycle plumbing — Ch 4/5).

| File | refs | Dominant classes (notes) |
|---|---|---|
| combat.cc | 119 | SIM-CORE (session machine, sequence, end rules, perk/trait gates `attacker==gDude` — perk gates stay host-only for extras, premade-parity moot) + UI (hud calls) |
| character_editor.cc | 116 | UI/CAM (client-only screens) |
| command.cc | 72 | debug port (acts as host; fine) |
| client_net.cc | 55 | UI/CAM + binding (Ch 5.6) |
| object.cc | 52 | BOOT/SAVE (dude create/save/load, netId walk, egg-follow, viewer guards) |
| server_control.cc | 47 | VERB (Ch 7 — all become actor) |
| proto_instance.cc | 44 | VERB-callee tree (scope-resolved) + a few SCRIPT-adjacent (item sid source) |
| game_mouse.cc | 41 | UI/CAM |
| interface.cc | 39 | UI/CAM |
| actions.cc | 39 | mixed: VERB-callee + SIM-CORE (attack paths) + UI |
| item.cc | 37 | VERB-callee (use/loot sid setups [V :3562,:3646] scope-resolved) |
| critter.cc | 37 | PC-GLOBAL (name, poison/rad gates) + SIM-CORE (critterKill S1) |
| interpreter_extra.cc | 36 | SCRIPT (opGetDude + dude-coupled handlers — Appendix C) |
| proto.cc | 33 | BOOT/SAVE (gDudeProto plumbing) |
| skill.cc | 32 | PC-GLOBAL (PC skill branches, XP) — scope makes messages right; stores stay shared |
| scripts.cc | 28 | SIM-CORE (dude script, elevator, rads sweep) + Ch 11 dialog request |
| inventory_ui.cc | 27 | UI/CAM (client screens; barter drain later) |
| stat.cc | 26 | PC-GLOBAL (PC stat branches) |
| combat_drain.cc | 25 | VERB/SIM-CORE (Ch 8.4 — all become turn actor) |
| worldmap.cc | 23 | out of scope (no worldmap in demo) |
| main.cc | 23 | UI/CAM (viewer loop reads repointed gDude) |
| map.cc | 21 | SIM-CORE (transition/place — Ch 14 loops) |
| game_dialog.cc | 18 | Ch 11 (driver scope makes them right) |
| combat_ai.cc | 17 | SIM-CORE (Ch 10.5 table — no changes) |
| state_dump.cc | 16 | BOOT/SAVE (extend like baseline) |
| animation.cc | 15 | UI/CAM + engine (dude-move-prep reserved-slot idiom [U detail]) |
| character_selector.cc | 14 | client-only |
| party_member.cc | 10 | party system — untouched (not identity) |
| object_render.cc | 9 | UI/CAM (egg/translucency) |
| pipboy.cc | 8 | client-only |
| character_transaction.cc | 7 | client-only |
| server_loop.cc | 6 | Ch 5.2/8.5 emit sites |
| game_ui.cc | 6 | client-only |
| inventory.cc | 5 | VERB-callee (wield fid paths) |
| trait.cc / sfall_opcodes.cc / server_boot.cc / pres_record.cc / automap.cc | 4 ea | misc: traits PC-GLOBAL; boot Ch 4; others UI |
| server_anim.cc | 3 | Ch 13 (two named sites) |
| queue.cc | 3 | explosion-owner fallback [V queue.cc:480-487] — acts as host; ACCEPT |
| perk.cc | 3 | PC-GLOBAL (party-member perk resolution) |
| loadsave.cc | 3 | client-only |
| ≤2 each: presenter.h, game_sound.cc, combat_ui.cc, client_net.h, client_dialog.cc, tile.cc, skilldex.cc | | UI/CAM |

--------------------------------------------------------------------------------
# Appendix B — Server-terminal site census (complete grep of `_game_user_wants_to_quit` writes, 2026-07-19)

| Site | Value | Server-reachable? | Disposition |
|---|---|---|---|
| critter.cc:914 (critterKill, dude dead) | 2 | YES — every player death | **Ch 9.2-S1: playerActorDied fork** |
| scripts.cc:373 (gameTimeAddTicks, year≥13) | 2 | YES — sim clock | **S2: suppress+log fork** |
| interpreter_extra.cc:3208 (METARULE_SIGNAL_END_GAME) | 2 | YES — any script | **S3: suppress+log fork** |
| endgame.cc:257 (endgamePlayMovie, serverLoopActive) | 2 | YES — endgame opcodes | KEEP (faithful terminal ending; not death) |
| endgame.cc:316 (continue-playing "No") | 2 | no (client modal; :257 returns first) | keep |
| interpreter_extra.cc:4775 (opTerminateCombat) | 1 | YES | KEEP — combat-break, consumed by teardown [V combat.cc:3453-3455] |
| map.cc:1084 (mapSetTransition in combat) | 1 | YES | KEEP — combat-break |
| main.cc:403 (mainLoop dude-death poll) | 2 | no (client loop) | keep |
| game_ui.cc:685 (quit confirm) | 2 | no | keep |
| loadsave.cc:939 / :1256 (load failure) | 2 | no | keep |
| game_ui.cc:852-857 (save/restore around pause) | n/a | no | keep |
| server_boot.cc:223 / main.cc:319,:347,:1084 / game_lifecycle.cc:397 (resets to 0) | 0 | — | keep |
| Readers that must stay `gameTerminalQuitRequested()`: server_main.cc:200,:308 | — | — | already correct [V] |

--------------------------------------------------------------------------------
# Appendix C — Opcode classification for Chapter 10

Spine = SCRIPT_OPCODE_MAP.md (all 181 game opcodes; the 78 core-VM opcodes are
player-blind). Classification here is by PLAYER-COUPLING, complementing that doc's
headless classes. Confidence: rows marked ✓ were verified in-handler this pass; the
rest are classified from the opcode map's system column + handler-name semantics and
were NOT individually re-read — treat unmarked rows as high-confidence-inferred, and
the final "unclassified residue" honestly lists what nobody checked.

**C.1 The producer (everything flows from here)**
- `0x80BF dude_obj` / opGetDude ✓ [V interpreter_extra.cc:1122-1127] — THE chokepoint;
  Ch 10.4 O2 changes exactly this.
- `0x80BD source_obj` ✓ / `0x80BE target_obj` ✓ — per-proc, set at the Ch 10.2 sites;
  already per-actor-correct where the engine sets them.

**C.2 IMPLICIT-PC-GLOBAL (ignore/bypass their object arg; hit the one PC store — the
swap does NOT localize these; Ch 15/10.6):**
`op_give_exp_points` ✓ (pcAddExperience, no critter arg [V :464-472]);
`op_critter_mod_skill` ✓ (dude-only, errors otherwise [V :4273-4310]);
`op_using_skill` ✓ (sneak = dude-state [V :580-596]); `op_get_pc_stat`;
`op_poison` ✓ / `op_radiation_inc/dec` ✓ (object arg present but critter.cc gates to
dude [V critter.cc:332,417]); `op_gsay_*`/`op_giq_option` ✓ (giq gates options on
gDude INT+SmoothTalker [V :3862-3864] — becomes the DRIVER under the Ch 11 scope);
`op_endgame_slideshow/movie` (terminal, Ch 9-S4); `METARULE_SIGNAL_END_GAME` ✓ (S3).

**C.3 SUBJECT-SCOPED (object-arg mutators/readers; correct for ANY critter today —
under O2 they act on whichever object the script passes, including a context-resolved
dude_obj):** the critter/stat, inventory/item, map/tile, anim/movement families of the
opcode map — e.g. `critter_heal`, `kill_critter(_type)`, `critter_injure`,
`critter_damage`, `move_to` ✓ (dude branch adds camera/scroll chrome
[V :805-835] — harmless server-side), `create/destroy_object`, `add/rm_obj_from_inven`,
`wield_obj_critter` ✓ (dude branch = armor AC + hud [V :1690-1736 region]),
`use_obj(_on_obj)`, `obj_can_see/hear_obj`, `tile_distance(_objs)`, `anim`/`reg_anim_*`.
No MP work beyond O2.

**C.4 WORLD-ONCE (must fire exactly once per proc regardless of player count — the
anti-broadcast argument, Ch 10.4-O4):** all gvar/mvar/lvar writes, `obj_lock/unlock/
open/close/jam`, `load_map`/`set_exit_grids`/`override_map_start` ✓ (relocates gDude
[V :523-560] — under a scope, relocates the context actor; on map procs = host —
review when first hit live), `party_add/remove`, `add/rm_timer_event`,
`game_time_advance`, `endgame_*`, `terminate_combat` ✓.

**C.5 Unclassified residue (nobody read these this pass — verify before relying):**
`op_metarule`'s ~20 sub-rules beyond SIGNAL_END_GAME/FIRST_RUN/ELEVATOR
[V :3195-3215 partial]; `op_metarule3` sub-rules; `op_inven_cmds`; sfall opcode set
(`sfall_opcodes.cc`, 4 gDude refs); the exact dude-read at interpreter_extra.cc:983
(inside opCreateObject region), :1322 (opSetCritterStat region), :2889, :3156, :3248-3267,
:3678, :4005, :4056, :4494, :4597-4620 — each was located by grep but not read in
context; Appendix A counts them, Chapter 10's mechanism does not depend on them
individually (they are all covered by "scope-resolved or host-anchored"), but a
line-by-line pass belongs to the M6 review.

--------------------------------------------------------------------------------
*End of spec. Maintenance rule (memory-doc-hygiene): as stages land, convert their
chapters' "Target" blocks into brief "SHIPPED @commit" notes or delete them — this
document should shrink toward quirks + forward plan; git is the changelog.*
