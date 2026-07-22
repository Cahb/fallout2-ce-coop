#include "combat_intent.h"

#include <deque>

namespace fallout {

// See combat_intent.h. A plain FIFO; the server _combat_input drains it.
static std::deque<CombatIntent> gCombatIntents;

void combatIntentPush(int kind, int arg, int hitLocation, bool run, int hitMode, int actorSlot)
{
    // Field order matches CombatIntent: kind, arg, hitLocation, hitMode, run, actorSlot.
    // interactVerb/interactArg trail those and keep their defaults here — this
    // overload only ever builds ATTACK/MOVE/END_* intents, which have no verb.
    gCombatIntents.push_back(CombatIntent { kind, arg, hitLocation, hitMode, run, actorSlot });
}

void combatIntentPushVerb(int kind, int verb, int arg, int verbArg, int actorSlot)
{
    // Built field-by-field rather than positionally: the attack-shaped members in
    // the middle (hitLocation/hitMode/run) are meaningless for these kinds, and
    // spelling them out as placeholders in a brace-init is how the wrong value
    // eventually lands in one of them.
    CombatIntent intent;
    intent.kind = kind;
    intent.arg = arg;
    intent.actorSlot = actorSlot;
    intent.interactVerb = verb;
    intent.interactArg = verbArg;
    gCombatIntents.push_back(intent);
}

// The queue is ONE FIFO holding every player's intents, filtered on the way out
// rather than split into per-slot queues: order between two players does not
// matter (only one player acts at a time), while order WITHIN a slot does, and a
// single deque preserves that for free. N is <= kMaxPlayerActors and a turn's
// backlog is a handful of entries, so the scan is not worth optimizing.
bool combatIntentPeekForSlot(int slot, CombatIntent* out)
{
    for (const CombatIntent& intent : gCombatIntents) {
        if (intent.actorSlot == slot) {
            *out = intent;
            return true;
        }
    }
    return false;
}

void combatIntentPopForSlot(int slot)
{
    for (auto it = gCombatIntents.begin(); it != gCombatIntents.end(); ++it) {
        if (it->actorSlot == slot) {
            gCombatIntents.erase(it);
            return;
        }
    }
}

bool combatIntentPendingForSlot(int slot)
{
    CombatIntent ignored;
    return combatIntentPeekForSlot(slot, &ignored);
}

void combatIntentDropForSlot(int slot)
{
    for (auto it = gCombatIntents.begin(); it != gCombatIntents.end();) {
        it = it->actorSlot == slot ? gCombatIntents.erase(it) : it + 1;
    }
}

void combatIntentClear()
{
    gCombatIntents.clear();
}

} // namespace fallout
