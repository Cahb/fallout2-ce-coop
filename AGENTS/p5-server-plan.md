---
name: p5-server-plan
description: "ACTIVE track — P5 dedicated server (f2_server) + SDL wire viewer. RESUME PROTOCOL at top, then the genuinely-open bugs, the banked forward plan, and the durable working-knowledge/open-risks. Finished slices live in git, not here ([[memory-doc-hygiene]])."
metadata:
  node_type: memory
  type: project
  originSessionId: 8b14f578-f7fb-4617-bcaf-af5cc7321c8a
  modified: 2026-07-19T05:33:37.644Z
---

The fixed-tick headless probe became a dedicated server (f2_server) that accepts commands and
streams a binary wire to a real SDL viewer client. Combat is playable with the mouse; the whole
Temple solo critical path has run LIVE over the wire EXCEPT dialog. The current active pivot is
[[dialog-streaming-track]] (the #1 remaining Temple blocker).

════════════════════════════════════════════════════════════════════════════════
►► RESUME PROTOCOL (fresh session, minimal bringup)
════════════════════════════════════════════════════════════════════════════════
1. Read THIS file. Skim [[mp-actor-architecture-principle]] (load-bearing) + IDEAS.md (vision/
   constraints) only if designing.
2. Docs of record IN REPO: MP_PROTOCOL.md (wire), CLIENT_JOIN_DESIGN.md (join/viewer),
   SERVER_LOOP_DESIGN.md, COMBAT_CLIENT_DESIGN.md (combat presentation), INTERACTION_UX_DESIGN.md
   (out-of-combat action UX), TEMPLE_DEMO_ROADMAP.md (north star). History = git log + [[p5-cut-list]].
3. Gate = scripts/check.sh → must print "ALL GATES PASS" (builds everything; legacy byte-identical
   + server goldens + netstream/netsocket/join/joining-client + resumable/wire-combat gates).
   ►► NEVER run golden harnesses concurrently, and NEVER run check.sh while a live viewer_live.sh
   server is up — the midjoin gate collides on the port and hangs; `pkill -x f2_server fallout2-ce`
   first.
4. Live demo = scripts/viewer_live.sh (server + SDL viewer + cmd port 9201; PACE=100 ≈ real time).
   Inject: `printf 'walk 8 1\n' | nc -q1 127.0.0.1 9201` (nc -q1 flaky here). Binaries must run
   from FO2/ (cwd-relative master.dat). FO2/ is gitignored.
5. CADENCE: batch hard, no per-commit review for benign/golden-invisible changes; adversarial
   review MANDATORY only for server_anim.cc (no headless oracle) + design-class / object-lifetime
   changes. See [[de-stub-cadence-rule]], [[model-choice-fable-vs-opus]].

All server milestones landed (git): all 155 maps serve (FRESH loads); joinable; N-viewers;
controllable client; resumable combat; combat presentation + vanilla mouse input; the vanilla
out-of-combat ACTION MENU (walk-then-act latch: use/usedoor/get/look/push/rot/skill/talk +
cancel); player-UI inventory (live dude inv, equip/unequip/drop, skill hotkeys, attack modes);
loot containers/corpses; out-of-combat useitem + C4 arm; automap severance (mid-run transitions no
longer abort). Combat presentation is driven by the record/replay channel ([[pres-record-build-track]],
[[combat-full-record-channel]]).

════════════════════════════════════════════════════════════════════════════════
►► OPEN BUGS (genuinely unresolved — instrument, don't blind-patch;
   [[visual-verification-protocol]] [[anim-decouple-verification]] [[dont-declare-not-a-bug-confidently]])
════════════════════════════════════════════════════════════════════════════════
• COMBAT-STATE DESYNC (the 62s hard-stop) — client sent `mv` (out-of-combat move) which the server
  dropped x20 "control mv dropped (in combat)" → the CLIENT believed combat was OVER while the
  SERVER stayed in combat on the dude's turn → the turn sat idle until the 62s resumable-combat
  idle timeout force-ended it (backstop WORKED, too slow for UX). Symptom = `_inCombat` went FALSE
  mid-dude-turn + self-healed at the next TURN_START. Prime suspect = a DEFERRED/premature
  COMBAT_EXIT applied while the server stayed in combat (client_net.cc onCombatExit queues behind
  the replay queue), OR a mid-combat rebaseline/transition. INSTRUMENT: the [cbtstate] trace is
  COMMITTED (ceafe9b) — a setInCombat(v,site) setter routes all 6 `_inCombat` writes and logs on a
  flip → `grep [cbtstate]` names the dropping site; correlate with the server's "mv dropped".
  Reproduce the hard-stop → the trace pins the site in one run. Remove the scaffolding once fixed.
• SKILL-USE gesture-replay FLOOD (root-caused, NOT fixed) — spam-lockpick an owned door → "stuck
  animation" + the walk-away sprite freezes + entering combat then waits the full ~62s. ROOT (recon
  2026-07-18, corrects the earlier "server latch re-fires" guess which was WRONG — the server latch
  is well-behaved): each lockpick CLICK emits a fresh EVENT_ACTION_ANIM gesture → client enterReplay
  resets replaySince UNCONDITIONALLY every call (client_present.cc:405) → the 2s replay cap
  (kReplayCapMs) NEVER expires while clicks continue → replay stuck Active → glideSuspended FREEZES
  the sprite AND a stuck-Active replay HOLDS the presentation drain → combatBusy LOCKS combat input
  (main.cc gates on !combatBusy) → the idle barrier waits ~62s. CRUCIAL: the client clears replays
  only on presReset (map/join), NOT on combat-enter → an out-of-combat gesture replay CARRIES INTO
  combat. Same 62s tail as the [cbtstate] desync but a DISTINCT root (two bugs, one tail — debug
  together). FIX CANDIDATES (confirm live, no oracle): (a) don't reset replaySince if the entry is
  already Active (kills flood-defeats-cap; also the banked empty-weapon-softlock fix); (b) reap
  Active gesture replays on combat-enter (like presReset). Built-in instrument: F2_TRACE_EVENTS →
  `[busy] STUCK combatAnim=? presBusy=?` every 2s once busy>3s; `combatAnim=1` persisting confirms it.
