# DEDICATED_HOWTO — launching `f2_server` & the wire client

Operator reference for the dedicated server (`f2_server`) and the SDL client used as a
network viewer/player (`fallout2-ce`). Everything here is derived from the code; when you
add an env var or a verb, update this file (grep anchors: `getenv("F2_` in `src/`, the verb
tables in `server_admin.cc` / `command.cc` / `server_control.cc`).

There are three ways in, smallest to largest:
1. **`scripts/viewer_live.sh`** — all-in-one: boots the server *and* one or more viewers,
   tears them all down when the first viewer quits. Best for solo testing on one box (§7).
2. **`f2_server` alone** — a bare dedicated server. Players connect from elsewhere (§2).
3. **`fallout2-ce` alone** — a client that joins a running server (§3).

All binaries must run with CWD = the game dir (`FO2/`), because they read `master.dat` /
`patch000.dat` relative to CWD. `viewer_live.sh` does the `cd` for you; the bare launches
below assume `cd FO2` first.

---

## 0. Build

```sh
# Linux (native)
cmake --build build     -j"$(nproc)" --target f2_server fallout2-ce
# Windows (mingw-w64 cross; wine-verified)
cmake --build build-win -j"$(nproc)" --target f2_server fallout2-ce
```

| binary | what it is |
|--------|------------|
| `f2_server` / `f2_server.exe` | headless dedicated server. No SDL window, no art locks |
| `fallout2-ce` / `fallout2-ce.exe` | the game. Plain single-player unless `F2_CLIENT_CONNECT` is set, which turns it into a network client |

First-time configure, if a build dir is missing:
```sh
cmake -B build     -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake -B build-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
```
Release archives ship **stripped** copies (`strip`), which is roughly 10× smaller; keep the
unstripped originals in the build dir for crash analysis.

---

## 1. Production quick start

### Server — this is the whole thing

```sh
cd FO2
env F2_SERVER_MAP=artemple.map \
    F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
    F2_SERVER_PACE_MS=100 \
    ../build/f2_server
```

A map, two ports, and a real-time pace. **Every feature is already on** — combat
presentation, recorded animation, smooth walking, live dialogue and barter, worldmap travel,
cutscenes. Add `F2_AUTOSAVE_SECS=300` if you want periodic autosaves (they rotate
through slots 11-15, oldest overwritten first; the announcement names the slot).

The feature variables still exist, but only as **kill switches** for debugging — set one to
`0` to turn that feature off (full list in §2.4):

| set it to `0` | what you lose |
|---------------|---------------|
| `F2_SERVER_RESUMABLE_COMBAT=0` | no combat presentation, no player-started combat |
| `F2_SERVER_PRES_RECORD=0` | discrete actions have no animation (pickups, reload gesture, throws) |
| `F2_SERVER_SMOOTH_WALK=0` | out-of-combat walkers teleport instead of walking |
| `F2_DIALOG_STREAM=0` | no live conversations and no barter |
| `F2_WORLDMAP_STREAM=0` | no travel, and therefore no random encounters |
| `F2_MOVIES=0` | cutscenes are marked seen and never shown (read the ⚠ in §2.6 first) |

`F2_SERVER_TICKS` unset means the server never closes on its own. With `F2_SERVER_CMD` set
it is a keepalive server: it freezes when empty and waits for reconnects.

### Client — two flavours

```sh
cd FO2
# (a) ASK: roll a character in vanilla's creation screen, then join
env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb F2_PLAYER_CREATE=ask \
    F2_WINDOWED=1 ../build/fallout2-ce

# (b) NO ASK: join straight in as whatever this name already is
env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb \
    F2_WINDOWED=1 ../build/fallout2-ce
```

**`F2_PLAYER_CREATE` only matters the first time the server sees that name.** After the
account exists the spec is ignored — you do not re-roll by reconnecting — so the same line
works for both first join and every reconnect. With no create spec at all, a brand-new name
joins as a copy of the host body.

---

## 2. Server env reference

### 2.1 World source — pick exactly one (or neither → lobby)
- `F2_SERVER_MAP=<map.map>` — boot a fresh world on that map (`artemple.map` = the Temple).
- `F2_SERVER_LOAD=<1-15>` — restore a save slot instead. **`11`-`15` are the autosave
  window**; each autosave says which slot it went to, in the server log and in-game.
