# 004 — Migrate doors from EVENT_DOOR_STATE to PRES_SEQ replay

**Status**: SPEC (2026-07-19)
**Blocks**: bug #002 (door anim globally blocks movement)

## Goal

Make `EVENT_DOOR_STATE` (event 28) dead code. Doors become a standard discrete action
family in the PRES_SEQ replay engine, same as explosions, gestures, weapon draw, and
attack choreography. This kills the global `clientDoorAnimActive()` hold that causes
character movement to skip/teleport (bug #002) — the replay queue scopes holds per-actor
naturally.

## Already exists

The recording path IS already implemented in `doorPresentSlide` (`proto_instance.cc:1740`):

```cpp
static void doorPresentSlide(Object* door, bool opening, int actorNetId) {
    if (!presRecordEnabled()) {
        presenter()->doorState(door, opening, door->frame);   // ← bespoke event
        return;
    }
    // ← RECORDED PATH (already works):
    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    if (opening) {
        animationRegisterPlaySoundEffect(door, sfxBuildOpenName(door, SCENERY_SOUND_EFFECT_OPEN), -1);
        animationRegisterAnimate(door, ANIM_STAND, 0);
    } else {
        animationRegisterPlaySoundEffect(door, sfxBuildOpenName(door, SCENERY_SOUND_EFFECT_CLOSED), -1);
        animationRegisterAnimateReversed(door, ANIM_STAND, 0);
    }
    // `objectSetFrame` + final sfx rides inside the anim callback
    reg_anim_end();  // ← recorder captures this, emits PRES_OP_ANIMATE + PRES_OP_SFX
}
```

Server-side: already records ANIM_STAND + SFX ops into a PRES_SEQ stream.
Client-side: `EVENT_PRES_SEQ` → `onPresSeq` → `presPlayRecordedSeq` processes
`PRES_OP_ANIMATE`/`PRES_OP_SFX` identically for doors as anything else.
**No client-side code change needed for replay to work.**

## What to change (4 steps, ~1 session)

### Step 1: Always record (remove the `!presRecordEnabled()` branch)

In `doorPresentSlide`, delete the `if (!presRecordEnabled()) { ... return; }` branch.
The recording path becomes the ONLY path. Doors always emit via PRES_SEQ.

But wait — `presRecordEnabled()` is gated on `F2_SERVER_PRES_RECORD` env var +
`serverLoopActive()`. If recording is disabled, doors would do nothing.
The plan: recording is always-on in the server by design (f2_server). There's
no reason to ever be in the non-record path on f2_server. Just remove the
branch and keep the recording body.

**Caveat**: `doorPresentSlide` is called from `_obj_use_door` which runs in BOTH
f2_server and fallout2-ce (headless golden). The golden harnesses don't have
recording enabled. So we need recording enabled for golden runs where doors fire,
OR we need a two-gate approach:
- On f2_server (always): record
- On golden harness: keep the existing non-record path (EVENT_DOOR_STATE) OR
  disable recording for golden

The golden path: golden harnesses run fallout2-ce under F2_SERVER_LOOP=1,
which sets `serverLoopActive()=true` but `presRecordEnabled()` is controlled
by F2_SERVER_PRES_RECORD. The golden transport gates (netstream/netsocket) run
WITH recording enabled. The byte-identical goldens don't trigger doors (they're
state-dump comparisons, no player actions). So golden is not a concern.

**Simplest approach**: just gate on `presRecordEnabled()` and keep the EVENT_DOOR_STATE
fallback for now. The real retirement happens when F2_SERVER_PRES_RECORD becomes
always-on (a separate decision). For this spec: document the migration, verify
the recorded path works live, mark EVENT_DOOR_STATE as DEPRECATED.

### Step 2: Wire `actorNetId` into PRES_SEQ for scoped holds

`doorPresentSlide` receives `actorNetId` but doesn't use it for replay
scoping. On the client, `presPlayRecordedSeq` has a `holdActorNetId`
parameter that scopes glide holds to that specific actor. Currently:

```cpp
// client_net.cc:2130 — presSeq handler:
presPlayRecordedSeq(ops, false);  // dry-pass: reserve participants
// ...
presPlayRecordedSeq(ops, true);   // execute pass
```