• #2 RUN-ANIM stand-slide OUT OF COMBAT — the client poses a locally-derived RUNNING fid for the
  glide but the server never animates so its authoritative fid stays STAND; a server
  OBJECT_DELTA_FID=STAND lands mid-glide → the walk tripwire → walkErase → critter slides in STAND
  pose + snaps hop-to-hop. OPEN LAST MILE: WHY the server emits a fid delta on a pure move
  (serverAnimStepOnce only touches location+rotation). INSTRUMENT object_delta.cc:155 (print
  before/after fid for the mover), don't static-read further. #4 glide "teleport" shares this root
  (per-hop walkErase makes movement choppy).
• #10 PERMANENT per-viewer glide-offset desync (OUT of combat, 2 viewers) — an NPC rendered a few
  hexes off its true tile on viewer A but CORRECT on B; server pos was right. A silent glide-drop /
  missed snap-forward leaves the sprite lagging and NEVER self-heals on A only. USER PROPOSAL
  (banked): a periodic PRESENTATION WATCHDOG in presAdvance — for each glide entry, if idle/stale
  (hops drained OR no progress > T ms) yet still holding a nonzero appliedX/Y offset, SNAP to the
  authoritative tile. Aligns with the FSM "failure direction = play/snap, never freeze". Instrument
  first (log net+offset+hops+age when it fires) to also NAME the silent-drop path. Don't snap a
  legitimately-active glide.
• server_anim SPATIAL-PROC gap → the TEMPLE floor TRAPS are inert over the wire: per-tile
  scriptsExecSpatialProc is NOT fired while walking (the walk streams as hop events; the spatial
  proc never runs). Hook point = the stepped-walk per-step loop in server_anim.cc. Was scoped
  "before free-roam/co-op"; the Temple demo pulls it earlier — a real Temple pass should trip traps.
