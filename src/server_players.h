#ifndef FALLOUT_SERVER_PLAYERS_H_
#define FALLOUT_SERVER_PLAYERS_H_

namespace fallout {

struct Object;
struct Program;

// The player-actor registry (MP_PROPOSAL.md Ch 3; AGENTS/mp-actor-architecture-
// principle.md). Every "is this object THE player?" seam in the engine answers
// with pointer-equality against the one global gDude. This registry is the
// N-actor generalization of exactly that predicate: slot 0 is ALWAYS the host
// actor (gDude), extra player actors occupy slots 1..N-1.
//
// DEGENERACY IS THE SAFETY ARGUMENT: with nothing registered the registry
// behaves as { gDude } — playerActorIs(obj) is literally `obj == gDude`, the
// count is 1, and every generalized loop collapses to today's single check. So
// the client, the headless probe and every golden are byte-identical by
// construction, and the feature stays dark until the server registers extras.
//
// Data only (no combat/animation/SDL dependency) so it can live in f2_core,
// where the per-beat consumers are — same precedent as combat_intent.{h,cc}.

constexpr int kMaxPlayerActors = 8; // arbitrary sane cap; N is env-driven

// The dude pid: critter type (PID_TYPE is value >> 24), list index 0.
constexpr int kPlayerActorPidBase = 0x1000000;

// Reserved pids for EXTRA player actors' character sheets
// (PLAYER_SHEET_DESIGN.md §2): slot k > 0 carries kPlayerActorSheetPidBase + k.
//
// ⚠ THE RANGE MUST NOT START AT THE DUDE PID. 0x1000000 is special ONLY because
// vanilla reserved critters.lst index 0 for the player; 0x1000001.. are ORDINARY
// CRITTERS, loaded on demand by _proto_load_pid off line (pid & 0xFFFFFF) of the
// list. Reserving them hands seven real critter protos a blank sheet instead —
// silently, since the intercept happens before the loader ever runs. That is a
// gate-visible sim divergence, and it is how this range got its first value
// wrong. The index here is far past any .lst length and stays type byte 1, so a
// sheet pid is still a legal critter pid everywhere a pid is decoded.
constexpr int kPlayerActorSheetPidBase = 0x1FFFF00;

// The pid carried by the actor in `slot`. Slot 0 is the dude pid VERBATIM — the
// host actor is not a special case of the range, it IS the original object, and
// every `pid == 0x1000000` site in the engine must keep matching it.
static inline int playerActorSheetPid(int slot)
{
    return slot > 0 ? kPlayerActorSheetPidBase + slot : kPlayerActorPidBase;
}

// True iff `pid` names an EXTRA actor's sheet row (never the dude's).
static inline bool playerActorIsSheetPid(int pid)
{
    return pid > kPlayerActorSheetPidBase && pid < kPlayerActorSheetPidBase + kMaxPlayerActors;
}

// Number of player actors. 1 = today's behavior in every respect.
int playerActorCount();

// The actor at slot i (0 <= i < playerActorCount()), else nullptr.
// Slot 0 is gDude.
Object* playerActorAt(int slot);

// TRUE iff obj is a player actor. THE generalized "obj == gDude" predicate.
// O(N) over <= kMaxPlayerActors — cheap enough for per-beat/per-event use.
bool playerActorIs(Object* obj);

// Slot for obj, or -1.
//
// The registry is keyed by Object*, which is sound ONLY because player actors
// are OBJECT_NO_SAVE | OBJECT_NO_REMOVE like the dude: map teardown spares them,
// so the pointers are process-stable (MP_PROPOSAL Ch 4.2 Option A). If that
// lifetime guarantee is ever weakened, re-key on id+pid snapshots the way
// ServerWalk does — a stale Object* does not go dead, it resolves to a
// DIFFERENT object (see the netId re-mint lesson).
int playerActorSlotOf(Object* obj);

// Boot-time registration (server_boot.cc only; slot 0 = gDude).
// Registration is idempotent and membership is FIXED after boot: a disconnect,
// a death or a rebaseline changes an actor's BINDING or STATE, never the
// registry's membership. Returns the assigned slot, or -1 if full.
int playerActorRegister(Object* actor);

// Drop every registration (the registry reverts to its empty == { gDude }
// state).
//
// CLIENT-SIDE ONLY. The server registers once at boot and never clears —
// membership is fixed for the process lifetime, which is what lets every
// consumer key on slot indices and raw Object* without add/remove races. The
// VIEWER is the exception: its extra actors are ordinary blob-loaded objects
// that mapLoad frees on every rebaseline (only the local gDude object is
// NO_REMOVE there), so the viewer re-seeds the registry from each blob's actor
// appendix — clear, register gDude, then register the extras in slot order,
// all BEFORE reproducing objectAssignAllNetIds (MP_PROPOSAL.md Ch 5.3).
void playerActorClear();

// True iff at least one player actor is alive (not DAM_DEAD). This is the
// N-player replacement for the combat machine's "the dude is dead → end the
// fight" rules: the fight ends when ALL players are down, not the host alone.
bool playerActorAnyAlive();

// PRESENCE, not membership: an actor whose player disconnected is parked
// OFF-MAP (_obj_disconnect — the Object* stays valid, sheet/inventory intact,
// tile == -1) until its account logs back in. The registry NEVER shrinks
// (slots are permanent); this flag is what "the slot is vacant" means to the
// world: baselines, rosters, script anchors and transition placement skip an
// offline body. SERVER-side state — defaults online, so the client, the probe
// and every golden are byte-identical by construction. Slot 0 is always
// online (the host body anchors the passive sim and never despawns).
bool playerActorOnline(int slot);
void playerActorSetOnline(int slot, bool online);

// POLICY: may this actor trigger a MAP TRANSITION? Owner ruling (MP_PROPOSAL
// Ch 14.2): only the host travels; an extra walking an exit grid is inert, and
// when the host takes the exit everyone is carried. Deliberately a predicate and
// not a slot test at the call sites — see the body for why.
bool playerActorMayTransit(Object* actor);

// First unblocked tile in a growing ring around `center`, or -1. Shared by boot
// spawn placement and post-transition placement so both put extras beside the
// host the same way.
int playerActorFindFreeTileNear(int center, int elevation);

// Scoped binding of the ACTING player for a verb / dialog / player-sourced proc
// span (MP_PROPOSAL.md Ch 7.2). Sets gDude to `actor` for the duration.
//
// Why a global swap and not just a parameter: the callee tree below the verb
// layer reads gDude internally at dozens of sites (skill.cc's PC branches,
// proto_instance.cc's use/book/drug paths, scriptSetObjects' source_obj), and
// those reads are exactly what makes a verb behave "as the acting player".
// Threading a parameter through all of them is the post-v1 refactor; this gets
// per-actor correctness at the boundaries that matter now.
//
// ⚠ STACK DISCIPLINE, NOT "restore slot 0". The destructor restores the PREVIOUS
// context actor. Scopes nest in exactly one place — the dialog barrier holds a
// conversation-long scope while its pump services other sessions' verbs, each of
// which opens its own — and restoring slot 0 there would silently re-anchor the
// rest of the conversation, node procs included, onto the host.
//
// What the swap does NOT fix (do not chase these as bugs): PC-GLOBAL stores
// (XP, skill points, sneak state, kill counts, gDudeName) hit the one PC record
// regardless; and anything running OUTSIDE a scope — critter heartbeats, timer
// events, AI turns — still sees the passive-sim anchor (Ch 10).
class ServerActorScope {
public:
    explicit ServerActorScope(Object* actor);
    ~ServerActorScope();

