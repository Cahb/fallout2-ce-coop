# Worldmap → Viewer Streaming Assessment

**Date:** 2026-07-19  
**Context:** Full codebase recon for streaming worldmap travel to the multiplayer viewer.

## The Architecture: Intent → Event, Rendering Stays

The viewer **is** the game client. `worldmap_ui.cc` (1976 lines of rendering) is already linked into the viewer binary — the worldmap visual surface already works. The work is purely disconnecting the blocking loop's sim calls from direct execution and re-routing them through wire intents + server state events.

**Intent:** viewer says "player clicked here on map" → wire verb `wmmove X Y`  
**Event:** server runs travel sim, mutates `wmGenData` → wire event streams state back → viewer's **existing** render code re-renders from updated state

This is identical to the dialog pattern. Dialog rendering wasn't "ported" — `client_dialog.cc` is just 147 lines of event dispatch glue that feeds into the existing dialog UI.

## What Actually Needs to Happen

The `wmWorldMapFunc` blocking loop (`worldmap_ui.cc:132-373`) currently:

1. Calls `inputGetInput()` — reads keyboard/mouse
2. Mutates `wmGenData` directly — sets walking, marks visited, etc.
3. Calls the 5 travel sim helpers directly — `worldmapTravelStep/RestHeal/MarkVisited/ClockTick/EncounterCheck`
4. Renders from `wmGenData` state

The streaming version needs:

1. **Input → Intent send** — mouse clicks, enter key, arrow keys become wire verbs sent to server
2. **Sim → Server-owned** — the 5 travel helpers + `wmGenData` mutations run on the server in a `serverLoopActive()` branch
3. **State → Wire event** — server streams `wmGenData` changes back to viewer
4. **Render → Unchanged** — viewer renders from received state using existing `worldmap_ui.cc` code
5. **Wire pump** — `kWorldmap` in `kViewerModalMask` so the modal loop pumps the wire

## Pieces (Following the Dialog Pattern)

### Piece 1: Server worldmap driver (~1 session)

Add a `serverLoopActive()` branch to `wmWorldMapFunc`'s inner loop — pattern-following copy of dialog's `_gdProcess` barrier. When the server is driving:

- Drain `worldmapIntent*` queue (model `dialog_intent.{h,cc}` — ~40-line FIFO, copy-paste)
- On `WMMOVE` intent: call `wmPartyInitWalking(x, y)`
- On `WMENTER` intent: enter city via `wmAreaFindFirstValidMap` → break
- On `WMESCAPE` intent: break
- While walking: call the 5 travel helpers each tick (already pure-sim)
- If encounter fires: handle encounter → break
- Emit `wmGenData` state to viewer each tick

**Block-and-pump** — copy dialog's pump from `server_main.cc:195-236`: accept-skip, beginDrain, pollInbound, usleep. Install/uninstall around worldmap entry.

### Piece 2: Wire events (~0.25 session — formulaic)

New presenter virtuals + `presenter_network.cc` serialization:

```
EVENT_WORLDMAP_BEGIN          // server entered worldmap, viewer should call wmInterfaceInit()
EVENT_WORLDMAP_END            // server exited worldmap, viewer should call wmInterfaceExit()
EVENT_WORLDMAP_STATE          // full wmGenData sim fields: pos, walking, fuel, visited areas, encounter state
EVENT_WORLDMAP_ENCOUNTER      // random encounter triggered → viewer shows Yes/No? Or server auto-resolves?
```

**`wmGenData` fields to stream** (sim fields only, chrome stays client-local):
- `worldPosX`, `worldPosY` — party position
- `worldDestX`, `worldDestY` — walk destination
- `isWalking`, `walkDistance` — travel state
- `carFuel` — fuel gauge
- `currentAreaId`, `currentCarAreaId` — which city the party is over
- Subtile visited states (the fog-of-war grid — large but sparse, only sends deltas)
- Encounter state (`encounterMapId`, `encounterOccurred`, etc.)

**Chrome fields that STAY on viewer** (not streamed, client-owned):
- `mousePressed`, `encounterCursorId`, `carImageFrm`, `dialFrm`
- Viewport offsets (`wmWorldOffsetX`, `wmWorldOffsetY`)
- Window buffer pointers

### Piece 3: Client-side intent dispatch (~0.5 session)

Replace direct `wmGenData` mutations + `worldmapTravel*` calls with intent sends. Existing `worldmap_ui.cc` rendering stays untouched.