• BANKED VISUAL NITS (user: don't care): per-hex AP dots on over-budget moves; aimed-shot 'N'/'B'
  aim selector wiring. The live viewer-bug backlog (gib corpse frame, gib-loot, NPC in-combat draw,
  throw-retrieve, walk/run desync) lives in [[presentation-viewer-bugs]].

════════════════════════════════════════════════════════════════════════════════
►► MAP-CHANGE + ELEVATION (open, demo-relevant)
════════════════════════════════════════════════════════════════════════════════
• MID-RUN MAP TRANSITION is DE-STUB-IN-PROGRESS, not done. "All 155 maps serve" = FRESH loads only.
  A mid-run transition hit a FATAL abort (client symbol automapSaveCurrent on the core-only server)
  → automapSaveCurrent + automapSetDisplayMap softened to benign no-ops (automap = pure client
  presentation); automapShow KEPT as a loud abort. ►► RE-RUN the all-map sweep (scripts/srv_sweep.sh,
  [[sweep-before-recon-lesson]]) to surface any NEXT unsevered-symbol domino past automapSaveCurrent.
• INTRA-MAP ELEVATION CHANGE (floor↔floor in one .map) is NOT WIRED — the dude changes elevation
  server-side and VANISHES from the viewer (still rendering the old floor). Three gaps: (1)
  OBJECT_DELTA carries fid/rot/flags/inv/hp/... but NOT tile OR elevation; (2) NO event fires on an
  intra-map elevation change (mapTransition is mapLoad-only); (3) gElevation is NEVER assigned in
  the viewer (only mapLoad sets it). FIX = stream the actor's elevation change (extend the delta, OR
  a small EVENT_ELEVATION, OR treat intra-map like a lightweight transition) + viewer calls
  mapSetElevation(myActor->elevation). This is the "gElevation follows the actor" work flagged for
  P2 co-op. ►► VERIFY EMPIRICALLY whether artemple.map uses intra-map elevation stairs or is
  single-floor + a map sequence (couldn't read .map metadata; load it and observe). IF multi-floor,
  elevation-follow is on the Temple critical path.
• ELEVATION ARCHITECTURE (Fable, durable — answers the P2-leash question): an elevation = a
  fully-resident floor of ONE loaded .map (≤3); mapLoad instantiates all elevations' objects;
  gElevation is only the CAMERA selection. THE PER-TICK SIM TICKS ALL ELEVATIONS (queue/critter_p_proc/
  scripts/movement/lighting are elevation-blind) → P1 on floor 0 + P2 on floor 1 are BOTH fully
  simulated; the wire already streams all elevations; each viewer owns its render gElevation. The
  ONE elevation wall = COMBAT (single global ctx / roster / off-elev start-guard / whole-world
  freeze) → two concurrent combats = a BOUNDED refactor (per-ctx struct + scope the freeze),
  tractable thanks to the resumable state machine. DIFFERENT MAP = the true wall (full teardown on
  mapLoad). P2-LEASH DECISION: grant SAME-MAP ANY-ELEVATION free-roam (nearly free — scrap the party
  sync-yank, make each viewer's gElevation follow its own actor, sweep the leak list
  obj_on_screen/_ai_search_environ/rest); "same elevation" only for COMBAT participation.

════════════════════════════════════════════════════════════════════════════════
►► BANKED FORWARD PLAN
════════════════════════════════════════════════════════════════════════════════
• ►► PRESENTATION SEAM — the answer to "stream actions? opcodes? script calls?" is NONE — seam at
  the ENGINE PRESENTATION PRIMITIVE layer. This is now the RECORD/REPLAY track
  ([[pres-record-build-track]]); design docs = PRESENTATION_RECORD_REPLAY_SPEC.md (Appendix A holds
  the folded-in seam rationale/lineage) + PRESENTATION_RECORD_REPLAY_COOKBOOK.md + PRESENTATION_FSM_DESIGN.md
  (the client_present glide subsystem). Remaining seam work is the
  script-presentation opcode pass (op_play_sfx/op_float_msg/op_anim/…) routing through the presenter
  once each (bounded, closed opcode world) + bucket-C script cutscenes — captured in the record/replay
  scope. The presenter vocabulary already exists (sfxPlay/floatText/consoleMessage/musicPlay… → wire
  EVENT_SFX/CONSOLE/FLOAT_TEXT already carried).
• STEAL — PARKED as "loot slice v2". ~90% is the committed loot modal + a _gIsSteal flag;
  request-driven like dialog (server installs the BASE handler whose stealing()/looting() are EMPTY
  no-ops → a steal request server-side is a SAFE no-op today, NOT a hang; the client handler opens
  the modal). The real 10% (hard): a SERVER-AUTHORITATIVE steal roll (skillGetValue(STEAL) vs
  catchChance) + "caught → target hostile → combat", run ON THE SERVER (RNG + hostility must be
  authoritative) = a new steal/plant verb pair replicating the _gIsSteal branch server-side +
  lootStealExperience XP. Object-lifetime + RNG-authoritative → adversarial review. Slot after dialog.
• CLOSED CONTAINERS (ice boxes/footlockers/desks) "do nothing" — need a server-side CONTAINER-OPEN
  decouple (=_obj_use_container welded to a reg_anim callback that no-ops headless, SAME class as the
  door/pickup decouples): a serverLoopActive() branch that sets the open frame + OBJECT_OPEN + "You
  search the %s" and streams via OBJECT_DELTA, wired into the kInteractLoot fire (open-if-closed).
  Core EASY (door template); off the Temple critical path (lockpick/C4 avoid loot).
• C4 boom SFX + explosion cloud are absent over the wire (welded to the skipped reg_anim); the
  record/replay explosion channel is the real home. The Temple DOOR destroy via C4 (script
  SCRIPT_PROC_DAMAGE→destroy_object→EVENT_DESTROY, not HP→0) is STILL UNVERIFIED — drop an armed
  charge next to the actual Temple door and confirm EVENT_DESTROY.
• ADMIN / DEBUG-CONSOLE VERBS (idea): over the EXISTING debug cmd port (~9201, server-local, not
  claim-gated). `inspect <netId>` (dump all object state to FO2/debug.log, drivable by ANY client —
  a live-debugging affordance), `destroy <netId>`, `magickey` (unlock-any). Cheap extension of
  command.cc's give/aggro/probe pattern. If pursued, target a STABLE object address (netId is
  re-minted on rebaseline — see the objectAssignAllNetIds lesson; may want a pid+id snapshot or name
  lookup).
