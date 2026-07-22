# Fable source sweep — co-op live-play bugs 2026-07-21

Scope per owner rule: items A–N only; banked hypotheses re-verified against today's source,
not inherited. All citations are working-tree (`rewrite/phase0`) unless marked `@HEAD`.
Per `dont-declare-not-a-bug-confidently`: every "vanilla-faithful" below is hedged — it means
"this specific mechanism matches the vanilla path I read", not "the behavior is correct".

## Cross-cutting mechanisms (the high-value output)

**X1 — STATE always outruns PRESENTATION at decode, and held-deltas cover only two cases.**
The decoder applies MOVE/OBJECT_DELTA the instant they decode (client_net.cc:1286,
1513-1533); recorded sequences and attack replays drain later through the paced queue.
The only fields ever *held* for presentation are (a) fid/flags/rot of a reserved **attack
participant** (client_net.cc:1501-1513, "held values land verbatim when the replay
finishes") and (b) fid/rot routed onto an active **glide** (1519, 1530). Knockback
position, corpse fid outside an attack replay, container frame — none are held. K, J, G
and half of D2 are this one seam. The banked "held-delta family" fix shape is right; the
sweep's finding is *where the holes are*: MOVE_STRAIGHT state (server_anim.cc:917 — "state
… NOT applied here", banked gap), and the held-fid landing *verbatim after* the replay
(which makes state the LAST writer in combat and the FIRST writer out of combat — see G's
bomb-vs-rocket differential).

**X2 — the turn barrier is the ONLY backpressure, and it throttles exactly one viewer.**
The server blocks only on the *acting* slot's client (combat.cc:3687-3692); every other
viewer's queue is bounded only by the 1024-event drop-oldest cap (client_net.cc:327,
672-691). There is no timestamp, no lag metric, no catch-up/skip rule anywhere in the
pump (presentationPump, client_net.cc:454-566, consults no ts; the frame `simTs` is read
and discarded — 2992 "unused headless"). MP_PROTOCOL §1's stale-event catch-up rule was
never implemented viewer-side. This is item C, and it feeds D2.

**X3 — per-process derived state that never crosses the wire.** The dude's base look
(movie-seen flags → `_art_vault_guy_num`, item B), weapon ammo (item M), container frame
(item I), and the per-actor proto-row fid seeds (B) are each derived or defaulted locally
per process. The report's own audit question ("for every visual attribute, who owns it,
what does a late joiner see") is confirmed as the right frame; B/M/I below name the exact
fields and the exact derivation sites.

**X4 — modal drivers killed by the other lane.** A (worldmap modal ESC-bombed by the
viewer's own service ticker because a *queued, un-drained* COMBAT_EXIT still reads
`_inCombat`), E (dialog window latched open forever if DIALOG_END never lands — no
client-side unwedge exists), L (movie was a silent server no-op at HEAD; the in-progress
barrier has its own wedge hazard). The common defect: modal entry/exit signals take
effect immediately while combat framing drains through the paced queue, so a modal can
open inside a combat state that has already ended on the wire.

