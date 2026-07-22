# PLAYER_ACTION_MAP.md — player-input → engine-action coverage map

**Purpose.** This is **surface 2 of 3** of the headless-decoupling effort. Where
`SCRIPT_OPCODE_MAP.md` (surface 1) bounds how *scripts* drive the engine through
181 opcode handlers, this document bounds how the **player** drives the sim
through the UI — every world click, combat sub-action, inventory op, skill,
character-editor edit, rest, and worldmap travel that mutates game state
**without going through a script opcode**. (Surface 3 — passive / time-driven
sim: queue events, ambient AI, radiation/poison ticks — is a separate map and is
not covered here.)

The load-bearing question for every row is the **Convergence** column: does this
player action funnel through the *same engine function* as an opcode already
classified in `SCRIPT_OPCODE_MAP.md`? If yes, the opcode-layer decouple already
covers it and there is nothing new to do. The delta this map exists to surface is
the set of player actions with **no opcode analog** that still need attention.

## Headless-class schema (identical to the opcode map)

- **PURE-SIM** — mutates sim state directly, no animation/UI/blocking dependency
  → headless-safe today.
- **ANIM-WELDED** — the outcome is welded to a `reg_anim` completion callback
  that never fires headless → needs a `serverLoopActive()` fast-path. Reference
  DONE examples: `_obj_use_door`/`objectOpenClose` (`proto_instance.cc`),
  `actionPickUp` + `_action_use_an_item_on_object` (`actions.cc:1185/1078`),
  `_combat_apply_attack_results(animated=false)` (`combat.cc:3504`).
- **MODAL** — opens a blocking input loop headless (inventory/barter/loot/
  worldmap/pipboy/char-editor/dialog/save-load) → needs an intent driver.
- **PURE-UI** — presentation only (cursor FRM, float text, HUD refresh) →
  safe/no-op headless.
- **TIME-RNG** — advances game time / draws RNG / schedules queue events → works
  headless but affects determinism cadence (the documented golden carve-out).

## Column definitions

- **Convergence** — `CONVERGENT (0xNNNN op_name)` if the action funnels through
  the same engine fn as a mapped opcode (inherits that opcode's DONE/NOT-STARTED
  from `SCRIPT_OPCODE_MAP.md`); `player-only` if there is **no** opcode analog
  (character progression, inventory-drag glue, combat hit-mode selection — things
  scripts never do). This is the key column.
- **Decouple** — `DONE` (cite site) / `NOT-STARTED` / `—` (no decouple needed).
  For movement/combat-move, `DONE (scheduler)` = handled by the server loop's
  fixed-timestep instant animation scheduler, not a per-call fast-path.
- **Probe-verb** — the `src/main.cc` headless-probe verb that exercises this
  engine path, if any (`walkto/pickup/climb/usedoor/useitem/usedrug/drop/give/
  wield/unload/reload/stow/lootall/stealall/useskill/rest/restopt/levelup/perk/
  tag4/mutate/charsnap/charroll/cattack/cmove/cendturn/wmtravel/hurt/xp/rad/…`).
- **Golden** — `COVERED` (a server golden drives this exact engine path),
  `INDIRECT` (a golden touches the same substrate via a different verb), `NONE`.
- `?` flags a low-confidence / verify row.

Reference decouple sites (verified this pass):
`actions.cc:1078` (`_action_use_an_item_on_object` → `_obj_use`/`_obj_use_item_on`),
`actions.cc:1185` (`actionPickUp` → `_obj_pickup`; container branch declines),
`combat.cc:3475/3504` (`_action_attack` skipped, `_combat_apply_attack_results(false)`),
`combat_ui.cc:200` (`_combat_input` server intent-queue driver).

---

## Master table (by System)

