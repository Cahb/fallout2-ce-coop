// f2_server headless animation backend (P5-C Step 1 head H2, [[p5-server-plan]]).
//
// COMPANION to server_shim.cc (the labeled per-side behavior-shim TU) and the
// OPPOSITE of server_stubs.cc (the loud-abort severance dashboard). This file
// resolves the 30 animation-family symbols that f2_core names at link time
// (reg_anim_*, animationRegister*, animation lifecycle, _object_animate) with a
// HEADLESS-SYNCHRONOUS implementation instead of the SDL-coupled animation.cc
// engine, which the server cannot link.
//
// WHY THIS EXISTS (the H2 correction to "the anim family is already guarded"):
// player-initiated animations ARE guarded (a !serverLoopActive() early-return
// applies the outcome directly), but three families run UNGUARDED under the
// server loop and reach these symbols every beat / every combat turn:
//   * enemy-AI movement            (combat_ai.cc, via unguarded combat.cc:3240)
//   * script-driven animation      (interpreter_extra.cc opcodes, via
//                                    scriptsHandleRequests every beat)
//   * map/elevator transitions     (map.cc mapHandleTransition every beat)
//   * the server's own `walkto`    (command.cc commandDispatch)
// On the in-process client-probe binary these worked because it links the real
// animation.cc + InstantAnimationScheduler, which finishes the sequence in one
// pump. f2_server links neither, so the same calls hit the abort stubs. A
// guard-and-skip would SILENTLY DROP AI movement; these must apply real state.
//
// THE MODEL — synchronous in CODE, zero wall-clock wait, never sleeps:
// each animationRegister* call applies its final game-state OUTCOME immediately
// and returns. reg_anim_begin/clear/end are bookkeeping no-ops (return 0);
// animationIsBusy() is always false, so combat's `while (animationIsBusy(x))
// _process_bk();` spin-loops (combat.cc:2589/2762, combat_ai.cc:3164/3210,
// interpreter_lib.cc:446) exit on their first check. Presentation duration
// ("play this move over N ms") is NOT computed here — the server never counts it
// down; it belongs to the future NetworkPresenter slice (STEP 2). This IS the
// locked cadence: logical-time-only, resolve-and-advance.
//
// STEPPED WALKING (F2_SERVER_SMOOTH_WALK=1, default off): with the gate set,
// OUT-OF-COMBAT registered moves enqueue a per-object walk instead of applying
// the whole path within one beat, and serverAnimAdvanceWalks() (server_anim.h,
// called once per beat from server_main's intents drain) steps each walk one
// tile — walk every 2nd beat, run every beat (≈ the original's pacing at the
// 100ms kServerTickDelta). Each step is one objectSetLocation → one objectMoved
// wire event in that beat's frame, so a connected viewer sees motion animate
// tile-by-tile instead of teleporting (STEP-4 follow-up; the per-step loop is
// also the future hook for en-route interaction / spatial procs — OPEN RISK a
// territory). IN-COMBAT moves stay synchronous regardless: a legacy combat turn
// drains entirely within one beat, so its approach completes before the attack.
// (Under the resumable-combat session gate — F2_SERVER_RESUMABLE_COMBAT, combat.cc
// — a fight spans beats, but serverAnimAdvanceWalks still early-returns on
// isInCombat(): in-combat approaches remain synchronous within their own turn.)
// Default-off because the golden harnesses script `walkto` + follow-up verbs at
// fixed ticks assuming instant arrival — flipping the default means re-authoring
// those timelines. animationIsBusy() stays false even mid-walk: the beat loop is
// the only advance point, so any spin-wait would deadlock; scripts polling
// op_anim_busy see "idle" mid-walk (same fidelity class as apply-immediately).
//
// WHY A SERVER-SIDE IMPL, NOT A CORE fn-ptr SEAM (Design 2 over the cut-list's
// Design 1): the client backend is the whole SDL animation.cc engine; this
// server backend is a from-scratch synchronous applier. They share NO code, so
// there is no shared policy to host in f2_core (unlike timing.cc's getTicks).
// This is genuinely-divergent per-side behavior -> a labeled boundary TU, the
// exact precedent set by slice-1d server_shim.cc. animation.cc and f2_core are
// left untouched (client stays byte-identical; no upstream-merge churn). Fully
// reversible to a core fn-ptr backend if a listen-server ever needs runtime swap.
//
// FIDELITY BOUNDARY (deliberately bounded; each choice is defensible and the
// link references are cleared regardless):
//   REAL game state (confident semantics + confirmed server reach + verifiable):
//     move/run to tile/object, rotate-to-tile, set-fid, unset-flag.
//   SAFE no-op (return success; presentation, or destructive/H4-coupled with
//   unconfirmed server reach, and unverifiable by the current goldens):
//     animate / animate-reversed / animate-forever (gestures return to STAND ->
//     no persistent state change), animate-and-hide + hide-object-forced
//     (faithful = objectDestroy, too destructive to apply blind; and
//     animate-and-hide needs buildFid = H4), move-to-tile-straight (knockback/
//     stairs = already-banked knockback gap), ping, play-sfx, set-light,
//     take-out-weapon, and ALL callbacks. Callbacks are no-op because the only
//     server-reachable ones are pure presentation (combat_ai _ai_print_msg float
//     text -> would abort on the textObjectsGetCount tail stub; _gsnd_anim_sound
//     -> never registered, its register is a no-op here); no state-bearing
//     callback is known on a server anim path (combat state flows through
//     _combat_apply_attack_results/critterKill, not anim callbacks). This DROPS
//     the recon's "callbacks fire in registration order" ideal in favor of not
//     pulling tail stubs / risking aborts; revisit if a state-bearing callback
//     on a server path is found.
//
// BANKED GAPS (same class as the already-tracked multi-victim-knockback gap;
// none crash, none corrupt state):
//   * (CLOSED) per-tile scriptsExecSpatialProc — see serverAnimStepOnce. It was
//     the one banked item that mutated SIM state rather than presentation.
//   * straight-line forced moves (knockback slide, stair transitions) are no-op.
//   * animate-and-hide / hide-object-forced do not remove the object.
//
// buildFid (H4) is intentionally NOT needed: the mover changes tile/rotation/AP
// only; the walk fid is presentation and the object begins/ends at STAND.
// Combat death FID is applied elsewhere (critterKill, via
// _combat_apply_attack_results(false) at combat.cc:3520), not through here.

#include "animation.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "actions.h" // actionShowDeathCallbackPtr / actionDeathDropItems — the STATE half
#include "art.h" // artExists — "can this mover's walk be realized" (see serverAnimMoveArtAvailable)
#include "combat.h"
#include "map.h"
#include "movement.h"
#include "obj_types.h"
#include "object.h"
#include "path.h"
#include "pres_record.h"
#include "presenter.h"
#include "presenter_network.h"
#include "proto_instance.h"
#include "scripts.h" // scriptsExecSpatialProc — per-tile trap/trigger procs
#include "server_anim.h"
#include "server_loop.h"
#include "server_players.h" // ServerActorScope / playerActorSlotOf — edge-grid transitions for extras
#include "sim_clock.h"
#include "tile.h"