**Misdiagnosis flags on the report itself**
- **A**: the reporter's shape ("deferred COMBAT_EXIT applies and clobbers the screen") is
  inverted. The combat exit does not tear down the worldmap — the *not-yet-applied* exit
  (still queued behind the fight's presentation) makes the service ticker believe combat
  is live and it force-feeds ESC into the worldmap modal, which *tells the server to leave
  the worldmap*. Server-side transition-vs-combat sequencing is the vanilla flee path and
  looks sound (hedged).
- **J**: "position delta interpolated as an ordinary walk" is not possible as stated — a
  durMs<=0 or non-adjacent MOVE always snaps (client_present.cc:1003-1015). The glide the
  owner saw is the *recorded knockback MOVE_STRAIGHT replaying through the real reg_anim*
  (likely anim-less → a slide), not the glide machinery.
- **C**: the addendum's hypothesis is structurally confirmed, with one correction: a
  *living* P2 also gets no direct backpressure during P1's turn — what saves a living P2
  is that his own turn arrives every round and the server then blocks until *his* client
  (input-locked until its queue drains, main.cc:1184-1185 + 1558-1560) answers. Death
  removes that per-round drain window, so the backlog integrates monotonically. Crowding
  raises per-round emission; death removes the drain — both are needed, neither alone.

---

## A. Transition × combat × dead player → black map — ROOT-CAUSED (chain), tail hedged

**Mechanism.**
1. P1 in combat steps on the exit grid → the exit-grid script funnels into
   `mapSetTransition` → `if (isInCombat()) _game_user_wants_to_quit = 1` (map.cc:1103-1105)
   — vanilla's flee-combat signal.
2. The resumable session sees the flag, ends the fight; `combatTeardown` runs
   `_combat_over` → `presenter()->combatExit()` (combat.cc:2743) and clears the flag
   (combat.cc:3554-3556). Same beat, *after* the session advance, `mapHandleTransition`
   (server_loop.cc:430-434) takes the `-2` branch — now `!isInCombat()` — and requests the
   worldmap (map.cc:1137-1142).
3. Next beat `scriptsHandleRequests` (server_loop.cc:382) enters `worldmapServerDriver`,
   which emits WORLDMAP_BEGIN and block-and-pumps (server_worldmap.cc:82, 171-186).
4. Client: COMBAT_EXIT is **queued** as `kExit` behind the fight's whole presentation
   backlog (onCombatExit, client_net.cc:1989-2001) — `_inCombat` stays TRUE until it
   drains. WORLDMAP_BEGIN applies **immediately** (onWorldmapBegin, 2814-2819) and the
   frame loop enters the worldmap modal (main.cc:1138-1142).
5. Inside the modal, `presentationPump` never runs (it is called only from the main frame
   loop, main.cc:1509; the modal ticker only pumps the wire, client_net.cc:3401), so
   `_inCombat` stays true for the whole modal. `viewerServiceTicker` then fires every
   iteration: in a modal + `inCombat()` + kWorldmap not a sanctioned screen →
   `enqueueInputEvent(KEY_ESCAPE)` (client_net.cc:3409-3420; kWorldmap is in
   kViewerModalMask, 164-166).
6. The worldmap modal reads that ESC → `clientViewerWmEscape()` (worldmap_ui.cc:344-347)
   → server driver pops WM_INTENT_ESCAPE → breaks with `map == -1` → WORLDMAP_END, **no
   map load, no rebaseline** (server_worldmap.cc:219-249, and its own comment: "the
   failure mode (map == -1) is invisible on the wire"). Client modal exits
   (worldmap_ui.cc:163).
7. Back in the main loop the queue finally drains → `applyCombatExit` chrome — the
   owner's "then combat ends fires", *inverted* from wire order.

**The black-screen tail (two cited candidates, not pinned):**
- (a) The ticker enqueues one ESC per iteration; the modal consumes ~one per iteration but
  the last ESC(s) survive the modal break, and the main loop treats ESC as *quit the
  viewer* (main.cc:1168-1171 `break` → teardown at 1624-1630, `main_unload_new`, process
  exit via falloutMain:134-139). Matches "black + input dead".
- (b) If WORLDMAP_BEGIN and WORLDMAP_END decode in the same pump (fast bail), the frame
  loop still consumes the stale `gPendingWorldmapEnter` latch and **force-sets
  `gWorldmapStreaming = true`** (main.cc:1139-1140), destroying the END that already
  applied → modal never exits, intents go to a server no longer in the driver.
  Matches "stuck", less well "black".

**Fix shape (structural, not a guard):** COMBAT framing must not be readable as "live"
by lifecycle logic once the wire has ended it — split the ticker's force-ESC test off the
presentation-paced `_inCombat` (it should read a decode-time mirror, e.g. wire-order
combat state), and make WORLDMAP_BEGIN ride the same presentation queue as COMBAT_EXIT so
modal entry cannot overtake the fight's end. Independently: the main loop's ESC-quit
should not consume synthetic ESCs minted by the ticker (they are addressed to modals).
The BEGIN/END latch race (b) wants `onWorldmapEnd` to also clear `gPendingWorldmapEnter`
(client_net.cc:2821-2827 currently doesn't).
**Confidence:** high on steps 1-7 (every link cited); medium on which tail produced the
owner's exact final screen. Falsified if the session log lacks the `[wmsrv] driver exit:
areaId=… map=-1` line — that line *must* be present if this chain fired.

## B. Skin desync (tribal vs vault suit) — ROOT-CAUSED (mechanism family)

**Mechanism.** The base look is not replicated state; it is re-derived per process from
the movie-seen ledger, at every map load:
- `mapLoad` tail calls `_proto_dude_update_gender()` unconditionally — including the
  viewer's blob load (map.cc:858; the `gMapViewerLoad` gate at 797 covers scripts only).
- `_proto_dude_update_gender` picks TRIBAL vs JUMPSUIT from
  `gameMovieIsSeen(MOVIE_VSUIT)` (proto.cc:946-949), rewrites `gDude->fid` locally when no
  armor is worn (proto.cc:960-968) and always rewrites `gDudeProto.fid` +
  `_art_vault_guy_num` (proto.cc:958, 970).
- The movie ledger is savegame-only state (game_movie_state.cc:82-99; wired at
  savegame.cc:106/137). The join blob (applyBlob, client_net.cc:883-990) carries map +
  dude + actors + sheets — **no `gameMoviesLoad`** — so each viewer keeps whatever ledger
  its own process has (fresh viewer = all-unseen = TRIBAL), while the server's ledger
  advances (stub `gameMoviePlay` marks seen, server_stubs.cc:319-331).
- Per-actor proto rows are seeded by memcpy from the *viewer-local* `gDudeProto`
  (proto.cc:403-410, called at client_net.cc:966) — the locally-derived look leaks into
  every actor row on that client.
- Server-side, `_proto_dude_update_gender` touches **only gDude** — extras (minted once by
  `_obj_copy(&extra, gDude)` at boot, server_boot.cc:309) are never re-derived, so host
  and extras can legitimately diverge server-side after VSUIT flips.
- During a blob apply, gDude is parked on the HOST body when map.cc:858 runs
  (client_net.cc:912-915; rebind happens later at 800/806) — the known cached-anchor
  class, though `_obj_load_dude`'s subsequent memcpy (client_net.cc:942) re-overwrites the
  host fid with server truth, so this particular write is *usually* masked.

The weapon-animation half of the symptom is the report's own known signature: a tribal
base fid has almost no weapon art, so any actor stuck on it plays wrong/missing weapon
animations (feeds D1).

**Gap (why not 100%):** the blob does overwrite obj->fid for host and extras with server
truth, so the *rendered* divergence needs one of the local re-derivation paths to fire
after the blob (the no-armor `objectSetFid` on a later local mapLoad, the proto-row leak,
or a server-side host/extra divergence distributed to everyone). Which one fired in the
session is not provable from source alone.
**Fix shape:** treat base look as replicated state: ship the movie-seen ledger in the
blob (one 17-byte array) AND stop re-deriving on the viewer (the map.cc:858 call has no
business running under `gMapViewerLoad` — move it to the sim-side load driver). Server
side, re-derive extras wherever the host is re-derived (or better: store nativeLook on
the sheet). Owner call on which of the two ends is authoritative.
**Confidence:** high on the mechanism family, medium on the exact rendered path.
Falsify: log `_art_vault_guy_num` on both clients + server after a repro load; if they
match, this is dead and the divergence is elsewhere (then suspect the recorded SET_FID
lane).

## K + J. Knockback vs position — ROOT-CAUSED (K), STRONG LEAD (J, same seam)

**K mechanism.** Two lanes, no hold between them:
- State lane: the server displaces victims with a bare `objectSetLocation`
  (`_combat_apply_knockback`, actions.cc:237-266; explosion call at 2136, attack path
  mirrors it) → a MOVE with durMs=0, which the viewer applies and **snaps immediately,
  by design** ("knockback/teleport hops (durMs<=0) never hold", client_net.cc:1291;
  snap: client_present.cc:1003-1015). `clientCombatAnimDeferMove` defers only armed
  recorded-*walk* movers (client_net.cc:1262-1273), not knockback.
- Presentation lane: the knockback animation arrives either inside the bespoke attack
  replay (local `_action_attack`, client_present.cc:1516) or as a recorded
  MOVE_STRAIGHT — whose recording leaf explicitly does **not** apply state
  (server_anim.cc:912-919: "the state (final tile) is still owed by the banked knockback
  gap fix and is NOT applied here").

Which lane wins is a decode-vs-pump race: if the snap decodes before the replay starts
(queued backlog — the normal in-combat case), the replay's move has nothing left to
animate → "only an SFX plays and the player is +1 hex". If the replay is already running,
the animation plays and the snap reconciles afterwards (`clientCombatAnimNotifyReposition`,
client_net.cc:1317-1320) → "sometimes it plays". Exactly the owner's read.

**J mechanism (out-of-combat C4, dead player).** actionExplode on the recording server
ships the recorded seq FIRST (actions.cc:2044-2099: record section around the animate
branch incl. `_show_damage` → knockdown MOVE_STRAIGHT + fall ANIMATE + CALL{SHOW_DEATH}),
then the state block (2107-2142: `_combat_apply_knockback` + `_report_explosion` +
`critterKill(…, -1, false)`) — all one beat. Out of combat the seq **plays immediately at
decode** (client_net.cc:2501: queued only if `_inCombat || actorNetId != 0`). The "slow
glide then dies on arrival" is the recorded MOVE_STRAIGHT replaying through the viewer's
real reg_anim with the death callback at seq end — if the fall ANIMATE op doesn't take
(missing art on a wrong base fid — item B — or the stale-frame class), the sprite slides
in stand pose, which is precisely "slowly slides ~5 hexes, then plays the death
animation on arrival". Not the glide machinery (see misdiagnosis flag).
**Fix shape (one fix for both):** the held-delta family, scoped per-victim: a MOVE that
arrives while (or just before) a recorded/replayed sequence owns that object's motion is
held and reconciled at sequence end — i.e. extend `clientCombatAnimDeferMove`'s
recorded-walk arming to MOVE_STRAIGHT/knockback participants; and close the server-side
banked gap so MOVE_STRAIGHT's end tile is applied by the composite (not left to the
separate `_combat_apply_knockback` emit ordering).
**Confidence:** K high; J medium-high (the anim-less slide detail is inferred, the
seq-then-snap ordering is cited). Falsify J by F2_TRACE_EVENTS: a `[walk] SNAP` for the
victim mid-seq confirms; its absence kills the ordering half.

## M. Reload / ammo never replicated — ROOT-CAUSED (both halves)

1. **RELOAD is not reachable.** The control plane's full verb vocabulary
   (server_control.cc:1083-1092 interact verbs; 1371-1375 inventory verbs; 1722-1723 loot
   verbs; plus mv/cmove/cattack/cstart/cendturn/cendcombat/dsay/dend/wm*/claim…) contains
   **no reload verb** (`grep -i reload server_control.cc` → zero). Vanilla's reload paths
   (interface-bar weapon-mode click, inventory drag ammo→weapon) are client-local UI that
   never mints a wire intent.
2. **Ammo state never crosses the wire.** `ammoQuantity` feeds only the change-detection
   hash (MP_PROTOCOL §6 step 3); the OBJECT_DELTA_INVENTORY payload is
   netId/pid/quantity/flags per item and the viewer rebuild "restores proto-default ammo"
   — cited verbatim in-source as a known latent gap (client_net.cc:1477-1482). And the
   local dude's inventory delta is skipped wholesale ("its gear is left to the join blob
   in v1", client_net.cc:1544-1546), so even a blob-accurate count goes stale on first
   shot.
**Fix shape:** add ammo (the item data-union word) to the inventory delta payload, and a
`reload <weaponNetId> [ammoNetId]` verb that runs the real `_item_w_reload` server-side
(AP-priced in combat like the other screen verbs). Both ends are small; no owner decision
needed beyond AP pricing.
**Confidence:** high — the absence greps are exact-symbol, and the payload omission is
documented in-source.

## L. Movie blocks the host — ROOT-CAUSED (at the session's build), fix already in flight

At HEAD — what the live session ran — the server's `gameMoviePlay` is
`gameMovieMarkSeen(movie); return 0;` (server_stubs.cc:327-331 @HEAD): no wire event, no
playback, silent skip. "Nothing happens at all, not even a black screen" is that no-op.
The "stuck" tail is not directly provable from HEAD source; the likely shape is whatever
the triggering script did around the skipped movie (fade/timer), or simply the owner
reading the silent skip as a hang.

The **uncommitted working tree already builds the fix**: EVENT_MOVIE_PLAY id 42
(presenter_network.cc:717-724), a server barrier that parks the tick until any viewer
acks (game_movie_state.cc:41-57), the client playback+ack (client_net.cc:2726-2744), and
a lobby-aware pump (server_main.cc:376-393). Two hazards to check before shipping it:
(1) a movie fired from a MAP_ENTER script runs inside the mapLoad suppression window —
NetworkPresenter gates every override except mapTransition on suppression (MP_PROTOCOL
§7d), so the MOVIE_PLAY emit would be swallowed while `gameMovieServerBarrier` waits for
an ack that can never arrive = a genuine wedge; (2) first-ack-wins releases the room for
everyone — deliberate per the header, just confirm the owner wants the skip to be global.
**Confidence:** high on the HEAD no-op; the report item is effectively "already being
fixed in the working tree" — re-test on that build.

## N. Camera QoL — ROOT-CAUSED (N1), answered (N2)

**N1.** After a blob apply the camera comes from `_obj_load_dude`'s trailing
`gCenterTile` word (object.cc:3597-3603) — the **server's** camera, which headless is
wherever `_map_place_dude_and_mouse`/the save left it (the map entrance). The only
viewer-side recenter-on-own-actor lives in `rebindLocalActor` *behind the binding-change
early-return* (client_net.cc:825-827 return before the tileSetCenter at 872-877) — a
save-load rebaseline doesn't change bindings, so nobody recenters. Fix: unconditional
`tileSetCenter(gDude->tile)` at the end of applyBlob (move it out of the change guard).
**N2.** The leash is vanilla's scroll restriction (`tileSetCenter` honors it unless
TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS, tile.cc:424+). The camera is
client-owned by architecture (ARCHITECTURE.md "who knows what"), so a viewer-side
free-camera toggle that bypasses the restriction is policy-clean. Trivial.

## F. No revive — answered (feature gap + one engine gate located)

The owner's sub-question resolves cleanly: the engine hard-rejects healing items on dead
critters — `_obj_use_item_on`'s drug branch returns -1 with "That won't work on the dead"
(proto_instance.cc:1206-1216). So revive-by-stimpak is a deliberate divergence: gate that
rejection for player-actor corpses, clear DAM_DEAD/restore hp, un-finalize the corpse
(fid/flags — note critterKill's generic corpse ride, item G), and re-admit the actor to
the roster. Owner design decision (respawn policy), plus the separately-flagged
host-ownership transfer (no code path exists; worldmap/dialog/transition gates key on
`serverControlIsHostSession`).

## C. Combat desync / dead-P2 lag — STRONG LEAD (owner's hypothesis confirmed, refined)

Answers to the addendum's specific questions, all cited:
- **Queue bound/drop:** yes, one: 1024 events, drop-oldest-droppable (never turn/exit),
  client_net.cc:327, 672-691. A 1-2-turn backlog sits far below it. Dropping a
  kMoveRelease snaps its glide (683) — state survives, ordering doesn't.
- **Catch-up/lag metric:** none. PresEvents carry no timestamp; the pump
  (client_net.cc:454-566) drains strictly serially at animation wall-speed; frame `simTs`
  is unused (2992). MP_PROTOCOL §1's stale-skip rule is unimplemented viewer-side.
- **Dead client pump path:** identical to a living one — nothing viewer-side knows it is
  dead. The difference is server-side: a dead actor's turn is `kFinalizeNoRun`
  (DAM_DEAD short-circuit, combat.cc:3770-3772) — no barrier, no wait; the barrier waits
  only for the acting slot's session (combatSessionShouldWait, combat.cc:3687-3692).
- **Serialization:** replay is serial per *viewer* (one queue), one AI turn per beat
  server-side (combat.cc:4042-4061) at a beat pace of `F2_SERVER_PACE_MS` or 10ms
  (server_main.cc:499-502) — emission is effectively instantaneous per round vs ~1.5s
  wall per attack replay (kAttackPresentationEstimateMs, combat.cc:3617, is a *timer
  credit to the acting player*, not pacing).
- **Held deltas under backlog:** attack participants' fid/flags are held per-replay, not
  per-queue (client_net.cc:1501-1513) — they land when *that* replay finishes, so a
  2-turn backlog holds them ~2 turns; positions of queued moves are already applied
  (state is current, pixels are old) — which is what makes D2's throw origin wrong.

Refinement of the hypothesis (see misdiagnosis flags): backpressure exists only via the
acting player's own input-lock (main.cc:1184-1185: can't act while `combatBusy`;
1558-1560), i.e. per-round the fight stalls until the acting viewer drains. A living P2
gets one such drain window per round; a dead P2 never does, so any round where
emission > drain adds permanently to his queue. **Policy decision, as the report says**:
a lag-threshold catch-up (collapse/skip queued presentation once behind by > N ms of
wall-time, snap to authority) is the convergent shape and the pump already has the
self-heal precedent (client_net.cc:559-565).
**Falsify:** log `_presQueue.size()` on the dead viewer; if it does NOT grow while P1
acts, this is wrong and the lag lives in the glide/anim layer instead.

## G. Gib/corpse resync — STRONG LEAD (two cited halves), double-C4 NOT REACHED

Re-verified against today's source (per scope rule; the banked note was re-derived, and
one part of it is now WRONG): the server path is actionExplode's state block —
`critterKill(deadTargets, -1, false)` (actions.cc:2140-2142) — whose generic corpse fid
rides that beat's OBJECT_DELTA, in the *same frame* as (but after) the recorded gib
sequence (shipped at 2099). The race then splits exactly along the owner's bomb/rocket
differential, by WHO WRITES LAST:
- Out of combat (placed bomb): the seq plays immediately at decode (client_net.cc:2501);
  the fid delta lands milliseconds later mid-replay, but the recorded CALL{SHOW_DEATH} at
  seq end re-applies the gib corpse **after** it → gibs look right.
- In combat (rocket): the victim is a reserved attack participant, so the fid delta is
  HELD and lands **verbatim when the replay finishes** (client_net.cc:1501-1513) → the
  generic critterKill corpse overwrites the gib final-frame → "resyncs to a standard
  death".
- "Gibs not rendered at all / settles to STANDING": consistent with the same overwrite
  arriving with a stale frame — but note the decoder's immediate path is already
  frame-gotcha-safe (`clientApplyPose`, client_net.cc:1518-1521); the HELD path's landing
  site is the one to audit for a frame reset. Not pinned.
- **Double C4: NOT REACHED.** The old cue-duplication theory is dead in today's tree:
  `presenter()->explosionFx` has zero emit sites (only the virtual, presenter.h:335, and
  the encoder, presenter_network.cc:618). A second EXPLOSION queue-event firing (double
  queue processing around a beat boundary) is the remaining candidate; needs a trace.
**Fix shape:** same as the death half of the record track's design: the corpse-finalize
fid must be carried IN the sequence (SHOW_DEATH already is) and the state-lane fid delta
for a victim whose death sequence is in flight must reconcile *to the sequence's
outcome*, not stomp it — i.e. the held-delta landing should skip fields the replay's own
callbacks already wrote (last-writer should be the sequence, both in and out of combat).

## I. Container open/closed desync — STRONG LEAD

The server flips authoritative state as `item->frame = opening ? 1 : 0` and ships a
one-shot recorded ANIMATE/ANIMATE_REV seq (proto_instance.cc:1926-1941). But `frame` is
not a field of OBJECT_DELTA (mask: fid/rot/flags/hp/rad/poison/ap/results/inventory —
client_net.cc:1465-1476) and has no other channel; the visual state is carried *only* by
that one broadcast sequence. Any client that misses or drops it — the backlog
drop-oldest can eat a kRecordedSeq (client_net.cc:678-688), and any `!_loaded` window
discards events wholesale (585-588) — diverges permanently until the next blob. A late
joiner is fine (frame rides the map blob), which matches "shows up mostly under stress
testing", i.e. backlog. Fix shape: frame (for frame-stateful scenery/items) joins the
delta mask, exactly like fid; the seq stays as presentation.

## D. Invisible weapon + wrong-origin throw — D2 STRONG LEAD, D1 NARROWED

**D2 (throw from across the map).** The attack family is deliberately the LAST record
migration (pres_record.h FAMILY TABLE :53-55) — today the viewer reconstructs the whole
attack locally: `clientCombatAnimPlay` → real `_action_attack(attack)`
(client_present.cc:715, 1499-1516). The projectile origin is therefore
`attacker->tile`-at-replay-time on the *local mirror*; no origin rides the event. Under
any position staleness — item C's backlog (positions applied but *pixels* and held
approach glides old), or a shed glide offset — the arc starts wherever the local mirror
last put him. The report's guess ("re-derived on the viewer") is confirmed. Fix shape:
either finish the attack-family migration (recorded seq carries OBJ_CREATE at the real
origin tile — the vocabulary already exists), or stamp origin tile into
EVENT_ATTACK_RESULT as an interim.
**D1 (invisible wielded spear).** Two candidate feeds, both cited, neither pinned: the
remote-critter inventory reconcile is deliberately equip-flags-only (double-free hazard,
client_net.cc:1556-1561) so a missed membership delta leaves the mirror without the item
object; and item B's base-fid divergence makes weapon-coded fids unrenderable on a tribal
base ("skipping frames" is the missing-art signature). Verify B first — it may subsume
D1 entirely.

## E. denbus2 dialog softlock — NARROWED (our side mapped; script side deliberately dropped per budget)

What the source pins:
- **The softlock shape is a latched dialog window.** While `gClientDialogWindowOpen` is
  true, the viewer routes *every* key into `clientDialogHandleKey` (main.cc:1168-1172);
  the ONLY thing that clears it is a wire DIALOG_END → `clientDialogOnEnd`
  (client_dialog.cc:80-95). There is no timeout, no self-heal, no ESC escape hatch. A
  lost/never-emitted DIALOG_END = permanent input capture = "no input does anything".
- **DIALOG_END has narrow emission conditions**: only when `_gdReenterLevel` returns to 0
  and only when a live pump drove the conversation (game_dialog.cc:2239-2254). Vanilla
  dialog is legitimately re-entrant (`_gdReenterLevel`), and the driver has abort paths
  for nested modes (game_dialog.cc:2085 "requested nested mode … aborting"). A
  conversation that re-enters (second `start_gdialog` — the "dialog appears twice"
  observation; a second dialogNode stream just repaints the open window,
  client_dialog.cc:24-56) and then unwinds through any path that skips the reenter==0
  teardown leaves the client latched forever.
- The money-taken + teleport half completed (state fine) — the wedge is driver lifecycle,
  consistent with the owner's "we own and mangle the fine-grained sequencing" framing.
  Both hypotheses stay open per the owner's note: co-op double-fire (a second body
  re-triggering the proc) vs a plain re-entrancy bug that would also repro solo.
**Next probes (cheap, in order):** (1) solo repro on denbus2 — separates the classes;
(2) stderr around the wedge: the `server dialog:` lines (game_dialog.cc:2085/2099) plus
whether `dialogEnd` was emitted at all; (3) if END was emitted but lost, look at the
`!_loaded`/backlog drop windows (client_net.cc:585-588, 678-691).
**Fix shape regardless of root:** the client dialog needs an unwedge — DIALOG_END is a
lifecycle signal and must not be droppable (force-flush is already the emit style,
presenter_network.cc:174 comment), plus a viewer-side watchdog (dialog open with no node
traffic and server says no dialog → close). That converts any future server-side dialog
bug from a softlock into a cosmetic blip.

## H. Cannot put items into a dead scorpion — NARROWED

Not a missing capability: the `put` verb exists and explicitly allows corpses
(server_control.cc:1722-1754, `isCorpse = … critterIsDead(container)`). First suspect is
already documented *in the verb itself*: the adjacency gate refuses with "not adjacent"
when the viewer draws the body somewhere it isn't (stale glide offset) — the in-source
comment records the owner hitting exactly this on `put` (server_control.cc:1755-1770).
Second candidate: item identity is by pid on the `put` path (1797-1808) — a pid missing
from the *server's* view of the dude inventory (vs the viewer's stale mirror, item M's
proto-default rebuild class) makes the verb a silent "no pid in source" drop (1809-1811).
Vanilla comparison unresolved: vanilla does allow planting into corpses via the loot
screen, but whether a *dead scorpion specifically* is lootable/plantable in the stock
client was not verified — do the stock-client check before treating any residue as ours.
The stderr tells (`control put …` lines) will name the branch on the next live repro.

---

## Incidental observations (one line each, per scope rule)
- The in-progress movie barrier can wedge if a movie fires inside the mapLoad suppression
  window (emit swallowed, ack never comes) — see L hazard (1).
- `onWorldmapEnd` not clearing `gPendingWorldmapEnter` (client_net.cc:2821-2827) is a
  latent latch race even outside item A's scenario.
- The viewer inventory-delta rebuild silently resets NPC weapon ammo to proto defaults
  (client_net.cc:1477-1482) — cited under M, but it also skews any client-side
  can-he-fire reasoning for remote actors.