### World / mouse — `src/game_mouse.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Walk to tile | game_mouse.cc:938/943 | `_dude_move` → `animationRegisterMoveToTile` | movement | ANIM-WELDED | player-only (op_move_to teleports, different path) | DONE (scheduler) | walkto | COVERED (arvillag_walk) |
| Run to tile | game_mouse.cc:948 | `_dude_run` | movement | ANIM-WELDED | player-only | DONE (scheduler) | walkto | INDIRECT |
| Pick up ground item | game_mouse.cc:957/1204 | `actionPickUp` → `_obj_pickup` | inventory/item | ANIM-WELDED | CONVERGENT (0x80D6 op_pickup_obj) | **DONE** (actions.cc:1185) | pickup | COVERED (denbus1_pickup) |
| Use scenery (door/ladder/stairs/lever) | game_mouse.cc:983/1198 | `_action_use_an_object` → `_obj_use` (→`_obj_use_door`/`useLadder`/`useStairs`) | map/scenery | ANIM-WELDED | CONVERGENT (0x80DB op_use_obj; 0x8131/0x8132 open/close) | **DONE** (actions.cc:1078) | usedoor / climb | COVERED (kladwtwn_door_open, denbus1_climb) |
| Use active weapon/item on object | game_mouse.cc:1031/1042 | `_action_use_an_item_on_object` → `_obj_use_item_on` | map/scenery | ANIM-WELDED | CONVERGENT (0x8145 op_use_obj_on_obj) | **DONE** (actions.cc:1078) | useitem | COVERED (arvillag_invmenu useitem:79) |
| Attack target (crosshair) | game_mouse.cc:1010 | `_combat_attack_this` → `_combat_attack` | combat | ANIM-WELDED | CONVERGENT (0x80D0/0x8143 op_attack/attack_setup) | **DONE** (combat.cc:3504) | cattack | COVERED (arvillag_gunfight) |
| Talk to critter | game_mouse.cc:974/1193 | `actionTalk` → `_talk_to` → gameDialog | dialog | ANIM-WELDED→MODAL | CONVERGENT (0x80DE op_start_gdialog + gsay family) | NOT-STARTED | — | NONE |
| Loot critter (dead) / open container-critter | game_mouse.cc:977/1201 | `_action_loot_container` → `scriptsRequestLooting` → `inventoryOpenLooting` | inventory/loot | ANIM-WELDED→MODAL | partial (primitives via lootall; modal analog = 0x80D6 container branch) | NOT-STARTED (modal); primitives DONE | lootall/stealall | COVERED (denbus1_loot) — primitives only |
| Use skill on object/critter | game_mouse.cc:1059/1241 | `actionUseSkill` → `_obj_use_skill_on` (callback3) | skill | ANIM-WELDED | **player-only (no opcode analog)** | **NOT-STARTED** | — (useskill drives self only) | NONE |
| Sneak toggle | game_mouse.cc:1215 | `_action_skill_use(SKILL_SNEAK)` | skill | PURE-SIM | player-only | — | — | NONE |
| Push critter | game_mouse.cc:1246 | `actionPush` | anim/movement | ANIM-WELDED | **player-only (no opcode analog)** | **NOT-STARTED** | — | NONE |
| Rotate self (face) | game_mouse.cc:963/1188 | `objectRotateClockwise` | map/tile | PURE-SIM | player-only (cosmetic facing) | — | — | NONE |
| Look / examine | game_mouse.cc:746/985/1183 | `_obj_examine` / `_obj_look_at` | sound/UI | PURE-UI | player-only | — | — | NONE |
| Open "use item on" (INVENTORY verb) | game_mouse.cc:1180 | `inventoryOpenUseItemOn` | inventory | MODAL | player-only | NOT-STARTED (modal; item outcome DONE) | — | INDIRECT |
| Open skilldex (USE_SKILL verb) | game_mouse.cc:1212 | `skilldexOpen` | skill/UI | MODAL | player-only | NOT-STARTED | — | NONE |
| Right-click cycle cursor mode | game_mouse.cc:920 | `gameMouseCycleMode` | sound/UI | PURE-UI | player-only | — | — | NONE |
| Action verb-menu (right-click picker) | game_mouse.cc:1135 (blocking loop) | `inputGetInput` loop → chosen `actionIndex` | sound/UI | MODAL (picker) | player-only | NOT-STARTED (synthesize choice) | — | NONE |