    ServerActorScope(const ServerActorScope&) = delete;
    ServerActorScope& operator=(const ServerActorScope&) = delete;

private:
    Object* _previous;
    bool _engaged;
};

// The "player" a script should see (MP_PROPOSAL.md Ch 10.4, owner ruling
// 2026-07-19: O2+O3 together). This is what `dude_obj` resolves to — every
// script's answer to "where is the player / how far / is he sneaking".
// Resolution order:
//   1. the innermost ServerActorScope's actor, when one is held (O2: verb,
//      dialog driver, in-combat turn, spatial span);
//   2. on a DEDICATED server with >1 registered actor, the LIVING registered
//      actor nearest to the executing script's self, same elevation (O3) —
//      this is why the Program* is a parameter;
//   3. gDude — the vanilla value, and the only branch reachable off-server,
//      in the golden probe, or with a single registered actor, so every
//      existing path stays byte-identical by construction.
//
// ⚠ Step 2's accepted limitations are OPEN-Q #1 in MP_PROPOSAL Ch 19 and are
// NOT bugs: "nearest" means a zone that should hit every present player hits
// only the closest one per proc tick; an lvar state machine that latched
// "dude approached" against P1 can be advanced by P2 (double-fire class); and
// a script that caches dude_obj across beats freezes on nearest-at-cache-time.
Object* scriptContextDude(Program* program);

// ---- The player-death POLICY SEAM (MP_PROPOSAL Ch 9.5). ----
//
// Invoked exactly once per player-actor death transition, from critterKill's
// finalization, SERVER PATH ONLY. The default body is a deliberate NO-OP:
// respawn / permadeath / spectator policy is not designed yet (owner decision
// 2026-07-19). Replacing this body is the whole policy change.
//
// GUARANTEED STATE when this fires (from the critterKill body preceding it):
//   - actor->data.critter.hp == 0; combat.results has DAM_DEAD;
//   - corpse fid applied (or fallback), OBJECT_NO_BLOCK set unless CRITTER_FLAT,
//     light off; actor->sid removed (== -1, like any critter's);
//   - the actor is STILL in the object list, still in _combat_list if fighting
//     (the round-end sweep partitions it out later);
//   - queued DRUG events cleared; other queue events NOT;
//   - the session binding is UNTOUCHED — the session stays bound to its corpse.
// MAY fire mid-beat inside combat resolution or a queue drain: the body must
// not re-enter combat, must not mutate the combat roster, and must not emit
// blocking UI. Safe: presenter() emits, registry reads, latching state for the
// loop tail to act on.
void playerActorDied(Object* actor);

} // namespace fallout

#endif /* FALLOUT_SERVER_PLAYERS_H_ */
