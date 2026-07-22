# INTERACTION_UX_DESIGN.md — the whole out-of-combat interaction UX, as one pipeline

STATUS: STAGES 0-2 LANDED (2026-07-17), live-verified (walk-then-act, action menu,
pickup, hover highlight); adversarial review clean after two fixes (below). Stage 3
(action gesture presentation, EVENT_ACTION_ANIM) is NEXT. Design pass = Fable
2026-07-17. SUPERSEDES the earlier cheapest-first verb ladder
§2-3 — that plan's recon (§0) remains valid and is cited here, but the delivery model
changes from "one thin verb per commit" to **one coherent pipeline in four stages**:
vanilla action menu on the viewer → targeted wire verbs → a server-side WALK-THEN-ACT
compound action → animated presentation. Slice 1 (`usedoor <netId>`, commit 789ccd5)
was the pipeline's proof and is now retrofitted into the compound-action model
(`usedoor` kept as an alias of `use`).

►► REVIEW CORRECTION (supersedes §2.3's re-mint claim): the design said a stale
`targetNetId` "fails the re-lookup" after a map load. FALSE — objectAssignAllNetIds
RE-MINTS netIds by re-assigning them sequentially, so a stale netId resolves to a
DIFFERENT object. This bites on the SAME map too (a second viewer joining triggers a
rebaseline re-mint). FIX SHIPPED: the latch snapshots target id+pid at arm and the
poll drops if the re-looked-up netId's id/pid don't match (ServerWalk's pattern) — so
it never fires an outcome on the wrong object. Second fix: no-approach / immediate-fire
verbs now `reg_anim_clear` the actor so a superseded approach walk doesn't keep running.
BANKED latent (pre-existing, not introduced here): `look` on a weapon-wielding critter
can reach `_obj_examine_func`'s `exit(1)` if proto msgs 546/547 are absent — impossible
on stock data, a threat only under incomplete mod data.

Companions: COMBAT_CLIENT_DESIGN.md §3 (the S3 capture-don't-dispatch + commit-fork
pattern this generalizes), PRESENTATION_FSM_DESIGN.md + src/client_present.{h,cc}
(the per-netId glide/replay/door presentation this rides), MP_PROTOCOL.md §1/§2/§8
(wire rules, netId addressing, control plane), memories [[mp-actor-architecture-
principle]], [[visual-verification-protocol]], [[sweep-before-recon-lesson]].

--------------------------------------------------------------------------------
## 0. Decisions to settle (recommendations inline — settle before Stage 1)

1. **Walk-then-act mechanism: pending-action latch, NOT reg_anim callbacks.**
   Recommend a server-side `PendingInteraction` latch (the `cstart` latch pattern,
   server_control.cc:42-49) armed at verb receipt and polled once per beat, over
   re-enabling vanilla's `animationRegisterCallback` chain under the server
   scheduler. Rationale in §2.2 — the server anim backend's callbacks are
   *deliberately* no-ops (server_anim.cc:74-82), and reversing that single fact
   re-opens the whole callback fidelity boundary for every caller in the engine,
   not just ours. The latch is ~80 lines, all in server-only TUs.
2. **Fire site: the inbound-drain site, immediately after `serverAnimAdvanceWalks()`**
   (server_main.cc:162-175), not the server loop's idle tick. This is the site where
   `mv` and `usedoor` already execute scripts synchronously (proven safe), and firing
   *after* the walk advance means an approach that completes this beat fires its
   action this beat — outcome events ride the same frame as the final hop, in wire
   order. §2.3.
3. **Gesture presentation: one new wire event, `EVENT_ACTION_ANIM`, emitted by the
   server-only executor** (so no shared-code fork at all), decoded into the existing
   client_present REPLAY concern exactly like `EVENT_WEAPON_TAKE_OUT` (=27). It is
   droppable scope: stages 1-2 are fully shippable with instant outcomes (state flip
   + door slide + console text), gestures land in stage 3. §4.
4. **Menu reuse: EXTRACT the vanilla build + hold-loop into callable functions**
   (`gameMouseBuildActionMenu` / `gameMouseRunActionMenu`) rather than copying ~150
   lines of game_mouse.cc into main.cc. The legacy `_gmouse_handle_event` calls the
   extracted functions and keeps its local execute switch; the viewer calls the same
   functions and maps the selection to wire verbs. One source of truth for menu
   contents; game_mouse.cc is never linked headless, so goldens are structurally
   untouched. §3.2.
5. **TALK ships as a server verb in stage 1 but is NOT wired to the viewer menu
   until dialog-options streaming exists.** The server dialog driver does not block —
   with no queued intent it *ends the conversation at the first node*
   (game_dialog.cc:1907-1911), so a viewer-triggered talk would run the script's
   entry node (side effects included) and instantly exit, invisible to the player.
   Wire the menu item in the dialog-streaming track, not here. §5.
6. **Adjacency rule per verb**: use/get/loot/skill = distance ≤ 1 (vanilla
   `_is_next_to`, actions.cc:1107-1122); talk = distance < 9 AND unblocked LOS
   (vanilla `_can_talk_to`, actions.cc:2081-2097); look/push = no approach at all
   (vanilla registers no move for them — game_mouse.cc:1184-1187, actions.cc:2280-2350
   move the *target*, not the actor). Encode the rule in the latch, per verb. §2.4.

--------------------------------------------------------------------------------
## 1. Architecture — the one pipeline

```
VIEWER (presentation + intent only)          SERVER (all authority)
─────────────────────────────────           ───────────────────────────────────
right-click cycles MOVE↔ARROW                serverControlLine (trust boundary):
hover → vanilla primary-action icon           parse verb, claim gate, validate
click → primary verb  ──────────────────▶     netId/type/elevation/args
hold  → vanilla action menu (extracted        │
        build+pick, pure-read on mirror)      ▼
        selection → sendLine("<verb>          PendingInteraction latch armed +
        <netId> [args]")                      approach registered (move-to-object
                                              → stepped-walk registry)
                                              │  …beats pass, MOVE hops stream…
                                              ▼
                                              latch poll (post-walk-advance, same
                                              drain site): adjacent? → fire REAL
                                              engine outcome (_obj_use / _obj_pickup
                                              / _obj_examine / …scripts run here)
                                              │
◀── OBJECT_DELTA / CONNECT / DISCONNECT ──────┘
◀── EVENT_DOOR_STATE / EVENT_CONSOLE / EVENT_ACTION_ANIM (presentation lane)
    │
    ▼
client_present: approach = ordinary MOVE-hop glide; outcome gesture = REPLAY
concern queued behind the actor's glide; door slide = DOOR concern (existing)
```

The seam, stated once: **the viewer owns menu construction and cursor modes (local,
pure-read on the mirrored world); the server owns validity, locomotion, timing, and
every state mutation; client_present owns how the result looks.** The viewer never
calls an action function — the S3 discipline (COMBAT_CLIENT_DESIGN.md §3.b
capture-don't-dispatch) extends verbatim: `_gmouse_handle_event` is still never
called (its ARROW branch mutates sim, game_mouse.cc:954-999); the extracted menu
functions the viewer *does* call are display/pick only.

Client-side validity checks (which menu items appear) are UX nicety; the server
re-validates everything at the trust boundary, exactly as `cattack` re-validates
hit mode/location (server_control.cc:227-240).

--------------------------------------------------------------------------------
## 2. WALK-THEN-ACT — the server compound action (the crux)

### 2.1 What vanilla does (the shape we're reproducing)

Every object action is a reg_anim sequence: *move-to-object → `_is_next_to` forced
callback (aborts with "You cannot get there.", actions.cc:1107-1122) →
`_check_scenery_ap_cost` → gesture anim (ANIM_MAGIC_HANDS_*) → outcome callback*
(`_obj_use` at actions.cc:1276, `_obj_pickup` at :1370, `_obj_use_skill_on` at
:1680, `scriptsRequestLooting` at :1452, `_talk_to`→`scriptsRequestDialog` at
:2076-2103). The sequence encodes approach + adjacency check + presentation +
outcome as one cancellable unit.

### 2.2 Why we do NOT reuse that sequence under the server scheduler

On the server, the reg_anim symbols are resolved by server_anim.cc, whose model is
*apply-state-at-register-time*: moves either apply synchronously or enqueue on the
stepped-walk registry (server_anim.cc:474-506), and **every `animationRegisterCallback*`
is a deliberate no-op** (server_anim.cc:756-769, rationale at :74-82: the only
server-reachable callbacks were pure presentation, and honoring them wholesale
risks pulling client-only tail stubs). That is precisely why the `serverLoopActive()`
decouples exist — they run the outcome directly and *skip the walk* (actions.cc:
1195-1213 use, 1302-1323 pickup, 1638-1650 skill). Making callbacks fire only for
"our" sequences means teaching server_anim which registrar is which — a hidden
mode flag — and firing them at walk completion *inside* `serverAnimAdvanceWalks`
puts synchronous script execution inside the loop that already needs an epoch
guard against exactly that re-entrancy (server_anim.cc:206-208, 426-440: a door-USE
script mid-step can mutate the registry; an outcome script can do strictly more —
start combat, transition the map, destroy the actor). Rejected.

Also rejected: keeping v0 act-from-any-distance and having the *viewer* walk first
then send the verb on arrival. The viewer is not trusted with sequencing (a
malicious client just skips the walk), and a client-side compound breaks on any
mid-walk authority change. Sequencing is sim behavior; sim behavior lives on the
server.

### 2.3 The design: `PendingInteraction` in server_control.cc

One latch per claimant (v1: one claimant, so one latch — but keyed alongside the
claim so the N-player generalization is a map, not a rewrite):

```
struct PendingInteraction {
    int verb;          // USE / GET / SKILL / TALK / LOOT / USEDOOR ...
    int targetNetId;   // re-looked-up EVERY poll (never a cached Object*)
    int arg;           // skill id for SKILL; 0 otherwise
    int beatsLeft;     // backstop cap (~150 beats = 15 s of pathing) — fail→drop
};
```

**Arm (at verb receipt, in `serverControlLine`)** — after the standard gates
(claimant, out-of-combat, netId lookup via `objectFindByNetId` object.cc:1991,
per-verb type/elevation/arg validation, §5 table):
1. If the adjacency rule (§2.4) already holds → **fire immediately** (this preserves
   slice-1 behavior for an adjacent door, and is the same call the probe verbs make).
2. Else register the approach through the same public entry points `mv` uses
   (server_control.cc:56-74): `reg_anim_begin(ANIMATION_REQUEST_RESERVED)` +
   `animationRegisterMoveToObject(actor, target, -1, run)` + `reg_anim_end()`.
   Under F2_SERVER_SMOOTH_WALK this enqueues a stepped walk that stops one tile
   short of the target and faces it on arrival (server_anim.cc:518-570 — the
   hidden-target pathing and `faceTile` are already implemented). Then arm the latch.
   A `run` heuristic mirrors vanilla: run when distance ≥ 5 (actions.cc:1337).

**Poll (once per beat)** — a new `serverControlAdvancePending()` called from
server_main's intentsDrain **after** `serverAnimAdvanceWalks()` (server_main.cc:175),
so arrival and firing share a beat:
```
if no latch → return
if isInCombat() → drop latch            // combat entry cancels intent (vanilla:
                                        // animationStop clears walks, combat.cc:2524)
target = objectFindByNetId(latch.targetNetId)
if target gone, or elevation mismatch, or actor dead → drop latch (console 2000 optional)
if adjacencyRule(verb) satisfied → CONSUME LATCH FIRST, then fire outcome
else if !serverAnimWalkInFlightFor(actor) → drop latch + console message 2000
        ("You cannot get there." — the vanilla _is_next_to text, actions.cc:1111-1116,
        via presenter()->consoleMessage → EVENT_CONSOLE, already decoded by the viewer)
else if --latch.beatsLeft <= 0 → drop latch + console 2000
```
`serverAnimWalkInFlightFor(Object*)` is a new ~6-line query on the walk registry
(server_anim.{h,cc}) — the only server_anim change this design needs.

**Fire** — call the same outcome functions the probe verbs and the
`serverLoopActive()` decouples already exercise (they are the proven headless
bodies; see §5 table). The outcome runs synchronously at the drain site — the
exact site `_obj_use_door` runs today (server_control.cc:172) and `commandDispatch`
runs the probe verbs, so no new safety class.

**Cancellation / replacement (explicit, all last-writer-wins):**
- A new `mv` or any new interaction verb **replaces** the latch and re-registers the
  approach (vanilla equivalent: a new click `reg_anim_clear`s the old sequence,
  actions.cc:1327-1329; `reg_anim_clear` already cancels the stepped walk,
  server_anim.cc:639-646). A bare `mv` clears the latch without arming a new one —
  "walk away" cancels the intent.
- Claim release (disconnect) → clear the latch in `serverControlBeginDrain`
  (server_control.cc:81-92), next to the claim itself.
- Map transition / rebaseline → the netId re-lookup fails (netIds are re-minted per
  load, MP_PROTOCOL §7d "netId RECYCLING") → drop. No generation bookkeeping needed
  beyond what the walk registry already does (server_anim.cc:407-409).
- `cstart` → combat entry → the isInCombat() poll gate drops it.

### 2.4 Adjacency rules (per decision #6)

```
USE / USEDOOR / GET / LOOT / SKILL : objectGetDistanceBetween(actor,target) <= 1
TALK                               : distance < 9 && !_combat_is_shot_blocked(...)
                                     (actions.cc:2070, 2083)
LOOK / PUSH / ROTATE               : no approach — fire at receipt (vanilla walks
                                     for none of these)
```
TALK's approach registration also mirrors vanilla: only register the move if the
rule doesn't already hold (actions.cc:2070-2072).

### 2.5 Re-entrancy and hazard inventory (call these out in review)

- **Outcome scripts can do anything.** `_obj_use` dispatches the target's USE
  script; doors/ladders/stairs route internally to `_obj_use_door` / `useLadder*` /
  `useStairs` (actions.cc:1195-1205 comment; proto_instance.cc:1560-1616 — stairs
  and ladders can `mapSetTransition`). `_obj_pickup` fires pickup procs.
  `scriptsRequestDialog` latches a request consumed by `scriptsHandleRequests`
  later the same tick (server_loop.cc:269; proven by the `dtalk` probe,
  command.cc:613-647). Mitigations: (a) fire at the drain site only — never inside
  the walk-advance loop; (b) **consume the latch before calling the outcome** so a
  script that re-triggers control flow cannot double-fire; (c) never hold `Object*`
  across the call — re-look-up by netId if needed after.
- **The actor itself can be destroyed/moved by the outcome** (a stairs use changes
  elevation; a trap use can kill). Nothing after the fire call touches the actor.
- **The approach can open doors en route** — already handled by the walk engine
  (serverAnimStepOnce opens usable doors and re-proves owner liveness,
  server_anim.cc:319-332). No interaction with the latch: the latch only ever
  observes tiles at the poll site.
- **Untrusted args index tables** — the `cattack hitLocation` lesson
  (server_control.cc:231-240). `skill` must be allow-listed to the eight skilldex
  skills (SKILL_SNEAK/LOCKPICK/STEAL/TRAPS/FIRST_AID/DOCTOR/SCIENCE/REPAIR,
  skill_defs.h:21-31, mirroring game_mouse.cc:1214-1240) before it reaches
  `_obj_use_skill_on`. Every netId is re-validated at both arm and poll.
- **Latch poll cost** is O(1); the netId lookup is an object-list walk (object.cc:
  1991) — once per beat, only while a latch is armed. Fine.

### 2.6 Composition with the stepped walk + presentation FSM (why no new desync class)

The approach is *ordinary* authoritative movement: per-tile MOVE events with
durMs stamped by the walk registry (server_anim.cc:433-434), glided by the GLIDE
concern with present-tile rebucketing (client_present.h:14-19). The outcome fires
in a beat ≥ the final hop's beat, so its events (delta / disconnect / doorState /
console / gesture) are wire-ordered **after** the last MOVE — the decoder's
presentation queue preserves that order (PRESENTATION_FSM_DESIGN.md §5.2). The
FSM's authority-leads-presentation doctrine is untouched because we add **no new
authority writers and no new suppression flags**: the gesture event is a
presentation-lane cue handled by the existing REPLAY reserve/hold machinery (§4),
and doors already work (EVENT_DOOR_STATE → DOOR concern → crosser hold). The #6/#9
class (obj->tile leading the render bucket) is a MOVE-lane property and this design
adds nothing to the MOVE lane.

One accepted v1 artifact: numeric/state effects never wait on pixels (the locked
doctrine, client_present.h:27-30), so a picked-up item DISCONNECTs from the world
the instant the outcome fires, possibly a few hundred ms before the viewer's
magic-hands gesture ends. Same class as per-hit HP, accepted.

--------------------------------------------------------------------------------
## 3. The viewer — vanilla menu, cursor modes, verb mapping

### 3.1 Cursor modes (gap 4)

Out of combat, stop pinning MOVE (main.cc:798, re-pin at :994): right-click calls
the REAL `gameMouseCycleMode()` (game_mouse.cc:1424-1442) — out of combat it
already self-skips CROSSHAIR, so the vanilla cycle *is* MOVE↔ARROW; nothing to
wrap. In combat, keep the existing viewer wrapper (MOVE↔CROSSHAIR, main.cc:894-897)
— ARROW-in-combat (look/inventory mid-fight) is deferred with the in-combat
interaction slice. The existing "snap CROSSHAIR back to MOVE on combat exit" guard
(main.cc:1017-1019) extends to "snap to MOVE" from any non-out-of-combat-legal mode.
The `GAME_MOUSE_MODE_USE_*` skill cursors (game_mouse.h:13-19) stay unused — the
menu's USE_SKILL path never needs them (game_mouse.cc:1210-1244 calls the skilldex
and acts directly).

ARROW hover lights up for free once the mode is reachable: the `gameMouseRefresh`
ticker (already running) draws the primary-action icon per object type
(game_mouse.cc:668-728) and calls `_obj_look_at` on pointer change (:730-733).
On the viewer, scripts are disabled (`scriptExecProc` gates on `gScriptsEnabled`,
scripts.cc:1264-1266), so hover look-at prints the proto name locally — vanilla-
looking, script-override-unfaithful (rare for look_at; accepted, and the LOOK verb
below gives the faithful path). The hover item-outline write (game_mouse.cc:680-686)
is a vanilla presentation write on mirror objects, cleared by the same ticker
(:861-866); note it for the future S6 outline work, harmless now.

### 3.2 The action menu (gap 1)

Extract from `_gmouse_handle_event`'s LEFT_BUTTON_DOWN_REPEAT block into
game_mouse.{h,cc} public functions (decision #4):

- `int gameMouseBuildActionMenu(Object* target, int items[6])` — the per-FID-type
  validity switch verbatim (game_mouse.cc:1074-1125). All predicates are pure reads
  the mirror can answer: `itemGetType` (proto), `_obj_action_can_use` /
  `_obj_action_can_talk_to` (proto flags + critter aliveness, object.cc:1939-1954),
  `_critter_flag_check(CRITTER_NO_STEAL)`, `actionCheckPush` (actions.cc:2236-2277).
  **Known infidelity:** `actionCheckPush` requires `scriptHasProc(sid,
  SCRIPT_PROC_PUSH)` (actions.cc:2258), and `scr->procs[]` is only populated by
  the lazy program load inside `scriptExecProc` (scripts.cc:1276-1297), which never
  runs on the viewer — so PUSH may be missing from viewer menus. Accepted v1 (PUSH
  is niche; the verb still works from the primary-click path when the server says
  yes); the durable fix is a tiny "hasPush" bit on the critter delta, banked.
- `int gameMouseRunActionMenu(int x, int y, const int* items, int count)` — the
  modal hold/highlight/release loop verbatim (game_mouse.cc:1127-1178), returning
  the selected `GAME_MOUSE_ACTION_MENU_ITEM_*`. It renders via
  `gameMouseRenderActionMenuItems` / `gameMouseHighlightActionMenuItemAtIndex`
  (:1736/:1847) and spins its own input/render loop with iso disabled — on the
  viewer this means `conn.pump()` pauses for the hold's duration; TCP buffers and
  the decode catches up on release (same stall class as any modal; the flood guard
  and the pres queue make it a non-event). Legacy `_gmouse_handle_event` calls both
  and keeps its execute switch (:1180-1250) — behavior-identical, and game_mouse.cc
  is not linked into f2_server, so every golden is untouched by construction.

The viewer (main.cc input block, extending :898-968):
- **Left-click in ARROW mode** → primary verb. Reuse the hover primary-action
  selection logic (the switch at game_mouse.cc:677-717): ITEM→GET, CRITTER→TALK
  (deferred, §5 — v1 falls back to LOOK) or USE(loot, deferred→LOOK), self→ROTATE,
  SCENERY→USE or LOOK, WALL→LOOK.
- **Left-hold in ARROW mode** → `gameMouseBuildActionMenu` +
  `gameMouseRunActionMenu` → map selection to a wire verb (§5 table) →
  `conn.sendLine(...)`. CANCEL/unavailable → nothing.
- **MOVE-mode click** keeps its current routing (`mv`; the slice-1 door special
  case at main.cc:955-965 is *retired* once ARROW mode lands — doors become USE
  like everything else, and MOVE-mode clicks mean move again, vanilla semantics).
- Target→netId: the clicked `Object*` comes from `gameMouseGetObjectUnderCursor`
  (game_mouse.cc:1610, pure-read) and carries `obj->netId` (synced by the wire);
  netId 0 = unsynced/cursor object → ignore (object.cc netId contract, :1985-1990).
- **No out-of-combat input lock**: unlike combat (`actionPending`, main.cc:838-840),
  out-of-combat verbs don't freeze input awaiting an answer — vanilla lets you
  re-click mid-walk and the server's last-wins replacement (§2.3) matches that.

### 3.3 ROTATE and self-menu

Vanilla ROTATE mutates `gDude->rotation` locally (game_mouse.cc:1189-1192) — sim
state on the wire's authority object, so it must be a verb: `rot` (no args) →
`objectRotateClockwise(gDude)` at the drain site; the rotation delta streams back.
Cheap, and it keeps the "viewer never mutates synced objects" invariant absolute.

--------------------------------------------------------------------------------
## 4. ANIMATED actions (gap 3) — presentation of the outcome

**Approach**: already animated (stepped-walk glide). **Doors**: already animated
(EVENT_DOOR_STATE=28 emitted at `_obj_use_door`'s presenter call, proto_instance.cc:
1784/2108 → client_present DOOR concern). **Console/float feedback**: already
decoded (client_net.cc:401-403 → displayMonitorAddMessage/textObjectAdd,
:1385/:1412). The missing piece is the actor's *gesture* — the ANIM_MAGIC_HANDS_
GROUND/MIDDLE crouch/reach vanilla plays before the outcome callback (actions.cc:
1258-1270, 1351-1383, 1668-1678).

Design (decision #3): **`EVENT_ACTION_ANIM` (new presentation-lane event, next id
29): { actorNetId: i32, anim: i32 }**, emitted by `serverControlAdvancePending()`
just before the outcome fires. Anim id computed with vanilla's own rule (prone/
ground-flag target → GROUND else MIDDLE, actions.cc:1260-1266). Because the emitter
is a server-only TU, there is **no shared-code fork and no golden exposure at all**
(the network presenter only exists under the server sink; goldens run without it —
same argument as every presenter event). Skip-unknown-T framing keeps old viewers
compatible (MP_PROTOCOL §2 "Open defaults").

Viewer decode → the client_present REPLAY concern, template =
`clientCombatAnimPlayTakeOut` (client_present.h:141-146): reserve the actor at
decode (holds any same-beat pose deltas), enqueue a `PresKind::kActionAnim`, pump
starts it only when the actor has no playable glide (the existing kAttack blocking
rule, client_net.cc pump — the predicate `clientAnimPlayableActiveFor` already
exists, client_present.h:93), play `animationRegisterAnimate(actor, anim)` via the
viewer's reg_anim, resolve on completion (commit held pose, frame 0 — the
[[frame-index-render-gotcha]] helper). Wall-clock cap ~2 s like every replay.
Result: walk → crouch → item vanishes/door opens, sequenced, with zero new FSM
states — this is exactly the "future transitions this design is shaped for" payoff
PRESENTATION_FSM_DESIGN.md §7.6 promised.

