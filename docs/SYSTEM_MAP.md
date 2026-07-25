# Fallout 2 CE — System Mapping Guide
### Technical reference for a headless-server / thin-client architectural rewrite

> Source of truth: `f2_headers_manifest.txt` (concatenation of every `src/**/*.h`).
> Every header cited below maps 1:1 to its implementation file (`foo.h` → `foo.cc`).
> Convention note: symbols prefixed with `_` are decompiled originals whose semantics were
> preserved verbatim; unprefixed camelCase symbols were renamed/understood during decompilation.

---

## 1. CORE SYSTEM ANATOMY

### 1.1 The world is three stacked coordinate systems

| Layer | Structs / constants | Header | Notes |
|---|---|---|---|
| **Hex grid** (logic space) | `HEX_GRID_WIDTH/HEIGHT = 200×200` → 40,000 hexes per elevation | `map_defs.h` | All simulation (movement, combat range, LOS, object placement) happens in hex-tile numbers (`int tile`, 0..39999). This is the grid a server must own. |
| **Square grid** (art space) | `SQUARE_GRID_WIDTH/HEIGHT = 100×100`, `TileData { int field_0[SQUARE_GRID_SIZE] }` | `map_defs.h`, `map.h` | Floor/roof art tile IDs. Purely presentational — each `int` packs floor+roof FRM ids. `_square[ELEVATION_COUNT]` in `map.h` holds it. |
| **Elevations** | `ELEVATION_COUNT = 3` | `map_defs.h` | Each map is up to 3 independent 200×200 hex grids. Objects carry their own `elevation` field. |

**Packed tile addressing:** `obj_types.h` defines *built tiles* — one `int` packing tile (26 bits) + rotation (3 bits) + elevation (3 bits), with `builtTileCreate/GetTile/GetElevation/GetRotation` inlines. Stairs/ladders/map-entrances all use this format (`StairsSceneryData.destinationBuiltTile`, `MapHeader.enteringTile`).

### 1.2 Map container

- **`MapHeader`** (`map.h`) — the on-disk `.MAP` header: version, `name[16]`, entering tile/elevation/rotation, `localVariablesCount`, `globalVariablesCount`, `scriptIndex`, `flags`, `darkness`, `index`, `lastVisitTime`.
- **`MapTransition`** (`map.h`) — `{map, elevation, tile, rotation}`; queued via `mapSetTransition()` and consumed by `mapHandleTransition()` in the main loop.
- **Map-scoped variables:** `gMapLocalVars` / `gMapGlobalVars` (raw `int*` arrays + lengths, `map.h`) — MVARs owned by the currently loaded map; script-visible via `mapSetLocalVar/mapGetLocalVar`.
- **Hex→pixel projection & hex math** live in `tile.h`: `tileToScreenXY`, `tileFromScreenXY`, `tileDistanceBetween`, `tileGetTileInDirection`, `tileGetRotationTo`, `tileIsEdge`, `_tile_num_beyond`. ⚠️ This one header mixes **pure hex math** (server-side keepers) with **camera/render** functions (`tileSetCenter`, `gCenterTile`, `tileWindowRefresh`, roof rendering) — it must be split in the rewrite.
- **Per-tile light:** `light.h` — `lightGetTileIntensity(elevation, tile)`, ambient intensity. Nominally rendering, but light level feeds perception/sneak checks, so the server needs the intensity model, not the palette math.
- **Overland layer:** `worldmap.h` is a *completely separate* spatial system: `City`/`Map` enums, party world position (`wmGetPartyWorldPos`), sub-tile visited state, car fuel (`CAR_FUEL_MAX`), random/special encounters (`wmSetupRandomEncounter`, `wmForceEncounter`). Local maps and the worldmap only touch through map transitions.

### 1.3 The universal entity: `Object` (`obj_types.h`)

Everything placed on the grid — critters, items, scenery, walls, misc — is one struct:

```
Object {
    id, tile, x, y, sx, sy,          // sx/sy/x/y are SCREEN-space render offsets ⚠️
    frame, rotation, fid,            // fid = current art frame id (render) ⚠️
    flags,                           // ObjectFlags bitfield
    elevation,
    ObjectData data,                 // inventory + type-specific union (below)
    pid,                             // prototype id → Proto template
    cid,                             // combat id: index into combat.cc's combatant list
    lightDistance, lightIntensity,
    outline,                         // render ⚠️
    sid,                             // script id → Script instance
    owner,                           // container/carrier back-pointer
    scriptIndex
}
```

