# 003 — Containers/chests/crates don't open over the wire

**Status**: OPEN (root-caused, not yet fixed)
**Files**: `src/server_control.cc` (verb routing), `src/proto_instance.cc`
  (`_obj_use_container`), `src/actions.cc` (action dispatch)

## Symptom
Clicking USE/LOOT on a container (wooden crate, footlocker, ice box) does
nothing visible — no open animation, no close animation, no container-inventory
screen. The character walks up to the container but no interaction occurs.

After the dialog streaming work (A3), the loot modal CAN open locally on
the viewer (via `viewerArmPendingLoot` → `viewerPollPendingLoot`). But
without the server running `_obj_use_container`, the container stays
visually closed — open/close animation never plays, frame state never
toggles, `SCRIPT_PROC_USE` never runs.

## Root cause — two independent gaps

### Gap A: `use` wire verb rejects OBJ_TYPE_ITEM

`server_control.cc:613` — the `use` verb gate:
```cpp
if (PID_TYPE(target->pid) != OBJ_TYPE_SCENERY) {
    fprintf(stderr, "f2_server: control %s target netId=%d not scenery\n", verb, netId);
    return;
}
```

Containers are `OBJ_TYPE_ITEM` with `ITEM_TYPE_CONTAINER`, not `OBJ_TYPE_SCENERY`.
The `use` verb rejects them outright. They never reach `kInteractUse`.

The viewer's action-menu mapping (main.cc:734-763) correctly routes containers
to `kInteractLoot` (approach-only, no server outcome), but this just walks the
dude adjacent — no open/close.

### Gap B: No `serverLoopActive()` decouple in `_obj_use_container`

`_obj_use_container` (proto_instance.cc:1867-1947) is only reachable as a
`reg_anim` callback from `actionPickUp` (actions.cc:1522-1549). On the
headless server, `reg_anim` callbacks never fire. The function has no
`serverLoopActive()` branch that applies the open/close directly —
unlike `_obj_use_door` (proto_instance.cc:1809-1831) which DOES have
a headless decouple:

```cpp
// _obj_use_door has this (line 1809):
if (serverLoopActive()) {
    if (!animateOnly) {
        if (door->frame != 0) { _set_door_state_closed(door, door); }
        else { _set_door_state_open(door, door); }
    }
    _check_door_state(door, door);
    if (!animateOnly) {
        doorPresentSlide(door, step > 0, user != nullptr ? user->netId : 0);
    }
}
```

`_obj_use_container` needs a similar decouple that:
1. Checks lock status
2. Toggles frame (open/close)
3. Emits a wire event for the viewer to render the animation
4. Optionally fires `SCRIPT_PROC_USE`

### Gap C: Wire events for container open/close

Even with Gap B fixed, the viewer needs to see the container's new frame.
Unlike doors (which have `EVENT_DOOR_STATE`), containers have no dedicated
event. The frame state change could stream through `OBJECT_DELTA` with a
FID delta, or a new `EVENT_CONTAINER_STATE` event.

The existing `objectOpenClose` (proto_instance.cc:2127-2180) already has a
partial serverLoopActive path that calls `doorPresentSlide` for doors —
containers hit the `_obj_is_openable` check but need their own presentation
emitter.

## Fix plan

### Step 1: Server-side `_obj_use_container` decouple
In `interactionFire`, the `kInteractLoot` case (currently a no-op):
```cpp
case kInteractLoot:
    _obj_use_container(actor, target); // NEW
    break;
```

Then in `_obj_use_container`, add `serverLoopActive()` branch before
`reg_anim_begin`:
```cpp
if (serverLoopActive()) {
    // Apply open/close directly (no animation)
    if (objectIsLocked(item)) return -1;
    bool opening = (item->frame == 0);
    if (item->sid != -1) {
        scriptExecProc(item->sid, SCRIPT_PROC_USE);
    }
    item->frame = opening ? 1 : 0;
    // Wire event for viewer animation
    presenter()->doorState(item, opening, item->frame); // reuse EVENT_DOOR_STATE
    return 0;
}
```

### Step 2: Viewer container animation
The viewer's `onDoorState` already handles frame animation via
`clientDoorAnimPlay` which calls `reg_anim_begin/end`. For containers
(OBJ_TYPE_ITEM with ITEM_TYPE_CONTAINER), this would play the
open/close stand animation. The existing door machinery should work
for containers too — `_obj_use_container` already registers the same
`ANIM_STAND` animation.

### Step 3: Loot modal integration
`viewerPollPendingLoot` already opens the loot modal when the dude
arrives adjacent. The container should be visually open (frame=1) by
then, making the "you search" message feel natural.

### Step 4 (polish): `use` verb allow containers
Relax `PID_TYPE(target->pid) != OBJ_TYPE_SCENERY` in `server_control.cc:613`
to also allow `OBJ_TYPE_ITEM` with `itemGetType(target) == ITEM_TYPE_CONTAINER`.
