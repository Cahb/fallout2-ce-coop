# Dialog + Trade — the complete nuance map (vanilla mechanism → co-op form)

Status: **reference spec** (2026-07-21). Written to END the whack-a-mole on the dialog/barter
viewer by enumerating *every* vanilla nuance from source and mapping each to our server-authoritative /
thin-viewer model. A nuance missing here is tomorrow's mole.

Model recap (established, not relitigated here):
- The **server** runs the real dialog/barter engine headless (`serverLoopActive()` branch), **parks
  its tick** inside the modal (block-and-pump barrier) and **streams state events**. Parking is
  required and permanent — dialog is a synchronous C call stack in the interpreter; barter nests
  inside it.
- Every **viewer** renders the *same* server-owned state by decoding events into local mirror objects
  and driving the vanilla renderer directly (bypassing the loops that would execute authority
  locally). The two instances are `client_dialog.cc` and `client_barter.{h,cc}` +
  `inventoryOpenTradeViewer` (`inventory_ui.cc`).
- **Whoever STARTS a conversation DRIVES it** (`server_control.cc` `serverControlMayDriveDialog`);
  everyone else is a read-only spectator. The trade opened from it inherits the same driver.
- **Drag/drop is DRIVER-ONLY** and must reroute to `b*` verbs; spectators get an instant "slap" from a
  full state snapshot after every accepted move. Money/valuations are **server-computed and streamed**,
  never client-derived.

Files this spec is drawn from (all `src/`): `game_dialog.cc`, `inventory_ui.cc`, `item.cc`,
`client_dialog.cc`, `client_barter.{h,cc}`, `server_control.cc`, `presenter.h`,
`presenter_network.cc`, `client_net.cc`, `main.cc`. Prior design: `DIALOG_STREAMING_PLAN.md`,
`BARTER_STREAMING_PLAN.md`, `drafts/ARCHITECTURE_FLOW.md`.

---

# PART A — how vanilla actually works

## A0. The window/object inventory (name every surface once)

There are **four windows** and **three-plus sim objects** in play. Getting their owners and order
right is the whole game.

| # | Surface | Global | Created by | FRM |
|---|---|---|---|---|
| W1 | Dialog **background** (full 640×480; head panel top, world/head render) | `gGameDialogBackgroundWindow` (`:248`) | `talk_to_create_background_window()` inside `_gdCreateHeadWindow` (`:2810`) | — |
| W2 | Dialog **subwindow** (lower strip; holds the red Barter/Review/Combat-control buttons) — **reused as the barter background** | `gGameDialogWindow` (same handle!) | `_gdialog_window_create` (`:4830`) for dialog; `_gdialog_barter_create_win` (`:3650`) for barter | di_talk.frm 99 / di_talkp.frm 389 (dialog); barter.frm 111 / trade.frm 420 (barter) |
| W3 | **Reply** text subwindow | `gGameDialogReplyWindow` (`:1808`) | `_gdProcessInit` (`:1792`) | — |
| W4 | **Options** subwindow | `gGameDialogOptionsWindow` (`:1838`) | `_gdProcessInit` | — |
| W5 | **Trade item-list** window (the two 3-slot columns + inner tables) | `gInventoryWindow` | `_setup_inventory(INVENTORY_WINDOW_TYPE_TRADE)` (`inventory_ui.cc:807`) inside `inventoryOpenTrade` | frmId reads `_barter_back_win` (=W2) |
| O1 | player offer table | `_peon_table_obj` | `_gdialog_barter_create_win` | pid −1, `OBJECT_HIDDEN` |
| O2 | merchant offer table | `_barterer_table_obj` | `_gdialog_barter_create_win` | pid −1, `OBJECT_HIDDEN` |
| O3 | merchant body-art proxy | `_barterer_temp_obj` | `_gdialog_barter_create_win` | merchant fid, `OBJECT_HIDDEN\|OBJECT_NO_SAVE` |
| O4 | hidden merge box | `hiddenBox` (local) | `inventoryOpenTrade` (`:4617`) | `PROTO_ID_JESSE_CONTAINER` |

Note the load-bearing reuse: **W2 IS the barter background**. `_gdialog_barter_create_win` builds
barter.frm into the *same* `gGameDialogWindow` handle that held di_talk.frm; `_setup_inventory(TRADE)`
then blits W5's backing from `_barter_back_win` (= W2, `inventory_ui.cc:822-823`) and `_display_body`
patches from barter.frm again (`inventory_ui.cc:2013-2021`). W1 (background/head) stays up the whole
time under everything.

## A1. The state-variable dictionary + full transition table

Four file-static ints in `game_dialog.cc` drive everything. **This is where the bugs live.**

### `_dialog_state_fix` (`:155`) — "entered via TALK", i.e. `_gdialogActive()`
- `_gdialogActive()` returns `_dialog_state_fix != 0` (`:694`).
- Set `1` **only** by `gameDialogEnter` (`:760`); cleared `0` on every `gameDialogEnter` early-out and
  at the normal end (`:781,:789,:830`).