### Combat sub-actions — `src/combat_ui.cc` + `src/combat.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Attack / fire weapon | combat_ui.cc:240 (server intent) / game_mouse.cc:1010 | `_combat_attack` → `_combat_apply_attack_results(false)` | combat | ANIM-WELDED | CONVERGENT (0x80D0/0x8143 op_attack) | **DONE** (combat.cc:3504) | cattack | COVERED (arvillag_gunfight) |
| Combat move (walk-to, spend AP) | combat_ui.cc:223 (MOVE intent) | `animationRegisterMoveToTile` + `_combat_turn_run` | movement | ANIM-WELDED | player-only | DONE (scheduler; combat_ui.cc:104) | cmove | INDIRECT (gunfight uses cattack) |
| End turn / end combat | combat_ui.cc:219 (END_TURN intent) / combat.cc:3033 | `combatAttemptEnd` / `combatPlayerTurnResolve` | combat | PURE-SIM | CONVERGENT (0x8153 op_terminate_combat family) | — | cendturn (auto-end default) | COVERED (klatoxcv_combat, restfight) |
| Called / aimed-shot target select | combat_ui.cc:782 | `calledShotSelectHitLocation` (WINDOW_MODAL loop) | combat | MODAL | **player-only (no opcode analog)** | **NOT-STARTED** (server always UNCALLED) | — | NONE |
| Hit-mode select (single/burst/aimed, primary↔secondary) | interface.cc:1211 (key N) | `interfaceCycleItemAction` / `interfaceGetCurrentHitMode` | sound/UI | PURE-UI selector | player-only (server uses `serverDudeHitMode`, combat_ui.cc:140) | — (bypassed) | — | INDIRECT |
| Swap active hand (L/R slot) | interface.cc:1172 (key B) | `interfaceBarSwapHands` (toggles `gInterfaceCurrentHand`) | sound/UI | PURE-UI selector | player-only | — | — | NONE |
| Reload (in combat / bar) | interface.cc:2019 | `_intface_item_reload` → `weaponAttemptReload` (AP charged inline) | inventory/item | PURE-SIM | CONVERGENT (item-reload mutator, shared w/ inventory reload) | — | reload | COVERED (arvillag_gunfight, arvillag_invmenu) |
| Rotate / change facing | game_mouse.cc:963 | `objectRotateClockwise` | map/tile | PURE-SIM | player-only | — | — | NONE |

### Inventory ops — `src/inventory_ui.cc` + `src/item.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Open inventory screen | inventory_ui.cc:548 | `inventoryOpen` (`for(;;) inputGetInput` @589) | inventory | MODAL | player-only | NOT-STARTED (modal; all mutators PURE-SIM) | — | INDIRECT |
| Move item between slots/containers (drag) | inventory_ui.cc:2175 `_inven_pickup` | drag loop → `itemMove`/`itemAdd`/`itemRemove` | inventory/item | MODAL wrapping PURE-SIM | CONVERGENT (0x8147 move_inven_to_obj / 0x80D8 add_to_inven) | mutators — ; drag loop NOT-STARTED | give / drop | COVERED (arvillag_invmenu) |
| Wield/arm weapon | inventory_ui.cc:2397 `_switch_hand` (UI) ; engine `_inven_wield` (inventory.cc:304) | slot statics + `itemAdd/Remove`; probe uses `_invenWieldFunc` | inventory/item | PURE-SIM | CONVERGENT (0x80DA op_wield_obj_critter) | — | wield | COVERED (arvillag_gunfight wield:1) |
| Unwield | `_op_inven_unwield` / `_inven_unwield` (inventory.cc:452) | slot clear | inventory/item | PURE-SIM | CONVERGENT (0x812C op_inven_unwield) | — | — | INDIRECT |
| Drop item to ground | inventory_ui.cc:3381 | `itemDropStack` → `_obj_drop` | inventory/item | PURE-SIM (qty picker MODAL) | CONVERGENT (0x80D7 op_drop_obj) | — | drop | COVERED (arvillag_invmenu drop) |
| Use item on self (drug/misc) | inventory_ui.cc:3406 | `itemUseDrug` / `itemUseFromInventory` → `_protinst_use_item` | inventory/item | PURE-SIM | partial (shares item-effect mutators; no exact opcode) | — | usedrug / useitem | COVERED (arvillag_invmenu usedrug:40) |
| Use item on target object | inventory_ui.cc:2582 (in `inventoryOpenUseItemOn`) | `actionUseItemOnObjectWithApCost` → `_action_use_an_item_on_object` | map/scenery | ANIM-WELDED (decoupled); screen MODAL | CONVERGENT (0x8145 op_use_obj_on_obj) | outcome **DONE** (actions.cc:1078); screen NOT-STARTED | useitem | COVERED |
| Unload weapon | inventory_ui.cc:3427 | `weaponUnloadIntoInventory` → `weaponUnload` + `itemAdd` | inventory/item | PURE-SIM | partial (weapon-unload mutator) | — | unload | COVERED (arvillag_invmenu unload:8) |
| Stow into container | inventory_ui.cc:4655 `_drop_into_container` | `containerStoreItem` → `itemAttemptAdd` | inventory/item | PURE-SIM (inside drag MODAL) | CONVERGENT (0x8147 move_inven_to_obj) | — | stow | COVERED (arvillag_invmenu stow:53) |
| Examine / look at item | inventory_ui.cc:3116 `inventoryExamineItem` | text render | sound/UI | PURE-UI | player-only | — | — | NONE |
| Quantity picker (drop/move N) | inventory_ui.cc:4767 `inventoryQuantitySelect` | `for(;;)` @4789 | inventory | MODAL | player-only | NOT-STARTED (bypassed) | — | NONE |

