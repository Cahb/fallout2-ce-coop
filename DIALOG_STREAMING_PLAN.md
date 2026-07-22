# Dialog Option Streaming to the Viewer — plan of record (Phase A1)

Status: **plan** (2026-07-18, recon-backed). The Temple-demo hard blocker. The dialog
ENGINE already works headless (`denbus1_dialog` golden; `dialog_intent` driver — see
`memory/dialog-headless-plan.md`); the GAP is viewer render + input + option streaming.
Today the viewer's TALK falls back to LOOK. See also `TEMPLE_DEMO_ROADMAP.md` (A1),
`MP_PROTOCOL.md`, `INTERACTION_UX_DESIGN.md`, `memory/p5-server-plan.md` (active track).

---

## ►► ARCHITECTURE RESOLVED (2026-07-19, Fable design pass — source-verified) — READ FIRST

The plan below assumed the block-and-pump seam installs from `server_main.cc` (f2_server).
**It can't yet: the dialog engine (`game_dialog.cc` + the `gsay_*` opcodes) is f2_client-ONLY,
and f2_server drops every dialog request at the null `ScriptRequestHandler`** (scripts.cc:1011
→ base no-op; server_stubs.cc:322 "the server drops dialog requests"). The `serverLoopActive`
headless-dialog branches run ONLY in the `fallout2-ce` binary — that is where `denbus1_dialog`
runs (`run_golden_server.sh` `BIN=build/fallout2-ce` under `F2_SERVER_LOOP=1`). So the dialog
DRIVER must first become reachable from f2_server. The dialog choice procs mutate authoritative
gvars/lvars → they MUST run server-side; the viewer only renders a node + returns the picked index.

**VERDICT: Option A now — add `game_dialog.cc` to the f2_server executable link — executed
deliberately as the enacted waypoint to Option B (split driver→core), which is banked with the
post-demo restructure. Reject C (hosting the wire on fallout2-ce). Ship the Temple demo on A.**

WHY A is principled, not debt: the dialog engine API is ALREADY on the core→client severance
surface — f2_core's `interpreter_extra.cc` calls ~20 `game_dialog.h` entry points directly
(`_op_gsay_*`, `_gdialogInitFromScript`, opcode regs), today satisfied on f2_server by ABORT
stubs. The engine is semantically core (executes authoritative script procs); it is only
syntactically client because the file also holds its renderer. A moves the engine to the side of
the seam its callers + authority already live on; the ~58 symbols it drags are the RENDERER's
deps, all behind the proven `serverLoopActive()` guards, all landing on the abort dashboard where
a mis-route fails loudly. C fails the doctrine from the other side (server_main/net/control are
f2_server-only; 11 CI gates + viewer_live.sh pin f2_server as host) and still leaves f2_server
dialog-incapable — reject.

**Fable's corrections to the earlier estimate (matter for implementation):**
- Measured gap = **~58 fallout symbols** unsatisfied (window*/button*/font*/lips*/gsound*/mouse*/
  blit*/FpsLimiter/inputGetInput/colorCycle/dialogSet*/inventoryOpenTrade/displayMonitorAddMessage).
- **24 duplicate-symbol collisions** must be RETIRED in the same commit or the link fails: 21
  function stubs + 3 data placeholders in server_stubs.cc AND the behavior shim **`_gdialogActive()`
  in server_shim.cc:64 (hardcoded `false` → becomes `_dialog_state_fix != 0`)**, plus the benign
  `gameDialogEnable/Disable` stubs. Semantic flips audited SAFE (§5.1 of the pass).
- **Barter is a SEPARATE track**: the headless barter body is NOT in game_dialog.cc — it's
  `inventoryOpenTrade` in `inventory_ui.cc` (another f2_client TU, +66 more symbols) and its
  DONE path hits abort stubs. v1 = an installable `gDialogTradeHook` (sibling of the pump);
  f2_server leaves it null → deterministic cancel (normalize `_dialogue_switch_mode=0/state=1`,
  break, dialogEnd). Matches hazard 7.