- ►► **TRAP (already bitten, documented `:1985-1999`):** a *script-driven* conversation
  (`gsay_start`/`gsay_end` → `_gdialogGo` → `_gdProcess`) never calls `gameDialogEnter`, so
  `_dialog_state_fix` stays `0` through a real conversation (denbus2/Sheila's post-teleport path). It is
  the **wrong** question for "is a dialog live". `gameDialogBarter` (`:3626`) gates the *scripted*
  barter opcode on it and returns `-1` when `0` — so a script `gdialog_barter` during such a
  conversation is a **silent no-op in vanilla** (vanilla read the keyboard directly so nobody noticed).

### `_gdialog_state` (`:220`, init `-1`) — "the from-script engine is live"
- `-1` never-initialized; `1` active; `0` torn down.
- Set `1` in `_gdialogInitFromScript` (`:941` headless, `:979` interactive); set `0` in
  `_gdialogExitFromScript` (`:1011,:1054`) and `gameDialogEnter` (`:815`).
- The latch `gsay_start`/`gsay_end` check.

### `_dialogue_state` (`:214`, init `0`) — the conversation's CURRENT screen
| val | meaning | set at |
|---|---|---|
| 0 | inactive / reset | `gameDialogEnter :816`, `_gdialogExitFromScript :1055`, `_gdCreateHeadWindow` guards |
| 1 | **normal dialog** (reply/options) | `_gdCreateHeadWindow :2798`, `_gdProcessUpdate` end, `_barter_end_to_talk_to :3645`, server barter-return `:2128` |
| 4 | **barter** | `gameDialogBarter :3632`, `gameDialogBarterButtonUpMouseUp :4808`, `_gdialog_barter_create_win :3652`, interactive mode-3 `:2188` |
| 10 | party control | `_gdProcess :2205` |
| 13 | party customization | `_gdProcess :2209` |
- Read by `gameDialogEnter` (`:785` — bails unless `==4`, i.e. only re-enters mid-barter), by
  `_gdDestroyHeadWindow` (`:2852` `==1` → destroy dialog win, `:2854` `==4` → destroy barter win), and
  as the resume flag in the interactive mode-3 branch (`:2195-2202`).

### `_dialogue_switch_mode` (`:217`, init `0`) — the PENDING mode transition (the "mode machine")
This is the state the `gameDialogTicker` switch (`:3260`) and the `_gdProcess` inner branches read.

| val | meaning | who SETS it | who READS/consumes it |
|---|---|---|---|
| 0 | no transition pending / plain dialog | ticker case 1 (`:3272`), server barter-return (`:2127`), server nested-abort (`:2154`), viewer inert-forcing (`:3257`) | everywhere as "nothing pending" |
| 1 | **pending return to dialog** (from barter/party) | `_barter_end_to_talk_to :3646`, interactive mode-3 resume `:2200` | ticker case 1 (`:3270`): destroy barter win, recreate dialog win, `gdUnhide`, re-render caps |
| 2 | **pending ENTER barter** | `gameDialogBarterButtonUpMouseUp :4807` (button), `gameDialogBarter :3633` (script opcode) | ticker case 2 (`:3261`): `enterGameMode(kSpecial)`, destroy dialog win, create barter win, → mode 3. **Server:** `_gdProcess :2102` drives create/trade/destroy inline (bypasses ticker) |
| 3 | **in barter** | ticker case 2 (`:3263`) | interactive `_gdProcess :2187`: `exitGameMode(kSpecial)`, `inventoryOpenTrade`, cleanup, destroy |
| 8 | pending enter party control | `gameDialogCombatControlButtonOnMouseUp` | ticker case 8 (`:3286`) → 9 |
| 9 | in party control | ticker (`:3288`) | interactive `_gdProcess :2204` |
| 11 | pending enter party customization | party-custom button | ticker case 11 (`:3292`) → 12 |
| 12 | in party customization | ticker (`:3294`) | interactive `_gdProcess :2209` |

**The two-step interactive dance for barter** (the source of the "one is a ghost" confusion):
`mode 2` is set synchronously (button/opcode) → **`gameDialogTicker` case 2** (a client tick callback)
does `enterGameMode(kSpecial)` + `_gdialog_window_destroy` + `_gdialog_barter_create_win` and bumps to
`mode 3` → **`_gdProcess`'s mode-3 branch** (`:2187`) does `exitGameMode(kSpecial)` +
`inventoryOpenTrade` + cleanup + destroy. **Two owners, split across the ticker and the process loop.**
The server collapses both into one inline block (`:2102-2143`) and never runs the ticker for it.

**`_dialogue_switch_mode` teardown interactions (subtle, cited):**
- `_gdialogExitFromScript` **early-returns `-1`** if mode is `2/8/11` (`:1000`) — a teardown while a
  transition is pending is refused. This is *why* the server code religiously normalizes
  `_dialogue_switch_mode = 0` before ending (`:2127`, `:2154`): otherwise the teardown short-circuits,
  the ticker stays registered, and it tries to build a party/barter SDL window on the next tick.
- `gameDialogEnter` re-entry choreography (`:795-813`) branches on all of `mode 2/8/11`, and on the
  equalities `_dialogue_switch_mode==_gdialog_state`, `_dialogue_state==_gdialog_state`,
  `_dialogue_state==a2` to pick between `_gdialog_window_destroy` and `_gdialog_barter_destroy_win`.

### Server-only: `gDialogServerNodeDepth`
`_gdProcess` increments (`:2002`) / decrements (`:2302`) a **depth counter** that is the *real*
dsay/dend ground truth — deliberately **not** `_dialog_state_fix` (`:1988` explains why: scripted
conversations leave that `0`). Depth-counted because conversations nest (`_gdReenterLevel`).

## A2. Entering barter — the OPTION opcode vs the on-screen BUTTON

There are **exactly two** entry points, and both funnel through `_dialogue_switch_mode = 2`.

1. **Script dialog option** → `gdialog_barter(modifier)` opcode → `gameDialogBarter` (`:3624`):
   guards `_dialog_state_fix` (returns `-1` if not entered via TALK — see A1 trap), sets the modifier,
   **calls `gameDialogBarterButtonUpMouseUp(-1,-1)`**, then sets `_dialogue_state=4`,
   `_dialogue_switch_mode=2`.
2. **On-screen Barter red button** (W2, created at `_gdialog_window_create :4873`, callback bound
   `:4875`) → `gameDialogBarterButtonUpMouseUp` (`:4788`+). This checks the merchant's proto
   `CRITTER_BARTER` flag (`:4800`); if set → `_dialogue_switch_mode=2; _dialogue_state=4; gdHide()`
   (`:4807-4811`). If NOT set → prints "This person will not barter with you" (msg 903, or 913 for a
   party member who "can't carry anything") (`:4813-4825`) and does nothing.

So the option opcode is a **superset**: it *calls the button handler* and then also forces the state.
They are **not** two different mechanisms — they converge on `mode 2`, and from there the identical
ticker/process path runs. The team's belief that "one is a local ghost" is about the **viewer**, not
vanilla:

►► **On a connected viewer the entire mode machine is INERT** (`gameDialogTicker :3252-3258`, gated on
`clientViewerActive()`). The live Barter button in W2 still exists and still sets
`_dialogue_switch_mode=2` when clicked — but the ticker now **forces it back to 0 and logs loudly**
instead of building anything. Before that guard, clicking the button built a **real but empty local
barter window**, entered `kSpecial` and never left, then handed to mode 3 whose handler lives in
`_gdProcess` — which never runs on a viewer. That stranded window **was** the "ghost trade screen", and
the server never heard a thing (commit `5390f87`). The button is thus a dead control on the viewer; the
real barter entry on a viewer is **`EVENT_BARTER_BEGIN` from the server**, opened from the main loop
(`main.cc:1142`), not from any local click.

## A3. Leaving barter — four exits, four different behaviors

The exits differ in (a) whether tables are **swept back** to owners, (b) window teardown, and (c) what
happens to the **conversation**. Table sweep matters because `_gdialog_barter_destroy_win` calls
`objectDestroy` on both tables **with their contents** (`:3801-3803` / headless `:3781-3783`).

| Exit | Trigger | Tables | Conversation | Code |
|---|---|---|---|---|
| **T / DONE / Talk** | `KEY_LOWERCASE_T` or reaction `<= -30` (interactive `:4853`); `BARTER_INTENT_DONE` (server `:4749`) | **swept back** (`itemMoveAll` table→owner, both sides) then `_barter_end_to_talk_to` | **RESUMES** the dialog (that is what "Talk" means) | interactive `:4853-4857`; server `:4749-4757` |
| **ESC / CANCEL** | `KEY_ESCAPE` (interactive `:4838`); `BARTER_INTENT_CANCEL` (server `:4759`) | **NOT swept** — teardown restores merchant's worn/held items + hidden box only; anything still on a table is **destroyed with it** | ends (interactive falls through) | interactive `:4838-4840`; server `:4759-4764` |
| **Empty queue** | headless golden, no pump | not swept | ends | server `:4728-4746` |
| **Pump bail** | driver disconnect / combat / quit | **swept back like DONE** (`:4738-4739`) — a dropped connection must not destroy the player's goods | ends | server `:4728-4741` |

►► **THE `break` vs `continue` BUG (commit `5390f87`, already fixed).** The interactive barter branch
in `_gdProcess` ends its mode-3 handling with **`continue`** (`:2203`) — leaving a trade returns you to
the conversation; `dialogEmitNode` at the loop top then re-emits the node and repopulates every
viewer's option list. The **server** DONE branch used to end in `break`, which hung up on the merchant
the instant a trade closed and left the viewer holding an empty dialog window no node would ever fill.
It is now **`continue`** (`:2130-2142`), matching the interactive path. This is the "half-baked dialog
menu, no options after leaving a trade" symptom — it was downstream of the wrong `break`, not a viewer
bug.

Also note the server's post-barter **state normalization** (`:2127-2128`): after `inventoryOpenTrade`
returns, force `_dialogue_switch_mode=0` (so `_gdialogExitFromScript` won't short-circuit — A1) and
`_dialogue_state=1` (so a leftover `==4` can't misroute the head-window teardown into the barter path).

## A4. Window lifecycle across dialog → barter → dialog (exact order)

**Dialog open** (`_gdialogInitFromScript :912`): builds W1 (background/head via `_gdCreateHeadWindow` →
`talk_to_create_background_window`), which *also* builds W2 (`_gdialog_window_create` at `:2832`), then
registers `gameDialogTicker`, sets the fidget/head, enters `GameMode::kDialog`. Reply/options W3/W4 are
built separately by `_gdProcessInit` (`:1792`, the top of `_gdProcess`).

**Enter barter** (ticker case 2, interactive `:3261`): `enterGameMode(kSpecial)` →
`_gdialog_window_destroy` (**W2 down**) → `_gdialog_barter_create_win` (**W2 handle reused for
barter.frm**, plus the three table objects O1-O3). Then `_gdProcess` mode-3 (`:2187`):
`exitGameMode(kSpecial)` → `inventoryOpenTrade` (builds **W5** via `_setup_inventory(TRADE)`, plus O4)
→ `_gdialog_barter_cleanup_tables` → `_gdialog_barter_destroy_win` (**W5 gone with inventory teardown,
O1-O3 destroyed**).

►► **W3/W4 (reply/options) are NOT torn down when barter opens** — in vanilla the barter option's proc
**registers no new options** (barter REPLACES them), so by the time barter draws there is nothing left
in them. On the **viewer** they must be torn down explicitly, because the viewer built them itself and
nothing was removing them → they stayed lit under the trade screen ("dialog leftovers blended with the
new background", commit `e55ff06`). Handled: `inventoryOpenTradeViewer` calls
`gameDialogExitNodeWindows()` then `gameDialogInitBarterWindows()` (`inventory_ui.cc:5169-5170`), and
on exit rebuilds them **only if the conversation is still running** (`:5280-5281`) — rebuilding for an
already-ended conversation created orphans that rendered over the world forever.

**Leave barter → dialog** (ticker case 1, `:3270`): `_gdialog_barter_destroy_win` (W2/barter down) →
`_gdialog_window_create` (**W2 back to di_talk.frm**) → `gdUnhide` → re-render caps.

**Dialog end** (`_gdialogExitFromScript :998` → `_gdDestroyHeadWindow :2846`): destroys W2 (via
`_dialogue_state==1` → `_gdialog_window_destroy`, or `==4` → `_gdialog_barter_destroy_win`), then W1.

### The unloaded-FRM-in-the-top-left candidate (found the blit; needs live confirm)

The leading concrete candidate is the **player body-art blit in the barter window**, fed by a **stale
`gInventoryWindowDudeFid`**:
- `_display_body` (TRADE branch) draws two bodies into `_barter_back_win` (=W2): index 0 = the player
  at `rect.left=15, top=25`; index 1 = the merchant at `560,25` (`inventory_ui.cc:2001-2025`). Index 0's
  fid is `gInventoryWindowDudeFid` (`:1962`).
- `gInventoryWindowDudeFid` is computed by `_adjust_fid` (`:2557`, sets `:2604`), which reads
  `_inven_dude`. `_adjust_fid` runs **inside `inventoryCommonInit`** (`:2488`).
- In `inventoryOpenTradeViewer`, `inventoryCommonInit()` is called at `:5151` **before** `_inven_dude`
  is reassigned to the driver at `:5187`. So `gInventoryWindowDudeFid` is derived from the **previous**
  `_inven_dude` (stale, possibly a freed pointer from an earlier modal), producing a wrong or invalid
  fid for the top-left body slot.
- `_display_body` null-guards `artLock` (`:1974` `if (art == nullptr) continue;`), so a truly-unloaded
  fid draws *nothing* rather than garbage — meaning the visible artifact is most likely a **wrong body
  sprite** (last-used critter) rather than a placeholder. Either way the fix is the same: recompute the
  dude fid **after** `_inven_dude` is set to the driver (call `_adjust_fid()` post-`:5187`, or set
  `gInventoryWindowDudeFid` from the driver explicitly).

Secondary candidate to rule out live: **W1 (`gGameDialogBackgroundWindow`) head panel** persists under
barter; if `gGameDialogHeadFid` were unset the top head panel would render the world/garbage — but the
viewer sets `gGameDialogHeadFid` off the wire before dialog opens (`client_dialog.cc:45`), so this is
unlikely and only relevant if barter opens as the FIRST node. Confirm by observing whether the artifact
is in the head panel (top, W1) or the player-body slot (left of the lower barter strip, W2).

## A5. The drag/drop slot handlers — exactly what each mutates

The interactive trade loop dispatches invisible slot buttons by keycode range
(`inventory_ui.cc:4946-5013`), gated on `gInventoryCursor == HAND` (arrow cursor opens the context menu
instead). The four ranges and what each moves:

| Keycode range | Slot | Handler | Moves | b* verb equivalent |
|---|---|---|---|---|
| 1000–1000+N | player pack (left column) | `_barter_move_inventory(..., fromDude=true)` (`:4956`) | dude → playerTable (O1) | `boffer <pid> [qty]` |
| 2000–2000+N | merchant stock (right column) | `_barter_move_inventory(..., fromDude=false)` (`:4973`) | merchant → merchantTable (O2) | `btake <pid> [qty]` |
| 2300–2300+N | player offer inner table | `_barter_move_from_table_inventory(..., fromDude=true)` (`:4989`) | playerTable (O1) → dude | `bunoffer <pid> [qty]` |
| 2400–2400+N | merchant offer inner table | `_barter_move_from_table_inventory(..., fromDude=false)` (`:5005`) | merchantTable (O2) → merchant | `bunoffer <pid> [qty]` |

`_barter_move_inventory` (`:4231`): animates the drag cursor (`mouseSetFrame` + `ipickup1`/`iputdown`
sfx, `:4266-4282`), on a stack `>1` opens **`inventoryQuantitySelect`** (`:4288`), then
`itemMoveForce`. `_barter_move_from_table_inventory` (`:4318`) is the reverse. The **Offer button (M)**
→ `barterAttemptTransaction` (`:4860`); **Talk (T)** → sweep + `_barter_end_to_talk_to` (`:4853`).

The **server drain** (`inventoryOpenTrade` serverLoopActive, `:4785-4833`) reproduces exactly these
four moves via `_barter_find_item_by_pid` + `itemMoveForce` — **by pid, not by slot index** (the slot
math reverse-indexes live window rows, which don't exist headless, `:4501`). `BARTER_INTENT_UNOFFER`
searches playerTable first, then merchantTable (`:4801-4811`). Quantity: `<=0` or `> available` clamps
to the whole stack (`:4816-4819`).

## A6. Everything read for DISPLAY (per-actor vs shared)

| Datum | Source | Per-actor or shared? | Notes |
|---|---|---|---|
| item lists (4: driver pack, merchant stock, playerTable, merchantTable) | `barterSnapshotTable` → `EVENT_BARTER_STATE` | **shared** (the trade's) — spectator renders the DRIVER's, not their own | all four snapshotted because the tick is parked and mirrors are frozen (`presenter.h:421-428`, `inventory_ui.cc:4557-4567`) |
| **offer value** ("$X" under player table) | `objectGetCost(playerTable)` (`:4573`) | shared | raw worth, no barter modifier |
| **asking value** ("$X" under merchant table) | `barterComputeValue` (`item.cc:3774`) | shared, **server-only-derivable** | see below |
| merchant body art | `_display_body(merchant->fid)` | shared | index 1, right slot |
| **player body art** | `gInventoryWindowDudeFid` → `_display_body` index 0 | **per-viewer today (bug)** — should be the DRIVER's | A4 stale-fid candidate |
| weight / encumbrance / carry cap | `critterGetStat(dude, STAT_CARRY_WEIGHT)`, `objectGetInventoryWeight` (in `barterAttemptTransaction` `item.cc:3822`) | driver's — computed **server-side** at commit | never shown as a live counter in NPC barter; only gates the transaction |
| party-member weight labels (msg 30) | `objectGetInventoryWeight` (`:4435,4474`) | shared | party-member trade only (A7) |
| reaction / fidget | `gGameDialogFidget` streamed in `EVENT_DIALOG_NODE` (`:1968`) | shared | drives the head mood, not barter directly |

►► **Why the two valuations MUST be server-computed** (`item.cc:3774-3812`, `presenter.h:415-420`):
`barterComputeValue` reads (1) `partyGetBestSkillValue(SKILL_BARTER)` — the **party's best** barter
skill, not the driver's own; (2) `perkHasRank(gDude, PERK_MASTER_TRADER)` gated on `dude == gDude` —
a **spectator would evaluate this for themselves**; (3) `barterMod` carrying the **script-mutated
reaction** (dialog scripts change it mid-conversation via `gameDialogSetBarterModifier`). A viewer
cannot reproduce any of the three. The reaction also feeds a hidden `barterReactionModifier(barterer)`
added to `barterMod` at open (`inventory_ui.cc:4661`, `:4710`).

There is **no persistent "player caps" counter** in the NPC barter window (unlike the dialog window,
which renders caps via `gameDialogRenderCaps`). Caps are just items on the tables; their value rolls
into the offer/asking `$` totals via `itemGetTotalCaps` (`item.cc:3781`).

## A7. Party-member barter (`gGameDialogSpeakerIsPartyMember`)

A companion can be the "merchant" (trade.frm 420 not barter.frm 111). Differences, all keyed on
`gGameDialogSpeakerIsPartyMember` (`:762`, `objectIsPartyMember`):
- **Valuation is by WEIGHT, not caps**: `barterComputeValue` returns `objectGetInventoryWeight(table)`
  outright (`item.cc:3776-3778`); the inner-table labels show "weight N" (msg 30) instead of "$N"
  (`inventory_ui.cc:4430-4437,4469-4476`).
- **Transaction gates on the companion's carry weight**, not a good-offer check
  (`item.cc:3827-3831`, `BARTER_RESULT_NPC_TOO_HEAVY`).
- The merchant's **held weapon is NOT stripped** for a party member (`inventory_ui.cc:4608`); the
  "can't barter" refusal message differs (913 vs 903, `:4816-4818`).
- Combat-control button exists only for party members (`_gdialog_window_create :4890-4903`).

For co-op this matters the day a *player-controlled companion* or a second player's follower is traded
with; the streamed `EVENT_BARTER_STATE` already carries `speakerIsPartyMember` implicitly (the server
computes both valuations correctly under it), so the viewer needs no branch — but the viewer's inner
labels ($ vs weight) read `gGameDialogSpeakerIsPartyMember` locally and it must be seeded correctly on
the viewer (currently derived from the mirror; see PART B).

## A8. Time/tick-driven things inside these loops

| Thing | Where | Server (parked) | Viewer (rendering) |
|---|---|---|---|
| **Fidget** (idle head animation) | `gameDialogTicker :3300-3341` | never runs (no head, `gGameDialogFidgetFrm==nullptr`) | must run — driven by the ticker below the mode switch, which the viewer KEEPS (`:3247-3248`) |
| **Lips / lip-sync** | `gameDialogTicker :3304-3320`, `gameDialogStartLips` | never runs headless | viewer starts lips from `audioFileName` on the wire (`client_dialog.cc:75-77`) |
| **Head reaction transitions** | `_talk_to_critter_reacts :3349` | no-op headless | viewer drives from streamed reaction |
| **Reply paging** (long text auto-advance) | `_gdProcess :2221-2260` (interactive only) | n/a | viewer render only |
| **Drag cursor sfx** `ipickup1`/`iputdown` | `_barter_move_inventory :4267,4281` | n/a (server uses `itemMoveForce`) | driver-only, once drag is rerouted |
| **`aiAttemptWeaponReload`** on barter teardown | `_gdialog_barter_destroy_win :3793,:3832` | **runs headless** — real sim mutation (merchant tops up its weapon from loose ammo, may destroy the emptied ammo); reload sfx self-neutralizes on null presenter | runs on viewer harmlessly |
| **Fidget/lips SFALL music volume** | `_gdialogInitFromScript :982-988` | skipped headless | viewer only |

►► The parked server streams **nothing** except what the modal explicitly emits (`objectDeltaScan`
does not run — `ARCHITECTURE_FLOW.md §5`). Everything time-based above is **viewer-local presentation**;
the only tick-driven **sim** mutation inside these loops is `aiAttemptWeaponReload`, which is correctly
kept on the headless path.

---

# PART B — the co-op form of each mechanism

Format: **[nuance] → behavior in our model → seam** (done @commit, or OPEN with the shape).

## B1. State machine (A1)
- **`_dialog_state_fix` is the wrong dsay/dend gate.** → Server gates on the **depth counter**
  `gDialogServerNodeDepth` (`game_dialog.cc:2002/2302`), raised for exactly as long as a node can
  consume an intent. **DONE.** Viewer gates its own key handling on `gClientDialogWaitingForResponse` +
  ownership (`client_dialog.cc:127-143`).
- **The mode machine has three owners on a viewer.** → On a connected viewer the mode switch is
  **forced inert and logged** (`gameDialogTicker :3252-3258`, gated `clientViewerActive()`), so only the
  server drives mode transitions and the viewer only renders. **DONE @`5390f87`.** The fidget/lips tail
  below the switch is deliberately kept.
- **`_dialogue_switch_mode`/`_dialogue_state` normalization around barter.** → Server forces
  `mode=0, state=1` after `inventoryOpenTrade` returns (`:2127-2128`) so teardown doesn't
  short-circuit. **DONE.**

## B2. Entering barter (A2)
- **Two entries converge on `mode 2`.** → Server: the option's proc sets `mode 2`; `_gdProcess` detects
  it **before** the `choiceResult==-1` end-check (`:2102`, ordering is load-bearing — barter's proc
  registers no options so it returns −1 even though barter must still run) and drives
  create→trade→destroy inline. **DONE.**
- **The on-screen Barter button on a viewer.** → Dead control: it still sets `mode 2` on click but the
  inert ticker forces it back to 0 and logs. The **only** real barter entry on a viewer is
  `EVENT_BARTER_BEGIN`, opened from `main.cc:1142`. **DONE @`5390f87`.** (Future: gray the button out on
  the viewer so it doesn't even log — cosmetic.)
- **Scripted `gdialog_barter` during a `gsay` conversation is a vanilla no-op** (`_dialog_state_fix==0`,
  A1 trap). → **OPEN / watch:** if a merchant's script uses the opcode form on its *second* conversation
  (denbus2/Sheila shape), `gameDialogBarter` returns −1 and barter never opens — server-side, silently.
  Shape of the fix: gate the opcode on `gameDialogServerNodeActive()` (the depth counter) instead of
  `_dialog_state_fix` when `serverLoopActive()`, mirroring the dsay/dend gate rationale. Verify against a
  merchant that barters via the opcode, not the button.

## B3. Leaving barter (A3)
- **T/DONE resumes the conversation, ESC/CANCEL ends it, bail sweeps like DONE.** → Server DONE sweeps +
  `_barter_end_to_talk_to` + `continue` (`:2749-2757` / `:2130-2142`); CANCEL no sweep + break
  (`:4759-4764`); pump bail sweeps like DONE (`:4728-4741`). **DONE @`c766a34` (bail-sweep) + `5390f87`
  (continue).**
- **Viewer ESC is a request, never local.** → Driver ESC sends `bdone`; spectator ESC is swallowed
  (`inventory_ui.cc:5251-5259`). Window closes only on `EVENT_BARTER_END` (`:5246`). **DONE @`7021697`.**
- **ESC/CANCEL destroys goods left on the table** (vanilla hole, A3). → **OPEN (owner call):** our
  `bcancel` verb inherits vanilla's behavior — items sitting on a table when a driver hits ESC are
  destroyed with the table. Since our ESC maps to `bdone` (sweep) not `bcancel` (`:5257`), a normal
  driver never hits it; `bcancel` is debug-only today. Leave as-is unless a UI ever wires a true Cancel.

## B4. Window lifecycle (A4)
- **W2 must be built as the barter background before the trade window.** →
  `gameDialogInitBarterWindows()` before `_setup_inventory` (`inventory_ui.cc:5170`). **DONE @`e55ff06`.**
- **W3/W4 (reply/options) must be torn down entering barter, rebuilt on exit only if the conversation
  survives.** → `gameDialogExitNodeWindows()` in, conditional `gameDialogInitNodeWindows()` out gated on
  `clientDialogActive()` (`:5169,:5280-5281`). **DONE @`e55ff06`.**
- **Mirror tables need a real proto** (subject-slot promotion via `_container_enter`). →
  `barterCreateTable` uses `PROTO_ID_JESSE_CONTAINER`, not pid −1 (`client_barter.cc:38-46`). **DONE
  @`5390f87`.**
- **`_inven_dude`/`_target_stack[0]` must be REAL critters; only the LIST comes from the snapshot.** →
  subject = `clientBarterDriver()`/`merchant` (real), list `_pud`/`_target_pud` = snapshot mirrors,
  re-seeded **after** `_setup_inventory` with a loud check that it took (`:5187-5220`). **DONE @`e55ff06`
  (segfault fix).**
- **Unloaded/wrong FRM top-left (A4).** → **OPEN.** Shape: `gInventoryWindowDudeFid` is computed from a
  stale `_inven_dude` because `inventoryCommonInit`→`_adjust_fid` runs before the driver is assigned.
  Fix: after `_inven_dude = clientBarterDriver()` (`:5187`), call `_adjust_fid()` (or set
  `gInventoryWindowDudeFid` from the driver's body fid) so the player-body slot draws the driver.
  Live-confirm whether the artifact is the player-body slot (this) or the W1 head panel.

## B5. Drag/drop (A5)
- **Drag/drop is driver-only, rerouted to `b*` verbs; spectators get a snapshot slap.** →
  **OPEN — the one remaining barter feature.** Today the viewer trade screen is **read-only for
  everyone** (`inventory_ui.cc:5123-5128`); the driver moves items with `b*` verbs directly. Shape:
  - In `inventoryOpenTradeViewer`, when `clientBarterIsDriver()`, re-enable the four slot-button ranges
    (A5 table), but replace each `_barter_move_*`/`itemMoveForce` with a **verb send**:
    1000→`boffer`, 2000→`btake`, 2300/2400→`bunoffer`, using `clientViewerBarterVerb` (the seam already
    exists per `7021697`). Pass the **pid** (from the mirror row) + quantity; open
    `inventoryQuantitySelect` locally for the count on a `>1` stack, then send the chosen qty.
  - **Do NOT mutate the mirror locally** — that "invents items the server does not have". The item only
    moves when `EVENT_BARTER_STATE` comes back (the "slap": `clientBarterConsumeDirty` → `repaint`,
    `:5264-5266`). This is correct-by-construction: the driver's own moves round-trip exactly like a
    spectator's view updates.
  - Spectator: leave read-only (verbs refused server-side anyway, `client_barter.h:42-45`).
- **Server drain already reproduces all four moves by pid** (`:4785-4833`). **DONE.**
- **Verbs at the trust boundary**: `boffer/btake/bunoffer/bcommit/bdone/bcancel`, gated on
  `serverControlMayDriveDialog` + `GameMode::kBarter` active (`server_control.cc:1284-1322`). **DONE.**

## B6. Display reads (A6)
- **Four inventories + two valuations streamed, all server-computed.** → `barterEmitState` snapshots all
  four tables and computes offer (`objectGetCost`) + asking (`barterComputeValue`) server-side
  (`inventory_ui.cc:4546-4577`); `EVENT_BARTER_STATE` carries them count-prefixed
  (`presenter_network.cc:774-800`); viewer stores as received, never recomputes
  (`client_barter.cc:120-121`, decoder `client_net.cc:2829-2861`). **DONE @`984f0e6`+`8f3aca7`.**
- **Spectator renders the DRIVER's pack, not their own.** → left panel = `clientBarterDriverInv()`
  snapshot (`inventory_ui.cc:5187-5197`). **DONE @`e55ff06`.**
- **Player body art should be the driver's** → **OPEN**, folded into B4 (stale-fid).
- **State re-sent after a REFUSED commit too** ("nothing moved" is the answer to a bad offer). →
  `barterEmitState` after both success and failure (`:4776-4782`). **DONE.**

## B7. Party-member barter (A7)
- **$ vs weight labels, weight-gated transaction, no weapon strip.** → Server computes both valuations
  correctly under `gGameDialogSpeakerIsPartyMember` and streams the results, so the numbers are right on
  every viewer. **Partial.** **OPEN / watch:** the viewer's inner-label branch ($ vs "weight N")
  reads `gGameDialogSpeakerIsPartyMember` **locally** (`inventory_ui.cc:4430,4469`) and the body-art
  background fid picks 420 vs 111 off it too (`:2013`). On a viewer this global is not set from the wire
  today. Shape: stream a `speakerIsPartyMember` bit in `EVENT_BARTER_BEGIN` (or derive from the
  merchant mirror's party flag) and seed `gGameDialogSpeakerIsPartyMember` in `clientBarterOnBegin`.
  Low priority until a player-companion is traded with.

## B8. Time/tick (A8)
- **Fidget/lips/reaction run on the viewer, never the server.** → viewer keeps the ticker tail
  (`:3247-3248`), starts lips from the wire (`client_dialog.cc:75`); server builds no head so all are
  free no-ops. **DONE.**
- **`aiAttemptWeaponReload` is a real sim mutation and must run headless.** → kept on the
  `serverLoopActive()` path in `_gdialog_barter_destroy_win` (`:3793`). **DONE.**
- **"Someone is trading/talking" freeze notice.** → per-actor `consoleMessageStyled` to every non-driver
  **before** the barrier (buffered flush caveat), emitted before `barterBegin` which force-flushes
  (`inventory_ui.cc:4667-4699`). **DONE @`e55ff06`.**
- **Viewer modal must pump the wire.** → `GameMode::kBarter` and `kDialog` are in `kViewerModalMask`
  (`client_net.cc:168-170`); the service ticker pumps + force-closes on combat/rebaseline. **DONE
  @`7021697`.**

---

## Open-items summary (the remaining moles, ranked)

1. **[B5] Driver drag/drop reroute to `b*` verbs** — the one feature still missing; read-only today.
   Correct-by-construction shape given above (send verb, never mutate mirror, wait for the slap).
2. **[B4/B6] Stale `gInventoryWindowDudeFid`** — wrong/blank player body in the barter window; recompute
   after the driver is assigned.
3. **[B2] Scripted `gdialog_barter` opcode gated on `_dialog_state_fix`** — silent no-op on a `gsay`
   second conversation; re-gate on the node-depth counter server-side. Watch for a merchant that uses
   the opcode form.
4. **[B7] `gGameDialogSpeakerIsPartyMember` not seeded on the viewer** — $ vs weight label + 420/111
   background wrong for a party-member trade; stream the bit. Low priority.
5. **[B3] `bcancel` inherits vanilla's destroy-goods-on-ESC hole** — unreachable via normal UI (ESC maps
   to `bdone`); owner call whether to ever wire a true Cancel.
