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

### Server — everything on

```sh
cd FO2
env F2_SERVER_MAP=artemple.map \
    F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
    F2_SERVER_PACE_MS=100 \
    F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 F2_SERVER_PRES_RECORD=1 \
    F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 \
    F2_MOVIES=1 \
    F2_AUTOSAVE_SECS=300 \
    ../build/f2_server
```

That is the full feature set. Every one of those five stream/record toggles is **off by
default** and each one silently removes a whole class of play if you forget it:

| leave it out | what breaks |
|--------------|-------------|
| `F2_SERVER_RESUMABLE_COMBAT` | no combat presentation, no player-started combat |
| `F2_SERVER_PRES_RECORD` | discrete actions have no animation (pickups, reload gesture, throws) |
| `F2_SERVER_SMOOTH_WALK` | out-of-combat walkers teleport instead of walking |
| `F2_DIALOG_STREAM` | no live conversations and no barter |
| `F2_WORLDMAP_STREAM` | no travel, and therefore no random encounters |
| `F2_MOVIES` | cutscenes are marked seen and never shown (read the ⚠ in §2.6 first) |

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
- `F2_SERVER_LOAD=<1-11>` — restore a save slot instead. **`11` is the autosave slot.**
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

### 2.4 Feature toggles — all default OFF
| var | meaning |
|-----|---------|
| `F2_SERVER_RESUMABLE_COMBAT` | beat-spanning combat. **Required** for combat presentation and player-started combat |
| `F2_SERVER_PRES_RECORD` | presentation record/replay channel — the animation for every discrete action |
| `F2_SERVER_SMOOTH_WALK` | animate out-of-combat walks one tile per beat |
| `F2_DIALOG_STREAM` | dialogue + barter block-and-pump (live conversations and trade) |
| `F2_WORLDMAP_STREAM` | worldmap block-and-pump (live travel, car travel, random-encounter prompts) |
| `F2_MOVIES` | actually project cutscenes to viewers — see §2.6 |

### 2.5 Pacing, timing, persistence
| var | default | meaning |
|-----|---------|---------|
| `F2_SERVER_PACE_MS` | `0` (full speed) | ms wall-clock per beat; `100` ≈ real time. Use `100` for play, lower to fast-forward |
| `F2_SERVER_TICKS` | `0` = **unlimited** | serve forever. A positive value is a safety cap for runs that must terminate |
| `F2_SERVER_KEEPALIVE` | on if `CMD` set | persistent server: don't quit when the last player leaves; idle **frozen** and accept reconnects. `=0` restores exit-on-empty |
| `F2_AUTOSAVE_SECS` | `300` | autosave interval → **slot 11**; `0` = off. Also fires on every **map change**, and an interval save that comes due at an unsafe moment lands on the first safe beat. Broadcasts "Game auto-saved." |
| `F2_SERVER_TURN_IDLE_MS` | `60000` | combat: sim-ms a human gets once their turn is on screen |
| `F2_SERVER_TURN_WAIT` | off | force the resumable-combat turn barrier to wait |
| `F2_SERVER_ACTION_GATE` | **on** | per-player action pacing (an action is gated for as long as the animation it produced runs). `=0` disables — the only kill switch here that defaults ON |
| `F2_SERVER_OUTBOX_PACE` | off | **experimental**: meter wire emission from animation durations instead of firing everything at once. In-combat only; out-of-combat stays realtime. Also switches combat glides to frame-true pacing (walk 371ms/run 125ms per tile vs the rounded 400/100) |
| `F2_SERVER_SEED` | — | RNG seed, for reproducible worlds and encounters |

### 2.6 Movies
Off by default. Two things must **both** be true for a cutscene to play:
1. `F2_MOVIES=1` in the **server** env (not the client), and
2. **at least one viewer connected** when it triggers — with none the barrier bails at once.

Without `F2_MOVIES`, `gameMoviePlay` marks the movie seen and returns without sending
anything, so `movie 4` prints "playing… / barrier released" instantly and nothing shows.

> ⚠ Why it defaults off: `movdone` — the ack that releases the movie barrier — is a wire
> verb only the CLIENT sends, so the operator console **cannot** release it. A viewer that
> renders black instead of the movie leaves the server parked with no escape but a restart.
> Turn it on once you've confirmed playback works on your build and data.

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
| `F2_UNLOCK_CAMERA=1` | unlock the map-edge camera leash (free scrolling) |
| `F2_NO_MUSIC=1` | mute music |
| `F2_JOIN_TMP_CLIENT=<path>` | scratch file for the join blob — **give each concurrent client on one box its own** |

The client sends its OS in the handshake, so joins are announced as
"X joined the game (Linux/Windows/…)".

### In-game keys worth knowing
| key | effect |
|-----|--------|
| `Left Alt` | toggle the **highlight-lootables** overlay: ground items, corpses, containers (including scenery lockers/safes/desks), and exits — doors, stairs, ladders, elevators. Purely local, never saved |
| `B` | swap active weapon hand (costs no AP, plays the put-away/take-out animation) |

---

## 4. Recipes

