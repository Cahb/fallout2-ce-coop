---
name: presenter-object-ref-model
description: REFERENCE primer for the sequence/record presenter — the netId / NO_SAVE / serverLoopActive / OBJ_CREATE object-reference substrate every session re-derives. Read this BEFORE spelunking object.cc again.
metadata: 
  node_type: memory
  type: reference
  originSessionId: b340e3c8-28f5-4753-b81a-aba378da8790
  modified: 2026-07-18T12:16:14.797Z
---

The substrate for [[pres-record-build-track]] / [[p5-server-plan]]. Every session that
touches the sequence presenter re-derives these SAME basics from object.cc; this is the
decoder ring so the next one doesn't. Line numbers are anchors, re-verify if code moved.

## netId — the wire identity (NOT the sim `id`)
- Fresh object: `netId = 0` (object.cc:3475, in objectAllocate memset+init). "0 = outside
  the syncable domain / unaddressable."
- `objectCreateWithFidPid` assigns a netId **only when `serverLoopActive()`** (object.cc:762-764).
  So in a NON-server binary a created object keeps netId 0.
- `objectNextNetId()` = `gNextNetId++` (POST-increment, object.cc:4495). `objectGetNextNetId()`
  = peek without incrementing (the recorder's watermark uses this).
- `objectFindByNetId(n)`: `n <= 0` never matches a real object (returns nullptr). Viewer uses
  this to resolve a wire ref.
- Rebaseline (join) renumbers persistent objects LOW & monotonic: `gDude` = netId 1
  (object.cc:3392 / 4536). Inventory items get netIds too (4515).

## NO_SAVE is NOT "transient" — the trap that cost slice-2 a bug
- `OBJECT_NO_SAVE = 0x04` (obj_types.h:58). `OBJECT_HIDDEN = 0x01`. So flags `0x5` = a hidden
  NO_SAVE transient (explosion cloud / synthetic attacker). `OBJECT_LIGHT_THRU = 0x20000000`
  rides on normal critters — a persistent-object tell in flag dumps.
- **`gDude` and `gEgg` are NO_SAVE** (created object.cc:313/327; NO_SAVE per :316) yet fully
  persistent + syncable. gDude saves via a special path. => NO_SAVE cannot discriminate
  "transient the viewer lacks" from "the player". Using it as the discriminator mis-minted
  gDude as a duplicate (the old resolveRef bug, [[pres-record-build-track]] slice 2).
- Save dump SKIPS NO_SAVE objects (why the purity gate also counts no_save leaks separately).
- At rebaseline, NO_SAVE objects get `netId = 0` so the syncable set owns the nonzero range
  (object.cc:4552-4561). NO_SAVE objects are NOT replicated to viewers → **a viewer can never
  resolve a NO_SAVE object's netId** (it never received a spawn for it). This is the crux below.

## serverLoopActive() — the one gate that means three things
True in BOTH f2_server AND the headless golden state-dump harness. It governs:
1. netId assignment on create (above).
2. The `!serverLoopActive()` skip of every composite's ANIMATE branch (actions.cc etc.) — the
   server/headless path takes the STATE fast-path instead, which applies the outcome directly.
3. Which fast-path arm runs (the `else if (serverLoopActive())` STATE blocks).
Record mode = `serverLoopActive() && presRecordEnabled()` → RUN the normally-skipped animate
branch inside a record section, then STILL run the STATE block (callbacks are DROPped).

## The recorder's object-ref rule (pres_record.cc resolveRef) + THE GAP
- `transient = obj->netId == 0 || obj->netId >= gSectionNetIdWatermark`. Transient →
  negative stream handle + lazy `OBJ_CREATE{handle,fid,tile,elev,flags}`; else → ship `netId`.
- Watermark = `objectGetNextNetId()` snapshotted at **SectionBegin** (pres_record.cc:142).
- **THE GAP (ground-truthed 2026-07-18 by tracing a live explode:200):** composites create
  their transients BEFORE opening the reg_anim section (actionExplode makes the 7 clouds at
  :1790-1818, SectionBegin is later at :1871). So on the server those clouds already hold
  netIds BELOW the watermark (traced: clouds netId 3336-3342, watermark 3343) → they ship by
  **netId, not OBJ_CREATE**. And being NO_SAVE they were never replicated → the viewer's
  `objectFindByNetId` returns nullptr → **those ops are silently dropped on the viewer**
  (every interpreter op is `if (o) animationRegister…`, client_net.cc — unresolvable = skip,
  no crash). Net: **OBJ_CREATE is effectively DEAD CODE for composite transients today**; it
  only fires for objects created strictly AFTER SectionBegin, which the current composites
  never do. The spec §6.5 worked example (cloud as `OBJ_CREATE{-1}`) is ASPIRATIONAL, not
  what runs.
- **Consequence for any new family:** only refs to PERSISTENT objects (real low netId — the
  victim/defender/door) actually replay. A synthetic/transient object's ops (its SFX, its own
  hide) are dropped on the viewer. The explosion "works" because its real seq payload is the
  VICTIM death anims (persistent); the cloud rides the separate explosionFx CUE, not the seq.
- **To make a pre-section transient replay** (e.g. play a synthetic attacker's whc1xxx1 on the
  viewer): set `obj->netId = 0` before it is referenced (netId 0 → transient branch → OBJ_CREATE),
  OR create it after SectionBegin. Purity-neutral (NO_SAVE, same-beat destroyed). Not yet done
  for any family — first family that wants it also becomes OBJ_CREATE's first real end-to-end test.

## Viewer side (client_net.cc presPlayRecordedSeq / resolveSeqRef)
- ref > 0 → `objectFindByNetId` (nullptr if the viewer lacks it); ref < 0 → per-stream
  `handles` map (minted by OBJ_CREATE); ref == 0 → null.
- Interpreter already executes ALL of Table A. Migrating a family adds a SERVER record site
  (+ maybe a callback tag), NEVER a new viewer op handler.

## Backend split (also in [[pres-record-build-track]], repeated because load-bearing)
fallout2-ce links animation.cc (REAL reg_anim). f2_server links server_anim.cc (recording
leaves). Mutually exclusive (both define reg_anim_begin). Recorder armed only in f2_server
(presRecordSetBackendActive(true) at static init) → inert + goldens byte-identical in the
client/golden binary.
