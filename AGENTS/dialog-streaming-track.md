---
name: dialog-streaming-track
description: "DONE (2026-07-19). Dialog option streaming to the wire viewer: engine linked (A0), server entry (A1), block-and-pump barrier (A2), viewer render + input (A3). Temple-demo dialog blocker resolved."
metadata: 
  node_type: memory
  type: project
  status: done
  modified: 2026-07-19T23:00:00.000Z
  originSessionId: 55b2b5c4-0450-4c7e-8283-b9b780e2c4e5
---

## ►► DONE — dialog streaming complete (A0→A3)

All four stages committed. Plan of record = `DIALOG_STREAMING_PLAN.md` (repo root).

- **✅ A0** — game_dialog.cc linked into f2_server, 24 collisions retired
- **✅ A1** — ServerScriptRequestHandler dialogEnter→gameDialogEnter
- **✅ A2** — block-and-pump barrier, dialogEmitNode, dsay/dend trust-boundary routing,
  renderer chain severed (Color2RGB/mouseHideCursor/mouseShowCursor/windowDestroy)
- **✅ A3** — client_dialog.{h,cc}: _gdialogInitFromScript → _gdProcessInit sends
  EVENT_DIALOG_NODE to viewer → window opens with reply+options rendered. Number keys
  → dsay, ESC → dend. kDialog in kViewerModalMask. TALK→talk un-fallbacked. Owner-
  editable vs spectator read-only gate. Barter→talk button ('T' key) wired. Mouse
  hover highlight (option enter/exit) wired.

### Known polish (non-blocking)
- Promote pump off F2_DIALOG_STREAM flag to always-on (needs adversarial review of
  sticky-bail, gate non-dialog verbs mid-dialog, hazard-5 gGameDialogSpeaker rebaseline
  revalidation)
- F2_DIALOG_STREAM=1 + F2_SERVER_PRES_RECORD=1 already baked into `viewer_live.sh`

### Live-verify
`scripts/viewer_live.sh` (must add F2_DIALOG_STREAM=1 on the f2_server line). Walk
to a scripted NPC, click TALK. Dialog window renders with talking head and options.

### Next: [[barter-streaming-track]]

See [[dialog-headless-plan]], [[p5-server-plan]], [[BARTER_STREAMING_PLAN.md]].