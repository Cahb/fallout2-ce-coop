# CHAT_INPUT_RECON.md

**VERDICT:** This is a SMALL, well-scoped addition — the server side is genuinely free (EVENT_FLOAT_TEXT
and onFloatText already exist), and even the client-side plumbing has a direct template to copy. Every
existing text-entry widget (`_win_input_str`, `_get_input_str`, `_get_input_str2`, the mapper's file-dialog
entry) is a BLOCKING pump-until-Enter loop written for a screen-owning modal, so none can be reused as-is
over live gameplay — a small (~80-120 line) non-blocking line editor must be written fresh, though
caret/font/measure calls port directly. The hardest part is NOT window teardown: this codebase already has
the exact mechanism needed (a `tickersAdd`-registered ticker, `viewerServiceTicker`, that runs inside every
blocking loop via `inputGetInput → _process_bk → tickersExecute`, plus a `windowDestroy`-triggers-
`windowRefreshAll` guarantee that repaints anything left underneath) — so a persistent top-level chat window
survives every lifecycle event below by construction, as long as chat never lives inside a window someone
else owns. The one open risk worth a design decision is Enter: it is already bound to "attempt end combat"
on the player's own turn, so it cannot be claimed unconditionally.

## Q1 — Existing text-input facilities

Four instances, all blocking modal loops built on top of `inputGetInput()`:

| Facility | Edit-loop function | Draws into | Keys via |
|---|---|---|---|
| Generic label/get-string (`_win_get_str` callers) | `_win_input_str` — `src/window_manager_private.cc:1107` | its own `windowCreate`'d popup (`_win_get_str`, `window_manager_private.cc:551-627`) | `inputGetInput()` each spin |
| Save/load slot description | `_get_input_str2` — `src/loadsave.cc:1743` (called from `lsgSaveGame` region, `loadsave.cc:1729`) | the save/load screen's own window buffer at a fixed x/y | same |
| Character-editor name entry | `characterEditorEditName` → `_get_input_str` — `src/character_editor.cc:1895` (loop body), entry at `character_editor.cc:1908-1966` | a `WINDOW_MODAL` sub-window created in `characterEditorEditName`, `character_editor.cc:3174` | same |
| Mapper/file-dialog filename field | inline loop in the file dialog, `src/dbox.cc:1118-1150ish` | the file-dialog's own window | same |

No interactive debug/console command line exists anywhere in this codebase — nothing sfall-side either.

## Q2 — Blocking vs non-blocking (the crux)

Confirmed: **every one is blocking.** Each is a `for`/`while` loop that calls `inputGetInput()`,
`renderPresent()`/`windowRefresh()`, and `sharedFpsLimiter.throttle()` in a tight spin until `KEY_RETURN`/
`KEY_ESCAPE` (e.g. `window_manager_private.cc:1132-1212`, `character_editor.cc:1925-1979`,
`loadsave.cc:1776-1836`). None of them pump `conn->pump()` or the wire — they were written for vanilla's
single-player, whole-screen-owned modals. **Reusing one directly means the viewer stops applying the
server's wire stream for the whole duration of typing** — unacceptable over live gameplay.

Reusable pieces: caret-blink bookkeeping, `fontGetStringWidth`/`fontDrawText`/`bufferFill` calls, the
backspace/printable-char filtering (`keyCode >= KEY_FIRST_INPUT_CHARACTER && keyCode <= KEY_LAST_INPUT_CHARACTER`,
`kb.h:329-330`), and `beginTextInput()`/`endTextInput()` (`SDL_StartTextInput`/`StopTextInput`, `src/input.cc:926-934`)
for IME. Everything about the *loop shape* (own spin, own render, own exit) must be written fresh: a
`clientChatActive()`/`clientChatHandleKey(int)`/`clientChatDraw()` triplet driven from the existing per-frame
loop in `main.cc`, exactly the shape already used for `client_dialog.{h,cc}` (see viewer-modal-pattern.md).
Estimate: ~40% reuse (font/measure/caret math), ~60% new (the non-blocking state machine + wire-safe integration).

## Q3 — The lifecycle question

