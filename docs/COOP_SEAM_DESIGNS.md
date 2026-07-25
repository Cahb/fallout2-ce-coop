# Co-op seam designs — sheet-delta, gender/appearance, attack-weapon

Status: **designs of record (Fable design pass, 2026-07-23)**, none implemented. These
are the three design-class items split out from the 2026-07-23 recon sweep (the genuine
one-liners #3 stat.cc:280 and #6 client_net.cc:1863 were already applied). No headless
N>1 oracle exists for any of these — every verify step is live-only.

Related: [[per-player-character-sheet]] (PLAYER_SHEET_DESIGN.md), [[vault-suit-appearance-gap]],
[[coop-liveplay-bugs-2026-07-21]] (bug D). Convention notes: PSHT/PAC2 sheet format,
ServerActorScope, `playerActorIs` = the subject-in-hand bucket of the 4-bucket taxonomy.

---

## ITEM 1 — Per-actor sheet-delta channel (`EVENT_PLAYER_SHEET = 48`)

**Problem.** Drug/chem effects apply correctly server-side to the acting actor's proto row
(`critterSetBonusStat`, item.cc:2722), but sub-`SAVEABLE_STAT_COUNT` stats (e.g.
STAT_PERCEPTION) live in the per-actor sheet, which only streams in the join blob
(map.cc:1444 → client_net.cc:1096). So a viewer's sheet is stale until the next transition:
healing powder's PE −1 is invisible, and the "You lost N Perception" line goes to the
server's null presenter.

**Recommendation.** Dirty-flag per slot at the storage choke points, drain once per beat,
ship the **whole sheet row** in the `PSHT` block format the client already reads.

- **Trigger:** `playerSheetMarkDirty(Object*)` (player_sheet.{h,cc}) — resolve slot via
  `playerActorSlotOf`, set a per-slot bit; no-op on slot < 0. Call from `critterSetBaseStat`
  (after proto write, stat.cc:534), `critterSetBonusStat` (stat.cc:594 — covers the drug
  path), `pcSetStat`, and the perk-rank setter. Dirty flag over per-call emit: one drug
  touches up to 3 stats + derived recompute in one chain; the beat coalesces.
- **Drain + wire:** `playerSheetDeltaEmit()` in `serverTick` next to `objectDeltaScan`
  (server_loop.cc:479, before `beatEnd` at :532). Gate on a new
  `virtual bool wantsSheetDeltas()` (NetworkPresenter → true) so golden/probe paths do
  zero work. `playerSheetBlockWriteOne(File*, slot)` writes `[PSHT][slot][1][row]`,
  byte-compatible with `playerSheetBlockRead` (player_sheet.cc:133). Use the temp-file
  idiom from `serverEmitJoinBlob` (server_loop.cc:220-245) — `XFILE_TYPE_MEMORY` is
  read-only (xfile.h:48). New presenter virtual `playerSheetDelta(int slot, const unsigned
  char*, int)`; `EVENT_PLAYER_SHEET = 48` (47 = EVENT_MOVIE_STOP, presenter_network.cc:202).
- **Consumer:** client_net.cc event switch (~:657). Drop if blob not yet loaded (its sheet
  block is fresher). Else `fileOpenMemory` → `playerSheetBlockRead` (storage-only apply to
  `gPlayerActorProtos[slot-1]` / `gDudeProto`). Do NOT recompute derived stats client-side
  (server ran `critterUpdateDerivedStats` before the snapshot, stat.cc:596-598 → row is
  self-consistent). If slot == mine: refresh HUD + `windowRefresh` if a sheet/inventory
  screen is open (async-wire-repaint trap).
- **Console feedback:** `_perform_drug_effect` gates before-capture (item.cc:2700-2702) and
  the message (item.cc:2724) on `critter == gDude`. Change both to `playerActorIs(critter)`
  (correct subject bucket; degenerates to `== gDude` with empty registry) and emit via
  `presenter()->consoleMessageFor(critter->netId, msgBuf)` (presenter.h:89; override
  presenter_network.cc:934).
  **Bonus bug:** the 360-min restore fires from `drugEffectEventProcess` (item.cc:2895)
  **outside** any ServerActorScope (server gDude == host), so extras get neither the −1 nor
  the +1 line today. The `playerActorIs` change fixes this too.

**Hazards.** (a) Same-beat rebaseline: blob carries the row (map.cc:1444); whole-row deltas
are idempotent → replay-after-blob harmless (the reason for whole-row over per-stat).
(b) Current HP is not sheet (Object, rides OBJECT_DELTA_HP) → no double-apply.
(c) Golden-inert (extras absent from goldens, new virtuals base no-ops) but run check.sh.

**Delayed-restore lifetime (why whole-row + park-not-destroy make the rejoin case safe).**
The immediate penalty (e.g. PE −1) is stored in the sheet (bonusStats) and persists; the
delayed RESTORE (+1) is a queued event OWNED BY THE ACTOR OBJECT (`queueAddEvent(delay,
critter, drugEffectEvent, EVENT_TYPE_DRUG)`, item.cc:2661), re-bound on load by object `id`
(queueLoad → `_inven_find_id`, queue.cc:~120). The rejoin failure mode "−1 not streamed,
later +1 streamed → net +1 above base" CANNOT occur: (1) deltas are whole-row ABSOLUTE
snapshots, so any single message (blob or delta) is the truth — a missed −1 can't drift, a
stray +1 can't exceed base; (2) disconnect PARKS the actor (`_obj_disconnect`,
server_control.cc:1346) instead of freeing it, so `queueRemoveEvents` never runs and the
restore survives disconnect→rejoin and fires on schedule.
**INVARIANT to enforce/verify:** wherever the sheet persists, the drug/withdrawal QUEUE must
persist and re-bind in lock-step. A lost restore = PERMANENT penalty (no "base" to recompute
from — the penalty is baked into bonusStats).

