# COOP_COVERAGE_STUBS — what the DEDICATED SERVER cannot execute at all

Recon only (2026-07-25, branch `pacing/phase1-seq-stamp`). Axis: the **stub / abort / no-op
surface** of `f2_server`. Every entry is a vanilla Fallout 2 feature that, when the *authority*
tries to run it, either kills the process or silently does nothing.

Companion to `docs/FULL_RUN_RECON.md` (drive primitives + the ending) — this file does not repeat
it. Classes used throughout: **ABORTS** (kills the server for everyone) / **SILENT NO-OP**
(feature just does not happen, nothing reported) / **REFUSED** (something is reported) /
**DONE** (wired).

---

## §0 The two stub helpers, and the numbers

`src/server_stubs.cc` resolves every `f2_client` symbol the core still names at link time.

- `serverStubAbort` — `src/server_stubs.cc:123`. **191 unique symbols**, 191 call sites.
- `serverStubHeadlessOnce` — `src/server_stubs.cc:151`. **13 symbols**, answer the headless truth
  and log once. This block is exactly the 13 script-reachable sfall aborts (§3) — **already
  fixed**; the memory note saying they abort is stale.
- Benign no-op / real bodies: **74 further entries** (~66 no-ops + ~8 real implementations:
  `gameReset` `:756`, `preferencesSave/Load` `:652/:679`, `interfaceSave/Load` `:720/:730`,
  `pipboySave/Load` `:748/:749`, `gameMoviePlay` `:387`, `bufferFill` `:330`).
- The only other deliberate `abort()` in the server build is a trust-boundary FATAL:
  `src/proto.cc:2330` (player-actor sheet row / pid mismatch), which is correct by design.

### Reachability triage of the 191 aborts (measured, not guessed)

Method: exact word-boundary call-site grep of each abort symbol across the 97 `f2_core` TUs plus
the 10 server-only TUs the `f2_server` target adds (`CMakeLists.txt:519-551`).

| bucket | count | meaning |
|---|---|---|
| **link-only** | 14 | no caller in any server-linked TU; pulled in transitively. Unreachable. |
| **renderer-tail-only** | 157 | named *only* by `interpreter_lib.cc`, `game_dialog.cc`, `inventory_ui.cc` — the render halves those files' `serverLoopActive()` branches skip. |
| **sim-path** | 20 | named by a genuinely server-executed TU. Detailed below. |

Of the 20 sim-path aborts: **10 are guarded** (`serverLoopActive()` / `presRecordActive()`),
**3 are teardown-only** (`objectsExit`, `object.cc:369/372/377` — `f2_server` never calls it),
**1 is intLib-only in practice** (`pcxRead` ← `datafile.cc:95` ← `interpreter_lib.cc:173`), and
**6 are live unguarded landmines**. Those 6 are the whole of §1.

---

## §1 The abort surface — the 6 live landmines

Every one is the `endgamePlayMovie` shape the recon warned about: an unguarded core call site into
a symbol whose body is `abort()`.

| # | symbol / stub | reached from | vanilla feature gated | reachability | class |
|---|---|---|---|---|---|
| ~~A1~~ | `gameShowDeathDialog` `server_stubs.cc` | `critter.cc` `_process_rads` | the SFALL death-by-radiation pop-up ("You have died from radiation sickness") | **plausible in normal play** — any player whose rad counter kills them (Toxic Caves goo, Gecko reactor, Broken Hills) | **FIXED 2026-07-25** — addressed to the dying actor; stub headless-safe |
| ~~A2~~ | `automapShow` | `item.cc` `_item_m_use_charged_item` | **using the Motion Sensor** | **plausible in normal play** — a real carryable item | **FIXED 2026-07-25** — addressed `EVENT_AUTOMAP_OPEN`; the USER's viewer opens its own automap (latched, never from inside pump) |
| A3 | `endgamePlayMovie` `server_stubs.cc:354` | `interpreter_extra.cc:4727` (`op_endgame_movie`, opcode `0x8148`, registered `:5131`) | the ending movie. Note the opcode calls it **directly**, bypassing `ScriptRequestHandler` entirely | **only via a script opcode** — but the natural pairing with `op_endgame_slideshow` (`0x8146`), i.e. exactly at victory. *Which shipped `.int` uses `0x8148` is unverified (no bytecode disassembly done).* | **ABORTS** |
| A4 | `textObjectsRemoveByOwner` `server_stubs.cc:530` | `interpreter_extra.cc:3202` | `float_msg(obj, <empty-or-non-string>, type)` — vanilla's idiom for **clearing** a floating message | **only via a script opcode**, but `float_msg` (`0x810A`) is one of the most-used opcodes in FO2. *Frequency of the clear form is unverified.* | **ABORTS** |
| A5 | `pipboyRestHeadless` `server_stubs.cc:492` | `command.cc:509` (`rest` / `restopt` debug verbs) | **RESTING** — see §4, this is the whole feature, not just a verb | **only via an admin/debug verb** on `f2_server` | **ABORTS** |
| A6 | `showMesageBox` `server_stubs.cc:510` | `game.cc:353` and `:369`, called from `server_boot.cc:84` (`gameDbInit`) | the "Could not find the master datafile" operator message | **boot-time misconfiguration** (wrong cwd / missing `master.dat`) | **ABORTS** |

