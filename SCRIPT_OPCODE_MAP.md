# SCRIPT_OPCODE_MAP.md — script-opcode → engine-system coverage map

**Purpose.** Scripts are how *all* game content (quests, critters, environmental
interactions, quirks) drives the engine, and a script can only touch the world
through the opcode handlers. So a complete opcode map is the complete map of
what must work headless. This document bounds the headless-decoupling effort:
every game-facing opcode is classified by which engine system it drives and by
whether that path already works under the fixed-timestep server loop
(`F2_SERVER_LOOP=1`, see `SERVER_LOOP_DESIGN.md`).

The **181 game opcodes** are registered in `src/interpreter_extra.cc`
(`interpreterRegisterOpcode(0x80A1…0x8155)`, in `interpreterRegisterOpcodeHandlers`
around line 4872). The **78 core-VM opcodes** in `src/interpreter.cc`
(math / stack / flow / var access) are headless-trivial and summarised as one
group at the end.

## Column definitions

- **System** — combat · inventory/item · critter/stat · map/tile/scenery ·
  worldmap · gvar/mvar/lvar · anim/movement · dialog · sound/UI · time/queue ·
  party · skill · misc/meta.
- **Kind** — `GENERAL` (a normal engine service) vs `QUIRK` (QUIRK-ENABLER: lets
  scripts do arbitrary / special-case things — force-set flags, arbitrary anim,
  debug/meta grab-bags).
- **Headless-class** (the load-bearing column):
  - `PURE-SIM` — mutates sim state directly, no animation/UI/blocking dependency
    → already headless-safe.
  - `ANIM-WELDED` — the outcome is welded to a `reg_anim` completion callback
    that never fires headless → needs/needed a `serverLoopActive()` fast-path.
  - `MODAL` — opens a blocking input loop headless (dialog/barter/loot/worldmap/
    elevator/endgame) → needs an intent driver.
  - `PURE-UI` — presentation only (float text, console msg, sfx, fades, gfx,
    tile refresh) → safe/no-op headless.
  - `TIME-RNG` — advances time / draws RNG / schedules queue events → works but
    affects determinism cadence.
  - A secondary tag is noted where relevant (e.g. `PURE-SIM+TIME-RNG`).
- **Decouple** — for ANIM-WELDED/MODAL: `NOT-STARTED` | `DONE` (cite site). `—`
  for classes that need no decouple.
