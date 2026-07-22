# COMBAT_CLIENT_DESIGN.md — P3: vanilla combat presentation on the wire viewer

STATUS: ACTIVE. §1–5 are the architecture (reference); §6 is the staged plan.
Slices V0→S5 + **S3 (vanilla mouse input)** + **weapon-draw** + **door open/close** are
done (git history). Combat is playable with the mouse: crosshair click-to-attack (real
_combat_attack_this + commit fork), SPACE/RETURN end-turn/end-combat, right-click
MOVE↔CROSSHAIR, cmove; PLUS `cstart` player-initiated combat start. NPCs draw weapons
mid-fight (server-authoritative armed fid) and doors slide open with crossers waiting.
NEXT = **interaction-verbs** (ARROW mode + action menu, recon done — §6, cheapest =
LOOK), then remaining **streaming-events fidelity** (script-spawned pop-in). Open bug:
**random combat-glide teleports** (separate from equip — §6). Details in §5 and §6.

Design of record for the P3 combat *presentation* correction: how the SDL wire
viewer gets real, playable, vanilla-Fallout-2 combat — the interface bar, the
targeting crosshair, attack/hit/death animations, damage feedback — **driven by
server events and requesting through server verbs**, reusing the combat UI code
already compiled into the viewer binary instead of the bespoke A/E-key UX that
derailed.

Read alongside: `MP_PROTOCOL.md` (§2 combat events, §7d wire, §8 control plane),
`SERVER_LOOP_DESIGN.md` §3 (resumable session machine), `CLIENT_JOIN_DESIGN.md`
§E (puppet suppression), memories `[[p5-server-plan]]`,
`[[mp-actor-architecture-principle]]`, `[[visual-verification-protocol]]`.

--------------------------------------------------------------------------------
## 1. Current state — the data plane is DONE; the presentation plane is empty

**What works (proven end to end, gates 9+10 green):**
- Server-authoritative combat runs over the network. The resumable session
  machine (`F2_SERVER_RESUMABLE_COMBAT`, combat.cc `combatSessionAdvance`) spans
  a fight across beats; the dude's turn is a barrier that waits on the connected
  wire claimant, with a sim-ms idle budget (`F2_SERVER_TURN_IDLE_MS`).
- The wire already carries the full combat truth:
  - `EVENT_COMBAT_ENTER(12)` / `EVENT_COMBAT_EXIT(13)` — fight framing.
  - `EVENT_TURN_START(14)` — netId + isPlayer + apAvailable + deadlineMs.
  - `EVENT_ATTACK_RESULT(15)` — the full causal envelope: attacker, defender,
    hitMode, hitLocation, both damages, both flag words, the whole extras[]
    (multi-victim) set (presenter_network.cc:419). PRESENTATION-tagged;
    all state it implies rides `OBJECT_DELTA`.
  - `OBJECT_DELTA` — hp / ap / combat.results / fid / flags / inventory
    (incl. ammo) per changed object per beat. `MOVE` — one authoritative hop.
  - Console / floatText / sfx presentation cues (combat messages included:
    `_combat_display` output crosses the presenter and is on the wire).
- Claim-gated wire verbs exist (server_control.cc): `cattack [target]` /
  `cmove <tile>` / `cendturn` push `COMBAT_INTENT_*` onto the queue the player
  barrier drains (`combatServerPumpIntents`, combat_drain.cc). Gate 10 proves
  claim + cattack over the wire → dude `ATTACK_RESULT`.
- client_net.cc decodes combat framing into `inCombat()` / `myTurn()`.