A6 is a pure operator paper cut but the highest-frequency one: the first-run mistake prints
`FATAL — client symbol 'showMesageBox' called on the core-only server` instead of naming the
missing data file.

**Verified NOT a hazard** (guards read in tree, cite for anyone re-auditing):
`combatInputClient` / `combatTurnRunClient` / `calledShotSelectHitLocation` are all behind
`serverLoopActive()` (`combat_drain.cc:412`, `:79`, `:1099`); `_register_priority` is behind
`presRecordActive()` (`server_stubs.cc:256`); all six `wm*` UI stubs are behind
`!serverLoopActive()` in `worldmap.cc` (`:2746`, `:2765`, `:2845`, `:3608-3610`, `:3726`,
`:3768`, `:3790`); `interfaceGetCurrentHitMode` is a benign `return -1` (`server_stubs.cc:454`),
so the cut-list's "latent server crash" wording is now **stale — it is safe today**.

---

## §2 The silent no-op surface — the ScriptRequestHandler split

`ScriptRequestHandler` (`src/script_request_handler.h:34-62`) has **8 virtuals**; the base class is
the null handler (`script_request_handler.cc:10`). `ServerScriptRequestHandler`
(`script_request_handler_server.cc:26-82`) overrides **2**. The client overrides all 8
(`script_request_handler_client.cc:16-58`). The drain is `scripts.cc:962-1071`, on the `serverTick`
spine.

| virtual | server | vanilla feature dropped | requester | class |
|---|---|---|---|---|
| `dialogEnter` | **override** `:28` | conversations | `scripts.cc:1055` | **DONE** |
| `worldMap` | **override** `:78` → `worldmapServerDriver()` | worldmap travel | `interpreter_extra.cc:3154` (`op_world_map`), `worldmap.cc:3130` | **DONE** |
| `townMap` | base no-op | the town map screen | `scripts_request_townmap()` (`scripts.cc:1177`) — **zero callers in tree**, so effectively unreachable | SILENT NO-OP (inert) |
| `elevatorSelect` | base `return -1` (`:47`) | **every elevator in the game.** `-1` makes the caller skip the whole placement/transition block (`scripts.cc:979-1045`) exactly as if the player cancelled | `interpreter_extra.cc:3289` (`METARULE_ELEVATOR`) — **plausible in normal play** | **SILENT NO-OP** |
| `automapSave` | base no-op | persisting the automap on a committed elevator move | `scripts.cc:980` (unreachable while `elevatorSelect` returns -1) | SILENT NO-OP (harmless) |
| `endgame` | base no-op | `endgamePlaySlideshow()` + `endgamePlayMovie()` — **the whole ending** | `interpreter_extra.cc:4656` (`op_endgame_slideshow`), `command.cc:877` (`endgame` verb) | **SILENT NO-OP** |
| `looting` | base no-op | the script/action-path loot screen. Player looting is covered by the wire verbs (`loot`/`take`/`put`/`takeall`) and the callback DOES fire (`server_anim.cc:1042`, allowlisted) — the *screen request* is where it dies | `actions.cc:1629`, `:1704` | SILENT NO-OP (covered) |
| `stealing` | base no-op | **the STEAL skill / pickpocketing, entirely** | `skill.cc:956` | **SILENT NO-OP** |

Other silent no-ops worth naming (all in `server_stubs.cc`, all deliberate, all *correct as
no-ops* but each is a vanilla behaviour the server does not have):

- `endgameSetupDeathEnding` `:361` — picks the death narration. `gEndgameDeathEndings` is empty
  server-side anyway (`endgameDeathEndingInit` dropped from `serverBoot`), so no death ending
  exists to pick. The sim half (`_game_user_wants_to_quit = 2`, `critter.cc:1037`) still fires and
  `serverRun` **deliberately ignores it** — that is the all-players-dead soft-lock on the owner's
  elephant list, not a stub bug.
