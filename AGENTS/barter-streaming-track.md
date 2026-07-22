---
name: barter-streaming-track
description: "►► ACTIVE — wire the barter/trade modal for the viewer. Server-side engine already done (inventoryOpenTrade serverLoopActive drain); remaing = viewer entry (B0), table streaming (B1), wire verbs (B2), modal integration (B3)."
metadata: 
  node_type: memory
  type: project
  modified: 2026-07-19T23:00:00.000Z
---

## ►► ACTIVE track — barter streaming to the viewer

Barter is the trade/inventory sub-modal reachable from dialog when talking to a
merchant NPC. Plan of record = `BARTER_STREAMING_PLAN.md` (repo root, JUST CREATED).
Read it FIRST for architecture + staged decomposition.

**Key fact**: Server-side barter engine is largely DONE — `inventoryOpenTrade` already
has a full `serverLoopActive()` drain loop processing OFFER/TAKE/UNOFFER/COMMIT/DONE
intents from the `barter_intent` queue. The gap is the viewer side.

**Resume ladder** (see BARTER_STREAMING_PLAN.md for detail):
- **B0** — Enter `inventoryOpenTrade` on viewer (~1 session)
- **B1** — Table object netIds + inventory streaming (~1-2 sessions, the real work)
- **B2** — Wire verbs: boffer/btake/bunoffer/bcommit through serverControlLine (~1 session)
- **B3** — Modal integration: kBarter in kViewerModalMask, matching teardown (~1 session)

**Estimated**: 3-4 sessions total. Much smaller than dialog was — no engine relocation,
no symbol collisions.

### Current state (viewer sees static barter window)
- `gameDialogTicker` processes mode 2→3 → calls `_gdialog_barter_create_win()` which
  shows barter window with Talk/Offer buttons
- 'T' key → `_barter_end_to_talk_to()` wired (returns to dialog)
- `kBarter` (0x20000) exists in GameMode, NOT yet in kViewerModalMask
- `inventoryOpenTrade` (the interactive trade screen) is NEVER entered on viewer

### Fresh-session bootstrap
1. Read `BARTER_STREAMING_PLAN.md` (repo root)
2. Read [[p5-server-plan]] for RESUME PROTOCOL + gate/live-demo commands
3. Read `src/inventory_ui.cc:4481` (`inventoryOpenTrade`) for existing server drain
4. Read `src/game_dialog.cc:2029-2057` for server barter branch in `_gdProcess`
5. Read `src/barter_intent.{h,cc}` for the intent queue (already exists)
6. Gate = `scripts/check.sh` → must pass "ALL GATES PASS"

### Predecessor
[[dialog-streaming-track]] (DONE — A0→A3 completed, dialog now renders on viewer)

See also [[p5-server-plan]], [[BARTER_STREAMING_PLAN.md]], [[dialog-headless-plan]].

Active bug specs (repo `bugs/`):
- `bugs/001-dialog-range-early-fire.md` — FIXED
- `bugs/003-containers-not-opening.md` — FIXED (serverLoopActive decouple in _obj_use_container)
- `bugs/004-migrate-doors-to-pres-replay.md` — DONE (all discrete actions → PRES_SEQ)
- `bugs/005-dialog-lips-audio.md` — SPEC (voiced dialogue)
- `bugs/006-in-combat-item-use.md` — SPEC (medic skills in combat)
- `bugs/007-character-creation.md` — SPEC (post-demo)
- `bugs/008-multiplayer-coop.md` — SPEC (demo 0.2v requirements)