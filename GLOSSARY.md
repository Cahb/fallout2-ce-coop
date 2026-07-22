# GLOSSARY — RE-derived name decoder

The core engine keeps its reverse-engineered names ON PURPOSE while the rewrite is in
flight: they are stable identifiers into upstream fallout2-ce, sfall, and 25 years of
community RE knowledge. This file is the accumulating decoder ring — and the rename map
for the post-v1 rename pass (banked, off critical path).

RULES: additive-only; append when a session decodes a name the hard way; one line each;
new code we author gets real names from day one (see server_control.cc, server_anim.cc)
and does NOT belong here.

## Combat

| Name | Meaning |
|---|---|
| `csd` / `CombatStartData` / `STRUCT_664980` | who starts a fight and against whom (attacker/defender + AP bonus); struct named after its original address |
| `_gcsd` | file-static pointer to the active CSD; initiator's first-turn bonuses; nulled after the first turn; may point at a STACK csd (`_combat_attack_this`) |
| `gScriptsCSD` / `gScriptsRequestedCSD` | global CSD slots used by script-requested combat (`scriptsRequestCombat` → `scriptsHandleRequests` → `_combat`) |
| `ctd` / `_main_ctd` | the shared `Attack` scratch struct ("combat: to-hit data"); one global instance reused per attack |
| `_combat_list` | initiative roster array (all critters on the combat elevation) |
| `_list_com` / `_list_noncom` / `_list_total` | roster partitions: active combatants `[0.._list_com)`, non-combatants, total allocated |
| `cid` | critter's index in `_combat_list` (stamped at `_combat_begin`) |
| `_combat_free_move` | the dude's Bonus Move perk AP pool (separate from regular AP) |
| `_combatNumTurns` | completed ROUND counter (misnamed: rounds, not turns) |
| `_combat_turn_running` | count of in-flight animations during a combat turn; drained by `_combat_turn_run` spin |
| `_combat_turn_obj` | whose turn it is right now (read via `_combat_whose_turn`) |
| `_combat_ending_guy` | set when a script ends combat while the dude is KO'd; breaks the round loop |
| `_combat_elev` | elevation the fight is locked to |
| `gCombatState` | flags: 0x01 in-combat, 0x02 dude-turn-live, 0x08 end-combat handshake |
| `whoHitMe` | critter's aggro backpointer (who last attacked it); drives hostility checks |
| `_combat_set_move_all` | round-start AP grant for every combatant |
| `_combat_sequence` | end-of-round roster maintenance: add joiners, drop dead, re-sort by Sequence stat, +5s game time |

## Scripts / vars

| Name | Meaning |
|---|---|
| `sid` | script instance id (object↔script binding) |
| `gvar` / `lvar` / `mvar` | GLOBAL_VAR (campaign-wide) / script-LOCAL var / MAP var |
| `pid` / `fid` | prototype id (what an object IS) / frame id (what art it shows) |
| `SCRIPT_PROC_*` | script entry points (map_enter, critter, damage, destroy, ...) |
| `_scr_end_combat` | script-requested combat end check |
| `_script_chk_critters` / `_script_chk_timed_events` | background ticker halves: per-critter procs (combat-gated) / event queue + game-time advance |

## Objects / world

| Name | Meaning |
|---|---|
| `_obj_blocking_at` | "is this tile blocked, by whom" query |
| `_obj_use_door` | door open/close incl. running its script SYNCHRONOUSLY (re-entrancy hazard) |
| `_obj_offset` | shift an object's per-pixel draw offset (presentation walking uses it) |
| `_tile_offx` / `_tile_offy` / `_tile_x` / `_tile_y` | camera state: pixel offset + tile origin of the iso view |
| `tileFromScreenXY` / `tileToScreenXY` | pure screen↔hex math on camera globals |
| `_make_path` | pathfinder query (read-only; global scratch buffers) |
| `OBJECT_NO_SAVE` | object excluded from saves AND from the netId walk (cursors, FX) |

## Input / UI

| Name | Meaning |
|---|---|
| `gmouse` / `_gmouse_*` | game-mouse layer: cursor modes (MOVE/ARROW/CROSSHAIR), hex cursor objects, edge scrolling |
| `_gmouse_3d_move_to` | move the two NO_SAVE hex-cursor objects to the hovered tile |
| `gGameMouseHexCursor` / `gGameMouseBouncingCursor` | the ground hex outline / the bouncing arrow above it |
| `_process_bk` / `_doBkProcesses` | background pump: tickers + mouse/keyboard state ("bk" = background) |
| `_get_input` / `inputGetInput` | per-frame input poll; -1 none, -2 mouse-button event pending |
| `_kb_getch` | logical key fetch from the engine's own key buffer |

## Server/net (ours — real names, listed for orientation only)

| Name | Meaning |
|---|---|
| beat | one 100ms sim tick of the dedicated server (`kServerTickDelta`) |
| rebaseline | netId re-walk + blob + baseline broadcast (join or map change) |
| claimant | the wire session currently holding control of the player actor |
