# Viewer Modal Streaming Pattern

**Date:** 2026-07-19  
**Context:** Extracted from the completed dialog streaming track and active barter streaming track. This is the reusable pattern for wiring any server-owned modal to the viewer.

## The 5-Piece Checklist

For any modal to be **complete and wired** in the viewer, all 5 pieces must exist:

### Piece 1: `kViewerModalMask` Entry

**File:** `src/client_net.cc:120-122`

```cpp
static const int kViewerModalMask = GameMode::kInventory | GameMode::kSkilldex
    | GameMode::kEditor | GameMode::kPipboy | GameMode::kLoot | GameMode::kUseOn
    | GameMode::kDialog;
```

**What it does:**
- Tells `viewerServiceTicker()` (line 2651) to pump the wire (`conn->pump()`) inside the modal's blocking loop
- Force-ESCs the modal on combat start, rebaseline, or disconnect
- `onBlobEnd` (line 624) defers rebaseline apply while the modal is open (prevents UAF on `gDude`)
- Main loop applies deferred blobs after modal close (line 1392)

**Current mask members:** `kInventory`, `kSkilldex`, `kEditor`, `kPipboy`, `kLoot`, `kUseOn`, `kDialog`

**Pending:** `kBarter` (0x20000), `kWorldmap` (0x1)

### Piece 2: Wire Events (Server → Viewer)

**Server side:**
1. Abstract virtual in `presenter.h` (e.g., `dialogNode(params)`, `dialogEnd(driver)`)
2. Serialized in `presenter_network.cc` → calls `flushFrame()` immediately
3. New `EVENT_*` constant in the viewer's wire event enum

**Viewer side:**
1. `client_net.cc Decoder::event()` dispatches to `onXxx(Reader& r)`
2. Handler calls into the `client_*.cc` module to seed state and trigger render

**Existing events for reference:**
- `EVENT_DIALOG_NODE = 32`, `EVENT_DIALOG_END = 33`
- `EVENT_PRES_SEQ = 31`

### Piece 3: Viewer Render + Input Module

**Files:** `src/client_{name}.{h,cc}` (same pattern for each modal)

**Required API:**
```cpp
// Called from wire decoder when server emits state
void clientXxxOnBegin(...);   // Open modal window, seed initial state
void clientXxxOnUpdate(...);  // Update state mid-modal
void clientXxxOnEnd();        // Tear down window, exit GameMode

// Called from main loop
bool clientXxxActive();       // Is the modal currently open?
void clientXxxHandleKey(int keyCode); // Dispatch key input
```

**Reference:** `client_dialog.{h,cc}` (147 lines) — `clientDialogOnNode`, `clientDialogOnEnd`, `clientDialogActive`, `clientDialogHandleKey`

### Piece 4: Main-Loop Integration

**File:** `src/main.cc`

**The gating pattern:**
```cpp
// Input priority (typically after dialog, before general input)
if (clientXxxActive()) {
    clientXxxHandleKey(keyCode);
} else if (keyCode == KEY_ESCAPE) {
    break;
}

// Wall off the rest of the loop
if (!clientXxxActive()) {
    // combat, skills, inventory, mouse, render...
}
```

**Reference:** `main.cc:1117-1121` (dialog input gating), `main.cc:1123-1385` (rest of loop walled off)

### Piece 5: Server-Side Block-and-Pump + Trust-Boundary Verbs

**Intent queue model** (for modals that need viewer input):
```cpp
// src/{name}_intent.{h,cc} — model dialog_intent.{h,cc} (41 lines)
enum XxxIntentType { XXX_INTENT_ACTION, XXX_INTENT_END };
struct XxxIntent { XxxIntentType type; int param; };
void xxxIntentPush(XxxIntentType type, int param);
bool xxxIntentPeek(XxxIntent* out);
bool xxxIntentPending();
void xxxIntentClear();
```

