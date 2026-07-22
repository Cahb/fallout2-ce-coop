# DEDICATED_HOWTO — launching f2_server & the wire client

Operator reference for the dedicated server (`f2_server`) and the SDL client used as a
network viewer (`fallout2-ce`). Everything here is derived from the code; when you add an
env var or a verb, update this file (grep anchors: `getenv("F2_` in `src/`, the verb tables
in `server_admin.cc` / `command.cc` / `server_control.cc`).

There are three ways in, smallest to largest:
1. **`scripts/viewer_live.sh`** — all-in-one: boots the server *and* one or more viewers,
   tears them all down when the first viewer quits. Best for solo testing on one box.
2. **`f2_server` alone** — a bare dedicated server. Players connect from elsewhere.
3. **`fallout2-ce` alone** — a client that joins a running server as a viewer/player.

All binaries must run with CWD = the game dir (`FO2/`), because they read `master.dat` /
`patch000.dat` relative to CWD. `viewer_live.sh` does the `cd` for you; the bare launches
below assume `cd FO2` first.

---

## 0. Build

```sh
cmake --build build --target f2_server fallout2-ce      # Linux
cmake --build build-win --target f2_server              # Windows cross (mingw, wine-verified)
```
`build/f2_server` and `build/fallout2-ce` are what the scripts expect.

---

## 1. All-in-one — `scripts/viewer_live.sh`

Boots `f2_server` on a map (or slot), waits ~1.5s, launches `VIEWERS` clients (extras join
mid-stream, staggered 5s apart, all windowed). First viewer quitting kills the server and
the rest. It sets sane demo defaults: `PACE=` inherited, `SMOOTH_WALK=1`, `PRES_RECORD=1`,
`RESUMABLE_COMBAT=1`, `DIALOG_STREAM=1`, `TICKS=500000000` (effectively no cap).

### Knobs (env vars *this script* reads — distinct from the raw `F2_*` below)
| var | default | meaning |
|-----|---------|---------|
| `MAP` | `artemple.map` | map to boot (ignored if `LOAD` set) |
| `LOAD` | — | restore save slot `1-10` (or `11` = autosave) instead of a map |
| `VIEWERS` | `1` | how many clients to launch; >1 ⇒ all windowed |
| `PACE` | `100` | ms wall per beat. `100` ≈ real time, `33` ≈ 3× fast, `0` = full speed |
| `NAMES` | — | comma list of account names; viewer *i* logs in as the *i*-th name |
| `CREATE0`,`CREATE1`,… | — | per-viewer char spec, **0-based, indexed by VIEWER not slot** |
| `HOST` | — | pin slot 0 (the host body) to this account name |
| `WIRE_PORT` | `9200` | viewer wire TCP port |
| `CMD_PORT` | `9201` | admin/command TCP port |

`CREATE<n>` is either a stat spec `"S P E C I A L tag tag tag trait trait"` (10+ ints) or
the literal `ask` to roll in vanilla's own character-creation screen (opens before connect).

### Common recipes
```sh
# Plain solo viewer on the Temple map
scripts/viewer_live.sh

# Different map
MAP=kladwtwn.map scripts/viewer_live.sh

# Roll a character interactively, then it joins and the server builds it
NAMES="Cahb" CREATE0=ask VIEWERS=1 PACE=100 MAP=artemple.map scripts/viewer_live.sh

# Two players, each rolling their own, host pinned so map-driving isn't a race
HOST=Cahb NAMES="Cahb,Mennoc" CREATE0=ask CREATE1=ask VIEWERS=2 PACE=100 scripts/viewer_live.sh

# Restore a slot instead of a fresh map (leave MAP unset)
LOAD=8 scripts/viewer_live.sh
```

> ⚠ `CREATE<n>` is 0-based and lines up with the **viewer index**, not the slot. `CREATE0`
> is the first viewer. Slots are first-come, so with interactive `ask` whoever finishes
> rolling first grabs slot 0 (the host body) — use `HOST=` to pin it.

---

## 2. Bare dedicated server — `f2_server`

Run it yourself when players are on other machines, or when you want the server to outlive
any one viewer. Minimum to be useful is a world source **and** a wire port:

```sh
cd FO2
env F2_SERVER_MAP=artemple.map F2_SERVER_NET=9200 F2_SERVER_CMD=9201 \
    F2_SERVER_PACE_MS=100 F2_SERVER_RESUMABLE_COMBAT=1 F2_SERVER_SMOOTH_WALK=1 \
    F2_SERVER_PRES_RECORD=1 F2_DIALOG_STREAM=1 F2_WORLDMAP_STREAM=1 \
    ../build/f2_server                          # F2_SERVER_TICKS unset = never closes
```

### World source — pick exactly one (or neither → lobby)
- `F2_SERVER_MAP=<map.map>` — boot a fresh world on that map.
- `F2_SERVER_LOAD=<1-10>` — restore a save slot. `11` = the autosave slot.
- **Both set → the LOAD wins loudly and the map is ignored** (they are alternatives).
- **Neither set → LOBBY**: requires `F2_SERVER_CMD`; the server waits and greets each
  operator with the slot listing. Pick a world at runtime with `load <n>` / `new <map.map>`.
  Without a command channel and no world, it prints the usage and exits.

### Wire vs command channels
- `F2_SERVER_NET=<port>` — the **viewer wire** (binary stream). At start it *blocks until
  the first client connects*, then serves. More clients join mid-stream. When the last
  client disconnects, the server stops.
- `F2_SERVER_CMD=<port>` — the **admin/command channel** (plain telnet/nc, one
  `verb arg arg2` per line). Independent of the wire, accepts connections any time, and is
  **unrestricted** (full admin + debug vocabulary). This is your operator console.

You can run `F2_SERVER_CMD` **without** `F2_SERVER_NET` for a headless, viewer-less server
you drive entirely by command (handy for scripted testing).

### Players & identity
- `F2_SERVER_PLAYERS=<n>` — pre-spawn an N-body party on a fresh world (default 1, capped
  at `kMaxPlayerActors`). On a co-op *load* the saved extras are restored instead, so this
  is ignored.
- `F2_SERVER_HOST=<name>` — pin slot 0 (the host body — the only one that can drive the
  worldmap / map transitions / dialog) to that account name. Unset = first-come.
- `F2_SERVER_NAME=<name>` — server display name sent in the handshake.
- `F2_REQUIRE_TOKEN=1` — require a matching `F2_PLAYER_TOKEN` from clients to claim.

### Pacing, combat, presentation
| var | default | meaning |
|-----|---------|---------|
| `F2_SERVER_PACE_MS` | `0` (full speed) | ms wall per beat; `100` ≈ real time. Demo/throttle only |
| `F2_SERVER_TICKS` | `0` = **unlimited** | serve forever (until disconnect / terminal quit / `quit`). A **positive** value is a safety cap for headless runs that must terminate |
| `F2_SERVER_KEEPALIVE` | on if `CMD` set | persistent server: don't quit when the last player leaves; idle **frozen** (no sim advance) and accept reconnects. `=0` forces the old exit-on-empty behaviour |
| `F2_MOVIES` | off | let the `movie` verb / script-triggered cutscenes actually project to viewers. Default off on purpose — see the ⚠ below |
| `F2_SERVER_SMOOTH_WALK` | off | animate out-of-combat walks one tile/beat (viewer sees motion) |
| `F2_SERVER_RESUMABLE_COMBAT` | off | beat-spanning combat — **required** for combat presentation & player-started combat |
| `F2_SERVER_PRES_RECORD` | off | presentation record/replay channel (discrete-action animation) |
| `F2_DIALOG_STREAM` | off | dialog + barter block-and-pump (live conversations/trade) |
| `F2_WORLDMAP_STREAM` | off | worldmap block-and-pump (live travel) |
| `F2_AUTOSAVE_SECS` | `300` | autosave interval → SLOT 11; `0` = off. Broadcasts "Game auto-saved." |
| `F2_SERVER_TURN_IDLE_MS` | `60000` | combat: sim-ms a human gets once their turn is on screen |
| `F2_SERVER_TURN_WAIT` | off | force the resumable-combat turn barrier to wait |
| `F2_SERVER_SEED` | — | RNG seed (reproducible worlds/encounters) |

> For a real "play it live" server you almost always want the same bundle
> `viewer_live.sh` sets: `RESUMABLE_COMBAT=1 SMOOTH_WALK=1 PRES_RECORD=1 DIALOG_STREAM=1
> WORLDMAP_STREAM=1 PACE_MS=100`. `TICKS` unset = it never closes on its own.
>
> ⚠ A headless server with **no `NET` and no `CMD` port** and unlimited ticks will loop
> forever at full speed (a spinning CPU core with no way to stop it but a signal). Give it
> at least a `CMD` port so you can `quit`, or a positive `TICKS` cap.

