# 007 — Character creation on connect to server

**Status**: SPEC (2026-07-19)
**Depends on**: multi-player actor binding (M0 from #008)

## Current state

On join, the viewer receives a premade character (build 304, "Narg").
This is a single hardcoded pid in `server_loop.cc` → `character_transaction.cc`
→ `protoInstanceCreatePidSet(304, ...)`. No name, stat, or appearance
selection is available.

The viewer's `mainClientViewer` directly enters the game loop — the character
selection/creation UI (`character_selector.cc`) is never called.

## Requirements

Like vanilla:
1. **Character creation menu**: full SPECIAL/Skills/Name/Age/Gender/Traits
   screen, identical to vanilla `character_editor.cc`.
2. **Stats streamed to server**: after creation, the selected stats (SPECIAL
   base values, tagged skills, traits, name, gender, age) must be sent to the
   server so it can build the authoritative character.
3. **Name**: the critter should display the chosen name (float-over, combat
   messages, dialog). Vanilla stores the name in `proto_instance` → `pcSetStat`.
4. **Premade option**: load a premade character from a `.gcd` save file (like
   vanilla "Premade Characters" menu).

## Architecture — where the creation runs

Vanilla's character creation is a LOCAL client operation:
1. `character_editor.cc` runs a modal loop — player picks stats, name, etc.
2. Stats stored in `pcSetStat` / `CharacterSnapshot` — local-only.
3. `protoInstanceCreatePidSet(pid, ...)` creates the dude object with those stats.
4. The game begins.

For the wire server, the creation must be CLIENT-SIDE only — the server can't
run the SDL character editor. The viewer builds the character locally, then
sends the stats to the server which creates the authoritative actor.

### Flow

```
Viewer                            Server
  |                                 |
  |-- connect -------------------->|  (TCP connect to wire port)
  |<-- CHARACTER_EDIT ------------|  (server says: "create a character")
  |                                 |
  | [character_editor.cc runs]    |
  | [player picks stats/name]     |
  |                                 |
  |-- CREATE_ACTOR {stats} ------>|  (send SPECIAL/skills/traits/name)
  |                                 |  (server creates dude from proto + overrides)
  |<-- JOIN_BLOB -----------------|  (normal join: map + actors)
  |                                 |
  | [main loop enters]            |
```

### Wire protocol for character data

New wire message `EVENT_CHARACTER_EDIT` (from server → viewer: "show character editor")
and response `CREATE_ACTOR` (from viewer → server: "here's my character").

Actually, the simplest approach: the viewer ALWAYS runs the character editor
before connecting. The character data is sent as part of the JOIN request (or
as a first message after TCP connect). The server reads the character data,
creates the dude, then sends the join blob.

```cpp
// Viewer sends (after TCP connect, before blob):
//   u8:  message type = MSG_CHARACTER_CREATE (≠ any EVENT type)
//   str: character name (null-terminated, u16 length)
//   u8:  gender
//   u8:  age
//   u8:  tag-skill-count
//   u16[tagCount]: tagged skill IDs
//   u8:  trait-count
//   u16[traitCount]: trait IDs
//   i32[7]: SPECIAL base values (ST,PE,EN,CH,IN,AG,LU)
// Server creates dude, sends join blob normally.
```

Alternatively, re-use the `character_transaction.h` `CharacterSnapshot` struct
which already captures: hitPoints, base SPECIAL, tagged skills, traits, name,
gender/age. Serialize this struct as a binary blob and send it.

### Demoted for v1 / demo 0.2v

The user said:
> "we'll not implement character creation at this point; initially - just spawn
> premade like now Narg character or whatever"

So full character creation is POST-demo. For v0.2, the spec documents what
needs to happen but the implementation can start simple:

**Demo 0.2v approach**: spawn additional premade characters (same as Narg but
with different names/appearances). No viewer-side editor.

**Post-demo**: implement the full character creation flow described above.

## Staged plan

### Stage C0 — Premade multi-character (~1 session, demo 0.2v)
- Server reads `F2_SERVER_PLAYERS=N`
- Creates N dude actors, each from a different premade pid (304, 305, 306...)
  or from the same pid with different appearance randomization
- Each has a unique name ("Player 1", "Player 2", etc.)
- Viewer gets assigned the next available actor on connect

### Stage C1 — Character stats wire format (~1 session)
- Define the wire format for character stats (re-use `CharacterSnapshot`)
- Server-side: `CharacterSnapshot → protoInstanceCreatePidSet` with stat overrides
- Viewer-side: serialize `CharacterSnapshot` after editor completes

### Stage C2 — Viewer character creation UI (~2-3 sessions)
- Run `character_editor.cc` on connect (before game loop)
- Pre-populate with default Narg stats or a premade template
- On "Done", serialize and send to server
- On "Premade", pick from `.gcd` files, serialize and send

### Stage C3 — Name display (~1 session)
- Set `critterSetName(actor, name)` on server after creation
- `EVENT_FLOAT_TEXT`/`EVENT_CONSOLE` already carry text — names will
  appear in combat messages automatically
- Float-over name bar (the isometric name tag) might need a separate
  delta or be computed client-side from the proto name

## Verification
```bash
# Boot server with 3 players, connect 3 viewers
F2_SERVER_PLAYERS=3 bash scripts/viewer_live.sh
# Each viewer should have a distinct actor with its own inventory, HP, position
# All act independently in combat
```