namespace fallout {

// Arm the presentation recorder: THIS is the recording animation backend (the
// leaves below record instead of executing). Linking this TU (f2_server only)
// runs the ctor at static-init, so presRecordEnabled() may honor its env flag.
// The client/golden binary links animation.cc instead → never armed → the flag
// is inert there (avoids the double-free described in pres_record.h).
// Applies the authoritative in-combat walk stashed by the MOVE record leaves; armed as the
// pres_record deferred-commit hook so f2_core composites (combat_ai/combat_drain) can invoke
// it after shipping the presSeq. Defined below, near the move leaves.
static void serverAnimCommitDeferredWalk();

namespace {
struct PresRecordBackendArmer {
    PresRecordBackendArmer()
    {
        presRecordSetBackendActive(true);
        presRecordSetDeferredCommitHook(serverAnimCommitDeferredWalk);
    }
};
static PresRecordBackendArmer gPresRecordBackendArmer;
} // namespace

// Max path length _make_path can emit into a rotations buffer; matches the
// engine's AnimationSad::rotations[3200] (animation.cc).
static const int kServerAnimMaxPath = 3200;

// ---- Stepped walking (F2_SERVER_SMOOTH_WALK=1) -----------------------------
// See the STEPPED WALKING section of the file header. Registry of in-flight
// out-of-combat walks, advanced one tile per beat by serverAnimAdvanceWalks().

static bool smoothWalkEnabled()
{
    // Default ON for a server (serverFeatureEnabled): out-of-combat walkers should
    // ANIMATE, not teleport. F2_SERVER_SMOOTH_WALK=0 restores the instant-apply path.
    static bool enabled = serverFeatureEnabled("F2_SERVER_SMOOTH_WALK");
    return enabled;
}

// S5 (COMBAT_CLIENT_DESIGN.md §3.d): stamp a presentation duration on IN-COMBAT
// per-step moves so a connected viewer glides critters between tiles instead of
// teleporting. Gated on F2_SERVER_RESUMABLE_COMBAT (mirrors combat.cc's
// combatResumableEnabled): only the resumable session flushes a whole turn's
// moves into one beat's frame as a burst the viewer can pace — and the combat
// goldens all run with the gate OFF, so their wire stays byte-identical
// (durMs 0 = snap, the pinned discriminator). Out-of-combat pace is unaffected;
// that path is the stepped-walk registry above.
static bool combatGlideEnabled()
{
    static bool enabled = serverFeatureEnabled("F2_SERVER_RESUMABLE_COMBAT");
    return enabled;
}

// Movement pace as a GAME RULE in sim milliseconds per tile, deliberately NOT in
// beats: beats-per-step is DERIVED from kServerTickDelta below, so a future finer
// beat (50ms serverTick) changes scheduling granularity, not walking speed
// (design-review pin, 2026-07-15). These also ride the wire as move.durMs — the
// client's animate-vs-snap discriminator and per-hop animation duration.
// Measured from the player FRMs (HMJMPSAB walk / HMJMPSAT run in critter.dat) as
// frames-per-tile x (1000/artFps): walk = fps10, ~3.7-4.0 frm/tile -> ~371-400ms;
// run = fps20, 2.5 frm/tile -> 125ms. Old placeholders (200/100) were ~2x fast for
// walk, ~25% fast for run. CONSTRAINT: the smooth-walk path emits one tile every
// (thisValue / kServerTickDelta) beats AND stamps thisValue as the glide durMs, so a
// value that is NOT a multiple of kServerTickDelta (100) emits faster than it plays and
// snaps (CAP-ERASE). So we use the nearest smooth multiples: walk 400 (~vanilla), run
// 100 (closest to 125). TRUE vanilla (371/125) needs a finer kServerTickDelta (~25ms) —
// a design-class change (shifts sim-clock cadence -> re-bless goldens). See anim_pace.md.
static const int kWalkMsPerTile = 400; // ~2.5 tiles/s (vanilla ~371-400)
static const int kRunMsPerTile = 100; // ~10 tiles/s (vanilla 125=8t/s; 100 is nearest smooth)

static int serverWalkMsPerTile(bool run)
{
    return run ? kRunMsPerTile : kWalkMsPerTile;
}

static int serverWalkBeatsPerStep(bool run)
{
    int beats = serverWalkMsPerTile(run) / (int)kServerTickDelta;
    return beats > 0 ? beats : 1;
}

// F2_SERVER_OUTBOX_PACE: arms the per-client presentation outbox (server_net.cc). Read
// here too so the in-combat GLIDE can use frame-true durations only when the outbox
// paces emission to match — see serverGlideMsPerTile.
static bool serverOutboxPaced()
{
    static bool v = getenv("F2_SERVER_OUTBOX_PACE") != nullptr;
    return v;
}

// Per-tile pace for the IN-COMBAT glide (the burst path, serverAnimWalk below) — NOT
// the out-of-combat stepped registry (that stays beat-quantized: weld (b), §8.6). When
// the outbox paces emission (F2_SERVER_OUTBOX_PACE), return the TRUE art-frame pace so
// run stops being 25% fast — the player FRMs are walk HMJMPSAB fps10 (~371 ms/tile) and
// run HMJMPSAT fps20 (125 ms/tile), the exact values the kWalk/kRunMsPerTile comment
// rounded to 400/100 because emission was welded to kServerTickDelta. The outbox breaks
// that weld (it holds the next frame by this cost), so a non-100-multiple no longer
// CAP-ERASEs. Off the paced path → the quantized 400/100 (unchanged default; the
// resumable-combat gates run with OUTBOX_PACE unset, so their wire durMs stays 100).
static int serverGlideMsPerTile(bool run)
{
    if (serverOutboxPaced()) {
        return run ? 125 : 371;
    }
    return serverWalkMsPerTile(run);
}

struct ServerWalk {
    Object* owner;
    // Identity snapshot taken at enqueue: the liveness scan is pointer-identity,
    // and a destroy + create between beats can recycle the allocation — id/pid
    // must match too before the walk touches the object.
    int ownerId;
    int ownerPid;
    std::vector<unsigned char> rotations; // one direction byte per step
    int pos; // next step index
    int destTile; // dedupe key: a re-registered identical move keeps its progress
    int faceTile; // rotate here on natural completion (-1 = none; move-to-object)
    bool run; // selects the ms-per-tile pace (kRunMsPerTile vs kWalkMsPerTile)
    int beatsWaited; // beats since the last step; steps when >= serverWalkBeatsPerStep
    // Tripwires, checked before every step (objectDestroy does NOT clear
    // animations, so the registry must protect its own pointers):
    int lastTile; // owner moved by something else since our last step → cancel
    int lastElevation;
    unsigned int generation; // mapGetLoadGeneration() at enqueue; a load frees every Object*
};

static std::vector<ServerWalk> gServerWalks;

// Bumped on EVERY registry mutation. A step can open a door, whose USE script
// runs synchronously (proto_instance.cc) and may re-enter this registry —
// reg_anim_clear, a fresh register-move, destroy-with-destroy-proc. The advance
// loop holds a reference/index into the vector across the step, so it snapshots
// this counter around serverAnimStepOnce and aborts the beat if it moved
// (surviving walks simply resume next beat).
static unsigned int gServerWalksEpoch = 0;

static void serverWalksCancelFor(Object* owner)
{
    for (size_t i = 0; i < gServerWalks.size(); i++) {
        if (gServerWalks[i].owner == owner) {
            gServerWalks.erase(gServerWalks.begin() + i);
            gServerWalksEpoch++;
            return; // at most one walk per owner
        }
    }
}

static void serverWalksClear()
{
    gServerWalks.clear();
    gServerWalksEpoch++;
}

// Query: is a stepped walk currently enqueued for `owner`? The interaction latch
// (server_control.cc) polls this each beat to tell "still approaching" apart from
// "walk finished / never started" (INTERACTION_UX_DESIGN.md §2.3). Pointer-identity
// is sufficient: the caller re-validates its target by netId every poll and only
// ever asks about the live claimant actor. Under instant (non-smooth) walking the
// registry is always empty — a move applies fully at register time — so this
// returns false and the latch resolves by adjacency on the next poll, as intended.
bool serverAnimWalkInFlightFor(Object* owner)
{
    for (const ServerWalk& walk : gServerWalks) {
        if (walk.owner == owner) {
            return true;
        }
    }
    return false;
}

// Pointer-identity liveness check: is `owner` still in the world? objectDestroy
// frees the Object without clearing animations, so a stored owner can dangle;
// never dereference before this passes. (The objectFindFirst walk filters by
// art-internal fid TYPE, not OBJECT_HIDDEN — a script-hidden walker stays listed
// and keeps stepping, matching the client engine, which also keeps animating
// hidden objects.) The advance loop additionally matches the id/pid snapshot,
// guarding against a recycled allocation.
static bool serverWalkOwnerAlive(Object* owner)
{
    if (owner == gDude) {
        return true;
    }
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        if (obj == owner) {
            return true;
        }
    }
    return false;
}