### Lifecycle — when does the server run vs freeze vs stop?
The dedicated server model is **"empty = freeze, player = play, never quit on its own"**:
- **No players connected** → the sim is *frozen*: the game clock, NPCs and the object-id
  budget do not advance (nobody is watching), but the server keeps accepting connections.
  A player connecting/logging in un-freezes it on the next beat.
- **≥1 player** → normal play, paced by `PACE_MS`.
- **Last player leaves** → a keepalive server (default when `F2_SERVER_CMD` is set) goes back
  to frozen-idle, ready for reconnects. A bare demo/probe server (no `CMD`) still exits.
- **Stopping it** → `quit` (or `shutdown`) on the command channel, terminal quit (dude
  death / endgame), or a signal. There is no auto-shutdown for a keepalive server.

> Startup still **blocks for the first wire client** (`F2_SERVER_NET` accept) before serving —
> the world comes alive when the first player joins, then persists across everyone leaving.

### Movies (`movie` verb / script cutscenes)
Projecting a movie to viewers is **off by default** — set **`F2_MOVIES=1` on the server**
(not the client). Without it, `gameMoviePlay` marks the movie seen and returns *without*
sending anything to viewers, so `movie 4` prints "playing… / barrier released" instantly and
nothing shows. Two things must both be true for a movie to actually play:
1. `F2_MOVIES=1` in the **server** env, and
2. **at least one viewer connected** at the moment you trigger it (with none, the barrier
   bails immediately — this is what "0 remain → movie 4 → barrier released" means).

> ⚠ Why it's default-off: `movdone` (the ack that releases the movie barrier) is a wire verb
> only the CLIENT sends — the operator's command channel *cannot* release it. So a viewer that
> renders a black screen instead of the movie leaves the server parked with no escape but a
> restart. Enable `F2_MOVIES` only when you've confirmed playback works on your build/data.

---

## 3. Bare client / viewer — `fallout2-ce`

The normal game binary becomes a network viewer when `F2_CLIENT_CONNECT` is set.

```sh
cd FO2
env F2_CLIENT_CONNECT=127.0.0.1:9200 F2_WINDOWED=1 \
    F2_PLAYER_NAME=Cahb F2_PLAYER_CREATE=ask ../build/fallout2-ce
```

