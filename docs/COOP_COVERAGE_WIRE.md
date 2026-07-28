# Co-op coverage, axis: SIMULATED BUT NOT FORWARDED

**The question.** *"What percentage of the engine have we actually tapped for co-op, and what is
still blind — the engine skipping something because a server hook is stubbed, or computing it
right and never forwarding it?"*

**This document's axis only:** state or feedback the server computes **correctly** and the player
never sees, or sees only on the host's screen. The most insidious class, because the sim is right,
the screen is wrong, and nothing errors.

**Classification** — `FORWARDED` (reaches every player correctly) / `HOST-ONLY` (computed, only
slot 0's screen shows it) / `MIS-ADDRESSED` (reaches everyone, but the text/effect is personal) /
`NOT FORWARDED` (nobody sees it) / `NO UPSTREAM VERB` (the player cannot express it).

**Do NOT re-derive from here.** Known-bug inventories live in memory `coop-open-issues`,
`presentation-viewer-bugs`, `presentation-backpressure-gap`; per-actor *math* lives in
`docs/COMBAT_GDUDE_AUDIT.md`. This file deliberately covers what those do **not**: the
addressing/forwarding layer. Buckets **A/B/C/D** below are that audit's taxonomy, reused.

---

## 1. Wire catalogue vs the presenter interface

- Wire: `src/wire_defs.h:37` `kWireVersion = 4`; event ids **1–53** enumerated at
  `src/presenter_network.cc:68-223`, mirrored by hand at `src/client_net.cc:128-173`.
- Seam: **84 declared virtuals** in `src/presenter.h` (`presenter.h:108-701`). Three are not
  channels (`wantsSheetDeltas:292`, `wantsSnapshotBlob:354`, `beatEnd:378` = framing) → **81
  feedback/state channels**. The `NetworkPresenter` overrides **56**.

### 1a. The 25 channels the network presenter does NOT override
Self-declared at `src/presenter_network.cc:1168-1183` ("the ~19 client-local chrome virtuals" —
the real count is 25; the list has drifted, and its own comment warns it drifts).

| channel(s) | verdict | why |
|---|---|---|
| `cursorSet/ModeSet/Refresh`, `scrollEnable/Disable`, `mouseObjectsShow/Hide`, `mouseResetBouncingCursor` (`presenter.h:683-689,642`) | FORWARDED (by locality) | pure local input chrome; a viewer drives its own mouse |
| `worldInvalidate`, `worldInvalidateRect`, `worldClear`, `worldEnable`, `worldDisable` (`presenter.h:615-653`) | FORWARDED (by locality) | repaint requests; the viewer repaints from its own mirror |
| `hudBarShow`, `movieFadeOut` (`presenter.h:634,638`) | FORWARDED (re-derived) | the viewer runs its own `mapLoad` on blob apply |
| `ambientSoundLoad` (`presenter.h:631`) | FORWARDED (re-derived) | sole call site `map.cc` is all compile-time constants; viewer's own `mapLoad` reproduces it |
| `hudHitPoints`, `hudArmorClass`, `hudActionPoints`, `hudItems` (`presenter.h:659-662`) | FORWARDED (re-derived) | `client_net.cc:546,614,1252,2440-2464` converge from `OBJECT_DELTA` in the pump; see memory `no-re-derivation-path-bug-class` |
| `hudEndButtonsShow/Hide/Green/Red` (`presenter.h:677-680`) | FORWARDED (re-derived) | rebuilt from `EVENT_COMBAT_ENTER`/`TURN_START` at `client_net.cc:2602,2658,2755-2775` |
| **`hudIndicatorBar`** (`presenter.h:663`) | **see §3** | the one with only an *incidental* path |

**Verdict: 24 of the 25 are correctly dropped.** This is not the hole it looks like.

### 1b. Channels emitted by the server and NEVER decoded by the client
Diffing the two enums against `client_net.cc:807-869`'s dispatch (`default: break;` at :867):

| event | emitter | verdict |
|---|---|---|
| `EVENT_FADE_OUT=20` / `EVENT_FADE_IN=21` (`presenter_network.cc:1124-1136`) | `skill.cc:716,765,816,910,924,1006,1089,1103` (First Aid/Doctor/Repair/Science), `proto_instance.cc:800,807` (book reading), `interpreter_extra.cc:4252,4265` (script fade opcodes) | **NOT FORWARDED** — the time-skip fade-to-black is on the wire and thrown away. Vanilla's only cue that a skill consumed 30 in-game minutes. |
| `EVENT_ERROR_BOX=22` (`presenter_network.cc:1138-1144`) | `map.cc:1331,1336`, `worldmap.cc` (~15 sites), `party_member.cc` (~11 sites) | **NOT FORWARDED** — fatal load/party errors reach stderr only. Also unaddressed, so a *broadcast* modal was the design; moot while undecoded. |
| `EVENT_WEAPON_TAKE_OUT=27`, `EVENT_DOOR_STATE=28`, `EVENT_ACTION_ANIM=29`, `EVENT_EXPLOSION_FX=30` | **no core call site left** (`grep presenter()->doorState\|actionAnim\|explosionFx\|weaponTakeOut` = empty; retired in favour of `EVENT_PRES_SEQ` — see `actions.cc:2097`, `inventory.cc:317`) | **DEAD CODE both ends.** ⚠ Corrects memory `pres-record-build-track`, which still says these "stay the DEFAULT (flag off)". They are **not** a fallback: with `F2_SERVER_PRES_RECORD=0` doors, gestures, explosions and weapon draws go **silent**, because the client never had decoders for the bespoke cues. |

### 1c. Sim-side feedback with no presenter method at all — the stubbed-hook class
`ScriptRequestHandler` (`src/script_request_handler.h:34-62`) is the *second* seam, and the
server's implementation (`src/script_request_handler_server.cc:26-80`) overrides **exactly two of
eight**: `dialogEnter` and `worldMap`. Its own comment says so. Consequences:

| request | drain site | verdict |
|---|---|---|
| `elevatorSelect` | `scripts.cc:979`, `scripts.cc:1086` | **NOT FORWARDED / feature dead.** Base returns `-1` (`script_request_handler.h:47`) → the whole placement+transition block is skipped "exactly as if the player had cancelled". **Elevators do nothing on a dedicated server.** (And `scripts.cc:986-990` would move `gDude`, i.e. slot 0, if it did.) |
| `townMap` | `scripts.cc:964`, `map.cc:1134` | **NOT FORWARDED.** A `map == -1` transition clears the pending transition and nothing happens — no town-map screen for anyone. |
| `automapSave` | `scripts.cc:980,1087` | NOT FORWARDED (see §3 automap) |
| `endgame` | `scripts.cc:1060` | **NOT FORWARDED.** No override; the flag is cleared and the world continues. `endgamePlayMovie` is a loud abort stub (`server_stubs.cc:354`) so this is *fortunate*, not designed. Corroborates memory `full-run-recon` ("2 traps kill the server AT victory"). |
| `looting` | `scripts.cc:1065,1135` | NOT FORWARDED — a *script-forced* loot screen is dropped (player-initiated loot works via the `loot`/`take`/`put` verbs) |
| `stealing` | `scripts.cc:1070` | NOT FORWARDED — and no verb either (§4) |

**And one abort:** `gameShowDeathDialog` (`server_stubs.cc:429`) is `serverStubAbort` →
`abort()` (`server_stubs.cc:123-129`). Its only caller is `_process_rads` on
`obj == gDude` (`critter.cc:725-731`). **The host dying of radiation sickness kills the whole
server for every connected player.** An extra dying the same way dies silently. *(Static read;
not reproduced — unverified.)*

---

## 2. Addressing: who the feedback is aimed at

The seam has three console paths: `consoleMessage` (broadcast), `consoleMessageFor(actorNetId,…)`
(`presenter.h:133`), `consoleMessageStyled(actorNetId, channel,…)` (`presenter.h:143`), plus the
non-virtual `consoleNarration` (`presenter.h:160`) that resolves a `PresenterNarrationScope`.

Whole-tree counts of `presenter()->…` call sites:

| path | sites | meaning |
|---|---|---|
| `consoleMessage(` | **93** | broadcast — every player reads it verbatim |
| `consoleMessageFor(` | 7 | addressed |
| `consoleMessageStyled(` | 20 | addressed + channel |
| `consoleNarration(` | 7 | per-reader (all 7 in `combat_drain.cc:447,546,643,673,713,721,747`) |

Of the 93 broadcasts, **29 are provably personal** — they sit inside an adjacent
`== gDude` / `playerActorIs` identity gate, i.e. the code already decided *whose* message this is
and then shouted it at the room. Full list (all in files linked into `f2_server`):

`actions.cc:1316,2448` · `combat.cc:3218,3235,6430` · `inventory_ui.cc:592` ·
`item.cc:2376,2425,2451,2499,2779,2879,3113,3146` ·
`proto_instance.cc:839,847,1201,1233,1358,1376,1916,1988,2034` ·
`skill.cc:655,781,863,941,1048,1115`

Worked examples, so the shape is unambiguous:

- **`proto_instance.cc:43-46`** — `presenterConsoleMessageBridge` → `consoleMessage`. This is the
  sink for `_obj_examine` / `_obj_look_at` (`proto_instance.cc:186,244`), which the `look` verb
  runs (`server_control.cc:2522`). **One player examines a locker; every player's log reads
  "You see: a locker."** plus its whole description. Bucket **C** (acting player).
- **`skill.cc:731-741`** — `"You heal %d hit points."` broadcast; `skill.cc:781` `"You've taxed
  your ability with that skill."` broadcast. Bucket **C**.
- **`actions.cc:1313-1317` `_is_next_to`, `actions.cc:2445-2449` `_can_talk_to`** —
  `"You cannot get there."` A pure personal refusal, broadcast. Bucket **C**.
- **`queue.cc:614-621`** — `"Due to your inept handling, the explosive detonates prematurely."`
  Fires from a **queued event**, so no `ServerActorScope` is held: the message is neither addressed
  nor even *authored* for the right actor, although `obj` is right there in the signature.
- **`interpreter_extra.cc:1019-1023` `opDisplayMsg`** — the single largest surface. **Every**
  script `display_msg` is broadcast, and vanilla scripts are written in the second person for the
  dude. ⚠ A blanket fix is wrong (some lines are genuine world narration); the correct fix is
  `scriptContextDude(program)` addressing, opt-in.

Contrast — the parts that got this right, and are the template: combat narration renders once per
reader (`combat_drain.cc:447-747` + memory `combat-narration-per-reader`), and every server
refusal is addressed (`server_control.cc:180`, `combat_drain.cc:367`).

**Not a policy gate:** per memory `coop-mp-track`, `subject == gDude` is **not** a host test —
`ServerActorScope` (`server_control.cc:2147` verb dispatch, `combat.cc:3942` turn beat) rebinds
`gDude` to the acting actor. So the 29 sites above have the **right author, wrong audience**. The
genuinely `HOST-ONLY` sites are the ones *outside* any scope (queued events, AI turns, script
opcodes) — bucket **B/C** confusion.

### 2a. Confirmed HOST-ONLY, with the TODO already in the source

| site | what only slot 0 sees | note |
|---|---|---|
| ~~`combat.cc:2941-2967`~~ | **`"…you earn %d exp. points."`** | **FIXED 2026-07-25** — addressed to the earner on `kMsgChannelReward` (and the "without taking a scratch" HP test reads the earner, not the host). Same for `skill.cc` `_show_skill_use_messages`. |
| `skill.cc:645-656` | `"You earn %d XP for honing your skills"` — `obj == gDude` | same stale TODO |
| `stat.cc:1041-1044` | `dudeEnableState(DUDE_STATE_LEVEL_UP_AVAILABLE)` + `sfxPlay("levelup")` — `if (isHost)` | the *message* was fixed (`stat.cc:1037`, addressed, `kMsgChannelReward`); the **indicator box and the sound were not**. An extra levels up with no LEVEL badge and no chime. |
| ~~`critter.cc:687-700` `_process_rads`~~ | radiation-level message | **FIXED 2026-07-25** — `playerActorIs` + `consoleMessageFor`, together with the geiger clicks, the large-dose warning and the poison-damage line |
| `item.cc:3108-3113`, `item.cc:3138-3146` | withdrawal start/end text — `obj == gDude` | fires from a **queued event** → no scope → literally slot 0 only |

### 2b. Personal effects that are broadcast as *effects*, not text
`sfxPlay` (`presenter_network.cc:1104-1110`), `sfxPlayAt` (:1112-1122), `screenFadeOut/In`
(:1124-1136) and `errorBox` (:1138-1144) carry **no actorNetId at all** — the seam has no way to
address them. So `stat.cc:1043`'s `"levelup"` chime plays on **every** viewer when the host levels,
and `combat.cc:6211`'s out-of-ammo click is everybody's. MIS-ADDRESSED by construction; the fix is
an `actorNetId` parameter mirroring `consoleMessageFor`.

---

## 3. Whole vanilla feedback systems, one by one

| system | verdict | evidence |
|---|---|---|
| Combat log narration, hit/miss/crit/death/cripple | **FORWARDED** (best-in-tree) | `combat_drain.cc:447-747`, per-reader |
| Damage / HP / AP / AC / combat.results (incl. `DAM_BLIND`, `DAM_CRIP_*`) | FORWARDED | `OBJECT_DELTA_HP/AP/COMBAT_RESULTS` (`presenter.h:66-68`) |
| Object lighting + global ambient light | FORWARDED | `OBJECT_DELTA_LIGHT` / `WORLD_DELTA_LIGHT` (`presenter.h:82,93`), applied at `client_net.cc:2106,2536` |
| Scenery/door/container art **frame** | FORWARDED | `OBJECT_DELTA_FRAME` (`presenter.h:79`) + the double-apply suppressor (memory `coop-open-issues`) |
| Doors opening/closing (motion) | FORWARDED via `EVENT_PRES_SEQ` only | §1b — no fallback |
| Quest steps / holodisks / karma / reputation **values** | FORWARDED | `gvarDelta` (`presenter.h:264`), `holodiskAdd` (:274); pipboy/char-screen render from gvars client-side |
| Karma *change* notification | MIS-ADDRESSED (benign) | `game.cc:131-144` broadcast; karma is shared, so acceptable |
| Music, ambient sfx, movies, movie-seen ledger | FORWARDED (+ baseline re-announce) | `EVENT_MUSIC_PLAY=41`, `MOVIE_*=42/46/47` |
| Worldmap position / fog of war / encounter prompt | FORWARDED | `worldmapState`/`Subtiles` (`presenter.h:606,612`), `EVENT_ENCOUNTER_PROMPT=49` |
| Day/night, palette cycling, weather | n/a | `cycle.cc`/`palette.cc` are `f2_client`-only and time-driven; game time streams (`WORLD_DELTA_GAMETIME`) |
| Floating text (script + AI barks) | FORWARDED | `EVENT_FLOAT_TEXT=17`, `interpreter_extra.cc:3261`, `combat_ai.cc:3425` |
| Examine / "You see: X" | **MIS-ADDRESSED** | `proto_instance.cc:43-46` (§2). Mouse-*hover* description is client-local and correct (`game_mouse.cc:732`). |
| Screen fades around time skips | **NOT FORWARDED** | §1b |
| **Radiation / poison / addiction / sneak / level-up INDICATOR BOXES** | **HOST-ONLY + never streamed** | `indicatorBarRefresh` (`interface.cc:2339-2410`) reads `dudeHasState(SNEAKING/LEVEL_UP_AVAILABLE/ADDICTED)` + poison/rad. Poison/rad **do** stream; the three `DUDE_STATE_*` bits **do not**: `dudeEnableState`/`dudeDisableState` (`critter.cc:1327-1354`) write `protoGetProto(gDude->pid)`'s flags and **never call `playerSheetMarkDirty`**, so the PSHT proto row (`player_sheet.cc:27`) is never marked and the change never reaches a viewer. They also always write **slot 0's** proto (bare `gDude`, no subject) — so addiction set from a queued drug path (`item.cc:3167-3174`) brands the host. The bar itself is only *incidentally* re-derived, piggybacking `interfaceBarRefresh` (`interface.cc:875-884`) whenever HP/AP happens to repaint. |
| Radiation/poison warning text ("geiger counter is clicking", "You take damage from poison") | MIS-ADDRESSED | `critter.cc:497-503,569,580` broadcast, although `critterAdjustPoison` at `critter.cc:477` **is** addressed — the inconsistency is inside one file |
| XP gain lines | **HOST-ONLY** | §2a |
| Level-up badge + chime | **HOST-ONLY** | §2a |
| **Kill counts** (char-screen Kills tab) | **NOT FORWARDED, ever** | `gKillsByType` is one global array (`critter.cc:253`, incremented `combat.cc:5873`); it rides the `.SAV` (`savegame.cc:93` `killsSave`) — **not** the map blob and **not** a PSHT row. No viewer ever learns a kill count. Also shared, not per-player. |
| **Automap** | **NOT FORWARDED** | `automapSaveCurrent` and `automapSetDisplayMap` are no-ops on the server (`server_stubs.cc:320-322`), so the worldmap-arrival "map revealed" flag (`worldmap.cc:2243`) never happens for anyone; `automapShow` is a loud abort (:322) |
| **Elevators / town map / endgame** | **NOT FORWARDED (stubbed hook)** | §1c |
| Barter | FORWARDED | snapshot design, `presenter.h:505-567` |
| Item-use / skill-use / refusal feedback | MIS-ADDRESSED (29 sites) | §2 |
| Stealing feedback | n/a — no verb | §4 |
| Rest / rest interruption | n/a — no verb | §4 |

---

## 4. Directionality: what a client can never tell the server

`server_control.cc` accepts **≈63 verbs** (tabulated `DEDICATED_HOWTO.md:321-337`; dispatch
`server_control.cc:1659-3460`). Vanilla player capabilities with **NO UPSTREAM VERB**:

| capability | evidence |
|---|---|
| **SNEAK toggle** (key `1` / skilldex) | `main.cc:902-904`: *"a self-TOGGLE (`dudeToggleState`) with no target — a different path, deferred (needs a self-state wire verb)"*; `viewerSkillModeForKey` omits `KEY_1` (`main.cc:911-923`) |
| **STEAL** | `main.cc:922-926` KEY_3 "intentionally parked"; `viewerSkillModeForSkilldexRc` returns -1 for `SKILLDEX_RC_STEAL`; server-side `stealing()` is a no-op (§1c) |
| **REST** (pipboy alarm clock) | `pipboy.cc:1864-1886` refuses locally with the *misleading* `"You cannot rest at this location!"`; no verb; and the operator's `rest` debug verb (`command.cc:480-513`) calls `pipboyRestHeadless`, which is an **abort stub** in `f2_server` (`server_stubs.cc:492`) — it only works in the headless golden probe, where `pipboy.cc` is linked |
| **Elevator floor selection** | no verb, and the hook is dead (§1c) |
| **Town-map entry** | no verb, hook dead (§1c) |
| **Player→player item transfer** | `take`/`put`/`takeall` require a container ITEM or a **dead** critter (`server_control.cc:3279-3286`); `loot` on a live critter is refused (`server_control.cc:2562-2566`); `talk` on a player is explicitly refused (`server_control.cc:2578-2582`). The only route between two players is drop-on-ground. |
| Player-initiated save / load | admin-port only (`DEDICATED_HOWTO.md:290`), by ruling |

---

## 5. Coverage on this axis — with the denominators stated

| denominator | what it counts | forwarded correctly | % |
|---|---|---|---|
| **81 presenter channels** (`presenter.h`, minus 3 predicates/framing) | is the channel reproduced on a viewer, by wire or by local re-derivation? | 56 overridden + 24 correctly-dropped-and-re-derived = **80**; holes: `hudIndicatorBar` (partial), fades ×2 + `errorBox` (emitted, undecoded) | **~95%** — flattering; a channel is not a feature |
| **63 personal-feedback call sites** where addressing demonstrably matters (34 already addressed + 29 broadcast-but-provably-personal) | does the line reach *only* the player it is about? | 34 | **54%** |
| **26 vanilla feedback systems** surveyed in §3 | does the system reach a non-host player at all? | 15 FORWARDED, 4 MIS-ADDRESSED, 4 NOT FORWARDED, 2 HOST-ONLY, 1 n/a | **~58% clean, ~85% at least visible** |
| **8 `ScriptRequestHandler` hooks** (`script_request_handler.h`) | implemented on the server? | 2 | **25%** |
| **≈70 player capabilities** (63 verbs + 7 named gaps in §4) | can the player express it upstream? | 63 | **~90%** |

**Honest headline for this axis: ~60%.** The *state* channel is in good shape (deltas, gvars,
holodisks, lighting, frames, worldmap all stream). What is weak is (a) **addressing** — roughly
half of the feedback that is about one player is shouted at everyone — and (b) a small set of
**entirely untapped subsystems** (elevators, town map, automap, kill counts, endgame, rest) that
are invisible because a hook returns a sentinel rather than because anything failed.

---

## 6. FIX THESE FIRST

1. **`dudeEnableState`/`dudeDisableState` take a subject and mark the sheet dirty**
   (`critter.cc:1327-1354`). One function pair fixes sneak, level-up badge and addiction *all at
   once*: today they write slot 0's proto and never stream. Also unblocks the sneak verb.
   — **leaf**
2. **Add `actorNetId` to `sfxPlay`/`screenFadeOut`/`screenFadeIn`, and decode the fades**
   (`presenter.h:667-674`, `presenter_network.cc:1104-1136`, `client_net.cc:807-869`). Closes a
   whole *category*: personal sound and the time-skip fade currently have no addressing mechanism
   at all, so no call-site fix is even expressible. — **medium**
3. **Route the 29 provably-personal broadcasts through `consoleMessageStyled`**, starting with
   `proto_instance.cc:43-46` (examine — the highest-frequency one) and the two XP lines
   (`combat.cc:2946`, `skill.cc:647`), whose own comments already say the routing exists.
   An extra earning XP silently is the single worst *player-facing* item here. — **leaf** per site,
   **medium** for the sweep
4. **Give the server a real `elevatorSelect` + `townMap`** (`script_request_handler_server.cc`).
   Two whole vanilla navigation systems are dead, and `scripts.cc:986-990` would move `gDude` even
   if they weren't — so it needs the acting actor threaded, not just an override. — **deep**
5. ~~**Make `gameShowDeathDialog` non-fatal**~~ — **DONE 2026-07-25**: the cause IS streamed, to
   the actor it happened to, and the abort is gone from the stub as well as from the call site. What
   is still owed is the wider death FEEDBACK (a teammate should be told a player went down, and by
   what) — that belongs with the undesigned death policy, `playerActorDied`. — **medium** (feedback)

**Runner-up worth naming:** no player-to-player item transfer (§4) — an odd hole for a co-op build,
and larger than it looks because the loot path is gated on *dead-or-container* in three places.