Out of scope v1 (each maps onto the same machinery later): climb-ladder gesture +
put-away/take-out bracketing (actions.cc:1162-1176), container open-frame flips,
NPC/script-driven use gestures (the emitter only covers claimant-driven actions —
script-spawned motion is the separate STREAMING-EVENTS-FIDELITY track).

--------------------------------------------------------------------------------
## 5. Verb inventory — menu item → wire verb → server executor

Legend: ✅ = executor exists headless (probe verb / decouple already exercises it).

| menu item (game_mouse.h:26-36) | wire verb | server executor at fire | approach | status / blockers |
|---|---|---|---|---|
| USE (scenery: doors, levers, ladders, stairs) | `use <netId>` | `_check_scenery_ap_cost` + `_obj_use` ✅ (actions.cc:1206-1212; `climb` probe command.cc:547-578) — doors route internally to `_obj_use_door` | ≤1 | **NOW**. Fold slice-1 `usedoor` into this (keep `usedoor` accepted as an alias for probe compat). Ladder/stairs map transitions ride the existing mapTransition path |
| GET (ground item) | `get <netId>` | `_obj_pickup` ✅ (actions.cc:1310-1318; `pickup` probe command.cc:521-546) | ≤1 | **NOW**. Item removal streams via DISCONNECT + owner inventory delta (client_net.cc:785 rebuild) |
| LOOK (examine) | `look <netId>` | `_obj_examine(gDude, target)` — **NEW verb, trivial**: runs SCRIPT_PROC_DESCRIPTION server-side and streams text via `presenterConsoleMessageBridge` → EVENT_CONSOLE (proto_instance.cc:240-250) | none | **DONE** — the old "blocked on console chrome" concern is stale: the viewer already renders EVENT_CONSOLE (client_net.cc:402, :1385, S2) |
| PUSH | `push <netId>` | `actionPush` ✅ (actions.cc:2280-2350; probe command.cc:579-612) — target moves via the anim backend, dispatches SCRIPT_PROC_PUSH | none | **NOW**. Viewer-menu visibility approximate (§3.2 scriptHasProc gap) |
| ROTATE (self) | `rot` | `objectRotateClockwise(gDude)` | none | **NOW**, trivial |
| USE_SKILL | `skill <netId> <skillId>` | `_obj_use_skill_on` ✅ (actions.cc:1638-1650; `useskillon` probe command.cc:134) | ≤1 | **NOW** server-side; viewer runs the local skilldex modal (game_mouse.cc:1214-1240, pure UI) — shown skill % reads premade-char data until the PC-data blob block lands (COMBAT_CLIENT_DESIGN §5.4, same caveat class as to-hit%) |
| TALK | `talk <netId>` | `_can_talk_to` gate + `scriptsRequestDialog` ✅ (actions.cc:2100-2104; `dtalk` probe command.cc:613-647; consumed by scriptsHandleRequests, server_loop.cc:269) | <9 + LOS | verb **NOW**; menu wiring **BLOCKED** on dialog-options streaming + a resumable dialog barrier (decision #5 — today's driver ends the conversation on an empty intent queue, game_dialog.cc:1907-1911). Do in the dialog-streaming track |
| USE on a critter (loot corpse) / INVENTORY | — | `_action_loot_container` → `scriptsRequestLooting` — no headless modal; container pickup declines by design (actions.cc:1318-1322) | ≤1 | **DONE** — the loot/inventory screens track shipped (take/put/takeall transfer verbs + dude-inventory streaming + the loot modal over the wire). Remaining refinement: ammo/charge counts + nested containers (the "A2" encoder gap) |
| CANCEL / DROP / UNLOAD | — | — | — | CANCEL is local. DROP/UNLOAD never appear in the object menu (build switch, game_mouse.cc:1074-1125) — they belong to the interface-bar item buttons, out of scope |

Server verb parsing note: `serverControlLine`'s `sscanf` already reads
`verb + 3 ints` (server_control.cc:100-105) — `skill <netId> <skillId>` fits; no
parser change.

--------------------------------------------------------------------------------
## 6. Staged migration

Every stage keeps gates 1-10 green (they prove non-leakage into headless, nothing
more — no headless oracle exists for any of this; live-verify per
[[visual-verification-protocol]] is the real gate). Order is by dependency; 1 and
2 are independently useful, 3 is polish on top, 0 unblocks 2.

**Stage 0 — menu extraction refactor (game_mouse.cc).** Mechanical (Opus). LOW
risk: pure code motion, behavior-identical for legacy; not linked headless.
Verify: build both binaries; legacy smoke — hold-menu over an item/critter/scenery
looks and executes as before. No adversarial review needed.

**Stage 1 — server compound-action substrate + the verb set.** DESIGN-CLASS
(this doc is its design; implementation Opus, **adversarial review MANDATORY** —
trust boundary + synchronous script re-entrancy + a new persistent server latch).
HIGH risk-concentration, ~all in server-only TUs: `PendingInteraction` +
`serverControlAdvancePending` + `serverAnimWalkInFlightFor` + verbs
`use/get/look/push/rot/skill/talk` (+`usedoor` alias) + the skill allow-list.
Golden-invisible by construction (claim-gated, network-only paths).
Live-verify checklist:
  1. click far door → dude walks tile-by-tile, door opens on arrival (not before);
  2. blocked path → walk stops short, "You cannot get there." in the monitor, no
     zombie latch (re-click works);
  3. mid-walk re-click elsewhere → old intent dead, new one runs (last-wins);
  4. mid-walk `cstart`/hostile aggro → latch dropped, combat clean;
  5. get: far item → walk → item vanishes exactly on arrival beat;
  6. look on a scripted object → script description text (not proto default);
  7. skill (lockpick a door, first-aid self-adjacent) → outcome + console;
  8. disconnect mid-walk → claim + latch released (server log), walk finishes inert;
  9. `F2_TRACE_EVENTS` pass: outcome events strictly after final MOVE in the frame.

**Stage 2 — viewer ARROW mode + menu → wire.** Mostly mechanical (Opus; one
design-adjacent spot: the out-of-combat mode-cycle interaction with the combat
wrapper + re-puppet re-pin, main.cc:984-999). MEDIUM risk, viewer-only.
Retires the MOVE-mode door special case (main.cc:955-965). Live-verify:
  1. right-click cycles MOVE↔ARROW out of combat; combat entry/exit restores modes;
  2. hover in ARROW shows vanilla primary-action icon per type;
  3. click = primary verb (item→get, scenery→use, wall→look, self→rotate);
  4. hold = the vanilla vertical menu; drag-select; CANCEL inert;
  5. rotate self → sprite turns (round-trips through the wire);
  6. skilldex opens locally, selection fires `skill`;
  7. TALK item present but wired to a local "dialog not yet on the wire" console
     note (or omitted — pick one at implementation);
  8. rebaseline mid-session → modes re-pin correctly, menu still works.

**Stage 3 — `EVENT_ACTION_ANIM` gesture presentation.** Mixed: wire event +
decode + present-queue integration (Opus; **targeted adversarial pass on the
queue/REPLAY interaction** — same no-oracle class as all presentation,
[[anim-decouple-verification]]). MEDIUM risk. Live-verify: walk→crouch→pickup
sequences correctly (gesture never plays mid-glide); door use = gesture + slide;
gesture on a killed/destroyed actor cancels clean (cap); out-of-combat pump
self-heal still releases (no new wedge).

**Stage 4 — banked follow-ups (separate tracks, not this design):** targeted TALK
UX (dialog-options streaming + resumable dialog barrier — its own modal-streaming
design); LOOT + inventory screens (dude-inventory/ammo/nested streaming + transfer
verbs); in-combat interaction verbs (enqueue as combat intents behind the barrier,
AP costs via `_check_scenery_ap_cost` which is only live in combat); enemy
outlines; aimed-shot 'N'/'B'.

--------------------------------------------------------------------------------
## 7. Constraints honored (checklist against the load-bearing invariants)

- **Server-authoritative / capture-don't-dispatch**: viewer adds zero sim-mutating
  calls; the only new local mutation is none — even self-ROTATE goes upstream (§3.3).
- **Claim-gated**: every new verb sits behind the claimant check like mv/usedoor;
  the latch dies with the claim (§2.3).
- **Actor-first (never party-branch)**: the latch is conceptually per-claimant and
  the executor is actor-parameterized (`gDude` only at the v1 binding resolve,
  server_control.cc:52-55 pattern); adjacency and outcomes read the actor object,
  never party state.
- **Golden-invisible**: server changes live in server_control/server_main/server_anim
  (server-only TUs) + one additive presenter event emitted only under the network
  sink; viewer changes in main.cc + the game_mouse extraction (not linked headless).
  The single shared-file edit class is the stage-0 refactor — code motion, and the
  `check.sh` gates stay the regression floor after every stage.
- **Engine singletons untouched**: one map, one object list, one latch — the
  multi-claimant map<sessionId, PendingInteraction> is a named future step, not
  scaffolding built now.