### Barter — `src/inventory_ui.cc` + `src/item.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Barter/trade screen | inventory_ui.cc:4252 `inventoryOpenTrade` | `for(;;) inputGetInput` @4314 | dialog/barter | MODAL | CONVERGENT (0x8129 op_gdialog_barter) | NOT-STARTED | — | NONE |
| Offer / execute transaction | inventory_ui.cc:4339 (key M) | `barterAttemptTransaction` (item.cc:3761) | inventory/item | PURE-SIM | CONVERGENT (barter mutator) | — | — | NONE |
| Value computation | inventory_ui.cc:4229 | `barterComputeValue` (item.cc:3715) | inventory/item | PURE-SIM | player-only | — | — | NONE |

### Loot / steal — `src/inventory_ui.cc` + `src/item.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Loot screen (container/corpse) | inventory_ui.cc:3455 `inventoryOpenLooting` | `for(;;) inputGetInput` @3601 | inventory/loot | MODAL | partial (via 0x80D6 container branch, which declines headless) | NOT-STARTED | — | NONE |
| Steal screen | inventory_ui.cc:3826 `inventoryOpenStealing` | delegates to `inventoryOpenLooting` (`_gIsSteal`) | inventory/loot | MODAL | player-only | NOT-STARTED | — | NONE |
| Loot open-check (runs PICKUP script) | item.cc:3535 `lootOpenCheck` | `SCRIPT_PROC_PICKUP`, scriptOverrides | inventory/loot | PURE-SIM (script side-effects) | player-only | — | lootall/stealall | COVERED (denbus1_loot) |
| Take-all | item.cc:3658 `lootTakeAll` → `itemMoveAll` | carry-weight gated | inventory/item | PURE-SIM | CONVERGENT (item move mutators) | — | lootall/stealall | COVERED (denbus1_loot) |
| Steal XP / caught reaction | item.cc:3675 `lootStealExperience` / :3642 `lootCaughtStealingReact` | XP add / PICKUP proc | critter/stat | PURE-SIM | partial (0x80A1 give_exp) | — | stealall | INDIRECT |

### Skill — `src/skill.cc` + skilldex UI + `src/actions.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Skilldex picker | game_mouse.cc:1212 | `skilldexOpen` | skill/UI | MODAL | player-only | NOT-STARTED | — | NONE |
| Use skill on self | (probe) → `skillUse(gDude,gDude,skill,0)` | `skillUse` (skill.cc) | skill | PURE-SIM+TIME-RNG | CONVERGENT (0x80AC roll_vs_skill / skillUse core) | — | useskill | COVERED (arvillag_skills useskill:6/7/8) |
| Use skill on target | game_mouse.cc:1241 | `actionUseSkill` → `_obj_use_skill_on` (callback3) | skill | ANIM-WELDED | **player-only (no opcode analog)** | **NOT-STARTED** | — | NONE |

### Character editor — `src/character_editor.cc` + `src/character_transaction.cc`