| var | meaning |
|-----|---------|
| `F2_CLIENT_CONNECT=<host:port>` | connect to a server's wire port as a viewer/player |
| `F2_PLAYER_NAME=<name>` | account name to log in as (binds you to that name's character). Unset = legacy bare `claim` |
| `F2_PLAYER_CREATE=<spec>` | first time this name is seen: `"S P E C I A L tags traits"` or `ask` to roll in the creation screen |
| `F2_PLAYER_TOKEN=<tok>` | auth token (needed if the server sets `F2_REQUIRE_TOKEN`) |
| `F2_WINDOWED=1` | windowed (so multiple viewers fit side by side) |
| `F2_JOIN_TMP_CLIENT=<path>` | scratch file for the join blob; give each concurrent viewer its own |

Without any `F2_CLIENT_CONNECT`, `fallout2-ce` is just the normal single-player game.

---

## 4. Runtime command channel (telnet/nc → `F2_SERVER_CMD` port)

One `verb arg arg2` per line. Admin verbs answer you directly; anything else is dispatched
into the sim as a debug verb. Example:
```sh
printf 'status\n'                          | nc -q1 127.0.0.1 9201
printf 'save 8\n'                          | nc -q1 127.0.0.1 9201
printf 'stress 20 0x010000EE 42\n'         | nc -q1 127.0.0.1 9201
printf 'give 41 1\ndrop 41\n'              | nc -q1 127.0.0.1 9201
```

### Admin verbs (answer the operator; from `server_admin.cc`)
| verb | meaning |
|------|---------|
| `saves` | list save slots |
| `save <1-10> [label]` | save the running world into a slot |
| `load <1-10>` | restore a slot (**lobby only**) — `11` = autosave slot |
| `new <map.map>` | boot a fresh world (**lobby only**) |
| `status` | what is running right now |
| `say <chan> <text>` | push a styled line to every viewer's message log |
| `saydemo` | one line per channel (style eyeball test) |
| `movie <0-16>` | project a movie to every viewer (4 = VSUIT) |
| `timeskip <minutes>` | advance the game clock like a script does |
| `spawn <pid> [n] [tile]` | place n critters of pid (default 1, random tile) |
| `stress <n> [pid] [seed]` | spawn n hostiles near the players and aggro them (default pid `0x010000EE` = Raider; seed is printed — reuse to replay) |
| `despawnall` | destroy everything `spawn`/`stress` created |
| `help` / `?` | list verbs + channels |
| `quit` / `shutdown` | stop the server |

### Debug verbs (dispatched into the sim; from `command.cc`)
Movement/combat/inventory pokes — drive the dude or nearest critter and watch the viewers:
```
walk walkto warp critwalk critwarp climb push mark
aggro cattack caim cdamage cmove cendturn hurt poison rad drug dr. useskill useskillon useitem usedoor
give drop pickup wield stow unload reload lootall stealall explode
rest restopt savegame loadgame endgame entermap levelup perk xp mutate charroll charsnap
dtalk dsay dend  wmenter wmmove wmtravel wmesc
```
(Full list lives in `command.cc`; these take `arg`/`arg2` ints, e.g. `walk 40 1` = walk +40
tiles running, `aggro 1` = start combat.)

### Control / wire verbs (`server_control.cc`)
These are what a **connected client** sends over the wire (gated per-session — the trust
boundary), not what you type into the admin console. Listed for reference:
`login claim create cstart cendcombat cendturn cattack cmove mv rot look use useitem
useitemon usedoor open take takeall put get loot invopen invclose invwield invunwield
invdrop skill talk dsay dend dbarter boffer bunoffer btake bcommit bcancel bdone
wmenter wmmove wmesc movdone wait gone limbo ok push`.

---

## 5. Persistence test recipe

With a server running (any launch above, with a `CMD` port):
```sh
printf 'save 8\n' | nc -q1 127.0.0.1 9201        # write slot 8
```
Quit. Relaunch **without** any `CREATE*` vars and with `F2_SERVER_LOAD=8` (or `LOAD=8` under
`viewer_live.sh`). The same account names return as the same characters — co-op saves carry
each extra's own body, inventory and sheet in the save tail. Autosaves land in slot 11
(`F2_SERVER_LOAD=11` / `load 11` to restore, `saves` lists it).

---

## 6. Dev / CI-only env (not for operators — kept here so nobody mistakes them for knobs)

These drive goldens, the record-purity harness, offline replay, and tracing. Ignore for
running a real server.

- Headless probe / harness: `F2_HEADLESS_PROBE`, `F2_PROBE_ACTIONS`, `F2_PROBE_AGGRO`,
  `F2_PROBE_DUMP`, `F2_PROBE_MAP`, `F2_PROBE_SEED`, `F2_PROBE_TICKS`, `F2_PROBE_LISTMAPS`,
  `F2_FAKE_CLOCK`.
- Server harness: `F2_SERVER_ACTIONS` (`tick:verb:arg,…` scripted, no socket),
  `F2_SERVER_DUMP` (state dump path), `F2_SERVER_BLOB_OUT`, `F2_SERVER_NET_TEE` (log the
  wire to a file), `F2_SERVER_LEAKPROBE`, `F2_SERVER_LOOP`, `F2_SERVER_SERVE`.
- Client offline replay: `F2_CLIENT_BLOB_IN`, `F2_CLIENT_STREAM_IN`, `F2_CLIENT_BLOB_TIME`,
  `F2_JOIN_TMP`, `F2_NETSTREAM`, `F2_INPUT_RECORD`, `F2_INPUT_REPLAY`.
- Tracing (stderr): `F2_TRACE_EVENTS`, `F2_TRACE_SPATIAL`, `F2_TRACE_GVAR`, `F2_TRACE_LVAR`,
  `F2_TRACE_OPCODE`, `F2_TRACE_BARTER`, `F2_DIALOG_TRACE`, `F2_BARTER_TRACE`, `F2_NARRATE`.
- Presentation/AV toggles: `F2_MOVIES`, `F2_NO_MUSIC`, `F2_VIEWER_SHOT_EVERY`,
  `F2_NO_TIMESKIP_COALESCE`.
