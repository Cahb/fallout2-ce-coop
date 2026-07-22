---
name: p5-cut-list
description: "P5 de-stub reference-chain cut-list — the RULE (restructure, not #ifdef), the REMAINING heads (H6 mapLoad chrome + the presenter/interface tail), and the latent interfaceGetCurrentHitMode server crash. H1–H5 are done (git)."
metadata: 
  node_type: memory
  type: project
  originSessionId: 742d179b-7cbd-4a2a-9211-7e59585528f7
  modified: 2026-07-19T05:11:37.764Z
---

De-stub cut-list for [[p5-server-plan]] Step 1 (clear f2_core→f2_client stub references). H1–H5
are done (art→core, boot relocation, combat_drain, scriptsHandleRequests seam, animation backend
shim); git has the details.

## ►► THE RULE
A stub exists because an f2_core `.cc` NAMES the symbol at LINK time. The ONLY way to delete it
is to RESTRUCTURE the reference out (relocate the fn to a client/seam TU, or split the TU). NO
#ifdef, NO in-place guards — a serverLoopActive() branch does NOT remove the symbol reference.
Only client→core moves are legal. LESSONS: (1) when a scoped "N-fn facade" head's closure drags
most of an SDL-free TU, move the WHOLE TU + split the 1–2 presentation fns (cleaner than a
facade — H4/sfx_name). (2) Re-verify every "clears N stubs" recon count per-symbol with
word-boundary exact-call grep (skip comments/substrings) BEFORE trusting it; the linker is final
proof. (3) Run the ALL-155-MAP sweep `scripts/srv_sweep.sh` FIRST — it names real REACHED stubs
instead of static guesses.

## ►► KEY CORRECTION (still live): the animation family is NOT "already guarded". combat_ai.cc,
interpreter_extra.cc opcodes, scriptsHandleRequests, mapHandleTransition, and combat up-stairs are
UNGUARDED and on the per-beat serverTick path. Guard-and-skip would SILENTLY DROP combat-AI
movement + elevator/transition — the anim backend seam is REQUIRED (and is what makes those sites
correct headless via synchronous apply), not merely non-crashing.

## ►► H6 — mapLoad CHROME → presenter seam + isoEnable/isoDisable SIM/CHROME SPLIT (~17 syms)
Server DOES reach mapLoad at runtime (server_loop.cc mapHandleTransition→mapLoadById→mapLoad) so
mapLoad/mapSetElevation/_map_place_dude_and_mouse/isoEnable/isoDisable ABORT LIVE today.
_map_init/_map_exit are client-only (link-forced). Clears 17 syms via 7 NEW presenter virtuals
(worldEnable/Disable/Clear, mapAmbientLoad, hudBarShow, sfxQueueStart, mouseResetBouncingCursor) +
reuse of existing virtuals. Getters (gameMouseGetCursor/…IsVisible/gameUiIsDisabled) can't be
presenter (return state to sim) → fold the read/bookkeeping into ClientPresenter overrides.
BLOCKED (H6 removes map.cc's ref but the stub survives because another core TU still names it):
_dude_stand (→ dude/anim head), _dude_fidget (→ combat head), windowShow/windowHide (scripts.cc),
gameUiIsDisabled (interpreter_extra.cc). Split isoEnable/isoDisable along the H-18 sim/chrome seam
(keep gIsoEnabled + _scr_enable/disable_critters core; chrome → map_render.cc).

## ►► PRESENTER/INTERFACE SEAM TAIL (recon done, all 12 reached-on-server)
- ►► **interfaceGetCurrentHitMode is a LATENT SERVER CRASH today** (combat.cc:3467 already assumes
  it returns -1; an abort stub would fire). Fix = flip abort→`return -1` (benign body).
- NEW presenter virtual (deletes stub): gameUiEnable/Disable [12 sites, highest leverage],
  _intface_update_ammo_lights→hudAmmoLights, _dude_fidget→idleAnimEnable/Disable, keyboardReset→
  inputReset.
- RELOCATE client→core: sfxBuildWeaponName family (+ lookup table; must return real value for the
  sfx opcode + PlaySoundEffect — one-way seam can't serve), _dude_standup (clears DAM_KNOCKED_DOWN
  + reg_anim), _dude_stand (sim mutator; sole presentation tail → route via worldInvalidateRect).
- BENIGN BODY (reached, returns state to sim so seam can't carry; flip abort→benign, stub stays
  but safe): interfaceGetItemActions→0, textObjectsGetCount→0, soundContinueAll→no-op.
- STILL OPEN from the earlier tail sweep: gameUiEnable/Disable, _intface_update_ammo_lights,
  _dude_fidget, keyboardReset, soundContinueAll, interfaceGetItemActions.
- BANKED GAP: interfaceGetCurrentHand→HAND_LEFT is savegame state (per-actor sim welded into the
  HUD) → relocate on the hand-swap protocol. Same for gameDialogSetBarterModifier→drop (safe only
  while H5's null handler drops dialog).

## ►► COVERAGE + SEQUENCE
H1–H6 + tail ≈ 90+ of the ~322 stubs. Remaining = deeper presentation subtrees (sound/movie/
pipboy/char-editor/worldmap-ui/dialog render) — later slices; presentation is NEVER de-stubbed to
real, only relocated behind seams or left aborting. server_loop.cc references ZERO stubs; the work
is in what serverTick CALLS. Each head = own commit + full 3-gate ritual + adversarial Opus review
(Class-B behavior, NOT byte-identical). See [[p5-server-plan]].
</content>