| Event | Teardown function | Effect on a foreign top-level window |
|---|---|---|
| Combat enter | `onCombatEnter`, `client_net.cc:1938` → `interfaceBarEndButtonsShow(true)`, `client_net.cc:1976` | Only shows/hides two buttons *inside* `gInterfaceBarWindow`; does not touch `gInterfaceBarWindow` itself or any other window. **Safe.** |
| Combat exit | `onCombatExit`/exit-applied, `client_net.cc:1986-2017` → `interfaceBarEndButtonsHide(true)`, `client_net.cc:2017` | Same — button visibility only. **Safe.** |
| Map transition / rebaseline (`SNAPSHOT_BLOB`) | `onBlobEnd`, `client_net.cc:721`; actual `mapLoad(rd)` call at `client_net.cc:932` | `mapLoad` (`src/map.cc:646`) reloads tiles/objects into the SAME `gIsoWindow`/`gInterfaceBarWindow` buffers (`isoInit`/`interfaceInit` are NOT re-run per map; those only bracket the whole session — `map_render.cc:46` / `map_render.cc:119`, called from `game_lifecycle.cc:212`). An independent chat window is untouched. **Safe**, but a rebaseline mid-modal is *deferred* (`onBlobEnd`, `client_net.cc:721-731`) until the modal closes — chat must not be "the modal" that blocks this. |
| Rebaseline while a modal is open | `viewerServiceTicker`, `client_net.cc:3393-3421`; force-ESC via `enqueueInputEvent(KEY_ESCAPE)` at `client_net.cc:3406` | Force-closes whatever vanilla blocking loop is open (kViewerModalMask, `client_net.cc:164-166`, now includes `kWorldmap`). A standalone chat window is not in this mask and is not closed by it — it would simply keep drawing through the teardown. |
| Screen-owning modal opens (dialog/inventory/pipboy/worldmap/character/barter/save-load) | Each creates its own `windowCreate(... WINDOW_MODAL ...)` (e.g. `pipboy.cc:532`, `character_editor.cc:1354/3174/3306`, `game_dialog.cc:3589/4736`, `loadsave.cc:1268`, `worldmap_ui.cc:539`) | `WINDOW_MODAL` only changes button/menu dispatch order (`window_manager.cc:1218`, `:1265`) — it does **not** hide or destroy other windows. Only **worldmap** additionally calls `interfaceBarHide()` (`worldmap_ui.cc:519`, restored at `:867`) which hides `gInterfaceBarWindow` (and anything drawn *into* it, e.g. the message log, `display_monitor.cc:169-176`). A chat window that is its own top-level `windowCreate` id is unaffected by any of these; a chat surface painted into `gInterfaceBarWindow`'s buffer would vanish for the whole worldmap trip. |
| Movie player (`gameMoviePlay`) | `windowDestroy(win)`, `game_movie.cc:279`, followed immediately by `windowRefreshAll(&_scr_size)`, `game_movie.cc:283` | The movie's OWN modal window is destroyed; `windowDestroy` itself always calls `windowRefreshAll(&rect)` over the destroyed rect (`window_manager.cc:438`), which repaints every remaining window (including chat) that overlaps that rect. **This is the general mechanism** — confirmed by the code comment at `game_movie.cc:281-283`. |

Net: nothing in this list destroys or orphans an independently created window. The one real trap is
**worldmap's `interfaceBarHide()`**, which will hide chat if (and only if) chat is drawn as a sub-surface of
`gInterfaceBarWindow` rather than its own window.

## Q4 — Where should the chat window live

Recommend: **its own top-level window via `windowCreate`, created once and kept alive for the process's
lifetime**, shown/hidden by chat's own logic, never as a child of `gInterfaceBarWindow`. Reasons, from Q3:
`gInterfaceBarWindow` is explicitly hidden during worldmap (`worldmap_ui.cc:519/867`) and is fully destroyed
+ recreated at session bracket (`interfaceFree`/`interfaceInit`, `interface.cc:608-692` / `interface.cc:304`,
bracketed by `isoExit`/`isoInit`, `map_render.cc:105-113`), so anything painted into its buffer inherits both
risks for free. A standalone window is untouched by every case above and self-heals visually via the
`windowDestroy → windowRefreshAll` guarantee (`window_manager.cc:438`). Drawing into an existing surface
(e.g. `gIsoWindow`) was considered and rejected: `gIsoWindow` gets its buffer punched by every tile/object
redraw each frame, so chat content would be erased continuously without its own compositing pass.

