# applyBlob clean-teardown/rebuild — plan of record

Status: **STEPS 1-2 IMPLEMENTED (staged, uncommitted 2026-07-22); steps 3-6 remain.** This is the
root fix for the client rebaseline crash/leak/corruption cluster. Anchors are `file:line` on
`rewrite/phase0` (current tree drifts slightly). Steps 1-2 close ASAN signature A (the
`onSnapshotObject` UAF in `dumps.txt`) — see [[asan-crash-cluster-2026-07-22]]. Built +
join/netstream/netsocket/record_purity/check_midjoin all GREEN.

## Why this exists — the root cause (answers "why is inventory length < 0?")
On every rebaseline (map change / player join-leave) the client rebuilds the world in
`applyBlob` (`src/client_net.cc:900-1126`). Two structural defects:

1. **`_net` (netId→Object* map) is stale for the whole teardown window.** `_net` is only
   cleared at step 12 (`seedNetMap`, `client_net.cc:1230`), but the previous world is FREED
   at step 6 (`mapLoad` → `_obj_remove_all`, `object.cc:2102` / `map.cc:569,694`). So during
   steps 6–11 `_net` holds pointers into freed objects. **`applyBlob` is NOT re-entrancy-safe**:
   any wire event or repaint that lands mid-teardown (exactly what **alt-tab-during-connect**
   injects) calls `lookup()` (`client_net.cc:641`) → resolves a stale entry to freed memory →
   mutates it / its inventory → dangling `Inventory.items[k].item`. That dangling item is what
   the *next* `_obj_remove_all → _obj_remove → _obj_inven_free` (`object.cc:1989`, deref
   `1998-1999`) faults on (ASAN heap-use-after-free), and what makes a downstream `length`/
   `capacity` read garbage (ASAN allocation-size-too-big in `objectLoadAllInternal`,
   `object.cc:548`). The three existing one-off patches (`onConnect` double-free 1453-1463,
   `onDestroy` 1411-1420, `forgetObjectRefs` 648-673) each patch one *producer* of a dangling
   item; nobody fixed the shared enabler (live `_net` over a freed world).

2. **Per-rebaseline host-inventory leak.** `_obj_load_dude` `memcpy(gDude, temp, sizeof)`
   (`object.cc:3582`) overwrites `gDude->data.inventory` without freeing the old one. gDude's
   carried items live only inside its Inventory (never in tile buckets, so `_obj_remove_all`
   skips them) → the host's entire inventory leaks every rebaseline (the LeakSanitizer noise).
   Vanilla only ran `_obj_load_dude` on explicit game-load; co-op runs it every rebaseline.