- **Both set → the LOAD wins loudly** and the map is ignored; they are alternatives.
- **Neither → LOBBY**: requires `F2_SERVER_CMD`; the server waits and greets each operator
  with the slot listing. Pick a world at runtime with `load <n>` / `new <map.map>`. With no
  command channel and no world it prints usage and exits.

### 2.2 Channels
| var | meaning |
|-----|---------|
| `F2_SERVER_NET=<port>` | the **viewer wire** (binary). Blocks at startup until the first client connects, then serves; more clients join mid-stream |
| `F2_SERVER_CMD=<port>` | the **admin/command channel** (plain telnet/nc, one `verb arg arg2` per line). Accepts connections any time, unrestricted, your operator console (§5) |

`F2_SERVER_CMD` works **without** `F2_SERVER_NET` for a viewer-less server you drive
entirely by command.

> ⚠ No `NET` **and** no `CMD` with unlimited ticks = a spinning CPU core you can only stop
> with a signal. Always give it a `CMD` port, or a positive `F2_SERVER_TICKS`.

### 2.3 Players & identity
| var | default | meaning |
|-----|---------|---------|
| `F2_SERVER_PLAYERS` | `1` | pre-spawn an N-body party on a **fresh** world (capped at `kMaxPlayerActors`). Ignored on a co-op load — saved extras are restored instead |
| `F2_SERVER_HOST` | first-come | pin slot 0 (the host body) to this account name |
| `F2_SERVER_NAME` | — | server display name sent in the handshake |
| `F2_REQUIRE_TOKEN` | off | require a matching `F2_PLAYER_TOKEN` from clients to claim a slot. Off = first-claimer-wins (fine for a private box) |

> **Slot 0 is no longer privileged.** As of v0.2 no screen is host-only: the worldmap, map
> transitions and dialogue are open to every player (dialogue is driven by whoever *started*
> it). `F2_SERVER_HOST` now only decides which account gets the slot-0 body — useful for
> identity and for the residual "no recorded dialogue driver" fallback (an NPC-opened
> conversation, or the CMD port), not as an anti-grief policy.

### 2.4 Feature kill switches — everything is ON by default
These are **not** things you switch on; they are things you can switch **off**. Unset means
enabled. `=0` disables. Any other value also enables, so `=1` is harmless and explicit.

| kill switch | the feature it disables |
|-------------|-------------------------|
| `F2_SERVER_RESUMABLE_COMBAT=0` | beat-spanning combat — required for combat presentation and player-started combat |
| `F2_SERVER_PRES_RECORD=0` | presentation record/replay channel — the animation for every discrete action |
| `F2_SERVER_SMOOTH_WALK=0` | animated out-of-combat walks, one tile per beat |
| `F2_DIALOG_STREAM=0` | dialogue + barter block-and-pump (live conversations and trade) |
| `F2_WORLDMAP_STREAM=0` | worldmap block-and-pump (live travel, car travel, random-encounter prompts) |
| `F2_MOVIES=0` | projecting cutscenes to viewers — see §2.6 |
| `F2_NO_MODAL_PRESENT=1` | keeping the world animating behind an open modal (note: this one is a `=1` switch) |
| `F2_NO_ATTACK_HEADER=1` | the "X throws the Spear at you." line before combat damage lines |

> ►► Anything added later follows the same rule: features ship ON, and the only sanctioned
> variable is a kill switch. The **headless golden probe** is the one context where features
> default off, because it exists to reproduce vanilla single-player deterministically — it is
> the test harness's job to opt out, never the server's job to opt in
> (`serverFeatureEnabled`, src/server_loop.h).