`ObjectData` = `Inventory` (every object can contain items: `InventoryItem { Object* item; int quantity }`) + a union:

- **`CritterObjectData`** — `reaction_to_pc`, `CritterCombatData` (`maneuver`, `ap`, `results` (Dam flags), `damageLastTurn`, `aiPacket`, `team`, `whoHitMe`), `hp`, `radiation`, `poison`. This is the *mutable* combat state.
- **`ItemObjectData`** — weapon (loaded ammo qty/type), ammo, misc charges, key code.
- **`SceneryObjectData`** — door open flags, stairs/ladder destination built-tile, elevator type/level.
- **`MiscObjectData`** — exit-grid destination `{map, tile, elevation, rotation}`.

Key enums in the same header: `ObjectFlags` (`OBJECT_HIDDEN`, `OBJECT_NO_SAVE`, `OBJECT_NO_BLOCK`, `OBJECT_MULTIHEX`, `OBJECT_SHOOT_THRU`, `OBJECT_LIGHT_THRU`, `OBJECT_SEEN`, equip-slot flags `OBJECT_IN_LEFT_HAND/RIGHT_HAND/WORN`), `CritterFlags`, `Dam` (damage-result flags: knocked out/down, crippled limbs, blind, dead, lose turn…), `CritterManeuver`, lock/jam bits (`OBJ_LOCKED`, `OBJ_JAMMED`).

### 1.4 Templates: the `Proto` union (`proto_types.h`)

Static data separated from instance data — a clean template/instance split you should preserve:

- **`Proto`** union with common prefix `{pid, messageId, fid, lightDistance, lightIntensity, flags, extendedFlags, sid}`, specialized as:
  - **`ItemProto`** → `ItemProtoData` union: `ProtoItemArmorData` (AC, DR[7], DT[7]), `ProtoItemWeaponData` (min/max damage, damage type, ranges, AP costs, caliber, ammo capacity, min ST, crit-fail table), `ProtoItemDrugData` (stat effects over 3 stages, addiction chance, withdrawal), `ProtoItemAmmoData` (AC/DR modifiers, damage mult/div), container/misc/key.
  - **`CritterProto`** → `CritterProtoData`: `baseStats[35]`, `bonusStats[35]`, `skills[18]`, `bodyType`, `experience`, `killType`, native `damageType`, plus `headFid`, `aiPacket`, `team`.
  - **`SceneryProto`**, **`WallProto`**, **`TileProto`**, **`MiscProto`**.
- Proto registry: `ProtoList`/`ProtoListExtent` cache; accessors in `proto.h` (`protoGetProto`, `protoGetDataMember`, `proto_new`, `_proto_dude_init`).
- ID packing: `PID_TYPE/FID_TYPE/SID_TYPE` macros (top byte = object type) in `obj_types.h`.

### 1.5 Object placement & spatial queries (`object.h`)

The de-facto spatial index API the server must reimplement: `objectSetLocation`, `_obj_connect/_obj_disconnect` (tile linked-lists via `ObjectListNode`), iterators `objectFindFirst[AtElevation|AtLocation]/Next`, occupancy & blocking probes `_obj_blocking_at`, `_obj_shoot_blocking_at`, `_obj_sight_blocking_at`, `_obj_ai_blocking_at`, `objectGetDistanceBetween`, `isExitGridAt`, and per-tile "seen" fog bits (`obj_set_seen`, `_obj_process_seen`).

### 1.6 Player vs NPC in the data model — where they diverge

The player ("dude") **is just an `Object`** with `CritterObjectData`, but a constellation of player-only state lives *outside* the Object:

