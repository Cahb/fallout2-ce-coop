# FULL_RUN_RECON — can we drive a complete Fallout 2 playthrough headless?

Recon only (2026-07-25). No code written. Question: Temple of Trials → Enclave endgame,
unattended, with a real viewer able to attach and spectate. **The hard part is authoring the
scenario, not the harness** — this document says how much of the critical path is expressible
as data, and what is genuinely missing.

## Verdict up front

- **The harness already exists twice over**: `F2_SERVER_ACTIONS="tick:verb:arg,…"` (no socket,
  `src/server_main.cc:270-288`, fired at `:637-641`) and the CMD port (`F2_SERVER_CMD` →
  `serverAdminLine` then `commandDispatch`, `src/server_main.cc:361-365`). `scripts/throw_smoke.sh`
  is the working template for "boot a real server + real viewer, drive by `nc`, assert on traces".
- **The ending is 100% gvar-determined.** `endgamePlaySlideshow` (`src/endgame.cc:212-243`) does
  nothing but `gameGetGlobalVar(ending->gvar) == ending->value` for each line of `data/endgame.txt`
  — 52 slides over **16** gvars. There is no hidden quest bookkeeping behind the ending.
- **But the two verbs that would prove it are missing on `f2_server`**: there is **no gvar
  read/write verb at all**, and the **server drops the endgame** (see §1 gaps E and F).
- **Recommendation: build (b), the state-warp run, first.** It is ~4 small primitives away and
  it proves the whole world traverses + the real ending renders. A "true" playthrough (a) cannot
  even finish the Temple from the command channel today (no *use-item-on-object*, no
  *skill-on-object*).

---

## §1 Inventory of drive primitives we already have

### Debug verbs — `src/command.cc`, reachable from the CMD port and `F2_SERVER_ACTIONS`
Int args only (`verb arg arg2`). All act on `gDude` (slot 0).

| capability | verb | site |
|---|---|---|
| walk to an absolute tile | `walkto <tile>` | `command.cc:728` |
| walk/run ±N tiles | `walk <n> [1]` | `command.cc:281` |
| teleport ±N tiles | `warp <n>` | `command.cc:255` |
| **change map** (real transition) | `entermap <mapIndex> [elev]` | `command.cc:893` → `mapSetTransition`; performed by `mapHandleTransition`, run **every beat** (`server_loop.cc:503`) |
| open nearest door | `usedoor` | `command.cc:515` |
| climb nearest stairs/ladder (intra-map elevation) | `climb` | `command.cc:567` |
| pick up nearest ground item | `pickup` | `command.cc:541` |
| talk to nearest critter with `scriptIndex == arg` | `dtalk <scriptIndex>` | `command.cc:633` |
| pick dialog option N / end convo | `dsay <n>` / `dend` (queue **before** `dtalk`) | `command.cc:668,674` |
| barter | `boffer/btake/bunoffer/bcommit/bdone/bcancel` | `command.cc:679-709` |
| worldmap | `wmenter`, `wmmove <x> <y>`, `wmesc`, `wmtravel <pos>` | `command.cc:878,421` |
| give item to the dude | `give <pid> [n]` | `command.cc:189` |
| drop / useitem(self) / wield / stow / reload / unload | — | `command.cc:199-250,772` |
| combat | `aggro <n>`, `cattack <n>`, `caim`, `cmove`, `cendturn`, `cdamage`, `explode` | `command.cc:253,740-830` |
| character | `xp`, `levelup`, `perk`, `tag4`, `mutate`, `charsnap`, `charroll` | `command.cc:137,320-370` |
| rest, save, load | `rest`, `restopt`, `savegame <slot>`, `loadgame <slot>` | `command.cc:480,120-136` |
| request the endgame | `endgame` | `command.cc:865` (**client-binary only — see gap F**) |
| state checkpoint into the dump | `mark` | `command.cc:251` |

