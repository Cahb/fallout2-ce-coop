#include "server_players.h"

#include <cstdlib> // getenv — the action-gate kill switch
#include <cstring> // strcmp

#include "critter.h"
#include "debug.h"
#include "input.h" // getTicks / getTicksBetween — wall-clock busy window (feature A)
#include "inventory.h" // HAND_LEFT / HAND_RIGHT — the active-hand default
#include "object.h"
#include "scripts.h" // scriptGetSelf — the O3 nearest-player anchor
#include "server_loop.h" // serverDedicatedActive — O3 is server-only
#include "tile.h" // tileGetTileInDirection — placement helper

namespace fallout {

// Empty until serverBoot registers. See the header: an EMPTY registry means
// { gDude } resolved dynamically, which is what keeps every non-server path
// (client viewer, headless probe, goldens) bit-for-bit unchanged. Note the
// dynamic resolution is also the RIGHT answer on the viewer, where gDude is a
// per-client role that gets repointed at join (MP_PROPOSAL Ch 5.6) — a
// registered pointer would go stale there; nothing registers on that path.
static Object* gPlayerActors[kMaxPlayerActors];
static int gPlayerActorCount = 0;

// Offline = true. Inverted so zero-init is "everyone online" — the vanilla
// state, and the only one the client/probe/goldens can ever observe.
static bool gPlayerActorOffline[kMaxPlayerActors];

int playerActorCount()
{
    return gPlayerActorCount > 0 ? gPlayerActorCount : 1;
}

Object* playerActorAt(int slot)
{
    if (gPlayerActorCount == 0) {
        return slot == 0 ? gDude : nullptr;
    }

    if (slot < 0 || slot >= gPlayerActorCount) {
        return nullptr;
    }

    return gPlayerActors[slot];
}

bool playerActorIs(Object* obj)
{
    if (obj == nullptr) {
        return false;
    }

    if (gPlayerActorCount == 0) {
        return obj == gDude;
    }

    for (int slot = 0; slot < gPlayerActorCount; slot++) {
        if (gPlayerActors[slot] == obj) {
            return true;
        }
    }

    return false;
}

int playerActorSlotOf(Object* obj)
{
    if (obj == nullptr) {
        return -1;
    }

    if (gPlayerActorCount == 0) {
        return obj == gDude ? 0 : -1;
    }

    for (int slot = 0; slot < gPlayerActorCount; slot++) {
        if (gPlayerActors[slot] == obj) {
            return slot;
        }
    }

    return -1;
}

// ---- Per-actor action gate + active hand (feature A; see server_players.h) --

// Wall-clock busy window: the tick a mark was made, and how long from it the actor
// stays busy. Zero-init = never marked = never busy (getTicksBetween(now, 0) is huge,
// which is >= a zero duration), the default the client/probe/goldens observe.
static unsigned int gActorBusyMarkAt[kMaxPlayerActors];
static unsigned int gActorBusyDurMs[kMaxPlayerActors];

// Active weapon hand per slot. Listed so the DEFAULT is HAND_RIGHT (right-hand-first,
// preserving serverDudeHitMode) rather than the zero-init HAND_LEFT.
static_assert(kMaxPlayerActors == 8, "gActorActiveHand initializer lists one value per slot");
static int gActorActiveHand[kMaxPlayerActors] = {
    HAND_RIGHT, HAND_RIGHT, HAND_RIGHT, HAND_RIGHT,
    HAND_RIGHT, HAND_RIGHT, HAND_RIGHT, HAND_RIGHT,
};

void serverActorBusyMark(int slot, int durMs, const char* what)
{
    if (slot < 0 || slot >= kMaxPlayerActors || durMs <= 0) {
        return;
    }
    gActorBusyMarkAt[slot] = getTicks();
    gActorBusyDurMs[slot] = (unsigned int)durMs;
    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        debugPrint("[actiongate] slot %d busy %dms (%s)\n", slot, durMs,
            what != nullptr ? what : "");
    }
}

