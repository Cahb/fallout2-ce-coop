# Phase 1 Worklist — Presenter Seam
### Generated from a 4-agent parallel classification sweep of all core sim files
### Status legend: [ ] todo · [x] converted & goldens green

Source of truth for Phase 1 conversion sessions. Derived from grep regex over
render/UI callees (see REWRITE_PLAN.md 1.1) + per-function human-grade
classification. Verdicts:
- **SEP** (sim-emits-presentation): function stays in `f2_core`; its
  presentation calls become `Presenter` emissions. THE conversion work.
- **WFU** (whole-function-UI): function is presentation; moves to `f2_client`
  wholesale, no call-by-call conversion.

## Grand totals

| File | Sites | in WFU fns | SEP conversions |
|---|---|---|---|
| inventory.cc | 190 | ~157 (22 fns) | ~33 (8 fns) |
| game.cc | 129 | ~124 (8 fns) | ~5 (3 fns) |
| combat.cc | 85 | 35 (8 fns) | 50 (15 fns) |
| worldmap.cc | 72 | 51 (15 fns) | 21 (11 fns) |
| proto_instance.cc | 54 | 0 | 54 (25 fns) |
| interpreter_extra.cc | 42 | 0 | 42 (31 op handlers) |
| map.cc | 41 | 15 (9 fns) | 26 (8 fns) |
| skill.cc | 33 | 0 | 33 (3 fns; `skillUse` alone = 30) |
| actions.cc | 30 | 0 | 30 (14 fns) |
| animation.cc | 24 | 2 | 22 (mostly display; phase-2 overlap) |
| item.cc | 19 | 0 | 19 (13 fns) |
| combat_ai.cc | 18 | 0 | 18 (5 fns) |
| party_member.cc | 15 | 0 | 15 (6 fns) |
| tile.cc | 13 | 6 | 7 (4 fns) |
| critter.cc | 11 | 0 | 11 (7 fns) |
| object.cc | 7 | render group | 3 |
| stat.cc | 5 | 0 | 5 (2 fns) |
| scripts.cc | 2 | 0 | 2 |
| queue.cc / light.cc | 2 | 0 | 2 |
| **TOTAL** | **~779** | **~390** | **~389** |

## Presenter event taxonomy (v1 = mechanical mirror of existing callees)

Phase-1 rule: the interface MIRRORS existing callees 1:1 so `SdlPresenter`
reproduces old behavior exactly (two-commit rule: mechanical first, semantic
events like "AttackResolved" come later). Categories, by frequency:

1. `consoleMessage` ← displayMonitorAddMessage (heaviest; everywhere)
2. `worldInvalidate(rect, elevation)` ← tileWindowRefresh/Rect, windowRefresh(iso)
3. HUD: `hudHitPoints`, `hudActionPoints`, `hudArmorClass`, `hudItems`,
   `hudEndTurnButtons`, `hudIndicatorBar`, `hudBarShow/Hide` ← interfaceRender*/interfaceUpdate*/indicatorBarRefresh/interfaceBarEndButtons*
4. `sfxPlay(name)` / `sfxPlayAt(name, obj)` ← soundPlayFile, soundEffectLoad(WithVolume)+Play, _gsound_play_sfx_file_volume
   (sfxBuild*Name are pure string builders → stay in core as data, NOT presenter)
5. `musicPlay(name)` / `musicStop` ← _gsound_background_play_level_music, backgroundSoundDelete/Load
6. `floatText(obj, text, colors)` ← textObjectAdd (+ its refresh)
7. `outlineChanged(obj)` ← objectSetOutline/objectClearOutline call sites in sim
   (setters themselves stay in core — they mutate Object state; client reads it)
8. `screenFade(direction/palette)` ← paletteFadeTo call sites in sim
9. `cursorMode(mode)` / `cursorHidden` / `mouseObjectsVisible(bool)` /
   `scrollEnabled(bool)` ← gameMouseSetCursor/SetMode, gameMouseObjects*, _gmouse_*
