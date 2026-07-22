# FO2 Dedicated Server — Architecture TL;DR
### One brain, many screens

> The server owns the **game**; each client owns a **view of it**. One rule applied
> everywhere: state and dice never leave the server, pixels and speakers never enter it.
> Rendered version: https://claude.ai/code/artifact/16dc220c-de7e-46a5-b3b9-349fc05fe5b5

## The loop

```
┌───────────────────────────────┐                      ┌───────────────────────────────┐
│        CLIENT ×N (f2_client)  │   intents            │         SERVER (f2_core)      │
│                               │   (what I want)      │                               │
│ · renders world from          │ ───────────────────▶ │ · owns world state, rules,    │
│   mirrored state              │                      │   RNG, game clock             │
│ · owns camera, palette,       │                      │ · validates every intent      │
│   fonts, volume, cursor       │   presenter events   │   (your turn? AP? range?)     │
│ · screens are local:          │   (what happened)    │ · ticks: queue → combat →     │
│   inventory browse, pipboy    │ ◀─────────────────── │   scripts → time              │
│ · input → intent, no rules,   │                      │ · never waits on any client   │
│   zero dice                   │                      │ · no window/camera/speaker    │
└───────────────────────────────┘                      └───────────────────────────────┘
```

Per tick the server drains client intents, advances the sim, and broadcasts the
resulting events. A dead-silent client changes nothing; a spamming client only
fills its own capped queue.

## The two vocabularies

| Intent — client ▶ server | Presenter event — server ▶ client |
|---|---|
| `Move(hex 15450)` | `objectMoved(id, hex, rot)` |
| `Attack(gecko#218, aimed: eyes)` | `floatText(gecko#218, "Ouch!")` · `hudHitPoints()` |
| `UseSkill(FirstAid, self)` | `screenFadeOut()` → `screenFadeIn()` · `consoleMessage(...)` |
| `UseObject(door#42)` | `sfxPlayAt("sodoors", door#42)` |
| `DialogChoice(option 2)` · `EndTurn` | `worldInvalidateRect(...)` · `errorBox(...)` |

`sfxPlayAt` is a **fact broadcast**, not a play command: "this object made this
sound." Each client computes its own loudness from its own camera. Meaning
crosses the wire; mechanism stays home — same reason a fade is
`screenFadeOut()`, not 768 bytes of palette.

## Who knows what

| | Server | Client |
|---|---|---|
| Object positions, HP, inventories, GVARs | **owns** | mirror (read-only) |
| Combat math, skill rolls, AI decisions | **owns** | never |
| RNG stream, game clock, event queue | **owns** | never |
| Camera position, scroll, elevation view | never | **owns** |
| Palette, fades, fonts, sound volume | never | **owns** |
| Mouse cursor objects on the hex grid | zero, by invariant (ledger H-31) | local only |
| Difficulty settings (they change the math) | **authority** (ledger H-35) | requests changes |

## Click-to-walk at 100 ms RTT (v1: no prediction)

| t | what happens |
|---|---|
| 0 ms | client: you click hex 15450. Instant **cosmetic ack** (click sound, target marker). Nothing simulated. |
| 50 ms | server: `Move` intent arrives → validate (walkable? not in combat? path exists?) → sim starts the walk. |
| 100 ms | client: `objectMoved` events arrive; dude starts the walk animation. |
| — | The walk itself takes seconds of animation. A 100 ms late **start** is invisible in this genre; the 1998 engine already had that much input latency. |

Held in reserve, never built unless playtests demand it: optimistic
*movement-only* prediction with smooth path-blend correction — not teleport-back.

## Phase strip

| Phase | Deliverable | Status |
|---|---|---|
| P0 | headless boot · deterministic clock · golden replays (7 cases) | ✓ done |
| P1 | presenter seam → core/client lib split (`f2_core`/`f2_client`, SDL-free core) | ✓ done |
| P2 | pathfinder + movement → f2_core; AnimationScheduler seam | ◐ mechanical parts done; instant-animation activation deferred to the P5 server loop (fake-clock coupling — see REWRITE_PLAN P2 status) |
| P3 | modal loops → intent-driven state machines | |
| P4 | Character & World objects; 2 dudes, one map | |
| P5 | f2_server + thin client · LAN co-op | |

The Presenter interface being built in P1 **is** the wire protocol of P5 —
serializing those calls is the network layer.

Docs of record: `SYSTEM_MAP.md` · `REWRITE_PLAN.md` · `WORKLIST_P1.md` ·
`WORKLIST_P1_LEDGER.md` (45 hidden-rule tickets).
