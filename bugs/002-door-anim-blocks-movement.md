# 002 — Door animation globally blocks all character movement

**Status**: FIXED (2026-07-19, replay unification — `0bf583a`)
**Files**: `src/client_present.cc` (deleted), `src/client_net.cc` (deleted)

## Symptom
When a door opens anywhere on the map, the character's movement sprite
freezes in place while the door slides. When the door finishes, the character
either rushes through accumulated hops or teleports to the authoritative
position (visual skip).

## Root cause
The door slide mechanism has a GLOBAL lock — `clientDoorAnimActive()` scans
ALL presentation entries, and any active door blocks the ENTIRE presentation
pump. There is no per-actor discrimination.

### Trace
1. Server: `_obj_use_door` → `doorPresentSlide` → `EVENT_DOOR_STATE` (with
   `actorNetId` passed but DISCARDED by `onDoorState` in client_net.cc:2168-2185)
2. Client: `onDoorState` → enqueues `PresKind::kDoor` (only stores doorNetId,
   opening, targetFrame — loses actorNetId)
3. Client pump `presentationTick()`: checks `clientDoorAnimActive()` → true →
   breaks the pump loop → holds ALL queued events including `kMoveRelease`
4. New MOVE events: `doorPresentationPending()` returns true → `holdGlide=true`
   → character sprite parked at origin via `walkApplyOffset(e, 0)`
5. Authority keeps advancing: `objectSetLocation` sets the real tile BEFORE
   `clientAnimOnMove`, so obj->tile keeps moving while sprite is frozen
6. Door finishes: `advanceDoors` clears `doorActive` → lock released
7. Accumulated movement rushes/teleports: if hop count exceeds
   `kMaxQueuedHops=4`, `endGlide` → `walkSnapToAuthority` snaps the sprite

### Key locations
| File | Line | Role |
|------|------|------|
| `client_present.cc` | 1612 | `clientDoorAnimPlay` sets `doorActive=true` |
| `client_present.cc` | 1596-1610 | `clientDoorAnimActive` — GLOBAL scan |
| `client_net.cc` | 420-428 | Pump block: global door check |
| `client_net.cc` | 2146-2162 | `doorPresentationPending` — GLOBAL check |
| `client_net.cc` | 861-891 | MOVE decoder: `holdGlide` when door pending |
| `client_present.cc` | 666-686 | Starved hold: sprite parked at origin |
| `client_present.cc` | 1042-1059 | CAP-ERASE: `kMaxQueuedHops=4` overflow |
| `client_present.cc` | 367-375 | `walkSnapToAuthority` — teleport to final tile |

## Fix direction
The `onDoorState` handler should capture the `actorNetId` from the wire event
(presenter_network.cc already emits it as `putI32(netIdOf(speaker))` —
actually it's `actorNetId` from `doorPresentSlide`), and `doorPresentationPending`
should scope its hold to only that actor's movement, not globally.

Alternatively: add `actorNetId` to `PresEvent` for kDoor, and change
`doorPresentationPending`/`clientDoorAnimActive` to accept an optional
netId filter. Existing call sites that need global checks (pump) can pass
0/no-filter; MOVE decoders pass the mover's netId.

## Impact
Blocking for smooth gameplay — any door interaction causes visible
teleport/skip for the controlling player's character.