10. `errorBox(text)` ← showMesageBox, _win_msg (fatal/save errors)
11. **Modal requests (interim passthrough → phase 3 sessions):**
    `requestQuantity(...)` ← inventoryQuantitySelect called from sim transfer paths;
    `requestConfirm(...)` ← showDialogBox yes/no (random-encounter confirm)
12. `presentationInit` ← debug console + font wiring in gameInitWithOptions

## STANDING RULE: coverage before conversion — each batch FIRST extends the
## golden corpus (traces or F2_PROBE_ACTIONS) to exercise the paths it will
## convert, THEN converts. Green must mean watched-green.

## Conversion batches (each = one session/agent; goldens must stay green)

### Batch 1 — pure emitters, no structural questions (low risk, do first)
- [x] critter.cc (11): critterAdjustPoison, poisonEventProcess, critterAdjustRadiation, _process_rads, critterKill, dudeDisable/EnableState
- [x] stat.cc (5): pcAddExperienceWithOptions (msg+levelup sfx+HUD), pcSetExperience
- [x] item.cc (19): device on/off, drug effects, withdrawal, trickle, stealth boy, item move/drop refreshes
- [x] queue.cc (1): explosionFailureEventProcess · light.cc (1): lightSetAmbientIntensity
- [x] scripts.cc (2): _scriptsCheckGameEvents (dawn/dusk refresh), scriptsInit (debug hook → presentationInit)
- [x] party_member.cc (15): errorBox ×12 in save/load prep; _partyMemberIncLevels (msg+float+refresh)
- [x] combat_ai.cc (18): _ai_try_attack + aiAttemptWeaponReload (sfx), critterSetTeam (outline), _ai_print_msg (float), _ai_magic_hands (msg)

### Batch 2 — actions/skills/proto_instance (medium; message+sfx heavy)
- [x] actions.cc (30): damage-show/death anims (sfx names OK in core), _is_next_to/_can_talk_to (msg), actionUseSkill (float), explosion/projectile refreshes
- [x] skill.cc (33): skillUse (30 sites: msg + fade + HP HUD), skillsPerformStealing, _show_skill_use_messages
- [x] proto_instance.cc (54): look/examine (msg via fn-ptr!), pickup/drop/destroy (refresh), use-book (fade), door/container (sfx+msg), ladders/stairs (refresh), _protinst_default_use_item (7 mixed)

### Batch 3 — script opcodes (42) — interpreter_extra.cc
- [x] Thin UI opcodes → presenter: opDisplayMsg, opFloatMessage, opGameFadeIn/Out, opPlaySfx
- [x] State opcodes' trailing refresh/HUD calls: opMoveTo, opCreateObject, opDestroyObject, opWieldItem, opCritterHeal, opSetObjectVisibility, opAnim, inventory ops, _correctFidForRemovedItem
- [x] opSfxBuild* stay in core (pure string builders returning to script)

### Batch 4 — combat.cc (50 SEP sites, 15 fns; the crown jewel)
- [x] _combat_begin/_combat_over/_combat (cursor+scroll+HUD+refresh at boundaries)
- [x] _combat_turn (14 sites: AP/AC HUD, end-buttons lights, cursor, refresh)
- [x] _combat_update_critter_outline_for_los / _combat_delete_critter (outline)
- [x] _combat_attack/_combat_standup (AP HUD), _set_new_results (hudItems), _damage_object (hudHitPoints)
- [x] _combat_attack_this (bad-shot msgs ×6 + out-of-ammo sfx), _combat_give_exps/combatAttemptEnd/_combatKillCritterOutsideCombat (msg)
- NOTE: attackCompute + _apply_damage are already pure — do not touch.
- Later (semantic, separate commit): single `attackResolved(Attack*)` event replacing _combat_display client-side narration.

