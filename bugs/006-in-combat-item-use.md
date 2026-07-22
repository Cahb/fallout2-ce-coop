# 006 — In-combat item use + actions (stimpacks, medic, doctor, doors, containers)

**Status**: SPEC (2026-07-19)
**Blocks**: Can't use stimpacks, medic/doctor skills, open doors, or loot containers
  while in combat over the wire.

## Current state

Combat in the wire viewer works for: movement (cmove), attacks (cattack → _combat_attack_this),
end-turn (cendturn), end-combat (cendcombat). Everything else that would normally be
accessible via the action menu or hotkeys during vanilla combat is either:

- **Dropped by the server**: the `serverControlLine` handler drops non-combat verbs
  (use/get/skill/loot/push/rot) when `isInCombat()`.
- **Never wired**: the viewer's combat input doesn't route item-use/inventory-open
  actions — only move/attack/end-turn are handled in the combat key dispatch
  (main.cc:1127-1147).

## Why this matters for the Temple demo

The Temple of Trials requires:
- **First Aid / Doctor skills** on self after combat (heal up between ant fights)
- Possibly **stimpack use** as a backup heal
- **Looting containers** — the Temple has lootable urns/crates
- **Doors** work out-of-combat, but what if combat starts mid-open?

## What "in-combat" means on the wire server

The server has `isInCombat()` which gates most verbs. This is intentional:
combat verbs go through the resumable barrier (`cmove`, `cattack`) and consume
AP. Non-combat verbs (`use`, `get`, `skill`, `talk`) don't have AP validation
and could be exploited.

But some verbs are LEGITIMATE in combat and should work:
- `skill <netId> <SKILL_FIRST_AID|SKILL_DOCTOR>` — consumes AP in vanilla
- Inventory open + use-item (stimpack) — consumes AP in vanilla
- `get` (pick up items from ground) — consumes AP in vanilla
- `use` (open doors/containers) — is this allowed in vanilla combat?

Actually: in vanilla, you CAN'T interact with scenery during combat — opening
doors is disabled, looting is disabled. The only in-combat actions are:
attack, move, use-item-on-self (stimpack), use-skill (first aid/doctor), reload,
change weapon/attack mode, end-turn, end-combat, pick up items.

## The gap — what needs new server-side handling

### A. Skill use in combat (First Aid / Doctor)

Currently `skill` verb is dropped when `isInCombat()` (server_control.cc:547-550).
Fix: allow `skill` when the skillId is SKILL_FIRST_AID or SKILL_DOCTOR, and add
AP cost validation. The resumable combat barrier already has a mechanism for
waiting on AP-validated actions.

In `server_control.cc`, modify the combat gate:
```cpp
// Allow medic skills in combat (they cost AP, validated by actionUseSkill)
if (isInCombat() && !(skillId == SKILL_FIRST_AID || skillId == SKILL_DOCTOR)) {
    // drop
}
```

### B. Inventory + use-item in combat (stimpacks)

Currently the viewer has NO in-combat inventory access. The 'I' key is gated
on `!conn.inCombat()` (main.cc:1186). The viewer's inventory screen would need
a combat-safe path:
1. Open inventory in combat (the 'I' key OR a dedicated combat inventory binding)
2. Item use resolves to `useitem <pid>` verb
3. Server validates the item is a usable consumable (stimpack, antidote, etc.)
4. Server applies the item's effect via `_obj_use_item` or `itemUse` action
5. If self-use (target = self), no approach walk needed

This is more involved than skill use. For v1/demo, the medic skills (First Aid,
Doctor) are the critical path — stimpacks are secondary.

### C. Item pickup in combat

`get` verb is dropped in combat. Vanilla allows picking up items during combat
(it costs AP). Fix: allow `get` in combat with AP validation. The existing
`actionPickUp` already has a `serverLoopActive()` path — just needs the verb
gate relaxed.

### D. Doors/containers in combat

Vanilla blocks scenery use during combat (`_obj_use_door` is gated on
`!isInCombat()`). The wire combat gate for `use` is correct — don't relax it.
Doors and containers are NOT accessible during combat in vanilla, and shouldn't
be on the wire either.

## Priority ordering (for Temple demo)

1. **[HIGH]** Skill use in combat: First Aid / Doctor — CRITICAL for survivability
2. **[MEDIUM]** Item pickup in combat: `get` verb with AP validation
3. **[LOW]** Inventory use-item in combat: stimpacks — nice-to-have, medic skills
   cover most healing needs
4. **[WON'T FIX]** Doors/containers in combat — vanilla-correct to block them

## Verification

```bash
# Boot combat fixture, connect viewer, enter combat, try:
printf 'skill <selfNetId> 7\n' | nc -q1 127.0.0.1 9201  # First Aid (SKILL=7)
# Server should process it without "dropped" message
```