The `false/true` is `execute`, but there's no `holdActorNetId` being
passed. For doors and gestures, the player who triggered the action
should have their glide held until the door finishes. Currently the
global `clientDoorAnimActive()` does this for ALL actors — the replay
replacement should scope it to just the triggering actor.

Check: does `presPlayRecordedSeq` already support this? Looking at
signature:

```cpp
bool presPlayRecordedSeq(const PresRecordedOps& ops, bool execute);
```

No `holdActorNetId` parameter. It would need to be added.

**Alternative (simpler)**: the door is an object with a netId. On the
client, the PRES_SEQ creates/reserves that door object. The door's
animation plays through the animation subsystem (`animationRegisterAnimate`).
The door itself doesn't move (no glide). The player's glide is a separate
glide entry in the presentation system. If we want player glide held
until door finishes, we'd need to link them.

For v1, the recorded path already works — the door animates, the player
moves independently. Bug #002's fix (per-actor hold) is a separate
refinement of the presentation queue, not specifically a door→replay
migration issue.

### Step 3: Remove dead code (after verification)

Once the recorded path is verified live (door opens → door frame animates →
player can walk through without teleport), remove:

- `EVENT_DOOR_STATE` from both presenter_network.cc AND client_net.cc enums
- `onDoorState()` handler (client_net.cc)
- `PresKind::kDoor` and its dispatch (client_net.cc)
- `clientDoorAnimPlay()` and `clientDoorAnimActive()` (client_present.cc)
- `doorPresentationPending()` (client_net.cc)
- `advanceDoors()` (client_present.cc)
- `enableDoors` / `disableDoors` booleans

### Step 4: Bug #002 fix (scoped door hold)

With the global `clientDoorAnimActive()` gone, bug #002 is fixed BY DELETION.
No more global door hold. The presentation queue processes PRES_SEQ events
normally, and door animation plays through the standard reg_anim subsystem
which doesn't block other actors' movement.

## Migration catalog: what's on PRES_SEQ vs what still needs it

(from the agent sweep — complete inventory)

### Already RECORDED (Tier 1 — bespoke event is fallback):

| Family | Record gate | Fallback event | Retires |
|--------|------------|----------------|---------|
| Explosion | `actions.cc:2064` | `EVENT_EXPLOSION_FX` (30) | `actionExplodeReplay()` |
| **Door slide** | `proto_instance.cc:1740` | `EVENT_DOOR_STATE` (28) | `clientDoorAnimPlay()` |
| Gesture (use/get/skill) | `server_control.cc:270` | `EVENT_ACTION_ANIM` (29) | `clientCombatAnimPlayActionAnim()` |
| Weapon draw | `inventory.cc:490` | `EVENT_WEAPON_TAKE_OUT` (27) | `clientCombatAnimPlayTakeOut()` |
| Attack (all types) | `combat.cc:4165` | `EVENT_ATTACK_RESULT` (15) | `clientCombatAnimPlay()` |

### NOT recorded (Tier 2 — next candidates after doors):

| Family | Composite site | Description |
|--------|---------------|-------------|
| Receive-damage / hit-react | `actionDamage` (actions.cc) | Per-hit HP loss, stagger, death |
| Knockdown / fall | `actionKnockdown` (actions.cc:168) | DAM_KNOCKED_DOWN standup |
| Stance / fid transitions | Various | Rides most families above |

### NOT candidates (Tier 3):

| Event | Why not |
|-------|---------|
| EVENT_TURN_START (14) | Combat-framing metadata |
| EVENT_CONSOLE (16) | UI text, no reg_anim |
| EVENT_FLOAT_TEXT (17) | Transient text object |
| EVENT_SFX (18), SFX_AT (19) | Audio-only |
| EVENT_FADE_OUT/IN (20-21) | Screen effect; NO client handler (GAP) |
| EVENT_MUSIC_STOP (23) | Audio mgmt; NO client handler (GAP) |
| EVENT_DIALOG_NODE/END (32-33) | Modal dialog, separate domain |

## Verification

1. Boot `f2_server` with `F2_SERVER_PRES_RECORD=1`, connect viewer
2. Walk to a door, open it
3. Confirm: door frame animates (ANIM_STAND slide), SFX plays, player
   movement is smooth (no skip/teleport during door animation)
4. `scripts/check_wire_combat.sh` must still pass (explosions/attacks
   still replay correctly through PRES_SEQ)