- **Golden** — `COVERED` (a headless golden drives this exact path),
  `INDIRECT` (a golden exercises the same engine substrate via a probe verb, not
  the opcode's own glue), `NONE`.
- `?` flags a low-confidence / verify row.

Reference DONE sites: combat outcome decouple `_combat_apply_attack_results(false)`
gated by `serverLoopActive()` in `src/combat.cc:3475/3504`; door decouple in
`_obj_use_door`/`objectOpenClose` (`src/proto_instance.cc:1763/2090`);
pickup/use-object/ladder fast-paths in `actionPickUp` (`src/actions.cc:1185`) and
`_action_use_an_item_on_object` (`src/actions.cc:1078`).

---

## Master table (all 181 game opcodes, by opcode)

| Opcode | Keyword | Handler (interpreter_extra.cc) | System | Kind | Headless-class | Decouple | Golden |
|---|---|---|---|---|---|---|---|
| 0x80A1 | op_give_exp_points | opGiveExpPoints @466 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT (xp) |
| 0x80A2 | op_scr_return | opScrReturn @477 | misc/meta | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80A3 | op_play_sfx | opPlaySfx @491 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x80A4 | op_obj_name | opGetObjectName @4841 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80A5 | op_sfx_build_open_name | opSfxBuildOpenName @4401 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x80A6 | op_get_pc_stat | opGetPcStat @4855 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80A7 | op_tile_contains_pid_obj | opTileGetObjectWithPid @4818 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80A8 | op_set_map_start | opSetMapStart @500 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80A9 | op_override_map_start | opOverrideMapStart @523 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80AA | op_has_skill | opHasSkill @561 | skill | GENERAL | PURE-SIM | — | NONE |
| 0x80AB | op_using_skill | opUsingSkill @580 | skill | GENERAL | PURE-SIM | — | NONE |
| 0x80AC | op_roll_vs_skill | opRollVsSkill @599 | skill | GENERAL | PURE-SIM+TIME-RNG | — | INDIRECT (useskill) |
| 0x80AD | op_skill_contest | opSkillContest @624 | skill | GENERAL | PURE-SIM+TIME-RNG | — | NONE |
| 0x80AE | op_do_check | opDoCheck @638 | skill | GENERAL | PURE-SIM+TIME-RNG | — | NONE |
| 0x80AF | op_success | opSuccess @674 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80B0 | op_critical | opCritical @696 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80B1 | op_how_much | opHowMuch @718 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80B2 | op_mark_area_known | opMarkAreaKnown @738 | worldmap | GENERAL | PURE-SIM | — | NONE |
| 0x80B3 | op_reaction_influence | opReactionInfluence @761 | dialog | GENERAL | PURE-SIM | — | NONE |
| 0x80B4 | op_random | opRandom @775 | misc/meta | GENERAL | TIME-RNG | — | INDIRECT |
| 0x80B5 | op_roll_dice | opRollDice @790 | misc/meta | GENERAL | TIME-RNG | — | NONE |
| 0x80B6 | op_move_to | opMoveTo @805 | map/movement | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80B7 | op_create_object | opCreateObject @864 | map/tile | GENERAL | PURE-SIM | — | INDIRECT (give) |
| 0x80B8 | op_display_msg | opDisplayMsg @1014 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x80B9 | op_script_overrides | opScriptOverrides @1027 | misc/meta | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80BA | op_obj_is_carrying_obj | opObjectIsCarryingObjectWithPid @1041 | inventory/item | GENERAL | PURE-SIM | — | NONE |
| 0x80BB | op_tile_contains_obj_pid | opTileContainsObjectWithPid @1058 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80BC | op_self_obj | opGetSelf @1080 | misc/meta | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80BD | op_source_obj | opGetSource @1088 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80BE | op_target_obj | opGetTarget @1106 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80BF | (op_dude_obj) | opGetDude @1124 | misc/meta | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80C0 | op_obj_being_used_with | opGetObjectBeingUsed @1133 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80C1 | op_get_local_var | opGetLocalVar @1151 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80C2 | op_set_local_var | opSetLocalVar @1167 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80C3 | op_get_map_var | opGetMapVar @1178 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | NONE |
| 0x80C4 | op_set_map_var | opSetMapVar @1193 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | NONE |
| 0x80C5 | op_get_global_var | opGetGlobalVar @1203 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80C6 | op_set_global_var | opSetGlobalVar @1223 | gvar/mvar/lvar | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80C7 | op_script_action | opGetScriptAction @1243 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80C8 | op_obj_type | opGetObjectType @1261 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80C9 | op_item_subtype | opGetItemType @1275 | inventory/item | GENERAL | PURE-SIM | — | NONE |
| 0x80CA | op_get_critter_stat | opGetCritterStat @1294 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80CB | op_set_critter_stat | opSetCritterStat @1314 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80CC | animate_stand_obj | opAnimateStand @1340 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x80CD | animate_stand_reverse_obj | opAnimateStandReverse @1364 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x80CE | animate_move_obj_to_tile | opAnimateMoveObjectToTile @1388 | anim/movement | QUIRK | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x80CF | tile_in_tile_rect | opTileInTileRect @1437 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80D0 | op_attack | opAttackComplex @1814 | combat | GENERAL | ANIM-WELDED | DONE (combat.cc:3504) | COVERED (gunfight) |
| 0x80D1 | op_make_daytime | opMakeDayTime @1467 | time/queue | GENERAL | PURE-SIM (empty) | — | NONE |
| 0x80D2 | op_tile_distance | opTileDistanceBetween @1473 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80D3 | op_tile_distance_objs | opTileDistanceBetweenObjects @1491 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80D4 | op_tile_num | opGetObjectTile @1515 | map/tile | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80D5 | op_tile_num_in_direction | opGetTileInDirection @1531 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80D6 | op_pickup_obj | opPickup @1562 | inventory/item | GENERAL | ANIM-WELDED | DONE (actions.cc:1185) | COVERED (pickup) |
| 0x80D7 | op_drop_obj | opDrop @1598 | inventory/item | GENERAL | PURE-SIM (_obj_drop) | — | INDIRECT (drop) |
| 0x80D8 | op_add_obj_to_inven | opAddObjectToInventory @1626 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT (give) |
| 0x80D9 | op_rm_obj_from_inven | opRemoveObjectFromInventory @1649 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80DA | op_wield_obj_critter | opWieldItem @1690 | inventory/item | GENERAL | PURE-SIM (_inven_wield) | — | COVERED (wield) |
| 0x80DB | op_use_obj | opUseObject @1751 | map/scenery | GENERAL | ANIM-WELDED | DONE (actions.cc:1078) | COVERED (climb) |
| 0x80DC | op_obj_can_see_obj | opObjectCanSeeObject @1784 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80DD | op_attack | opAttackComplex @1814 (alias) | combat | GENERAL | ANIM-WELDED | DONE (combat.cc:3504) | COVERED (gunfight) |
| 0x80DE | op_start_gdialog | opStartGameDialog @1894 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x80DF | op_end_gdialog | opEndGameDialog @1949 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x80E0 | op_dialogue_reaction | opGameDialogReaction @1959 | dialog | GENERAL | PURE-SIM (dialog-ctx) | — | NONE |
| 0x80E1 | op_metarule3 | opMetarule3 @1969 | misc/meta | QUIRK | PURE-SIM | — | INDIRECT |
| 0x80E2 | op_set_map_music | opSetMapMusic @2065 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x80E3 | op_set_obj_visibility | opSetObjectVisibility @2081 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80E4 | op_load_map | opLoadMap @2128 | map/worldmap | GENERAL | PURE-SIM (defers MapTransition) | — | NONE (see cross-cut) |
| 0x80E5 | op_wm_area_set_pos | opWorldmapCitySetPos @2167 | worldmap | GENERAL | PURE-SIM | — | NONE |
| 0x80E6 | op_set_exit_grids | opSetExitGrids @2181 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80E7 | op_anim_busy | opAnimBusy @2202 | anim/movement | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x80E8 | op_critter_heal | opCritterHeal @2218 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT (hurt) |
| 0x80E9 | op_set_light_level | opSetLightLevel @2234 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80EA | op_game_time | opGetGameTime @2270 | time/queue | GENERAL | PURE-SIM (getter) | — | INDIRECT |
| 0x80EB | op_game_time_in_seconds | opGetGameTimeInSeconds @2278 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x80EC | op_elevation | opGetObjectElevation @2286 | map/tile | GENERAL | PURE-SIM | — | INDIRECT (climb) |
| 0x80ED | op_kill_critter | opKillCritter @2302 | combat/critter | GENERAL | PURE-SIM (critterKill direct) | — | INDIRECT (combat) |
| 0x80EE | op_kill_critter_type | opKillCritterType @2365 | combat/critter | QUIRK | PURE-SIM | — | NONE |
| 0x80EF | op_critter_damage | opCritterDamage @2472 | combat | GENERAL | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x80F0 | op_add_timer_event | opAddTimerEvent @2510 | time/queue | GENERAL | TIME-RNG | — | NONE |
| 0x80F1 | op_rm_timer_event | opRemoveTimerEvent @2526 | time/queue | GENERAL | TIME-RNG | — | NONE |
| 0x80F2 | op_game_ticks | opGameTicks @2543 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x80F3 | op_has_trait | opHasTrait @2561 | critter/stat | GENERAL | PURE-SIM | — | NONE |
| 0x80F4 | op_destroy_object | opDestroyObject @952 | map/tile | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80F5 | op_obj_can_hear_obj | opObjectCanHearObject @2621 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80F6 | op_game_time_hour | opGameTimeHour @2646 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x80F7 | op_fixed_param | opGetFixedParam @2654 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80F8 | op_tile_is_visible | opTileIsVisible @2672 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x80F9 | op_dialogue_system_enter | opGameDialogSystemEnter @2686 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x80FA | op_action_being_used | opGetActionBeingUsed @2715 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x80FB | critter_state | opGetCritterState @2733 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT |
| 0x80FC | op_game_time_advance | opGameTimeAdvance @2762 | time/queue | GENERAL | TIME-RNG | — | INDIRECT (rest) |
| 0x80FD | op_radiation_inc | opRadiationIncrease @2780 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT (rad) |
| 0x80FE | op_radiation_dec | opRadiationDecrease @2795 | critter/stat | GENERAL | PURE-SIM | — | NONE |
| 0x80FF | critter_attempt_placement | opCritterAttemptPlacement @2813 | map/movement | GENERAL | PURE-SIM | — | NONE |
| 0x8100 | op_obj_pid | opGetObjectPid @2836 | misc/meta | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8101 | op_cur_map_index | opGetCurrentMap @2852 | map/tile | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8102 | op_critter_add_trait | opCritterAddTrait @2860 | critter/stat | QUIRK | PURE-SIM | — | NONE |
| 0x8103 | critter_rm_trait | opCritterRemoveTrait @2930 | critter/stat | QUIRK | PURE-SIM | — | NONE |
| 0x8104 | op_proto_data | opGetProtoData @2963 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x8105 | op_message_str | opGetMessageString @2986 | sound/UI | GENERAL | PURE-SIM | — | NONE |
| 0x8106 | op_critter_inven_obj | opCritterGetInventoryObject @3010 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8107 | op_obj_set_light_level | opSetObjectLightLevel @3059 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x8108 | op_scripts_request_world_map | opWorldmap @3084 | worldmap | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x8109 | op_inven_cmds | _op_inven_cmds @3091 | inventory/item | GENERAL | PURE-SIM (index getter) | — | NONE |
| 0x810A | op_float_msg | opFloatMessage @3114 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x810B | op_metarule | opMetarule @3198 | misc/meta | QUIRK | PURE-SIM (+MODAL: elevator) | **NOT-STARTED** (elevator sub-rule) | INDIRECT |
| 0x810C | op_anim | opAnim @3353 | anim/movement | QUIRK | ANIM-WELDED (death/knockdown fid; rot/frame subcmds PURE) | **NOT-STARTED** | NONE |
| 0x810D | op_obj_carrying_pid_obj | opObjectCarryingObjectByPid @3436 | inventory/item | GENERAL | PURE-SIM | — | NONE |
| 0x810E | op_reg_anim_func | opRegAnimFunc @3453 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x810F | op_reg_anim_animate | opRegAnimAnimate @3475 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x8110 | op_reg_anim_animate_reverse | opRegAnimAnimateReverse @3494 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x8111 | op_reg_anim_obj_move_to_obj | opRegAnimObjectMoveToObject @3511 | anim/movement | QUIRK | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x8112 | op_reg_anim_obj_run_to_obj | opRegAnimObjectRunToObject @3528 | anim/movement | QUIRK | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x8113 | op_reg_anim_obj_move_to_tile | opRegAnimObjectMoveToTile @3545 | anim/movement | QUIRK | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x8114 | op_reg_anim_obj_run_to_tile | opRegAnimObjectRunToTile @3562 | anim/movement | QUIRK | ANIM-WELDED | **NOT-STARTED** | NONE |
| 0x8115 | op_play_gmovie | opPlayGameMovie @3579 | sound/UI | GENERAL | PURE-UI (guarded, game_movie.cc:148) | — | NONE |
| 0x8116 | op_add_mult_objs_to_inven | opAddMultipleObjectsToInventory @3628 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT (give) |
| 0x8117 | rm_mult_objs_from_inven | opRemoveMultipleObjectsFromInventory @3654 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8118 | op_month | opGetMonth @3691 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8119 | op_day | opGetDay @3701 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x811A | op_explosion | opExplosion @3711 | combat | GENERAL | ANIM-WELDED+TIME-RNG ? | **NOT-STARTED** ? | NONE |
| 0x811B | op_days_since_visited | opGetDaysSinceLastVisit @3732 | time/queue | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x811C | op_gsay_start | _op_gsay_start @3747 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x811D | op_gsay_end | _op_gsay_end @3761 | dialog | GENERAL | MODAL (_gdialogGo loop) | **NOT-STARTED** | NONE |
| 0x811E | op_gsay_reply | _op_gsay_reply @3770 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x811F | op_gsay_option | _op_gsay_option @3791 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x8120 | op_gsay_message | _op_gsay_message @3828 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x8121 | op_giq_option | _op_giq_option @3853 | dialog | GENERAL | MODAL | **NOT-STARTED** | NONE |
| 0x8122 | op_poison | opPoison @3906 | critter/stat | GENERAL | PURE-SIM | — | INDIRECT (poison) |
| 0x8123 | op_get_poison | opGetPoison @3923 | critter/stat | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8124 | op_party_add | opPartyAdd @3943 | party | GENERAL | PURE-SIM | — | NONE |
| 0x8125 | op_party_remove | opPartyRemove @3956 | party | GENERAL | PURE-SIM | — | NONE |
| 0x8126 | op_reg_anim_animate_forever | opRegAnimAnimateForever @3969 | anim/movement | QUIRK | PURE-UI | — | NONE |
| 0x8127 | op_critter_injure | opCritterInjure @3985 | combat/critter | GENERAL | PURE-SIM | — | NONE |
| 0x8128 | op_is_in_combat | opCombatIsInitialized @4017 | combat | GENERAL | PURE-SIM (getter) | — | INDIRECT |
| 0x8129 | op_gdialog_barter | _op_gdialog_barter @4024 | dialog | GENERAL | MODAL (barter UI) | **NOT-STARTED** | NONE |
| 0x812A | op_game_difficulty | opGetGameDifficulty @4035 | misc/meta | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x812B | op_running_burning_guy | opGetRunningBurningGuy @4042 | misc/meta | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x812C | op_inven_unwield | _op_inven_unwield @4048 | inventory/item | GENERAL | PURE-SIM | — | NONE |
| 0x812D | op_obj_is_locked | opObjectIsLocked @4065 | map/scenery | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x812E | op_obj_lock | opObjectLock @4081 | map/scenery | GENERAL | PURE-SIM | — | NONE |
| 0x812F | op_obj_unlock | opObjectUnlock @4094 | map/scenery | GENERAL | PURE-SIM | — | NONE |
| 0x8130 | op_obj_is_open | opObjectIsOpen @4107 | map/scenery | GENERAL | PURE-SIM (getter) | — | INDIRECT (door) |
| 0x8131 | op_obj_open | opObjectOpen @4123 | map/scenery | GENERAL | ANIM-WELDED | DONE (proto_instance.cc:2090) | COVERED (door) |
| 0x8132 | op_obj_close | opObjectClose @4136 | map/scenery | GENERAL | ANIM-WELDED | DONE (proto_instance.cc:2090) | COVERED (door) |
| 0x8133 | op_game_ui_disable | opGameUiDisable @4149 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8134 | op_game_ui_enable | opGameUiEnable @4156 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8135 | op_game_ui_is_disabled | opGameUiIsDisabled @4163 | sound/UI | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8136 | op_gfade_out | opGameFadeOut @4170 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8137 | op_gfade_in | opGameFadeIn @4183 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8138 | op_item_caps_total | opItemCapsTotal @4196 | inventory/item | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8139 | op_item_caps_adjust | opItemCapsAdjust @4212 | inventory/item | GENERAL | PURE-SIM | — | NONE |
| 0x813A | op_anim_action_frame | _op_anim_action_frame @4229 | anim/movement | GENERAL | PURE-SIM (art getter) | — | NONE |
| 0x813B | op_reg_anim_play_sfx | opRegAnimPlaySfx @4253 | sound/UI | QUIRK | PURE-UI | — | NONE |
| 0x813C | op_critter_mod_skill | opCritterModifySkill @4273 | skill | GENERAL | PURE-SIM | — | INDIRECT |
| 0x813D | op_sfx_build_char_name | opSfxBuildCharName @4323 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x813E | op_sfx_build_ambient_name | opSfxBuildAmbientName @4341 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x813F | op_sfx_build_interface_name | opSfxBuildInterfaceName @4352 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8140 | op_sfx_build_item_name | opSfxBuildItemName @4363 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8141 | op_sfx_build_weapon_name | opSfxBuildWeaponName @4374 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8142 | op_sfx_build_scenery_name | opSfxBuildSceneryName @4388 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8143 | op_attack_setup | opAttackSetup @4418 | combat | GENERAL | ANIM-WELDED | DONE (combat.cc:3504) | COVERED (gunfight) |
| 0x8144 | op_destroy_mult_objs | opDestroyMultipleObjects @4469 | map/tile | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8145 | op_use_obj_on_obj | opUseObjectOnObject @4529 | map/scenery | GENERAL | ANIM-WELDED | DONE (actions.cc:1078) | COVERED (useitem) |
| 0x8146 | op_endgame_slideshow | opEndgameSlideshow @4571 | sound/UI | GENERAL | MODAL (endgame) | **NOT-STARTED** | NONE |
| 0x8147 | op_move_obj_inven_to_obj | opMoveObjectInventoryToObject @4580 | inventory/item | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8148 | op_endgame_movie | opEndgameMovie @4632 | sound/UI | GENERAL | MODAL (endgame) | **NOT-STARTED** | NONE |
| 0x8149 | op_obj_art_fid | opGetObjectFid @4641 | misc/meta | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x814A | op_art_anim | opGetFidAnim @4657 | misc/meta | GENERAL | PURE-SIM | — | NONE |
| 0x814B | op_party_member_obj | opGetPartyMember @4665 | party | GENERAL | PURE-SIM | — | NONE |
| 0x814C | op_rotation_to_tile | opGetRotationToTile @4675 | map/tile | GENERAL | PURE-SIM | — | NONE |
| 0x814D | op_jam_lock | opJamLock @4686 | map/scenery | GENERAL | PURE-SIM | — | NONE |
| 0x814E | op_gdialog_set_barter_mod | opGameDialogSetBarterMod @4695 | dialog | GENERAL | PURE-SIM (setter) | — | NONE |
| 0x814F | op_combat_difficulty | opGetCombatDifficulty @4704 | misc/meta | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8150 | op_obj_on_screen | opObjectOnScreen @4711 | sound/UI | GENERAL | PURE-UI (getter) | — | NONE |
| 0x8151 | op_critter_is_fleeing | opCritterIsFleeing @4738 | combat | GENERAL | PURE-SIM (getter) | — | NONE |
| 0x8152 | op_critter_set_flee_state | opCritterSetFleeState @4754 | combat | GENERAL | PURE-SIM | — | NONE |
| 0x8153 | op_terminate_combat | opTerminateCombat @4772 | combat | GENERAL | PURE-SIM | — | INDIRECT |
| 0x8154 | op_debug_msg | opDebugMessage @4789 | sound/UI | GENERAL | PURE-UI | — | NONE |
| 0x8155 | op_critter_stop_attacking | opCritterStopAttacking @4803 | combat | GENERAL | PURE-SIM | — | NONE |

> Note on Golden column: the headless goldens (`tests/golden/run_golden_server.sh`)
> drive *engine functions* through probe verbs (`xp`→`pcAddExperience`,
> `hurt`→`critterAdjustHitPoints`, `give`→`itemAdd`, `wield`→`_inven_wield`,
> `usedoor`→`_obj_use_door`, `pickup`→`actionPickUp`, `climb`/`useitem`→
> `_action_use_an_item_on_object`, `cattack`→combat), **not** the opcodes
> themselves. So `COVERED` means the opcode's decoupled outcome path is pinned by
> a golden that shares that substrate; `INDIRECT` means only the underlying
> mutator is exercised (not the opcode's own arg-marshalling/glue). No golden
> loads a `.int` script and dispatches opcodes directly yet.

---

## Remaining headless work (the actionable backlog)

### ANIM-WELDED — NOT-STARTED
These weld their sim outcome to a `reg_anim` completion callback that never
fires headless; each needs a `serverLoopActive()` fast-path (pattern:
`_combat_apply_attack_results(false)` / `_obj_use_door` / `actionPickUp`).

| Opcode | Keyword | What is dropped headless | Priority |
|---|---|---|---|
| 0x80EF | op_critter_damage | `actionDamage(animated=true)` → `_apply_damage` runs only in the `_report_dmg` anim callback; scripted damage/death is silently dropped. **The one general-purpose gameplay op still broken.** | HIGH |
| 0x811A | op_explosion | `scriptsRequestExplosion` resolves via the combat/anim path; area damage + death likely dropped headless. Verify against the combat explosion resolution. | HIGH ? |
| 0x810C | op_anim | Arbitrary anim play; the death/knockdown **fid change** is anim-welded (rotation `anim==1000` and frame `anim==1010` subcommands are already PURE-SIM). Cosmetic-leaning; knockdown flag itself is set immediately. | MED |
| 0x80CE | animate_move_obj_to_tile | Ambient/scripted NPC repositioning; final tile only applied on anim completion. Skipped in combat. | LOW |
| 0x8111 | op_reg_anim_obj_move_to_obj | Scripted NPC move — final position welded to anim completion. | LOW |
| 0x8112 | op_reg_anim_obj_run_to_obj | as above | LOW |
| 0x8113 | op_reg_anim_obj_move_to_tile | as above | LOW |
| 0x8114 | op_reg_anim_obj_run_to_tile | as above | LOW |

The five `reg_anim_*` movers + `animate_move_obj_to_tile` share one fix: a
`serverLoopActive()` branch that applies the destination via `objectSetLocation`
instead of registering an animation (all are `!isInCombat()`-gated cosmetic/
ambient scripted motion, so "teleport to destination" is the correct headless
semantics).

### MODAL — NOT-STARTED (need an intent driver)
Two subsystems dominate:

**Game dialog** (the designated NEXT subsystem — see the dialog-headless plan):
- 0x80DE `op_start_gdialog`, 0x80DF `op_end_gdialog`, 0x80F9 `op_dialogue_system_enter`
- 0x811C `op_gsay_start`, 0x811D `op_gsay_end` (the blocking `_gdialogGo` option loop),
  0x811E `op_gsay_reply`, 0x811F `op_gsay_option`, 0x8120 `op_gsay_message`, 0x8121 `op_giq_option`
- 0x8129 `op_gdialog_barter` (barter UI, `inventory_ui.cc`)

Blocking loops live in `_gdProcess` (`src/game_dialog.cc`) and barter/loot in
`src/inventory_ui.cc`. Plan: a `dialog_intent` queue that selects reply options
without the input loop.

**Worldmap / travel:**
- 0x8108 `op_scripts_request_world_map` (`worldmap_ui.cc` travel loop)

**Elevator (sub-rule):**
- 0x810B `op_metarule` → `METARULE_ELEVATOR` → `scriptsRequestElevator` (elevator UI).

**Endgame (terminal, low priority):**
- 0x8146 `op_endgame_slideshow`, 0x8148 `op_endgame_movie`.

Still-pending modal loops referenced by the plan and not reachable via any
opcode above but part of the same driver surface: pipboy rest (`src/pipboy.cc`),
loot/inventory (`src/inventory_ui.cc`).

---

## Cross-cutting notes

- **Leaving a map while in combat ends/evades combat** (combat × map-transition
  quirk to preserve). `op_load_map` (0x80E4) only defers a `MapTransition` via
  `mapSetTransition`; the actual load runs in the game loop, and the transition
  path tears combat down. Under the server loop, cross-map exits are **not driven
  yet** — a candidate golden once map transitions are drivable headless. Also
  `op_metarule3` `METARULE_111` (`_map_target_load_area`) and exit-grid opcodes
  (`op_set_exit_grids`) feed this same machinery.

- **reg_anim art-load-failure → dropped-outcome.** Even on the client,
  `reg_anim_end()` can reject a sequence when required art is missing (esp.
  ranged attack/projectile fids); the welded completion callback then never runs
  and the outcome is silently lost. This is the *same* failure mode as headless
  (the callback simply never fires) — it is why the decouple pattern applies the
  computed outcome directly with `animated=false` rather than trying to make the
  anim complete. See the combat comment at `src/combat.cc:3470`.

- **TIME-RNG cadence coupling.** `op_random`/`op_roll_dice` draw the shared RNG;
  `op_game_time_advance`/`op_add_timer_event`/`op_rm_timer_event` mutate the
  queue and game clock; the skill rolls (`op_roll_vs_skill`, `op_skill_contest`,
  `op_do_check`) also draw RNG. These all *work* headless but the server golden
  set legitimately diverges from the legacy frame-driven golden on
  timing/RNG-cadence fields (game_time, rng_state, queue timestamps) — this is
  the documented equivalence-review carve-out, not a bug.

- **Combat request vs. resolution.** `op_attack`/`op_attack_setup`/`op_explosion`
  only *request* combat/explosion (`scriptsRequestCombat`/`scriptsRequestExplosion`);
  the resolution runs later in the combat driver, where the Phase 2.4 outcome
  decouple (`_combat_apply_attack_results(false)`) applies. So the "attack"
  opcodes are DONE by virtue of the combat-loop decouple, while `op_critter_damage`
  (which calls `actionDamage` **directly**, not through the combat loop) is the
  outlier still welded to an anim callback.

---

## Summary counts (primary headless-class)

| Class | Count | Notes |
|---|---:|---|
| PURE-SIM | 122 | getters + var/stat/inventory/map mutators; headless-safe today |
| PURE-UI | 25 | sfx builders, float/console msg, fades, ui-disable, reg_anim cosmetic, guarded movie |
| ANIM-WELDED | 16 | 8 DONE (attack ×2, pickup, use_obj, obj_open, obj_close, attack_setup, use_obj_on_obj), 8 NOT-STARTED |
| MODAL | 13 | dialog ×10, worldmap ×1, endgame ×2 (+ elevator as a metarule sub-rule) |
| TIME-RNG | 5 | random, roll_dice, add/rm_timer_event, game_time_advance |
| **Total game opcodes** | **181** | registered `0x80A1…0x8155` in `interpreter_extra.cc` |

**Remaining decouple backlog: 8 ANIM-WELDED-NOT-STARTED + 13 MODAL = 21 opcodes**
(plus the elevator metarule sub-rule). Of these, only `op_critter_damage` (and
possibly `op_explosion`) are general-purpose gameplay ops; the other six
ANIM-WELDED are cosmetic/ambient scripted motion sharing a single fix, and the
MODAL set collapses into two driver projects (dialog+barter, worldmap) plus the
terminal endgame path.

---

## Core-VM opcodes (`src/interpreter.cc`, 78 total) — summarized group

Registered in `interpreterRegisterOpcodeHandlers` (`src/interpreter.cc:2520+`)
as symbolic `OPCODE_*` constants (not `0x80xx`). These are the language runtime:
stack ops (`PUSH`/`POP`/`DUP`/`SWAP`), control flow (`JUMP`/`CALL`/`CALL_AT`/
`CALL_WHEN`/`EXEC`/`SPAWN`/`FORK`/`EXIT`/`DETACH`), procedure/return-flag
plumbing (`POP_RETURN`/`POP_FLAGS*`/`POP_EXIT`), variable access
(`FETCH_GLOBAL`/`STORE_GLOBAL`/`FETCH_EXTERNAL`/`STORE_EXTERNAL`/`EXPORT_*`),
type conversion (`A_TO_D`/`D_TO_A`), arithmetic/logical/comparison/bitwise
operators, string ops, and critical-section markers.

**All 78 are `PURE-SIM`** — they touch only the interpreter's own stacks, program
counters, and the variable stores. No animation, UI, blocking, or time/RNG
dependency. Headless-trivial; no per-opcode decoupling required. (`op_random`
and friends live in the *game* opcode set above, not here.)

--------------------------------------------------------------------------------
## The SUBJECT axis — who does an opcode act on? (co-op / N-actor)

Orthogonal to `Headless-class` above, and added when per-actor character sheets
landed (`PLAYER_SHEET_DESIGN.md`). An opcode can be perfectly `PURE-SIM` and
still be wrong under co-op, because it acts on the wrong *player*.

**The rule, which holds for ~175 of the 181:** an opcode either takes an
object/critter parameter and honours it (`HONORS`), or never touches a player at
all (`NO-SUBJECT`). Both are already N-actor-correct and need nothing. Only the
exceptions are listed here — do not re-derive the other 175.

Classes: `HONORS` · `NO-SUBJECT` · `IGNORES` (takes a subject, forces `gDude`
anyway — the bug class) · `HOST` (hardcodes `gDude`, no parameter) · `CONTEXT`
(bare opcode, correctly resolves via `scriptContextDude`).

| Opcode | Keyword | Class | State |
|---|---|---|---|
| 0x80A1 | op_give_exp_points | CONTEXT | correct — the reference example |
| 0x80BF | op_dude_obj | CONTEXT | correct — the MP mint point |
| 0x80F3 | op_has_trait | ~~IGNORES~~ | FIXED — TRAIT case now takes the popped object |
| 0x80CB | op_set_critter_stat | ~~IGNORES~~ | FIXED — gate is `playerActorIs` |
| 0x813C | op_critter_mod_skill | ~~IGNORES~~ | FIXED — gate + `skillAddForce`/`skillSubForce` take the critter |
| 0x80AB | op_using_skill | IGNORES | ⚠ OPEN — sneak check gated on `object == gDude`; blocked on per-actor SNEAK, same blocker as Silent Death |
| 0x8147 | op_move_obj_inven_to_obj | IGNORES | ⚠ OPEN — AC / gender / HUD fixups gated on `object1 == gDude`, so armour moved into an EXTRA's inventory never re-runs `_adjust_ac` |
| 0x80A6 | op_get_pc_stat | HOST | ⚠ RULING OWED — always reads the host's XP/level/karma. Whether it should follow the interacting player is a design call, not a bug |
| 0x814B | op_party_member_obj | ? | UNRESOLVED — reads the global party list with no player selector; ambiguous under N players |
| 0x80A9 | op_override_map_start | HOST | correct by ruling — only the host triggers map transitions (MP_PROPOSAL Ch 14.2) |
| 0x8121 | op_giq_option | HOST | correct by ruling — dialog is host-only (`Smooth Talker` + INT gate) |

⚠ **A `HONORS` row can still hide a host-pinned HUD refresh.** `0x8117`
(`rm_mult_objs_from_inven`), `0x8127` (`critter_injure`) and `0x8144`
(`destroy_mult_objs`) all mutate the right critter and then repaint only if it is
`gDude`. The SIM is per-actor and correct; the SCREEN is not. That is a
presentation-routing gap, not a subject bug — do not "fix" it by widening the
sim gate.