- `interfaceGetCurrentHand` `:450` → always `HAND_LEFT`. **Banked real gap**: the active hand is
  per-actor sim state stranded in the HUD TU. Partially routed around by the `hand <0|1>` verb.
- `interfaceSave` `:720` writes `HAND_LEFT` unconditionally into the savegame block.
- `_dude_fidget` `:221`, `soundContinueAll` `:512`, `keyboardReset` `:478`,
  `_intface_update_ammo_lights` `:244`, `gameUiEnable/Disable` `:432-433`,
  `interfaceGetItemActions` `:456`, `textObjectsGetCount` `:528` — the six "STILL OPEN" tail items
  in memory `p5-cut-list` are **all benign bodies today**; that note is stale.

### `server_shim.cc` — divergent-by-design, not stubbed
3 shims, all correct and all load-bearing: `_process_bk` (`:50`, tickers-only pump — no input
device), `_updateWindows` (`:57`), `gameMovieIsPlaying` (`:63`, `false` so `_doBkProcesses` keeps
the critter/timed-event block live). `_gdialogActive` is no longer here — the real one from
`game_dialog.cc` is linked. Nothing in this file is a coverage gap.

---

## §3 sfall opcodes — the 13 aborts are FIXED; the real gap is elsewhere

Measured: intersecting every abort symbol against `sfall_opcodes.cc` / `sfall_metarules.cc` /
`sfall_kb_helpers.cc` now yields **zero hits**. All 13 are `serverStubHeadlessOnce` and answer
sanely. What each one is FOR:

| sfall opcode / metarule | call site | stub answer | capability lost |
|---|---|---|---|
| `get_screen_width` / `get_screen_height` | `sfall_opcodes.cc:523`, `:529` | 640 / 480 (`server_stubs.cc:171-172`) | mods laying out UI get vanilla-resolution answers, not the viewer's |
| `show_window` | `sfall_metarules.cc:241`, `:244` | `false` (`:306-307`) | mod-created windows never show |
| `create_message_window` | `sfall_opcodes.cc:559` → `showDialogBox` | `0` = "NO" (`:503`) | a mod prompt is auto-answered NO. ►► A prompt a *player* should answer must be streamed (`wmEncounterPromptBarrier`); this is the never-fatal backstop |
| `get_cursor_mode` / `set_cursor_mode` | `sfall_metarules.cc:121`, `:192` | `0` / no-op (`:366`, `:376`) | cursor-mode mods inert |
| `outlined_object` | `sfall_metarules.cc:186` | `nullptr` (`:437`) | "what is under the cursor" mods inert |
| `get_mouse_x` / `get_mouse_y` / `tile_under_cursor` | `sfall_opcodes.cc:499`, `:508`, `:619` | `0,0` (`:480`) | pointer-driven mods inert |
| `get_mouse_buttons` | `sfall_opcodes.cc:517` | `0` (`:490`) | as above |
| `key_pressed` / `tap_key` | `sfall_opcodes.cc:144`, `:115` | `false` / no-op (`:838`, `:840`) | hotkey mods inert |
| `get_attack_type` | `sfall_opcodes.cc:577` → `interface_get_current_attack_mode` | `false` (`:475`) | a mod reading the current attack mode takes its failure branch |

**The real mod gap is unchanged and is NOT opcode count**: 83 sfall opcodes registered
(`sfall_opcodes.cc`), and **hookscripts (`hs_*.int`) are entirely absent** — zero references in
`src/`. Anything built on hookscripts silently no-ops. See memory `mods-support-landscape`
(track is PARKED by owner).

**The intLib block is the 157.** `interpreter_lib.cc` (in `f2_core`) registers the whole
mapper/GUI opcode family — `0x8062-0x80A0` windows/buttons/regions/movies/sound, and the generic
`say_*` dialog stack `0x804C-0x8061` (`:2200-2279`, `:2239-2260`). *Every one of those leaves is
an abort.* FO2's real dialog does **not** go through them (`gsay`/`giq` land in
`interpreter_extra.cc:3856-3922` → `game_dialog.cc`, which the server links), so the family is
**effectively unreachable in shipped FO2 content** — but a custom `.int` using the GUI opcodes
kills the server on the first call. *Unverified*: I did not disassemble any shipped `.int` to
prove zero usage of `0x804C-0x80A0`.

---

## §4 Screens / modals with no server driver

Drivers that exist: **dialog, barter, rest (client binary only — see below), endgame (client
binary only), worldmap travel, encounter prompt** (memory `modal-drivers-plan`). What is left:

