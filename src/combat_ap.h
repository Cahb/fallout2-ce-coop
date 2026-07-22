#ifndef FALLOUT_COMBAT_AP_H_
#define FALLOUT_COMBAT_AP_H_

namespace fallout {

// ─── The one in-combat AP policy switch ─────────────────────────────────────
//
// OWNER RULING 2026-07-20: the AP drain on the in-combat interaction suite may
// prove irritating in co-op and must be removable "by a code fix, not
// necessarily an env var". Flip this to false and rebuild; every in-combat
// interaction becomes free.
//
// A build-time constant rather than a runtime lookup on purpose: an env var
// would be a live input at the trust boundary — a knob reachable on the one
// authoritative sim that every player shares — whereas this compiles the policy
// away entirely.
//
// Exactly THREE functions charge AP for player interactions, and all three
// consult this. There is no fourth; if you add one, put the check in it.
//   1. inventoryApCostApply      (item.cc)           — the inventory screen, 4 AP
//                                                      (2 with Quick Pockets), charged
//                                                      at OPEN and nothing per action
//                                                      inside it.
//   2. _check_scenery_ap_cost    (proto_instance.cc) — using or looting a world
//                                                      object, 3 AP.
//   3. actionUseItemOnObjectWithApCost (actions.cc)  — an inventory item used on a
//                                                      world target, 2 AP.
//
// Turning this off changes ONLY what things cost. Turn ownership, adjacency and
// target validation are enforced separately and deliberately do not consult it,
// so a free action is still an action taken on your own turn against a legal
// target.
//
// NOT covered, on purpose: the AP cost of ATTACKING and MOVING. Those are
// charged inside _combat_attack and movementChargeApForStep, they are the core
// combat economy rather than interaction feel, and vanilla has no notion of
// switching them off. This is about the interaction suite only.
constexpr bool kCombatApChargeEnabled = true;

} // namespace fallout

#endif /* FALLOUT_COMBAT_AP_H_ */
