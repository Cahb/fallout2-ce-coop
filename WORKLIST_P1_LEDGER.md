# Hidden Rules Ledger — sim logic found inside "UI-only" functions
### Output of the reverse audit over Batch-7 WFU functions (WORKLIST_P1.md)
### Every ticket = a rule/mutation/RNG-consumption that would silently vanish
### from the dedicated server if its host function moved to the client wholesale.

Dispositions: **E2C** extract-to-core (WFU shell calls a new core fn as intent) ·
**RECLASS** function is sim control-flow, do not move wholesale · **HCS** harmless client state.

Severity order for scheduling: H-5 and H-13 first (RNG / time / world-state,
highest silent-divergence risk), then the equip/inventory cluster
(H-3/H-4/H-6/H-9/H-11), then AP/XP/price rules (H-1/H-2/H-7/H-8/H-10/H-12),
then transition/teardown (H-14/H-15/H-17/H-18).

| # | Host (file:lines) | Hidden sim logic | Disposition |
|---|---|---|---|
| H-1 | `inventoryOpen` inventory.cc:571-594 | Quick-Pockets AP cost to open inventory in combat (`4 - 2*rank`), writes dude AP | E2C `inventoryUseCostApply()` |
| H-2 | `inventoryOpenUseItemOn` inventory.cc:2715-2726 | 2-AP gate + debit for use-item-on in combat | E2C into core use-on path |
| H-3 | `_setup_inventory` inventory.cc:1488-1515 | strips equipped items via `itemRemove` into staging globals | RECLASS/E2C equip staging |
| H-4 | `_exit_inventory` inventory.cc:1529-1546 | equip COMMIT: hand/armor flags + `itemAdd` — the real equip mechanic | RECLASS/E2C `equipmentCommit()` |
| H-5 | `_exit_inventory` inventory.cc:1564-1601 | dropped-live-explosive resolution: **statRoll perception per nearby critter (RNG!)**, whoHitMe, `scriptsRequestCombat` | RECLASS/E2C — must run server-side |
| H-6 | `_inven_pickup` inventory.cc:2408-2471 | drag/drop does `itemAdd/itemRemove`, armor swap `_adjust_ac`, `_adjust_fid` | RECLASS/SPLIT like `_move_inventory` |
| H-7 | `inventoryOpenLooting` inventory.cc:4273-4277 | take-all carry-weight rule + `itemMoveAll` | E2C `lootTakeAll()` |
| H-8 | `inventoryOpenLooting` inventory.cc:4470-4476 | steal-XP rule `min(300 - skill, xp)` + `pcAddExperience` | E2C into steal resolution |
| H-9 | `inventoryOpenLooting` inventory.cc:4102-4467 | `scriptExecProc(PICKUP)`, hidden-item moves, target equip strip/re-equip | RECLASS/E2C (pair H-3/H-4) |
| H-10 | `inventoryOpenTrade` inventory.cc:5090-5126 | barter price modifier from NPC reaction {+25/0/-15}, auto-walkaway ≤ -30 | E2C barter valuation |
| H-11 | `inventoryOpenTrade` inventory.cc:5035-5348 | barter transaction, table transfers, equip churn | RECLASS/E2C |
| H-12 | `_combat_input` combat.cc:3167,3191 | turn-end rule (ap<=0 && free_move<=0), clears `COMBAT_STATE_0x08` | RECLASS — turn manager is server authority |
| H-13 | `wmWorldMapFunc` worldmap.cc:3025-3251 | **inline worldmap travel sim**: car speed from GVARs, `wmCarUseGas(100)`, out-of-gas state, party rest-heal timer, exploration marking, `wmGameTimeIncrement(18000)`/step, encounter dispatch, area visited-state | RECLASS whole fn → extract `worldmapTick(dt)` |
| H-14 | `wmInterfaceInit` worldmap.cc:4459,4730 | `_map_save_in_game(true)`, `scriptsDisable()`, `_scr_remove_all()` on map→worldmap | E2C boundary transition |
| H-15 | `wmInterfaceExit` worldmap.cc:4812-4830 | encounter-state reset + `scriptsEnable()` on worldmap→map | E2C (pair H-14) |
| H-16 | `wmMarkSubTileRadiusVisited` worldmap.cc:5064-5073 | Scout-perk exploration radius rule + subtile visited state (called from H-13 loop) | keep core; call from `worldmapTick` |
| H-17 | `isoExit` map.cc:276-279 | frees MVARs/LVARs — server-side map teardown must do this or leak/stale vars | E2C core map teardown |
| H-18 | `isoEnable/isoDisable` map.cc:331,343 | `_scr_enable/_scr_disable_critters` — the "world freeze during modals" mechanism | E2C core "suspend world sim" toggle |