• MID-JOIN UX etiquette — a join briefly holds the sim + announces to every player (pipboy/console).
  Server-side trivial (hold beats N ms around the rebaseline + a presenter consoleMessage broadcast).
  Console rendering is done (EVENT_CONSOLE), so the announce rides it.
• GOLDEN-TEMPLE CAPSTONE (idea) — a headless harness driving a full scripted Temple solo-pass as a
  regression gate. CAVEAT: the Temple is AI/RNG/wall-clock-heavy → NONDETERMINISTIC → CANNOT be a
  byte-exact reconstruction golden; must be a LIVENESS test ("the adaptive scripted pass REACHES the
  exit grid without crash/softlock/abort"). Would have auto-caught both the automap crash and the 62s
  softlock. A CAPSTONE (needs combat+doors+C4+dialog+transitions working first), not the next step.

════════════════════════════════════════════════════════════════════════════════
►► MODAL-LOOP SAFEGUARD (durable design rule)
════════════════════════════════════════════════════════════════════════════════
Every vanilla modal screen (inventory/skilldex/char/pipboy/loot/dialog/barter) runs its OWN blocking
input/render loop → on the viewer it STARVES conn.pump() → a server COMBAT_ENTER mid-modal is missed.
So EVERY modal MUST pump the wire inside its loop and BAIL (force-close) on COMBAT_ENTER / deferred-
rebaseline / disconnect. MECHANISM = a per-iteration ticker (clientViewer service ticker via
tickersAdd) self-gated to kViewerModalMask; inside a modal it pump()s the wire and
enqueueInputEvent(KEY_ESCAPE) to force-close (ESC breaks all vanilla modals at their top-of-loop
check). BLOB-DEFER: if a viewer modal is open, buffer an incoming blob and apply it AFTER the modal
closes (applyBlob→mapLoad→_obj_remove_all would free gDude under the screen's static _pud/_stack →
crash). Adding a new modal = add its GameMode to kViewerModalMask or the wire STALLS if it opens.

════════════════════════════════════════════════════════════════════════════════
►► KNOWN GAPS (S2, v1-acceptable)
════════════════════════════════════════════════════════════════════════════════
Ammo/charges + NESTED container contents not streamed (OBJECT_DELTA_INVENTORY is top-level-only →
viewer can't show gun ammo counts / disable fire-on-empty; the "A2" encoder-side gap). Game GVARs /
worldmap pos / map LVARs not streamed (needed for a controlling or mid-game-join client). sid
binding on combat corpses. NO_SAVE objs spawned post-rebaseline ride objectCreated with a nonzero
netId outside the syncable domain (decoder policy when it matters).

════════════════════════════════════════════════════════════════════════════════
DURABLE LESSONS / WORKING KNOWLEDGE (quirks — keep)
════════════════════════════════════════════════════════════════════════════════
• ORACLE RULE: validate binary reconstruction vs **state_dump** (independent of the seam), never
  binary-vs-narrate (shared omission passes silently). THE PINNED PROFILE IS THE TEETH, not
  replay.py's exit code (twice-proven: netId recycle + same-index reload both "passed" rc=0 while
  lying). Don't weaken gates to rc==0.
• f2_server is NONDETERMINISTIC run-to-run on AI-heavy maps (no F2_FAKE_CLOCK; wall-clock steers AI).
  Deterministic+clean: artemple/kladwtwn/newr1/vault13/arcaves/modmain. Transport gates must be
  SAME-RUN TEE (F2_SERVER_NET_TEE), never two-runs-compare.
• Presenter recipe (7×): base no-op virtual → pure-read hook at choke point → narrate override →
  byte-identical check → F2_NARRATE firing golden → adversarial review. NetworkPresenter must
  BUFFER+FLUSH (never mutate the object list in a callback) + honor presenterEmissionsSuppressed().
  Install fns that can fail must return bool (a void early-return silently leaves the wrong presenter
  installed — measured 2× wall-clock).
• grep -a MANDATORY on golden/narrate output: .msg text = raw high-ASCII codepage (NOT UTF-8) → GNU
  grep declares the pipe binary. Free-text wire channels are raw codepage bytes.
• Live-debug: nothing on stdout — DEBUGACTIVE=log → FO2/debug.log (NDEBUG compiles out the stderr
  fallback), OR fprintf(stderr) direct (a trace beats a stack of static hypotheses). Wayland: use
  the in-engine takeScreenshot (scrNNNNN.bmp in FO2/, via F2_VIEWER_SHOT_EVERY); X `import` can't
  grab. Multi-line inline Bash gets newline-flattened → write .sh FILES.
• netId RE-MINT: objectAssignAllNetIds re-mints netIds SEQUENTIALLY on every rebaseline (incl. a 2nd
  viewer joining the same map) — a stale netId does NOT go dead, it resolves to a DIFFERENT object.
  Any server-side structure holding a netId across a rebaseline MUST snapshot id+pid and drop on
  mismatch, or clear on rebaseline (presReset pairs with the renumber). [[dont-declare-not-a-bug-confidently]].
• Engine singletons (one gMap / object list / combat ctx) are THE roadmap wall for multi-map/parallel
  — vanilla-inherent, don't contort v1 around them.
• Sweep-before-recon: run the thing on ALL inputs, let aborts name the work. Ask "does the CLIENT do
  this too?" to separate pre-existing bugs from regressions. [[sweep-before-recon-lesson]].
• ►► SCRIPT RE-ENTRANCY is server_anim's recurring hazard class: _obj_use_door (and any scriptExecProc
  path) runs scripts SYNCHRONOUSLY that can cancel/register anims or destroy the very object being
  moved — never hold a reference/index into a registry across such a call (epoch-counter guard) and
  never touch an Object* after one without re-proving liveness. objectDestroy does NOT clear anims;
  pointer-identity scans are blind to recycled allocations (snapshot id/pid).