### Batch 5 — map.cc + worldmap.cc + game.cc SEP remnants
- [x] map.cc (26): mapLoad (12 interleaved: music/cursor/bar/refresh around data load — HIGHEST reorder risk, keep event order exactly), mapSetElevation (cursor+mouse objects), _map_save_file (errorBox), transitions
- [x] worldmap.cc (21): loader errorBox ×15, wmRndEncounterOccurred (msg + requestConfirm + fade/blink events), wmSetupRandomEncounter (msg), wmCarGiveToParty (msg), map music (musicPlay/Stop)
- [x] game.cc (5): gameSetGlobalVar karma msg, gameTakeScreenshot msg, gameInitWithOptions presentationInit

### Batch 6 — inventory.cc SEP islands (33) + split candidates
- [x] _inven_update_lighting (refresh), _invenWieldFunc/_invenUnwieldFunc (sfx+refresh)
- [x] _drop_ammo_into_weapon (sfx + requestQuantity)
- [x] SPLIT: _move_inventory done (lootTransferItem in item.cc); barter pair analyzed — bare itemMoveForce dispatch, no extraction needed (Phase 3 intent)
- [x] SPLIT: inventoryWindowOpenContextMenu — itemDropStack/itemUseDrug/itemUseFromInventory/weaponUnloadIntoInventory in core (item.cc); menu chrome + quantity modal + staging slots stay client; hudHitPoints from sim path; covered by arvillag_invmenu golden
- [x] EXTRACT: H-1 (inventoryApCostApply), H-2 (actionUseItemOnObjectWithApCost), H-5 (actionResolveDroppedExplosive), H-3/H-4 (equipmentDetach/Apply) all in core

