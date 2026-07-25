# PRESENTATION_RECORD_REPLAY_SPEC.md — record the leaves, replay the stream

Status: **spec of record** for the presentation record/replay channel. The design
rationale and lineage from the two precursor docs — the "state SNAPS / presentation
ANIMATES" principle, the three-bucket closure argument, the fast-path law, the STATE
gaps, and the prior-art vocabulary — are folded into **Appendix A**. Read alongside
`PRESENTATION_RECORD_REPLAY_COOKBOOK.md` (the migration recipe), `COMBAT_CLIENT_DESIGN.md`
§3 (the per-action replay precedent this generalizes), and `MP_PROTOCOL.md` (wire framing).

Every claim below carries a file:line where one exists. Where I could not verify from
the code, it says so explicitly. Nothing in this document is built yet; the §7-style
explosion probe (§11) is the de-risk gate before anything else.

---

## 0. Verdict up front

**Yes, with boundaries.** The record/replay-the-leaves design is the convergent
endgame and it is cheaper than continuing to hand-write per-action replays —
**provided** you accept three corrections to the original mental model:

1. The leaf seam (`server_anim.cc`) records the *sequence*; it is **blind to the
   callback bodies** and to the direct object-primitive calls composites make while
   building a sequence. Closing those is a bounded, enumerable, one-time cost
   (§4.2, §4.4) — but it is real work that touches `actions.cc`/`proto_instance.cc`,
   not a zero-cost "instrument one file and done."
2. The client_present **pacing/reconcile substrate stays**. What this design deletes
   is the per-action *replay builders* (bespoke choreography re-creators), not the
   glide queue, the presentation pump, or the reserve/tripwire machinery (§8).
3. The classification work over the leaves + callbacks is **common to every option**,
   including "just keep writing per-action replicas." The marginal cost of the
   recorder over the replicas is small; the marginal benefit (no more replicas,
   exact timing, bucket C) is large **iff bucket C is wanted** (§10).

Do not block Demo v1 on this. The explosion probe is the first slice, post-demo.

---

## 1. The one-line idea (recap)

Stop hand-writing a replay function per action. The presentation of *every* engine
action and script cutscene bottoms out in a **closed set of leaf calls** (the
`animationRegister*` family + a closed callback set). Classify the leaves once; make
them **record** a typed command instead of no-op'ing on the server; run the real,
unmodified composite (`actionExplode`, `_action_attack`, a raw script's reg_anim
block) inside an authoritative-state-inert critical section; ship the recorded
command stream; replay it on the viewer with ONE generic interpreter that feeds the
client's **own real** `reg_anim` engine. Vanilla fidelity by construction; zero
per-action hand-picking.

---

## 2. The mental model: `actions.cc` vs `server_anim.cc` (the confusion, resolved)

A previous session left the owner with the takeaway "it's not about actions.cc, it's
about server_anim.cc — tap server_anim, not actions." That was **right about where
the recording hook lives and incomplete about everything else**. Here is the precise
picture, because getting this wrong makes the whole design unbuildable.

### 2.1 Does `actions.cc` run on the dedicated server? YES.

`actions.cc` executes **in full** on the server; it is not bypassed, replaced, or
stubbed. When C4 detonates, the server runs the real `actionExplode`
(`src/actions.cc:1755`): it creates the 7 transient cloud objects (:1766-1796),
rolls the damage (:1816-1828), computes death flags (:1830), and applies the outcome
(:1879-1951). The ONLY thing that differs from single-player is that the
**presentation branch inside it** is gated:

```c
if (animate && !serverLoopActive()) {   // actions.cc:1838 — the server SKIPS this
    ... sfx + cloud reveal + _show_damage + callbacks ...   // :1841-1877
} else if (serverLoopActive()) {
    ... the state-equivalent fast-path ...                  // :1879-1927
}
```

So: **the server "handles the action" by running the composite in actions.cc**, and
inside that composite the *presentation steps* are skipped while the *state steps*
run. "The server handles actions" and "we tap actions.cc" are different claims — the
first is true today; the second is what this design deliberately avoids.

### 2.2 Why the recording seam is the LEAVES (`server_anim.cc`), not the composites (`actions.cc`)

You do **not** instrument `actions.cc` per action — that is exactly the per-action
band-aid this design exists to kill (`actionExplodeReplay`, `actions.cc:1968`, is
that debt in the flesh: ~55 lines that mirror the animate branch by hand and must be
kept in sync with it forever). Instead you instrument the **closed leaf set** once:
on the dedicated server, every `animationRegister*` call already funnels into ONE
file, `src/server_anim.cc`, where the presentation-only leaves are silent no-ops
(`:713-786`) and `reg_anim_begin/end` bracket a sequence (`:651/:665`). Make those
leaves **append a typed command** instead of returning 0, and then *every*
composite — `actionExplode`, `_action_attack`, `_show_damage_to_object`, and a raw
script's `reg_anim` cutscene that has no action function to replicate at all —
records itself **by simply running**. Instrument ~20 leaves once; cover an unbounded
set of composites forever. That is the entire economic argument.

### 2.3 But BOTH files are touched — in different roles

To have anything to record, the animate branch inside `actions.cc` must actually
**run** on the server — and today it is skipped by the `!serverLoopActive()` gate
(:1838). So the change has two legs:

- **`actions.cc` (and the other fast-path sites): relax the gate in record mode.**
  A small, per-fast-path-site edit: `if (animate && (!serverLoopActive() ||
  presRecordActive()))` around the existing branch, bracketed by the record critical
  section (§5). The branch body itself is untouched.
- **`server_anim.cc`: the leaves record instead of no-op.** The one-time central
  seam — one file, ~20 functions, each gaining an `if (recording) { append(op);
  return 0; }` head.

The prior agent's "tap server_anim not actions" described leg 2 and glossed leg 1.
Both are required; neither is per-action work (leg 1 is per *fast-path site*, a
closed set of ~21 sites of which only the animate-bearing ones matter — 2 in v1).

### 2.4 The crisp model to hold

> **`actions.cc` = the recipe.** It runs on the server today, in full, with its
> presentation steps skipped.
> **`server_anim.cc` = the individual instructions the recipe calls.** On the server
> they are no-ops (presentation) or immediate state appliers (movers).
> **Record mode = run the recipe with RNG frozen and the instructions writing
> themselves down instead of doing nothing.**
> **Ship the written-down list; the client replays it through its own REAL
> instructions** (the SDL `animation.cc` engine) — which is why the result is
> vanilla-faithful for free.

### 2.5 What the leaf seam CANNOT see — the callback-body blind spot (crux)

The owner spotted the hole, and it is real: `actions.cc` registers **file-local
static helpers as reg_anim callbacks**. Example: `hideProjectile`
(`actions.cc:2523`, static; body = `objectHide(projectile)` +
`presenter()->worldInvalidateRect` + `OBJECT_NO_SAVE`) is registered at
`actions.cc:1055` inside `_action_ranged`. The seam at
`animationRegisterCallback/Callback3/CallbackForced` (`server_anim.cc:773-786`)
captures **the fact** that a callback with target X and args (a1, a2) was registered,
in sequence order — but it cannot capture **what the body does**: on the server the
callback is never invoked (`return 0`), and the body lives elsewhere as a static.

So the design decomposes into two unequal halves:

- **The sequence half is generic and free**: order, delays, anims, sfx, fids,
  brackets — all captured by ~20 leaf instrumentations, no per-composite work, and
  it covers future scripts/mods by construction.
- **The callback half is bounded but real**: every callback *target* must be
  enumerated (it is a closed set — §4.2 proves it: 38 registration sites, 24 unique
  targets, 5 files), classified (presentation / state / logic / client-local), and
  the presentation-bearing ones given a wire tag + client-side dispatch, with their
  pointer args made serializable. This work touches `actions.cc`,
  `proto_instance.cc`, `combat_ai.cc`, `interface.cc` — **per callback target, not
  per action** — and it is the honest price of "100% coverage."