• ►► For viewer presentation with no headless oracle: read the regressing COMMIT diff first, and
  instrument (stderr trace) over stacking static hypotheses. See [[frame-index-render-gotcha]].

════════════════════════════════════════════════════════════════════════════════
OPEN RISKS (still open; size before the relevant slice)
════════════════════════════════════════════════════════════════════════════════
• SINGLE gDude identity — co-op = de-singleton the actor ([[mp-actor-architecture-principle]]).
  gDude-relative UI computations that must resolve through the per-client gDude-ROLE (else P2's
  viewer shows P1's view): combat OUTLINES (#8 — LOS/team/skip-self all gDude-keyed), crosshair
  target resolution, AP/HP bars, myTurn keying. All vanilla-faithful today; the fix is de-singletoning
  gDude, NOT rewriting these — do not add NEW gDude-singleton assumptions.
• MODAL LOOPS block the tick (dialog/barter drain synchronously) — see the MODAL-LOOP SAFEGUARD rule.

════════════════════════════════════════════════════════════════════════════════
►► NORTH STAR = TEMPLE_DEMO_ROADMAP.md
════════════════════════════════════════════════════════════════════════════════
Demo v1 = SOLO pass the Temple of Trials; then P2 co-op. The remaining solo blocker is DIALOG OPTION
STREAMING ([[dialog-streaming-track]] — the dialog ENGINE works headless, the gap is the viewer
render+input+option streaming). P2 = an INDEPENDENT manually-driven free-roam actor on the SAME map
(the cheap co-op case — respects the one-object-list/one-map singleton wall; separate maps = the
wall); leash/party SCRAPPED ([[mp-actor-architecture-principle]]). Needs a 2nd controllable dude
actor + 2nd claimant binding (N-viewers + control gate exist; gDude→per-client role partially done).
See [[post-demo-hardening]] (the 3 recurring taxes; stay feature-first to Demo v1).
</content>