### Batch 7 — WFU migration (mechanical file moves, CMake split lands here)
- Ledger pre-work done so far: H-1..H-5 (Batch 6), H-6 (containerStoreItem/weaponLoadAmmo; _inven_pickup/_switch_hand slot choreography = session editing state, stays UI until Phase 3 intents), H-7 (lootTakeAll), H-8 (lootStealExperience), H-9 (lootOpenCheck/lootTargetDetach/Reattach/lootCaughtStealingReact), H-10 (barterReactionModifier/barterComputeValue), H-11 partial (barterAttemptTransaction in core; table-staging/equip churn still UI — pairs H-56), H-12 (combatPlayerTurnShouldBreak/OutOfAp/Resolve — _combat_input is now a pure pump), H-13 (worldmapTravelStep/RestHeal/MarkVisited/ClockTick/EncounterCheck — wmWorldMapFunc travel sim as core steps; H-16 mark-radius called from worldmapTravelMarkVisited; wmPartyInitWalking + wmPartyIsWalking exposed as core seams; arvillag_wmtravel golden + 'worldmap' state-dump line), H-14/H-15 (wmTransitionSaveMap/SuspendScripts/ResumeScripts + wmEncounterStagingClear seams), H-17 (mapVariablesFree from gameExit), H-18 (reclass note on Batch 7 map.cc line)
- Ring-2/3 E2C done: H-30 (gameMovieMarkSeen), H-41 (restHealReset/Check/Apply in party_member.cc), H-43 (pcLevelUpApply in stat.cc; saved lastLevel/hasFreePerk state stays editor — moves with H-49), H-44 (perkChoiceApply in perk.cc — perk commit + Lifegiver/Educated instants; Tag!/Mutate! follow-ups via PerkChoicePending out-param), H-47 (traitsMutateDrop/traitsMutateGain in trait.cc — Mutate! swap; dialog selection passed as params, session rollback memcpy stays editor), H-48 (skillsTagPerkApply in skill.cc — Tag! 4th-tag commit; cancel restore stays editor), H-57 (elevatorResolveStartLevel/Destination), H-40 (restSimPacing/restSimMinutesTick/restSimMinutesFinish/restSimHoursTick/restSimHoursFinish/restSimOverdueEvents + restUntilHealedDuration in party_member.cc — pipboyRest's inline clock/queue sim as core steps at their original positions; pipboy keeps redraw/ESC/delay/_proc_bail_flag chrome; the until-healed 24h/3h two-pass chunking stays in pipboyRest since it recurses into the UI rest loop, but is trivially re-drivable from the core steps — see the rest/restopt probe actions), H-42 (restOptionDecode/restUntilHourDuration in party_member.cc — rest-duration tables + until-hour wall-clock math, _ClacTime moved whole; PipboyRestDuration enum moved to party_member.h; the _critter_can_obj_dude_rest gate stays at the pipboy call site because it guards menu opening before any intent exists and vanilla never re-checks it per option — headless drivers own it, as the rest probe action does), H-49 (characterSnapshotTake/TakeSkillPoints + characterSnapshotRestore/RestoreSkillPoints/RestoreHitPoints in character_transaction.cc — the editor's begin/rollback transaction over committed dude sim state: critter proto data, hit points, pc name, unspent skill points, held in a CharacterSnapshot; take/restore split into named steps so characterEditorSavePlayer/RestorePlayer keep every editor-only line at its exact original position, no reorder. Deliberately left editor-side and documented: _pop_perks + gCharacterEditorPerksBackup (the perk backup doubles as the perk dialog's session copy passed to perkChoiceApply — rule 5), the tagged-skill/trait backups (they double as the skill/trait dialogs' reset baselines), and gCharacterEditorLastLevel/HasFreePerk + backups (savegame stream state — the accessor move was skipped to avoid changing the save/load byte layout). Golden: arvillag_chartxn pins the rollback via charsnap:0/charroll:0 probe actions — snapshot 44hp/0SP, mutate to 19hp/10SP via hurt+levelup, roll back to 44hp/0SP)
- Ring-1 ledger COMPLETE (H-13 was the last ticket; worldmap now has golden coverage via arvillag_wmtravel). Perk cluster covered by arvillag_perks golden + dude_tags/dude_traits dump lines. Rest sim covered by arvillag_rest golden (rest/restopt probe actions; queue-event interruption of a rest not pinned — no vanilla-deterministic triggering event was cheaply arrangeable, the rule lives in restSim*Tick). Next: Ledger pre-work COMPLETE (H-49 was the last E2C ticket; the editor transaction now has a core begin/rollback model + arvillag_chartxn golden). Batch 7 (physical file moves + CMake library split) is next — and Batch 7 is the user's switch-to-Opus point.
- [x] object.cc render group → client (`src/object_render.cc` + seam `object_render.h`): _obj_render_pre/post_roof, _obj_render_object, objectDrawOutline, render/blend table init/exit, the 4 pixel blenders, and their render-only statics. CORRECTION: `_obj_light_table_init` + all `_light_*` state STAY in object.cc — they fill the light-sim offsets consumed only by `_obj_adjust_light` (no pixels/palette), not a render table. Seam = 4 core→render calls (render/blend table init/exit from objectsInit/objectsExit) + 12 object.cc statics de-static'd and exposed read-only via object_render.h. Mechanical, replay-identical (13/13 goldens byte-identical). f2_core-no-client-symbols enforcement deferred to the CMake-split task (needs a client-init hook).
- [x] tile.cc render group → client (`src/tile_render.cc` + seam `tile_render.h`): tileWindowRefresh/Rect dispatchers, tileRefreshGame/Mapper, tileRenderRoofsInRect/tileRenderRoof, tileRenderFloorsInRect/tileRenderFloor, _grid_render/_draw_grid, the 5 floor-render struct types + 6 render-only tables. Seam de-statics 15 shared statics (window buffer/pitch/rect/dims, gTileSquares, square-grid dims, roof/grid/enabled flags, the 2 refresh-proc pointers, _tile_grid_blocked/occupied) exposed read-only, plus tileRefreshGame/Mapper de-static'd so core tileInit can wire the proc pointer by name. DEFERRED to CMake step: tileInit still builds the grid source bitmaps (bufferFill/bufferDrawLine ~389-435) — pixel prep left in core for now, buffers exposed extern. Mechanical, replay-identical (13/13 goldens). tile.cc 1984→1295.
- [x] map.cc iso window group → client (`src/map_render.cc` + seam `map_render.h`): isoInit, isoExit, mapScroll, isoWindowRefreshRect/Game/Mapper + render-only statics (gIsoWindow def, gIsoWindowBuffer/Rect/ScrollTimestamp). isoReset STAYS core (H-17 MVAR/LVAR frees). isoEnable/isoDisable/isoIsDisabled + gIsoEnabled STAY core WHOLE (H-18 world-freeze sim authority — comment added at map.cc:231; ticker/mouse/text chrome moves only once the client-init hook lands). Seam de-statics isoWindowRefreshRectGame/Mapper (core _map_init wires _map_scroll_refresh by name) + square_init/mapMakeMapsDirectory (moved isoInit calls them; defs stay core), exposes _map_scroll_refresh read-only. DEFERRED to CMake step: _map_init/_map_exit/mapLoad keep 3 direct gIsoWindow window calls (windowShow/Hide/Fill/Refresh) — route through presenter before f2_core enforcement. Mechanical, replay-identical (13/13 goldens). map.cc 1785→1547.
- [x] inventory.cc modal screens → client (`src/inventory_ui.cc` + seam `inventory_ui.h`): 41 functions moved (7 public open/close entry points keep inventory.h decls; 34 static modal/draw/drag/quantity helpers), all UI geometry #defines/enums/const-tables/statics. Core inventory.cc collapses to 502 lines (equip/data-structure logic only: _inven_wield/unwield, critterGetItem1/2/Armor, _adjust_ac, objectGetCarried*, _inven_find_*). CLEANEST SEAM: sim was already in item.cc (H-6..H-11/H-56), so ZERO core→UI call edges and NO modal-callback risk (quantity picker runs UI-side, passes resolved value into item.cc sim — the Phase-3-friendly shape). Seam = 5 shared slot statics (_inven_dude, _inven_pid, gInventoryArmor/LeftHandItem/RightHandItem) de-static'd in core, extern in inventory_ui.h — note INVERSE flow: UI populates them, core critterGet* reads them. Mechanical, replay-identical (13/13 goldens). inventory.cc 5610→502.
- [x] game.cc UI → client (`src/game_ui.cc` + seam `game_ui.h`): gameHandleKey (clean input dispatcher — moved whole, no split; all switch arms call public subsystem entry points, no inline sim), gameUiDisable/Enable/IsDisabled, showHelp, showQuitConfirmationDialog, showSplash, gameShowDeathDialog + UI-only statics (gGameUiDisabled, VERSION_BUILD_TIME string). Thinnest seam yet: 1 extern (gIsMapper, core-owned) + 1 de-static (showSplash, called by core gameInitWithOptions). gameDbInit STAYS core (its showMesageBox is already an external client call). H-35 non-issue here (gGameDifficulty lives in settings, not game.cc). Mechanical, replay-identical (13/13 goldens). game.cc 1612→~800.
- [x] combat.cc UI → client (`src/combat_ui.cc` + seam `combat_ui.h`): _combat_turn_run, _combat_input, _combat_display (+combatCopy/AddDamage* helpers), _print_tohit, hitLocationGetName, _draw_loc_*, calledShotSelectHitLocation, _combat_outline_on/off, + 6 UI-only statics. CLEAN because H-12 already extracted turn-end authority: the moved pumps route every AP/turn/RNG op through named core fns (combatPlayerTurnShouldBreak/OutOfAp/Resolve, combatAttemptEnd, _scripts_check_state_in_combat, sfall_gl_scr_process_main) — no moved fn touches sim state or RNG. Seam: 4 read-only externs (_combat_turn_running, gCombatMessageList, _list_total, _combat_list) + de-static _combat_input/calledShotSelectHitLocation (core calls them) + de-static combatAttemptEnd/_combat_update_critters_in_los (moved UI calls them, defs stay core). DISTINCT from other splits: combat core↔UI edges are INWARD direct by-name calls BOTH directions — fine for same-binary TU split, but the link-split will need presenter methods/callbacks for _combat_turn_run/_combat_display/_combat_outline_on-off/calledShotSelectHitLocation (presenter does NOT yet cover combat presentation). Mechanical, replay-identical (13/13 goldens). combat.cc 6872→6053.
- [x] worldmap.cc UI → client (`src/worldmap_ui.cc` + seam `worldmap_ui.h`; also NEW `src/worldmap_defs.h`). DONE. VERDICT held: clean mechanical split (H-13 already carved travel sim into core steps; wmWorldMapFunc/wmTownMapFunc are thin drivers). PREREQ discovered: the worldmap domain types (WmGenData/CityInfo/CitySizeDescription/TileInfo/SubtileInfo/EntranceInfo + enums/macros/encounter tables) lived inside worldmap.cc and are used by-value by moved fns — relocated the whole top-of-file type block to worldmap_defs.h (repo `*_defs.h` convention; pure textual move) so both TUs see complete types. Moved 37 fns + 12 UI-only statics. Seam: 12 shared MUTABLE externs (plan said 10; +wmMaxTileNum, +wmLastRndTime — both genuinely core-written AND read by moved wmInterfaceInit), 9 INWARD UI targets, 3 OUTWARD de-static'd core fns. wmRndEncounterOccurred/wmPartyInitWalking/wmPartyWalkingStep/wmGameTimeIncrement stayed core. LIKE combat: INWARD core↔UI edges both directions, none via presenter (link-split needs presenter methods); wmGenData is a mutable sim+chrome struct (flag for headless decomposition). Mechanical, replay-identical (13/13 goldens). worldmap.cc 6787→4456, worldmap_ui.cc 1976, worldmap_defs.h 419.
  MOVE (~40 fns): wmWorldMap/wmWorldMapFunc, wmTownMap/wmTownMapFunc/wmTownMapInit/Refresh/Exit, all wmInterface* (Init/Exit/Refresh/RefreshDate/CenterOnParty/Scroll/ScrollPixel/ScrollTabsStart-Stop-Update/DrawCircleOverlay/DrawSubTileRectFogged/DrawSubTileList/DialSyncTime), wmRefreshInterfaceOverlay, wmInterfaceRefreshCarFuel, wmRefreshTabs/wmMakeTabsLabelList/wmTabsCompareNames/wmFreeTabsLabelList, wmRefreshInterfaceDial, wmMouseBkProc, wmCheckGameEvents, wmTileGrabArt, wmDrawCursorStopped, wmCursorIsVisible, wmFadeOut/In/Reset, wmBlinkRndEncounterIcon + UI-only statics (wmBkWin, wmBkWinBuf, wmInterfaceWasInitialized, wmLabelList/Count, wmTownMapCurArea, wmRndCursorFids, wmTownMapButtonId/SubButtonIds, _backgroundFrmImage, _townFrmImage, wmFaded).
  DEFER — LEAVE CORE (guardrail): wmRndEncounterOccurred is sim-dominant (RNG rolls, encounter tables, XP) — do NOT move; only its wmBlinkRndEncounterIcon/wmFadeOut calls cross the seam. Its showDialogBox accept/reject prompt stays core (shared dbox util feeding sim return). wmPartyInitWalking/wmPartyWalkingStep/wmGameTimeIncrement stay core (sim) but are the source of the inward edges.
  SEAM worldmap_ui.h: (a) SHARED externs — wmGenData is MUTABLE/read-write (sim+chrome fields in one struct — NOT read-only like the others; flag for headless split to decompose), plus wmAreaInfoList/wmMaxAreaNum/wmSphereData/wmTileInfoList/wmNumHorizontalTiles/wmMsgFile/wmWorldOffsetX/Y/gTownMapHotkeysFix; (b) 7 INWARD de-static'd UI targets core calls by name: wmCursorIsVisible, wmInterfaceCenterOnParty, wmInterfaceScrollPixel, wmInterfaceDialSyncTime, wmInterfaceRefreshDate, wmBlinkRndEncounterIcon, wmFadeOut; (c) 3 de-static'd core fns the UI calls (defs stay core): wmMatchWorldPosToArea, wmAreaFindFirstValidMap, wmGetAreaName. Public wrappers wmWorldMap/wmTownMap keep worldmap.h decls (callers: scripts.cc, map.cc). wmMapMusicStart stays core (not moved).
  LIKE combat: INWARD core↔UI by-name edges BOTH directions, none via presenter — fine for same-binary TU split, link-split needs presenter methods. worldmap.cc ~6787 lines.
