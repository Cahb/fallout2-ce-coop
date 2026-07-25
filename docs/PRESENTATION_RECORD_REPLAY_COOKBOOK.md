# Presentation Record/Replay — Plumbing Cookbook

**Read this before wiring anything into the record/replay channel.** Its job is to
kill the tax every fresh session pays re-deriving the same substrate ("what's an
Object", "what's a netId", "why NO_SAVE", "why does OBJ_CREATE not fire"). Part 1 is
the substrate (read once). Part 2 is the step-by-step recipe to migrate one animation
family. Part 3 is the debug/gotcha kit.

Companion docs: `PRESENTATION_RECORD_REPLAY_SPEC.md` (the *why* / design of record — its
Appendix A holds the folded-in rationale/lineage), `src/pres_record.h` (the one-screen
COVERAGE MANIFEST — which families are done/scoped). Line numbers are anchors; re-verify if
code moved.

---

## Part 1 — The substrate (the stuff you keep re-deriving)

### 1.1 The three object identities — do not confuse them
An `Object` carries three IDs; presentation wiring only ever cares about `netId`.
- **`id`** — the sim/script identity (`scriptsNewObjectId`). Save-stable. NOT the wire id.
- **`cid`** — combatant id (combat bookkeeping). Irrelevant to presentation.
- **`netId`** — the WIRE identity: the unique handle the viewer resolves an object by.
  Assigned on create **only when `serverLoopActive()`** (`object.cc:762-764`).
  `objectNextNetId()` = `gNextNetId++` (post-increment, `object.cc:4495`);
  `objectGetNextNetId()` = peek. Renumbered LOW & monotonic at every rebaseline
  (gDude → netId 1, `object.cc:3392/4536`). `objectFindByNetId(n)`: `n <= 0` never
  matches. A fresh object starts `netId = 0` (`object.cc:3475`).

### 1.2 `OBJECT_NO_SAVE` is NOT "transient" — the trap
`OBJECT_NO_SAVE = 0x04` (`obj_types.h:58`), `OBJECT_HIDDEN = 0x01`, so flags `0x5` =
a hidden NO_SAVE transient (explosion cloud / synthetic attacker); `OBJECT_LIGHT_THRU
= 0x20000000` rides normal critters (a persistent-object tell in flag dumps).
**`gDude` and `gEgg` are NO_SAVE too** (`object.cc:316`) yet fully persistent — gDude
saves via a special path. So NO_SAVE alone cannot mean "the viewer lacks this". What
NO_SAVE *does* reliably mean: **not replicated to the viewer** (no spawn event gave it
a netId there) and **netId zeroed at rebaseline** (`object.cc:4552-4561`). Combine it
with "created this beat" to get a real transient test (§1.5).

### 1.3 `serverLoopActive()` — one gate, three meanings
True in BOTH f2_server AND the headless golden state-dump harness. It governs:
1. netId assignment on create (§1.1).
2. The `!serverLoopActive()` skip of every composite's ANIMATE branch (the headless/
   server path takes a STATE fast-path that applies the outcome directly).
3. Which fast-path arm runs (`else if (serverLoopActive())` STATE blocks).

**Record mode** = `serverLoopActive() && presRecordEnabled()` → RUN the normally-
skipped animate branch inside a record section, then STILL run the STATE block (its
reg_anim callbacks are DROPped by the recorder — state comes from the fast-path).

### 1.4 The two animation backends (backend split — cost a SIGBUS to learn)
- `fallout2-ce` (client + golden harness) links `animation.cc` — the REAL reg_anim +
  InstantAnimationScheduler.
- `f2_server` links `server_anim.cc` — the RECORDING leaves (each `animationRegister*`
  appends a typed op instead of animating).
- They are **mutually exclusive** (both define `reg_anim_begin`). The recorder is armed
  ONLY in f2_server (`presRecordSetBackendActive(true)` at static init). So
  `presRecordEnabled()` = env flag `F2_SERVER_PRES_RECORD` **AND** backend armed → the
  recorder is inert in the client/golden binary, and goldens stay byte-identical.

### 1.5 The wire pipeline + object references
Server records leaves → flat buffer → ships as `EVENT_PRES_SEQ` (id 31) → viewer
replays the ops through its OWN real reg_anim (`client_net.cc presPlayRecordedSeq`).
Vanilla-faithful by construction — no per-action replay function.

