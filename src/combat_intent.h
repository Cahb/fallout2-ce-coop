#ifndef FALLOUT_COMBAT_INTENT_H_
#define FALLOUT_COMBAT_INTENT_H_

#include "combat_defs.h" // HIT_LOCATION_UNCALLED (pure enum/struct header, obj_types.h only)

namespace fallout {

// Dude combat-turn intent queue (SERVER_LOOP_DESIGN.md §3; seed of the Phase 3.4
// typed command queue). Under the server loop the dude's turn is not driven by
// keyboard/mouse — the server _combat_input drains these intents and calls the
// core combat entry points directly (the same ones the AI uses). Data only: no
// combat/animation dependency, so this lives in f2_core (SDL-free).

enum CombatIntentKind {
    // Attack a target. arg = target netId, or -1 = nearest hostile critter.
    COMBAT_INTENT_ATTACK,
    // Move toward/onto a destination tile. arg = tile index.
    COMBAT_INTENT_MOVE,
    // End the dude's turn now. arg unused.
    COMBAT_INTENT_END_TURN,
    // Attempt to end combat (vanilla RETURN). arg unused. The drain calls
    // combatAttemptEnd(), which self-refuses (console msg) if hostiles remain.
    COMBAT_INTENT_END_COMBAT,
    // Interact with a world object on this actor's turn: use a door/lever, pick
    // an item up, loot a container, use an inventory item on a target. arg =
    // target netId; `interactVerb` says which; `interactArg` carries the skill
    // id / item pid the verb needs. The drain runs the SAME approach-then-fire
    // body the out-of-combat latch runs (server_control.cc), so there is one
    // implementation of "walk over and do it" rather than a combat fork of it.
    COMBAT_INTENT_INTERACT,
};

// NO "inventory" intent kind, deliberately. Vanilla prices the SCREEN, not the
// operation: opening charges once (item.cc inventoryApCostApply) and wield /
// unwield / drop / use inside it are free. While that screen is open the actor's
// turn is parked — the pump has drained its queue and is waiting across beats —
// so an inventory mutation arrives at the one moment nothing else is animating
// and can be applied inline at the drain. Queueing it as an intent would post it
// to a pump that is, by construction, already idle waiting for it.

// Sentinel hitMode meaning "let the server pick the dude's current weapon mode"
// (serverDudeHitMode) — used by the debug-port bare `cattack` and as a safe
// fallback when the viewer's interface bar can't report a weapon hit mode.
#define COMBAT_INTENT_HITMODE_AUTO (-1)

struct CombatIntent {
    int kind;
    int arg;
    // ATTACK only: the aimed hit location (HIT_LOCATION_HEAD..GROIN), or
    // HIT_LOCATION_UNCALLED for a normal unaimed shot. Ignored for MOVE/END_*.
    // Defaulted so a future 2-field brace-init can't silently become a HEAD shot.
    int hitLocation = HIT_LOCATION_UNCALLED;
    // ATTACK only: the client-selected hit mode (HIT_MODE_*), or
    // COMBAT_INTENT_HITMODE_AUTO to let the server pick. Ignored for MOVE/END_*.
    int hitMode = COMBAT_INTENT_HITMODE_AUTO;
    // MOVE only: run to the tile instead of walk. Same AP either way (vanilla —
    // running in combat is only a faster animation); ignored for ATTACK/END_*.
    bool run = false;
    // Registry slot of the PLAYER WHO ISSUED THIS (MP_PROPOSAL Ch 8.4). The queue
    // holds a mix of slots and the turn barrier consumes only the slot whose turn
    // it is, so an intent sent while someone else is acting waits for its own turn
    // instead of being spent on theirs. 0 = the host, which is what every existing
    // caller (debug port, headless probe) means and gets by default.
    int actorSlot = 0;
    // INTERACT / INVENTORY only: which verb, and its operand. Kept as opaque ints
    // here because this header is data-only and must stay in f2_core — the verb
    // enums live with the code that acts on them (server_control.cc). Appended
    // LAST, after actorSlot, so the positional brace-init in combatIntentPush
    // and every existing caller keep their meaning.
    int interactVerb = 0;
    int interactArg = 0;
};

// Enqueue an intent (FIFO). hitLocation/hitMode apply to ATTACK intents only, run
// to MOVE intents only; all default so existing callers are unaffected.
void combatIntentPush(int kind, int arg, int hitLocation = HIT_LOCATION_UNCALLED,
    bool run = false, int hitMode = COMBAT_INTENT_HITMODE_AUTO, int actorSlot = 0);

// Enqueue an INTERACT intent. Separate from the call above because it carries a
// different operand set entirely (verb + operand, no hit mode / location / run),
// and threading it through the attack-shaped signature above would mean four
// meaningless defaulted arguments at every call site.
void combatIntentPushVerb(int kind, int verb, int arg, int verbArg, int actorSlot);

// Peek / pop / test the front intent BELONGING TO `slot` (the queue is one FIFO
// carrying every player's intents; these preserve each slot's own order while
// ignoring the others'). Peek returns false and leaves `out` untouched when that
// slot has nothing queued.
bool combatIntentPeekForSlot(int slot, CombatIntent* out);
void combatIntentPopForSlot(int slot);
bool combatIntentPendingForSlot(int slot);

// Discard everything queued for one slot — the disconnect path (MP_PROPOSAL
// Ch 6.3): a player who drops must not have their stale orders executed when
// their body's turn comes around.
void combatIntentDropForSlot(int slot);

// Drop all queued intents (called at serverRun start so runs are independent).
void combatIntentClear();

} // namespace fallout

#endif /* FALLOUT_COMBAT_INTENT_H_ */