Transaction-commit model (H-47/48/49): the editor commits pending edits on
**Save** (`characterEditorSave` @5632); the perk sub-dialog commits via
`perkChoiceApply` @5884. The probe drives the **extracted commit primitives**
directly (never the modal), which is exactly how each golden pins them.

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Open char editor | character_editor.cc:782 `characterEditorShow` | modal window + input loop | critter/stat | MODAL | player-only | NOT-STARTED (commits reachable directly) | — | INDIRECT |
| Level-up commit | (probe) `pcLevelUpApply` (stat.cc:647) | applies level, HP, skill points, free-perk flag | critter/stat | PURE-SIM | **player-only (no opcode analog)** | — | levelup | COVERED (arvillag_actions, arvillag_chartxn) |
| Perk selection commit | (probe) `perkChoiceApply` (perk.cc:510) | `perkAdd` + effects; reports pending Tag!/Mutate! | critter/stat | PURE-SIM | player-only | — | perk | COVERED (arvillag_perks) |
| Tag 4th skill (Tag! perk) | (probe) `skillsTagPerkApply` (skill.cc:226) | sets 4th tagged skill | skill | PURE-SIM | player-only | — | tag4 | COVERED (arvillag_perks tag4:12) |
| Mutate! trait swap | (probe) `traitsMutateDrop`/`traitsMutateGain` + `traitsSetSelected` | drops/gains trait | critter/stat | PURE-SIM | player-only | — | mutate | COVERED (arvillag_perks mutate:2) |
| SPECIAL stat +/- (creation) | character_editor.cc:924/3698 | `characterEditorAdjustPrimaryStat` → `critterIncBaseStat`/`critterDecBaseStat` | critter/stat | PURE-SIM (immediate) inside MODAL sub-loop | player-only | — | — | INDIRECT |
| Skill-point +/- commit | character_editor.cc:5140/5196 | `characterEditorHandleAdjustSkillButtonPressed` → `skillAdd` (spends `PC_STAT_UNSPENT_SKILL_POINTS`) | skill | PURE-SIM (immediate) inside MODAL sub-loop | player-only | — | — | INDIRECT |
| Tag-skill toggle (creation) | character_editor.cc:1150/5273 | `characterEditorToggleTaggedSkill` (temp array) → `skillsSetTagged` at save | skill | PURE-UI/pending (commit at save) | player-only | — | — | INDIRECT |
| Stat/skill-point snapshot & rollback | character_transaction.cc:14/34 | `characterSnapshotTake`/`Restore(+SkillPoints/HitPoints)` | critter/stat | PURE-SIM | player-only | — | charsnap / charroll | COVERED (arvillag_chartxn) |

### Rest — `src/pipboy.cc`

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Rest fixed duration | pipboy.cc:1752 `pipboyRest` (countdown loops @1966/2005) | extracted `restSimPacing`/`restSimMinutesTick`/`restSimHoursTick`/`…Finish` | time/queue | MODAL (UI); helpers PURE-SIM+TIME-RNG | CONVERGENT (0x80FC op_game_time_advance) | NOT-STARTED (modal; helpers driven directly) | rest | COVERED (arvillag_rest) |
| Rest until healed / until time | pipboy.cc rest-option branches | same helpers + heal-loop | time/queue | MODAL; helpers PURE-SIM | CONVERGENT (0x80FC) | NOT-STARTED (helpers driven directly) | restopt | COVERED (arvillag_rest, arvillag_restfight) |
| Rest-here gate | pipboy.cc:591/1752 | `_critter_can_obj_dude_rest` | time/queue | PURE-SIM | player-only | — | (probe owns gate) | COVERED |

### Worldmap travel — `src/worldmap_ui.cc` + `src/worldmap.cc`

