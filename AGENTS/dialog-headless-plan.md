---
name: dialog-headless-plan
description: "The headless dialog engine (v1 shipped) — durable structure facts, probe verbs, and the v1 scope boundaries. Reference substrate for the active [[dialog-streaming-track]]."
metadata: 
  node_type: memory
  type: project
  originSessionId: 38343c62-84e6-420c-8476-b1c2611c38a7
  modified: 2026-07-19T05:10:43.086Z
---

Dialog v1 is headless-drivable and golden-covered (denbus1_dialog). This memory is the durable
engine reference; the live work is [[dialog-streaming-track]] (stream options to the viewer).

## ►► STRUCTURE (the hard truth — load-bearing)
Dialog runs SYNCHRONOUSLY nested in the script interpreter — a CALL STACK, not a steppable state
machine: `SCRIPT_PROC_TALK` → `gsay_*` opcodes → `_gdProcess` (blocking `for(;;)`) →
`_gdProcessChoice(i)` → `_executeProcedure(proc)` (runs MORE script). All in `src/game_dialog.cc`
(opcodes in interpreter_extra.cc, plumbing in scripts.cc). The whole blocking convo runs+finishes
INSIDE scriptExecProc(TALK); `_dialogue_state` is 0 at the gameDialogEnter gate (state==4 is
BARTER, a red herring). The server loop already calls scriptsHandleRequests, so a queued dialog
request fires headlessly.

## ►► HOW v1 DRIVES IT (the shape the streaming work extends)
A `serverLoopActive()` branch creates NO window and drains a `dialog_intent` FIFO (SELECT/END,
like combat_intent), calling the SAME `_gdProcessChoice → _executeProcedure` the keyboard path
uses. 4 window/render skips (_gdCreateHeadWindow, _gdProcessInit, _gdProcessUpdate, the
_gdProcessChoice render block — leaving windows=-1 makes fidget/transition pumps + gameDialogTicker
self-guard to no-ops) + the gameDialogEnter LOS/talk-range gate bypassed (approach locomotion
dropped, same accepted class as door/pickup/skill). _gdSetupFidget forced to the no-head path so
its fidget-variant RNG is never drawn → server dialog is RNG-ISOLATED for every NPC.

## ►► DATA MODEL (plain globals, no UI query needed)
- Reply: `gDialogReplyText[900]` + `gDialogReplyMessageListId/Id`.
- Options: `gDialogOptionEntries[]`, count `gGameDialogOptionEntriesLength`. Struct =
  {messageListId, messageId, reaction, proc, btn, top, text[900], bottom}; option i's `proc` is
  the script proc run on select.
- Opcode flow: gsay_start → (gsay_reply + N×gsay_option)* → gsay_end (which CALLS _gdProcess =
  what blocks). gsay_message = reply + a synthetic [Done].

## ►► PROBE VERBS (main.cc)
`dtalk:<scriptIndex>` (nearest scripted critter of that script — obj->id is NOT unique for map
objects, match on scriptIndex/sid), `dsay:<optionIndex>`, `dend`. Push dsay/dend BEFORE dtalk
(intents queue; dtalk fires the whole convo inline via scriptsHandleRequests at tick end,
draining them). Opt-in `F2_DIALOG_TRACE` dumps reply/option text (invisible headless otherwise) —
the tool for authoring dialog goldens.

## ►► v1 SCOPE BOUNDARIES (respect these in the streaming work; not bugs)
- Reply/option only. BARTER aborts; party-control / combat-start nested modes are UI-button-only
  and unreachable headless.
- A dtalk that FAILS to open (sid==-1 / isInCombat / scriptOverrides) leaves queued dsay intents
  (cleared only per-serverRun + at convo end, not on dtalk-failure) — fine for single-dialog runs.

See [[modal-drivers-plan]] (the driver pattern), [[dialog-streaming-track]] (active track).
</content>
