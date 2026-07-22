# 001 — Dialog range fires too early ("talk across room")

**Status**: FIXED (2026-07-19, commit pending)
**File**: `src/server_control.cc` — `interactionRuleSatisfied`

## Symptom
Clicking TALK on an NPC fires the dialog before the character arrives.
The character runs toward the NPC, but dialog opens mid-run. Also possible
to trigger TALK from across the room with no walk at all (distance < 9).

## Root cause
`interactionRuleSatisfied` used `distance < 9` for talk, vs `distance <= 1`
for all other verbs. The approach walk stops one tile short of the target
(adjacent = distance 1). With the loose rule, the latch fired as soon as
the walk brought the dude within 8 hexes, or immediately at arm time if
already within range.

Two failure modes:
A. Dude at distance 0-8 → `serverControlArmInteraction` fires `interactionFire`
   immediately with NO walk registered.
B. Dude at distance >= 9 → walk registered, but `serverControlAdvancePending`
   polls `interactionRuleSatisfied` before checking `serverAnimWalkInFlightFor`
   — fires mid-walk when distance drops to ≤ 8.

## Fix
Changed talk adjacency to `distance <= 1` (same as use/get/skill).
Now the latch only fires when the approach walk has fully completed
and the dude stands adjacent to the NPC.

If `gameDialogEnter` rejects due to `_action_can_talk_to` (no-path /
too-far), the latch is consumed harmlessly and the dialog never opens.

## Verification
- Walk to NPC at distance > 1, click TALK → character walks all the way
  adjacent before dialog opens.
- Adjacent TALK → fires immediately (no walk needed).