There is a second, smaller blind spot of the same shape: composites also make
**direct object-primitive calls** while building a sequence (creating/hiding/placing
projectiles and clouds: `objectCreateWithFidPid`/`objectHide`/`objectSetLocation`/
`objectSetLight`/`objectSetRotation` at `actions.cc:897-913` and `:1766-1796`).
Those never pass through `server_anim.cc` either. §4.4 and §6.2 close this with the
recorder's transient-capture rule instead of more per-site edits.

---

## 3. Is the unification correct? (collapsing buckets B and C into one channel)

**Mostly yes — with one boundary kept.** The seam doc's three buckets survive as
*analysis*; as *mechanism* the B/C split collapses:

**What unifies (and should):** everything that flows through `reg_anim` brackets.
Bucket C (ordered scripted choreography) and the multi-step bucket-B members (the
explosion's cloud+sfx+gibs, the attack choreography, the door slide) are the same
thing at the leaf layer — an ordered sequence of `animationRegister*` calls plus
callbacks — and hand-splitting them is exactly the per-action hand-picking the owner
is complaining about. One recorded stream carries both. The rule is mechanical, no
judgment per action: **if it goes through `reg_anim_begin/end`, it records** — even a
1-op sequence (uniformity beats special-casing; a 1-op stream is ~20 bytes).

**What stays outside (the boundary):** the *direct* `presenter()` one-shot cues that
never touch reg_anim — `consoleMessage`, `floatText`, `sfxPlay/At`, screen fades
(`presenter.h:58/334/345/351`; routed from the opcode handlers — Appendix A.2,
bucket B). These are already on the wire as single events,
carry no ordering, and wrapping each in a one-op recorded stream buys nothing and
costs an envelope + interpreter trip per cue. **Keep them as-is.** A lightweight
one-shot cue is still cheaper than a recorded stream exactly when there is no
sequence — i.e. when there is nothing to order. That is the whole boundary; it
requires no per-action decisions, only "did this come through reg_anim or through a
direct presenter call," which the code already decides for us.

**Semantic-envelope events** (`EVENT_ATTACK_RESULT`, `EVENT_DOOR_STATE`,
`EVENT_EXPLOSION_FX`, `EVENT_ACTION_ANIM`, `EVENT_WEAPON_TAKE_OUT`) are the middle
ground the recorded stream eventually replaces — but only after live-verify, per the
migration path (§8). Their ids are append-only and get *retired, never reused*.

---

## 4. Leaf classification — the one-time bounded work

### 4.1 Table A: the `animationRegister*` / reg_anim leaf set (`src/server_anim.cc`)

Classification: **UI** = record a command (server state untouched); **STATE** =
authoritative, already applied server-side + rides OBJECT_DELTA/MOVE — do not record
the state, but see the SYNC note; **BOTH** = needs a split; **BOOK** = sequence
bookkeeping, recorded as structure.

| Leaf | server_anim.cc | Today on server | Class | Record-mode disposition |
|---|---|---|---|---|
| `reg_anim_begin` | :651 | no-op ret 0 | BOOK | record `SEQ_BEGIN{flags}` |
| `reg_anim_end` | :665 | no-op ret 0 | BOOK | record `SEQ_END`; closes the stream section |
| `reg_anim_clear` | :656 | cancels stepped walk | BOOK/STATE | keep behavior; optionally record `SEQ_CLEAR{ref}` so a replay cancels the same object's in-flight client sequence (v1: not recorded; the interpreter's own reserve/cancel rules cover it) |
| `animationIsBusy` | :672 | always 0 | BOOK | unchanged (getter; keeps server spin-loops instant) |
| `_register_priority` | **abort stub**, `server_stubs.cc:200` | ABORTS if reached | BOOK | must become benign + record `PRIORITY{n}` — today it is unreachable only because the animate branches are skipped; record mode makes it reachable (§5.4) |
| `animationRegisterAnimate` | :713 | no-op | UI | record `ANIMATE{ref, anim, delay}` |
| `animationRegisterAnimateReversed` | :718 | no-op | UI | record `ANIMATE_REV{ref, anim, delay}` |
| `animationRegisterAnimateForever` | :723 | no-op | UI | record `ANIMATE_FOREVER{ref, anim, delay}` |
| `animationRegisterAnimateAndHide` | :728 | no-op (banked: faithful = destroy) | UI | record `ANIMATE_AND_HIDE{ref, anim, delay}` — the client's real engine plays then destroys (that's how the cloud self-disposes) |
| `animationRegisterHideObjectForced` | :735 | no-op (banked) | UI | record `HIDE_FORCED{ref}` |
| `animationRegisterMoveToTileStraight` | :742 | no-op (**banked STATE gap** — knockback/stairs, seam doc gap #1) | BOTH | record `MOVE_STRAIGHT{ref, tile, elev, anim, delay}`; the STATE half (final tile) is separately owed by the banked gap fix (`objectSetLocation`) and must NOT be conflated with this record (§4.3) |
| `animationRegisterMoveToTileStraightAndWaitForComplete` | :748 | no-op | BOTH | as above, `MOVE_STRAIGHT_WAIT{...}` (used by `actionKnockdown`, `actions.cc:168`) |
| `animationRegisterPing` | :753 | no-op | UI | record `PING{flags, delay}` verbatim. Used by `_show_damage` (`actions.cc:643`) to stagger multi-victim reactions; runtime semantics live in `animation.cc`'s `ANIM_KIND_PING` arm (`animation.cc:1495`) — **not re-derived here**; the client's real engine interprets it (unverified detail, deliberately delegated) |
| `animationRegisterPlaySoundEffect` | :758 | no-op | UI | record `SFX{ref, name, delay}`. Names are server-computable: `sfxBuildCharName`/`sfxBuildWeaponName` live in `sfx_name.cc` (f2_core, CMakeLists:182) and are deterministic string builders |
| `animationRegisterSetLightIntensity` | :763 | no-op | UI (v1) | record `SET_LIGHT{ref, dist, intensity, delay}`. NOTE: object light is genuinely dual-purpose (LOS?) — seam doc banked it as "minor/visual"; keep UI in v1, revisit if a sim consumer of lightIntensity surfaces |
| `animationRegisterTakeOutWeapon` | :768 | no-op | UI | record `TAKE_OUT{ref, weaponAnimCode, delay}` (replaces the bespoke `EVENT_WEAPON_TAKE_OUT` eventually) |
| `animationRegisterSetFid` | :612 | **applies state** (objectSetFid + frame reset) | BOTH | keep the state apply (it is authoritative — armed fids etc. ride OBJECT_DELTA); ALSO record `SET_FID{ref, fid, delay}` so mid-sequence fid transitions play in order client-side. Final delta reconciles (same value) — benign double-apply, held by the reserve window (§7.3) |
| `animationRegisterRotateToTile` | :601 | **applies state** (objectSetRotation) | BOTH | keep apply; record `ROTATE{ref, tile}` for mid-sequence facing. Same reconcile argument |
| `animationRegisterUnsetFlag` | :632 | **applies state** (objectShow/light/flag clear) | BOTH | keep apply; record `UNSET_FLAG{ref, flag, delay}` — the cloud-reveal ordering (`actions.cc:1844`) needs it client-side |
| `animationRegisterMoveToTile` / `RunToTile` | :525/:530 | **applies state** (real walk; emits MOVE per hop) | STATE | do NOT record a move op (the hops already ride MOVE with durMs). Record a `SYNC_MOVE{ref}` marker instead so subsequent recorded ops wait for that object's glide (§6.1). v1 (explosion) does not need it; bucket-C staging does |
| `animationRegisterMoveToObject` / `RunToObject` | :589/:594 | **applies state** | STATE | as above |
| `animationRegisterCallback` / `Callback3` / `CallbackForced` | :773/:778/:783 | **no-op — the bucket-C dropped-state hole** (header :74-82) | per-tag | dispatch by the tag table (§4.2): RECORD / DROP / EXEC |

### 4.2 Table B: the defunctionalized callback set (closed — verified 2026-07-18)

Census: `grep -rn animationRegisterCallback src/ --include=*.cc --include=*.h`
excluding the two implementations (`animation.cc`, `server_anim.cc`) yields **38
registration sites in exactly 4 files** — `actions.cc` (29), `proto_instance.cc` (6),
`combat_ai.cc` (1), `interface.cc` (2) — and **24 unique targets**. The set is
closed; if a mod-support future adds script-registered callbacks, they arrive via
the opcode layer, which bottoms out in these same registrations (SCRIPT_OPCODE_MAP.md
proves the opcode world finite).

Dispositions: **RECORD** = presentation body; gets a wire tag, client dispatches it.
**DROP** = state the fast-path/server already applies; never in the stream.
**EXEC** = state-bearing outcome that today is *silently dropped* on the server (the
bucket-C hole, `server_anim.cc:74-82`); must run **server-side at record time**
(combat's resolve-at-assembly model) — never on the client. **LOGIC** = gating
check; server-only. **LOCAL** = client-HUD; never reaches the server path.

| Target | Body | Registered at | Body does | Class | Args marshalling |
|---|---|---|---|---|---|
| `_show_death` | `actions.cc:579` | `actions.cc:574` | fid→corpse + frame 0 + flat/NO_BLOCK + outline off (**presentation of the corpse**) + `itemDropAll` (:608-610, **STATE**) + invalidate | **RECORD, split** | `CALL{SHOW_DEATH, ref, anim:int}` — a2 is `(void*)anim`. Client tag body = `_show_death` with the `itemDropAll` half guarded (see §4.3) |
| `hideProjectile` | `actions.cc:2523` (static) | `actions.cc:1055` | objectHide(transient) + NO_SAVE + invalidate | **RECORD** | `CALL{HIDE_PROJ, ref}` — a1 is the `Attack*`, **ignored by the body**; only a2 serializes. Alternatively express as the existing `HIDE_FORCED` op — preferred, no tag needed (verify the timing nuance: hideProjectile fires at the *action frame*, not sequence end) |
| `_obj_drop` | `proto_instance.cc:678` | `actions.cc:556/:568` | drop weapon to ground (STATE; rides CONNECT/delta) | **DROP** | — |
| `_internal_destroy` | `actions.cc:392` | `actions.cc:566` | `_obj_destroy(weapon)` (STATE; rides DESTROY) | **DROP** | — |
| `_report_explosion` | `actions.cc:2027` | `actions.cc:1860` | damage/XP/aggro + frees `attack` (STATE) | **DROP** (fast-path runs it, `actions.cc:1923`) | — |
| `_combat_explode_scenery` | `combat.cc:5897` | `actions.cc:1853` | radius SCRIPT_PROC_DAMAGE (STATE — destroys the Temple door) | **DROP** (fast-path `actions.cc:1944`) | — |
| `_finished_explosion` | `actions.cc:2103` | `actions.cc:1861` | clears `_action_in_explode` | **DROP** (bookkeeping; record section must clear the flag itself on exit — §5.4) | — |
| `_report_dmg` | `actions.cc:2282` | `actions.cc:2239` | `_apply_damage` etc. (STATE) | **DROP** (fast-path `actions.cc:2247`) | — |
| `_is_next_to` | `actions.cc:1107` | `actions.cc:1158/1245/1344/1450/1666` | adjacency gate | **LOGIC** | — |
| `_check_scenery_ap_cost` | `actions.cc` (static, near :1107) | `actions.cc:1160/1248/1345/1451` | AP charge (STATE) | **LOGIC/EXEC** — on the server interaction fast-paths this is already handled; on a raw bucket-C path it is dropped today (EXEC at stage 3) | — |
| `_obj_use` | `proto_instance.cc:1456` | `actions.cc:1172/1276` | use-object outcome incl. script procs (STATE) | **EXEC** (stage 3) — server-side at record time; today the interaction executors fast-path it (`actions.cc:1195/1302` serverLoopActive branches) | server-side only; no wire form |
| `_obj_use_item_on` | `proto_instance.cc:1380` | `actions.cc:1274` | use-item-on outcome (STATE) | **EXEC** (stage 3; fast-pathed today at `actions.cc:1302`) | — |
| `_obj_pickup` | `proto_instance.cc:578` | `actions.cc:1370` | pickup (STATE) | **EXEC** (stage 3; fast-pathed `actions.cc:1195`) | — |
| `_obj_use_container` | `proto_instance.cc:1836` | `actions.cc:1398` | container open (STATE) | **EXEC** (stage 3) | — |
| `scriptsRequestLooting` | `scripts.cc:1236` | `actions.cc:1406/1452` | loot modal request (STATE/modal) | **EXEC** (stage 3; modal-driver territory) | — |
| `_obj_use_skill_on` | `proto_instance.cc:1919` | `actions.cc:1680` | skill-use outcome (STATE) | **EXEC** (stage 3; `actions.cc:1638` gate exists) | — |
| `_can_talk_to` | `actions.cc:2164` | `actions.cc:2158` | LOS/distance gate | **LOGIC** | — |
| `_talk_to` | `actions.cc:2183` | `actions.cc:2159` | dialog request (STATE/modal) | **EXEC** (stage 3; dialog-intent driver exists) | — |
| `_set_door_state_open` | `proto_instance.cc:1634` | `proto_instance.cc:1816/2135` | door state field (STATE) | **DROP** (door fast-path decoupled, `proto_instance.cc:1763/2090`) | — |
| `_set_door_state_closed` | `proto_instance.cc:1641` | `proto_instance.cc:1807/2128` | as above | **DROP** | — |
| `_check_door_state` | `proto_instance.cc:1648` | `proto_instance.cc:1826/2142` | drives the door frame slide from state | **DROP** in v1 (door slide already carried by EVENT_DOOR_STATE; migrates to a recorded stream in §8 step 3) | — |
| `_ai_print_msg` | `combat_ai.cc:3389` | `combat_ai.cc:3385` | AI float text | **DROP** (the server already streams real taunts as floatText — COMBAT_CLIENT_DESIGN §3.c; recording would double them) | — |
| `_intface_redraw_items_callback` | `interface.cc:1801` | `interface.cc:1830` | HUD redraw | **LOCAL** (client-only code path; the server never registers it) | — |
| `_intface_change_fid_callback` | `interface.cc:1808` | `interface.cc:1845` | HUD fid | **LOCAL** | — |

**Bottom line of Table B:** of 24 targets, only **2** need a client-side wire tag in
v1 (`SHOW_DEATH`, and `HIDE_PROJ` if not folded into `HIDE_FORCED`); **8** are DROPs
whose state the existing fast-paths already apply; **7** are the stage-3 EXEC class
(the real bucket-C state fix — a *sim-correctness* project independent of streaming,
unlocked by the same table); the rest are logic gates or client-local. The arg
marshalling problem (`void*` args) is per-tag and small: `(void*)anim` and
`(void*)skill` are ints in disguise; `Attack*` args never cross the wire (either
ignored by the body, or the target is a DROP/EXEC).

### 4.3 The "both" splits, named

1. **`_show_death`** — the one genuine `ui_only` split in v1. Presentation half
   (fid/frame/flat/outline) replays client-side via the tag; state half
   (`itemDropAll`, `actions.cc:608`) must NOT run on the viewer — the server's
   `critterKill` path owns corpse consequences and the drops ride CONNECT/delta.
   **Uncertainty flag:** today's attack replay already runs `_show_death` on the
   viewer (COMBAT_CLIENT_DESIGN S4 recon called it "replay safe as-is") — either the
   drop duplication is somehow benign or it is a latent live bug; the split should
   guard `itemDropAll` under `clientViewerActive()` regardless, and the probe's
   live-verify should watch for duplicate ground loot. Do not take the S4 recon's
   all-clear on faith (per `[[dont-declare-not-a-bug-confidently]]`).
2. **`animationRegisterSetFid` / `RotateToTile` / `UnsetFlag`** (Table A) — keep
   their server state-apply, add the record. The stream replays the *transition*;
   the delta carries the *end state*; the reserve window (§7.3) prevents the end
   state from painting first.
3. **The movers** — state stays state (MOVE events); the stream gets only a
   `SYNC_MOVE` ordering marker. Never record a mover as presentation — it would
   fight the authoritative MOVE lane (the exact "replaying knockback would fight the
   tile" rule, `actions.cc:1963-1965`).
4. **`animationRegisterMoveToTileStraight`** — currently a **no-op with a banked
   STATE gap** (knockback slide / stairs / scripted falls, `server_anim.cc:742`).
   The record op carries its presentation; the state gap remains owed separately.
   Do not let the stream paper over it: if the state fix lands later, the leaf
   becomes apply+record+SYNC like the other movers.

### 4.4 The second blind spot: direct object primitives in composite build phases

Composites create and place **transient objects** with direct calls that bypass both
the presenter lifecycle suppression concerns and `server_anim.cc`:
`actionExplode` :1766-1796 (7 clouds: create/hide/NO_SAVE/setLocation),
`_action_ranged` :897-913 (projectile: create/hide/setLight/setLocation/
setRotation). The recorder cannot see these as leaf calls. **Closure rule (no
per-site edits):** the recorder resolves every `Object*` a recorded op references
(§6.2); the first reference to an object *created since the current beat started*
lazily emits an `OBJ_CREATE{handle, fid, tile, elev, flags}` op capturing its
current pose. The NetworkPresenter already observes `objectCreated`
(`presenter.h:71`) — the recorder keeps the this-beat-created set from that hook.
This converts the direct-call build phase into stream data without touching the
composites. (The projectile's mid-sequence `objectSetLight` is presentation polish;
v1 captures create-pose only — flag: projectile light won't replay until SET_LIGHT
capture-at-create is extended. Cosmetic.)

---

## 5. Record mode — the critical section

### 5.1 Definition

**Record mode = authoritative-state-inert execution of the animate branch.**
Formally, for the recording to be observationally pure w.r.t. the sim, ALL of:

1. **RNG net-zero:** the authoritative RNG stream position at section exit equals
   entry. Mechanism: **snapshot/restore** (not a side stream — see below). The RNG
   state is tiny and fully accessible: `_idum`, `_iy`, `_iv[32]`
   (`random.cc:166-183`); `randomStateFingerprint()` (`random.cc:156`) is the
   already-golden-pinned tripwire that proves restoration byte-exactly.
2. **State callbacks dropped:** every Table-B DROP/EXEC/LOGIC target is *not
   invoked* by the recorder (v1 scoped mode); the fast-path branch outside the
   section applies the equivalent state, as it does today.
3. **Register-leaves record-instead-of-do:** inside the section, ALL leaves —
   including the normally state-applying `SetFid`/`Rotate`/`UnsetFlag`/movers —
   record only. This matters: the fast-path branch already handles the state
   (e.g. `_combat_apply_knockback`, `actions.cc:1921`), and a leaf that *also*
   applied would double-mutate. Example that would bite: the fire-dance branch
   registers `RotateToTile` + `MoveToTileStraight` (`actions.cc:485-486`);
   `RotateToTile` on today's server_anim *applies rotation* (:601) — inside the
   section it must not.
4. **No presenter STATE emissions from section-created transients:** objects the
   section itself creates/moves/destroys must not leak SPAWN/MOVE/DESTROY onto the
   state lane (they'd reference objects the client tears down same-frame). In v1
   (explosion) the transients are created *outside* the section (:1766-1796, before
   the gate) and already emit a net-zero spawn/destroy pair — unchanged, harmless
   (this is today's behavior). If record mode is later widened to sections that
   create objects inside (`_action_ranged`'s projectile), suppress their lifecycle
   emissions via the created-this-beat set (§4.4) — the stream's `OBJ_CREATE`
   replaces them.
5. **Malloc'd scratch freed on the normal path:** the animate branch's failure
   handling (`reg_anim_end() == -1`, `actions.cc:1862-1875`) destroys transients
   and frees `attack` — under the recorder `reg_anim_end` returns 0, so that path is
   dead; the fast-path branch's ownership of `attack` (freed by
   `_report_explosion`, :1923) is unchanged.

### 5.2 Where the last authoritative roll happens (the compute-then-present proof, per action)

For **`actionExplode`**: every authoritative roll is spent before the gate —
`_compute_explosion_damage` per victim (`randomBetween` at `actions.cc:2113`,
called from :1816 and :1827) and `attackComputeDeathFlags` (:1830, deterministic
given damage). The gate at :1838 opens the animate branch; nothing after it feeds
state. **Verified for explosion.** For **`actionDamage`**: same shape — damage
rolled at :2300-region, gate at :2235. For **`_action_attack`** (combat): the
attack is fully resolved by `_combat_attack` before `_action_attack` animates
(COMBAT_CLIENT_DESIGN §3.c). **This ordering must be re-proven per fast-path site
as each is migrated** — it is a per-site checklist item, not a global theorem. The
snapshot/restore mechanism makes a violation *loud* rather than silent: if a
composite ever rolled authoritative state inside its animate branch, restore would
rewind a real roll — which is why the section must also **assert
`randomStateFingerprint()` equality in the golden state dumps** (already pinned)
and why the animate branch must never mutate sim state (invariant 3).

**Why snapshot/restore over a side stream:** a side stream (swap the RNG for a
separate generator inside the section) also achieves net-zero, but it changes
*which* values presentation-internal rolls see and adds a second generator to carry.
Snapshot/restore is 34 ints copied twice, uses the real generator (the cosmetic
choice inside the section is drawn from the authentic stream state, then unwound),
and its correctness is mechanically checkable via the fingerprint. The riff's
"frozen" wording resolves to: **run free, then restore** — not "return a constant"
(a constant would break `randomBetween(2,5)`-style loops, e.g. the fire-dance
placement loop `actions.cc:479-499`, which needs varying values to terminate
sensibly).

Honest nuance carried over: the cosmetic choices inside the section
(gib pick, fire-dance fling) are baked into the recording — every client sees the
same one. Fine: no canonical renderer exists in MP; SP's per-render variety is
unobservable.

### 5.3 The three record contexts (they have different rules — do not conflate)

1. **Scoped record section (v1, the probe):** wraps the gated animate branch of a
   fast-pathed action. Leaves record-only; RNG snapshot/restore; DROP callbacks
   dropped. Requires relaxing the gate at the site. Activation:
   `F2_SERVER_PRES_RECORD=1` env + the presenter opting in
   (`wantsPresentationRecording()`, the `wantsSnapshotBlob()` idiom,
   `presenter.h:185`) — goldens stay byte-identical until the default flips.
2. **Ambient recording (stage 2 — bucket C presentation):** the *always-running*
   server_anim path (scripted reg_anim, AI choreography) additionally records
   presentation ops per top-level `reg_anim_begin/end` bracket. No gate changes, no
   RNG section needed (recording observes registrations that already happen; it
   executes nothing new). Movers keep applying state and add `SYNC_MOVE` markers.
   Purely additive; wire-only effect.
3. **Callback-EXEC (stage 3 — bucket C state, the `kill_the_guy` fix):** the
   Table-B EXEC targets start executing server-side at register time (combat's
   resolve-at-assembly model; never run timed sequences that block on
   `animationIsBusy` — seam doc §C.i). **This is a sim-correctness change**
   (today those callbacks are silently dropped, `server_anim.cc:74-82`), gets its
   own golden re-bless + adversarial review, and is valuable even if streaming
   never ships. Keep it a separate commit series.

### 5.4 Enumerated risks

- **Abort stubs become reachable.** `_register_priority` is
  `serverStubAbort` (`server_stubs.cc:200`) and sits at `actions.cc:1842` — first
  line of the branch record mode runs. There WILL be others. Mitigation is the
  `[[sweep-before-recon]]` method: build with the record gate on, run every animate
  branch (explosion/damage/attack under a harness), and let the aborts name the
  stubs to soften. Budget a short sweep, not a guess. (Verified benign already:
  `gameUiDisable/Enable` are silent no-ops, `server_stubs.cc:350-351`;
  `textObjectsGetCount` returns 0, :410; art queries work server-side — `artExists`
  is already load-bearing on the server via `_knockback_headless_dest`,
  `actions.cc:211`, and `art.cc` is f2_core, CMakeLists:34.)
- **Register-time state mutation inside an animate branch.** The known offender:
  the **thrown-weapon** branch of `_action_ranged` mutates inventory during
  sequence *build* (`itemRemove`/`itemReplace`/`_obj_connect`,
  `actions.cc:879-895`) — genuine SP state welded into presentation code, outside
  the callback system entirely. In record mode this would double-apply against the
  combat fast-path's own accounting. Must be guarded (`presRecordActive()` skip)
  or the attack migration must exclude throws initially — same hazard the S4 recon
  flagged for the *viewer* replay (COMBAT_CLIENT_DESIGN §3.c NEW caveat). Assume
  more such sites exist until the sweep proves otherwise.
- **RNG inside the animate branch** — real, and handled by snapshot/restore:
  fire-dance `randomBetween(2,5)/(0,5)` at `actions.cc:471-472` is the proven
  instance. (`_pick_death`/`_check_death` are deterministic — art + damage +
  settings, `actions.cc:291-388`. The client-ticker RNG at `animation.cc:2595`
  (`_object_fidget`) never runs server-side.)
- **Nested / concurrent sequences.** `actionExplode` is not one bracket: the main
  sequence (:1841-1862) is followed by `_show_damage_extras`, which opens a NEW
  `reg_anim_begin/end` **per ring victim** (:625-631). The record section therefore
  spans multiple brackets and the stream must carry them as separate `SEQ_BEGIN/END`
  groups in order (§6.5). The engine itself never nests brackets (a second begin
  while one is open is an error in animation.cc); the recorder should assert
  non-nesting.
- **`_action_in_explode` latch.** The animate branch sets it (:1839) and only
  `_finished_explosion` (DROPped) clears it (:2105). The record section must clear
  it on exit or the next explosion returns -2 (:1757).
- **Violence/gore preference gets baked server-side.** `_pick_death`/`_check_death`
  read `settings.preferences.violence_level` (:315/:372) — recording on the server
  bakes the *server's* setting into every client's gibs. Today's client-side
  replays honor each viewer's own setting. Accepted cost (server-authoritative
  cosmetics), but say it out loud; a per-client re-filter is possible later (client
  downgades an anim it won't show) — do not build in v1.
- **Event size ceiling.** Wire events carry a u16 length
  (`[u8 type][u8 flags][u16 len]`, COMBAT_CLIENT_DESIGN §3.c recon). A recorded
  explosion is a few hundred bytes (§6.5); a pathological modded cutscene could
  approach 64KB. v1: assert + truncate-with-log (presentation is droppable by
  contract, MP_PROTOCOL §1); chunking is a later, mechanical extension.
- **Transient churn** stays net-zero per beat (today's behavior, seam doc worked
  example) — but the recorder must never hand a same-beat netId to the wire (§6.2
  rule), because the client destroys that object the same frame it decodes it.

---

## 6. The command stream

A **tagged flat command stream** — not a VM. No branching, no registers, no state
machine beyond "next op." The client interpreter is a `switch` in a loop feeding the
real engine (§7). This is deliberately the smallest thing that carries ordering.

### 6.1 Op set (u8 opcode, LE args, append-only within the stream's own versioned tag space)

```
SEQ_BEGIN   {u8 flags}                      reg_anim_begin's requestOptions
SEQ_END     {}
PRIORITY    {u8 n}                          _register_priority
OBJ_CREATE  {i8 handle, i32 fid, i32 tile, i32 elev, u32 flags}   lazy transient mint (§4.4)
ANIMATE     {ref, u8 anim, i8 delay}
ANIMATE_REV {ref, u8 anim, i8 delay}
ANIMATE_FOREVER {ref, u8 anim, i8 delay}
ANIMATE_AND_HIDE {ref, u8 anim, i8 delay}
HIDE_FORCED {ref}
SET_FID     {ref, i32 fid, i8 delay}
ROTATE      {ref, i32 tile}
UNSET_FLAG  {ref, u32 flag, i8 delay}
MOVE_STRAIGHT      {ref, i32 tile, i32 elev, u8 anim, i8 delay}
MOVE_STRAIGHT_WAIT {ref, i32 tile, i32 elev, u8 anim, i8 delay}
SFX         {ref, str name, i8 delay}       length-prefixed ASCII sfx name
SET_LIGHT   {ref, i32 dist, i32 intensity, i8 delay}
TAKE_OUT    {ref, u8 weaponAnimCode, i8 delay}
PING        {u8 flags, i8 delay}
SYNC_MOVE   {ref}                           stage 2: wait for ref's glide queue to drain
CALL        {u8 tag, ref, i32 arg}          defunctionalized UI callback (tag enum §4.2)
```

`delay` is the engine's own register-arg (−1/0/N frames) carried verbatim — **the
stream does not invent timing**; the client's `animation.cc` computes real durations
from art FPS exactly as SP does. That is what kills the "ballparked timings"
complaint by construction.

### 6.2 Object references

`ref` is an `i32`: **positive = netId** (persistent object; resolved via the
decoder's existing netId map at *play* time, so a victim freed since decode drops
its ops — the `playExplosion` rule, `client_net.cc:1691-1694`); **negative =
stream-scoped handle** minted by `OBJ_CREATE`; **0 = null**. Recorder-side rule:
an `Object*` created since the current beat began (tracked from the `objectCreated`
hook) MUST go out as a handle with a lazy `OBJ_CREATE`, never as its netId — its
same-beat SPAWN/DESTROY pair means the netId is dead by pump time on the client.
Everything else goes out as netId. The interpreter owns handle lifetime: created on
`OBJ_CREATE` (hidden, `OBJECT_NO_SAVE`, never entering the decoder's netId
domain — the projectile precedent, COMBAT_CLIENT_DESIGN §3.c), destroyed by the
engine's own `ANIMATE_AND_HIDE`/`HIDE_FORCED` completion, and **backstopped** by a
teardown free of leftover handles when the stream's cap timer fires (§7.3) —
transients must never outlive their stream.

### 6.3 The wire event

**`EVENT_PRES_SEQ` (id 31)** — next append-only id after `EVENT_EXPLOSION_FX = 30`
(`presenter_network.cc:116`); skip-unknown-T keeps old viewers safe (they lose the
animation, keep the state — presentation is droppable by contract). Payload:
`u8 streamVersion, u16 opCount, ops…`. PRESENTATION-flagged (flags bit 0 clear,
`presenter_network.cc:120-124`). Emitted at record-section exit, **before** the
fast-path state work of the same beat where ordering matters (the explosion already
emits its cue before `_report_explosion` for exactly this reason,
`actions.cc:1887-1891` comment). The IDEAS.md §A2 reserved mod-message envelope
must stay clear of 31 — same rule as ids 30-32 in the seam doc.

### 6.4 Callback tags

A closed `u8` enum from Table B; v1 ships `SHOW_DEATH` (and `HIDE_PROJ` only if the
`HIDE_FORCED` folding proves wrong). The client dispatcher maps tag → the real
engine function with the marshalled args (`CALL{SHOW_DEATH, ref, anim}` →
`animationRegisterCallbackForced(obj, (void*)anim, _show_death, -1)` on the REAL
engine — i.e. even callbacks replay through vanilla registration, preserving their
position in the sequence). Tags are append-only like event ids.

### 6.5 Worked example — what `actionExplode`'s animate branch records

Running :1841-1877 under the recorder, with a live defender D (netId 17, dies) and
one ring victim V (netId 23, knocked down), yields (annotations = source):

```
SEQ_BEGIN   {RESERVED}                        ; :1841
PRIORITY    {1}                               ; :1842  (via softened stub)
OBJ_CREATE  {-1, MISC/10, T, E, HIDDEN|NO_SAVE} ; lazy: first ref of `explosion` (:1766-1776)
SFX         {-1, "whn1xxx1", 0}               ; :1843
UNSET_FLAG  {-1, OBJECT_HIDDEN, 0}            ; :1844
ANIMATE_AND_HIDE {-1, ANIM_STAND, 0}          ; :1845
PING        {RESERVED, 1}                     ; _show_damage :643 (one per critter extra)
SFX         {17, "<die-sfx>", 0}              ; _show_damage_to_object :429-430
ANIMATE     {17, ANIM_FALL_BACK…, 0}          ; :433 (or MOVE_STRAIGHT_WAIT via actionKnockdown :168 — recorded with knockback tile; client plays the slide, authority's MOVE already landed: see §7.3 knockback note)
CALL        {SHOW_DEATH, 17, anim}            ; :574
OBJ_CREATE  {-2…-7, MISC/10, ring tiles, E, HIDDEN|NO_SAVE}   ; lazy (:1778-1796)
UNSET_FLAG  {-2…-7, OBJECT_HIDDEN, 0}         ; :1849
ANIMATE_AND_HIDE {-2…-7, ANIM_STAND, 0}       ; :1850
                                              ; :1853 _combat_explode_scenery → DROP (state, fast-path :1944)
HIDE_FORCED {-1}                              ; :1854
HIDE_FORCED {-2…-7}                           ; :1856-1858
                                              ; :1860 _report_explosion → DROP (fast-path :1923)
                                              ; :1861 _finished_explosion → DROP (§5.4 latch note)
SEQ_END     {}                                ; :1862
SEQ_BEGIN   {RESERVED}                        ; _show_damage_extras :625 (per ring victim)
PRIORITY    {1}                               ; :626
SFX         {23, "<knockdown-sfx>", 0}        ; via _show_damage_to_object :510-511
ANIMATE     {23, ANIM_FALL_…, 0}              ; :516 (or knockdown slide)
ANIMATE     {23, ANIM_…_TO_STANDING, -1}      ; :528-530 (survivor gets up)
SEQ_END     {}                                ; :630
```

~30 ops, ≈ 300-400 bytes. Compare: today this same behavior costs a bespoke event
(`EVENT_EXPLOSION_FX` emit + payload code, `presenter_network.cc:524`), a bespoke
decode + `Attack` reconstruction (`client_net.cc:1622-1702`), and a hand-maintained
mirror of the branch (`actionExplodeReplay`, `actions.cc:1968-2024`) — all deleted.

**Knockback in the stream — the one deviation from verbatim capture:** the recorded
`MOVE_STRAIGHT_WAIT` from `actionKnockdown` would glide the victim to a tile the
authoritative MOVE already carried (server applies knockback via
`_combat_apply_knockback`, :1921). v1 keeps today's rule — knockbacks zeroed, slide
snaps via MOVE (`actions.cc:1963-1965`) — by having the recorder *skip* MOVE_STRAIGHT
ops whose object the fast-path displaced this beat, OR (simpler, v1) by recording
with the same zeroed-knockback input the replay uses today. This is the already-
banked smooth-knockback gap, unchanged in scope; the stream does not solve it and
must not fight it.

---

## 7. The client interpreter

### 7.1 One function

`presPlayRecordedSeq(stream)` (earlier working name
`unpackPlayBucketSequence`): loop ops; maintain the handle table
(`OBJ_CREATE` → `objectCreateWithFidPid` + `objectHide` + `OBJECT_NO_SAVE` +
`objectSetLocation` — exactly the setup `actionExplodeReplay` hand-writes at
:1970-1991); resolve positive refs through the decoder's netId map at play time;
`SEQ_BEGIN/END` → real `reg_anim_begin/end`; each op → the corresponding **real**
`animationRegister*` on the viewer's linked `animation.cc`; `CALL` → tag table.
Ticked by the existing `presAdvance()` → `_object_animate()` driver
(`client_present.cc:847`, `:625` comment) — the same driver that plays door slides
and attack replays today. Because it feeds the vanilla engine with the vanilla
registration args, timing, action frames, ping staggering, and art-FPS pacing are
vanilla by construction — the interpreter contains **no timing logic at all**.

### 7.2 Queue integration

A new `PresKind::kRecordedSeq` in the presentation queue (`client_net.cc:260-282`),
carrying the raw op buffer + the participant netId set (extracted at decode by a
single pass over the ops). It obeys the existing pump discipline
(`presentationPump`, `client_net.cc:397`): plays in wire order; waits while its
participants are playable-gliding (the `kAttack` rule); `kTurnStart`/`kExit` wait
on it. Out of combat it plays immediately unless a door/glide hold applies — the
`onExplosionFx` in/out-of-combat fork (:1661-1670) generalizes verbatim.

### 7.3 Authority-leads-presentation (#6/#9) under multi-step streams

The known gap magnifies exactly as the seam doc predicted: the state deltas of the
whole beat land at decode, the stream plays over seconds. The existing machinery is
the answer and already generalizes:

- **Reserve at decode:** every participant netId in the stream gets
  `clientCombatAnimReserve` at decode time (the same-beat-leak guard the explosion
  already uses, `client_net.cc:1653-1660`), holding fid/flags/rotation deltas until
  the stream completes. hp/ap numbers never wait on pixels (standing doctrine).
- **Tripwires force release:** a new stream or ATTACK_RESULT touching the same
  object, a MOVE on a participant, map transition/rebaseline — all existing
  (`client_present.cc` REPLAY concern, `COMBAT_CLIENT_DESIGN §3.c`).
- **Caps:** per-stream wall-clock cap scaled by op count,
  `min(kReplayCapMs × seqCount, ~6s)` — a multi-bracket stream legitimately runs
  longer than one attack; unbounded holds are the #6/#9 failure mode, so the cap is
  load-bearing, and cap expiry also frees leftover transient handles (§6.2).
- **Present-tile rule** (the FSM #6/#9 fix): a participant mid-glide renders in its
  presented-tile bucket; the stream's anims play on the presented object — already
  how attack replays interact with glides.

What this does NOT solve (unchanged, inherited): a very long recorded cutscene will
finish against a world whose state has visibly moved on. That is the fundamental
authority-leads-presentation trade the architecture accepts (state cannot be lossy,
presentation can), and the caps bound it.

---

## 8. What it deletes, what it keeps, and the migration path

### Deletes (eventually, each step live-verified — there is NO headless oracle for presentation)

| Step | Deletes | Replaced by |
|---|---|---|
| 1. **Explosion probe** (§11) | `actionExplodeReplay` (`actions.cc:1968-2024`); `playExplosion`/`onExplosionFx` decode (`client_net.cc:1622-1702`); `EVENT_EXPLOSION_FX` emission (id 30 retired, never reused) | record section at :1838 + `EVENT_PRES_SEQ` + interpreter |
| 2. **actionDamage** | the `whc1xxx1`/hit-anim cue gap (never built — this step *adds* coverage rather than deleting) | same mechanism, second proof of generality |
| 3. **Door slide** | `clientDoorAnimPlay` + `EVENT_DOOR_STATE` replay body (id 28 retired); `doorState` presenter emit | recorded `_check_door_state`-driven sequence (needs the door fast-path to run its animate path under record — same shape as step 1) |
| 4. **Attack replay** | the `Attack` reconstruction + `_action_attack` local call (`client_combat_anim`), `EVENT_ATTACK_RESULT`'s replay half (the event itself may stay as combat telemetry), the takeOut/actionAnim bespoke cues (ids 27/29 retired) | record section around `_action_attack` in the combat pipeline. **Last** because it is the most tuned (pacing, throws, AI-chatter guards) and currently works |
| 5. **client_present sequencer bodies** | the per-concept replay builders (attack-replay arm, door arm, gesture arm) | the one interpreter |

### Keeps (the honest correction to "drop the whole FSM")

- **The glide machinery** (`client_present.cc` GLIDE concern) — movement is
  STATE-lane (MOVE events with durMs); it is not reg_anim on the wire and never
  becomes a recorded stream. Walk pacing stays exactly as built.
- **The presentation pump/queue** (`client_net.cc:397`) — cross-event ordering
  (turn starts vs glides vs streams) is a wire-arrival-order problem the stream
  does not remove; `kRecordedSeq` is one more customer.
- **Reserve/tripwire/present-tile reconcile** — the #6/#9 substrate is *more*
  needed, not less (§7.3).
- **Modal chrome** (dialog/movie/worldmap/endgame) — separate intent-driver track,
  explicitly out of scope here.
- **Direct presenter one-shot cues** (console/float/sfx/fade) — §3 boundary.

Every step is independently shippable and reversible: the old event ids keep
decoding until their step lands (retire = stop emitting; the decode arm lingers one
release for stragglers), and each step's server change is a per-site record bracket
that can be reverted without touching the others. The interpreter grows op coverage
monotonically.

---

## 9. Honest costs / open risks / non-coverage

### 9.1 Golden re-bless, quantified

- **Bring-up (env-gated `F2_SERVER_PRES_RECORD`):** zero re-bless. Gate off =
  today's bytes everywhere (the S1 explosion cue precedent: base presenter no-op,
  ALL GATES PASS).
- **Default-on flip:** wire-capturing gates gain `EVENT_PRES_SEQ` events →
  **wire-stream goldens re-bless once**. The **state dumps and
  `randomStateFingerprint` must NOT change** — they are the purity oracle for
  record mode, and any diff there is a bug (a state leak from the section), not a
  re-bless. This split is the safety property: re-bless is confined to the channel
  that is *supposed* to grow.
- **Stage 3 (callback-EXEC):** genuine sim change (dropped outcomes start firing) →
  full re-bless + adversarial review + its own design pass. Do not smuggle it in
  with streaming.

### 9.2 Non-coverage (confirmed)

- **`op_wait`/game-time-paced script choreography** (synchronous opcodes + sleeps)
  bypasses reg_anim entirely; state carries via deltas, pacing snaps. Confirmed
  acceptable-to-snap; there is no registry to capture (seam doc §C honesty,
  re-verified — the opcode map shows the sequencing lives in the script VM's wait,
  not in any presentation structure).
- **Camera/pan:** there is no scripted camera primitive; scripts recentre the view
  via `tileSetCenter` inside a few opcode handlers
  (`interpreter_extra.cc:513/552/829`). On the viewer the camera is client-local
  today. Out of scope; nothing to record.
- **Particles:** none exist. `grep -ri particle src/` → zero hits; every "effect"
  is FRM art on a (transient) object — the explosion cloud IS 7 art objects
  (:1766-1796). The riff's uncertainty is resolved: no particle system to carry.
- **Ambient/idle client cosmetics** (`_object_fidget`, `animation.cc:2595`) are
  client-ticker-local and correctly stay off the wire.

### 9.3 Cost ledger (build items)

| Item | Size | Notes |
|---|---|---|
| Recorder in `server_anim.cc` (+1 small TU for the buffer/section) | M | leaves gain record heads; section enter/exit; RNG snapshot/restore; created-this-beat set |
| Fast-path gate relax, per migrated site | S ×N | 1 site for the probe (:1838); :2235 next; combat later |
| Abort-stub sweep | S | run-and-let-aborts-name-it; `_register_priority` proven, expect a handful more |
| Callback tag table + `ui_only` split of `_show_death` | S-M | 2 tags in v1; the itemDropAll guard; arg marshalling per §4.2 |
| Throw-branch record guard | S | `actions.cc:879-895`; blocks attack migration, not the probe |
| Wire event + encoder | S | append-only id 31; u16 ceiling assert |
| Client interpreter + handle table + `kRecordedSeq` | M | one file; reuses reserve/pump/advance machinery wholesale |
| Live-verify ×4 (explosion/damage/door/attack) | M | the gate for each deletion; no headless oracle |
| Re-bless (default flip) | S | wire goldens only, once |

### 9.4 Residual risks I could not fully verify

- Whether **`_show_death`'s `itemDropAll`** double-applies on the viewer today
  (§4.3 flag) — needs a targeted live check, not an assertion.
- Whether **`hideProjectile`'s action-frame timing** survives the `HIDE_FORCED`
  folding (§4.2) — if not, it costs one more tag. Trivial either way.
- The **full inventory of register-time state mutations** inside animate branches —
  the throw branch is proven; the sweep must find the rest before the attack step
  (not before the probe).
- **`ANIM_KIND_PING` runtime semantics** — delegated to the client's real engine,
  deliberately unstudied here (`animation.cc:1495`).

---

## 10. The decision — three options on a shared baseline

The framing that actually decides this: **the leaf + callback classification work
(§4) is common to every option.** Any path to "the viewer plays the engine's own
animations" — including hand-writing more replicas — must (a) know which leaves are
UI vs state, (b) enumerate and classify the callback targets, (c) split
`_show_death`, (d) guard the throw branch, (e) marshal object references. That work
is unavoidable, grounded, and adds no new engine behavior. The choice is only about
what sits ON TOP of it.

### Option 1 — per-action replicas (the `actionExplodeReplay` road)

One hand-written replay per action, each mirroring its animate branch minus state.
On top of the shared baseline you pay: **N replica functions maintained in lockstep
with their originals forever** (every CE upstream merge, every Sfall quirk touches
both); **a bespoke wire event + decode + argument reconstruction per action**
(the explosion needed a custom payload, `Attack` rebuild, attacker synthesis);
**ballparked or hand-transcribed timing** wherever the replica diverges from the
original's registration pattern; and **zero bucket C** — a raw script cutscene has
no action function to replicate, so this road structurally cannot cover mods/scripts
(the owner's original complaint, unfixable in this option).

### Option 2 — a full presentation VM (explicitly de-scoped)

Branching bytecode, an execution environment, its own scheduler. Nobody is
proposing this and nothing in the requirement needs it: the sequences are straight-
line (reg_anim has no control flow — loops/conditions live in the composite, which
already ran on the server). **De-scoped permanently**; mentioned only so the thin
stream is not confused with it. If someone finds themselves adding a jump op, stop.

### Option 3 — mode-flagged leaf wrappers + a thin flat command stream (RECOMMENDED, gated on bucket C)

The leaves themselves are the "interpreter": server-side, a wrapped leaf **appends
`{TAG, args}`**; client-side, a `switch(tag)` calls the **same real leaf**. The
"bytecode" is nothing but the leaf/callback enum the shared baseline already forced
you to write down. Marginal cost over option 1: the command enum, an `append()` per
wrapped leaf, the handle table, one wire event, one client switch — call it days,
not weeks, on top of the shared work. Marginal benefit: **zero replica functions**
(composites record themselves by running — every one of option 1's N functions and
its maintenance tail evaporates), **exact captured sequence and timing** (the
register args ARE the timing; nothing is ballparked ever again), and **bucket C
falls out of the same seam** (a modded script's reg_anim cutscene records exactly
like `actionExplode` does).

### The one question that drives it: do you need bucket C?

Evidence for calibration: the script opcode surface has a **full reg_anim authoring
family** — `op_reg_anim_func` (begin/clear/end), `op_reg_anim_animate/-reverse/
-forever`, `op_reg_anim_play_sfx`, four reg_anim movers, plus `op_anim` /
`animate_stand(-reverse)` / `animate_move_obj_to_tile` (SCRIPT_OPCODE_MAP.md rows
0x80CC-CE, 0x810C-0x8114, 0x8126, 0x813B) — ~12 opcodes whose entire purpose is
script-driven choreography, all classified QUIRK (arbitrary content-authored
sequences). So bucket C is real and script-reachable. **What I cannot quantify from
this repo: how many of the game's ~1400 shipped scripts actually use them** (the
.int sources are not in the tree; classic-game experience says "cutscene-ish
moments and ambient staging — a minority of scripts, but present in memorable
places"). Two facts ARE clear from the docs of record: the **Temple demo does not
depend on bucket C** (its blockers are dialog streaming and item use —
TEMPLE_DEMO_ROADMAP / p5 memory), and the **modding-platform vision (IDEAS.md)
makes bucket C load-bearing** — "100% support of any possible script/mod by
construction" is bucket C by definition.

**Recommendation:** If the project's endpoint were "ship the engine's own action
set, no mods," option 1 + the remaining semantic envelopes would be defensible —
smaller, already half-built. But the stated direction (mod platform, arbitrary
scripts, no per-action hand-picking ever again) makes bucket C a requirement, and
then option 3 is the only road that reaches it — at a small marginal cost over work
you must do anyway, minus N replicas you never write. **Build option 3, staged as
§11, after Demo v1.** The reframe to hold: it is not "simple replays vs a
serializer" — it is **"N ballparked replicas forever" vs "one thin recorder," on
top of identical shared leaf work.**

---

## 11. The probe (first slice, post-demo — the explosion, concretized)

Make the explosion generic end to end: record section at `actions.cc:1838` (env-
gated), leaves recording per Table A, DROPs per Table B, `SHOW_DEATH` tag,
`EVENT_PRES_SEQ`, interpreter + `kRecordedSeq`. **Success =** same observable result
as `actionExplodeReplay` (cloud + boom + gibs, in-combat queueing, victim reserve)
with the bespoke path deleted; state dumps + `randomStateFingerprint` byte-identical
with the gate on; live-verify per `[[visual-verification-protocol]]` (C4 a Temple
door; watch cloud → gibs → door DESTROY; check no stuck sprites, no double ground
loot from `_show_death`, no flicker). It exercises every mechanism this spec adds —
leaf recording, lazy transients, RNG section, callback tags, the interpreter — in
one bounded slice. If the probe fights back anywhere, the fight itself names which
section of this spec was wrong; fix the spec before widening.

---

## Appendix A — Design rationale & lineage

*Folded in from the two precursor docs (the seam analysis and the owner's design riff)
when the cluster was consolidated. The spec body above is the mechanism; this appendix is
the **why** — the load-bearing principle, the closure argument, the project law, and the
prior-art vocabulary — kept because they outlive any one implementation.*

### A.1 The load-bearing principle: state SNAPS, presentation ANIMATES

Two channels, separate on purpose — this is the whole architecture, and it answers "do we
deduce animation from state, or from a separate channel?": **a separate channel.**

- **STATE channel** (`OBJECT_DELTA` + spawn/move/destroy) = *what is true* (HP=0, this
  critter is `DAM_DEAD`, its fid is the corpse, the door is gone). Applied verbatim. But it
  is the **end snapshot** — it never says *how* the transition looked. Drive animation from
  state alone and everything **snaps** (critter pops to corpse, no death throes).
- **PRESENTATION channel** (the recorded seq; historically `EVENT_ATTACK_RESULT`,
  `EVENT_DOOR_STATE`, `EVENT_ACTION_ANIM`, `EVENT_EXPLOSION_FX`) = *how to animate the
  transition*, replayed with the real handlers.

Why split: **state cannot be lossy** (drop a death → critter wrongly alive = desync);
**presentation can be lossy** (drop a death anim → critter still correctly dead, just
snapped). Different rails, different guarantees. We do NOT *infer* animation by watching
deltas ("a critter near the blast just died → guess a death anim") — that is the fragile
guessing we reject (wrong anim, wrong timing, knockback-direction from a teleport). The
server ran the real action and *knows*, so it records an explicit stream; the viewer replays it.

### A.2 The three buckets (the closure argument)

What do we stream so we are not hand-wiring every script's expectations? Not actions, not
opcodes, not script calls — the layer *below* all of them, which splits into three
categories with very different costs:

**A) STATE — free by construction.** A script cannot mutate the world except through the
engine object API; the per-beat shadow-diff (`src/object_delta.cc`) + the lifecycle hooks
(`objectCreated/Moved/Destroyed/Connected/Disconnected`) observe the **result** of any
mutation regardless of which of 100 scripts caused it. Toxic-waste damage = an HP delta.
Teleport = a position delta. Destroy = `EVENT_DESTROY`. Falling through a floor is mostly
here (`EVENT_MOVE` carries `from/toElevation`, `presenter_network.cc:250`). Closure proof =
`SCRIPT_OPCODE_MAP.md` (all 181 game + 78 core-VM opcodes classified; the pure-sim ones are
covered with zero per-script work).

**B) ONE-SHOT CUES — one bounded seam.** Scripts never touch a pixel/speaker directly;
every presentation-capable opcode bottoms out in a few primitive families: (1) direct
`presenter()` calls already routed (`op_display_msg→consoleMessage`, `op_float_msg→floatText`,
`op_play_sfx→sfxPlay`, `op_gfade→screenFadeOut/In`); (2) the `animationRegister*` family — the
seam, ONE file (`src/server_anim.cc`), where the primitives were silent no-ops; recording
them covers `op_anim`, the `op_reg_anim_*` family, and **every engine-internal caller**; (3)
semantic envelopes (attack/door/gesture). **The seam altitude is the primitive layer, NOT the
opcode/action/script layer** — the opcode audit is the *proof of closure*, not the seam.

**C) ORDERED SCRIPTED SEQUENCES — the real hole.** "An NPC walks over, kills a guy, turns,
speaks." Ordered, timed, state+presentation braided. The reg_anim serializer (this spec) is
the capture point. Two honest gaps remain even so: (i) **non-reg_anim choreography** —
scripts that sequence via direct synchronous opcodes (`op_kill`/`op_move_to`/`op_destroy`)
gated by `op_wait`/game-time bypass reg_anim entirely; their state carries via deltas but
their *pacing* has no registry to capture (a smaller class, likely acceptable to snap). (ii)
**authority-leads-presentation reconciliation** — a multi-step manifest inherits the gap that
bites combat (#6/#9 in `p5-server-plan`): the state delta lands before the viewer finishes
replaying, so a reaction can play at an already-moved tile; a general sequence channel
magnifies it and needs the present-tile rules the combat FSM already uses.

### A.3 The fast-path law (project law to adopt)

> A `serverLoopActive()` fast-path MUST emit / record the presentation-cue equivalents of
> the reg_anim block it replaces.

The decoupled fast-paths (`actionExplode`, `actionDamage`, use-skill, attack, pickup,
use-door) **skip the reg_anim block entirely**, so their cues never reach the primitives.
That set is closed (~21 core-file sites, each a deliberate decouple with a comment); the
migration's job is to make each record its sequence instead of dropping it.

### A.4 STATE gaps (sim-correctness, banked — none is presentation)

1. `animationRegisterMoveToTileStraight` no-op (`server_anim.cc:742`) — the straight-line
   forced move used by knockback slides, stairs, and **scripted falls**. Close with a real
   `objectSetLocation`, not a cue. (This is the real hole behind "falling through a floor.")
2. Per-tile spatial procs skipped during stepped walks (`server_anim.cc:86`) — sim
   divergence; close before free-roam (the Temple traps ride this).
3. `animationRegisterAnimateAndHide` / `HideObjectForced` no-ops — "faithful = objectDestroy";
   matters only if a script-reachable removal relies on it.
4. `objectSetLight` not in the delta mask — minor/visual, later.

### A.5 Prior-art vocabulary (why this shape is not novel — the good news)

Three well-worn techniques compose into exactly this design, so we can search for prior art:

1. **Defunctionalization** (Reynolds, 1972) — the callback half. You can't serialize a C
   function pointer. Replace each function-value with a *data tag + captured args* and write
   ONE `apply(tag, args)` dispatcher on the client: `animationRegisterCallback(&_show_death)`
   → `{CALL, SHOW_DEATH, netId, anim}`. (This is what every RPC is: method-id + serialized
   args = a defunctionalized call.) → spec §4.2, §6.4.
2. **Command buffer / display list / IR** — the sequence half. Model `reg_anim` as a list of
   typed ops, serializable by construction. A tagged command stream is enough; do NOT reach
   for a full VM (over-build). → spec §6.
3. **Execution mode / two-phase (predict vs authoritative)** — the state/presentation split.
   One function, a flag for "apply state" vs "just present." → spec §5.

The owner's one-liner that makes it cheap: **"instrument the leaves; the composite dumps
itself."** You do not model each composite by hand — classify the closed leaf set once, then
run the real unmodified composite and out falls the command stream. Generalized, the record
critical section is **authoritative-state-inert mode = a dry run / shadow execution**: RNG
frozen + state callbacks dropped + register-leaves record-instead-of-do, so the pass is
observationally pure w.r.t. the sim (spec §5).