static void serverWalkEnqueue(Object* owner, const unsigned char* rotations, int steps,
    int destTile, int faceTile, bool run)
{
    gServerWalksEpoch++;

    for (ServerWalk& walk : gServerWalks) {
        if (walk.owner != owner) {
            continue;
        }
        // Same destination re-registered (wanderer scripts re-issue their move
        // every map_update): keep the in-flight progress — unless the tripwire
        // state went stale (owner teleported / map generation moved), in which
        // case the freshly computed path below is the valid one.
        bool stale = owner->tile != walk.lastTile || owner->elevation != walk.lastElevation
            || walk.generation != mapGetLoadGeneration();
        if (walk.destTile == destTile && !stale) {
            walk.run = run;
            walk.faceTile = faceTile;
            return;
        }
        // New destination (or stale entry): replace with the path the caller
        // just computed from the owner's current tile.
        walk.ownerId = owner->id;
        walk.ownerPid = owner->pid;
        walk.rotations.assign(rotations, rotations + steps);
        walk.pos = 0;
        walk.destTile = destTile;
        walk.faceTile = faceTile;
        walk.run = run;
        walk.beatsWaited = 0;
        walk.lastTile = owner->tile;
        walk.lastElevation = owner->elevation;
        walk.generation = mapGetLoadGeneration();
        return;
    }

    ServerWalk walk;
    walk.owner = owner;
    walk.ownerId = owner->id;
    walk.ownerPid = owner->pid;
    walk.rotations.assign(rotations, rotations + steps);
    walk.pos = 0;
    walk.destTile = destTile;
    walk.faceTile = faceTile;
    walk.run = run;
    walk.beatsWaited = 0;
    walk.lastTile = owner->tile;
    walk.lastElevation = owner->elevation;
    walk.generation = mapGetLoadGeneration();
    gServerWalks.push_back(walk);
}

// Apply ONE tile step of a walk. Mirrors the per-step STATE mutations of
// _object_move (animation.cc): set facing, step one tile in that direction,
// open a usable door blocking the way, run the tile's spatial procs. Omits all
// presentation (pixel offset, frame/fid, dirty-rect refresh). Reads owner->tile
// fresh, exactly as the engine's ticker pass does.
// Returns false when the walk must STOP: blocked by an unusable obstacle (the
// planned path is blocked since it was computed — the engine re-paths, we stop
// short: its re-path-failure branch, animation.cc:2104, field_20 = -1000), or
// the walker died under a script, or a spatial proc latched a map transition.
//
// durMs rides the wire as move.durMs (presentation pace of this hop): the
// stepped advance passes its ms-per-tile; the synchronous within-beat path
// passes 0 = snap, keeping every golden wire byte-identical.
static bool serverAnimStepOnce(Object* owner, int rotation, int durMs, bool run)
{
    Rect rect; // required out-param of the object primitives; discarded headless

    objectSetRotation(owner, rotation, &rect);

    int nextTile = tileGetTileInDirection(owner->tile, rotation, 1);

    Object* obstacle = _obj_blocking_at(owner, nextTile, owner->elevation);
    if (obstacle != nullptr) {
        if (canUseDoor(owner, obstacle)) {
            _obj_use_door(owner, obstacle, false);
            // The door's USE script ran synchronously and can do anything —
            // including destroying the walker itself. Never touch `owner`
            // again without re-proving it is still in the world.
            if (!serverWalkOwnerAlive(owner)) {
                return false;
            }
        } else {
            return false;
        }
    }

    // Bracketed set/clear so the pending value can never leak onto an unrelated
    // mover's objectMoved (the door script above runs BEFORE the set).
    int elevation = owner->elevation;

    // Scope gDude to the mover for the commit + spatial proc below. The engine's
    // exit-grid trigger inside objectSetLocation is gated on `obj == gDude`, and
    // the spatial proc keys the acting player the same way — but this stepped-walk
    // playback runs at tick top level with gDude still bound to the HOST, so an
    // extra (slot >= 1) walking onto a map edge would otherwise never latch a
    // transition. No-op for the host (slot 0, already gDude) and for NPC walkers
    // (ServerActorScope(nullptr) does nothing).
    ServerActorScope moverScope(playerActorSlotOf(owner) >= 1 ? owner : nullptr);

    presenterSetNextMoveDurationMs(durMs);
    presenterSetNextMoveRun(run);
    objectSetLocation(owner, nextTile, elevation, &rect);
    presenterSetNextMoveDurationMs(0);
    presenterSetNextMoveRun(false);

    // PER-TILE SPATIAL PROC — the sim-state half of a step, and for a long time
    // the one banked gap here that could silently diverge WORLD state (a trap the
    // walker should have tripped simply not firing). Vanilla runs this from the
    // animation pass after every _object_move that changed the tile
    // [animation.cc:2274]; the stepped walker replaces that pass, so it owes the
    // same call. Scripts see the walker ON the tile, which is why it follows the
    // move rather than preceding it.
    //
    // ⚠ This is the most dangerous call in this file: an arbitrary script runs
    // SYNCHRONOUSLY here and may cancel animations, re-enter the walk registry,
    // start combat, latch a map transition, or destroy the walker itself. Passing
    // the tile/elevation by VALUE (captured above) is deliberate — `owner` may be
    // freed by the time this returns, so it must not be dereferenced for its own
    // arguments. The advance loop's epoch snapshot covers registry mutation; the
    // liveness re-proof below covers the walker's own death, exactly as the door
    // script above is handled.
    scriptsExecSpatialProc(owner, nextTile, elevation);
    if (!serverWalkOwnerAlive(owner)) {
        return false;
    }

    // A tile we just crossed triggered an exit grid. The load itself happens at
    // the beat tail (mapHandleTransition), so every further step would be walking
    // a world that is already condemned — and could trip a SECOND transition on
    // the way. Stop short; vanilla gets this for free by only ever stepping once
    // per frame.
    if (mapTransitionPending()) {
        return false;
    }

    return true;
}

