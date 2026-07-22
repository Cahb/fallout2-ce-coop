# CLIENT_JOIN_DESIGN.md — STEP 4 "MAKE IT JOINABLE": the first real client

Doc of record for P5 STEP 4 (see `MP_PROTOCOL.md` §7b, `[[p5-server-plan]]`).
Companion to `SERVER_LOOP_DESIGN.md`. STEP 3 (socket transport, both ways) is
done; this is the client that connects, becomes present on the map, and tracks
the server live.

> **Status:** design reviewed by a Fable adversarial pass (2026-07-15). Core
> architecture confirmed sound; the review's four BLOCKERs (B1–B4) and should-fix
> items are folded in below and marked ◆. Build next in a fresh session.

## 0. Goal & scope

- **STEP 4 deliverable:** one real SDL `fallout2-ce` client connects to a running
  `f2_server`, receives a binary **join snapshot**, becomes fully present on the
  server's current map, then applies the live wire stream and **renders** the
  world as a **read-only viewer** (the player watches; no local sim authority).
- **STEP 5 (next):** N viewers. The server already keeps a `_clients` vector and
  broadcasts to all; going 1→N is near-trivial *for viewers* — SUBJECT TO the
  rebaseline broadcast rule (§C.4). No party system.
- **NOT this step (co-op, controlled 2nd actor):** blocked on the `gDude`
  singleton (OPEN RISK b, Phase-4 Character/World aggregates), NOT the party
  system — per `[[mp-actor-architecture-principle]]`, party/leash is a removable
  v1 behavior, never the player's identity. Co-op = de-singleton the actor.

The viewer is the honest v1 of "one authoritative dude + N read-only viewers"
(OPEN RISK b). It is also the first binary with a real correctness oracle beyond
`state_dump`: **its rendered world must match the server's**, driven end-to-end.

## 1. The five components

### A. Client connection (mirror of `SocketByteSink`)
New `f2_client` TU `src/client_net.{h,cc}`. A `ByteSource` (the read dual of
`ByteSink`): `connect(host, port)`, non-blocking `poll()` that recvs into a
growing buffer, and a frame extractor. TCP, `TCP_NODELAY`, `SIGPIPE` ignored —
same discipline as `server_net.cc`. Entry: env `F2_CLIENT_CONNECT=host:port`
(chosen in `main()` before the menu, like `F2_SERVER_MAP`).

### B. Join snapshot — REUSE THE SAVE PIPELINE (§7b)
The current wire's `SNAPSHOT_OBJECT` baseline (netId/pid/tile/elev/fid/flags) is
enough for `replay.py`'s position check, **not** to reconstruct a playable world.
Step 4 adds a real snapshot.

**Server side, at join, in this exact order:**
1. ◆ **B2 — stamp the header as an in-game save WITHOUT the script side effects.**
   Set `gMapHeader.flags |= 0x01` (the "saved map" bit) and
   `gMapHeader.lastVisitTime = gameTimeGetTime()`. These are the two benign lines
   of `_map_save_in_game` (map.cc:1287); do NOT call it (it runs
   `scriptsExecMapExitProc`, which mutates). Without the saved bit, the client's
   `mapLoad` takes the first-run branches (re-reads the `.GAM` overwriting the
   blob's gvars, map.cc:747-772, and passes `map_first_run=1`, map.cc:789 →
   re-runs one-time spawn logic → divergent objects → shifted netIds).
2. ◆ **C.2 — rebaseline: run `objectAssignAllNetIds()`** (fixed per §C) so the
   emitted netIds are the ones the client will independently reproduce.
3. Write the live map via `_map_save_file(stream)` (map.cc:1180 — header + gvars +
   lvars + squares + `scriptSaveAll` + `objectSaveAll`) to a temp file.
   `_map_save_file` is `static` → de-static (declare in map.h) or wrap in a new
   exported `mapSaveToStream`.
4. ◆ **B1 — append the dude.** The dude is `OBJECT_NO_SAVE` (object.cc:3334/3364)
   so `objectSaveAll` skips him. Append `_obj_save_dude(stream)` (object.cc:3324,
   already handles the NO_SAVE flag dance) after the map body. Without this the
   client uses its own locally-created character, whose position/fid/hp/inventory
   all differ from the server's dude.
5. Read the temp bytes and frame them on the wire as new events (§2), through the
   NetworkPresenter framing (◆ S3 — never raw-write to the `ByteSink`; a frame may
   be open).

**Client side, at join, in this exact order:**
1. ◆ **S4 — `gameTimeSetTime(blob.gameTime)`** BEFORE loading (the map-enter/
   update procs read `gGameTime` during load; time-gated logic must see the
   server's clock, not the client's).