## Ring 3 — role-triaged files (H-30..H-35)

| # | Host (file:lines) | Hidden sim logic | Disposition |
|---|---|---|---|
| H-30 | `game_movie.cc:270,324-327` | `gGameMoviesSeen[]` is savegame state scripts query (`gameMovieIsSeen` opcode) — playback client-side, seen-flag must be core | RECLASS/E2C `gameMovieMarkSeen()` |
| H-31 | `game_mouse.cc:1964-2315` | hex/bouncing cursors are real Objects inserted into the spatial index (NO_BLOCK/SHOOT_THRU so LOS safe) — server must own ZERO cursor objects; tile-list iteration counts could diverge | RECLASS invariant |
| H-32 | `game_mouse.cc:1029-1038,930-945` | inline combat-AP gate + debit in click handlers (same class as H-2/H-12); move-AP budget computed client-side | RECLASS/E2C into use/move resolution |
| H-33 | `character_selector.cc:558-566` | `characterSelectorWindowRefresh` calls `_proto_dude_init` — full player init masquerading as a display refresh | E2C `dudeInitFromPremade()` server-owned |
| H-34 | `game_sound.cc:2111-2159` | ambient-SFX event draws **shared RNG twice per cycle** and re-queues itself on the persistent game event queue (EVENT_TYPE_GSOUND_SFX_EVENT is saved!) — silent desync class, same as H-5 | RECLASS — one authority + isolated RNG substream, or off the shared queue |
| H-35 | `preferences.cc:781-782` | writes `game_difficulty`/`combat_difficulty` which sim rules read (combat.cc, skill.cc, worldmap.cc + script opcodes) | SETTINGS-OWNERSHIP — server is config authority for these two; rest of screen client-only |

Ring 3 clean files (audited, no findings): options.cc, mainmenu.cc, credits.cc,
dialog.cc, dbox.cc, movie.cc, display_monitor.cc, text_object.cc, cycle.cc.

## Ring 2 — modal-screen files (H-40..H-60; renumbered from agent's H-19..H-39)