A recorded op references objects by an `i32 ref`:
- **`ref > 0`** = **netId** → viewer resolves via `objectFindByNetId` (nullptr if it
  lacks it). Use for PERSISTENT objects (victim, door — the viewer already has them).
- **`ref < 0`** = **stream handle** minted by a lazy `OBJ_CREATE{handle,fid,tile,elev,
  flags}` → viewer creates a local NO_SAVE transient under that handle. Use for objects
  the viewer can't resolve.
- **`ref == 0`** = null.

The recorder's discriminator (`pres_record.cc resolveRef`):
```
transient = (netId == 0) || (netId >= beatWatermark && (flags & OBJECT_NO_SAVE))
```
- `beatWatermark` is snapshotted at **beat start** (`serverTick` top,
  `presRecordBeatBegin`) — BEFORE composites build their transients. (Snapshotting at
  `SectionBegin` was too late: composites create clouds/attackers up front, so they
  fell below a section watermark and shipped by an unresolvable netId — the "explosion
  clouds vanished" regression.)
- Both signals are required: `netId >= watermark` = created-this-beat (its netId is
  dead by pump time); `NO_SAVE` = not replicated (no viewer-side spawn). gDude is
  NO_SAVE but old → netId. A same-beat-spawned PERSISTENT critter is new but replicated
  → netId. Only NO_SAVE-and-new → OBJ_CREATE.

**Viewer resolves unknown refs to nullptr and silently skips** (every op is
`if (o) animationRegister…`) — a dropped ref = a missing beat, never a crash. The
interpreter already executes ALL of Table A; migrating a family adds a SERVER record
site, NEVER a viewer op handler.

---

## Part 2 — The recipe: migrate one animation family

### Step 0 — Find the composite site
The family's outcome is produced by a composite (`actions.cc` / `combat.cc` /
`proto_instance.cc`) with an animate branch it skips when `serverLoopActive()`. Grep
the FAMILY TABLE in `src/pres_record.h` for the site.

### Step 1 — Hazard check (W3): does the build phase mutate authoritative state?
The animate branch must be REGISTER-ONLY (it may only schedule reg_anim leaves). Audit
for register-time mutation:
- **Safe:** `actionKnockdown` computes a tile and registers a move — mutates nothing
  now (`actions.cc:149`). `_show_damage_to_object` for `critter_damage` has knockback
  ≈ 0 (`_compute_dmg_damage:2354` nulls the knockback for non-NO_KNOCKBACK critters).