| Player-only concern | Where it lives | Header |
|---|---|---|
| Identity | `extern Object* gDude;` global singleton (plus `gEgg`, the transparency overlay object — pure render) | `object.h` |
| PC-only stats | `PcStat` enum: unspent skill points, **level, XP, reputation, karma** — accessed by `pcGetStat(int pcStat)` / `pcSetStat` **with no Object parameter**; state is module-static in `stat.cc` | `stat_defs.h`, `stat.h` |
| Tagged skills | `skillsSetTagged/skillsGetTagged/skillIsTagged` — global, no critter param | `skill.h` |
| Traits | `traitsSetSelected(trait1, trait2)` — global pair, no critter param | `trait.h` |
| Kill counters | `killsIncByType/killsGetByType` — global tallies | `critter.h` |
| Dude state flags | `DudeState` (sneaking / level-up available / addicted) via `dudeEnableState()` etc. — no object param | `critter.h` |
| Character file | `.GCD` load/save: `gcdLoad/gcdSave`, `_proto_dude_init` (dude's proto is rewritten from the GCD) | `critter.h`, `proto.h` |
| Separate serialization | `_obj_save_dude` / `_obj_load_dude` — dude bypasses normal object save (has `OBJECT_NO_SAVE`) | `object.h` |
| Name | `dudeSetName/dudeResetName` (critter names otherwise come from proto message files / `scrname.msg`) | `critter.h` |
| Party | `party_member.h` — pid list `gPartyMemberPids`, level-scaling `_partyMemberIncLevels`, save-prep juggling (`_partyMemberPrepSave`) — party NPCs get special proto copies | `party_member.h` |

**Consequence for the rewrite:** NPCs are fully described by `Object` + `CritterProto`; the PC is `Object` + GCD + ~6 disconnected global subsystems. A multi-client server must fold `PcStat`, tagged skills, traits, kill counts, and dude-state into a per-player `Character` aggregate keyed by player id — the headers make the current 1-player assumption explicit precisely because these APIs take no entity parameter.

---

## 2. GLOBAL STATE & THE "SINGLE-PLAYER" TRAP

### 2.1 Inventory of global state managers

**World / map (one map loaded at a time — destructive swap):**
- `map.h`: `gMapHeader`, `gMapSid` (map script), `gMapLocalVars`/`gMapGlobalVars` + lengths, `gElevation` (**the** current elevation — camera state used as an implicit simulation parameter), `_square[3]`, `gIsoWindow` (render), `gMapMessageList`. `mapLoadById()` tears down and replaces the entire world in place.
- `object.h`: global tile→object linked lists (internal to `object.cc`), `gDude`, `gEgg`, `_moveBlockObj`.
- `light.h`: global per-elevation light grids. `tile.h`: `gCenterTile`, `gHexGridSize`.

**Game-wide variables:**
- `game.h` / `game_vars.h`: `gGameGlobalVars` + `gGameGlobalVarsLength` — the **GVAR** array (~700 named indices enumerated in `game_vars.h`), plus sfall's `gameGetGlobalPointer`. `_game_user_wants_to_quit`.
- `sfall_global_vars.h`: a second, string/int-keyed global var store (`sfall_gl_vars_store/fetch`).
- `scripts.h`: game clock — `gameTimeGetTime()` (ticks; 10 ticks/sec game time), `gameTimeAddTicks`, `gameTimeSetTime`. Single world clock, module-static.
- `queue.h`: the timed-event queue (drugs, poison, radiation, script timers, knockout, sneak, explosions) keyed on game time, owner = `Object*`. **This is the cleanest, most server-shaped subsystem in the codebase** — already a pure discrete-event scheduler with read/write serialization procs.
- `random.h`: one global RNG (`randomRoll`, `randomSeedPrerandom`, save/load of seed).

**Combat (a modal singleton):**
- `combat.h`: `gCombatState` bitfield + `isInCombat()`, `_combatNumTurns`, `_combat_free_move`, `combat_get_data()` returning the **global `Attack` scratch struct**. Combat entry `_combat(CombatStartData*)` runs a **blocking modal turn loop** (`_combat_turn_run`) inside the main loop; exactly one combat may exist, and it freezes the rest of the world.
- `Object.cid` indexes into `combat.cc`'s static combatant list (`_find_cid`); AI "memory" (`aiInfoGetLastTarget` etc.) is a parallel static array indexed by cid.

**Scripting VM:**
- `interpreter.h`: `Program` — stack-based VM instance. ⚠️ `Program.windowId` — scripts are physically bound to UI windows. Program list is module-static (`_updatePrograms` pumps all).
- `scripts.h`: `Script` registry (SIDs), per-script local vars stored in a map-owned pool (`localVarsOffset` into `gMapLocalVars`' sibling array), `scriptsRequest*` functions (combat, worldmap, elevator, dialog, endgame, looting) that set **global request flags** consumed by `scriptsHandleRequests()` in the main loop — this is the engine's homegrown command queue, and a natural seam to formalize into server messages.
- `export.h` (cross-script variables/procedures), `sfall_global_scripts.h` (scripts hooked to the main loop), `sfall_arrays.h`/`sfall_lists.h` (global script-visible collections).

**Per-subsystem lifecycle quadruple:** nearly every module exposes `xInit/xReset/xExit` + `xLoad(File*)/xSave(File*)` (`skill.h`, `perk.h`, `trait.h`, `stat.h`, `critter.h` (kills), `combat.h`, `queue.h`, `party_member.h`, `automap.h`, `interface.h`, `pipboy.h`, `preferences.h`, `worldmap.h`, `game_movie.h`…). The savegame is the world-state serializer, and `lsgSaveGame/lsgLoadGame` (`loadsave.h`) is its orchestrator — an authoritative-state snapshot mechanism already exists; it just writes to disk instead of the wire.

### 2.2 Structures hard-coupled to a single-player execution context

Ranked by how much they will fight a headless multi-client server:

1. **`gDude` as a load-bearing global** — referenced across combat, actions, items (`dudeIsWeaponDisabled`), skills, dialog, worldmap. Every "the player" assumption in game rules routes through this pointer.
2. **Parameter-less player APIs** — `pcGetStat/pcSetStat`, `skillsGetTagged`, `traitsGetSelected`, `dudeHasState`, `killsGetByType`, `pcAddExperience`. No entity handle ⇒ no second player without refactoring the call graph.
3. **`gElevation` + `gCenterTile` (one camera)** — the *current elevation* is global render state, yet object iteration, light, and spatial queries take it as the implicit "active" layer. A server has no current elevation; every query must be explicitly parameterized (the low-level functions already accept `elev` — it's the callers that assume the global).
4. **One-map world** — `map.cc` owns exactly one loaded map; MVAR/local-var arrays, spatial index, light grids, and the automap all swap on `mapLoadById`. Concurrent parties on different maps require making "loaded map" an instanceable `World`/`Zone` object.
5. **Modal combat loop** — `_combat()` blocks; turn advancement is interleaved with input polling and **animation completion** (`_combat_anim_begin/_combat_anim_finished`). Server combat must become a state machine ticked by messages, not a nested event loop.
6. **The animation registry drives game logic** (`animation.h`) — `reg_anim_begin/reg_anim_end` build global animation sequences, and *gameplay outcomes wait on them* (movement consumes AP inside animation callbacks; combat waits on `animationIsBusy`). Movement/pathfinding must be extracted from the animation system (see §3 pathfinding note).
7. **`GameMode` static bitfield** (`game.h`) — kDialog/kCombat/kBarter/kPipboy... a single modal-UI state machine on a static; encodes "one screen, one player."
8. **Scripts drive UI directly** — `Program.windowId`, dialog subsystem globals (`gGameDialogSpeaker`, `_dialog[4]` in `dialog.h`), `game_dialog.h` state. Dialog must be re-modeled as a server-side conversation session with client-rendered options.
9. **UI/render globals to amputate wholesale:** `gIsoWindow`, `interface.h` (`gInterfaceBarWindow`…), `game_mouse.h` (`gGameMouseBouncingCursor`/`gGameMouseHexCursor` are actual `Object`s living *in the world grid*), `display_monitor.h`, `text_object.h`, `automap.h` (client concern; but its per-tile seen bits come from `object.h` fog state — that part is server data), `svga.h`, `window_manager.h`, `cycle.h`, `palette.h`.
10. **Global RNG + wall-clock coupling** — `randomRoll` is one stream; `input.h getTicks()` (real time) leaks into logic via animation pacing. Server needs per-instance seeded RNG for determinism/replay.

---

## 3. RULES & MECHANICS EXTRACTION INDEX

Lookup table: mechanic → declaration header → implementation to read. ⚠️ marks places where the *actual formula* is a `static` function in the `.cc` that never appears in any header.

### Combat mathematics
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| To-hit chance | `combat.h` | `combat.cc` | `_determine_to_hit`, `_determine_to_hit_no_range`, `_determine_to_hit_from_tile`; hit-location penalties `combat_get_hit_location_penalty` |
| Attack resolution pipeline | `combat.h`, `combat_defs.h` | `combat.cc` | `attackInit` → `_combat_attack` → `_apply_damage`; the `Attack` struct (attacker/defender damage, flags, knockback, explosion `extras[6]`) is the entire combat transaction record |
| Damage formula | ⚠️ header-invisible | `combat.cc` (`attackComputeDamage`, static) | Inputs from `item.h` (weapon damage, ammo mult/div, DR/DT via `armorGet*`) and `stat.h` (DR/DT stats); sfall alternate formulas gated by `damageModGetBonusHthDamageFix` (`combat.h`) |
| Critical hits / crit tables | `combat.h`, `combat_defs.h` | `combat.cc` (table is a static array) | `CriticalHitDescription` (per kill-type × hit-location × roll-severity), `criticalsGetValue/SetValue`, massive-crit stat checks |
| Critical failures | `item.h` (`weaponGetCriticalFailureType`) | `combat.cc` | crit-fail table per weapon class |
| Unarmed martial-arts ladder | `combat.h`, `combat_defs.h` (`HitMode` enum: strong/hammer punch, snap/power kick…) | `combat.cc` | `unarmedGetDamage`, `unarmedGetActionPointCost`, `unarmedIsPenetrating` |
| Turn order & AP economy | `combat.h`, `stat_defs.h` (`STAT_SEQUENCE`, `STAT_MAXIMUM_ACTION_POINTS`) | `combat.cc` | `_combat_turn_run`, `_combat_whose_turn`; AP costs from `item.h` `itemGetActionPointCost`, `weaponGetActionPointCost` |
| Burst/spray pattern | sfall keys in `sfall_config.h` (`ComputeSpray*`) | `combat.cc` (`computeSpray`, static ⚠️) | grouping of burst rounds into center/target/periphery |
| Explosions & area damage | `actions.h`, `item.h` | `actions.cc`, `combat.cc` | `actionExplode`, `_compute_explosion_on_extras`, radius fns `weaponGetDamageRadius`, explosive pid registry (`explosiveAdd/IsExplosive`) |
| Shot blocking / LOF | `combat.h`, `object.h` | `combat.cc`, `object.cc` | `_combat_is_shot_blocked`, `_combat_bullet_start`, `_obj_shoot_blocking_at` |
| Knockback & death anim selection | `combat.h` | `combat.cc` | `attackComputeDeathFlags`, `Dam` flags → anim mapping |

### Character rules (S.P.E.C.I.A.L.)
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| Stat access & derived stats | `stat.h`, `stat_defs.h` | `stat.cc` | `critterGetStat` (the everything-modifier funnel: base + bonus + traits + drugs), `critterUpdateDerivedStats` (⚠️ derivation formulas — max HP, AC, sequence, carry weight — are static tables/exprs in `stat.cc`) |
| Stat checks | `stat.h` | `stat.cc` | `statRoll(critter, stat, modifier, &howMuch)` |
| Skills: value & checks | `skill.h`, `skill_defs.h` | `skill.cc` | `skillGetValue` (base from stats + points + tag + traits + difficulty), `skillRoll`, `skillUse` (first aid/doctor/lockpick/steal/repair… incl. XP awards & timers), `skillsPerformStealing`, `skillGetGameDifficultyModifier` |
| Dice engine | `random.h` | `random.cc` | `randomRoll` → `Roll` enum (crit-fail/fail/success/crit-success); `randomBetween` |
| Traits | `trait.h`, `trait_defs.h` | `trait.cc` | `traitGetStatModifier`, `traitGetSkillModifier` (⚠️ per-trait effect tables static in `trait.cc`) |
| Perks | `perk.h`, `perk_defs.h` | `perk.cc` | `perkGetRank`, `perkAddEffect/perkRemoveEffect` (stat side-effects), `perkGetSkillModifier`; rank/requirement table static in `perk.cc` ⚠️ |
| XP / leveling | `stat.h` | `stat.cc` | `pcGetExperienceForLevel` (formula), `pcAddExperience`; kill XP via `critter.h` `critterGetExp`, `_combat_give_exps` (`combat.h`) |
| HP / poison / radiation | `critter.h` | `critter.cc` | `critterAdjustHitPoints`, `critterAdjustPoison/Radiation`, `_process_rads` (rad-level effect table ⚠️), `poisonEventProcess`, `radiationEventProcess`, healing `_critter_heal_hours` |
| Drugs & addiction | `item.h` (+ `ProtoItemDrugData` in `proto_types.h`) | `item.cc` | `_item_d_take_drug`, `drugEffectEventProcess`, `withdrawalEventProcess`; queue event structs in `queue.h` |
| Encumbrance / crippled movement | `critter.h`, `item.h` | `critter.cc`, `item.cc` | `critterIsEncumbered`, `objectGetInventoryWeight`, `critterGetMovementPointCostAdjustedForCrippledLegs` |

### Movement, pathfinding, perception
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| **Pathfinding (A*-style)** | ⚠️ declared in **`animation.h`**, not a dedicated header | `animation.cc` | `pathfinderFindPath`, `_make_path` (returns rotation steps), obstacle callback `PathBuilderCallback` (defaults to `_obj_blocking_at`). **Extraction warning:** the pathfinder is embedded in the animation module; it is pure and server-side, but its only callers are animation registrations. |
| Straight-line path / projectile trace | `animation.h` | `animation.cc` | `_make_straight_path(_func)` → `StraightPathNode`, obstacle out-param — this doubles as the LOS/LOF raycast |
| Hex geometry | `tile.h` | `tile.cc` | `tileDistanceBetween`, `tileGetTileInDirection`, `tileGetRotationTo`, `_tile_num_beyond` |
| Sight / perception | `actions.h`, `combat_ai.h`, `object.h` | `actions.cc`, `combat_ai.cc` | `_can_see`, `_is_hit_from_front`, `isWithinPerception` (⚠️ perception-distance math static in `combat_ai.cc`), `_obj_sight_blocking_at` |
| Movement execution | `animation.h` | `animation.cc` | `animationRegisterMoveToTile`, `_dude_move/_dude_run` — ⚠️ AP deduction happens inside animation callbacks; server must re-home this into a movement rule module |

### AI
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| AI packets (personality) | `combat_ai_defs.h` (enums: `Disposition`, `BestWeapon`, `DistanceMode`, `AttackWho`, `ChemUse`, `RunAwayMode`, `AreaAttackMode`) | `combat_ai.cc` (packets parsed from `ai.txt` via `string_parsers.h`) | `aiGet*/aiSet*` per-critter accessors |
| Combat AI decision loop | `combat_ai.h` | `combat_ai.cc` | `_combat_ai` (top-level per-turn brain), `_ai_search_inven_weap/armor`, `aiAttemptWeaponReload`, target pick `_combat_ai_random_target`, join/flee `_combatai_want_to_join/_want_to_stop` |
| Team combat setup | `combat_ai.h` | `combat_ai.cc` | `_caiSetupTeamCombat`, `critterSetTeam`, retaliation `_combatai_check_retaliation`, `_combatai_notify_friends` |

### Items & economy
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| Weapon math | `item.h` | `item.cc` | `weaponGetDamage(MinMax)`, `weaponGetRange`, `weaponGetActionPointCost`, `weaponGetMinStrengthRequired`, `weaponGetBurstRounds`, reload rules `weaponCanBeReloadedWith` |
| Ammo modifiers | `item.h` | `item.cc` | `weaponGetAmmoArmorClassModifier`, `...DamageResistanceModifier`, `...DamageMultiplier/Divisor` |
| Armor | `item.h` | `item.cc` | `armorGetArmorClass`, `armorGetDamageResistance/Threshold(damageType)` |
| Inventory transactions | `item.h`, `inventory.h` | `item.cc`, `inventory.cc` | `itemAdd/itemRemove/itemMove(Force)`, equip `_invenWieldFunc/_invenUnwieldFunc`, slot queries `critterGetItem1/Item2/Armor` (left hand/right hand/armor) |
| Value & barter | `item.h`; UI entry `inventoryOpenTrade` (`inventory.h`), modifier `gameDialogSetBarterModifier` (`game_dialog.h`) | `inventory.cc` ⚠️ (`_barter_compute_value`, static) | `itemGetCost`, `objectGetCost`, caps helpers `itemGetTotalCaps/itemCapsAdjust`; Barter skill + Master Trader feed the static formula |
| Books / consumables | `item.h` | `item.cc` | `booksGetInfo` (book pid → skill), `itemIsHealing`, `miscItem*` charges |

### World rules & scripting surface
| Mechanic | Declarations | Implementation | Key symbols |
|---|---|---|---|
| Game clock & calendar | `scripts.h` | `scripts.cc` | `gameTimeGetTime/GetDate`, `GAME_TIME_TICKS_PER_*`, `gameTimeScheduleUpdateEvent` |
| Timed-event scheduler | `queue.h` | `queue.cc` | `queueAddEvent(delay, owner, data, type)`, `queueProcessEvents`, `EventType` enum — **adopt this design server-side** |
| Script engine (bytecode VM) | `interpreter.h` | `interpreter.cc` | `Program`, opcode set, `_interpret`, `_executeProcedure` |
| Game opcode bindings | `interpreter_extra.h` (thin) | **`interpreter_extra.cc`** ⚠️ | ~350 game opcodes (all `op*` statics) — the true script↔engine ABI; the definitive list of every world mutation scripts can perform. Read this file before designing the server API. |
| Script registry & procs | `scripts.h` | `scripts.cc` | `Script` struct, `ScriptProc` enum (start/use/talk/damage/map_enter/…), `scriptExecProc`, spatial scripts (`scriptGetFirstSpatialScript`, trigger radius via built-tile) |
| GVAR/MVAR/LVAR access | `game.h`, `map.h`, `scripts.h` | `game.cc`, `map.cc`, `scripts.cc` | `gameGet/SetGlobalVar`, `mapGet/SetGlobalVar`, `scriptGetLocalVar/scriptSetLocalVar` |
| Map load/serialize & transitions | `map.h` | `map.cc` | `mapLoadById/mapLoadSaved`, `mapSetTransition/mapHandleTransition`, `_map_save_in_game` |
| Object actions (use/pickup/lock) | `proto_instance.h`, `actions.h` | `proto_instance.cc`, `actions.cc` | `_obj_use_item_on`, `_obj_use_door`, `objectLock/Unlock/JamLock`, `_obj_pickup`, `actionPickUp`, push rules `actionCheckPush` |
| Worldmap travel & encounters | `worldmap.h` | `worldmap.cc` ⚠️ (encounter tables parsed from `worldmap.txt` into statics) | `wmSetupRandomEncounter`, terrain/visited state, car fuel economy `wmCarUseGas` |
| Party behavior | `party_member.h` | `party_member.cc` | level-ups `_partyMemberIncLevels`, best-skill queries, disposition support checks |
| NPC reaction | `reaction.h` | `reaction.cc` | `reactionSetValue/GetValue`, `NpcReaction` bands |
| Difficulty knobs | `game_config.h`, `settings.h` | consumed in `skill.cc`, `combat.cc`, `stat.cc` | `GameDifficulty`, `CombatDifficulty`; `skillGetGameDifficultyModifier` |
| Data-file config (moddable rules) | `sfall_config.h` | various | damage formula selector, burst-mod params, unarmed/books/elevators override files — treat as server-side rule config |

### Cross-cutting extraction warnings

1. **Header ≠ formula.** The headers give you the *call surface*; the actual constants and equations (damage computation, derived-stat tables, crit tables, XP curve, perk requirements, rad/poison stage tables, barter pricing, burst spray) are `static` in the `.cc` files and never declared. Budget a dedicated pass over `combat.cc`, `stat.cc`, `skill.cc`, `perk.cc`, `trait.cc`, `critter.cc`, `item.cc`, `inventory.cc`, `worldmap.cc`, `interpreter_extra.cc`.
2. **Pathfinding lives inside animation.** Server-side movement requires lifting `pathfinderFindPath`/`_make_straight_path` out of `animation.cc` and re-homing AP charging from animation callbacks into a rules module.
3. **The savegame code is your serialization spec.** Every `xSave(File*)`/`xLoad(File*)` pair plus `objectDataWrite/objectDataRead` (`proto.h`) documents exactly which fields constitute authoritative state vs. derivable/render state.
4. **`scriptsRequest*` + `queue.h` are the embryonic command/event buses.** The engine already funnels "world mutations requested from below" through global request flags and a timed queue — formalizing these two seams into the server's message loop is the lowest-friction path.
5. **`Object` mixes sim and render state.** `tile/elevation/rotation/hp/inventory/flags` are server; `x/y/sx/sy/frame/fid/outline` are client. The wire protocol falls directly out of this field partition.