### 2.5 Pacing, timing, persistence
| var | default | meaning |
|-----|---------|---------|
| `F2_SERVER_PACE_MS` | `0` (full speed) | ms wall-clock per beat; `100` ≈ real time. Use `100` for play, lower to fast-forward |
| `F2_SERVER_TICKS` | `0` = **unlimited** | serve forever. A positive value is a safety cap for runs that must terminate |
| `F2_SERVER_KEEPALIVE` | on if `CMD` set | persistent server: don't quit when the last player leaves; idle **frozen** and accept reconnects. `=0` restores exit-on-empty |
| `F2_AUTOSAVE_SECS` | `300` | autosave interval → **rotating window, slots 11-15**; `0` = off. Also fires on every **map change**, and an interval save that comes due at an unsafe moment lands on the first safe beat. Fills empty slots first, then overwrites the OLDEST (by in-game clock). Broadcasts "Game auto-saved to slot N." — ►► the window exists because a single slot got overwritten in place: walking into a random encounter checkpointed a mid-fight transient map over the only save you had, with nothing older to fall back to. |
| `F2_SERVER_TURN_IDLE_MS` | `60000` | combat: sim-ms a human gets once their turn is on screen |
| `F2_SERVER_TURN_WAIT` | off | force the resumable-combat turn barrier to wait |
| `F2_SERVER_ACTION_GATE` | **on** | per-player action pacing (an action is gated for as long as the animation it produced runs). `=0` disables — the only kill switch here that defaults ON |
| `F2_SERVER_OUTBOX_PACE` | off | **experimental**: meter wire emission from animation durations instead of firing everything at once. In-combat only; out-of-combat stays realtime. Also switches combat glides to frame-true pacing (walk 371ms/run 125ms per tile vs the rounded 400/100) |
| `F2_SERVER_SEED` | — | RNG seed, for reproducible worlds and encounters |

### 2.6 Movies
**On by default.** One thing still has to be true for a cutscene to play: **at least one
viewer connected** when it triggers — with none the barrier bails at once.

With `F2_MOVIES=0`, `gameMoviePlay` marks the movie seen and returns without sending
anything, so `movie 4` prints "playing… / barrier released" instantly and nothing shows.

> ⚠ Why the kill switch is worth knowing about: `movdone` — the ack that releases the movie
> barrier — is a wire verb only the CLIENT sends, so the operator console **cannot** release
> it. A viewer that renders black instead of the movie leaves the server parked with no
> escape but a restart. If you hit that on your build or data, `F2_MOVIES=0` is the way out.

### 2.7 Lifecycle — run vs freeze vs stop
The model is **"empty = freeze, player = play, never quit on its own"**:
- **No players** → sim *frozen*: game clock, NPCs and the object-id budget do not advance,
  but connections are still accepted. A player logging in un-freezes it on the next beat.
- **≥1 player** → normal play, paced by `PACE_MS`.
- **Last player leaves** → a keepalive server (default when `CMD` is set) returns to
  frozen-idle. A bare demo server with no `CMD` still exits.
- **Stopping** → `quit`/`shutdown` on the command channel, a terminal quit (dude death or
  endgame), or a signal.

> Startup still blocks for the first wire client before serving: the world comes alive when
> the first player joins, then persists across everyone leaving.

---

## 3. Client env reference

`fallout2-ce` becomes a network client when `F2_CLIENT_CONNECT` is set; without it, it is
the ordinary single-player game.

| var | meaning |
|-----|---------|
| `F2_CLIENT_CONNECT=<host:port>` | connect to a server's **wire** port (`F2_SERVER_NET`) |
| `F2_PLAYER_NAME=<name>` | account name to log in as — binds you to that name's character across reconnects. Unset = legacy bare `claim` (slot-0-preferred dev affordance) |
| `F2_PLAYER_CREATE=<spec>` \| `ask` | only used the **first** time the server sees this name: either `ask` to roll in vanilla's creation screen (opens before connecting), or a literal spec `"S P E C I A L [tag tag tag] [trait trait]"`. Ignored for an existing account |
| `F2_PLAYER_TOKEN=<tok>` | auth token; needed when the server sets `F2_REQUIRE_TOKEN` |
| `F2_WINDOWED=1` | run windowed, so several clients fit side by side |
| `F2_UNLOCK_CAMERA=0` | re-leash the camera. Free scrolling is **on by default** — you can pan to the edge of the world, but not into the void (the map-edge border and the script/cutscene scroll blockers still apply) |
| `F2_NO_MUSIC=1` | mute music |

The client sends its OS in the handshake, so joins are announced as
"X joined the game (Linux/Windows/…)".