**Server barrier** (in the modal's server `serverLoopActive()` branch):
```cpp
while (!haveIntent) {
    if (!gXxxServerPump()) { break; }
    haveIntent = xxxIntentPeek(&intent);
}
```

**Pump body** (installed in `server_main.cc`, uninstalled after `serverServe`):
```cpp
static int xxxServerPump() {
    if (_game_user_wants_to_quit != 0) return 0;
    if (combatStarted) return 0;
    if (noClients) return 0;
    serverControlBeginDrain();
    netSink.pollInbound(serverControlLine);
    usleep(4000); // don't busy-spin
    return 1;
}
```

**Trust boundary** (`server_control.cc`):
```cpp
// Route wire verbs through claim-gated + mode-active-gated handlers
if (verb == "xxxact") {
    if (sessionId != gClaimant) { /* reject */ return; }
    if (!gXxxActive()) { /* reject */ return; }
    xxxIntentPush(XXX_INTENT_ACTION, param);
}
```

**Reference:**
- `dialog_intent.{h,cc}` — intent queue
- `server_main.cc:195-236` — dialog pump install
- `server_control.cc:490-513` — `dsay`/`dend` routing

## The A0→A3 Dialect (Dialog's Proven Staging)

| Stage | What | Commit Pattern |
|---|---|---|
| **A0** | Link server engine into `f2_server`, retire stubs, create UI stubs | Mechanical, byte-identical |
| **A1** | Install server-side entry point (e.g., `ScriptRequestHandler::dialogEnter`) | Wire the trigger |
| **A2** | Block-and-pump barrier + emit events + intent queue | Behind feature flag |
| **A3** | Viewer render + input + main-loop integration + wire verbs | Full modal working |

## What Makes a Modal "Easier" vs "Harder"

| Factor | Easy (dialog/barter) | Hard (worldmap) |
|---|---|---|
| **Render complexity** | Text + list + highlight | Full graphical surface with FRM art, tiles, fog-of-war, scrolling |
| **Interaction model** | Keys 1-9 + ESC | Mouse click-to-walk, click-to-enter, scroll, tabs, hotspots |
| **State to stream** | Dialog node text + options | Position, gas, visited tiles (42/tile × N tiles), car, encounter state |
| **Map transitions** | None | Encounter maps require rebaseline + critter spawn |
| **Sub-modals** | None | Town-map entrance picker, encounter Yes/No dialog |
| **Viewer code size** | ~150 lines | ~2000 lines (port from worldmap_ui.cc) |
| **Need for intent queue** | Yes (player chooses option) | Yes (player chooses destination/entrance) |
| **Need for block-and-pump** | Yes | Yes |
| **Server sim already done** | N/A | Yes (all 5 travel helpers are pure-sim + golden-covered) |

## Completed Modals

| Modal | GameMode | Viewer Module | Server Handler | Intent Queue | Status |
|---|---|---|---|---|---|
| Inventory | `kInventory` | Built-in (vanilla) | N/A (viewer-owned) | N/A | Done |
| Skilldex | `kSkilldex` | Built-in (vanilla) | N/A (viewer-owned) | N/A | Done |
| Pipboy | `kPipboy` | Built-in (vanilla) | N/A (viewer-owned) | N/A | Done |
| Loot | `kLoot` | Built-in (vanilla) | Server streams container state | N/A | Done |
| UseOn | `kUseOn` | Built-in (vanilla) | N/A (viewer-owned) | N/A | Done |
| Rest | Built-in | Built-in (vanilla) | `serverLoopActive()` drain | N/A | Done |
| Endgame | Built-in | Built-in (vanilla) | `serverLoopActive()` drain | N/A | Done |
| Dialog | `kDialog` | `client_dialog.{h,cc}` | `ServerScriptRequestHandler::dialogEnter` | `dialog_intent.{h,cc}` | **DONE** (2026-07-19) |
| Barter | `kBarter` | `client_barter.{h,cc}` (to do) | `inventoryOpenTrade` drain (done) | `barter_intent.{h,cc}` (to do) | **ACTIVE** |
| Worldmap | `kWorldmap` | `client_worldmap.{h,cc}` (to do) | `ServerScriptRequestHandler::worldMap` (no-op, to do) | `worldmap_intent.{h,cc}` (to do) | **NOT STARTED** |

## The `ScriptRequestHandler` Seam

**File:** `src/script_request_handler.h`

This is the dispatch point for all vanilla modal entry paths. Abstract virtuals:

```cpp
virtual void townMap() {}           // SCRIPT_REQUEST_TOWN_MAP → city entrance picker
virtual void worldMap() {}          // SCRIPT_REQUEST_WORLD_MAP → global map travel
virtual void dialogEnter(Object*) {} // SCRIPT_REQUEST_DIALOG → conversation
virtual void endgame() {}           // endgame slideshow
virtual void looting() {}           // looting screen
virtual void stealing() {}          // stealing screen
virtual void elevatorSelect(...) {} // elevator floor picker
```

**Server handler** (`script_request_handler_server.h`): Only `dialogEnter` is wired → `gameDialogEnter`. All others are base no-ops. The server **drops** worldmap/townmap/endgame/loot/steal requests by design.

**Client handler** (`script_request_handler_client.cc`): All wired to vanilla blocking loops.

**Entry points that hit this seam:**
- Script opcode `0x8108` → `op_scripts_request_world_map` → `scriptRequestHandler()->worldMap()`
- Map transition with `map == -2` → `mapHandleTransition` → `scriptRequestHandler()->worldMap()`
- Map transition with `map == -1` → `mapHandleTransition` → `scriptRequestHandler()->townMap()`
- Script opcode `0x810A` → dialog entry