## Q5 — Keyboard capture

The single, universal choke point is `inputGetInput()`, `src/input.cc:146-170`. It is called by literally
every loop in the engine — the viewer's own main frame loop (`main.cc:1166`) and every vanilla blocking
modal loop alike — and already special-cases the viewer once (`!gProgramIsActive && !clientViewerActive()`,
`input.cc:155`, to keep pumping while unfocused). To swallow all keys while chat is focused: at the very top
of `main.cc`'s per-frame `keyCode = inputGetInput()` (`main.cc:1166`), branch first on
`clientChatActive()` exactly the way `clientDialogActive()`/`clientDialogHandleKey()` already gate the rest
of the loop (`main.cc:1168-1174`) — if chat is active, route the key to `clientChatHandleKey(keyCode)` and
`continue`/skip the combat/letter/mouse dispatch entirely (do not fall through to the `A`/`N`/`B`/`I`/`S`
letter checks at `main.cc:1198-1283`). Release on Enter (commit + send) or Escape (cancel) inside
`clientChatHandleKey` itself, mirroring `client_dialog`'s own `KEY_ESCAPE` handling.

### Enter/Return during live play (owner's follow-up)

- `KEY_RETURN = '\r'` (`kb.h:37`); the physical Return key AND the numpad Enter both resolve to this same
  code (`kb.cc:947-950`, `kb.cc:1252-1253`) — verified, they are indistinguishable at `inputGetInput()`.
- In the viewer's own live-map dispatch (`main.cc`), `KEY_RETURN` appears **exactly once**, at
  `main.cc:1190`, inside `conn.inCombat() && conn.myTurn() && !combatBusy` → sends `"cendcombat"` (vanilla's
  "attempt to end combat" button, `interface.cc:1955`, code `13`). Outside combat, or in combat but not your
  turn, or while `combatBusy`, Enter is currently a no-op at this chokepoint. **So Enter is live-bound during
  exactly the moment (your own combat turn) a player is most likely to want to type something.**
- Inside the coexisting vanilla modals, Enter/`KEY_RETURN` is *also* meaningfully bound: dialog reply confirm
  (`game_dialog.cc:4363/4567`), inventory quantity/"Done" (`inventory_ui.cc:5167/5436`), the save/load
  description field's own commit (`loadsave.cc:1786`), skilldex confirm (`skilldex.cc:128`), pipboy close
  (`pipboy.cc:434`), character-editor name/age confirm (`character_editor.cc:1933/3399/3631`). These loops are
  vanilla's own, unrelated to whether chat claims Enter in the top-level frame loop, but they rule out "Enter
  always means open chat" as a blanket rule if chat is ever made reachable inside those screens later.
- **Recommendation: do not claim Enter.** It already has a live, frequently-hit meaning (end-combat-on-your-turn).
  Use a different, verified-free key instead. Checked and confirmed **unbound** anywhere in `main.cc`'s
  dispatch chain or in any `gInterfaceBarWindow` button's event code (`interface.cc`, which reserves
  `I/O/S/Tab/P/C/B/Space/Return` for its buttons, plus `A`/`N` claimed directly in `main.cc`): **`T`**
  (`KEY_LOWERCASE_T`/`KEY_UPPERCASE_T`, no hits in `main.cc` or `interface.cc`) is clean and matches the
  Minecraft/Quake convention closely enough; `Y`, backtick (`KEY_GRAVE`), and the F-keys are also free but
  less conventional. Avoid `Tab` even though it's unbound *today* — it is already reserved as the worldmap
  button's mouse-up code (`interface.cc:433`) and will collide once that button is wired to a physical key.

## Q6 — Precedent for a persistent non-modal overlay