### In-game keys worth knowing
| key | effect |
|-----|--------|
| `Left Alt` | toggle the **highlight-lootables** overlay: ground items, corpses, containers (including scenery lockers/safes/desks), and exits — doors, stairs, ladders, elevators. Purely local, never saved |
| `B` | swap active weapon hand (costs no AP, plays the put-away/take-out animation) |
| `1` | toggle **sneak** (per-player: your roll, your Silent Running / Silent Death — not the host's) |
| `3` | **Steal** — the skilldex entry, on a living critter opens the server-owned steal session |
| `P` | the **pipboy** — holodisks (including the server's own SERVER INFORMATION disk), quests, status, automaps. Refused in combat, as in vanilla; the alarm clock refuses too, because the server owns the clock |
| `PageUp` / `PageDown`, wheel | scroll the **dialog option list** when a node has more options than fit (the reply window keeps first claim on the wheel while it is paging itself) |

---

## 4. Recipes

```sh
# ── NEW GAME on the Temple map, everything on (nothing to opt into — this IS §1)
cd FO2 && env F2_SERVER_MAP=artemple.map F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 ../build/f2_server

# ── LOAD SAVE SLOT 8 (note: no F2_SERVER_MAP — they are alternatives)
cd FO2 && env F2_SERVER_LOAD=8 F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 ../build/f2_server

# ── LOAD AN AUTOSAVE (window slots 11-15; `saves` shows what is in each)
cd FO2 && env F2_SERVER_LOAD=11 F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 ../build/f2_server

# ── LOBBY: decide the world at runtime (neither MAP nor LOAD)
cd FO2 && env F2_SERVER_NET=9200 F2_SERVER_CMD=9201 F2_SERVER_PACE_MS=100 \
  ../build/f2_server
printf 'saves\n'              | nc -q1 127.0.0.1 9201   # what can I restore?
printf 'load 8\n'             | nc -q1 127.0.0.1 9201   # ... or
printf 'new artemple.map\n'   | nc -q1 127.0.0.1 9201

# ── CLIENT, ask (first join for this name)
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb \
  F2_PLAYER_CREATE=ask F2_WINDOWED=1 ../build/fallout2-ce

# ── CLIENT, no ask (returning, or happy to copy the host)
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb \
  F2_WINDOWED=1 ../build/fallout2-ce

# ── CLIENT, scripted character (no screen, no prompt)
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Mennoc \
  F2_PLAYER_CREATE="6 7 6 5 6 8 6 smallguns doctor speech kamikaze finesse" \
  F2_WINDOWED=1 ../build/fallout2-ce

# ── TWO CLIENTS on one box (nothing extra needed — the join blob lives in RAM)
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb \
  F2_WINDOWED=1 ../build/fallout2-ce &
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Mennoc \
  F2_WINDOWED=1 ../build/fallout2-ce &

# ── REMOTE server, token-gated
#   server: ... F2_REQUIRE_TOKEN=1 F2_SERVER_NAME="Cahb's box" F2_SERVER_HOST=Cahb ...
cd FO2 && env F2_CLIENT_CONNECT=203.0.113.7:9200 F2_PLAYER_NAME=Mennoc \
  F2_PLAYER_TOKEN=hunter2 ../build/fallout2-ce
```

---

## 5. Runtime command channel (`nc`/telnet → `F2_SERVER_CMD`)

One `verb arg arg2` per line. Admin verbs answer you; anything else is dispatched into the
sim as a debug verb.
```sh
printf 'status\n'                  | nc -q1 127.0.0.1 9201
printf 'save 8\n'                  | nc -q1 127.0.0.1 9201
printf 'stress 20 0x010000EE 42\n' | nc -q1 127.0.0.1 9201
```

### Admin verbs — answer the operator (`server_admin.cc`)
| verb | meaning |
|------|---------|
| `saves` | list save slots |
| `save <1-10> [label]` | save the running world into a slot |
| `load <1-15>` | restore a slot (**lobby only**); `11`-`15` = the autosave window |
| `new <map.map>` | boot a fresh world (**lobby only**) |
| `status` | what is running right now |
| `say <chan> <text>` / `saydemo` | push a styled line to every viewer's log / style eyeball test |
| `movie <0-16>` | project a movie to every viewer (`4` = vault-suit intro) |
| `timeskip <minutes>` | advance the game clock like a script does |
| `spawn <pid> [n] [tile]` | place n critters of pid |
| `stress <n> [pid] [seed]` | spawn n hostiles near the players and aggro them (default pid `0x010000EE` = Raider; the seed is printed — reuse it to replay) |
| `despawnall` | destroy everything `spawn`/`stress` created |
| `revive <slot>` | bring a dead player back at 1 HP (`0` = host, `1..` = extras) |
| `xp <slot> <amount>` | award experience to ONE seat (`0` = host, `1..` = extras). **Levels up as it goes** — a grant that crosses several thresholds levels several times, each awarding HP, skill points and (on the cadence) an owed perk. Negative amounts allowed (clamped at the floor); the reply reports the level before/after |
| `sheet [slot]` | read a seat's sheet: level, XP, unspent skill points, whether a perk pick is owed, tagged skills, traits. All seats if no slot |
| `rest <minutes> [slot]` | pass time for **everyone** (one clock) and heal every player, at vanilla's rate. Honours the map's own "you cannot rest here" gate; reports the clock and HP either side. The debug `rest` verb answers through `debugPrint`, which the server drops — use this one when you want to *see* the result |
| `sp <slot> <points>` | **set** a seat's unspent skill points. The one cheat of the group — it hands out the level-up currency so the spend path can be exercised without grinding XP |
| `skillup <slot> <skillId>` | spend ONE point in one skill, through the exact rulings a client's intent goes through: enough points, under the 300% cap, in *that seat's* row |
| `skilldown <slot> <skillId>` | take one point back. Only down to the value the sheet held when its session opened — a refund below that would sell skill the seat was never awarded |
| `perkpick <slot> <perkId>` | take one perk. Requires an **owed pick** (a level-up on the 3rd/4th-level cadence) and passes vanilla's prerequisites; Tag!/Mutate! then owe a follow-up (`tagpick`/`mutpick`, wire verbs) |
| `encnext` | **test hook.** Arms the next worldmap travel check to roll an encounter **and** to detect it, so the accept/decline prompt is reachable on demand instead of behind two dice. One-shot — the check it arms consumes it. Travel afterwards; the ordinary movement/rate-limit guards still apply, so it will not fire on a party standing still |
| `party [skillId]` | **both** party numbers side by side — `script-count` (companions only, what a script may refuse you on) and `group-size` (companions + every online living player) — plus the group-best and each seat's own value for one skill (default Barter). The difference between the two counts is the load-bearing part; see §5's control-verb notes and `party_member.cc` |
| `fixcar` / `fixcar list` / `fixcar park [n]` | warp the car trunk back onto the car; list areas; re-park the car (writes saved state) |
| `help` / `?` | list verbs + channels |
| `quit` / `shutdown` | stop the server |

### Debug verbs — dispatched into the sim (`command.cc`)
```
walk walkto warp critwalk critwarp climb push mark
aggro cattack caim cdamage cmove cendturn hurt poison rad drug useskill useskillon useitem usedoor
give drop pickup wield stow unload reload lootall stealall explode
rest restopt savegame loadgame endgame entermap levelup perk xp mutate charroll charsnap
dtalk dsay dend   wmenter wmmove wmtravel wmesc
stealon stake splant sdone
audit
```
These take `arg`/`arg2` ints — e.g. `walk 40 1` = walk 40 tiles running, `aggro 1` = start
combat.

**`audit`** is the divergence oracle, and it is also on the control plane. It makes the
server dump **its own view of every object** — the full field set, not just position — as
`EVENT_STATE_AUDIT`. Two things read it: a connected client diffs it against its own mirror
and prints one line per divergent field mid-play, and `tools/replay.py --audit` diffs it
against what the **stream alone** rebuilt. The second is the sharper question, because a
divergence there is a protocol hole rather than a client bug — no client can be right about
a field the wire never sent. Reach for this first when something "does nothing" on one
screen and works on another (`scripts/check_audit.sh` is the gate).

### Control / wire verbs (`server_control.cc`)
What a **connected client** sends over the wire, gated per-session (the trust boundary) —
listed for reference, not for typing into the console:
```
login claim create  cstart cendcombat cendturn cattack cmove caim  mv rot push
look use useitem useitemon usedoor open take takeall put get loot
invopen invclose invwield invunwield invdrop unload reload hand skill
talk dsay dend dbarter  boffer bunoffer btake bcommit bcancel bdone
stake splant sdone
wmenter wmmove wmesc  encaccept encdecline  movdone
sheetopen sheetclose skillup skilldown perkpick tagpick mutpick
rest restopt  elev  sneak  audit
wait gone limbo ok cancel platform
```
New in v0.3: `elev` (elevator panel answer, below), `rest` / `restopt` (player-initiated
rest, below), `sneak` (per-player sneak toggle — key `1` or the skilldex; it is a self-toggle
with no target, which is why it never fitted `skill <netId>`), the sheet edit intents
(below), and `wmenter` now takes an **optional entrance index** so a city's town map can
choose a district (absent = the front door). `audit` is the divergence oracle (§5, debug
verbs).

New in v0.2: `reload`, `unload`, `hand` (weapon-hand swap), `encaccept` / `encdecline`
(random-encounter answer, first answer wins). `invdrop` now takes an optional count.
`stake` / `splant` / `sdone` drive the steal screen (below).

**Character-sheet edit intents** (`sheetopen` … `sheetclose`): what the character screen
sends when a player spends a level-up. One point or one perk per line — the server holds
the entitlement (unspent points, an owed pick, prerequisites) and streams the whole sheet
row back, so the client renders the authoritative answer instead of its own guess. A
refusal arrives as a line on the refusal channel and the row simply does not change.
`tagpick <skillId>` / `mutpick <dropTrait> <gainTrait>` answer the follow-up that Tag! and
Mutate! demand; `-1` there means the player cancelled, which takes the perk back off and
returns the pick. These cost no AP, take no turn, and are allowed while dead or busy.

**Steal / pickpocket / plant** (`stake <pid> [qty]` / `splant <pid> [qty]` / `sdone`): using
the Steal skill on a living critter opens a **server-owned** session — the server rolls the
skill for every transfer, so these verbs carry no target netId (the session already knows
whose pockets they are; letting a client name the victim would make `stake` a reach-into-
anyone primitive). While one is open the world is **parked**, exactly as it is for a trade,
and *every* player gets the screen: the thief drives it, everyone else watches, and each of
them is told `"<thief> is stealing from <victim>."`. A non-thief's verb is refused with
"You're only watching." Getting caught ends the session and runs the victim's reaction.
Operators can drive the whole thing from the console with `stealon` (use Steal on the
nearest living critter) followed by these three — that is what `scripts/steal_smoke.sh`
does.

**`rest <minutes>` / `restopt <option>`**: player-initiated rest, which co-op did not have
at all. The server owns the clock, so a client can only ask — `restopt` carries the pipboy
menu's own kinds, including *until healed* and *until the party is healed*. One clock means
**resting is a group act**: the night passes for everybody and every player heals, and the
others get told who did it. Refused in combat, where vanilla refuses it, and while dead or
busy. Capped at 24 hours per verb, because the whole simulation is parked inside the loop.

**`elev <level>`**: the answer to `EVENT_ELEVATOR_PROMPT`. The server has no elevator screen,
so it streams the panel to the rider's client, which renders **vanilla's own** picker and sends
back the button index. The server then resolves what that button means from its own table and
rides — instantly, as a plain map transition, and **the whole party rides together** (one map,
one camera elevation). A client never names a destination: `elev` is refused unless that actor
was actually offered a panel, and the level must be inside that elevator's button count.

> ⚠ Scripting the admin port with `echo … | nc` **runs** the verb but loses the reply:
> closing stdin half-closes the socket, the server drops the client, and only then
> dispatches the lines it had buffered. Hold the pipe open (`{ printf '…\n'; sleep 3; } |
> nc …`) when you want to read the answer — see `scripts/check_sheet.sh`.

---

## 6. Persistence check

With any server running that has a `CMD` port:
```sh
printf 'save 8\n' | nc -q1 127.0.0.1 9201
```
Quit, then relaunch with `F2_SERVER_LOAD=8` and **no** `F2_PLAYER_CREATE` on the clients.
The same account names come back as the same characters — a co-op save carries every
extra's own body, inventory and sheet in its tail. Autosaves rotate through slots 11-15 (`saves` lists
it; `F2_SERVER_LOAD=11` or `load 11` restores it).

---

## 7. All-in-one — `scripts/viewer_live.sh`

Boots `f2_server` on a map (or slot), waits ~1.5s, launches `VIEWERS` clients (extras join
mid-stream, staggered 5s apart, all windowed). The first viewer quitting kills the server and
the rest. Sets demo defaults: `SMOOTH_WALK=1`, `PRES_RECORD=1`, `RESUMABLE_COMBAT=1`,
`DIALOG_STREAM=1`, `TICKS=500000000` (effectively no cap).

| var | default | meaning |
|-----|---------|---------|
| `MAP` | `artemple.map` | map to boot (ignored if `LOAD` is set) |
| `LOAD` | — | restore slot `1-10`, or `11`-`15` = an autosave, instead of a map |
| `VIEWERS` | `1` | how many clients to launch; >1 ⇒ all windowed |
| `PACE` | `100` | ms wall per beat. `100` ≈ real time, `33` ≈ 3×, `0` = full speed |
| `NAMES` | — | comma list of account names; viewer *i* logs in as the *i*-th |
| `CREATE0`,`CREATE1`,… | — | per-viewer char spec, **0-based by VIEWER, not slot** |
| `HOST` | — | pin slot 0 to this account name |
| `WIRE_PORT` / `CMD_PORT` | `9200` / `9201` | ports |

```sh
scripts/viewer_live.sh                                     # solo, Temple
MAP=kladwtwn.map scripts/viewer_live.sh                    # elsewhere
NAMES="Cahb" CREATE0=ask scripts/viewer_live.sh            # roll one, then join
HOST=Cahb NAMES="Cahb,Mennoc" CREATE0=ask CREATE1=ask VIEWERS=2 scripts/viewer_live.sh
LOAD=8 scripts/viewer_live.sh                              # restore a slot
```

> ⚠ `CREATE<n>` is 0-based and indexes the **viewer**, not the slot. Slots are first-come, so
> with interactive `ask` whoever finishes rolling first takes slot 0 — use `HOST=` to pin it.

---

## 8. Dev / CI-only env (not operator knobs)

These drive goldens, the record-purity harness, offline replay and tracing. Ignore them for
running a real server.

- **Headless probe / harness**: `F2_HEADLESS_PROBE`, `F2_PROBE_ACTIONS`, `F2_PROBE_AGGRO`,
  `F2_PROBE_DUMP`, `F2_PROBE_MAP`, `F2_PROBE_SEED`, `F2_PROBE_TICKS`, `F2_PROBE_LISTMAPS`,
  `F2_FAKE_CLOCK`.
- **Server harness**: `F2_SERVER_ACTIONS` (`tick:verb:arg,…`, no socket), `F2_SERVER_DUMP`,
  `F2_SERVER_BLOB_OUT`, `F2_SERVER_NET_TEE` (tee the wire to a file), `F2_SERVER_LEAKPROBE`,
  `F2_SERVER_LOOP`, `F2_SERVER_SERVE`.
- **Client offline replay**: `F2_CLIENT_BLOB_IN`, `F2_CLIENT_STREAM_IN`, `F2_CLIENT_BLOB_TIME`,
  `F2_NETSTREAM`, `F2_INPUT_RECORD`, `F2_INPUT_REPLAY`.
- **Tracing (stderr)**: `F2_TRACE_EVENTS`, `F2_TRACE_SPATIAL`, `F2_TRACE_GVAR`,
  `F2_TRACE_LVAR`, `F2_TRACE_OPCODE`, `F2_TRACE_AGGRO`, `F2_TRACE_BARTER`, `F2_BARTER_TRACE`,
  `F2_DIALOG_TRACE`, `F2_NARRATE`.
- **Presentation / AV**: `F2_VIEWER_SHOT_EVERY`, `F2_NO_TIMESKIP_COALESCE`,
  `F2_NO_ATTACK_HEADER` (drop the co-op "X throws the Spear at you." line that precedes
  vanilla's damage lines; the damage lines themselves stay personalized either way).

> ⚠ `f2_server` never runs `gameInitWithOptions`, so `gDebugPrintProc` is null and every
> `debugPrint` is **silently dropped** — probe verbs look like they did nothing. Set
> `DEBUGACTIVE=screen` when you need that output.