### Admin verbs — `src/server_admin.cc` (answer the operator)
`saves`, `save <1-10>`, `load <1-11>` (lobby only), `new <map.map>` (lobby only), `status`,
`say`, `saydemo`, `movie`, **`timeskip <minutes>`** (`:667`), `spawn <pid> [n] [tile]` (`:1067`),
`stress` (`:1118`), `despawnall`, `revive <slot>`, `xp <slot> <amt>`, `fixcar` (`:706`, **not in
DEDICATED_HOWTO's table — doc drift**), `quit`/`shutdown`.

### Control/wire verbs — `src/server_control.cc`
Per-session gated; a **client** sends these. NOT reachable from the CMD port except the
explicitly-added twins (`wmenter/wmmove/wmesc`, `dsay/dend`). Notably wire-only:
`useitemon`, `put`, `take`, `takeall`, `skill`, `open`, `loot`, `invwield`, `hand`, `encaccept`.

### Named gaps

- **primitive `gvar` missing** — nothing anywhere reads or writes a global variable from an
  operator/scenario channel (`grep gvar src/command.cc src/server_admin.cc src/server_control.cc`
  → only comments and `fixcar`'s two reads at `server_admin.cc:734-738`). This is the single
  biggest gap: it blocks (b) entirely and blocks *asserting* anything in (a).
  **Good news:** `src/game_vars.h` already carries the full `GameGlobalVar` enum — **696 entries,
  and I verified its ordinals match a fresh `data/vault13.gam` decode exactly** (`GVAR_FALLOUT_2`
  = 494, `GVAR_ENDGAME_MOVIE_ARROYO` = 408, `GVAR_TALKED_TO_ELDER` = 531 …). A `gvar <name|n>
  [value]` verb can resolve names from a generated table with zero data probing.
- **primitive `state query` missing** — `status` replies exactly one line, `"world: LOADED"`
  (`server_admin.cc:587-590`). There is no way to read the dude's map/tile/elevation/HP, whether
  combat is active, whether a walk is still in flight, or the live dialog node. Consequence: a
  scenario driver is **open-loop** today. The only readback is `F2_SERVER_DUMP`, written *after*
  the run (`server_main.cc:736-739`), and `mark` checkpoints inside it.
- **primitive `map-arrival trace` missing** — `src/map.cc` has **zero** `fprintf(stderr)`. The
  only arrival line is `[wmsrv] entered map=%d elev=%d dudeTile=%d` (`server_worldmap.cc:289`),
  which only the worldmap driver emits — an `entermap` or exit-grid transition logs nothing. So a
  scenario driver cannot even see that a warp landed.
- **primitive `use-item-on-object` missing** — `useitem` only uses on self (`command.cc:222`).
  **This is on the Temple critical path**: the blocking Temple door is script-locked and needs the
  plastic explosive (`docs/TEMPLE_DEMO_ROADMAP.md`, "Key insight — REVISED 2026-07-18").
- **primitive `skill-on-object` missing** — `useskillon` hard-filters to a *wounded living
  critter* (`command.cc:161-178`). No lockpick/repair/science on a door, terminal or container.
- **primitive `elevator` missing** — `elevatorSelect` is the base null handler returning `-1` on
  the server (`script_request_handler.cc:5`; `script_request_handler_server.cc:22` states it
  explicitly), so every elevator button press is a silent no-op on `f2_server`. Elevators are on
  the Enclave / Vault City / Sierra / tanker paths. `entermap` routes around it.
- **primitive `server endgame` missing (E)** — `ServerScriptRequestHandler` overrides only
  `dialogEnter` and `worldMap` (`script_request_handler_server.cc:26-80`); `endgame()` falls
  through to the base no-op, so `op_endgame_slideshow` from the Enclave script is **silently
  dropped**. And `endgamePlayMovie` is `serverStubAbort("endgamePlayMovie")`
  (`server_stubs.cc:354`) — so naively wiring the handler *aborts the server*.
- **primitive `endgame verb on f2_server` missing (F)** — `command.cc:865` works, but only in the
  **client** binary (`ClientScriptRequestHandler::endgame`, `script_request_handler_client.cc:43`).
  That is what the existing `denbus1_endgame` golden runs (`run_golden_server.sh:212` drives
  `$ROOT/build/fallout2-ce`, not `f2_server`).
- **primitive `terminal quit honored` missing** — `serverRun` never breaks on
  `_game_user_wants_to_quit` (banked in memory `modal-drivers-plan`, "serverRun does not honor
  the terminal quit"; changes 5 goldens). So even a *working* endgame leaves the server running:
  detect completion from a trace/dump, never from process exit.
- **primitive `party-member add` missing** — `spawn <pid>` makes a plain critter; nothing calls
  `partyMemberAdd`. Matters only if a companion is on the chosen path (Vic is not, if you warp).
- **primitive `worldmap teleport / mark-area-known` missing** — no verb touches
  `wmAreaMarkVisitedState` / `wmTeleportToArea`. Partly self-solving: stepping onto an exit grid
  calls `wmMapMarkMapEntranceState` (`object.cc:1425`).
- **primitive `exit-grid targeting` missing** — exit grids *do* fire from a script-driven move:
  `objectSetLocation(gDude,…)` scans the destination tile for `isExitGridPid` and calls
  `mapSetTransition` (`object.cc:1401-1428`). So `walkto <exitGridTile>` is a real transition —
  but you must know the tile. A `exitgrid [n]` verb (nearest exit grid, same shape as
  `usedoor`/`climb`) removes all map-data mining. Note: that branch is `obj == gDude` only, so an
  **extra player walking onto an exit grid does nothing** (separate co-op bug, unverified live).
- **tool bug**: `tools/f2data.py` crashes (`IndexError` at `read_proto`, line 158) on
  `OBJ_TYPE_MISC` protos, and `--find` matches one pid by name — so it cannot currently enumerate
  the 8 exit-grid pids `0x05000010-0x05000017` (`proto_types.h:202-203`). Small fix.

---

## §2 How progression is actually gated

**The ending is a pure function of 16 gvars.** `data/endgame.txt` holds 52 slides; each is
`gvar, value, art, narrator`, and `endgamePlaySlideshow` renders every line whose gvar matches
(`endgame.cc:229-240`). Decoded (indices validated against `src/game_vars.h`):

```
408 GVAR_ENDGAME_MOVIE_ARROYO        [1]              417 ..._GECKO          [1..5]
409 ..._MODOC          [2,3,4,5]     418 ..._REDDING       [1..4]
410 ..._DEN            [1..4]        419 ..._BROKEN_HILLS  [1,2,3]
411 ..._VAULT_CITY     [2,3,4,5,10,11] 420 ..._NCR         [1..4]
412 ..._RENO           [1..10]       421 ..._VAULT_15      [1,2]
413/414/415 ..._RENO_ADD1/2/3 [1]    422 ..._VAULT_13      [1,2]
                                     423 ..._SAN_FRAN_SHI  [1,2,3]  425 ..._SAN_FRAN_PUNKS [1]
```

**The plot's macro state is also gvar+clock, in the engine itself.**
`_scriptsCheckGameEvents` (`scripts.cc:466-537`, run at every midnight from
`gameTimeEventProcess`) is FO2's main-quest timer: it keys on game *day* vs the sfall
`gMovieTimerArtimer1..4` thresholds and on `GVAR_FALLOUT_2` (`>= 3` forces the Arroyo-destroyed
beat, which then `wmAreaSetVisibleState(CITY_ARROYO,0,1)` +
`wmAreaMarkVisitedState(CITY_DESTROYED_ARROYO,2)`), and `GVAR_ENEMY_ARROYO != 0` sets
`_game_user_wants_to_quit = 2` — the failure ending — with no script involved.

So: **the critical path is gvar state + map visits + a handful of inventory items.** Named
milestones all exist as gvars: `GVAR_START_ARROYO_TRIAL`(10), `GVAR_TALKED_TO_ELDER`(531),
`GVAR_FIND_VIC`(619), `GVAR_VIC_DEVICE`(29), `GVAR_VAULT13_FOUND_GECK`(233),
`GVAR_ARROYO_RETURN_GECK`(480), `GVAR_GAVE_GECK_EXP`(629), `GVAR_NAVARRO_FOB`(512),
`GVAR_SAN_FRAN_TANKER`(363), `GVAR_ENCLAVE_REACTOR`(435), `GVAR_ENCLAVE_COUNTDOWN`(498),
`GVAR_ENCLAVE_FRANK_DEAD`(499), `GVAR_FALLOUT_2`(494).

**What is *not* gvar-expressible** and would still need real interaction: the per-map `lvar`
state some scripts key on, party membership (Vic/Cassidy as objects), and the *exact* value a
dialog branch writes (a gvar name identifies the slot, not the writer —
memory `game-data-probing` §6). I did **not** disassemble any `.int`, so "which gvar value each
dialog option writes" is **unverified**; that is the one real authoring cost, and the cheap way
to pay it is empirically: run the real dialog once under `F2_DIALOG_TRACE` + `F2_TRACE_GVAR` and
record the diff. That turns scenario authoring into *capture-then-replay* instead of reverse
engineering.

**Map indices for the critical path** (from `data/maps.txt`, all `saved=Yes`):
`126 artemple · 4 arvillag · 3 arcaves · 5 arbridge · 9 kladwtwn · 6 denbus1 · 41 v13ent ·
40 vault13 · 127 ardead · 109 navarro · 135 sftanker · 129 encdock · 131 encgd · 134 enctrp ·
133 encrctr · 132 encpres · 130 encfite`.

---

## §3 Three approaches, ranked

**(b) STATE-WARP RUN — build this first.** `entermap` chapter to chapter, set the gvars each
stage would have produced, play only the bits worth watching (the Temple fight, Horrigan).
*Cost:* `gvar` verb + `state`/`status` readback + a map-arrival trace + server-side endgame
(≈ the 4 gaps above; each is a leaf change, none is design-class except the endgame stream).
*Fidelity:* low for the journey, **exact** for the ending (it is literally the gvar-matching
function). *Proves:* every critical-path map loads and mid-run-transitions cleanly under our
engine; the ending renders; a spectator can watch a 15-map run end to end. That is the demo the
owner described, and it is the honest first build.

**(c) CHAPTER SUITE — the natural second step, and the thing that makes (a) affordable.** N
independently-runnable chapters, each booting from a canned save (`F2_SERVER_LOAD=<slot>` /
`load <n>`; `save`/`savegame` verbs already exist and are core, `savegame.cc`), each asserting a
small end state. Chapters can be authored *by capture*: play a segment live once with
`F2_TRACE_GVAR`/`F2_DIALOG_TRACE`/`F2_SERVER_NET_TEE` on, then replay the recorded verb list.
Each chapter is short, so nondeterminism and the object-id budget stay bounded, and one broken
chapter does not hide the other fourteen. *Cost:* mostly authoring; needs (b)'s readback verbs
to write assertions. *Proves:* the same as (a), chapter by chapter, and it is a usable
regression gate.

**(a) TRUE PLAYTHROUGH — do not start here.** Script every real action end to end.
*Blockers today:* no *use-item-on-object* (so the Temple door — the very first obstacle — cannot
be blown), no *skill-on-object*, no elevator driver, no party-add, no closed-loop readback, and
`f2_server` is documented **nondeterministic run-to-run on AI-heavy maps** (memory
`p5-server-plan`: only artemple/kladwtwn/newr1/vault13/arcaves/modmain are clean). A 40-hour
game of open-loop tick-scheduled verbs will not survive one AI reroll. It only becomes tractable
*after* (c) exists, one chapter at a time.

---

## §4 Smallest useful first milestone

**`scripts/fullrun_smoke.sh` — the WARP TOUR.** Reuses the `scripts/throw_smoke.sh` shape
(boot `f2_server` on `artemple.map`, `F2_SERVER_CMD`, drive with `nc`, read stderr) or, simpler,
needs *no new code at all* in its first form:

```sh
cd FO2 && env F2_SERVER_MAP=artemple.map F2_SERVER_TICKS=40000 \
  F2_SERVER_DUMP=/tmp/tour.dump \
  F2_SERVER_ACTIONS="200:entermap:4,600:entermap:9,1000:entermap:6,1400:entermap:41,\
1800:entermap:40,2200:entermap:127,2600:entermap:109,3000:entermap:135,3400:entermap:129,\
3800:entermap:131,4200:entermap:134,4600:entermap:133,5000:entermap:132,5400:entermap:130,\
5600:mark:1" ../build/f2_server
```

Asserts: (1) exit code 0 — **no `serverStubAbort`, no crash, no hang** across 15 real mid-run
transitions on the critical path (the all-map sweep only ever proved *fresh* loads); (2) the
final `F2_SERVER_DUMP` reports the Enclave End Fight map and a live dude; (3) `served … to
completion` on stderr. That is a genuine liveness gate on the whole world, today, with zero new
primitives — and it is the exact run that will surface the next unsevered symbol or the
`objectFindNext` transition crash.

Then, in order: add `gvar <name|n> [value]` → extend the tour to set the 16 ending gvars →
wire `ServerScriptRequestHandler::endgame()` (evaluate the 52 slides server-side, stream them,
and **replace the `endgamePlayMovie` abort stub**) → the tour ends in the real ending with a
viewer watching. That is the owner's "prove it's possible", in four leaf changes.

---

## §5 Spectating — yes, with two caveats

- **Spectators are a real, supported state.** A session whose `claim` is denied "stays a
  spectator. It keeps receiving the whole stream and may claim later"
  (`server_control.cc:2039-2044`); every verb past that point is refused for an unbound session
  (`:2057-2063`). The client handles it: `mine = gClientHostDude; // spectator …`
  (`client_net.cc:1202`), and dialog/inventory/loot windows already render read-only for
  spectators (`client_dialog.cc:183-199`, `inventory_ui.cc:5271-5400`).
- **A scripted run is fully compatible**: the CMD port drives `gDude` directly and is not
  claim-gated, while the wire keeps streaming to viewers. Nothing about `commandDispatch` needs a
  claimant.
- **Caveat 1 — the wire listener blocks at boot.** `netSink.acceptClients(1)`
  (`server_main.cc:317-322`) waits for the *first* client before serving. So either a viewer is
  present at launch, or you run CMD-only and **nobody can ever attach**.
  → *primitive missing: listen-without-waiting.*
- **Caveat 2 — the client always claims.** `main.cc:1237-1243` unconditionally sends
  `login`/`claim`, so a human viewer would seize slot 0 and could fight the script with the
  mouse. **Workaround that needs no code:** boot with `F2_SERVER_PLAYERS=2` and let the human
  claim slot 1 — and `_map_place_dude_and_mouse` **re-plants every online extra beside the host
  on every map load** (`map.cc:1665-1687`), so the spectator body follows the scripted run across
  all 15 maps for free. (Read from code; **unverified live**.)
  → *primitive missing (nice-to-have): `F2_PLAYER_SPECTATE=1`.*
- Movies: keep `F2_MOVIES` **off** for an unattended run — `movdone` is a wire-only ack the
  operator cannot send, so a viewer that fails to render wedges the barrier (DEDICATED_HOWTO
  §2.6). With `F2_MOVIES` unset, `gameMoviePlay` marks-seen and returns, which is exactly what
  the `_scriptsCheckGameEvents` ARTIMER movies need to not block.

---

## §6 Risks / dead ends worth knowing up front

1. **Server-side endgame is a stub trap, not just a gap** — `endgamePlayMovie` is
   `serverStubAbort` (`server_stubs.cc:354`). Wire the handler without replacing it and the
   server dies at the moment of victory.
2. **The server never stops at the terminal point** (`_game_user_wants_to_quit` unchecked in
   `serverRun`). Assert on a trace or the dump, not on exit.
3. **Elevators are dead on the server** (`elevatorSelect` → -1). Any chapter that expects to ride
   one silently stalls; use `entermap` instead.
4. **Modal drivers park the sim clock.** A block-and-pump driver that reads `simClockNow()`
   without advancing it freezes every time-keyed cadence (memory `worldmap-streaming-track`,
   BUG CLASS 2). Re-check this for anything new a long run leans on.
5. **`wmesc` is an UNDO, not a way off the screen** — the ESCAPE path exits with `map==-1` and no
   `mapLoadById`, i.e. it ends the session on top of the map the party walked out of. That map is
   intact (the destructive teardown moved to the `mapLoadById`), and the driver now rewinds
   `worldPosX/Y` to the session's entry position and clears `isInCar` so the world agrees with it.
   Safe for a scenario to use, but it is a *cancel*: it does not advance the run, and a scenario
   that sends it expecting to land somewhere will sit on the old map. Use `wmenter` to land.
6. **Object-id budget**: `_cur_id` is process-monotonic, warns and continues at 18000, and is
   only reset by a map reload (memory `object-id-budget-long-session`). A warp tour reloads maps
   constantly, so this is fine — a single-map 40-hour run would not be.
7. **Nondeterminism**: `f2_server` is wall-clock-steered on AI-heavy maps. A full run can **never**
   be a byte-exact reconstruction golden; it must be a *liveness* gate ("reached the state without
   crash/softlock/abort") — the same conclusion the banked GOLDEN-TEMPLE CAPSTONE reached.
8. **Mid-run transitions are the least-tested path** — "all 155 maps serve" means *fresh* loads;
   the transition path once died on `automapSaveCurrent` and there is an open intermittent
   `objectFindNext` SIGSEGV on the post-transition netId walk (memory
   `transition-netid-walk-crash`; ASAN build at `build-asan/`). The warp tour is the ideal way to
   hunt it — run it under ASAN.
9. **Warping into a map whose prerequisites were never met is untested.** Map scripts may read
   lvars/gvars they expect to be set; behaviour is **unverified**. Expect to discover a few maps
   that need a gvar preset before the warp lands cleanly — which is another argument for the
   `gvar` verb landing early.
10. **Save/load between chapters**: `save`/`saves`/`load` (admin, `load` lobby-only) and
    `savegame`/`loadgame` (debug) all exist and `savegame.cc` is core, so chaptering works — but
    memory `headless-load-lvar-leak` records a **vanilla** bug leaking an lvar block and running
    `obj_dude`'s start proc twice on **every** load. A 15-chapter suite pays that 15 times.