The CE tree extracted the vanilla travel body into five pure helpers
(`worldmapTravel*`); the probe's `wmtravel` driver calls them directly, replacing
the MODAL `wmWorldMapFunc` loop.

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Enter worldmap | worldmap_ui.cc:126 `wmWorldMap`→`wmWorldMapFunc(0)` (loop @152) | modal input+render loop | worldmap | MODAL | CONVERGENT (0x8108 op_scripts_request_world_map) | NOT-STARTED | — | NONE |
| Start travel to destination | worldmap_ui.cc:267 `wmPartyInitWalking` (worldmap.cc:3588) | sets walk destination / Bresenham deltas | worldmap | PURE-SIM | player-only | — | wmtravel | COVERED (arvillag_wmtravel) |
| Step party across map | worldmap.cc:2495 `worldmapTravelStep` → `wmPartyWalkingStep` | mutates world pos, walk distance, fuel | worldmap | PURE-SIM | player-only | — | wmtravel | COVERED |
| Advance clock per step | worldmap.cc:2590 `worldmapTravelClockTick` → `wmGameTimeIncrement` | `gameTimeAddTicks` + `queueProcessEvents` | time/queue | TIME-RNG | CONVERGENT (0x80FC game_time_advance) | — | wmtravel | COVERED |
| Rest-heal cadence | worldmap.cc:2562 `worldmapTravelRestHeal` → `_partyMemberRestingHeal` | HP mutation (wall-clock gated) | party | PURE-SIM | player-only | — | wmtravel | COVERED |
| Mark tiles visited | worldmap.cc:2576 `worldmapTravelMarkVisited` | `wmMarkSubTileRadiusVisited` | worldmap | PURE-SIM | CONVERGENT (0x80B2 mark_area_known) | — | wmtravel | COVERED |
| Random-encounter roll | worldmap.cc:2601 `worldmapTravelEncounterCheck` → `wmRndEncounterOccurred` | `randomBetween`, encounter pick | worldmap | TIME-RNG | player-only | — | wmtravel | COVERED (roll only) |
| Random-encounter map entry | worldmap_ui.cc:205 `mapLoadById(encounterMap)` | map load / transition | map/worldmap | MODAL/I-O | cross-cut (see notes; ties to 0x80E4 op_load_map) | NOT-STARTED | — | NONE |
| Enter town/location | worldmap_ui.cc:1396 `wmTownMapFunc` (loop @1411) → `mapLoadById` | nested modal + map load | map/worldmap | MODAL | cross-cut (map transition) | NOT-STARTED | — | NONE |

### Save / load — `src/loadsave.cc` (OUT-OF-SCOPE for sim goldens)

| Action | Trigger (@ file:line) | Engine fn(s) | System | Headless-class | Convergence | Decouple | Probe-verb | Golden |
|---|---|---|---|---|---|---|---|---|
| Save screen | loadsave.cc:363 `lsgSaveGame` (loop @509) | slot UI + input | misc/meta | MODAL | player-only | out-of-scope | — | NONE |
| Load screen | loadsave.cc:879 `lsgLoadGame` (loop @1013) | slot UI + input | misc/meta | MODAL | player-only | out-of-scope | — | NONE |
| Perform save (primitive) | loadsave.cc:1531 `lsgPerformSaveGame` | disk write `SAVE.DAT`, `SaveGameHandler`s | misc/meta | I/O | player-only (serializes, no sim mutation) | out-of-scope | — | NONE |
| Perform load (primitive) | loadsave.cc:1699 `lsgLoadGameInSlot` | disk read, `LoadGameHandler`s, `_combat_over_from_load` | misc/meta | I/O | player-only | out-of-scope | — | NONE |

---

## Remaining player-input work (the actionable delta)

These are the **ANIM-WELDED-NOT-STARTED** and **MODAL** actions that have **no
opcode analog already covered by the opcode-layer decouples** — i.e. work this
surface adds beyond `SCRIPT_OPCODE_MAP.md`. Actions that are CONVERGENT with an
opcode (attack, pickup, use-obj, use-obj-on-obj, dialog, barter, worldmap-enter,
rest-time-advance) are already tracked in the opcode map and are **not** repeated
as new work here.

### A. Genuinely-new gameplay decouples (no opcode analog, sim NOT yet reachable)

| # | Action | Engine fn | Class | Why it's new | Priority |
|---|---|---|---|---|---|
| 1 | **Use skill on target** (lockpick/steal/first-aid/doctor/science/repair/traps on an object or critter) | `actionUseSkill` → `_obj_use_skill_on` (actions.cc:1370, callback3) | ANIM-WELDED | Scripts never call `actionUseSkill`; the `useskill` probe only drives `skillUse` **on self**. Skill-on-target (the primary skilldex use case) is welded to an anim callback with no `serverLoopActive()` branch → silently dropped headless. Fix: mirror `actionPickUp`/`_action_use_an_item_on_object` — a `serverLoopActive()` fast-path calling `_obj_use_skill_on` directly. | **HIGH** |
| 2 | **Called / aimed-shot selection** | `calledShotSelectHitLocation` (combat_ui.cc:782) | MODAL | The server combat driver always passes `HIT_LOCATION_UNCALLED`; aimed attacks (and their crit-table effects) are unreachable headless. Needs the hit-location fed as an attack-intent field instead of the modal picker. | **MED** |
| 3 | **Push critter** | `actionPush` (actions.cc:2082) | ANIM-WELDED | No opcode analog; welded to a move anim, no server branch. Minor gameplay surface. | **LOW** |