// Walk `owner` up to `steps` tiles along `rotations` (one direction byte per
// step, as produced by _make_path), all within the current beat. In combat,
// charge the per-step movement AP — a combat-only rule that applies to critters
// only, matching _object_move (animation.cc:2117); outside combat movement is
// free.
//
// Each step's MOVE carries durMs: 0 (snap) except IN COMBAT with the glide gate
// set (S5), where it carries the ms-per-tile so the viewer glides the approach.
// The whole turn still drains synchronously within this beat — the durMs is a
// PRESENTATION pace on the wire, not a change to when state settles; the burst of
// stamped hops arrives in one frame and the viewer paces it (client_anim.cc).
static void serverAnimWalk(Object* owner, const unsigned char* rotations, int steps, bool run)
{
    if (steps <= 0) {
        return;
    }

    bool chargeAp = isInCombat() && FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER;
    int durMs = (combatGlideEnabled() && isInCombat()) ? serverGlideMsPerTile(run) : 0;

    for (int step = 0; step < steps; step++) {
        if (!serverAnimStepOnce(owner, rotations[step], durMs, run)) {
            break;
        }

        // Idle-deadline pacing (server_loop.h): tally this AI mover's on-wire glide time
        // so the player barrier can add the backlog before starting the human clock. Only
        // NON-player movers count (the player's own glide is already on screen), and only
        // the glide path (durMs>0 = resumable) — off the glide path this never fires, so
        // goldens are byte-identical. `owner != gDude` is a v1 player-actor test
        // ([[mp-actor-architecture-principle]]): a cost estimate, not a logic branch.
        if (durMs > 0 && owner != gDude) {
            serverAddPresentationCostMs((unsigned int)durMs);
        }

        if (chargeAp) {
            // movementChargeApForStep spends _combat_free_move first, then AP,
            // and returns true once the critter can no longer afford a step.
            if (movementChargeApForStep(owner)) {
                break;
            }
        }
    }
}

// In-combat MOVE over the presentation record channel (COMBAT_MOVE_RECORD_DESIGN.md).
// The move leaf, inside a record section, records its REAL args (the RunTo/MoveTo leaf) and
// STASHES the authoritative walk here; the composite ships the presSeq FIRST, then calls
// presRecordCommitDeferred() → serverAnimCommitDeferredWalk() to apply the walk. So the
// presentation event precedes its own state (EVENT_MOVE / AP delta) on the wire — the client
// arms its held-delta reserve from the seq before those deltas land, HOLDS the mover's
// position + AP until the replayed walk completes, then reconciles. State always rides the
// state lane (never suppressed) → it can never be lost. See the client held-delta plumbing.
struct DeferredWalk {
    bool pending = false;
    Object* owner = nullptr;
    bool toObject = false;
    int tile = 0; // toTile only
    Object* target = nullptr; // toObject only
    int elevation = 0; // toTile only
    int actionPoints = 0;
    bool run = false;
};
static DeferredWalk gDeferredWalk;

static int serverAnimMoveToTileApply(Object* owner, int tile, int elevation, int actionPoints, bool run);
static int serverAnimMoveToObjectApply(Object* owner, Object* destination, int actionPoints, bool run);
// Defined with the sequence bookkeeping at the bottom of the file; the move leaves above
// need it to abandon a sequence they cannot realize.
static int serverAnimSeqFail();

static void serverAnimCommitDeferredWalk()
{
    if (!gDeferredWalk.pending) {
        return;
    }
    DeferredWalk w = gDeferredWalk;
    gDeferredWalk.pending = false; // clear before Apply (a mid-walk door script must not see it)
    if (w.toObject) {
        serverAnimMoveToObjectApply(w.owner, w.target, w.actionPoints, w.run);
    } else {
        serverAnimMoveToTileApply(w.owner, w.tile, w.elevation, w.actionPoints, w.run);
    }
}

// The once-per-beat advance point for the stepped-walk registry (see the
// STEPPED WALKING header section and server_anim.h). Runs from server_main's
// intents drain, so a step's objectMoved event lands in the same beat's frame.
void serverAnimAdvanceWalks()
{
    if (gServerWalks.empty()) {
        return;
    }

    // A combat turn drains synchronously within a beat; in-flight free-roam
    // walks hold position for its duration. Combat entry normally clears them
    // anyway via animationStop() (combat.cc:2524) — belt and braces.
    if (isInCombat()) {
        return;
    }

    for (size_t i = 0; i < gServerWalks.size();) {
        ServerWalk& walk = gServerWalks[i];
        bool drop = false;

        if (walk.generation != mapGetLoadGeneration()) {
            // A map load freed every Object*; never dereference the owner.
            drop = true;
        } else if (!serverWalkOwnerAlive(walk.owner)) {
            drop = true;
        } else if (walk.owner->id != walk.ownerId || walk.owner->pid != walk.ownerPid) {
            // Same address, different object: the owner was destroyed and the
            // allocation recycled between beats. The pointer scan can't see
            // that; the identity snapshot can.
            drop = true;
        } else if (walk.owner->tile != walk.lastTile
            || walk.owner->elevation != walk.lastElevation) {
            // Something else moved the owner since our last step (script
            // teleport, elevator): the path is stale — cancel.
            drop = true;
        } else {
            walk.beatsWaited++;
            if (walk.beatsWaited >= serverWalkBeatsPerStep(walk.run)) {
                walk.beatsWaited = 0;
                // A step can run a door USE script that re-enters this registry
                // (cancel / enqueue / destroy) — which invalidates `walk` and
                // `i`. Snapshot the epoch; if the step moved it, abort the beat
                // touching NOTHING (surviving walks resume next beat; a walk
                // whose owner advanced without its lastTile updated is dropped
                // by the tripwire above — safe direction).
                unsigned int epoch = gServerWalksEpoch;
                if (!serverAnimStepOnce(walk.owner, walk.rotations[walk.pos],
                        serverWalkMsPerTile(walk.run), walk.run)) {
                    if (gServerWalksEpoch != epoch) {
                        return;
                    }
                    drop = true; // blocked mid-walk: stop short (see serverAnimStepOnce)
                } else if (gServerWalksEpoch != epoch) {
                    return;
                } else {
                    walk.lastTile = walk.owner->tile;
                    walk.lastElevation = walk.owner->elevation;
                    walk.pos++;
                    if (walk.pos >= (int)walk.rotations.size()) {
                        if (walk.faceTile != -1) {
                            // Trailing rotate-to-face-target of a completed
                            // move-to-object (animation.cc:624).
                            Rect rect;
                            objectSetRotation(walk.owner,
                                tileGetRotationTo(walk.owner->tile, walk.faceTile), &rect);
                        }
                        drop = true; // arrived
                    }
                }
            }
        }

        if (drop) {
            gServerWalks.erase(gServerWalks.begin() + i);
        } else {
            i++;
        }
    }
}