**What the viewer suppresses (§E puppet, main.cc `mainClientViewer`):**
`scriptsDisable()`, `tickersRemove(_object_animate/_dude_fidget)`, gmouse pinned
to `GAME_MOUSE_MODE_MOVE`, no combat driver, presentation events
(console/float/sfx/attackResult) decoded as *skip* (client_net.cc's event switch
doesn't even list `EVENT_ATTACK_RESULT`). Result: combat is fully simulated and
fully streamed but **invisible and unplayable-looking** — no HUD, no anims,
teleporting in-combat movement, HP dropping silently, a bespoke 'A' key as the
only attack.

--------------------------------------------------------------------------------
## 2. What derailed and why it's wrong

P3 started growing a **bespoke combat UX**: 'A' = attack nearest, 'E'/Enter =
end turn, click-in-pinned-MOVE = cmove, plus a floated idea of an invented
on-screen text HUD. Rejected (user, 2026-07-16), and rightly:

- **All of that UI already exists, compiled into the viewer binary.** The AP
  dots (`interfaceRenderActionPoints`), the end-turn/end-combat buttons
  (`interfaceBarEndButtonsShow`, buttons that post keycodes 32/13 — i.e. the
  vanilla SPACE/RETURN combat keys), the targeting crosshair with hover to-hit%
  (`GAME_MOUSE_MODE_CROSSHAIR`, game_mouse.cc), the called-shot picker
  (`calledShotSelectHitLocationClient`), attack/dodge/hit/death animation
  choreography (`_action_attack` → `_action_melee`/`_action_ranged`,
  actions.cc), the combat message log (display monitor). Reinventing any of it
  is throwaway work that also *diverges the look* — the game IS Fallout 2 and
  must play like it.
- **The principle:** reuse vanilla presentation and input; never invent a
  parallel UX. The wire verbs are *plumbing under* the vanilla UI (the request
  encoding), not the interface the player touches.
- The bespoke keys also violated vanilla semantics ('E'/Enter conflated "end
  turn" with vanilla RETURN = *attempt end combat*).

What did NOT derail: the substrate. Verbs, intent queue, barrier, decode — all
correct plumbing. §4 keeps them.

--------------------------------------------------------------------------------
## 3. Corrected architecture — vanilla client presentation + input, server-controlled

```
   vanilla input widgets ──(pure-read validation on mirrored world)──▶ wire VERB
        ▲                                                               │
        │ vanilla UI calls                                              ▼
   thin combat-context MIRROR ◀──(decode)── wire EVENTS ◀── authoritative server sim
```

**Locked:** the server runs the one authoritative combat sim (engine singletons,
determinism, anti-cheat; the whole P5 track rests on it). The client runs **no
combat state machine** — not `_combat()`, not `_combat_turn`, not AI, not
damage application, not RNG. This is non-negotiable and this design never
violates it.

**The crux question — pure event-driven vs local mirror:** the vanilla combat
widgets are deeply coupled to local globals, so a *purely* stateless
presentation cannot light them up. But the coupling surface is small and
**entirely pure-read**. The answer is a **thin combat-context mirror**: the
decoder writes a handful of real engine globals from wire events, and every
vanilla widget then works unmodified. The mirror is *derived state, never
advanced locally* — no transition happens except in response to a wire event.

### 3.0 The mirror — exhaustive inventory (this is all of it)

| vanilla state read by UI/input | mirrored from | who writes it on the viewer |
|---|---|---|
| `gCombatState & 0x01` (in combat) — `isInCombat()` | COMBAT_ENTER / COMBAT_EXIT / MAP_TRANSITION (and: any TURN_START implies in-combat — covers the mid-fight joiner who missed COMBAT_ENTER) | decoder |
| `gCombatState & 0x02` (my turn is live) | TURN_START(isPlayer && netId == my actor) sets; TURN_START(other) / COMBAT_EXIT clears | decoder |
| `gDude->data.critter.combat.ap` (AP dots, path preview, bad-shot check) | already mirrored — `OBJECT_DELTA_AP` (client_net.cc:375 applies it today) | decoder (existing) |
| `_combat_free_move` (bonus-move dots, move budget) | append `freeMove` to TURN_START payload (length-prefixed events make appends forward/backward safe) — v1 fallback: 0 | decoder |
| dude hp / wielded weapon / ammo (bar numbers, item buttons, hit-mode) | already mirrored — OBJECT_DELTA hp/flags/INVENTORY rebuild | decoder (existing) |
| whose-turn cue (green/red lights, watch cursor) | TURN_START.isPlayer + netId | decoder → UI calls |
| active hand / aiming toggle (`interfaceGetCurrentHitMode`) | **client-local UI state** — not sim state; stays local, rides the verb upstream | player |

Nothing else. `_combat_list` (the roster) is deliberately NOT mirrored in v1 —
the only consumer we lose is the red target-outline highlight (§5, banked).

The decoder writes the *real* globals (`gCombatState`), not shadow copies, so
`_combat_attack_this`'s `(gCombatState & 0x02)` guard, game_mouse's in-combat
AP-limited path preview, and `gameMouseCycleMode`'s crosshair unlock all work
untouched. The viewer never calls anything that *advances* combat, so the
globals can only change under decode — the mirror cannot drift.

### 3.a Interface bar: AP display + end-turn/end-combat buttons

The bar (`gInterfaceBarWindow`) is created by `interfaceInit` (isoInit) and
shown by mapLoad's `hudBarShow` (map.cc:855) during the blob load — it is
already in the binary and (to verify, slice V0) on screen. The wire deliberately
does NOT carry `hud*` events (dropped client-local chrome, MP_PROTOCOL §7d) —
the viewer *derives* the same calls ClientPresenter makes on the SP path,
directly from decode hooks:

- COMBAT_ENTER → `interfaceBarEndButtonsShow(true)` + red lights + local combat
  music start (musicPlayLevel is client-driven by design).
- TURN_START(mine) → `interfaceRenderActionPoints(ap, freeMove)` +
  `interfaceBarEndButtonsRenderGreenLights()` + normal cursor.
- TURN_START(other) → red lights + `MOUSE_CURSOR_WAIT_WATCH` +
  `interfaceRenderActionPoints(-1, -1)` (vanilla's out-of-turn blank).
- OBJECT_DELTA(dude, ap) while my turn → re-render AP dots (attacks/moves
  charge AP server-side; the delta arrives same beat).
- OBJECT_DELTA(dude, hp) → `interfaceRenderHitPoints(true)`.
- COMBAT_EXIT → `interfaceBarEndButtonsHide(true)`, AP blank, music restore.

**Input falls out for free:** the end-turn button posts keycode **32** (SPACE)
and the end-combat button posts **13** (RETURN) into the normal input queue
(interface.cc:1903/1955 region) — exactly the vanilla keyboard keys. The viewer
frame loop already reads `inputGetInput()`; it maps, gated on the mirror:
- `32` (button or SPACE), when myTurn → send `cendturn`.
- `13` (button or RETURN), when myTurn → send `cendcombat` (NEW verb, §3.f —
  vanilla RETURN semantics = *attempt* end combat, server-validated).

No bespoke widget, no bespoke key.

### 3.b Targeting crosshair + click-to-attack

Vanilla flow, reused wholesale; ONE fork at the commit point.

- **Mode unlock:** stop pinning MOVE while in combat. Right-click cycles modes
  via vanilla `gameMouseCycleMode` — viewer v1 cycles **MOVE ↔ CROSSHAIR only**
  (skip ARROW until the banked interaction-verbs slice lands; a one-line viewer
  cycle wrapper, not a game_mouse.cc edit). Out of combat the pin stays (ARROW
  isn't wired yet).
- **Hover to-hit %:** the `gameMouseRefresh` ticker (already running) draws the
  crosshair cursor + hit chance in CROSSHAIR mode via `attackDetermineToHit` —
  a pure function of dude + target mirrored state. Zero work. (Fidelity caveat:
  PC skills/perks aren't in the dude blob — the % is approximate until the
  PC-data block ships, §5. The server's roll is authoritative regardless.)
- **Click:** the frame loop keeps its capture-don't-dispatch discipline (never
  `_gmouse_handle_event` — its ARROW branch mutates sim). On left-up in
  CROSSHAIR mode it runs the vanilla selection
  (`gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, …)`) and calls the real
  **`_combat_attack_this(target)`** (combat.cc:5648). Everything upstream of
  the commit is pure-read UX we want verbatim: the `gCombatState & 0x02` guard,
  `interfaceGetCurrentHitMode` (bar's hand + aiming toggle),
  `_combat_check_bad_shot` with its vanilla out-of-ammo/out-of-range/need-N-AP
  console messages and sfx, and the called-shot picker
  (`calledShotSelectHitLocationClient` — modal, local, pure).
- **The fork (the one shared-code edit):** where `_combat_attack_this` would
  call `_combat_attack(gDude, target, hitMode, hitLocation)` — the line where
  simulation begins — a `clientViewerActive()` branch instead sends
  **`cattack <netId> <hitMode> <hitLocation>`** and returns. Golden-invisible
  by construction (the flag is set only in `mainClientViewer`), same pattern as
  the existing `serverLoopActive()` forks. The `!isInCombat()` combat-*starting*
  branch of `_combat_attack_this` is unreachable on the viewer in v1 (crosshair
  only cycles in combat); player-initiated combat entry is a banked verb (§6 S6).
- **Server re-validates everything** (`_combat_check_bad_shot` again inside the
  drain, claim gate, turn barrier). Client-side validation is UX nicety only —
  the trust boundary stays at server_control.cc.
- **In-combat move click** (MOVE mode): vanilla path preview with AP cost
  numbers lights up automatically once `isInCombat()` mirrors true (the
  gameMouseRefresh ticker draws it from `ap + _combat_free_move`). The click
  sends `cmove <tile>` exactly as today.

### 3.c Attack / hit / death animations — replay `_action_attack` from ATTACK_RESULT

The vanilla attack choreography (`_action_attack` → `_action_melee` /
`_action_ranged`, actions.cc:680) is *driven entirely by the contents of the
`Attack` struct* — attacker, defender, hitMode, DAM_HIT/DAM_DEAD/knockback
flags, damages, extras — and `EVENT_ATTACK_RESULT` carries every one of those
fields. So the viewer can replay the real animation code:

- **New `src/client_combat_anim.{h,cc}` (f2_client, sibling of client_anim):**
  decode ATTACK_RESULT → reconstruct an `Attack` (netId→Object* for
  attacker/defender/extras; `weapon = critterGetWeaponForHitMode(attacker,
  hitMode)` from the mirrored inventory; damages/flags/hitLocation verbatim) →
  call `_action_attack(&attack)`. Melee swing, ranged fire + projectile,
  dodge-vs-hit branch, sfx registration, and the death animation
  (`_show_damage_to_object` registers `_show_death` off `DAM_DEAD` in the
  flags) all play with vanilla timing from vanilla code.
- **Why this is structurally double-apply-safe:** the damage-application path
  (`_combat_anim_finished` → `_combat_apply_attack_results`, combat.cc:5452) is
  gated on `_combat_cleanup_enabled`, which only `_combat_attack` sets — and the
  viewer never calls `_combat_attack`. Sequence completion on the viewer is a
  guaranteed no-op for ammo/damage/XP/end-combat. HP/ammo/death *state* arrives
  via OBJECT_DELTA as always.
- **Anim advance:** the puppet removed the `_object_animate` ticker. Rather than
  re-adding it (a standing lever in three re-puppet sites), the viewer loop
  calls `_object_animate()` explicitly each frame next to `clientAnimAdvance()`
  — same effect, explicit ordering, and the §E "tickers removed" invariant
  stays literally true. Safety argument: with scripts disabled and no local
  action paths, the ONLY registrar of reg_anim sequences is this replay driver,
  so the ticker body has nothing else to animate. (`_dude_fidget` stays
  removed.)
- **Ordering vs authoritative state (the one real subtlety):** within a beat
  frame, ATTACK_RESULT is emitted *before* the beat-end delta scan, but the
  decoder applies the frame atomically — by the time the replay starts, the
  defender may already wear its corpse fid/flags. Mirror of the client_anim
  doctrine (*store is authoritative-first; render may lag; authority wins at
  the end*): the replay driver **defers applying fid/flags deltas** for the
  attack's participants (records them as pending-final-state) until the replay
  completes, then applies them verbatim. Tripwires force immediate apply +
  replay cancel: MAP_TRANSITION/rebaseline, a new ATTACK_RESULT touching the
  same object, a MOVE on the defender, or a wall-clock cap (~2s). hp/ap deltas
  apply instantly always (numbers never wait on pixels).
- **Deliberate v1 exclusions:**
  - *Knockback:* pass `knockbackDistance = 0` into the replay. The server
    applied knockback authoritatively (`_combat_apply_knockback`) and the tile
    change rides MOVE — replaying the knockdown-move animation would fight it.
    Corpse/prone position snaps; smooth knockback is post-v1 (this is the
    already-banked multi-victim-knockback gap, holistic-audit memory).
  - *AI chatter:* `_combatai_msg` inside `_action_melee/_action_ranged` must be
    suppressed under `clientViewerActive()` (the server already streams the
    real taunts as floatText; replaying would double them and read AI packets
    the mirror may not faithfully hold).
  - *Projectile transients* are local netId-0 objects created/destroyed inside
    the sequence — invisible to the decoder's lookup (0 = "no object"), no
    domain pollution; the cancel path must run the sequence to completion or
    destroy them.

**S4 RECON CONFIRMED (2026-07-16) — implementation-ready facts:**
- *Wire payload* (`EVENT_ATTACK_RESULT`=15, presenter_network.cc:419), in order:
  i32 attackerNetId, i32 defenderNetId, i32 hitMode, i32 defenderHitLocation,
  i32 defenderDamage, i32 defenderFlags, i32 attackerDamage, i32 attackerFlags,
  u16 extrasLength, then per extra { i32 netId, i32 damage, i32 flags }. Event
  framing `[u8 type][u8 flags][u16 len][payload]`. `netIdOf(nullptr)`=sentinel.
- *`Attack` reconstruction* (combat_defs.h:102, EXPLOSION_TARGET_COUNT=6):
  resolve netIds→mirror Object*; `weapon = critterGetWeaponForHitMode(attacker,
  hitMode)` (item.cc:1006, reads mirrored inventory); WIRE fills hitMode/
  defenderHitLocation/defender+attacker damage&flags/extras; DEFAULT the rest —
  `attackHitLocation`, `ammoQuantity`, `criticalMessageId`, `tile`(-1 or
  defender->tile), `oops`(null), `extrasHitLocation[]`, and CRUCIALLY
  `defenderKnockback=0` + `extrasKnockback[]=0`. DAM_ flags: DAM_DEAD=0x80,
  DAM_HIT=0x100, DAM_KNOCKED_DOWN=0x02 (obj_types.h:126).
- *Double-apply safety HOLDS, no hole* (agent-verified): `_combat_apply_attack_
  results` (combat.cc:5454) — the ONLY damage/ammo/XP/critterKill/attackResult/
  end-combat path — is gated on `_combat_cleanup_enabled`, armed ONLY in
  `_combat_attack` (combat.cc:4078), which the viewer never calls. `_combat_anim_
  finished`→`_combat_apply_attack_results(true)` (combat.cc:5440) hits the gate
  and returns. Guaranteed no-op.
- *`clientViewerActive()` guard points* (shared-file, golden-invisible forks):
  AI chatter `_combatai_msg` at actions.cc:737/783/786 (melee) + 841/1025/1028
  (ranged). Knockback needs NO code guard (data-side 0 + the 3 `actionKnockdown`
  sites self-gate on `!=0`). Death anim (`_show_death` off DAM_DEAD, actions.cc:
  573/578) + projectile transients (id -1, self-disposed) replay safe as-is.
- *NEW caveat NOT in the original design:* the `ANIM_THROW_ANIM` branch mutates
  LOCAL INVENTORY synchronously (actions.cc:869-870 `itemRemove`/`itemReplace` +
  `hudItems` at 882) DURING sequence-build — OUTSIDE the cleanup gate. Replaying
  a thrown-weapon (grenade/knife/spear) Attack would consume the item from the
  mirror. Needs its own `clientViewerActive()` guard (or skip the swap and let
  the server's inventory delta carry it). Must handle before S4 ships throws.

### 3.d In-combat movement gliding

Today in-combat moves teleport: the turn drains synchronously within one beat,
every hop's MOVE carries `durMs=0` (= snap, the pinned discriminator). Fix on
the **producer** side — never synthesize durations client-side (that would
falsely animate scripted teleports and violates the §7d "duration slaved to
wire" rule):

- **Server:** stamp `durMs` (kWalkMsPerTile 200 / run 100) on per-step combat
  moves via the existing `presenterSetNextMoveDurationMs` bracket (the seam
  @d85951a built), in the in-combat register/drain path — **gated on
  `F2_SERVER_RESUMABLE_COMBAT`** so every golden (gate off) stays
  byte-identical. AI critters' combat moves get stamped too — the whole fight
  glides, not just the dude.
- **Viewer:** the client_anim hop queue currently snaps when >4 hops queue up
  (the "behind = catch up" heuristic). A combat move arrives as one same-frame
  burst of stamped hops — teach the queue that a burst of *stamped* hops in one
  frame is choreography, not lag: play sequentially up to a cap (~16 tiles ≈
  max-AP + Bonus-Move, `kMaxBurstHops`), then snap. The lag heuristic keeps
  applying to everything else (cross-`gAdvanceGen` trickle arrivals → cap 4).

**S5 SEQUENCING (implemented; extends the recon) — the glide is a
COMBAT-PRESENTATION lane event, not a STATE-lane one.** The bug the naive fix
exposed: the server resolves combat fast — spending all AP auto-ends your turn,
so the enemy's whole turn dispatches in the next beat(s). MOVE's *authoritative
tile* is STATE (applied on decode, always), but its *glide* (pixels over durMs)
is presentation and must ride the paced queue like attacks — else the enemy's
approach glides ON TOP of your still-gliding move ("cat and mouse"). So movement
joins the presentation lane:
- **Held glides** (client_anim.cc): in combat, `clientAnimOnMove(..., hold=true)`
  queues a move's hops WITHOUT starting playback (a per-walk `heldHops` tail
  budget); the sprite parks at its origin while the authoritative tile is already
  the destination.
- **Queued release** (client_net.cc): a `PresKind::kMoveRelease` (coalesced per
  mover) rides the queue in wire order; the pump calls `clientAnimRelease` to
  start the glide when it reaches that point. So a future turn's approach cannot
  glide before its own TURN_START.
- **Blocking rules** (presentationPump): a `kTurnStart`/`kExit` waits while ANY
  walk is *playable*-gliding (`clientAnimAnyPlayableActive` — the outgoing actor
  finishes moving before the AP dots flip / doors close); a `kAttack` waits while
  its participants are *playable*-gliding (`clientAnimPlayableActiveFor` — the
  attacker finishes approaching before it swings). "Playable" excludes still-held
  hops so an attack never deadlocks on a segment it precedes.
- **Anti-wedge (doubly bounded, always fails toward snap/play, never freeze):**
  (1) the pump's self-heal — with the queue empty and no attack animating, a held
  glide has nothing to sequence against, so `clientAnimReleaseAll()` frees every
  hold each such frame; (2) a `kHeldCapMs` (5 s) starvation cap in the advance
  loop snaps a never-released walk forward; (3) glides self-terminate by wall
  clock (≤ `kMaxBurstHops`×durMs). Holds die with their walks on rebaseline/map
  transition (walks reset before the queue clears), and a dropped `kMoveRelease`
  cancels its walk (snap). Tripwires stay armed on held walks — authority always
  wins; the store is never wrong, only the pixels are late.
- **Migration seam:** the presentation queue IS the seam to the banked
  server-action-pacing / per-action-HP work — when the producer stops bursting,
  events trickle, holds go ~empty, releases fire immediately; nothing here is
  torn down (it degrades to a thin jitter absorber the locked cadence requires
  anyway). Rejected alternative: server ack-paced dispatch (overturns the LOCKED
  never-gate-on-client-ACKs cadence, MP_PROTOCOL §1 / decision #5; multi-viewer
  ack-aggregation is a subsystem bought to fix a viewer-local ordering problem).

### 3.e Damage / HP feedback

All vanilla, all already on the wire, just not decoded:

- `EVENT_CONSOLE` → `displayMonitorAddMessage` — the interface bar's message
  window IS vanilla combat feedback ("Gecko was hit for 7 points…" — the
  server's `_combat_display` output is already streaming).
- `EVENT_FLOAT` → `textObjectAdd` (floating combat text over critters, raw
  codepage bytes per §7d).
- `EVENT_SFX` / `EVENT_SFX_AT` → `soundPlayFile` (weapon fire, hit grunts,
  death screams — the server emits them; the replay driver also registers its
  own anim-synced sfx: accept the near-duplicate in v1, then prefer the
  anim-synced one and drop wire-sfx for attack participants as polish).
- Dude HP number: §3.a's delta hook. No floating damage numbers, no enemy HP
  bars — vanilla has none and we do not invent presentation.

### 3.f Server-side deltas this design needs (all small)

1. **BUG FIX — target addressing:** `serverResolveTarget` (combat_drain.cc:112)
   matches `obj->id`, which is measured ~53% NON-UNIQUE (MP_PROTOCOL §7). The
   client knows objects by netId. `cattack` target moves to **netId**;
   resolution walks by netId. (Today's nearest-hostile fallback stays for the
   debug port.)
2. **`cattack` carries `hitMode` + `hitLocation`;** `CombatIntent` gains a
   `hitMode` field (it has hitLocation already). Server validates via
   `_combat_check_bad_shot` exactly as now — a bogus hitMode is rejected, the
   bar's hand/aim selection is honored when legal.
3. **New verb `cendcombat`** → `COMBAT_INTENT_END_COMBAT` → the pump calls
   `combatAttemptEnd()` and *continues pumping* (vanilla RETURN semantics: the
   hostiles check may refuse with the vanilla console message, which streams
   back).
4. **TURN_START payload += `freeMove`** (append; length-prefixed events make
   this compatible both directions).
5. **durMs stamp on in-combat steps** (§3.d, gated).
6. (S6, banked) out-of-combat `cattack` = player-initiated combat entry: build
   the `CombatStartData` and enter the session, claimant-only.

Per `[[mp-actor-architecture-principle]]`: every verb stays
actor-parameterized (session→actor binding resolves to gDude in v1); the
decoder's myTurn stays keyed on `turnStart.netId == my actor's netId` (already
true — never on `isPlayer` alone, which will be true for *other* players'
turns too); nothing branches on party membership.

--------------------------------------------------------------------------------
## 4. Current P3 work: KEEP vs DISCARD

**KEEP (it's the plumbing under the vanilla UI):**
- server_control.cc combat verbs + claim gating + flood cap (upgraded per §3.f).
- combat_intent queue + `combatServerPumpIntents` + the resumable barrier
  (untouched — this design is purely about who *produces* the verbs).
- client_net.cc combat decode (`inCombat`/`myTurn`) — extended into the §3.0
  mirror + UI hooks.
- Gate 9 / gate 10 as-is; new slices add eyeball recipes, not headless gates
  (visual-verification protocol).

**DISCARD:**
- 'A' = attack-nearest and 'E'/Enter = end-turn viewer keys (main.cc:836-843).
  Replaced by crosshair click and the vanilla 32/13 keycodes (which cover both
  the bar buttons AND the vanilla SPACE/RETURN keys for free).
- Pinned-MOVE-during-combat (main.cc gmouse pin becomes combat-aware, §3.b).
- The bespoke on-screen text HUD idea — never build it.
- `cattack`'s nearest-hostile default as the *player* path (stays as debug-port
  convenience only; the player always names a netId target).

--------------------------------------------------------------------------------
## 5. Open questions / risks

1. **Mirror-consumer audit (top risk) — DONE (V0 recon, 2026-07-16).** Setting
   real `gCombatState` bits on the viewer makes `isInCombat()` true for every
   consumer reachable in the viewer frame loop. VERDICT: the "only pure-read
   consumers light up" thesis holds with **exactly two exceptions**, and both
   are in `objectSetLocation` reached via the **decoder** (`conn.pump`) — NOT
   the ticker/render path this list originally suspected (`_combat_highlight_
   change` / `_combat_outline_on/off` / gameMouseRefresh are all either
   sim-only, CROSSHAIR-transition-only, or pure-read). The game_mouse.cc:809
   AP-cost hex-cursor display IS reachable and IS pure-read — the intended
   "vanilla UI lights up" behavior, no mitigation. The two exceptions:
   - **object.cc:1377** — a gDude *elevation* change while in-combat sets
     `_game_user_wants_to_quit = 1`; on the viewer that silently exits
     `mainClientViewer`. Severe but rare (combat is single-elevation in v1).
   - **object.cc:1287** — every decoder-driven critter move while in-combat
     runs `_combat_update_critter_outline_for_los`, rewriting `critter->outline`
     on synced objects. Cosmetically inert in v1 (called with `a2=false` since
     synced critters have `outline==0` → outline set-then-disabled, never
     drawn) but an uncommanded write on authoritative objects that would
     collide with the S6 outline feature.
   MITIGATION (folded into S1): guard both sites with `clientViewerActive()`
   (golden-invisible fork, same idiom as `serverLoopActive()`). **This makes
   S1 touch object.cc — it is NOT purely viewer-only as §6 states.**
2. **Puppet-lever conflicts.** `_object_animate` per-frame call is new sim
   surface in the viewer; the §3.c "only we register" argument must hold — a
   backstop assert (registry empty unless replay in flight) is cheap. Re-puppet
   on rebaseline (loadCount change) must also cancel replays + clear the mirror
   and re-derive from the next events (TURN_START implies in-combat covers the
   mid-fight joiner — the blob does NOT carry combat state).
3. **Replay vs authority races.** The deferred-final-state window (§3.c) is the
   one place render intentionally lags store. Tripwires + cap bound it; the
   invariant to keep provable: *the store is never wrong, only the pixels are
   late*. Adversarial review warranted (no headless oracle — same class as
   server_anim.cc).
4. **To-hit % / AP-cost preview fidelity.** PC skills/perks/traits are not in
   the dude blob (`_obj_save_dude` saves the object, not PC data), so previews
   compute off the premade char. Server validates, nothing desyncs — but shown
   numbers can lie. Banked fix: a PC-data block in the join blob. Decide before
   calling combat UX "done".
5. **Target outlines** (`_combat_outline_on`) iterate the server-side roster
   `_combat_list` — empty on the viewer. v1 ships without red/green target
   outlines; post-v1 derive from mirrored critter state or stream a roster
   event.
6. **Turn-gap sends.** Between `cendturn` and the next TURN_START (≤1 beat) the
   mirror still says myTurn; stray clicks send verbs the server correctly
   drops. Harmless — authority gates; noted so nobody "fixes" it client-side.
7. **Duplicate sfx** during S4 (wire sfx + replay sfx) — accepted v1 wart,
   polish later (§3.e).
8. **Idle-timer UX.** `turnStart.deadlineMs` has no vanilla widget. v1: nothing
   (server auto-ends; the red lights flip when the next turn starts). A console
   courtesy message is a candidate for the join-etiquette chrome slice.

--------------------------------------------------------------------------------
## 6. Staged plan — each slice independently eyeball-verifiable

Presentation slices verify by eyeball (`viewer_live.sh` recipe + targeted questions),
`[[visual-verification-protocol]]`; gates stay green as the regression floor.

Done (git history): V0 recon, S1 HUD, S2 feedback, S4 anim-replay + presentation-pacing
(ordered queue + three-lane rule + per-hit HP), S5 move-glide + sequencing + combat-run,
per-hex AP, server idle-deadline pacing. Mechanics are in §3; the quirks these surfaced
live as memories (e.g. [[frame-index-render-gotcha]]).

- **S3 — vanilla input (combat becomes PLAYABLE) — DONE.** 32/13 → `cendturn`/
  `cendcombat`; combat-aware mode cycle (MOVE↔CROSSHAIR); crosshair click →
  `_combat_attack_this` with the commit fork → `cattack <netId> <hitMode>
  <hitLoc>`; called-shot picker; cmove click. **Server:** §3.f items 1-4 landed +
  the netId targeting fix + hitMode/hitLocation on the intent. PLUS **`cstart`**
  (player-initiated combat start = vanilla 'A' toggle; server idle-tick poll →
  `_combat(nullptr)`; latch/modal/dead-actor guards). Shared-code touched: combat.cc
  commit fork + viewer-attack hook (golden-invisible, clientViewerActive/null-ptr),
  combat_drain.cc netId fix, server_control verbs, game_mouse.h exposure. Gates 1-10
  green. Adversarial review done (one HIGH fixed: untrusted wire `hitLocation` OOB —
  now clamped in the cattack handler).
  QUIRKS FIXED live (no headless oracle — eyeball): (a) stuck watch cursor at combat
  entry — `onCombatEnter` set the watch directly but the frame loop owns the busy
  latch, so a dude-first fight orphaned it; fix = frame loop is the SOLE cursor owner.
  (b) entering combat mid-glide wedged the first TURN_START — `clientAnimReset()` on
  enter snaps free-roam glides. (c) a locally-rejected crosshair click froze input for
  the round-trip timeout — arm the lock only when a verb actually sent.
  KNOWN-ISSUE (banked, visual-only): per-hex AP dots don't tick down on an OVER-BUDGET
  / turn-ending combat move (bar held at pre-move value, then blanked at turn/combat
  end before the per-hop tick plays). AP charging + movement are correct; display only.
**Banked — forward work / known issues (do NOT band-aid; the durable fix is noted):**
- **Player-DEATH sequence races** — the server ends the game / drops the stream on dude
  death before the killing swing finishes. True fix = server ACTION-PACING (one action per
  beat, wait for client anim-ack), which also yields per-action HP on the wire. Revisit with
  a spectator death/revive design.
- **Idle-deadline caps** — CAP the added AI backlog for pathologically huge fights (e.g.
  `min(backlog, 90s)`); MP idle-wait etiquette (a long deadline makes other players wait on
  an idle one). Left uncapped in v1 (generous-toward-the-player is the safe direction).
- **Silent mid-fight re-sync** — the mid-fight-join re-assert replays the enter fanfare (door
  slide + sting) on already-in-combat clients; want a silent re-sync (track wasInCombat across
  reload; a fresh joiner still gets the fanfare).
- **PC-data-not-in-blob (risk 4)** — to-hit% / trait-gore (`BLOODY_MESS`/`PYROMANIAC`) for the
  dude's own actions read premade-char data until the join blob carries a PC-data block.
- **RANDOM COMBAT-GLIDE TELEPORTS (open, next combat bug)** — separate from the equip work
  (now done): critters still occasionally teleport mid-combat instead of gliding. Suspects
  (instrument `F2_TRACE_EVENTS=1` → `[move]`/`[walk] SNAP|CAP-ERASE|TRIPWIRE`): a combat move
  longer than `kMaxBurstHops`(16) CAP-ERASEs the whole walk (client_anim.cc), a durMs<=0 or
  non-adjacent hop SNAPs, or a held-walk fid/offset TRIPWIRE from some other in-place presentation.
  Run the trace on a repro and let the `[walk]` line name which path fires.
- **WEAPON draw quirks to KEEP** (the slice is DONE @<git>): (1) the viewer must APPLY
  `OBJECT_DELTA_INVENTORY`, reconciling in-hand/worn flags IN PLACE — never `_obj_inven_free`+
  recreate (freeing a weapon referenced by an in-flight `reg_anim` replay double-frees = live
  crash). (2) `_action_ranged`/`_action_melee` build the fire anim from the ATTACKER'S CURRENT
  FID's weapon code (`actions.cc:825/854`), NOT `attack->weapon`; server now arms the fid as
  STATE on an in-combat wield (rides OBJECT_DELTA) and the viewer also arms at fire time.
  (3) a MOVING critter's draw plays at the DESTINATION after its approach glide (the draw can
  never run over a live held walk — that tripwire desync was the equip-teleport class). Same
  machinery serves the banked reload / put-away / weapon-swap slices (put-away/swap DO have a
  real final fid → reuse the deferred-final-state hold, already wired for the armed fid).
- STILL OPEN (secondary): sfx dedupe (wire cue vs anim-synced replay) for multi-hit; ammo count
  not on the wire (`putInventory` omits it → rebuild restores proto-default ammo; presentation OK).
- ►► **STREAMING-EVENTS FIDELITY** — DOORS now present (open/close frame slide + crossers wait,
  EVENT_DOOR_STATE). Remaining: objects "spawn from thin air" — connect/spawn/spatial appearances
  ride OBJECT_DELTA/connect with NO glide/cue, so they pop in. Root (partly diagnosed): script
  object placement (`critter_attempt_placement`, interpreter_extra.cc:2828) snaps create→tile-0→
  dest. Needs a presentation pass over the non-MOVE appearance/relocation paths. Single-enemy map first.
- ►► **INTERACTION-VERBS (ARROW mode + action menu) — NEXT, recon DONE (staged, cheapest first):**
  enable `GAME_MOUSE_MODE_ARROW` out of combat (main.cc mouse pin ~:799/:979/:1011); reuse the
  VANILLA left-hold menu BUILD+display (game_mouse.cc:1069-1178, pure-read) and FORK the EXECUTE
  switch (:1180-1250) to wire verbs (never `_gmouse_handle_event` — its ARROW branch mutates sim).
  Template = the crosshair `_combat_attack_this` fork (combat.cc:5776, `gViewerAttackHook`).
  Order: (S-A) LOOK — fork :1185 → new `look <netId>` verb; executor `_obj_examine`/`_obj_look_at`
  + `EVENT_CONSOLE` streaming ALREADY exist → near-zero server work, delivers the whole menu shell.
  (S-B) USE+PICKUP — fork :1200/:1206 → `use`/`pickup`; executors exist (`_action_use_an_object`,
  `actionPickUp` serverLoopActive fast-paths; door `_obj_use_door`). (S-C) USESKILL — skilldex is a
  client-local modal, carry chosen skill → `useskill <netId> <skill>` (executor `actionUseSkill`).
  (S-D) TALK — server dialog-intent driver exists; needs dialog-OPTIONS streamed (new event).
  (S-E) LOOT — biggest gap: `inventoryOpenLooting` has no headless path + container pickup declines
  (actions.cc:1319) → blocked on inventory-streaming, do last. New verbs go in server_control.cc
  (dispatcher `serverControlLine`); generalize `serverResolveTarget` (combat_drain.cc:112) beyond
  critters. Scripts are disabled client-side → look/use MUST be server verbs (not local calls) to
  run SCRIPT_PROC_DESCRIPTION/LOOK_AT/USE. All forks gated `clientViewerActive()`.
- **Aimed-shot input** — 'N' (cycle action/aim) + 'B' (swap hands) not wired in the viewer
  yet, so crosshair attacks are unaimed-only in v1 (the called-shot fork + picker exist).
- **cstart edge (LOW)** — the idle-tick entry guard excludes dialog/barter/dead-actor but not
  a same-beat pending map transition; low prob on a combat server.
- Target outlines (roster mirror); sfx dedupe (wire vs replay); thrown-weapon anim; smooth
  knockback; per-hex AP dots on over-budget/turn-ending moves (visual, §6/S3 known-issue).
- **LOW self-healing edge** — a combined FID+ROT delta on a NON-participant critter WHILE it is
  gliding loses that delta's rotation until the next rotation delta (viewer-only, very narrow).
