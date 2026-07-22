---
name: f2-rewrite-project
description: Fallout 2 CE headless-server rewrite — branch, docs of record, the 3-surface coverage model, and the operational env/gate/reverse-eng reference
metadata: 
  node_type: memory
  type: project
  originSessionId: 28d1c122-6c28-4a88-abdb-17a4337257f2
  modified: 2026-07-19T05:08:41.114Z
---

Rewrite of fallout2-ce into dedicated server + thin client. ALL work on branch
`rewrite/phase0` (never main). This memory is the STABLE substrate; the live tracks are
[[p5-server-plan]] (dedicated server + wire viewer) and [[dialog-streaming-track]] (the
current active pivot). Phase-1/Batch-7/Phase-2/server-loop bring-up is long done — git is
the changelog.

## DOCS OF RECORD (repo root)
`REWRITE_PLAN.md` (phases/gates), `SYSTEM_MAP.md` (coupling), `WORKLIST_P1.md` (conversion
checklist), `WORKLIST_P1_LEDGER.md` (hidden-rule tickets), `ARCHITECTURE.md` (client/server
TL;DR). The THREE coverage maps that together bound the whole headless surface:
`SCRIPT_OPCODE_MAP.md` + `PLAYER_ACTION_MAP.md` + `PASSIVE_SIM_MAP.md`.

## THE 3-SURFACE MODEL (durable framing — the sim is driven three ways, don't treat the
opcode map as the whole picture):
1. SCRIPT-driven = the ~181 game opcodes.
2. PLAYER-INPUT-driven = UI→engine funcs with NO script (movement, combat sub-actions,
   inventory, skilldex, char/perks, rest, worldmap, barter/steal/loot, save/load).
3. PASSIVE/autonomous = time/queue-driven, no trigger (rad/poison/drug ticks, HP/limb heal,
   addiction, ambient AI/wander, scheduled queue events, game-time).
KEY: all decouples were placed at the SHARED engine-action layer (_obj_use_door, actionPickUp,
_action_use_an_item_on_object, _combat_apply_attack_results…), so surfaces 1 & 2 CONVERGE there
— each fix covers both the opcode AND the player-input path. Core mechanism = a
`serverLoopActive()` fast-path that calls the same outcome fn(s) directly (mirror of
actionExplode(animate=false)).

## OPERATIONAL (durable)
- Gate = `scripts/check.sh` (one-shot build + golden suites; user-allowlisted — always use it
  over raw cmake/run_golden). Must stay legacy byte-identical; `BLESS=1` re-records only after
  a signed-off semantic change.
- Two-commit rule: mechanical (replay-identical) vs semantic (needs sign-off) NEVER mixed.
- Game assets live in `FO2/` (Steam copy, gitignored). Headless runs need
  `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy F2_FAKE_CLOCK=1 F2_HEADLESS_PROBE=1` from that
  dir. Probe knobs: F2_PROBE_MAP/TICKS/SEED/AGGRO/ACTIONS/DUMP, F2_INPUT_REPLAY.
  `DEBUGACTIVE=screen` for debug output. Playable-into-a-map: ddraw.ini [Misc] StartingMap.
- ►► F2_FAKE_CLOCK is still LOAD-BEARING on the wmtravel server path (getTicks-as-counter);
  fully deleting it = the abandoned S3 clock redesign, belongs in the P5 server loop.

## REVERSE-ENGINEERING REFERENCE
Original `FO2/fallout2.exe` is vanilla US 1.02d — CE's `// 0x4xxxxx` annotations are its
virtual addresses and line up directly. `r2 -q -e bin.relocs.apply=false -c "s 0x<addr>; pd N"
FO2/fallout2.exe` resolves "check in debugger" float/precision TODOs. User offers a Ghidra MCP
hookup if bulk decompilation is ever needed.

See [[model-choice-fable-vs-opus]], [[de-stub-cadence-rule]], [[anim-decouple-verification]].
</content>
</invoke>