// ---- Movement ------------------------------------------------------------
// animationRegister{Move,Run}To{Tile,Object}: build the path, then either apply
// the walk within this beat (default / in combat) or enqueue it on the stepped
// registry (F2_SERVER_SMOOTH_WALK, out of combat). Run == Move for game state
// (the per-step AP cost and route are identical); under stepped walking `run`
// additionally selects the every-beat pace instead of every 2nd beat.

// ►► CAN THIS MOVER'S WALK ACTUALLY BE REALIZED? The one leaf-level failure test the
// server needs, mirroring animationRegister{Move,Run}To* in animation.cc leaf for leaf:
//
//   * RunTo first asks artExists(ANIM_RUNNING) and DOWNGRADES to ANIM_WALK when there is
//     no run art (animation.cc:737). The server never did this either, so a critter with
//     no run art was walked at RUN pace here while the viewer played the walk — a
//     smaller sibling of the same divergence, fixed for free by asking the same question.
//   * Both then _anim_preload the CHOSEN anim's fid and fail the sequence if the art is
//     absent (animation.cc:700, :751). artExists is the right server-side spelling: it is
//     the same predicate, and unlike artLock it does not pull frames into a cache the
//     server has no intention of rendering from.
//
// Critters only — the fid anim nibble is a critter concept; scenery/items reach the
// world through MoveToTileStraight (projectile arcs), which vanilla preloads differently.
// `effectiveRun` reports the post-downgrade choice so the caller paces and RECORDS the
// anim the viewer will really play.
static bool serverAnimMoveArtAvailable(Object* owner, bool run, bool* effectiveRun)
{
    *effectiveRun = run;
    if (owner == nullptr || FID_TYPE(owner->fid) != OBJ_TYPE_CRITTER) {
        return true;
    }

    // Vanilla passes weaponCode 0 for this probe but the real weapon code for the
    // preload; mirror that asymmetry rather than tidying it (animation.cc:737 vs :748).
    if (run && !artExists(buildFid(OBJ_TYPE_CRITTER, owner->fid & 0xFFF, ANIM_RUNNING, 0, owner->rotation + 1))) {
        *effectiveRun = false;
    }

    int anim = *effectiveRun ? ANIM_RUNNING : ANIM_WALK;
    if (artExists(buildFid(OBJ_TYPE_CRITTER, owner->fid & 0xFFF, anim, (owner->fid & 0xF000) >> 12, owner->rotation + 1))) {
        return true;
    }
    // Name the refusal. A rooted critter is INVISIBLE otherwise — the AI simply stops
    // asking to move and nothing in the log says why — so this is the line that tells a
    // "why is this thing not chasing me" question from a real AI bug. pid names the proto,
    // which is what you look up.
    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        fprintf(stderr, "[anim-noart] net=%d pid=0x%08X anim=%d — no art, move refused (sequence abandoned)\n",
            owner->netId, owner->pid, anim);
    }
    return false;
}

// The authoritative body: build the path and walk (or enqueue out-of-combat). Byte-identical
// to the pre-record path; also the deferred-commit target for a recorded in-combat move.
static int serverAnimMoveToTileApply(Object* owner, int tile, int elevation, int actionPoints, bool run)
{
    if (actionPoints == 0) {
        return -1;
    }

    unsigned char rotations[kServerAnimMaxPath];
    // WIDE budget when a PLAYER asked to go there: vanilla's 2000-node search gives up on
    // long-but-walkable routes and the caller then registers nothing, so the click looks
    // like the game refusing to move. AI and scripts keep the vanilla budget (path.cc).
    int steps = playerActorIs(owner)
        ? _make_path_wide(owner, owner->tile, tile, rotations, 0)
        : _make_path(owner, owner->tile, tile, rotations, 0);
    if (steps == 0) {
        // No route. The engine registers the move and it fails silently at
        // execution; the register itself still returns success.
        return 0;
    }

    // Destination tile itself blocked -> stop one tile short, capping by AP
    // (animateMoveObjectToTile, animation.cc:1875-1886).
    if (_obj_blocking_at(owner, tile, elevation) != nullptr) {
        steps--;
        if (actionPoints != -1 && actionPoints < steps) {
            steps = actionPoints;
        }
    }

    if (steps > 0 && smoothWalkEnabled() && !isInCombat()) {
        serverWalkEnqueue(owner, rotations, steps, tile, -1, run);
        return 0;
    }

    // Synchronous apply: make sure no stale queued walk resumes over it.
    serverWalksCancelFor(owner);
    serverAnimWalk(owner, rotations, steps, run);
    return 0;
}

