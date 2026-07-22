---
name: modal-drivers-plan
description: "Modal-driver track — all headless drivers done (dialog/rest/endgame/worldmap/barter). Kept for the reusable driver PATTERN, the remaining low-priority UI-glue screens, and the banked fidelity follow-ups."
metadata: 
  node_type: memory
  type: project
  originSessionId: 4b024979-e172-4895-ab95-a4e2031a3fe6
  modified: 2026-07-19T05:10:00.253Z
---

All modal drivers are done: DIALOG ([[dialog-headless-plan]]), REST, ENDGAME, WORLDMAP travel,
BARTER (decoupled the second nested blocking UI, inventoryOpenTrade). Git holds the details.

## ►► THE PROVEN PATTERN (reusable for any remaining modal)
A `serverLoopActive()` branch inside the blocking modal loop that DRAINS an intent queue /
auto-resolves instead of reading input, with window/render skipped; new probe verb(s) in
main.cc; a server golden in run_golden_server.sh + tests/golden/server/*.golden.txt; TWO commits
(mechanical=legacy byte-identical / semantic=golden re-bless with sign-off); MANDATORY
adversarial equivalence review before the bless (scripts run arbitrary sim —
[[anim-decouple-verification]]). Gate = scripts/check.sh (legacy byte-identical + server
all-pass). Fan out ≤3 background recons first ([[working-style-background-and-context]]).
GATE NOTE: rest gates on the BROAD `headlessProbeActive()` (both probe modes skip the pipboy
window), NOT serverLoopActive(); that flag is the reusable "any headless probe path" gate.

## ►► REMAINING (low priority)
The 8 inventory/skilldex/char-editor UI-glue screens: the SIM is already pure + golden, so a
driver only exercises the UI, not the sim. Low value — pick up only if a screen becomes needed.

## ►► BANKED FIDELITY FOLLOW-UPS (not bugs today, each its own signed-off task)
- **serverRun does not honor the terminal quit**: adding `if (_game_user_wants_to_quit != 0)
  break;` after serverTick in serverRun (server_loop.cc) is a real fidelity win (the interactive
  game exits its while(quit==0) loop there), BUT it changes 5 goldens (endgame + the 4
  combat-death cases ALL set quit on the server path when the dude dies) → a SEPARATE semantic
  re-bless. Deferred (user had no opinion, kept minimal). Revisit if server fidelity past
  terminal points ever matters.
- **freeplay-on-endgame** (in IDEAS.md): optional non-faithful mode taking the "keep playing"
  branch (skip quit=2) — natural persistent-world choice for the headless server (teleport party
  to a post-game sandbox, new-game+). Future feature.
- **simClockReset() does not clear process statics** (wmLastRndTime, _last_light_time/_last_time__,
  gGameTimeIncRemainder): a SECOND serverRun in one process would start gSimNow=0 vs big statics →
  getTicksBetween returns INT_MAX → heal/encounter/light gates fire every step in run 2. Inert
  today (one serverRun per process). If a future real driver runs multiple travels per process,
  reset these in simClockReset or a serverRun teardown.

See [[f2-rewrite-project]], [[dialog-headless-plan]] as pattern references.
</content>