Wire verbs (sent from viewer to server via `clientSendLine`):
```
wmmove X Y      — click on worldmap to start walking to (X,Y)
wmenter         — click on city / mousePressed near current position → enter city
wmesc           — escape key → exit worldmap back to local map
wmtab N         — CTRL+F1 through F7 → quick-travel to tabbed location (N = 1-7)
wmencounter Y/N — respond to encounter dialog (or server auto-resolves)
```

Intent queue on server (model `dialog_intent.{h,cc}`):
```cpp
enum WmIntentType { WM_INTENT_MOVE, WM_INTENT_ENTER, WM_INTENT_ESCAPE, WM_INTENT_TAB };
struct WmIntent { WmIntentType type; int x; int y; };
```

Trust-boundary routing in `server_control.cc`:
```cpp
// claim-gated + GameMode::kWorldmap-active-gated
if (verb == "wmmove")  { push(WM_INTENT_MOVE, x, y); }
if (verb == "wmenter") { push(WM_INTENT_ENTER, 0, 0); }
if (verb == "wmesc")   { push(WM_INTENT_ESCAPE, 0, 0); }
```

### Piece 4: Main-loop integration (~0.25 session)

- `GameMode::kWorldmap` added to `kViewerModalMask` in `client_net.cc:120`
- `clientWorldmapActive()` gate in viewer frame loop (blocks combat/skills/inventory input while worldmap is open)
- Wire-send helpers: `clientViewerWmMove(x,y)`, `clientViewerWmEnter()`, `clientViewerWmQuit()`, `clientViewerWmTab(n)`

### Piece 5: Encounters + map transitions (~1 session)

Encounter flow when server detects one:
1. Server calls `wmRndEncounterPick()` + `wmSetupRandomEncounter()` + `wmSetupCritterObjs()` (all pure-sim)
2. Server emits `EVENT_WORLDMAP_ENCOUNTER` → viewer fades out
3. Server calls `mapLoadById(encounterMapId)` → viewer gets rebaseline blob (new map with spawned critters)
4. Combat can start naturally (combat record/replay already works)
5. After encounter resolved / map exit grid hit → server transitions back to worldmap (or to city)

Alternative: encounter is auto-resolved by server (no player Yes/No dialog) for the streaming path. Player always enters the encounter. Simpler trust model.

### Total: ~2–3 sessions (mostly mechanical)

## Why This Is NOT A Renderer Port

| Wrong mental model | Correct mental model |
|---|---|
| Port 1976 lines of `worldmap_ui.cc` to a new SDL surface | `worldmap_ui.cc` IS the viewer's renderer — it's already linked and working |
| Rewrite city overlay rendering | Viewport render, city spheres, fog-of-war, tabs, dial — all already render on viewer |
| Write new input handling | Same `inputGetInput()`, same mouse→world coord math — just sends intent instead of mutating `wmGenData` |
| Build town-map sub-modal from scratch | `wmTownMapFunc()` already exists and renders — just needs server-owned entrance resolution |
| Load FRM art assets | Already loaded by `wmInterfaceInit()` which runs on client |

The 1976 lines of `worldmap_ui.cc` are **client code**. They were split out from the core sim precisely so the server could have the pure sim. The rendering surface, art loading, fog-of-war, city spheres, scrolling, tabs, dial, car — all of it already works on the viewer side. The only thing that changes is **who calls the 5 travel helpers** and **who mutates `wmGenData`**.

## Source File Inventory

| File | Lines | Role |
|---|---|---|
| `src/worldmap_defs.h` | 419 | All types: `WmGenData`, `CityInfo`, `MapInfo`, `TileInfo`, `SubtileInfo`, `EncounterTable`, enums, constants |
| `src/worldmap.h` | 303 | Public API: `City`/`Map` enums, extern function declarations |
| `src/worldmap.cc` | 4456 | Core simulation: travel step/clock/heal/mark, encounters, save/load, init/exit |
| `src/worldmap_ui.cc` | 1976 | Client UI: `wmWorldMapFunc` blocking modal loop, `wmTownMapFunc`, all `wmInterface*` rendering |
| `src/worldmap_ui.h` | 66 | Core↔UI seam: shared mutable state, inward/outward call edges |
| `src/server_stubs.cc` | 416-422 | 7 `wm*` UI abort stubs (server never enters worldmap UI — stubs catch bugs) |

## Current State

### DONE (server sim is pure + golden-covered)

| Component | File | Status |
|---|---|---|
| Travel step | `worldmap.cc:2497` | PURE-SIM, golden: `arvillag_wmtravel` |
| Rest/heal cadence | `worldmap.cc:2564` | PURE-SIM |
| Mark visited | `worldmap.cc:2578` | PURE-SIM |
| Clock tick | `worldmap.cc:2592` | PURE-SIM |
| Encounter check/roll | `worldmap.cc:2603` | PURE-SIM |
| Party init walk | `worldmap.cc:3595` | PURE-SIM |
| Worldmap init | `server_boot.cc:164` | Server boots worldmap data tables |
| Headless probe | `command.cc:402` `wmtravel` | Drives travel headless |
| Core/UI split | H-13 through H-16 | All complete |

