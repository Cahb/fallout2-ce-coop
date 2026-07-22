# In-combat MOVE via the record channel — design (as built)

Plan of record for the **Locomotion IN COMBAT** family in `src/pres_record.h`'s COVERAGE
MANIFEST. Faithful to the attack/door/projectile model: **server records the reg_anim leaf,
client replays it as-is.** Companion to `PRESENTATION_RECORD_REPLAY_SPEC.md` / `_COOKBOOK.md`.

## What it closes

In-combat movement did NOT ride the record channel — it used the `EVENT_MOVE` glide lane with
a **locally-guessed** walk-vs-run (`client_present.cc` `durMs <= kRunThresholdMs ?
ANIM_RUNNING : ANIM_WALK`, BUG 4), a separate animation system from `reg_anim`. Now the server
records the actual walk leaf; the client replays it through its own real
`animationRegisterRunToTile/RunToObject` — vanilla-exact frames, rotation, run/walk art
fallback — folded into the one turn-serial stream with attack/wield.

## The model — record the real leaf, hold the state

A projectile moves straight, so its leaf is `MOVE_STRAIGHT`; a critter walk is pathfound, so
its leaf is `RunToTile(dest, ap)`. We record whichever leaf the engine actually emits. For a
walk that is `RunToTile` — the client's engine pathfinds internally on replay, exactly as
vanilla does when it plays that leaf. Not a loose "intent": it's the exact leaf + args, and
determinism (same tiles/endpoint) comes from **same replicated map + deferring the mover's AP**
so the client re-walks the identical path.

The one hard rule the first (rejected) attempt broke: **authoritative state never rides only
the droppable presentation channel.** `EVENT_MOVE` + the AP `OBJECT_DELTA` keep shipping on the
state lane as always. On the CLIENT, the mover's position + AP deltas join the **held-delta
family** (the same mechanism that holds fid/flags/rotation for attack participants) and are
**reconciled when the replayed walk completes**. Reconcile-on-completion, never
suppress-at-source → state can never be lost (any replay exit — completion, cap, stall, forget
— snaps to authority).

### Why the AP defer is load-bearing (this was the "half-hex then cancel" bug)
The viewer mirrors combat state, so `isInCombat()` is true there too, and the real engine
charges AP per step during a walk (`_object_move` → `movementChargeApForStep`,
`animation.cc:2117`). The server had already drained the mover's AP; if that AP delta applies
immediately on the client, the first replayed step's charge fails and the walk dies after one
tile. Holding the AP delta until the walk finishes means the client re-walks from the mover's
**pre-walk** pool, reproducing the server's stop point step-for-step.

## Server (`server_anim.cc`, `combat.cc`, `combat_ai.cc`, `combat_drain.cc`)

- **Section at the composite `reg_anim` bracket** (like the attack slice), gated by
  `combatMoveRecorded()` (`combat.cc`): `serverLoopActive() && presRecordEnabled() &&
  isInCombat() && !presRecordActive()`. It's an **AMBIENT** section (`presRecordAmbientBegin/
  End`, no RNG snapshot/restore) — unlike attack's skipped animate branch, the move bracket
  runs on the flag-off server too and holds authoritative rolls (`_combatai_msg` taunt); an
  RNG-restoring section would desync flag-off vs flag-on (record-purity would catch it).
- **The leaf records its real args + defers state.** `serverAnimMoveToTile/Object`, inside a
  section, `presRecordMoveToTile/Object(dest, elev, ANIM_WALK|ANIM_RUNNING, actionPoints)` and
  STASHES the authoritative walk (`gDeferredWalk`) — it does NOT apply yet. The body is factored
  into `serverAnimMoveToTile/ObjectApply` (byte-identical to the pre-record path).
- **Composite ships, then commits.** `combatMoveRecordClose()` (`combat.cc`): end the ambient
  section, `presenter()->presSeq(..., actor->netId)`, then `presRecordCommitDeferred()` →
  `serverAnimCommitDeferredWalk()` applies the stashed walk. So the presSeq precedes its own
  `EVENT_MOVE`/AP deltas on the wire (spec §6.3) — the client arms its hold before they land.
  `EVENT_MOVE` emission is unchanged (a flag-off / old viewer that dropped the seq falls back to
  today's glide — graceful degradation).
- Wrapped brackets: `combat_ai.cc` `_ai_run_away`, `_ai_move_away`, `_ai_move_steps_closer`,
  the friendly-fire retarget in `_ai_try_attack`; `combat_drain.cc` player `COMBAT_INTENT_MOVE`
  (actor `gDude`). The f2_core→server_anim layering is bridged by a static-init commit hook
  (`presRecordSetDeferredCommitHook`).

## Wire (`pres_record.{h,cc}`)

- `PRES_OP_MOVE_TO_TILE {ref, tile, elev, anim, actionPoints, delay}` and
  `PRES_OP_MOVE_TO_OBJ {ref, targetRef, anim, actionPoints, delay}` (both refs via
  `resolveRef`). `kPresStreamVersion` 3.

## Client (`client_net.cc`, `client_present.{h,cc}`)

- Interpreter: `MOVE_TO_TILE`/`MOVE_TO_OBJ` replay the real `animationRegisterRun/MoveTo*` with
  the RECORDED `actionPoints`, then `clientCombatAnimMarkActive(o, kMoveReplayCapMs)` (holds the
  pump so the walk serializes against the rest of the turn + resolves its held deltas on
  completion; the 8 s cap replaces the generic 2 s that would kill a legit 3–6 s walk). DRY
  pass: `clientCombatAnimArmMoveHold(mover)` — reserve + flag `moveHold`.
- `onMove`: `clientCombatAnimDeferMove(obj, tile, elev)` holds the mover's authoritative
  position (no snap/glide/`kMoveRelease`/`notifyReposition`). Every non-armed object keeps
  today's path bit-for-bit (knockback untouched).
- `onObjectDelta`: `clientCombatAnimDeferAp(obj, ap)` holds the mover's AP (the fix above);
  `_dudeApAuth` still tracks authority; the dude HUD ticks via the client's own per-step charge.
- `resolveHeld`: applies held position first (drift-traced snap; normally a no-op — the walk
  landed there), then AP, then the existing fid/flags/rot; clears `moveHold`.

## Gates

- `scripts/check.sh` — byte-identical (all behind `combatMoveRecorded()`, false off record / SP
  / golden). `run_record_purity.sh` — add an in-combat move case (artemple aggro); the ambient
  section is proven RNG-neutral by the flag-off vs flag-on differential. Live-verify (no viewer
  oracle): AI approach plays a real vanilla walk/run, turn-serial, endpoint == authoritative
  tile (drift trace silent), dude cmove with per-step AP dots, multihex face-target, door
  crossing. Adversarial review mandatory.

## Deferred / banked
Generalize participant-Active promotion to all recorded seqs (latent attack-pacing pump gap);
combat gesture + pre-attack rotate brackets onto the same ambient mold; then wield/use/pickup
(BUG 5/6) — discrete actions that reuse the attack pattern with no reconciliation.