void serverActorBusyMarkByNetId(int actorNetId, int durMs)
{
    if (actorNetId == 0) {
        return;
    }
    Object* actor = objectFindByNetId(actorNetId);
    int slot = actor != nullptr ? playerActorSlotOf(actor) : -1;
    if (slot >= 0) {
        serverActorBusyMark(slot, durMs, "seq");
    }
}

bool serverActorBusyIs(int slot)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return false;
    }
    return getTicksBetween(getTicks(), gActorBusyMarkAt[slot]) < gActorBusyDurMs[slot];
}

void serverActorBusyClearAll()
{
    for (int i = 0; i < kMaxPlayerActors; i++) {
        gActorBusyMarkAt[i] = 0;
        gActorBusyDurMs[i] = 0;
    }
}

int serverActorActiveHand(int slot)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return HAND_RIGHT;
    }
    return gActorActiveHand[slot] == HAND_LEFT ? HAND_LEFT : HAND_RIGHT;
}

void serverActorSetActiveHand(int slot, int hand)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return;
    }
    gActorActiveHand[slot] = hand == HAND_LEFT ? HAND_LEFT : HAND_RIGHT;
}

bool serverActionGateEnabled()
{
    // Default ON. Disabled only by an explicit F2_SERVER_ACTION_GATE=0 (the "0" form
    // matches the other numeric kill switches; any other value stays enabled).
    static bool enabled = []() {
        const char* v = getenv("F2_SERVER_ACTION_GATE");
        return v == nullptr || strcmp(v, "0") != 0;
    }();
    return enabled;
}

int playerActorRegister(Object* actor)
{
    if (actor == nullptr) {
        return -1;
    }

    // Idempotent: re-registering an actor returns its existing slot rather than
    // duplicating it (a duplicate would double-number it in the netId walk and
    // double-emit it in the baseline).
    for (int slot = 0; slot < gPlayerActorCount; slot++) {
        if (gPlayerActors[slot] == actor) {
            return slot;
        }
    }

    if (gPlayerActorCount >= kMaxPlayerActors) {
        return -1;
    }

    // Slot 0 must be the host actor, or every "slot 0 == gDude" assumption
    // downstream (scope restore, roster preference order, blob appendix
    // ordering) is quietly wrong.
    if (gPlayerActorCount == 0 && actor != gDude) {
        return -1;
    }

    int slot = gPlayerActorCount;
    gPlayerActors[slot] = actor;
    gPlayerActorCount = slot + 1;
    return slot;
}

void playerActorClear()
{
    for (int slot = 0; slot < kMaxPlayerActors; slot++) {
        gPlayerActors[slot] = nullptr;
        gPlayerActorOffline[slot] = false;
    }
    gPlayerActorCount = 0;
}

bool playerActorOnline(int slot)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return false;
    }
    return !gPlayerActorOffline[slot];
}

void playerActorSetOnline(int slot, bool online)
{
    // Slot 0 is gDude — the passive-sim anchor. It never goes offline
    // (host-transfer is its own future feature); refusing here keeps every
    // caller honest rather than each remembering the exception.
    if (slot < 1 || slot >= kMaxPlayerActors) {
        return;
    }
    gPlayerActorOffline[slot] = !online;
}

bool playerActorAnyAlive()
{
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        // An offline body is not a combatant: a fight must not stay open (or
        // "players still alive" hold) on someone who left the game.
        if (!playerActorOnline(slot)) {
            continue;
        }
        if (actor != nullptr && !critterIsDead(actor)) {
            return true;
        }
    }

    return false;
}

bool playerActorMayTransit(Object* actor)
{
    // ANY player actor may transit: whoever crosses an exit grid leads the whole
    // party to the new map (mapHandleTransition still places the group from slot
    // 0). This reverses the host-only rule (MP_PROPOSAL Ch 14.2) by owner decision
    // 2026-07-23 — the anti-grief / no-unconsented-travel asymmetry is dropped in
    // favor of any co-op player being able to lead travel. The question this
    // answers stays "is this actor the transit authority for its group", so when
    // there are multiple live maps or per-map groups it grows a real body HERE,
    // still without any call site testing a slot.
    return playerActorIs(actor);
}