### NOT DONE

| Component | Status |
|---|---|
| `serverLoopActive()` branch in `wmWorldMapFunc` | NOT STARTED |
| Worldmap intent queue (`worldmap_intent.{h,cc}`) | NOT STARTED |
| Wire events (`EVENT_WORLDMAP_BEGIN/END/STATE`) | NOT STARTED |
| Wire verbs (`wmmove`, `wmenter`, `wmesc`) | NOT STARTED |
| `kWorldmap` in `kViewerModalMask` | NOT in mask |
| `ScriptRequestHandler::worldMap()` on server | Base no-op — server drops worldmap-enter |
| Encounter→mapLoad→combat pipeline over wire | NOT STARTED |

## Key Data Structures

| Struct | Defined | Key Fields |
|---|---|---|
| `WmGenData` | `worldmap_defs.h:335` | `worldPosX/Y`, `destX/Y`, `isWalking`, `walkDistance`, `carFuel`, `currentAreaId`, `encounterMapId`, **+ chrome** (mousePressed, cursors, viewport — CLIENT-ONLY) |
| `CityInfo` | `worldmap_defs.h:192` | Name, position, size, state, lockState, visitedState, mapFid, labelFid, entrances[10] |
| `TileInfo` | `worldmap_defs.h:320` | 350×300 tile: fid, walk mask, encounter difficulty, 7×6 SubtileInfo grid |
| `SubtileInfo` | `worldmap_defs.h:311` | 50×50 sub-tile: terrain, fill, encounter chances (morning/afternoon/night), encounter table index, visited state |
| `EncounterTable` | `worldmap_defs.h:269` | Name, random maps, weighted entries with condition filtering |

## Key Entry Points

| Entry | Source | Path |
|---|---|---|
| Script requests worldmap | Script opcode `0x8108` | `op_scripts_request_world_map` → `scriptRequestHandler()->worldMap()` |
| Map transition to worldmap | `map.cc:1110` | `map==-2` → `scriptRequestHandler()->worldMap()` |
| Map transition to town map | `map.cc:1101` | `map==-1` → `scriptRequestHandler()->townMap()` |

Both currently resolve to `ScriptRequestHandler::worldMap()` = base no-op on server. Need to wire to the server worldmap driver.

## Known Gaps & Hazards

1. **`wmGenData` mixes sim + chrome** — chrome fields (`mousePressed`, `encounterCursorId`, viewport offsets, `dialFrm`) are client-only and must NOT be streamed. Need a clean sim-only subset for wire serialization.
2. **`wmLastRndTime` static** — not cleared by `simClockReset()`. Fires on second worldmap entry in same process. Inert today (one `serverRun`/process).
3. **Core↔UI by-name edges** — `wmInterfaceRefreshDate`, `wmInterfaceCenterOnParty` etc. are called by the sim. These have headless guards (`wmBkWinBuf == nullptr → return`) but confirm they won't abort on server.
4. **`F2_FAKE_CLOCK` dependency** — `wmtravel` probe uses `getTicks()`. Real server driver should use `simClockNow()`. This is the blocker for S3 `F2_FAKE_CLOCK` cleanup (`SERVER_LOOP_DESIGN.md §1`).
5. **Encounter→combat pipeline untested over wire** — mapLoadById after encounter + critter spawn + potential combat entry is a new code path.

## Pre-existing Bug Comments (vanilla CE, not P5)

- `worldmap.cc:3469` — Encounter condition overflow with 3 conditions
- `worldmap.cc:3584` — `wmPartyInitWalking` Bresenham math suspect
- `worldmap_ui.cc:848` — Diagonal scroll success-flag incorrect

## Open Design Decisions

1. **Encounter Yes/No dialog** — server auto-accepts encounters (streaming path always enters combat) vs. server pauses and waits for viewer to respond? Auto-accept is simpler and faithful (players rarely decline encounters).
2. **Server-owns-everything vs client-runs-local-sim** — should the viewer run travel sim locally for responsiveness (with server as authority that corrects)? Or does the server drive everything (viewer is pure terminal)? Server-owns = simpler, consistent with rest of architecture.
3. **Visited subtile state streaming** — the fog-of-war grid is sparse but large (42 subtiles per tile × many tiles). Stream deltas on change vs. full state on entry.