- [~] animation.cc: DEFERRED to Phase 2 — the AnimationScheduler (REWRITE_PLAN 2.3) subsumes this; animation.cc lives in f2_client for now (it includes SDL). Not extracted in Batch 7 to avoid churning code Phase 2 rewrites.
- [x] CMake f2_core/f2_client library split (REWRITE_PLAN 1.4). Done via CMake OBJECT libraries (not static libs) so every object still links into `fallout2-ce` → byte-identical binary, no static-lib pruning / circular-dep hazards, 13/13 goldens byte-identical. Partition (deterministic generator, all 276 src files classified): `f2_core` (138 files) = SDL-free sim + shared infra reusable by the eventual headless/server binary; `f2_client` (136) = presentation/SDL/input/audio/screens + the 7 extracted *_ui/*_render; executable keeps only main.cc/.h + platform glue. Both libs get the executable's include dirs / link usage-reqs (SDL2/zlib/fpattern) so compilation is unchanged (vendored SDL exposes headers via the SDL2-static target, NOT ${SDL2_INCLUDE_DIRS}). ENFORCEMENT: `scripts/check_core_no_sdl.sh` fails on any direct `#include <SDL...>`/`"SDL..."` in an f2_core source — wired into `scripts/check.sh`; passes today (0 direct SDL in the 138 core files). KNOWN RESIDUAL (deferred to P5 full decoupling, NOT P1): core files still pull SDL TRANSITIVELY via svga.h (object.cc uses _scr_size; worldmap.cc uses getTicks; 5 others have a now-dead svga.h include), and f2_core still references f2_client symbols at link time (the extern seams + combat/worldmap INWARD by-name edges + map.cc's 3 direct gIsoWindow calls + tileInit's grid-bitmap prep). P1.4 only requires "no SDL *include* in core, CI-checked" — met. Zero-client-symbol-refs + the client-init hook are the P5/headless-binary task.

## REVERSE AUDIT RESULTS — see WORKLIST_P1_LEDGER.md

The Batch-7 WFU list was audited in reverse (sim logic hidden in "UI"
functions): 18 extraction tickets (H-1..H-18). RECLASSIFIED — do NOT move
wholesale: `wmWorldMapFunc` (inline travel sim → extract worldmapTick),
`_combat_input` (turn-end authority), `_exit_inventory`/`_setup_inventory`/
`_inven_pickup`/`inventoryOpenLooting`/`inventoryOpenTrade` (equip commit,
loot/barter transactions, and one RNG-consuming explosive handler H-5).
Batch 7 must process the ledger tickets BEFORE or WITH the file moves.

## Cross-cutting risks (from the sweep)
1. `mapLoad` interleaving — presentation events wrap the data read (progress
   handler registration around fileSetReadProgressHandler); preserve order.
2. `_obj_look_at`/`_obj_examine` pass displayMonitorAddMessage as a FUNCTION
   POINTER — presenter conversion changes the callback type, touch all callers.
3. sfxBuild*Name = pure string builders (core data); the PLAY calls are the
   presenter events. interpreter_extra's opSfxBuild* return strings to scripts.
4. Sim calling modals (quantity picker, encounter yes/no) — interim
   requestQuantity/requestConfirm presenter passthrough; real fix is Phase 3.
5. animation.cc refreshes overlap Phase 2 — convert mechanically now, the
   AnimationScheduler subsumes them later.