| vanilla screen | server state | consequence | quest impact |
|---|---|---|---|
| **Elevator level picker** (`elevator.cc`, client-only) | `elevatorSelect` → `-1`, **SILENT NO-OP** | button press does nothing, no message. `entermap` routes around it; `climb` (`command.cc:567`) is stairs/ladders, a different mechanism | ►► **QUEST BLOCKER.** Elevators are on the Enclave / Vault City / Sierra Army Depot / tanker paths (`FULL_RUN_RECON` §1). No wire verb exists (`server_control.cc` verb list has none) |
| **Pipboy REST / alarm clock** (`pipboy.cc`, client-only) | three-way dead: `pipboyRestHeadless` **ABORTS** on `f2_server` (A5); a viewer is **REFUSED** with vanilla's own "You cannot rest at this location!" (`pipboy.cc:1869`, leaf guard `:2050`); the golden that "proves rest works" runs the **client** binary | **resting is impossible in co-op** — no healing between fights, no time-passing | ►► blocks any quest with a *wait N days* gate; also strands the owner-ruled "rest heals ALL players" work. The rest SIM is already core (`party_member.h:58-69`); only the loop lives in `pipboy.cc` |
| **STEAL / plant** (`inventory.cc` steal mode) | `stealing()` base no-op — **SILENT NO-OP**, not a hang | the Steal skill does nothing | quest-relevant in Den/New Reno pickpocket content; owner's elephant list |
| **Endgame slideshow + movie** | `endgame()` **SILENT NO-OP**; `op_endgame_movie` **ABORTS** (A3) | the game cannot end. `METARULE_SIGNAL_END_GAME` is additionally suppressed by design (`interpreter_extra.cc:3276`, MP survival) | the ending never renders |
| **"You died" / death narration** | `gameShowDeathDialog` **ABORTS** on the rad path (A1); combat death takes the benign `endgameSetupDeathEnding` (`:361`) + an ignored `quit=2` | no death screen; server keeps running (intended); rad death crashes it | — |
| **Automap screen** | `automapShow` **ABORTS** (A2). `automapSaveCurrent`/`automapSetDisplayMap` are benign no-ops (`:320-321`) — that pair is what stopped mid-run transitions aborting | Motion Sensor kills the server; the viewer's own automap is local | — |
| **Level-up: HP / skill points / free perk** | **DONE 2026-07-25** — sheet edit intents shipped (`docs/PLAYER_SHEET_DESIGN.md` §9, `src/sheet_intent.{h,cc}`, verbs `sheetopen/skillup/skilldown/perkpick/tagpick/mutpick`); the award moved off `characterEditorShow` onto the XP loop (`stat.cc:1001-1018`) | — | — |
| **Character creation** | **DONE** — server-authoritative (`docs/ACCOUNT_IDENTITY_DESIGN.md`, `F2_PLAYER_CREATE`) | — | — |
| **Movies / cutscenes** | **DONE but hazardous**: `gameMoviePlay` (`:387`) marks seen, projects to viewers, then parks in `gameMovieServerBarrier` (`game_movie_state.cc:41-57`). Bails if no viewers remain, so a CMD-only run is safe | ⚠ **doc drift**: the comment at `server_stubs.cc:406-414` says "DEFAULT OFF (F2_MOVIES=1 to project)". It is **default ON** — `serverFeatureEnabled` (`server_loop.cc:169-176`) returns `!headlessProbeActive()` when the var is unset, and `DEDICATED_HOWTO.md:70` correctly documents `F2_MOVIES=0` as the kill switch. So the "viewer fails to render → server parked, `movdone` unreachable from CMD" failure mode is **live by default** | — |
| **Credits, main menu, load/save screen, options, skilldex, char sheet render, pipboy quests/holotapes, town map** | client-only TUs, **not named by any core TU** (verified: no `creditsOpen`/`characterEditorShow`/`skilldexOpen`/`pipboyOpen`/`mainMenuWindowInit`/`lsgWindowHandle` call sites in `f2_core`) | genuinely unreachable — nothing to fix | — |
| **Terminals / computers / slot machines / gambling** | no dedicated screen — they are script + `gdialog` + a skill roll, so they ride the dialog driver | expected to work; **unverified live** | — |

---

## §5 P5 de-stub cut-list — this doc supersedes it

Memory `p5-cut-list` is substantially **stale**; re-measured in tree:

- **H6 (mapLoad chrome + iso split) is DONE.** `map.cc` is in `f2_core` (`CMakeLists.txt` core
  block); `isoEnable` (`map.cc:251`) / `isoDisable` / `mapLoad` / `mapSetElevation` are real
  functions, not stubs. The claim "they ABORT LIVE today" no longer holds.