static int serverAnimMoveToTile(Object* owner, int tile, int elevation, int actionPoints, bool run)
{
    // ►►►► DO NOT POISON THE SEQUENCE HERE, EVEN THOUGH VANILLA DOES (_anim_cleanup on
    // actionPoints == 0). Tried it; it FAILED the record-purity melee gate — record mode
    // ended with the dude one tile further along and a thrown Spear on the ground that the
    // non-record run did not have. `actionPoints` is MUTABLE STATE, and record mode
    // legitimately DEFERS the authoritative walk (gDeferredWalk below), so the AP pool has
    // not been charged yet when this is called and the zero-test can answer differently in
    // the two modes. While the -1 was being ignored that asymmetry was harmless; routing it
    // into rc propagated it into whether the caller runs _combat_turn_run(), i.e. straight
    // into the simulation. Same shape as the record-purity AP asymmetry already on file
    // (actionPickUp's ignored _check_scenery_ap_cost return — do not honor it either).
    // ►► THE RULE THIS BUYS: only MODE-INVARIANT facts may fail a sequence. The art test
    // below qualifies (a critter's FRMs do not depend on record mode); an AP read does not.
    // Restoring vanilla's AP-driven cleanup needs the deferred-AP asymmetry solved first.
    if (actionPoints == 0) {
        return -1;
    }
    // A mover with no art for the walk cannot move — refuse the leaf and abandon the
    // sequence, exactly as _anim_preload's failure does in the real engine. This must sit
    // ABOVE the record branch: recording the move and then deferring it would ship the
    // viewer a walk it cannot play AND still relocate the critter authoritatively, which
    // is precisely the teleport.
    if (!serverAnimMoveArtAvailable(owner, run, &run)) {
        return serverAnimSeqFail();
    }
    // In-combat record: capture the leaf's REAL args and DEFER the authoritative walk (the
    // composite commits it after shipping the presSeq). presRecordActive() is true only
    // inside a combat MOVE bracket's ambient section (combatMoveRecorded()).
    if (presRecordActive()) {
        // owner->ap is the PRE-walk pool (the walk is deferred — it hasn't charged AP yet).
        int preWalkAp = FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER ? owner->data.critter.combat.ap : -1;
        presRecordMoveToTile(owner, tile, elevation, run ? ANIM_RUNNING : ANIM_WALK, actionPoints, preWalkAp, 0);
        gDeferredWalk = DeferredWalk{ true, owner, false, tile, nullptr, elevation, actionPoints, run };
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[cmove-rec] net=%d toTile=%d ap=%d run=%d\n",
                owner->netId, tile, actionPoints, run ? 1 : 0);
        }
        return 0;
    }
    return serverAnimMoveToTileApply(owner, tile, elevation, actionPoints, run);
}

int animationRegisterMoveToTile(Object* owner, int tile, int elevation, int actionPoints, int delay)
{
    return serverAnimMoveToTile(owner, tile, elevation, actionPoints, false);
}

int animationRegisterRunToTile(Object* owner, int tile, int elevation, int actionPoints, int delay)
{
    return serverAnimMoveToTile(owner, tile, elevation, actionPoints, true);
}

static int serverAnimMoveToObjectApply(Object* owner, Object* destination, int actionPoints, bool run)
{
    if (actionPoints == 0) {
        return -1;
    }

    // Already on the target tile -> nothing to do, no rotate (animation.cc:602).
    if (owner->tile == destination->tile && owner->elevation == destination->elevation) {
        return 0;
    }

    Rect rect;

    // The target is temporarily hidden so the pathfinder routes TO its tile
    // rather than treating it as a blocker; the walk then stops one tile short
    // (two for a multihex mover). Mirrors animateMoveObjectToObject
    // (animation.cc:1649-1685).
    unsigned char rotations[kServerAnimMaxPath];
    bool wasHidden = (destination->flags & OBJECT_HIDDEN) != 0;
    destination->flags |= OBJECT_HIDDEN;
    int steps = playerActorIs(owner)
        ? _make_path_wide(owner, owner->tile, destination->tile, rotations, 0)
        : _make_path(owner, owner->tile, destination->tile, rotations, 0);
    if (!wasHidden) {
        destination->flags &= ~OBJECT_HIDDEN;
    }

    if (steps > 0) {
        steps -= (owner->flags & OBJECT_MULTIHEX) != 0 ? 2 : 1;
        if (actionPoints != -1 && actionPoints < steps) {
            steps = actionPoints;
        }
    }

    if (steps > 0 && smoothWalkEnabled() && !isInCombat()) {
        // The trailing rotate-to-face-target (below) applies on completion
        // instead, from wherever the walk actually ends. faceTile is captured
        // now; a target that wanders mid-walk gets a re-registered move with a
        // fresh path anyway (destTile dedupe key changes).
        serverWalkEnqueue(owner, rotations, steps, destination->tile, destination->tile, run);
        return 0;
    }

    serverWalksCancelFor(owner);
    serverAnimWalk(owner, rotations, steps, run);

    // The engine's public animationRegisterMoveToObject appends a trailing
    // animationRegisterRotateToTile(owner, destination->tile) (animation.cc:624)
    // that executes AFTER the (possibly zero-length) move, so the mover ends up
    // facing its target. This fires regardless of whether a path was found or the
    // approach was already adjacent -> replicate unconditionally from the final
    // tile. rotation is persistent, serialized sim state.
    objectSetRotation(owner, tileGetRotationTo(owner->tile, destination->tile), &rect);
    return 0;
}

static int serverAnimMoveToObject(Object* owner, Object* destination, int actionPoints, bool run)
{
    // Mode-invariant facts only — see the *ToTile twin above for why this one must NOT
    // fail the sequence.
    if (actionPoints == 0) {
        return -1;
    }
    if (owner->tile == destination->tile && owner->elevation == destination->elevation) {
        return 0;
    }
    // Same art gate as the *ToTile family — the AI reaches a target through this entry
    // (_ai_move_steps_closer), so leaving it ungated would let a rooted critter still
    // chase, just via the other verb.
    if (!serverAnimMoveArtAvailable(owner, run, &run)) {
        return serverAnimSeqFail();
    }
    // In-combat record: capture the RunTo/MoveToObject leaf's REAL args and DEFER the
    // authoritative walk. The client replays *ToObject, so the real engine appends its own
    // trailing RotateToTile (vanilla-exact facing); the server's authoritative rotation
    // (in the deferred Apply) reconciles via the mover's held rotation delta.
    if (presRecordActive()) {
        int preWalkAp = FID_TYPE(owner->fid) == OBJ_TYPE_CRITTER ? owner->data.critter.combat.ap : -1;
        presRecordMoveToObject(owner, destination, run ? ANIM_RUNNING : ANIM_WALK, actionPoints, preWalkAp, 0);
        gDeferredWalk = DeferredWalk{ true, owner, true, 0, destination, 0, actionPoints, run };
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[cmove-rec] net=%d toObj=%d ap=%d run=%d\n",
                owner->netId, destination->netId, actionPoints, run ? 1 : 0);
        }
        return 0;
    }
    return serverAnimMoveToObjectApply(owner, destination, actionPoints, run);
}

int animationRegisterMoveToObject(Object* owner, Object* destination, int actionPoints, int delay)
{
    return serverAnimMoveToObject(owner, destination, actionPoints, false);
}

int animationRegisterRunToObject(Object* owner, Object* destination, int actionPoints, int delay)
{
    return serverAnimMoveToObject(owner, destination, actionPoints, true);
}

// ---- Rotation ------------------------------------------------------------

int animationRegisterRotateToTile(Object* owner, int tile)
{
    // Inside a record section this leaf must RECORD-ONLY, not apply: the state
    // (final rotation) is authoritative via the fast-path + objectDelta; applying
    // here too would double-mutate. Outside a section (today's path) it applies.
    if (presRecordActive()) {
        presRecordRotate(owner, tile);
        return 0;
    }
    Rect rect;
    objectSetRotation(owner, tileGetRotationTo(owner->tile, tile), &rect);
    return 0;
}

// ---- Explicit fid --------------------------------------------------------
// The caller passes a concrete fid (no buildFid); applying it matches the
// engine's end-state for the set-fid action.

