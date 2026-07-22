# Barter Streaming Plan — wire the trade/inventory modal for the viewer

Status: **plan** (2026-07-19). Barter is the sub-screen reachable from dialog when
talking to a merchant NPC. Today the server-side barter engine is largely implemented
(headless drain loop in `inventoryOpenTrade`; `barter_intent` queue → OFFER/TAKE/
UNOFFER/COMMIT/DONE). The gap is the viewer: `inventoryOpenTrade` is never entered
(the viewer skips `_gdProcess`), and the barter table objects have no netIds so the
viewer can't mirror their inventories.

---

## Architecture (what already exists)

### Server side (working)
- `_gdProcess` server branch (game_dialog.cc:2029-2057): when `_dialogue_switch_mode==2`
  (barter proc fired), calls `_gdialog_barter_create_win()` + `inventoryOpenTrade()` +
  cleanup inline, bypassing the ticker
- `inventoryOpenTrade()` (inventory_ui.cc:4481+): FULL `serverLoopActive()` drain loop:
  - Peek `barterIntentPeek()`, process OFFER/TAKE/UNOFFER/COMMIT/DONE/CANCEL
  - `itemMoveForce` moves items between dude ↔ playerTable and merchant ↔ bartererTable
  - `barterAttemptTransaction` commits the offer
  - DONE: `itemMoveAll` back to owners + `_barter_end_to_talk_to()`
  - After drain, server returns to `_gdProcess` → emits next dialog node (or end)
- `barter_intent.{h,cc}`: FIFO intent queue, analogous to `dialog_intent`
- `command.cc`: `barter` push-through probe for headless test (OFFER/TAKE/COMMIT/DONE)

### Viewer side (partially wired)
- `gameDialogTicker` processes mode 2→3: calls `_gdialog_barter_create_win()` which
  creates the static barter window with Talk/Offer buttons (frmId 111 or 420)
- 'T' key → `_barter_end_to_talk_to()` (wired in clientDialogsHandleKey)
- `kBarter` (GameMode::kBarter = 0x20000) exists in game.h
- Not yet in `kViewerModalMask` — the ticker won't pump inside barter

### What's missing
- **`inventoryOpenTrade` is never entered on the viewer**: called from `_gdProcess`
  mode-3 branch (game_dialog.cc:2106) which we bypass; the viewer only sees the
  static barter window from `_gdialog_barter_create_win`
- **Table objects have no netIds**: `_peon_table_obj` / `_barterer_table_obj` are
  NO_SAVE transients created with pid=-1 — the viewer can't mirror them or reconcile
  their inventory deltas
- **No wire verbs**: barter commands (offer/take/unoffer/commit) need claim-gated
  routing through `serverControlLine` → `barterIntentPush`
- **No table state events**: the viewer needs to know what items are on each table
  to render the trade screen

---

## Staged decomposition

### B0 — Enter barter on the viewer (small, ~1 session)
**Goal**: When the ticker sets mode 3 and creates the barter window, the viewer enters
`inventoryOpenTrade` (the real interactive trade screen) instead of just showing the
static window.

- In `_gdialog_barter_create_win()` or the ticker mode-3 path: call
  `inventoryOpenTrade(gGameDialogWindow, gGameDialogSpeaker, _peon_table_obj,
  _barterer_table_obj, gGameDialogBarterModifier)` on the non-server path
- `_gdialog_barter_create_win` is called from the ticker with `serverLoopActive()` =
  false on the viewer → the non-server path creates the SDL trade window normally
- The barter window from `_gdialog_barter_create_win` becomes the container; the
  `inventoryOpenTrade` call fills it with item lists
- `kBarter` should already be in `kViewerModalMask` from this point
- Gate check: does `inventoryOpenTrade` barter loop work on the viewer without
  `_gdProcess`? The barter loop is self-contained — it calls `inputGetInput()`,
  `renderPresent()`, and its own rendering functions. The viewer's `inputGetInput`
  will fire the service ticker (once kBarter is in the mask), keeping the wire
  pumping inside the modal.

**Risk**: `inventoryOpenTrade` calls `_setup_inventory(INVENTORY_WINDOW_TYPE_TRADE)`
which may abort or render incorrectly because the barter table objects are empty (no
items streamed yet) and have pid=-1. The static setup (window, slot lists, empty
tables) should still render; items won't appear until table state is streamed (B1).

### B1 — Table state + inventory streaming (medium, ~1-2 sessions)
**Goal**: The viewer sees barter table contents and can interact with items.

**Approach**: Model table objects as pseudo-objects with netIds and stream them.

- Server `_gdialog_barter_create_win()` → assign netIds to `_peon_table_obj` /
  `_barterer_table_obj` via `objectAssignAllNetIds` or a targeted assign
- Emit a `EVENT_BARTER_BEGIN` event carrying the two table netIds + the
  merchant's netId + barter modifier
- After that, inventory changes (items moving onto/off tables) stream naturally
  as `EVENT_OBJECT_DELTA` with `OBJECT_DELTA_INVENTORY` on the table objects
- Viewer decodes → has table objects in its mirror → `inventoryOpenTrade` renders
  item lists from the mirrored table inventories

**Alternative (simpler, v1)**: Instead of full inventory deltas, stream the table
contents as a one-shot snapshot in a new `EVENT_BARTER_TABLES` event each time an
offer/take/unoffer/commit happens. Simpler but less elegant; the deltas are free
if we just assign netIds to the tables.