### ►► SUB-BUG (found 2026-07-23, PRE-EXISTING, independent of the wire delta): extras' queued events are orphaned across save/load/restart.

VERIFIED mechanism:
- `queueSave` writes the owner by `object->id` with **no NO_SAVE guard** (queue.cc:211), so an
  extra's event *is* written.
- `queueLoad` re-binds by scanning the LIVE world for that id (queue.cc:~120,
  `_inven_find_id`), and runs **inside the savegame handler loop** (savegame.cc:141).
- But extras are `OBJECT_NO_SAVE` → skipped by `objectSaveAll` → reconstructed only by
  `playerActorAppendixLoad`, a tail step that runs **after** the handler loop
  (savegame.cc:437, comment 432-436). Host + map load *inside* the loop, so the HOST re-binds
  fine — extras do not exist at `queueLoad` time → `owner = nullptr`.
- Result for an extra: the +1 restore fires with a null owner → silently dropped (permanent
  −1, the −1 persists in the appendix sheet row) or null-deref crash in
  `_perform_drug_effect`.

GENERAL: not drug-specific — ANY queued event owned by an extra (EVENT_TYPE_DRUG, radiation,
poison, withdrawal, script timers) is orphaned the same way. This is a latent co-op
persistence bug on its own; the chem feature just surfaces it.