- **Scary:** the attack THROW branch mutates inventory during build (spec §8) — guard
  before migrating attack (it's LAST for this reason). Per spec §5.4: run-and-let-
  aborts-name-it — run the case and let a crash/assert name the hazard rather than
  auditing blind.

### Step 2 — Relax the gate (the two-`if` pattern)
Replace the composite's `if (animate && !serverLoopActive()) {…} else if (serverLoop
Active()) {STATE}` with the explosion's pattern (`actions.cc:1868` is the reference):
```c
bool recording = serverLoopActive() && presRecordEnabled();
if (animate && (!serverLoopActive() || recording)) {
    if (recording) presRecordSectionBegin();
    reg_anim_begin(...);
    ...leaves...                 // unchanged — these now RECORD under the section
    if (reg_anim_end() == -1) { if (recording) presRecordSectionAbort(); ...cleanup...; return; }
    if (!recording) { ...SP-client tail (gameUiEnable/return)...; }   // MUST return — do not fall into STATE
    presRecordSectionEnd();
    presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount() /*, actorNetId*/);
}
if (serverLoopActive()) { ...STATE block (authoritative apply, unchanged)... }
else { ...non-animated SP... }
```
Control-flow invariants to preserve (verify each against the four cases):
- **SP client** (serverLoopActive false): recording is false → runs the animate branch,
  hits `!recording` tail, **returns** (must NOT fall into the STATE/non-animated ifs).
- **Golden/f2_server flag-off** (serverLoopActive true, recording false): the first `if`
  is false → STATE branch, **byte-identical** to the old `else if`. This is the golden
  path; it must not change.
- **f2_server flag-on** (recording true): records, ships presSeq, falls through to STATE
  (authoritative apply). The recorded reg_anim callbacks are DROPped.

### Step 3 — Object refs (usually free)
Persistent participants (victim/door) → netId automatically. Synthetic transients you
create THIS beat (a cloud, the `actionDamage` FID_0x20001F5 attacker) → OBJ_CREATE
automatically, **provided they're created after `presRecordBeatBegin` ran** (i.e. by
normal action dispatch inside a serverTick — always true). Nothing to wire.

### Step 4 — Actor routing (the presSeq `actorNetId` arg)
- **World/ambient effect** (explosion, scripted `actionDamage`): `actorNetId = 0` →
  viewer replays immediately.
- **Actor-initiated out-of-combat action** (gesture, door-by-a-mover): pass the actor's
  `netId` → the viewer routes it through the approach-glide pump (waits out the actor's
  walk before playing). See gesture slice (`server_control.cc:274`).

### Step 5 — Callback tags (W1, only if the family registers a RECORD-class callback)
Most callbacks need NO tag: DROP (state the fast-path already applied), EXEC (server
runs it at record time), LOGIC (pure gates), LOCAL (client-only HUD). Only a RECORD
callback — one that PRESENTS (e.g. `_show_death` → corpse fid/flat) — needs a
`PresCallbackTag` + a client dispatch arm. v1 ships only `SHOW_DEATH`.

### Step 6 — Prove purity + verify
1. Add a CASE to `tests/golden/run_record_purity.sh` (`name|map|ticks|tick:verb:arg`).
   Differential identity test: f2_server twice (flag off vs on), require state dump +
   no_save leak count identical. No baseline, no rebless.
2. `scripts/check.sh` — goldens byte-identical (the family is inert on the golden path).
3. **Live-verify on the viewer** (§3.1) — there is NO headless oracle for the viewer
   replay, only the server sim is gated. Adversarial review is mandatory (goldens pin
   determinism, not viewer correctness).

---

## Part 3 — Debug / gotcha kit

### 3.1 Live-verify recipe
```
F2_SERVER_PRES_RECORD=1 F2_TRACE_EVENTS=1 VIEWERS=1 PACE=100 \
  MAP=denbus2.map scripts/viewer_live.sh
```
Trace: `[presseq] SEND ops=N actor=M` (server) / `RECV` (viewer). Gesture = ops≈3,
door = ops≈4. `pkill -x f2_server fallout2-ce` between runs (visual-verification-protocol).

### 3.2 Headless scripted run (no socket) — for purity + op inspection
```
cd FO2 && F2_SERVER_MAP=kladwtwn.map F2_SERVER_SEED=1337 F2_SERVER_TICKS=600 \
  F2_SERVER_ACTIONS="300:explode:200" F2_SERVER_DUMP=/tmp/x.dump F2_SERVER_LEAKPROBE=1 \
  F2_SERVER_PRES_RECORD=1 ../build/f2_server
```
Deterministic maps only (kladwtwn/artemple/newr1/vault13/arcaves). To see the
transient/netId decision per ref, temporarily add a `getenv("F2_PRESREC_TRACE_REF")`
stderr line in `resolveRef` (this is how the clouds-regression was ground-truthed).

### 3.3 Recurring gotchas
- **Clouds/attacker invisible on viewer** → a transient shipped by netId (unresolvable).
  Check `resolveRef`: was it created after `presRecordBeatBegin`? Is it NO_SAVE? (§1.5)
- **A "duplicate hidden dude/critter"** → a persistent object mis-minted as OBJ_CREATE.
  The NO_SAVE conjunct exists to prevent exactly this; don't drop it.
- **SIGBUS/double-free enabling record in fallout2-ce** → you ran the recording path in
  the REAL-reg_anim binary; the recorder is f2_server-only by design (§1.4).
- **Golden diff after a "presentation-only" change** → the change leaked onto the golden
  path (serverLoopActive true, flag off). It must be gated behind `recording`.

### 3.4 Worked example — receive-damage / hit-react (`actionDamage`, `actions.cc:2248`)
Scripted/environmental damage (the `critter_damage` opcode — traps, steam, forced
damage; NOT combat hits, which ride attack). Records: `SFX(whc1xxx1)` on the synthetic
attacker (OBJ_CREATE — first family to exercise it for an attacker) + `_show_damage`
(ANIM_HIT_FROM_* / death anim on the persistent defender, netId) + `CALL{SHOW_DEATH}`
when lethal. STATE block applies damage authoritatively. Knockback ≈ 0 for this path,
so no move divergence. This is the reference for a clean, low-hazard migration.