### B. Modal-screen drivers with no opcode analog (sim already reachable via probe primitives — UI-glue only)

These MODAL screens have **no** opcode analog, but every state mutation behind
them is already PURE-SIM and pinned by a probe verb / golden. They only need a
driver if a golden must exercise the exact UI glue (item selection, drag, qty).

| # | Modal screen | Entry | Sim already covered by |
|---|---|---|---|
| 4 | Inventory screen | `inventoryOpen` (548) | give/drop/wield/unload/stow/useitem/usedrug probes (arvillag_invmenu) |
| 5 | Use-item-on picker | `inventoryOpenUseItemOn` (2498) | `useitem` verb → decoupled `_action_use_an_item_on_object` |
| 6 | Loot / steal screen | `inventoryOpenLooting`/`Stealing` (3455/3826) | `lootall`/`stealall` verbs → `lootTakeAll` (denbus1_loot) |
| 7 | Skilldex picker | `skilldexOpen` | `useskill` verb → `skillUse` (arvillag_skills) |
| 8 | Character editor | `characterEditorShow` (782) | levelup/perk/tag4/mutate/charsnap/charroll (arvillag_perks, arvillag_chartxn) |
| 9 | Pipboy rest UI | `pipboyRest` (1752) | rest/restopt verbs → `restSim*` (arvillag_rest) |
| 10 | Quantity picker | `inventoryQuantitySelect` (4767) | (drop/move default to full stack in probe) |
| 11 | Action verb-menu picker | game_mouse.cc:1135 | verb outcomes driven directly by their probes |

### C. Convergent with opcode-map backlog (already tracked there — listed for cross-reference, NOT new)

- **Dialog** (`actionTalk` → `_talk_to`) ≡ opcode dialog driver (0x80DE + gsay
  family, MODAL NOT-STARTED). Same dialog-intent driver project.
- **Barter** (`inventoryOpenTrade`) ≡ 0x8129 `op_gdialog_barter` (MODAL).
- **Worldmap enter** (`wmWorldMapFunc`) ≡ 0x8108 `op_scripts_request_world_map`
  (MODAL). Travel *sim* is already covered by the `wmtravel` probe.
- **Rest / time-advance** (`restSim*`, `wmGameTimeIncrement`) ≡ 0x80FC
  `op_game_time_advance` (TIME-RNG). Already covered by rest/wmtravel goldens.
- **Map transition** (random-encounter entry, town enter, `mapLoadById`) ≡ the
  `op_load_map` (0x80E4) cross-cutting note in the opcode map — cross-map exits
  are not driven headless yet in either surface.

---

## Cross-cutting notes

- **Movement is handled by the scheduler, not per-call fast-paths.** `_dude_move`/
  `_dude_run` and combat-move are ANIM-WELDED, but the server loop's
  fixed-timestep **instant animation scheduler** runs the registered move to
  completion within the tick (that is why the `walkto`/`cmove` intents work and
  the `arvillag_walk` golden passes). So movement needs no `serverLoopActive()`
  branch — the scheduler is its decouple. `_combat_turn_run` (combat_ui.cc:104)
  has the matching server drain.

- **The player attack path fully converges on the opcode combat decouple.** The
  crosshair attack (`_combat_attack_this`) and the server intent-queue attack
  (`_combat_input` → `_combat_attack`) both resolve through
  `_combat_apply_attack_results(false)` (combat.cc:3504) — the same Phase 2.4
  outcome decouple that makes `op_attack`/`op_attack_setup` DONE. The
  `arvillag_gunfight` golden additionally pins that killed critters are finalized
  as corpses via `critterKill` on the server path.