- The four H6 "BLOCKED" symbols are resolved: `_dude_stand` / `_dude_standup` relocated to
  `critter.cc` (`server_stubs.cc:222` records it); `windowShow` / `windowHide` /
  `gameUiIsDisabled` are benign no-ops (`:543`, `:548`, `:435`).
- `sfxBuildWeaponName` relocated to `f2_core/sfx_name.cc`.
- All six "STILL OPEN from the earlier tail sweep" items are benign bodies (§2).
- `interfaceGetCurrentHitMode` is **safe** (`return -1`, `:454`); the "latent server crash"
  framing is retired.
- **What genuinely remains** is exactly the 157 renderer-tail aborts (intLib / dialog-UI /
  inventory-UI blocks) plus the 14 link-only ones. Per the standing rule, presentation is never
  de-stubbed to real — only relocated behind a seam or left aborting. So the cut-list's remaining
  work is **not on the co-op critical path**; the 6 landmines in §1 are.

---

## §6 Coverage on this axis

**Denominator = the 191 `serverStubAbort` symbols + the 8 `ScriptRequestHandler` virtuals + the
13 sfall-reachable leaves = 212 counted items.** (Not counted: the ~66 benign no-op stubs, which
are *decisions* rather than gaps; the 85 `Presenter` virtuals, a different axis; anything
requiring a live run.)

| | count | share |
|---|---|---|
| Safe: unreachable, guarded, teardown-only, or answered headless | 197 | **93%** |
| **Live ABORTS** (§1) | 6 | 2.8% |
| **SILENT NO-OPs that lose a vanilla feature** (elevator, endgame, steal) | 3 | 1.4% |
| Silent no-ops that lose nothing material (townMap dead, automapSave, script-path looting) | 3 | 1.4% |
| DONE (server override exists) | 2 | 0.9% |
| Renderer-tail aborts reachable only by non-vanilla `.int` GUI opcodes | (157 of the 197 above) | — |

**Honest reading: ~93% of the enumerated severance surface is safe, and the residue is 9 items.**
That number is *static reachability*, not behaviour: it says nothing about features that are
wired but wrong, and it deliberately excludes the streaming gaps (memory `coop-open-issues`).
Two of the six aborts (A3, A4) rest on unverified assumptions about which opcodes shipped scripts
actually emit — the cheap way to settle them is `F2_TRACE_OPCODE` over a real run, not more
reading. And per `sweep-before-recon-lesson`, `scripts/srv_sweep.sh` over all 155 maps is the
oracle that would name any *reached* stub this static pass mis-triaged.

---

## §7 RANKED — fix these first

1. ~~**A1 `gameShowDeathDialog`**~~ — **DONE 2026-07-25**, exactly as prescribed:
   `serverDedicatedActive()` → the dying actor's own reader (`kMsgChannelSystem`), pop-up only in
   single-player. The gate is `playerActorIs(obj)` now, so an extra dying of radiation is
   reported too, and the stub was demoted to headless-safe rather than left aborting — a path
   ordinary play walks down should never be one mis-route away from killing every session.
2. **A5 rest — re-home `pipboyRest`'s loop** so `f2_server` can drive it, and add a `rest` wire
   verb. Resting is a whole missing verb-level feature *and* the current `rest` verb aborts; the
   sim already lives in `f2_core` (`party_member.h:58-69`). **medium**
3. **A2 `automapShow` (`item.cc:2321`) + A4 `textObjectsRemoveByOwner`
   (`interpreter_extra.cc:3202`) + A3 `endgamePlayMovie` (`interpreter_extra.cc:4727`)** — three
   one-line abort→benign/route conversions that each remove a "server dies at the worst possible
   moment" (using an item / a common opcode / winning the game). Do them as one commit.
   **leaf**
4. **Elevators (`elevatorSelect` → `-1`)** — the only *silent* no-op that hard-blocks quests.
   Needs a real driver: stream the level picker to the requesting viewer, take the answer, run
   the existing post-selection sim inline at `scripts.cc:982-1044`. Same shape as the encounter
   prompt. **medium**
5. **Server-side `endgame()` override** — evaluate the 52 gvar-matched slides server-side and
   stream them (`FULL_RUN_RECON` §2 proved the ending is a pure function of 16 gvars). Must land
   *with* A3, or wiring the handler naively re-introduces the abort. **medium**

Also worth a two-line commit while nearby: fix the `F2_MOVIES` "DEFAULT OFF" comment
(`server_stubs.cc:406-414`) — it is default **ON** — and give A6 (`showMesageBox` ←
`server_boot.cc:84`) a real stderr line so a wrong cwd names the missing data file. **leaf**