### Staged decomposition (the resume ladder)
- **✅ A0 DONE (`24dea73`)** — game_dialog.cc linked into f2_server, 24 collisions retired,
  the labeled "dialog-UI" stub block added (18 no-ops / 39 aborts / 7 inert data), 5 dialogSet*
  flipped. check.sh byte-identical. ✅ **A1 DONE (`81cf71d`)** — ServerScriptRequestHandler
  (dialogEnter→gameDialogEnter) installed from serverBoot(), f2_server-only; all gates green.
  ✅ **A2 SCAFFOLD DONE (`8a94b62`, DORMANT — NO REVIEW YET, owner-directed)** — server-side seams
  wired but INERT: (1) `dsay`/`dend` routed through serverControlLine (claimant + owner +
  `_gdialogActive()` gated) into the dialog intent queue; (2) `gameDialogSetServerPump` installed
  ENV-GATED off (`F2_DIALOG_STREAM`), uninstalled after serverServe. Pump body is an inert
  early-return placeholder (bails immediately — can't hang even flag-on). All gates byte-identical.
  🔶 **A2-ACTIVATE PARTIAL (unreviewed, gate-safe, owner-directed)** — real block-and-pump body
  installed behind `F2_DIALOG_STREAM` (server_main.cc: acceptPending-skipped/beginDrain/pollInbound
  + quit/combat/no-claimant bail); `game_dialog.cc` `_gdialogInitFromScript`/`_gdialogExitFromScript`
  now `serverLoopActive()`-guarded to skip ALL window/head/tickers/lips setup (BYTE-IDENTICAL — the
  window build was sim-neutral by dump time; denbus1_dialog golden unchanged, no re-bless). All gates pass.
  ✅ **A2-ACTIVATE RENDERER CHAIN SEVERED (`8964969`) — DIALOG FULLY DRIVES ON f2_server.**
  The chain the smoke (scripts/dialog_pump_smoke.sh) named is closed: `Color2RGB` (caller-guard
  _gdialogInitFromScript) → `mouseHideCursor`/`mouseShowCursor` (benign no-ops) → `windowDestroy`
  (caller-guard _gdProcessExit, symmetric to _gdProcessInit). VERIFIED: with F2_DIALOG_STREAM=1,
  the CMD-driven smoke opens dialog, the block-and-pump barrier emits each node + drains dsay/dend,
  walks 4 distinct real Den-Story-Teller nodes, ends clean, server runs to completion. Byte-identical
  (server-only guards/no-ops). STILL behind the F2_DIALOG_STREAM flag.
  ►► **REMAINING = A3 + the reviewed pump pass.** (1) **A3 viewer render** — the on-screen dialog
  UI: consume EVENT_DIALOG_NODE/_END on the viewer (client_dialog.{h,cc}), kDialog in
  kViewerModalMask, un-fallback TALK→talk (main.cc), send dsay/dend back over the wire (serverControlLine
  routing already exists). (2) **Reviewed pump pass** — promote off F2_DIALOG_STREAM to always-on;
  MANDATORY adversarial review of: STICKY-bail (inner nested-dialog bail unwinds outer _gdReenterLevel),
  gate non-dialog control verbs (mv/use) mid-dialog, hazard-5 (re-validate gGameDialogSpeaker
  netId+id+pid across a rebaseline; acceptPending is currently skipped mid-dialog). Then live-verify
  over the real SDL viewer (viewer_live.sh) — the Temple story-teller.
  - ⚠️ **A1's byte-identical golden is DEFERRED (open decision).** The stated acceptance —
    f2_server `denbus1_dialog` dump == fallout2-ce-headless dump — collides with **f2_server
    nondeterminism**: no F2_FAKE_CLOCK on the server (wall-clock steers AI) and denbus1 is
    AI-heavy (NOT in the deterministic-clean set). A byte-exact server dialog golden needs either
    the abandoned server fake-clock, or a deterministic map, or accepting a **liveness probe**
    instead (drive dtalk/dsay/dend over the CMD port, assert reach-exit + effect, not byte-exact).
    The handler itself is verified inert (gates green); this only gates the *regression golden*.
- **Stage A0 — link the engine (behavior-INERT).** Add `src/game_dialog.cc` to the **f2_server
  EXECUTABLE sources** (next to server_stubs.cc in CMakeLists.txt ~:476-484 — NOT to f2_core: it
  states "shared engine, client-homed, server links it pending the split" + keeps
  `check_core_no_sdl.sh` honest + leaves fallout2-ce's object untouched = golden-invisible).
  Delete the 24 collisions (21 fns + 3 data from server_stubs.cc, `_gdialogActive` from
  server_shim.cc). Add ONE delimited stub block headed "dialog-UI — retires wholesale with the
  game_dialog core/client split": ~40 loud `serverStubAbort` for functions the sim must NEVER
  reach + ~18 benign no-ops for the ones the live `_gdialogInitFromScript`/`ExitFromScript` path
  DOES call (see §5.3 list: fontGet/SetCurrent, dialogSetReply*/Option* [FLIP these 5 from abort],
  _dialogRegisterWinDrawCallbacks, colorCycleDisable/Enable, indicatorBar*, gameMouseObjects*,
  gameMouseSetCursor, _gmouse_*_scrolling, _gsound_background_volume_get_set,
  backgroundSound{Delete,Restart,SetVolume}, soundPlayFile). No handler installed → dialog still
  unreachable → ALL gates must pass BIT-FOR-BIT (machine-checked: linker + 11 gates).
- **Stage A1 — server dialog entry + golden parity.** New `script_request_handler_server.cc`
  (f2_server sources; installed from server_boot beside the presenters) with
  `ServerScriptRequestHandler::dialogEnter` → `gameDialogEnter(speaker, 0)`. Port `denbus1_dialog`
  (+ a barter-cancel case) to an f2_server golden; ACCEPTANCE = the state dump is BYTE-IDENTICAL
  on f2_server vs fallout2-ce-headless (both drive the same core sim via the same intent queue).
  Add a dialog case to record-purity.
- **Stage A2 — activate the drafted plumbing (ADVERSARIAL-REVIEW commit).** The dialog-side draft
  is already committed DORMANT (6fbe89c). Add: install `gDialogServerPump` from server_main.cc;
  route `dsay`/`dend` through `serverControlLine` claim-gated + `_gdialogActive()`-gated +
  owner-gated, pushing ONLY into `dialog_intent.cc` (never reach game_dialog statics from
  server_control — that would make A a dead end). The earlier server-side draft was reverted; it
  is re-specified here + the reverted diff was in scratchpad (ephemeral — regenerate from this).
  INVARIANT for review: the pump's bail must be STICKY (disconnect/quit stays false across calls)
  so an inner nested-dialog bail unwinds outer `_gdReenterLevel` levels.
- **Stage A3 — viewer render (plan Stage 3 below, unchanged).** `src/client_dialog.{h,cc}`,
  `kDialog` in `kViewerModalMask`, un-fallback TALK, owner-editable vs spectator-read-only.
  Live-verify Temple story-teller.
- **Stage B (BANKED, post-demo restructure).** Split `dialog_engine.cc` (f2_core) /
  `game_dialog_ui.cc` (f2_client) along the cut below; retire the whole "dialog-UI" stub block;
  the single-player interactive loop becomes a CLIENT pump (renders on `dialogNode`, pushes
  `DIALOG_INTENT_SELECT` from kb/mouse — local UI = viewer of local engine); barter-drain
  extraction rides the same wave. Add dialog symbols to `check_server_core_only.sh`'s forbidden
  list so the debt can't silently return.

### Stage-B cut line (bank now so A doesn't fight it)
- **→ f2_core `dialog_engine.cc` (~900 lines):** node/session state (gDialogReplyText/Program/
  MessageListId/Id, dword_58F4E0, gDialogOptionEntries + length, gGameDialogSpeaker/
  SpeakerIsPartyMember/HeadFid/Sid, _dialogue_state, _dialogue_switch_mode, _gdialog_state,
  _dialog_state_fix, _gdReenterLevel, gGameDialogBarterModifier, review ENTRIES array); driver
  fns (gameDialogEnter [error path → presenter()->consoleMessage], _gdialogActive/Start/Go,
  _gdProcess loop skeleton, _gdProcessChoice LOGIC half, _gdProcessUpdate LOGIC half — the
  existing serverLoopActive early-return at ~:2526 IS the in-function cut line, gameDialogGetOptionText,
  the 6 gameDialogSet*/Add*Option* opcode targets, _gdialogSayMessage, gameDialogBarter,
  gameDialogSetBackground/BarterModifier, dialogEmitNode, intent-drain + pump seam).
- **stays f2_client `game_dialog_ui.cc`:** _gdProcessInit/Exit/Cleanup, ALL window/button/blit/
  scroll/highlight/caps rendering, head window + fidget + transitions + gameDialogTicker + lips,
  review/party-control/party-custom/barter windows, red buttons, the interactive kb/mouse input
  half of _gdProcess.
- **Seam:** mostly collapses onto the already-added `presenter.h` `dialogNode`/`dialogEnd` +
  the intent queue as the input seam; does NOT collapse: session chrome
  (`_gdialogInitFromScript`/`ExitFromScript` window choreography), reaction→head-transition cue,
  and nested modals (barter/party — two-way, `ScriptRequestHandler`-shaped). Needs a small
  two-way client `DialogUi` handler (~5 methods).

### Commit state at park (2026-07-19)
S0 `3aaf035` (gameDialogGetOptionText) · S1 `f8ff06c` (wire events 32/33 + presenter dialogNode/
dialogEnd, inert) · **S2-draft DORMANT `6fbe89c`** (barrier block-and-pump + dialogEmitNode +
gameDialogSetServerPump; pump never installed → inert; goldens byte-identical). HEAD builds clean,
`check.sh` ALL GATES PASS. **Next session starts at Stage A0.**

---

## What already exists (works)
`talk` verb, walk-then-act approach latch, `scriptsRequestDialog(target)`, the headless
dialog driver, and the `dsay`/`dend`/`dtalk` intents (`dialog_intent.{h,cc}`). Dialog is
reached synchronously inside one beat: `serverControlAdvancePending` fires
`scriptsRequestDialog` → `scriptsHandleRequests()` (`server_loop.cc:269`) → `gameDialogEnter`
→ `_gdProcess`.

## The 4 missing pieces
1. **[the real work] Server-side block-and-pump barrier.** The headless server branch of
   `_gdProcess` (`game_dialog.cc:1892-1928`) **ends the conversation the instant the intent
   queue is empty** (`:1907-1911`). Fine when intents are pre-queued (the golden); fatal for
   a live viewer where nothing is queued — the entry node runs and instantly exits,
   invisible. This is the SERVER analog of the client "modal starves the pump" hazard.
2. **New wire events.** `EVENT_DIALOG_NODE=30` (PRESENTATION) + `EVENT_DIALOG_END=31`,
   mirroring `EVENT_ACTION_ANIM`/`EVENT_DOOR_STATE`.
3. **Viewer render + input.** Reuse the real vanilla gdialog window, seeded from the wire;
   number-key/click → `dsay <index>` / ESC → `dend`.
4. **Route `dsay`/`dend` through the trust boundary.** They live only in the unrestricted
   debug `commandDispatch` (`command.cc:648-658`), NOT in `serverControlLine`
   (`server_control.cc:356`). Add them there, claim-gated + dialog-active-gated.

## Non-obvious server-side gap (must fix — Stage 0)
`_gdProcessUpdate` early-returns for the server at `game_dialog.cc:2377`, **before** the
option-text-resolution loop at `:2413-2445`. So message-list options (`messageListId >= 0`,
the common case) have **empty `.text`** on the server; only `gameDialogAddTextOption`
options (`messageListId == -4`) carry text. The streaming emitter MUST resolve text itself:
extract a helper `gameDialogGetOptionText(index, out, size)` from the render loop (uses
`entry->text` for `-4`, else `_scr_get_msg_str_speech(entry->messageListId,
entry->messageId, 0)` — the exact call at `:2414`) and share it between render + emitter so
they never drift. **Trap:** the `F2_DIALOG_TRACE` at `:1899` prints empty `.text` for
message options → a text-option fixture looks fine and real content breaks.

## Where the node state lives (server knows all of it)
File-statics in `game_dialog.cc`: reply `gDialogReplyText[900]` (`:531`); options
`gDialogOptionEntries[]` (`:534`, struct `:102-111` = messageListId/messageId/reaction/
**proc**/btn/**text[900]**), count `gGameDialogOptionEntriesLength` (`:154`); speaker
`gGameDialogSpeaker` (`:308`). Node transition: numeric key → `_gdProcessChoice(index)`
(`:2141`) → `_executeProcedure(gDialogReplyProgram, entry->proc)` (`:2204`) repopulates the
node → `_gdProcessUpdate()` (`:2214`). Returns `-1` when the proc registers no new options.

## Intents (exact)
- `dtalk <scriptIndex>` (`command.cc:613`): nearest live critter with `scriptIndex==arg`
  (scriptIndex NOT obj->id — ids collide ~53%, MP_PROTOCOL §7) → `scriptsRequestDialog`.
- `dsay <index>` (`command.cc:648`): `dialogIntentPush(DIALOG_INTENT_SELECT, index)`,
  0-based, bounds-checked at drain vs `gGameDialogOptionEntriesLength` (`:1922`).
- `dend` (`command.cc:654`): `dialogIntentPush(DIALOG_INTENT_END, 0)`.

## Design decision: block-and-pump, NOT a resumable machine
Combat's resumable session worked because `_combat` sets up and RETURNS. `_gdProcess` drives
node transitions *through* `_executeProcedure` synchronously and can recurse into sub-dialogs
(`_gdReenterLevel`) — CPS-rewriting that is large + risky. Blocking is explicitly sanctioned:
`MP_PROTOCOL` §1 lists a dialog choice as an accepted input barrier, and vanilla freezes the
world during dialog anyway. **v1 cost: while one actor is in dialog, the server tick blocks;
a 2nd viewer's world is frozen** — but see the co-op refinement below, which turns that from a
liability into read-only spectating. Resumable machine = banked scale-up (Stage 4).

The seam: `_gdProcess` is in f2_core, cannot touch `SocketByteSink`. Install a
`std::function<bool()> gDialogServerPump` from `server_main.cc` (returns false = bail); body =
`netSink.acceptPending()` + `serverControlBeginDrain` + `netSink.pollInbound(serverControlLine)`
+ short `usleep`, mirroring the main drain (`server_main.cc:154-165`). Replace the empty-queue
`break` with: while queue empty, call the pump, re-peek; bail (emit `dialogEnd`, break) on
claimant disconnect / `isInCombat()` / `_game_user_wants_to_quit`.

## CO-OP DIALOG refinement (user, 2026-07-18) — "all see, only the owner drives"
Because `EVENT_DIALOG_NODE` ships via `flushFrame()`, it **broadcasts to every viewer by
default** (like every presentation event). So co-op dialog is nearly free and is the natural
"all render, one actor commits" invariant the whole engine already follows:
- `EVENT_DIALOG_NODE` stamps the **driver's actor netId** (the talking dude). Each viewer
  compares to ITS OWN actor: match → render **editable**; mismatch → **read-only** ("someone
  is talking" indicator; number-keys/clicks do nothing; ESC does NOT send `dend`).
- `EVENT_DIALOG_END` tears down EVERY viewer's window (spectators can't dismiss early).
- `serverControlLine` gates `dsay`/`dend` on "is this claimant the active dialog's OWNER" —
  a spectator's `dsay` is a no-op. One condition, not a new mechanism.
- **v1 caveat:** P2 does not exist yet (`gDude` singleton) → today every viewer is a
  spectator-window of the SAME dude; the multi-player payoff lands in Phase B. But build the
  read-only render + owner-stamp NOW — it is nearly free and is the mp-actor rule in action:
  **gate on actor-ownership, never hardcode "viewer 0 drives."** See
  `memory/mp-actor-architecture-principle.md`.

## Viewer render approach — reuse the real vanilla gdialog window
On first `EVENT_DIALOG_NODE`: set `gGameDialogSpeaker = objectFindByNetId(speakerNetId)`,
derive headFid, call the real `_gdialogInitFromScript(headFid, reaction)` (`game_dialog.cc:884`
— builds head + reply/options windows, enters `GameMode::kDialog`; window creation runs
normally on the viewer since `serverLoopActive()` is false there). Seed `gDialogReplyText` +
`gDialogOptionEntries[]` from the wire as **text options** (`messageListId=-4`, `proc=0`, fill
`.text`) so the vanilla renderers use `.text` and never touch script msg-lists or
`_executeProcedure`. Run a loop adapted from `_gdProcess`'s INPUT half (`:2084-2116`) but
**forked at the commit point**: capture the selected index → `conn.sendLine("dsay <index>")`
(or `dend` on ESC/0), NEVER call `_gdProcessChoice`. Block+pump until the next node re-seeds or
`EVENT_DIALOG_END` breaks → `_gdialogExitFromScript` (`:946`). Home: new
`src/client_dialog.{h,cc}` (sibling of `client_combat_anim.cc`). Send RAW resolved option text;
the viewer applies its own "1. " prefix so server/client SFALL `gNumberOptions` can't disagree.

## Staged plan (cheapest-first)
- **Stage 0 — option-text helper.** Extract `gameDialogGetOptionText` (`game_dialog.cc:2413-
  2445`), use in render loop. Pure refactor, golden-invisible.
- **Stage 1 — wire events + presenter plumbing (inert).** Base virtuals `dialogNode`/
  `dialogEnd` (`presenter.h`), NetworkPresenter overrides with forced `flushFrame()` (model
  `snapshotEnd` `presenter_network.cc:354-368`), enum `EVENT_DIALOG_NODE=30`/`_END=31`
  (`presenter_network.cc:75-110`, `client_net.cc:77-101`). No emit sites → byte-identical.
- **Stage 2 — server barrier + emit + pump seam + trust boundary.** Emit `dialogNode`
  (resolve options via Stage 0, stamp owner netId) at loop top; block-and-pump; emit
  `dialogEnd` on all exits; install `gDialogServerPump` from `server_main.cc`; route
  `dsay`/`dend` through `serverControlLine` claim-gated + dialog-active + owner-gated.
  **MANDATORY adversarial review** (new indefinitely-blocking server loop + trust boundary —
  the risk concentrates HERE). `denbus1_dialog` golden must stay green (pre-queued intents
  drain before the pump is reached).
- **Stage 3 — viewer render + input + co-op.** `src/client_dialog.{h,cc}`; add
  `GameMode::kDialog` to `kViewerModalMask` (`client_net.cc:121`); TALK→`talk` (`main.cc:795,
  839`); owner-editable vs spectator-read-only; defer rebaseline while `kDialog` open.
  Live-verify: walk → talk → node renders → pick → next node → exit (Temple story-teller).
- **Stage 4 (banked):** resumable dialog session (unblock tick for true multi-viewer);
  barter/skilldex-in-dialog streaming; talking-head lip-sync/fidget fidelity.

## Ranked hazards
1. **[HIGH]** Viewer modal starves `conn.pump()` — solved by adding `kDialog` to
   `kViewerModalMask` so `viewerServiceTicker` (`client_net.cc:2315`) pumps + force-ESC on
   combat/rebaseline/disconnect; the viewer loop top must re-check + force-exit emitting no
   verb (model `viewerPollPendingLoot` `main.cc:705`). Defer rebaseline mid-dialog.
2. **[HIGH]** Server tick blocks for the whole conversation — the pump hook must always make
   progress + always have a bail (disconnect/combat/quit) so the server never wedges on a
   vanished viewer. Adversarial review.
3. **[MED]** Option-text-resolution gap (Stage 0) — empty strings unless the emitter resolves.
4. **[MED]** `dsay`/`dend` at the trust boundary — claim-gated + only honored while a dialog
   is active for that claimant + owner-gated; per-session flood cap.
5. **[MED]** `gGameDialogSpeaker` raw `Object*` — a mid-dialog rebaseline re-mints netIds and
   can free it; defer rebaseline (hazard 1) + re-validate netId+id+pid on re-lookup
   (`serverControlAdvancePending` pattern `server_control.cc:281`).
6. **[MED]** gDude singleton / claim binding — on claim release mid-dialog
   (`serverControlBeginDrain` `:342`) bail the conversation (`dialogEnd`); keep the executor
   actor-parameterized (resolves to `gDude` in v1).
7. **[LOW]** Nested modes (barter/party/skilldex-in-dialog): server already handles barter
   inline (`:1937`), aborts party modes; for v1 restrict to plain reply/option nodes, gate any
   option whose `proc` opens barter (barter-streaming = separate track).
8. **[LOW]** Forced-flush frame ordering — confirm `_seq` increments for interstitial frames
   (`presenter_network.cc:190,736`) so client gap detection isn't tripped.

## Critical files
- `src/game_dialog.cc` — server `_gdProcess` barrier `:1892-1928`; node globals; option-text
  gap `:2377`/`:2413-2445`; `_gdialogInitFromScript` `:884`; `_gdialogExitFromScript` `:946`.
- `src/presenter_network.cc` (+ `src/presenter.h`) — new events, forced `flushFrame`.
- `src/client_net.cc` — EVENT enum `:77-101`, dispatch `:515-543`, `viewerServiceTicker` /
  `kViewerModalMask` `:121,:2315`.
- `src/server_control.cc` — route `dsay`/`dend` through `serverControlLine`; `talk` fire `:153`.
- `src/main.cc` — TALK→LOOK fallback `:795-799,:839-841`; dialog modal home; pump-and-bail
  template `viewerPollPendingLoot` `:705-725`.
- `src/server_main.cc` — install `gDialogServerPump` `:145-183`.
- `src/dialog_intent.cc`, `src/command.cc` (`dsay`/`dend`/`dtalk` semantics `:613-658`).