3. **Inventory-flash (#3):** `_obj_load_dude` calls `_inven_reset_dude()` (`object.cc:3633`)
   anchoring `_inven_dude` to gDude while gDude is still the HOST (`client_net.cc:932`); gDude
   isn't repointed to MY actor until `rebindLocalActor` (step 14, `client_net.cc:846/871`). Any
   paint between steps 7 and 14 shows the host's HP/AC/inventory for an extra player.

## Current applyBlob order (what frees vs re-creates)
`client_net.cc:900-1126`: (2) `gDude=gClientHostDude` 932 · (3) `presReset()` 934 · (4)
`playerActorClear()` 943 (nulls registry only) · (5) `gameTimeSetTime` 945 · (6) `mapLoad` 952
→ `_obj_remove_all` frees whole prev world · (7) `_obj_load_dude` 959 (memcpy, no prior-inv
free; `_inven_reset_dude` → host) · (8) sheet seeds 983-988 · (9) register + `_obj_load_player_actor`
loop 990-1003 (strips NO_REMOVE, `object.cc:3530`) · (10) `playerSheetBlockRead` 1010 · (11)
netId walk + `scriptsDisable` 1017-1019 · (12) `seedNetMap` 1021 (`_net.clear()` 1230, rebuild)
· (13) leak tripwire 1034-1050 · (14) `rebindLocalActor` 1073 (repoint gDude to my slot, first
paint) · (15) combat reassert/reset 1088-1124.

## The fix — 6 incremental steps (land in order)
Principle: establish a reference-free, paint/pump-suppressed state BEFORE `mapLoad`; split into
`teardownPreviousWorld()` (before mapLoad) + `rebuildAndBind()`, separated by a re-entrancy guard.

**Steps 1-3 are GOLDEN-GUARDED — safe to land now, no live verify:**
- **Step 1 — ✅ DONE (staged) — free host dude inventory before the memcpy** (leak #2).
  `_obj_inven_free(&gDude->data.inventory)` added inside `_obj_load_dude` right before the memcpy
  (object.cc). No-op on empty first load / SP load. Guards green: `run_golden_join`, `netstream`,
  `record_purity`.
- **Step 2 — ✅ DONE (staged) — moved `_net.clear()` + `_adoptTransients.clear()` out of `seedNetMap`
  to the top of teardown (right after `presReset`, before `mapLoad`).** THE core root fix — no stale
  netId can resolve during teardown. `seedNetMap` is now populate-only (single caller = applyBlob,
  verified). Guards green: `run_golden_join`, **`check_midjoin`** (real guard, PASS), `netsocket`.
- **Step 3 — TODO — pull equipped-slot statics (`gInventoryLeftHandItem/RightHandItem/Armor`, from
  rebindLocalActor 868-870), `gViewerLootTargetNetId`, and `clientViewerFlushDeferredItemFrees()`
  (`client_net.cc:3795`, the `gDudeDeferredItemFrees` list) into the teardown block before
  `mapLoad`.** Guards: `check_midjoin`, `record_purity`.

**Step 4 — inventory-flash (#3), LIVE-visual verify:** rebind + `_inven_reset_dude` + HUD repaint
run as the LAST pre-release step under a paint-suppression guard, so no frame shows wrong actor
data. Slot→actor (`playerActorAt`) is available right after step 9, doesn't need `seedNetMap`.

**Step 5 — re-entrancy safety (THE alt-tab crash fix), LIVE ASAN verify:** add `_rebaselining`
bool (true for the whole applyBlob); while set, the decoder dispatch (`client_net.cc:599-634`)
early-returns/buffers (no `lookup()`/list mutation) and HUD paint + input drain are suppressed.
Repro: `build-asan/`, connect, alt-tab repeatedly during connect → the `_obj_inven_free` UAF must
vanish. **No headless golden generates focus events mid-applyBlob.**

**Step 6 — reconnect-in-combat teardown (hang #5), LIVE verify:** the `reassert` branch
(`client_net.cc:1088-1124`) preserves `_inCombat/_myTurn`/end-buttons across the rebaseline;
the hang is the preserved turn-gate waiting on a `TURN_START` the re-emit doesn't reliably
re-deliver to an already-combatant client. Fix: tear combat framing down to resting
(`clearCombatMirror` 1962) and let the server re-emit rebuild it, OR make the re-emit
unconditional for a reconnecting session. Design decision; root banked server-side (STEP-5
netId-sidecar, comment `client_net.cc:1085-1087`). Guard: `netstream`; live: reconnect mid-combat.

## Keep, don't regress
- `_obj_load_player_actor` strips NO_REMOVE (`object.cc:3530`) + `objectApplyWireFlags`
  (`object.cc:3500`) preserves local lifecycle bits — the actor-leak tripwire (`client_net.cc:1034`)
  guards this. The clean teardown must not reintroduce a NO_REMOVE survivor.
- Combat/dialog/barrier spawn safety unchanged (spawn drains only at the safe main-phase site,
  refused in combat).

## Verify command set
`tests/golden/run_golden_{join,netstream,netsocket}.sh`, `run_record_purity.sh`,
`scripts/check_midjoin.sh`. Live-only: steps 4 (2-player join, watch extra's HUD), 5 (alt-tab ASAN),
6 (reconnect mid-combat). See [[coop-mp-track]] for the symptom history.