FIX — the appendix becomes the sole persistence owner of extras' queued events:
1. `playerActorAppendixSave`: after each extra's sheet row, snapshot its queued events
   (remaining delay = `node->time - gameTimeGetTime()`, type, data via the type's writeProc).
2. Filter extras' events OUT of the global `queueSave` (owner is a registered slot ≥ 1) so
   they are not written twice → double-apply / orphan.
3. `playerActorAppendixLoad`: after the extra is registered (player_sheet.cc:291),
   `queueAddEvent(remainingDelay, actor, data, type)` re-bound to the fresh object.
4. Defensive: null-owner guard in `drugEffectEventProcess` / `_perform_drug_effect`.

This is a SEPARATE work item from the EVENT_PLAYER_SHEET wire delta: the delta gives LIVE
visibility, this gives save/restart INTEGRITY. Both are needed for chems to be fully correct.
Test: extra drinks healing powder → autosave → restart server → +1 restore still fires
(PE returns to base), no crash; also verify a radiation/poison timer on an extra survives.

**Live test.** Viewer eats healing powder → viewer console "You lost 1 Perception" (host
console silent), viewer char screen PE drops immediately; rest 6+ game hrs → PE restores +
line. Reverse: host chem use prints nothing on the viewer.

---

## ITEM 2 — Gender/appearance through creation + de-globalized `_art_vault_guy_num`

Vanilla has one creation-time appearance axis (gender); suit-vs-tribal is the world's
`MOVIE_VSUIT` flag, armor look is inventory-derived. The vanilla create screen already has
the toggle; the co-op gap is the wire struct not carrying it.

**(a) Gender flow.**
- `PlayerCreateSpec` (player_create.h:21): add `int gender;`, default GENDER_MALE in
  `playerCreateSpecDefaults`. Owner ruling intact — no-spec path never reaches the applier;
  spec presence *is* the explicit intent. Validate ∈ {male, female}.
- Viewer composer (main.cc:1054-1073): `characterEditorShow(1)`'s sex toggle already writes
  `critterSetBaseStat(gDude, STAT_GENDER, …)` (character_editor.cc:3675); read it back and
  append as a **13th int** on the `create` line.
- Verb parse (server_control.cc:1431-1454): widen `v[]` to 13;
  `spec.gender = (n2 > 12 && v[12] != -1) ? v[12] : GENDER_MALE`. Positional append keeps
  "SPECIAL alone" working; both ends ship together (wire_defs.h:22 no-compat) → no version
  dance.
- `playerCreateApply` (player_create.c:96): after resets at :139-141
  (`protoCritterDataResetStats` is what wipes gender today),
  `critterSetBaseStat(actor, STAT_GENDER, spec->gender)` — STAT_GENDER (stat_defs.h:57)
  passes the derived-stat refusal (guard covers STAT_LUCK..POISON_RESISTANCE, stat.cc:516),
  no trait modifier. Both apply sites (spawn drain :1284, existing-slot :1637) funnel here.
- **Re-fid:** slots ≥ 1 need nothing — the drain's `serverRequestRebaseline`
  (server_control.cc:1299) reaches `protoPlayerActorsUpdateLook` (server_loop.cc:317 →
  proto.cc:1008-1040), which derives from each actor's OWN STAT_GENDER + does
  `objectSetFrame(0)`. **Slot 0 is the gap**: that loop starts at slot 1 (proto.cc:1012) and
  the first named login can create on slot 0. In `playerCreateApply` when `slot == 0`, call
  `_proto_dude_update_gender()` (proto.cc:~948-987, legitimately updates `_art_vault_guy_num`)
  **plus `objectSetFrame(gDude, 0, nullptr)`** — the vanilla path (proto.cc:981) lacks the
  frame reset (same gotcha client_net.cc:2991 patches).
- **Persistence is free:** gender = `baseStats[STAT_GENDER]` in CritterProtoData → rides the
  sheet row into join blob, PAC2 disk appendix, and the Item-1 delta channel; body FID
  streams via baseline / OBJECT_DELTA_FID.

**(b) De-globalize `_art_vault_guy_num`.** New helper `artDudeBaseFrmIdFor(Object* critter)`:
if `playerActorSlotOf(critter) > 0` → `_art_vault_person_nums[gameMovieIsSeen(MOVIE_VSUIT) ?
JUMPSUIT : TRIBAL][critterGetStat(critter, STAT_GENDER)]` (the proto.cc:1010-1024
derivation verbatim); else return `_art_vault_guy_num`. Gating on **slot > 0** (not
`playerActorIs`) keeps single-player/golden byte-identical.
- **interpreter_extra.cc:451** (highest value — server-side under ServerActorScope, so
  `a1 == gDude` is the acting extra, and the wrong fid streams to everyone as authoritative):
  `buildFid(FID_TYPE(fid), artDudeBaseFrmIdFor(a1), …)`; add `objectSetFrame(a1, 0, …)` by
  the `objectSetFid` at :459 when the frm index changed — plausible reproducer of the banked
  "invisible after load/equip" tail in [[vault-suit-appearance-gap]].
- **inventory_ui.cc:2563 and :2578** (paper-doll `_adjust_fid`): `artDudeBaseFrmIdFor(_inven_dude)`.
  Today only accidentally right because `onMovieSeenState` (client_net.cc:2990) re-syncs the
  local global; a female character created before that sync renders a male doll.
- **object.cc:313 — leave it.** That constructs THE gDude at boot before any sheet exists;
  no subject to derive from. Document, don't touch.

**Live test.** `F2_PLAYER_CREATE=ui`, toggle female, login new name → female tribal body on
both screens; equip/remove leather armor → correct body, never invisible; host doll
unchanged; server restart + re-login → still female (appendix round-trip).

---

## ITEM 3 — Attack-weapon on the wire (bug D)

**Investigation verdict.** A non-null `attack->weapon` Object IS needed beyond the fid anim
code — but the **mirror**, not the Attack, is where to guarantee it.
- `_action_attack` re-resolves the swing anim from the attacker's **inventory**, not
  `attack->weapon` (actions.cc:777 → `critterGetAnimationForHitMode` → item.cc:1347-1351 →
  `critterGetWeaponForHitMode` item.cc:1008). Empty mirror → ANIM_THROW_PUNCH →
  `_action_melee` (actions.cc:778-779) = bug D's punch. So "anim code on wire, no Object"
  (option 2) needs surgery inside `_action_attack` and still fails:
- `_action_ranged` hard-derefs the weapon (actions.cc:902-903; sfx :935/:1036/:1083;
  projectile pid :947) and the throw branch consumes the weapon **out of the attacker's
  inventory** as the projectile (:955-979) = bug D.2's spear.
- Option 1 (netId only) fails by construction: the missing weapon has no mirror object and
  no `_net` entry (reconcile client_net.cc:1807-1819 never creates).

**RECOMMENDED: weapon netId + pid on the wire, plus lazy single-item mirror completion at
replay-reconstruct time** — a surgically-scoped option 3 that only ever **creates**, never
frees, so the double-free hazard the 1807-1819 comment defends (freeing under an in-flight
reg_anim) is untouched.

- **Wire.** `attackResult` (presenter_network.cc:616-632): append **after the extras block**
  (length-prefixed events make tail-appends safe, cf. turnStart's `_combat_free_move`
  :593-599) — `putI32(netIdOf(attack->weapon))` (0 = unarmed) and
  `putI32(weapon ? weapon->pid : -1)`. No `kWireVersion` bump (wire_defs.h:23 — ship
  together; readers stop at event length).
- **Consumer.** `onAttackResult` (client_net.cc:2320): read both if `!r.overflow()` into
  `PendingAttack` (:289; add `weaponNetId=0`, `weaponPid=-1`). `playPending` (:2739)
  replaces line :2761 with a ladder on the attacker's mirror:
  1. `w = critterGetWeaponForHitMode(attacker, hitMode)`; if `w` and (`weaponPid == -1` or
     `w->pid == weaponPid`) → use (today's path, now verified).
  2. `weaponPid == -1` → null weapon, true unarmed.
  3. Else find by `lookup(weaponNetId)` then pid scan → clear `OBJECT_IN_ANY_HAND` on other
     items, set the hand flag for the hit mode (item.cc:1010-1019). Convergent: the server's
     OBJECT_DELTA_INVENTORY for the wield lands this same beat anyway.
  4. Else **create:** `objectCreateWithPid` → `_obj_disconnect` → `applyWireItemAmmo`
     (proto default) → `itemAdd(attacker, item, 1)` → hand flag (the gDude reconcile idiom,
     client_net.cc:1771-1776). Set `item->netId = weaponNetId` and register in `_net`
     (forgetObjectRefs-on-free trap) so thrown-weapon adoption keys match
     (`presRecordSetAdoptNetId` matches on `weapon->netId`, actions.cc:1013) and the landed
     spear doesn't duplicate.
  Then `attack.weapon = resolved`. **No client_present.cc change** — the fid-arm at
  client_present.cc:1568-1574 reads `attack->weapon`, and `_action_attack`'s inventory
  re-resolve now finds the flagged item → melee swing, ranged fire, throw consumption, and
  projectile origin all fall out.

**Hazards.** Creation-only discipline (never free/recreate in steps 3-4). The one
review-carefully point is step 4's netId adoption — if fiddly, shipping netId 0 costs only a
cosmetic duplicate ground spear until the next reconcile/baseline. Attacks riding the
recorded-seq channel (EVENT_PRES_SEQ) are recorded server-side where the real weapon exists
and never had bug D; this fix covers the reconstructed EVENT_ATTACK_RESULT path only, and
`playPending` is only reached for `kAttack`.

**Live test.** P2 joins, THEN P1 picks up a spear + a pistol. P1 melee → P2 sees armed idle
+ thrust + weapon sfx; `[atk] REPLAY` trace (client_net.cc:2788) prints `resolvedWeaponPid`
== wire pid, non-punch animCode. P1 throws spear → projectile leaves P1's real tile, lands,
exactly one ground spear. Regressions: pre-join weapons still animate; fists still punch.