```sh
# ── NEW GAME on the Temple map, everything on (the §1 line, condensed)
cd FO2 && env F2_SERVER_MAP=artemple.map F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 \
  F2_SERVER_PRES_RECORD=1 F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 F2_MOVIES=1 \
  ../build/f2_server

# ── LOAD SAVE SLOT 8 (note: no F2_SERVER_MAP — they are alternatives)
cd FO2 && env F2_SERVER_LOAD=8 F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 \
  F2_SERVER_PRES_RECORD=1 F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 F2_MOVIES=1 \
  ../build/f2_server

# ── LOAD THE AUTOSAVE (slot 11) — same thing, slot 11
cd FO2 && env F2_SERVER_LOAD=11 F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
  F2_SERVER_PACE_MS=100 F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 \
  F2_SERVER_PRES_RECORD=1 F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 F2_MOVIES=1 \
  ../build/f2_server

# ── LOBBY: decide the world at runtime (neither MAP nor LOAD)
cd FO2 && env F2_SERVER_NET=9200 F2_SERVER_CMD=9201 F2_SERVER_PACE_MS=100 \
  F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 F2_SERVER_PRES_RECORD=1 \
  F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 F2_MOVIES=1 ../build/f2_server
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

# ── TWO CLIENTS on one box (each needs its OWN join scratch file)
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Cahb \
  F2_JOIN_TMP_CLIENT=/tmp/join_a F2_WINDOWED=1 ../build/fallout2-ce &
cd FO2 && env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_PLAYER_NAME=Mennoc \
  F2_JOIN_TMP_CLIENT=/tmp/join_b F2_WINDOWED=1 ../build/fallout2-ce &

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
| `load <1-11>` | restore a slot (**lobby only**); `11` = autosave |
| `new <map.map>` | boot a fresh world (**lobby only**) |
| `status` | what is running right now |
| `say <chan> <text>` / `saydemo` | push a styled line to every viewer's log / style eyeball test |
| `movie <0-16>` | project a movie to every viewer (`4` = vault-suit intro) |
| `timeskip <minutes>` | advance the game clock like a script does |
| `spawn <pid> [n] [tile]` | place n critters of pid |
| `stress <n> [pid] [seed]` | spawn n hostiles near the players and aggro them (default pid `0x010000EE` = Raider; the seed is printed — reuse it to replay) |
| `despawnall` | destroy everything `spawn`/`stress` created |
| `revive <slot>` | bring a dead player back at 1 HP (`0` = host, `1..` = extras) |
| `help` / `?` | list verbs + channels |
| `quit` / `shutdown` | stop the server |

### Debug verbs — dispatched into the sim (`command.cc`)
```
walk walkto warp critwalk critwarp climb push mark
aggro cattack caim cdamage cmove cendturn hurt poison rad drug useskill useskillon useitem usedoor
give drop pickup wield stow unload reload lootall stealall explode
rest restopt savegame loadgame endgame entermap levelup perk xp mutate charroll charsnap
dtalk dsay dend   wmenter wmmove wmtravel wmesc
```
These take `arg`/`arg2` ints — e.g. `walk 40 1` = walk 40 tiles running, `aggro 1` = start
combat.

### Control / wire verbs (`server_control.cc`)
What a **connected client** sends over the wire, gated per-session (the trust boundary) —
listed for reference, not for typing into the console:
```
login claim create  cstart cendcombat cendturn cattack cmove caim  mv rot push
look use useitem useitemon usedoor open take takeall put get loot
invopen invclose invwield invunwield invdrop unload reload hand skill
talk dsay dend dbarter  boffer bunoffer btake bcommit bcancel bdone
wmenter wmmove wmesc  encaccept encdecline  movdone
wait gone limbo ok cancel platform
```
New in v0.2: `reload`, `unload`, `hand` (weapon-hand swap), `encaccept` / `encdecline`
(random-encounter answer, first answer wins). `invdrop` now takes an optional count.

---

## 6. Persistence check

With any server running that has a `CMD` port:
```sh
printf 'save 8\n' | nc -q1 127.0.0.1 9201
```
Quit, then relaunch with `F2_SERVER_LOAD=8` and **no** `F2_PLAYER_CREATE` on the clients.
The same account names come back as the same characters — a co-op save carries every
extra's own body, inventory and sheet in its tail. Autosaves land in slot 11 (`saves` lists
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
| `LOAD` | — | restore slot `1-10`, or `11` = autosave, instead of a map |
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
  `F2_SERVER_LOOP`, `F2_SERVER_SERVE`, `F2_SHEET_TMP`.
- **Client offline replay**: `F2_CLIENT_BLOB_IN`, `F2_CLIENT_STREAM_IN`, `F2_CLIENT_BLOB_TIME`,
  `F2_JOIN_TMP`, `F2_NETSTREAM`, `F2_INPUT_RECORD`, `F2_INPUT_REPLAY`.
- **Tracing (stderr)**: `F2_TRACE_EVENTS`, `F2_TRACE_SPATIAL`, `F2_TRACE_GVAR`,
  `F2_TRACE_LVAR`, `F2_TRACE_OPCODE`, `F2_TRACE_AGGRO`, `F2_TRACE_BARTER`, `F2_BARTER_TRACE`,
  `F2_DIALOG_TRACE`, `F2_NARRATE`.
- **Presentation / AV**: `F2_VIEWER_SHOT_EVERY`, `F2_NO_TIMESKIP_COALESCE`.

> ⚠ `f2_server` never runs `gameInitWithOptions`, so `gDebugPrintProc` is null and every
> `debugPrint` is **silently dropped** — probe verbs look like they did nothing. Set
> `DEBUGACTIVE=screen` when you need that output.