2. Buffer the blob to a temp file and load it via the ◆ **B4 — inner
   `mapLoad(File* stream)`, de-static'd — NOT `mapLoadSaved`/`mapLoadByName`.**
   The outer wrappers do things a viewer must not: `_map_age_dead_critters`
   (heals critters, spawns blood via `randomBetween` → netId shift, map.cc:
   1006-1016), `objectUnjamAll`, deletes the `.SAV` from disk (map.cc:910-921),
   and require planting the blob at `master_patches/MAPS/<name>.SAV` (clobbering
   the user's real single-player save). `mapLoad` itself does the good part:
   scripts-before-objects, gvars/lvars, the map-enter proc for NO_SAVE regen.
3. ◆ **B1 — apply the dude blob** (`_obj_load_dude`-style) and repoint `gDude`.
4. ◆ **C — run `objectAssignAllNetIds()`** (same fixed walk) and seed the
   `netId → Object*` map.
5. ◆ **S1 — `scriptsDisable()`** (mapLoad re-enabled scripts at map.cc:775; the
   viewer must disable them AFTER load returns). See §E.

### C. netId alignment — THE load-bearing decision (rebaseline-at-join)
**Problem:** `objectDelta`/`move`/`destroy` events address objects by
`Object::netId`, a CE-added runtime field NOT persisted by `objectSave` — so
after load the client's objects carry netId 0. It must arrive at the *same*
netId→object map the server holds, or every delta corrupts the wrong object (the
§7d "the stream did not go incomplete — it LIED" failure, client-side).

**Decision — the JOIN IS A REBASELINE.** Server runs `objectAssignAllNetIds()`
(object.cc:4489) right before snapshotting; client runs the **same** walk after
loading. Both assign in the same deterministic order over the same object set ⇒
identical netIds by construction, none on the wire. This reuses the mechanism
`serverEmitBaseline` already uses on a map transition ("snapshot is ONE
mechanism", §7d).

**◆ The invariant only holds with the B1+B3 fix — align the three domains.**
The walk (`objectAssignAllNetIds`), the blob (`objectSaveAll`), and the delta
layer (`objectIsSyncable`) must cover the SAME object set, or a slot mismatch
shifts every netId after it:
- The delta layer already excludes `OBJECT_NO_SAVE` (`objectIsSyncable`,
  object_delta.cc:126-129) — NO_SAVE objects can never be addressed by a delta.
- But `objectSaveAll` skips NO_SAVE (object.cc:674) **while** the current
  `objectAssignAllNetIds` numbers *everything* including NO_SAVE, AND numbers the
  dude + his inventory first (object.cc:4494). So NO_SAVE objects (crucially
  **recruited party members**, object_delta.cc:113-115) and the client-vs-server
  dude-inventory size difference both shift the whole map.
- ◆ **FIX (one predicate, kills the whole class):** make `objectAssignAllNetIds`
  skip `OBJECT_NO_SAVE` **except the dude** — the same predicate as
  `objectIsSyncable`. Then walk domain == blob domain == delta domain. NO_SAVE
  *regen determinism* (was risk #2) drops out of the invariant entirely. NO_SAVE
  objects that spawn later still get a wire netId at creation
  (object.cc:761-763) and ride a SPAWN event — unaffected.
- ◆ **The dude rides the blob** (B1, §B.4/§B-client.3) → both sides number him
  identically. `dudeNetId` in `SNAPSHOT_BLOB_BEGIN` is a fail-loud cross-check
  (assert dude's post-walk netId == dudeNetId), not the repair.

**Rejected alternatives:** persist-netId-in-save (forks the shared savegame
format); match-by-(pid,tile) (not unique — stacked items, same-pid critters);
client-fresh-walk-vs-live-server-netids (fails mid-game: server = load-order +
spawn-order, client = pure tile-order). A **sidecar netId table** (stream netIds
in `objectSaveAll` order, apply in `objectLoadAll` order) is the one real
alternative — it removes the shared-walk dependence and eases mid-stream join —
but with the domain-alignment fix the rebaseline is robust and keeps the simpler
no-netId-on-the-wire property. Take the fix; keep rebaseline. (Revisit the
sidecar if STEP 5 mid-stream join proves the walk fragile.)

**◆ C.4 — rebaseline broadcasts to ALL clients (locks STEP 5).** Any
`objectAssignAllNetIds` run resets the counter for the whole stream
(object.cc:4491). So a late joiner (STEP 5) that triggers a rebaseline silently
remaps netIds under every existing client. RULE: *every* rebaseline (join OR map
transition) re-emits blob + baseline to **all** connected clients.
**IMPLEMENTED (STEP 5):** `SocketByteSink::acceptPending()` (per-beat,
non-blocking) preambles each joiner and flags `serverRequestRebaseline()`; the
loop consumes it at the beat tail — `objectDeltaReset()` (fresh tile-order walk,
exactly what the joiner's blob-load walk reproduces; the live assignments from
the LAST reset are stale-order and would misalign) + `serverEmitBaseline()` to
everyone. Coalesces N joins/beat; skipped when a map-change baseline already
went out that beat. The joiner's SNAPSHOT_OBJECT tripwire is the alignment
oracle (gate: scripts/check_midjoin.sh). COST (banked, acceptable v1): every
connected viewer reloads its world on each join; per-client blob targeting
needs the rejected netId sidecar. Also banked: blocking-write backpressure (one
stalled client delays the beat loop ≤5s), `_obj_process_seen`'s OBJECT_SEEN
flag writes leaking spurious flag deltas after each snapshot.

### D. Live event apply (client decoder — inverse of `NetworkPresenter`)
`client_net.cc` decodes the frame/event format (presenter_network.cc:16-45) and
mutates the local sim. It holds the `netId → Object*` map, seeded from the
snapshot walk and maintained by SPAWN/DESTROY. Event → mutation:

| event | mutation |
|---|---|
| `SPAWN(netId,pid,tile,elev,fid)` | create object at tile; set netId; map it |
| `MOVE(netId, from→tile, from→elev, durMs)` | `objectSetLocation` (state, always instant) + when durMs>0 the SDL viewer glides the sprite over the hop (src/client_anim.cc: pixel-offset decay slaved to durMs, walk/run art at art fps for frame selection only; off by default — headless decode is pure snap) |
| `DESTROY(netId,pid)` | `objectDestroy`; unmap |
| `CONNECT/DISCONNECT` | inventory attach/detach |
| `OBJECT_DELTA(netId,mask,…)` | set fid/rot/flags/hp/rad/poison/ap/results; INVENTORY → rebuild top-level |
| `WORLD_DELTA` | `gameTimeSetTime` |
| `MAP_TRANSITION` | drop world; expect a fresh `SNAPSHOT_BLOB` next |
| `COMBAT_*`, `ATTACK_RESULT` | presentation cues (render the attack) — skippable |
| `CONSOLE/FLOAT/SFX/FADE/ERROR/MUSIC_STOP` | drive the local presentation layer |
| `SNAPSHOT_OBJECT/BEGIN/END` | ◆ (nit) NOT ignored — assert each (netId,pid,tile) against the post-walk map as a free join-time misalignment tripwire |

`OBJECT_DELTA_FLAGS` carries the whole flags word (authoritative as sent —
presenter_network.cc:262). Strings are RAW codepage bytes, transcode at
presentation only. Buffer a whole frame, apply atomically (payloadLen guarantees
no partial frame).

### E. Viewer main loop + SIM SUPPRESSION (the correctness crux)
A normal client runs the whole sim; a viewer must be a **puppet** or it diverges
the instant anything nondeterministic fires. ◆ **S1 — the doc's original "mirror
the `serverLoopActive()` gates" lever was WRONG:** those gates suppress UI/pacing
on the server, while the sim drivers are things the server *runs* and never gates.
The real client sim engine is the ticker chain `inputGetInput()` → `_process_bk()`
→ `tickersExecute()` (input.cc:145-155), where `_doBkProcesses` (scripts.cc:676)
runs `_script_chk_critters` (AI) and `_script_chk_timed_events` (scripts.cc:750:
`gGameTime += 1` per 100ms + `queueProcessEvents` — poison/rad/explosions).

**The structural lever, primary mechanism (not N gates):**
- `scriptsDisable()` after the snapshot load — `_doBkProcesses` self-gates on
  `gScriptsEnabled` (scripts.cc:696), so ONE lever kills AI, time advance, and the
  event queue wholesale.
- `tickersRemove(_object_animate)` and `tickersRemove(_dude_fidget)`
  (presenter_client.cc:73-74, combat.cc:2732) — cosmetic, and they fight the
  authoritative `OBJECT_DELTA_FID`.
- Never call `scriptsHandleRequests` / `mapHandleTransition` / `_combat` in the
  viewer loop.
- `clientViewerActive()` predicate = a backstop ASSERT (catch any sim-advance that
  slipped through), not the primary means.

**STEP 6 (controllable client) — input ENABLEMENT, and its re-puppet requirement.**
The controllable viewer *enables* the game mouse instead of `_gmouse_disable(0)`:
`_gmouse_enable()` + pinned `gameMouseSetMode(GAME_MOUSE_MODE_MOVE)` + `gameMouseObjectsShow()`.
This is **presentation-safe**: the `gameMouseRefresh` ticker then draws the hex +
bouncing cursor, which mutate ONLY the two `OBJECT_NO_SAVE` cursor objects the netId
walk provably skips — no addressable sim state. The mode is pinned MOVE (ARROW/CROSSHAIR
hover would touch `_obj_look_at`; MOVE never does), and the viewer NEVER calls
`_gmouse_handle_event`/`_dude_move`/`_dude_run` — a left-click is captured in the frame
loop, converted via `tileFromScreenXY`, and sent UPSTREAM as an `mv` intent (§8 of
MP_PROTOCOL). `claim` is sent once after the first blob. ⚠ This enablement, like the
tickers, is **re-applied on every rebaseline** (`loadCount()` change): the blob's
mapLoad re-runs `_gmouse_enable()` and re-adds the animation tickers, so the re-puppet
branch must re-pin MOVE mode (but NOT re-`claim` — the server holds the claim for the
connection's lifetime).

```
connect → gameTimeSetTime(blob) → mapLoad(temp stream) → apply dude blob →
          objectAssignAllNetIds → seed map → scriptsDisable() + remove tickers
loop:
  pump SDL input   (quit / camera scroll ONLY — no gameplay input)
  client_net.poll(): decode frames → apply events (§D)
  render the frame (normal ClientPresenter render path)
```

## 2. Wire additions (append-only, no version bump)
```
EVENT_SNAPSHOT_BLOB_BEGIN = 24 :
    i32 mapIndex | i32 elevation | i32 dudeNetId | u32 gameTime |
    u16 mapSaveVersion | u32 mapBlobLen | u32 dudeBlobLen | u32 crc32   ◆ guard fields
EVENT_SNAPSHOT_BLOB_CHUNK = 25 : bytes[len]   (len from event header; N chunks; map body then dude)
EVENT_SNAPSHOT_BLOB_END   = 26 : (empty)
```
All `EVENT_FLAG_STATE`, emitted as their own frame(s) via the presenter framing
(◆ S3: `flushFrame` before the first chunk), BEFORE the first beat frame, and
re-emitted after `MAP_TRANSITION` to ALL clients (§C.4). Chunk payload ≤ 0xFFFB
(the u16 event-len ceiling); 32 KiB is a fine working size. `crc32` + the length
fields make the temp-file round trip fail loud, extending the `dudeNetId`
cross-check posture to the bytes.

## 3. Build slices (each validated before the next; the DELIVERABLE is S3)
- **S1 — server snapshot + round-trip proof (internal validation, not a shipped
  milestone).** Server produces the blob (with the B1/B2/C fixes); a headless
  harness loads it back in a second core instance and asserts reconstructed state
  == source `state_dump` AND that `objectAssignAllNetIds` yields the identical
  netId map both sides. PINS §C. (Not "done" — the safety net under the real thing.)
- **S2 — headless joining client.** A core/headless consumer connects, receives
  the blob, loads it, applies the live stream, dumps state == server. Validates §D
  without SDL. Cross-check against `replay.py` semantics.
- **S3 — the real SDL client (DELIVERABLE).** `F2_CLIENT_CONNECT` → viewer loop →
  render. Drive live: `f2_server` on a map with a moving critter; connect the
  client; confirm it renders the same world and the critter moves in lockstep
  (the `verify`-skill end-to-end observation).

## 4. Risks (post-review)
1. **Sim-suppression completeness.** Reduced but not eliminated by `scriptsDisable`
   (§E): audit for any engine input pump that still drives tickers, and any
   client-only action path (input→`action*`) reachable in viewer mode.
2. ~~NO_SAVE regen determinism~~ — ◆ REMOVED from the invariant by the §C
   domain-alignment fix (NO_SAVE no longer occupies netId slots).
3. **Viewer presenter re-entry.** Apply-path setters (`objectSetLocation` etc.)
   must not re-enter the presenter into a loop; they are pure state, render reads
   separately. Confirm.
4. **Mid-stream join / consistent cut.** v1 = connect-at-start (quiescent
   snapshot). True late join needs a between-beats cut + §C.4 broadcast — STEP 5.
5. **Game GVARs are not on the wire** (only map gvars/lvars ride the blob). Fine
   for a fresh-boot v1; a hard prerequisite for any mid-game join, since scripts
   branch on `global_var()`. Bank it.
6. ◆ **Per-join live-state perturbation (nit).** `_map_save_file` →
   `_obj_process_seen` + header/darkness rewrites mutate sim-visible server state
   each join; `objectLoadAllInternal` applies the *client's* violence_level to
   fids (cosmetic, first `OBJECT_DELTA_FID` corrects). Document, don't fix.

## 5. Model / process
Design-class + cross-cutting + hard-to-reverse (silent-corruption fork §C) →
Fable design pass DONE (2026-07-15, folded above). Opus builds slice-by-slice,
adversarial Opus review per commit (standing gate). Existing gates stay green
(viewer/client code is new, off the server + legacy paths — the one shared edit,
the `objectAssignAllNetIds` NO_SAVE-skip, must be checked byte-identical on the
netstream `xmap` gate, which exercises the walk). S3's proof is the live
end-to-end observation, not a golden.