int playerActorFindFreeTileNear(int center, int elevation)
{
    // Same primitives the AI flee path uses. Conservative by design: a spawn or
    // a post-transition placement inside a wall leaves an actor unable to move,
    // which is worse than co-locating two bodies on one tile.
    for (int dist = 1; dist <= 6; dist++) {
        for (int rot = 0; rot < ROTATION_COUNT; rot++) {
            int tile = tileGetTileInDirection(center, rot, dist);
            if (tile == -1 || tile == center) {
                continue;
            }

            if (_obj_blocking_at(nullptr, tile, elevation) == nullptr) {
                return tile;
            }
        }
    }

    return -1;
}

// The innermost held scope's actor, or nullptr when no scope is active. Depth is
// tracked purely to assert the nesting bound the design allows.
static Object* gContextActor = nullptr;
static int gScopeDepth = 0;

ServerActorScope::ServerActorScope(Object* actor)
    : _previous(gContextActor)
    , _engaged(actor != nullptr)
{
    if (!_engaged) {
        // A null actor is not an error (an unbound session's verb is rejected
        // before it gets here) — the scope simply does nothing, so gDude keeps
        // whatever the enclosing context set.
        return;
    }

    gContextActor = actor;
    gScopeDepth++;
    gDude = actor;

    // Depth > 2 means a nesting path nobody designed for. The known nest is
    // exactly dialog-barrier + one inner verb (MP_PROPOSAL.md Ch 11.2); a third
    // level means a new re-entrancy the passive-sim analysis has not covered.
    if (gScopeDepth > 2) {
        debugPrint("server_players: ServerActorScope depth %d — unexpected nesting\n", gScopeDepth);
    }
}

ServerActorScope::~ServerActorScope()
{
    if (!_engaged) {
        return;
    }

    gScopeDepth--;
    gContextActor = _previous;

    // Restore the PREVIOUS context, falling back to the host actor when this was
    // the outermost scope — that restores the invariant "gDude is the host actor
    // outside all scopes", which everything in the passive sim assumes.
    gDude = _previous != nullptr ? _previous : playerActorAt(0);
}

Object* scriptContextDude(Program* program)
{
    // (1) O2 — an acting player is known for certain.
    if (gContextActor != nullptr) {
        return gContextActor;
    }

    // (3-early) Off a real dedicated server, or with a single actor, there is
    // nothing to choose between: keep the vanilla answer verbatim. Note the
    // gate is serverDedicatedActive(), NOT serverLoopActive() — the headless
    // golden probe drives the same loop and must never take branch (2).
    if (program == nullptr || playerActorCount() < 2 || !serverDedicatedActive()) {
        return gDude;
    }

    // (2) O3 — the passive sim (critter heartbeats, AI turns, map procs) runs
    // scope-less, so "the player" has to be resolved from geometry. Nearest
    // LIVING actor on the script's own elevation; dead actors are corpses and
    // must not draw notice, and cross-floor answers would be worse than the
    // host anchor (AI rosters are elevation-filtered anyway, Ch 14.3a).
    Object* self = scriptGetSelf(program);
    if (self == nullptr) {
        // A self-less program is a map/system script — no geometry to resolve
        // against, so it keeps the host anchor.
        return gDude;
    }

    Object* nearest = nullptr;
    int nearestDistance = 0;
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr || critterIsDead(actor) || !playerActorOnline(slot)) {
            continue;
        }

        if (actor->elevation != self->elevation) {
            continue;
        }

        // Ties resolve to the LOWER SLOT because the loop only replaces on a
        // strict improvement — keep it that way, it is the only thing making
        // this deterministic when two players share a tile.
        int distance = tileDistanceBetween(actor->tile, self->tile);
        if (nearest == nullptr || distance < nearestDistance) {
            nearest = actor;
            nearestDistance = distance;
        }
    }

    return nearest != nullptr ? nearest : gDude;
}

void playerActorDied(Object* actor)
{
    // v1: deliberate no-op. The contract this must honor is in the header;
    // the reasoning is MP_PROPOSAL.md Ch 9.5.
    (void)actor;
}

} // namespace fallout
