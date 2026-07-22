---
name: frame-index-render-gotcha
description: "objectSetFid does NOT reset obj->frame; a stale frame >= new art's frame count renders NOTHING (invisible). Latent bug class + a usable hide-trick."
metadata: 
  node_type: memory
  type: reference
  originSessionId: 4169d5ed-c9b8-434c-9df1-571a8d0011ba
---

`objectSetFid(obj, fid, rect)` writes ONLY `obj->fid` — it never touches `obj->frame`
(verified object.cc). If `obj->frame >= frameCount(new fid art)` the object renders as
**nothing** (frame index out of the art's frame pool → no draw). Vanilla `critterKill`
guards this by calling `objectSetFrame(critter, 0)` immediately before its `objectSetFid`
(critter.cc).

BIT US as the S5 combat-corpse regression: viewer `resolve()` (client_combat_anim.cc)
applied the held SETTLED fid — the single-frame `ANIM_*_SF` corpse — over the high frame
index left by the just-finished multi-frame fall animation → invisible body. Everyone
(me + user) mis-chased it as reserve/resolve deferral logic, then EVENT_DESTROY, then a
windowed-vs-fullscreen render bug (traces were identical — a frame-timing red herring;
whether the anim happened to end on frame 0 decided if the body showed). Fix = one line:
`objectSetFrame(obj, 0, nullptr)` after `objectSetFid` in `resolve` (settled/SF poses are
always frame 0). See [[p5-server-plan]] S5.

TWO forward implications (user, 2026-07-16):
1. **Latent bug class.** ANY code swapping an object's fid to a shorter-frame art without
   resetting the frame can silently blank it. Whenever you set a settled/SF/stand fid,
   reset the frame too. Watch for this across the rewrite.
2. **Usable dirty-hack.** Frame-out-of-range = a way to make an object NOT render (e.g. hide
   players during a scripted camera pan) IF no cleaner hide mechanism is available. Noted
   as an option of last resort, not a recommendation.