int animationRegisterSetFid(Object* owner, int fid, int delay)
{
    // Mirrors _anim_change_fid (animation.cc:2760): an anim-type fid is set with
    // the frame reset to 0; a bare STAND fid (anim type 0) is rebuilt via
    // _dude_stand. _dude_stand is client-only (stubbed on the server) and no
    // server-reachable caller passes a STAND fid today, so that branch falls back
    // to a raw set.
    if (presRecordActive()) {
        presRecordSetFid(owner, fid, delay);
        return 0;
    }
    Rect rect;
    objectSetFid(owner, fid, &rect);
    if (FID_ANIM_TYPE(fid) != 0) {
        objectSetFrame(owner, 0, &rect);
    }
    return 0;
}

// ---- Flags ---------------------------------------------------------------
// Mirrors the ANIM_KIND_UNSET_FLAG branch of animationRunSequence
// (animation.cc:1440-1454): lighting and hidden route through their object
// primitives, everything else clears the bit directly.

int animationRegisterUnsetFlag(Object* object, int flag, int delay)
{
    if (presRecordActive()) {
        presRecordUnsetFlag(object, flag, delay);
        return 0;
    }
    Rect rect;
    if (flag == OBJECT_LIGHTING) {
        _obj_turn_off_light(object, &rect);
    } else if (flag == OBJECT_HIDDEN) {
        objectShow(object, &rect);
    } else {
        object->flags &= ~flag;
    }
    return 0;
}

// ---- Sequence bookkeeping ------------------------------------------------
// We apply each action at register time, so begin/clear/end have nothing to
// accumulate or run. reg_anim_begin still returns success (0) unconditionally: the
// engine's -1 cases (a sequence already open, or during animationStop) do not
// arise in the server's single-threaded, apply-immediately model.
//
// ►►►► BUT A SEQUENCE IS ATOMIC, AND reg_anim_end MUST BE ABLE TO SAY SO.
// reg_anim_end used to return 0 unconditionally too, and that quietly dropped an
// engine CONTRACT the callers depend on: in the real engine any leaf that cannot be
// realized calls _anim_cleanup() (animation.cc), which abandons the whole sequence
// and makes reg_anim_end() return -1 — and callers branch on exactly that:
//
//     reg_anim_begin(ANIMATION_REQUEST_RESERVED);
//     animationRegisterRunToTile(a1, destination, ...);
//     int rc = reg_anim_end();
//     if (rc == 0) { _combat_turn_run(); }        // combat_ai.cc:1218, :1268
//
// With `return 0` welded in, the server could never refuse a move, so the AI always
// believed its move happened. That is how a SPORE PLANT walks: vanilla keeps plants
// rooted with no rule that says "plants are rooted" — MAPLNT simply ships no walk
// FRM, so _anim_preload's artLock fails, the sequence is abandoned, and the AI skips
// its move. Emergent, but the mechanism is a real contract and this is the half of it
// we had stubbed out. (It is not just plants: of 96 critter art sets, exactly six have
// no walk frame — MAPLNT, MAGUNN/MAGUN2 gun turrets, MADEGG deathclaw egg, MABOS2 and
// the RESERV placeholder. All of them are stationary props implemented as critters,
// and we were walking every one of them.)
//
// ►► WHY THIS IS THE CONVERGENT FIX AND NOT A BAND-AID: the server and the viewer were
// answering the SAME question from DIFFERENT information — the server said "always
// yes", the client asked artExists and refused. An authority that decides one way while
// the presenter decides the other is a desync generator by construction, and the
// teleport IS that desync: the viewer's registration failed, nothing animated, and the
// reap snapped the sprite onto the authoritative tile. Making the server ask the
// question the client will ask removes the disagreement at the source instead of
// papering over its symptom.
static bool gSeqLeafFailed = false;

// A leaf that cannot be realized poisons the open sequence — the server's stand-in for
// _anim_cleanup(). Kept deliberately dumb (a flag, not a rollback): leaves apply
// immediately here, so there is nothing to unwind; what the callers actually need is to
// be TOLD the sequence did not happen.
static int serverAnimSeqFail()
{
    gSeqLeafFailed = true;
    return -1;
}

int reg_anim_begin(int requestOptions)
{
    gSeqLeafFailed = false;
    if (presRecordActive()) {
        presRecordSeqBegin(requestOptions);
    }
    return 0;
}

int reg_anim_clear(Object* a1)
{
    // Synchronously-applied actions have nothing to cancel, but an in-flight
    // stepped walk does (combat entry clears combatants this way,
    // combat.cc:2583/2755, and scripts cancel before re-driving an object).
    serverWalksCancelFor(a1);
    return 0;
}

int reg_anim_end()
{
    if (presRecordActive()) {
        presRecordSeqEnd();
    }
    // Report the abandoned sequence, then clear — a caller that ignores the result
    // must not inherit a stale poison from the previous bracket.
    bool failed = gSeqLeafFailed;
    gSeqLeafFailed = false;
    return failed ? -1 : 0;
}

// Always "not busy": every animation completed the instant it was registered, so
// the combat/interpreter busy-wait spin-loops terminate immediately.
int animationIsBusy(Object* a1)
{
    return 0;
}

// ---- Lifecycle (no-ops; there is no engine/sad list to manage) -----------

void animationInit()
{
}

void animationReset()
{
    serverWalksClear();
}

void animationExit()
{
    serverWalksClear();
}

// Stop everything mid-flight: synchronously-applied actions never are, but the
// stepped-walk registry can be. Combat entry calls this (combat.cc:2524), which
// is what freezes free-roam walks before a turn drain.
void animationStop()
{
    serverWalksClear();
}

// The per-pump ticker that drains the sad list on the client. No sads exist
// headless (all applied synchronously), so there is nothing to advance.
void _object_animate()
{
}

// ---- Presentation / deferred family (safe no-ops; see FIDELITY BOUNDARY) --
// Gestures that return to STAND (no persistent state), sound/light/ping/weapon-
// ready presentation, destructive-and-unconfirmed hides, straight-line forced
// moves (banked knockback/stair gap), and all callbacks. Each returns the
// engine's success value so callers proceed normally.

// Each presentation leaf below is a no-op on the normal server path (as before),
// and RECORDS a typed command inside a record section (pres_record.cc) so the
// viewer can replay the sequence through its own real reg_anim.

int animationRegisterAnimate(Object* owner, int anim, int delay)
{
    if (presRecordActive()) presRecordAnimate(owner, anim, delay);
    return 0;
}

int animationRegisterAnimateReversed(Object* owner, int anim, int delay)
{
    if (presRecordActive()) presRecordAnimateReversed(owner, anim, delay);
    return 0;
}

int animationRegisterAnimateForever(Object* owner, int anim, int delay)
{
    if (presRecordActive()) presRecordAnimateForever(owner, anim, delay);
    return 0;
}

