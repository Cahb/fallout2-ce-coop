---
name: id-plus-cached-pointer-antipattern
description: "De-rot target — the \"serialized id + cached runtime pointer, partial/ad-hoc resolution\" pattern class; whoHitMe/whoHitMeCid exemplar; 64-bit port promotes its latent UB to reliable segfaults."
metadata: 
  node_type: memory
  type: project
  originSessionId: 8662ace8-e34d-49db-a95f-f85efc712417
  modified: 2026-07-18T14:00:01.641Z
---

A recurring vanilla-RE code-rot CLASS to hunt in the eventual de-rot/restructure pass
(IDEAS.md rename+restructure; [[p5-cut-list]]). NOT to chase per-instance now — bank
the pattern, fix the class deliberately.

**The disease:** one fact stored in TWO fields — a serialized ID and a *cached runtime
pointer derived from it* — where the pointer is resynced in several scattered places and
invalidated in ~none. Resolution is ad-hoc and PARTIAL, so the pointer is silently stale
across lifecycle boundaries (load, map change, combat enter/exit, free).

**Exemplar (FIXED 2026-07-18, map.cc):** `CritterCombatData { Object* whoHitMe; int
whoHitMeCid; }`. Save: `cid = whoHitMe->cid`. Load fixup `_map_fix_critter_combat_data`:
`if (cid == -1) ptr = nullptr` — patched ONLY the null case; left the pointer stale when
`cid != -1`. Real resolution (`ptr = _find_cid(cid)`) happens ONLY at `_combat_begin`
(combat.cc:2063-2091). So OUT of combat a loaded critter with `cid != -1` holds a garbage
pointer; `_damage_object` derefs it (combat.cc:5425, guard only checks `!= nullptr`) →
crash. On 32-bit the stale value was a plausible address that usually didn't fault (slept
25 yrs); 64-bit puts a clean 4-byte `-1` in the low word of the 8-byte pointer
(`0x00000000ffffffff`), sails past `!= nullptr`, and segfaults reliably. Fix = null the
runtime pointer UNCONDITIONALLY on load; combat rebuilds it from the id. (Tactical fix
only — the design is still rotten.)

**►► The 2026-07-18 map.cc fix was ITSELF incomplete — re-fixed at objectRead (62e1d87).**
`_map_fix_critter_combat_data` nulls whoHitMe but walks the SPATIAL grid (objectFindFirst/
Next) → only sees critters ON the map. OFF-GRID staged critters (loaded, then a script
snaps them on-grid — e.g. artemple's Temple guards) and `_obj_copy`'d critters escape it.
PROVEN: artemple boot, critter id=24 (guard @tile 21101) loads whoHitMe=0xffffffff; killing
it out-of-combat SIGSEGVs f2_server. Towns were fine only because their critters are on-grid
at load. REAL fix: null whoHitMe at `objectRead` (object.cc ~415) — the single deserialization
chokepoint for map load, savegame, join blob AND `_obj_copy` (which round-trips through
objectRead) — SUBSUMES the spatial map-fix, no load path escapes. LESSON for the whole class:
fix at the DESERIALIZATION chokepoint (objectRead), not at a post-hoc spatial sweep that a
lifecycle boundary (off-grid staging, copy, mid-sim spawn) can dodge. Also note the SAME deref
hazard lives at state_dump.cc:98 (whoHitMe->id) and actions.cc:2449 — all nullptr-only-guarded.

**Same disease, other faces already hit:** raw `Object*`-as-identity across a lifecycle
boundary — netId recycle on rebaseline (`objectAssignAllNetIds`, [[dont-declare-not-a-bug-confidently]]
lesson), `Object*`-keyed presentation registries (the FSM netId-keying fix). Also the
frame-index-stale-after-fid class ([[frame-index-render-gotcha]]) is adjacent (derived
render state not invalidated).

**Correct shape (cleanup pass):** ONE source of truth = the id is canonical; either
resolve to a pointer AT POINT OF USE (a `whoHitMe()` accessor doing `_find_cid`), or use a
generational HANDLE so a stale reference is *detectably* stale, never silently garbage. No
"derived pointer cached in the struct, resynced in 3 places, invalidated in 0."

**►► 64-bit COROLLARY (owner, 2026-07-18):** the 64-bit port will keep LIGHTING UP this
latent-UB class (wider pointers turn "plausible garbage" into clean sentinels / hard
faults). Do NOT retreat to a 32-bit build to mask it — that throws away the rewrite and
only hides symptoms. When a 64-bit crash appears, suspect an id/cached-pointer or
raw-pointer-identity site first. Grep seeds for the pass: `Cid;` fields paired with an
`Object*`, `_find_cid`, `(Object*)` casts from ints, pointer fields written in save/load.