| # | Host (file:lines) | Hidden sim logic | Disposition |
|---|---|---|---|
| H-40 | `pipboyRest` pipboy.cc:1968-2174 | **REST simulation** (peer of H-13): loops `gameTimeSetTime` + `queueProcessEvents`, heals via `_AddHealth`; rest-until-healed formula `hpToHeal/healingRate*3.0` | RECLASS → core `restTick(dt)`/`restUntilHealed()` |
| H-41 | `_Check4Health`/`_AddHealth` pipboy.cc:2177-2199 | heal cadence (1 tick / 180 min) + amount (`_partyMemberRestingHeal(3)`) | E2C `restHealStep()` |
| H-42 | `pipboyHandleRest`+`_ClacTime` pipboy.cc:1770-1839,2202-2229 | rest-duration tables incl. until-morning/noon wall-clock math; `_critter_can_obj_dude_rest` gate | RECLASS rest-intent decoder |
| H-43 | `characterEditorUpdateLevel` character_editor.cc:5681-5746 | **level-up award**: SP = `5 + INT*2 + Educated*2 + Skilled*5 − Gifted*5` cap 99; perk cadence every 3 (4 if Skilled) levels | E2C `pcLevelUpApply()` — HIGH severity |
| H-44 | `perkDialogShow` character_editor.cc:5931-5959 | perk commit + specials: LIFEGIVER +4 maxHP, EDUCATED +2 SP, TAG, MUTATE | E2C `perkChoiceApply()` |
| H-45 | `characterEditorHandleAdjustSkillButtonPressed` cc:5158-5288 | `skillAdd/skillSub` spending unspent SP; floor = entry backup | E2C skill-spend intent |
| H-46 | `characterEditorAdjustPrimaryStat` cc:3709-3786 | creation SPECIAL spend + char-point budget | RECLASS (creation) |
| H-47 | `perkDialogHandleMutatePerk` cc:6338-6448 | MUTATE trait swap via `traitsSetSelected` | E2C (pair H-44) |
| H-48 | `perkDialogHandleTagPerk` cc:6471-6504 | TAG 4th tag skill via `skillsSetTagged` | E2C (pair H-44) |
| H-49 | `characterEditorRestorePlayer`+`_pop_perks` cc:4838-4893,6713-6736 | cancel-rollback restores name/SP/tags/traits/HP/perks — editor is a **transaction** | RECLASS — server needs begin/commit/rollback model |
| H-50 | `characterEditorWindowFree` cc:1880-1885 | creation-exit commit: tags/traits + full-heal | E2C `characterCreationCommit()` |
| H-51 | `characterEditorDrawSkills` cc:2968 | **mutation in a RENDER helper**: `skillsSetTagged` applied on every redraw | RECLASS — commit out of draw path |
| H-52 | `characterEditorShowOptions` cc:4038-4095 | premade GCD load: `_ResetPlayer`/`gcdLoad`/`pcStatsReset`/`perksReset`/full-heal | RECLASS (creation tooling) |
| H-53 | `characterEditorEditAge/Gender` cc:3555,3671,3690 | live `critterSetBaseStat(AGE/GENDER)` | RECLASS (creation) |
| H-54 | `gameDialogEnter` game_dialog.cc:725-801 | dialog driver: `scriptExecProc(TALK)` (all NPC script side effects), isoDisable/Enable world-freeze | RECLASS — talk execution is server sim; heads/window client |
| H-55 | `_gdialog_barter_cleanup_tables` gd.cc:3321-3350 | `itemMoveForce` returns barter-table items | RECLASS (pair H-11) |
| H-56 | `_gdialog_barter_create_win` gd.cc:3189-3260 | creates hidden barter staging Objects | RECLASS (pair H-3 staging) |
| H-57 | `elevatorSelectLevel` elevator.cc:338-477+123-268 | elevator **destination table** {map,elev,tile} + Sierra/MilBase remap math | E2C `elevatorResolveDestination()` |
| H-58 | loadsave.cc pipeline (lsgPerformSaveGame/lsgLoadGameInSlot/_GameMap2Slot/_SlotMap2Game + master lists) | authoritative (de)serialization incl. `gameTimeSetTime`, `_combat_over_from_load` | RECLASS → core persistence service; slot UI client |
| H-59 | automap.cc persistence (save/load/decode/entries) | `automap.db` explored-map state, called from map.cc + scripts | RECLASS — core, do not ship client-only |
| H-60 | `automapShow` automap.cc:316 | `_obj_process_seen()` sets OBJECT_SEEN as render-entry side effect | RECLASS/verify — server marks SEEN via core path |
| H-61 | `sfall_arrays.cc:43-45` ARRAY_ACTION_SHUFFLE | **script-visible RNG outside the pool**: sfall array-shuffle opcode uses `std::random_device`+`mt19937` — non-reproducible across runs; breaks replay for any mod script using it (found by full-project non-pool-RNG sweep 2026-07-11; window_manager.cc rand() jitter = render-only, OK; random.cc libc use = seeding-internal, OK) | E2C later — route through pool getRandom or an isolated seeded substream; defer until a mod-scripting pass |

Ring 2 clean: skilldex.cc (pure picker → intent), endgame.cc (reads GVARs to
pick slides; terminal cosmetic RNG only). HCS noted: pipboy screensaver RNG,
dialog fidget/msg-pick RNG — cosmetic but consume the shared stream (same
class as H-34): isolate to a client RNG when split.

**Cross-cutting (peer of H-18):** every modal calls isoDisable/isoEnable
(world-freeze). When these UIs move client-side, the freeze must become a
server-side suspend/resume driven by the session — critical for the
time-advancing ones (REST H-40, dialog H-54).

## Confirmed clean (worth recording)
- `_combat_display`: reads stats/perks ONLY to pick message strings; no RNG, no
  mutation → safe client-side narration.
- `calledShotSelectHitLocation`: `_determine_to_hit` path verified RNG-free;
  returns hit location as pure intent.
- `gameHandleKey`: pure dispatch, EXCEPT `,`/`.` rotate keys mutate dude
  rotation via animation — must become a rotate-intent, not dropped.
- `_combat_outline_on/off`, `mapScroll`, `_container_enter/exit`, viewport
  scrolling: harmless client state.
- `isoInit`: no rules, but binds `objectsInit/tileInit` to the render buffer —
  the headless server needs an equivalent init decoupled from the window
  (already partially proven by the Phase-0 probe).