int animationRegisterAnimateAndHide(Object* owner, int anim, int delay)
{
    // Faithful = play anim then objectDestroy(owner); destructive with
    // unconfirmed server reach and buildFid-coupled -> banked no-op. Recorded so
    // the viewer's real engine plays-then-destroys its own transient (the cloud).
    if (presRecordActive()) presRecordAnimateAndHide(owner, anim, delay);
    return 0;
}

int animationRegisterHideObjectForced(Object* object)
{
    // Faithful = objectDestroy(object) (ANIM_KIND_HIDE at _anim_set_end);
    // destructive with unconfirmed server reach -> banked no-op.
    if (presRecordActive()) presRecordHideForced(object);
    return 0;
}

int animationRegisterMoveToTileStraight(Object* object, int tile, int elevation, int anim, int delay)
{
    // Straight-line forced move (knockback / falling / stairs). Banked gap. The
    // record op carries only the PRESENTATION; the state (final tile) is still
    // owed by the banked knockback gap fix and is NOT applied here.
    if (presRecordActive()) presRecordMoveStraight(object, tile, elevation, anim, delay);
    return 0;
}

int animationRegisterMoveToTileStraightAndWaitForComplete(Object* owner, int tile, int elev, int anim, int delay)
{
    if (presRecordActive()) presRecordMoveStraightWait(owner, tile, elev, anim, delay);
    return 0;
}

int animationRegisterPing(int flags, int delay)
{
    if (presRecordActive()) presRecordPing(flags, delay);
    return 0;
}

int animationRegisterPlaySoundEffect(Object* owner, const char* soundEffectName, int delay)
{
    if (presRecordActive()) presRecordSfx(owner, soundEffectName, delay);
    return 0;
}

int animationRegisterSetLightIntensity(Object* owner, int lightDistance, int lightIntensity, int delay)
{
    if (presRecordActive()) presRecordSetLight(owner, lightDistance, lightIntensity, delay);
    return 0;
}

int animationRegisterTakeOutWeapon(Object* owner, int weaponAnimationCode, int delay)
{
    if (presRecordActive()) presRecordTakeOut(owner, weaponAnimationCode, delay);
    return 0;
}

// ►► STATE-BEARING CALLBACK ALLOWLIST (bugs list item X, stage 1).
//
// On the dedicated server an animation callback is NEVER invoked: the animation
// engine that would fire it at the action frame does not exist here, and both
// register entry points below simply record and return. This file's header used
// to justify that with "no state-bearing callback is known on a server anim
// path". That was FALSE, and it silently ate real outcomes for as long as the
// server has existed — f2_core registers all of the below through these two
// functions, and every one of them is a world-state mutation, not presentation:
//
//   _obj_use               use a door / scenery      actions.cc:1345/1456
//   _obj_pickup            pick an item up           actions.cc:1565
//   _obj_use_container     open a container          actions.cc:1593
//   scriptsRequestLooting  open the loot screen      actions.cc:1601/1676
//   _talk_to               start a conversation      actions.cc:2407
//   _set_door_state_closed finish a door close       proto_instance.cc:1847
//
// That is the "walked over to it and nothing happened, had to click again" class:
// the first click went through vanilla's move+callback path and the callback
// evaporated; the second found the actor already adjacent, so server_control.cc's
// own immediate-fire path ran instead. Two implementations of one feature, one of
// which did nothing.
//
// ALLOWLIST, not "invoke everything", deliberately (stage 1 of 2). Firing blind
// would run PRESENTATION callbacks that reach tail stubs and abort — the header
// names _ai_print_msg → textObjectsGetCount. ►► _check_scenery_ap_cost is also
// held back on purpose: the record and non-record paths already disagree about
// its return value ([[record-purity-ap-asymmetry]]), so it needs its own pass.
// Stage 2 inverts this to a denylist once the presentation set is audited, and
// then server_control.cc's parallel approach-then-fire can be deleted.
//
// Applied UNCONDITIONALLY, not only while recording: a non-recording server owes
// the same world state. Recording changes what is PRESENTED, never what is TRUE.
static void serverAnimApplyCallbackState(void* a1, void* a2, void* proc)
{
    if (proc == nullptr) {
        return;
    }

    // ►► _show_death is deliberately NOT applied here — see bugs list W/X.
    // Applying its STATE half (the annihilation inventory drop) at this site made
    // RECORD and NON-RECORD mode diverge: record mode walks the reg_anim path and
    // reaches this function, the plain server path resolves death directly in
    // combat.cc's `if (!animated)` branch and never does. The record-purity
    // explosion gate catches it (items on the floor with recording on, still in
    // the corpse with it off) — the same asymmetry class as
    // [[record-purity-ap-asymmetry]]. The drop must be applied where BOTH paths
    // converge; deriving the death anim there is the open part of that work.

    if (proc == (void*)(AnimationCallback*)_obj_use
        || proc == (void*)(AnimationCallback*)_obj_pickup
        || proc == (void*)(AnimationCallback*)_obj_use_container
        || proc == (void*)(AnimationCallback*)scriptsRequestLooting
        || proc == actionTalkToCallbackPtr()
        || proc == protoInstanceDoorCloseCallbackPtr()) {
        // Announce it. These callbacks silently no-opped for the entire life of the
        // server, so the FIRST question is not "is the fix correct" but "does this
        // path get reached at all in real play". A player's own clicks do NOT come
        // through here (server_control.cc calls the outcomes directly); only core's
        // vanilla action path does — script-driven uses, door auto-close, and the
        // like. If a long session never prints this line, the allowlist is inert in
        // practice and that is worth knowing before trusting it.
        fprintf(stderr, "f2_server: [anim-cb] applying state-bearing callback %s\n",
            proc == (void*)(AnimationCallback*)_obj_use ? "_obj_use"
                : proc == (void*)(AnimationCallback*)_obj_pickup ? "_obj_pickup"
                : proc == (void*)(AnimationCallback*)_obj_use_container ? "_obj_use_container"
                : proc == (void*)(AnimationCallback*)scriptsRequestLooting ? "scriptsRequestLooting"
                : proc == actionTalkToCallbackPtr() ? "_talk_to"
                : "_set_door_state_closed");
        ((AnimationCallback*)proc)((Object*)a1, (Object*)a2);
    }
}

int animationRegisterCallback(void* a1, void* a2, AnimationCallback* proc, int delay)
{
    if (presRecordActive()) presRecordCallback(a1, a2, (void*)proc);
    serverAnimApplyCallbackState(a1, a2, (void*)proc);
    return 0;
}

int animationRegisterCallback3(void* a1, void* a2, void* a3, AnimationCallback3* proc, int delay)
{
    // No v1 RECORD callback takes 3 args; drop it in record mode (state is applied
    // by the server fast-path, as when not recording).
    return 0;
}

int animationRegisterCallbackForced(void* a1, void* a2, AnimationCallback* proc, int delay)
{
    if (presRecordActive()) presRecordCallback(a1, a2, (void*)proc);
    serverAnimApplyCallbackState(a1, a2, (void*)proc);
    return 0;
}

} // namespace fallout