**Risk**: Table objects are NO_SAVE transients (pid=-1). netId assignment for
transient-composite objects has precedent issues (see memory:
`presenter-object-ref-model.md` — "OBJ_CREATE is dead code for composite
transients"). May need to model them as non-NO_SAVE top-level objects with a
barter-specific pid, or use a dedicated wire event for table updates without
netIds.

### B2 — Wire verbs (small, ~1 session)
**Goal**: Viewer sends barter commands upstream through the trust boundary.

- Route through `serverControlLine`, claim-gated + `kBarter`-active-gated:
  - `boffer <pid> [qty]` → `barterIntentPush(BARTER_INTENT_OFFER_ITEM, pid, qty)`
  - `btake <pid> [qty]` → `barterIntentPush(BARTER_INTENT_TAKE_ITEM, pid, qty)`
  - `bunoffer <pid> [qty]` → `barterIntentPush(BARTER_INTENT_UNOFFER_ITEM, pid, qty)`
  - `bcommit` → `barterIntentPush(BARTER_INTENT_COMMIT, 0, 0)`
  - `bdone` → `barterIntentPush(BARTER_INTENT_DONE, 0, 0)` (already handled by 'T' key)
- Follow existing pattern: `dsay`/`dend` routing (`server_control.cc:489-527`)
- Viewer hooks: `inventoryOpenTrade`'s existing LMB slot handlers → call
  `clientViewerBarterOffer(pid)` etc. (parallel to `clientViewerDialogSay`)

### B3 — Viewer modal integration (small, ~1 session)
- Add `kBarter` to `kViewerModalMask` (client_net.cc:128)
- The service ticker pumps wire inside barter + force-closes on combat/rebase
- Barter screen → 'T' key handling already wired (clientDialogsHandleKey)
- On barter end: viewer exits `inventoryOpenTrade` → ticker recreates dialog
  window → viewer sees next dialog node (or end)

---

## Critical files
- `src/inventory_ui.cc` — `inventoryOpenTrade()` (line 4481): server drain loop +
  viewer interactive loop
- `src/game_dialog.cc` — server barter branch (:2029-2057), `_gdialog_barter_create_win`
  (:3491, table object creation), `gameDialogTicker` mode 2/3 handling (:3083-3108)
- `src/barter_intent.{h,cc}` — intent queue (already exists)
- `src/server_control.cc` — barter verb routing (follow dsay/dend pattern)
- `src/client_net.cc` — `kViewerModalMask`, barter send helpers
- `src/client_dialog.cc` — 'T' key for barter→talk (already wired)

## Ranked hazards

1. **[HIGH] Table object netId assignment**: The barter table objects (pid=-1, fid=-1)
   are NO_SAVE composites created inline during `_gdialog_barter_create_win`. The
   viewer-side mirror needs to track them. Using OBJECT_DELTA_INVENTORY (already on
   the wire) is the cleanest path but requires these objects to have netIds in the
   syncable domain. Alternative: dedicated wire events with pid+quantity snapshots
   → simpler but diverges from the presenter infrastructure.

2. **[MED] `inventoryOpenTrade` on viewer**: The barter loop runs `inputGetInput()`
   which blocks. The service ticker (`kBarter` in modal mask) already pumps the wire
   inside that block. But the ticker also force-ESCs on combat/rebase — the barter
   teardown needs to handle ESC cleanly (restore merchant items, destroy hidden box).
   The server's ESC/CANCEL path (break → teardown) is simple; the viewer's needs to
   match.

3. **[MED] `_barter_end_to_talk_to` → dialog re-entry**: When barter ends, the server
   continues in `_gdProcess` and updates dialog state. The viewer exits
   `inventoryOpenTrade` and needs to re-render reply/options. The 'T' key handler
   already calls `_barter_end_to_talk_to()` → ticker processes mode 1 → recreates
   dialog window. If a new EVENT_DIALOG_NODE arrives from the server, the viewer seeds
   new content. If no new node arrives (same-conversation return), the viewer shows
   the previous node's options. Both paths should work after B0.

4. **[LOW] No server hook needed**: Contrary to initial assumptions, `inventoryOpenTrade`
   already has a full `serverLoopActive()` drain path. No new hook infrastructure needed.
   The `gDialogTradeHook` mentioned in DIALOG_STREAMING_PLAN is not required for v1.

5. **[LOW] `inventoryOpenTrade` server guard hazard**: The non-server path calls
   `_setup_inventory(INVENTORY_WINDOW_TYPE_TRADE)` which exits(1) when the back window
   is -1. The server guard (`serverLoopActive()` at :4518) prevents this. On the viewer,
   `_barter_back_win` is set to `gGameDialogWindow` which should be valid (created by
   `_gdialog_barter_create_win`). Verify.

---

## Difference from dialog streaming (reuse vs new)

Barter is **smaller scope** than dialog streaming was:
- **Server engine already done** — no game_dialog.cc relocation, no collisions, no
  server-shim stubs needed. `inventoryOpenTrade` already links into f2_client (it's
  in `inventory_ui.cc` which is in the f2_client library), and the serverLoopActive()
  drain is already implemented.
- **No block-and-pump barrier** — the server already drains barter intents inline
  inside `_gdProcess`'s existing server loop. No new pump to design.
- **Table streaming is the real work** (B1) — same class as inventory deltas /
  container contents, a known A2 gap.
- **Viewer modal** follows the existing modal pattern (loot, inventory, dialog):
  kViewerModalMask + wire verbs + force-close on combat/rebase.

Estimated: **3-4 sessions** total, with B1 (table streaming) being the lion's share.