- **Inventory / item mutation is already PURE-SIM.** The CE refactor moved every
  mutator out of `inventory_ui.cc` into `item.cc` (`itemMove*`, `itemDropStack`,
  `itemUseFromInventory`, `weaponUnloadIntoInventory`, `containerStoreItem`,
  `lootTakeAll`, `barterAttemptTransaction`, …), all synchronous with no
  `reg_anim`/window calls. Only the outer screens remain MODAL. This is why the
  entire `arvillag_invmenu` action stream (give/drop/wield/unload/reload/stow/
  usedrug/useitem/lootall/stealall) works headless without touching a modal.

- **Character progression is a player-only surface with no opcode analog.** No
  script opcode calls `pcLevelUpApply`/`perkChoiceApply`/`skillsTagPerkApply`/
  `traitsMutate*` — level-up, perks, tags, and mutations are exclusively
  player-driven through the char editor. They are all PURE-SIM and fully pinned
  by the `levelup`/`perk`/`tag4`/`mutate`/`charsnap`/`charroll` probes; only the
  `characterEditorShow` modal is undriven (and unnecessary for sim goldens).

- **Wall-clock coupling in worldmap travel.** Two travel gates read `getTicks()`
  real time (rest-heal cadence @worldmap.cc:2564, encounter throttle @2643), so
  the `wmtravel` probe depends on `F2_FAKE_CLOCK=1` turning `getTicks()` into a
  deterministic call-counter. A real server travel driver must replace these, not
  feed them the sim clock.

- **Save/Load is out of scope.** The screens are MODAL and the primitives are
  pure disk I/O; they serialize/deserialize sim state rather than mutate it, so
  they are listed for completeness but excluded from sim-golden coverage.

---

## Summary counts

Rows counted from the master table (excluding save/load, which is out-of-scope):

| Headless-class | Count | Notes |
|---|---:|---|
| PURE-SIM | 29 | inventory/item mutators, character-progression commits (incl. SPECIAL/skill-point +/-), worldmap step helpers, reload, end-turn, sneak/rotate |
| ANIM-WELDED | 9 | walk, run, pickup, use-obj, use-item-on-obj, attack, combat-move, use-skill-on-target, push |
| MODAL | 18 | inventory/use-item/loot/steal/qty/skilldex/char-editor/verb-menu/rest/barter/dialog(talk)/worldmap/town/encounter-entry |
| PURE-UI | 7 | look/examine, cursor-cycle, hit-mode select, swap-hand, item-examine, tag-toggle(pending) |
| TIME-RNG | 3 | worldmap clock tick, encounter roll (+ rest helpers overlap PURE-SIM) |

**CONVERGENT-DONE (already covered by an opcode-layer decouple)** — 6 distinct
player actions inherit a DONE opcode decouple and are pinned by a server golden:
pickup (0x80D6), use-scenery/door/ladder (0x80DB/0x8131/0x8132), use-item-on-object
(0x8145), attack + combat move-to-attack (0x80D0/0x8143), reload (item mutator),
plus movement via the scheduler. Combined with the PURE-SIM player surfaces that
are pinned by their own probes (drop, wield, unwield, stow, unload, use-item-self,
skill-use-self, level-up, perk, tag4, mutate, char-txn, rest, worldmap-travel,
loot/steal-all, end-turn), **~24 player actions are already covered** (CONVERGENT-
DONE or PURE-SIM-with-golden).

**Genuinely-new work (Section A)** — **3 decouples**: use-skill-on-target
(HIGH), called/aimed-shot selection (MED), push (LOW).

**Modal-driver-only work (Section B)** — **8 MODAL screens** whose sim is already
reachable via probe primitives; needed only to exercise UI glue, not to make the
sim reachable.

**Convergent-with-opcode-backlog (Section C)** — dialog, barter, worldmap-enter,
time-advance, and map-transition are the same 5 projects already enumerated in
`SCRIPT_OPCODE_MAP.md`; no new work is created by the player surface for them.

**Bottom line:** the player-input surface adds essentially **one HIGH-value new
gameplay decouple (use-skill-on-target)** plus a MED aimed-shot selector and a LOW
push, on top of the opcode-map backlog. Everything else a player can do is either
already decoupled at the opcode layer, already PURE-SIM and golden-pinned via a
probe, or a modal-UI wrapper around sim state that is already reachable.