**No true precedent exists.** The closest analog, the message log (`displayMonitorInit`,
`display_monitor.cc:144`), is NOT an independent window — it is a fixed-offset region painted directly into
`gInterfaceBarWindow`'s own buffer (`display_monitor.cc:169-176`), so it inherits the interface bar's
worldmap-hide (`worldmap_ui.cc:519`) exactly like the risk flagged in Q3/Q4. The one genuinely reusable
*mechanism* (not a rendering precedent, but the right template) is `viewerServiceTicker`
(`client_net.cc:3393-3421`), registered once via `tickersAdd` (`client_net.cc:3424-3426`,
`clientViewerInstallServiceTicker`) and executed on every single `inputGetInput()` call via
`_process_bk → tickersExecute` (`input.cc:180-198`) — i.e., it already runs *inside every vanilla blocking
modal loop in the game*, gated by `kViewerModalMask` (`client_net.cc:164-166`). This is the copyable pattern
for "stay alive and correct no matter whose loop is currently spinning."

## RECOMMENDED SHAPE

1. Own top-level window, created once near `clientViewerInstallServiceTicker()` (`main.cc:1104`) and never
   destroyed for the process lifetime; hidden (`windowHide`) rather than destroyed when not focused/typing.
2. New module `client_chat.{h,cc}` mirroring `client_dialog.{h,cc}`: `clientChatActive()`,
   `clientChatHandleKey(int)`, `clientChatOpen()/Close()`, internal caret/backspace/printable-char handling
   ported from `_win_input_str` (`window_manager_private.cc:1107`).
3. Bind open with `T` (not Enter — see Q5), close/cancel on `KEY_ESCAPE`, send+close on `KEY_RETURN` *only
   while `clientChatActive()`* so it never collides with the combat `cendcombat` binding at `main.cc:1190`.
4. Gate in `main.cc` exactly like dialog: check `clientChatActive()` before the `A/N/B/I/S`/mouse dispatch
   block (`main.cc:1174` onward) and `continue` past it when active, so typing "w" can't also walk.
5. Wire verb: reuse the dialog/barter intent-queue pattern (`dialog_intent.{h,cc}`) for a `chat_intent`
   carrying the typed line; server routes it through the existing `EVENT_FLOAT_TEXT`
   (`presenter_network.cc:85/843`, `client_net.cc:2679 onFloatText`) plus the console/message-log path,
   confirmed already present — no new server rendering work needed.
6. Do NOT add chat's `GameMode` (if any) to `kViewerModalMask` (`client_net.cc:164-166`) — chat must not
   force-defer rebaselines or get force-ESC'd by `viewerServiceTicker`; it should coexist with live wire
   traffic, which is the entire point.

## TRAPS

- Worldmap hides `gInterfaceBarWindow` (`worldmap_ui.cc:519`, restored `:867`) — anything painted into that
  window's buffer (like the message log) disappears for the whole worldmap trip. **Do not paint chat there.**
- `KEY_RETURN` is live-bound to `cendcombat` on the player's own turn (`main.cc:1190`) — claiming Enter
  globally for "open chat" will race real combat input.
- `KEY_TAB` is reserved as the (currently physically-unwired) worldmap interface-bar button's mouse-up code
  (`interface.cc:433`) — using it for chat now risks a future collision when that button gets a physical key.
- A rebaseline mid-modal is deferred, not dropped (`onBlobEnd`, `client_net.cc:721-731`, drained at
  `main.cc:1457-1459`) — if chat is ever folded into `kViewerModalMask` by mistake, it will start deferring
  world updates for as long as the chat box is open, which defeats the entire "stay live" requirement.
- `interfaceFree()`/`interfaceInit()` (`interface.cc:608`/`304`) fully destroy/recreate `gInterfaceBarWindow`
  at session bracket (`isoExit`/`isoInit`, `map_render.cc:105-113` — UNVERIFIED exact call sites for when
  isoExit/isoInit re-run mid-session, e.g. main-menu round-trip; only confirmed they do NOT run per ordinary
  map transition/rebaseline). If chat is ever anchored as a child of the interface bar despite the
  recommendation above, this is the one path that would truly destroy it, not just hide it.
- Pipboy/character-editor/barter/in-session save-load are not yet reachable from the viewer's `main.cc`
  (only inventory/skilldex/dialog/loot/use-item-on/worldmap are wired in — UNVERIFIED whether any other entry
  path reaches them today). Their teardown behavior above is verified from vanilla source but not verified as
  currently exercised in the viewer.
