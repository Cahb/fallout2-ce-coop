#include "client_present.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

#include "actions.h" // _action_attack
#include "animation.h" // reg_anim_* / animationRegister* / animationIsBusy / animationStop / _object_animate / ANIM_*
#include "art.h" // buildFid / artExists / artLock / artGetFramesPerSecond
#include "debug.h"
#include "game_sound.h" // sfxBuildOpenName / SCENERY_SOUND_EFFECT_*
#include "input.h" // getTicks / getTicksBetween
#include "item.h" // critterGetAnimationForHitMode / weaponGetAnimationCode / itemGetType
#include "object.h" // objectSetFid / objectSetFrame / objectSetLocation / objectSetRotation / _obj_offset
#include "tile.h" // tileToScreenXY / tileGetRotationTo / tileDistanceBetween

// See client_present.h for the module overview. This is step 2 ENTRY FUSION: the
// three former registries (gWalks glide / gDeferred attack-replay / gDoors slide)
// are folded into ONE table keyed by netId, one PresEntry per presented object.
// An object is legitimately BOTH a moving critter AND an attack/take-out replay
// participant at once (a wielding critter's held approach; a defender fleeing the
// same beat it is hit; a reserved participant still gliding into range), so the
// entry carries three ORTHOGONAL concerns side by side — not a single linear state:
//
//   • GLIDE   — live iff `hops` is non-empty. Plays a walk/run cycle and glides the
//               sprite between tiles for each authoritative MOVE hop; lags obj->tile
//               behind authority mid-glide (present-tile rebucketing, #6/#9).
//   • REPLAY  — `replay` ∈ {None, Reserved, Active}. Reserved at ATTACK_RESULT /
//               decode (holds the settled fid/flags/rotation so the corpse
//               pose doesn't beat the fall animation); Active while a reg_anim
//               sequence is in flight.
//
// The entry exists iff any concern is live; when a concern ends, the entry is
// dropped only once ALL are empty (eraseIfEmpty). The old cross-module
// `replaySuspended` flag + its per-frame setter are GONE — "the glide's tripwire is
// suspended while its own replay owns the pixels" is now the DERIVED predicate
// glideSuspended() (replay Active && the glide is still parked/unposed).
//
// The load-bearing invariant is unchanged: sim state (obj->tile/elevation, hp/ap,
// inventory) is ALWAYS the wire's latest authoritative value, applied at decode;
// this module only paces the PIXELS (MP_PROTOCOL.md §1: behind = snap forward,
// never replay). Off by default; only mainClientViewer enables it (presSetEnabled),
// so the headless joining client and every golden gate stay byte-identical.

namespace fallout {

namespace {

// One shared enabled flag for the whole presentation layer (was three, always set
// together in mainClientViewer). Disabling clears the table (presSetEnabled).
bool gEnabled = false;

// ---- glide timing / backlog constants (GLIDE) ------------------------------

// One authoritative hop awaiting or undergoing playback.
struct Hop {
    int fromTile;
    int toTile;
    int durMs;
};

// One server beat (kServerTickDelta). Playback starts one beat late so hop N+1 has
// normally arrived by the time hop N finishes — jitter absorbed, not stuttered.
constexpr unsigned int kStartDelayMs = 100;

// Backlog policy (§3.d). Out of combat hops trickle one per beat, so a queue
// deeper than this means the viewer fell behind DURING playback: snap forward. But
// an IN-COMBAT move is flushed as one same-frame BURST (a whole turn drains in one
// beat), which is choreography, not lag — allow the larger burst cap so a full move
// glides tile-by-tile. kMaxBurstHops covers the theoretical max combat move (~12 AP
// + up to 4 Bonus-Move free tiles ≈ 16 tiles); a longer burst still snaps its tail.
constexpr size_t kMaxQueuedHops = 4;
constexpr size_t kMaxBurstHops = 16;

// Backstop for a genuinely orphaned hold (a dropped release / lost world). A STALL
// timer, NOT a wall-clock lifetime: a hold waiting its turn in a big, slowly-
// draining fight is making progress and must never snap — only a hold sitting while
// the WHOLE presentation is frozen for this long is truly stuck. Measured against
// gProgressTick (bumped by any presentation activity).
constexpr unsigned int kHeldStallMs = 5000;

// Stepped hops stamp sim-ms-per-tile: run = 100, walk = 200/400 (server_anim.cc).

// ---- replay window bounds (REPLAY) -----------------------------------------
// kReplayCapMs backstops an ACTIVE sequence that never reports idle (measured from
// playback start — a single attack anim is short, so a wall-clock bound is correct;
// a progress-based one would let a stuck replay mask itself). kReserveStallMs
// backstops a RESERVED entry whose attack is never played (dropped / lost world);
// it is a STALL timer against gProgressTick — a reserved killing blow legitimately
// waits for every prior turn to animate, so only a frozen presentation resolves it.
constexpr unsigned int kReplayCapMs = 2000;
constexpr unsigned int kReserveStallMs = 5000;

// ---- door window bound (DOOR) ----------------------------------------------
// A short single sequence, so the same wall-clock cap as an attack replay backstops
// a slide that never reports idle (missing art / cancelled sequence).

// The replay concern's lifecycle. RESERVED holds deltas but does NOT count as an
// active replay (the presentation pump must be free to reach and start the attack).
enum class Replay {
    None,
    Reserved, // reserved at decode, awaiting its replay (holds the same-beat deltas)
    Active, // a reg_anim sequence is in flight (enterReplay, at play time)
};

// One recorded MOVE seq's held authoritative endpoint (position + AP). Held per-SEQ,
// not per-actor: when several recorded seqs stack on one actor in a turn (throw ->
// retrieve-move -> pickup-gesture -> wield -> move), a single shared bucket let seq N's
// completion snap to seq N+1's endpoint and clear the holds N+1 needed -> the drift-
// teleport (Fable review C.1). Frames form a FIFO; decode-order == execute-order (gate
// (1e) serializes execution), so DeferMove/DeferAp write the BACK (currently decoding)
// and a completion reap pops the FRONT (currently completing).
struct MoveHoldFrame {
    bool hasPos = false;
    int tile = 0;
    int elev = 0;
    bool hasAp = false;
    int ap = 0;
};

// How resolveHeld consumes the move-hold FIFO. One = pop the front frame only (a single
// move seq completed). None = leave the frames untouched (a NON-move seq — wield/throw —
// completed; it owns no frame). All = drain every frame applying the last (the whole
// entry is being torn down: forget/reset/reposition/reserve-stall).
enum class FrameReap {
    None,
    One,
    All,
};

// One presented object. Concerns are orthogonal (see file header): the entry lives
// while ANY is non-empty.
struct PresEntry {
    Object* obj = nullptr; // cached; keyed by obj->netId, verified on lookup

    // ---- GLIDE (was Walk); live iff !hops.empty() ----
    std::deque<Hop> hops; // front = the hop being played
    unsigned int startedAt = 0; // getTicks() when the front hop began; 0 = pending
    unsigned int readyAt = 0; // playback may begin (jitter buffer)
    unsigned int frameAt = 0; // last art-frame advance
    int ticksPerFrame = 100; // wall ms per art frame (frame selection only)
    int walkFid = 0; // the walk/run fid this module set; mismatch = authority overrode
    // A HELD glide (combat, awaiting its queued release) must not TOUCH the sprite's
    // pose: it parks at the hop origin in its ORIGINAL stand fid + facing until the
    // pump releases it — else a not-yet-presented turn's approach snaps to face its
    // destination while standing still. posed=false means the walk fid/travel-rotation
    // have NOT been applied to obj yet; the first PLAYABLE advance applies them.
    bool posed = false;
    // The stand fid the sprite wears WHILE unposed (parked, held). The fid tripwire
    // guards against THIS (not walkFid) until the pose is applied. Unused once posed.
    int parkedFid = 0;
    int elevation = 0;
    // The advance-generation at this glide's most recent hop push. A push sharing the
    // last push's gen arrived in the SAME frame (a combat burst); a later gen is a
    // fresh trickle arrival (the lag domain).
    unsigned int lastPushGen = 0;
    // Held-hop budget (§3.d). Hops at the TAIL of `hops` not yet released for
    // playback: playable count = hops.size() - heldHops.
    int heldHops = 0;
    // The server's authoritative end-facing, deferred here so a parked/gliding sprite
    // doesn't snap-turn before moving; applied when the glide drains.
    bool hasPendingRot = false;
    int pendingRot = 0;
    // An authoritative fid that arrived mid-glide, ROUTED here (applied at drain)
    // instead of written through — a write-through would trip the fid check and kill
    // the run, stand-sliding it (PRESENTATION_FSM_DESIGN §4.1a).
    bool hasPendingFid = false;
    int pendingFid = 0;
    // The presentation offset currently applied to obj->x/y. obj->x/y != applied
    // (with obj->tile off our bucket) means an authoritative snap landed.
    int appliedX = 0;
    int appliedY = 0;

    // ---- REPLAY (was Deferred); live iff replay != None ----
    Replay replay = Replay::None;
    unsigned int replaySince = 0; // getTicks() when reserved / entered active replay
    bool dHasFid = false;
    int dFid = 0;
    bool dHasFlags = false;
    unsigned int dFlags = 0;
    bool dHasRot = false; // facing is final visual state too — held like fid/flags so an
    int dRot = 0; // attacker doesn't snap to face its target before its turn presents
    // A COUPLED position keyframe (Pillar 1): a durMs<=0 authoritative MOVE that arrived
    // while this object had a live replay — the knockback whose slide the replay animates.
    // Held (obj->tile stays at the pre-knockback origin so the slide animates from there,
    // not a stale post-snap origin = bug J) and committed at resolveHeld, the slide's
    // completion / action frame. Distinct from the recorded-walk holdFrames FIFO: this is a
    // single loose snap, not tied to a MOVE_TO_TILE seq's frame.
    bool dHasSnapPos = false;
    int dSnapTile = 0;
    int dSnapElev = 0;

    // ---- in-combat MOVE hold (COMBAT_MOVE_RECORD_DESIGN.md) ----
    // A recorded combat walk owns the mover's motion; its authoritative position + AP
    // deltas ride the STATE lane (never suppressed) but are HELD here (armed at the seq's
    // decode) until the replayed walk completes, then reconciled in resolveHeld. Deferring
    // AP is what lets the client re-walk the identical tiles — the real engine charges AP
    // per step (animation.cc:2117) from the mover's PRE-walk pool, reproducing the server's
    // stop point instead of dying on step 1 with an already-drained pool.
    // moveHold mirrors !holdFrames.empty() (kept so the Defer hooks fast-reject a
    // non-mover). holdFrames = the per-seq FIFO (see MoveHoldFrame above). activeIsMove =
    // the currently-Active replay is a recorded MOVE that OWNS the front frame; a wield/
    // throw seq (no frame) leaves it false so its completion reaps nothing.
    bool moveHold = false;
    std::deque<MoveHoldFrame> holdFrames;
    bool activeIsMove = false;
    // Per-entry replay cap: a pathed walk (up to kMaxBurstHops tiles) outlives the generic
    // kReplayCapMs; a recorded move sets this so advanceReplays doesn't force-resolve it
    // mid-stride.
    unsigned int replayCapMs = 0; // 0 = use the generic kReplayCapMs
};

// The one table. Keyed by netId (monotonic, never recycled within a world — a missed
// forget-hook degrades to a lookup miss, not a freed-pointer deref); obj cached and
// verified against the key on every lookup.
std::unordered_map<int, PresEntry> gEntries;

// Bumped once per advanceGlides. Hops pushed with no advance between them share a
// generation — a same-frame burst (see PresEntry::lastPushGen).
unsigned int gAdvanceGen = 0;

// getTicks() of the last observed PRESENTATION PROGRESS — a glide hop advancing here,
// or the pump popping an event / animating an attack. Held glides + reserves measure
// their stall backstop against this. 0 until the first activity.
unsigned int gProgressTick = 0;

// -- entry lookup / lifecycle ------------------------------------------------

// Find obj's entry (keyed by netId, guarded against netId reuse by the cached obj).
// Returns nullptr if obj has no entry.
PresEntry* entryFind(Object* obj)
{
    if (obj == nullptr) {
        return nullptr;
    }
    auto it = gEntries.find(obj->netId);
    if (it == gEntries.end() || it->second.obj != obj) {
        return nullptr;
    }
    return &it->second;
}

// True when a glide is in flight (any hop, held or playing).
bool glideLive(const PresEntry& e)
{
    return !e.hops.empty();
}

bool entryEmpty(const PresEntry& e)
{
    return e.hops.empty() && e.replay == Replay::None;
}

// Drop the entry once no concern is left (called after ending any one concern). A
// no-op if another concern is still live — that is what keeps a held glide alive
// under an active replay, and a reserve alive under a still-gliding approach.
void eraseIfEmpty(int netId)
{
    auto it = gEntries.find(netId);
    if (it != gEntries.end() && entryEmpty(it->second)) {
        gEntries.erase(it);
    }
}

// DERIVED replacement for the old `replaySuspended` flag + its cross-module setter.
// While this object's OWN replay is Active and its glide is still parked/unposed,
// the replay (weapon draw / hit reaction) legitimately owns the sprite's fid + sub-
// tile offset; the glide's authority tripwires must stand down so they don't read
// the replay's per-frame writes as an override and erase the pending approach
// (→ teleport). A POSED/gliding walk is never suspended — the pump won't start a
// replay over a playable glide. Checked directly by the glide tripwires each frame,
// so it also covers a glide created AFTER the replay started (was the per-frame
// assert loop). NOTE: this suspends synchronously from enterReplay (one frame earlier
// than the old assert loop) — a benign tightening: on the first replay frame nothing
// foreign has been written yet (the held fid is deferred; _object_animate has not
// stepped the new sequence when advanceGlides runs).
bool glideSuspended(const PresEntry& e)
{
    return e.replay == Replay::Active && !e.posed;
}

// ============================================================================
// GLIDE helpers
// ============================================================================

// Rebuild obj's fid with a different anim code, keeping id/weapon, facing rot.
int animFid(Object* obj, int anim, int rotation)
{
    return buildFid(FID_TYPE(obj->fid), obj->fid & 0xFFF, anim, (obj->fid & 0xF000) >> 12, rotation + 1);
}

// Desired obj->x/y so the sprite renders at fraction t256/256 along the front hop,
// relative to the object's CURRENT tile bucket — which walkRebucket keeps at the
// front hop's toTile (the presented tile), so the anchor is the hop being PLAYED,
// not the far authoritative destination. Screen coords of all three tiles shift
// equally under camera scroll, so the difference is scroll-invariant per frame.
bool walkOffset(const PresEntry& e, int t256, int* dx, int* dy)
{
    const Hop& hop = e.hops.front();
    int fx, fy, tx, ty, ax, ay;
    if (tileToScreenXY(hop.fromTile, &fx, &fy, e.elevation) == -1) return false;
    if (tileToScreenXY(hop.toTile, &tx, &ty, e.elevation) == -1) return false;
    if (tileToScreenXY(e.obj->tile, &ax, &ay, e.elevation) == -1) return false;
    *dx = fx + (tx - fx) * t256 / 256 - ax;
    *dy = fy + (ty - fy) * t256 / 256 - ay;
    return true;
}

void walkApplyOffset(PresEntry& e, int t256)
{
    int dx, dy;
    if (!walkOffset(e, t256, &dx, &dy)) {
        return;
    }
    if (dx != e.appliedX || dy != e.appliedY) {
        _obj_offset(e.obj, dx - e.appliedX, dy - e.appliedY, nullptr);
        e.appliedX = dx;
        e.appliedY = dy;
    }
}

// Re-bucket obj into `tile`'s render slot. objectSetLocation moves the object between
// the renderer's per-tile z-order buckets (gObjectListHeadByTile, keyed by obj->tile).
// The decoder jumps obj->tile to a MOVE's FINAL destination immediately (state first);
// a multi-hop combat move thus buckets the sprite up to a whole move ahead of where it
// is visually gliding, so it paints OVER the walls it hasn't reached yet (#6) and a
// hit reaction / z-sort reads the fled tile (#9). Vanilla _object_move advances the
// tile ONE STEP at a time. This mirrors that: the glide keeps obj bucketed at the hop
// it is presently PLAYING (front hop's toTile, ≤1 tile of z-lead), so obj->tile LAGS
// authority for the glide's duration and reconciles to the authoritative destination
// (hops.back().toTile) at drain / cancel / stand-down. objectSetLocation zeroes
// obj->x/y, so the offset is cleared here and re-applied by the caller. No-op when
// already bucketed correctly, so it fires only at a hop boundary (front changed).
void walkRebucket(PresEntry& e, int tile)
{
    if (e.obj->tile == tile) {
        return;
    }
    objectSetLocation(e.obj, tile, e.elevation, nullptr);
    e.appliedX = 0;
    e.appliedY = 0;
}

// Retract this module's glide render state: remove the offset (only if it is still
// ours — after an authoritative objectSetLocation it is already gone) and put the art
// back to a stand cycle (only if the fid is still ours — an authoritative
// OBJECT_DELTA_FID outranks presentation).
void walkRetract(PresEntry& e)
{
    Object* obj = e.obj;
    if (obj->x == e.appliedX && obj->y == e.appliedY) {
        _obj_offset(obj, -e.appliedX, -e.appliedY, nullptr);
    }
    if (obj->fid == e.walkFid) {
        clientApplyPose(obj, animFid(obj, ANIM_STAND, obj->rotation));
    }
}

// Snap obj to its AUTHORITATIVE tile (the last decoded hop's toTile), undoing the
// render lag walkRebucket introduced. "Snap forward, never replay": dropping a glide
// must jump obj->tile to authority. Skipped when obj->tile no longer equals our
// presented bucket (front hop's toTile): an external objectSetLocation — a real
// reposition, or the decoder's jump right before a CAP-ERASE — has already set a NEWER
// authoritative tile we must keep. No-op for a single-hop / already-drained glide.
void walkSnapToAuthority(PresEntry& e)
{
    if (!e.hops.empty() && e.obj->tile == e.hops.front().toTile
        && e.obj->tile != e.hops.back().toTile) {
        objectSetLocation(e.obj, e.hops.back().toTile, e.elevation, nullptr);
        e.appliedX = 0;
        e.appliedY = 0;
    }
}

// End the GLIDE concern (only) — the fused analog of the old walkErase, which
// discarded the whole Walk. Reconcile the lagging tile forward, retract the pose,
// then reset every glide field so a future glide on this (surviving) entry starts
// clean. The replay/door concerns are untouched; the caller runs eraseIfEmpty.
// Any routed pendingFid/pendingRot are LANDED here (was: dropped — a banked latent gap).
// clientAnimDeferFid parks the server's authoritative fid/facing on the glide to avoid
// killing the run mid-write; that fid is the truth (a weapon/armor equipped while moving)
// and MUST survive every glide exit, not just the normal drain. Dropping it here lost the
// equip on any cancel / cap-erase / snap-kill — the world model kept the pre-equip pose
// while the paperdoll showed the new gear (owner-found: equip-while-running desync, both
// weapon and armor). Land it AFTER the stand-down so it wins the pose; idempotent with the
// drain path (which also applies its own captured copy).
void endGlide(PresEntry& e)
{
    walkSnapToAuthority(e);
    walkRetract(e);
    if (e.hasPendingFid) {
        clientApplyPose(e.obj, e.pendingFid);
        // Dirty the tile so the new pose repaints NOW. clientApplyPose writes the fid with a
        // null rect, so on a SETTLED sprite (glide just ended) nothing marks the tile dirty
        // and the dirty-rect renderer keeps drawing the old sprite until a full-screen redraw
        // — which is why an equip-while-running only showed up on the next inventory open/close
        // (owner-found). A moving sprite repaints every frame so it never needed this.
        Rect dirty;
        objectGetRect(e.obj, &dirty);
        tileWindowRefreshRect(&dirty, e.obj->elevation);
    }
    if (e.hasPendingRot) {
        objectSetRotation(e.obj, e.pendingRot, nullptr);
    }
    e.hops.clear();
    e.startedAt = 0;
    e.readyAt = 0;
    e.frameAt = 0;
    e.heldHops = 0;
    e.posed = false;
    e.hasPendingFid = false;
    e.hasPendingRot = false;
    e.appliedX = 0;
    e.appliedY = 0;
}

// ============================================================================
// REPLAY helpers
// ============================================================================

// Apply obj's held final state (the deferred corpse fid / armed pose / facing) and
// clear the replay-held bucket. When resume=true (the normal completion path, was
// walkSetReplaySuspended(false)) also re-baseline a still-parked held glide: adopt
// the settled (armed) fid as its tripwire baseline and re-park to origin, so the
// now-armed fid matches and the queued approach releases cleanly. resume=false for
// paths that must not touch the glide (reposition tripwire, throw-skip, forget).
void resolveHeld(PresEntry& e, bool resume, FrameReap reap = FrameReap::All)
{
    Object* obj = e.obj;
    // In-combat MOVE hold: reconcile authoritative position FIRST (the replayed walk
    // normally already landed here → no-op; a drift means the client walk stopped short —
    // snap to authority). Then AP (the client's per-step charge already ticked it there).
    // This is the state-never-lost guarantee: EVERY replay exit (completion, cap, reserve
    // stall, forget) runs through here, so a dropped/failed walk still snaps to truth.
    // `reap` selects which of the per-seq FIFO frames this exit consumes (see FrameReap).
    bool reconciledPos = false; // a recorded-walk frame committed a position this call
    if (reap != FrameReap::None && !e.holdFrames.empty()) {
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[frame-reap] net=%d mode=%s frames=%d objTile=%d activeIsMove=%d\n",
                obj->netId, reap == FrameReap::One ? "ONE" : "ALL", (int)e.holdFrames.size(), obj->tile, e.activeIsMove ? 1 : 0);
        }
        bool hasPos = false, hasAp = false;
        int tile = 0, elev = 0, ap = 0;
        do {
            MoveHoldFrame f = e.holdFrames.front();
            e.holdFrames.pop_front();
            if (f.hasPos) { hasPos = true; tile = f.tile; elev = f.elev; }
            if (f.hasAp) { hasAp = true; ap = f.ap; }
        } while (reap == FrameReap::All && !e.holdFrames.empty());
        if (hasPos) {
            if (getenv("F2_TRACE_EVENTS") != nullptr && obj->tile != tile) {
                fprintf(stderr, "[cmove-drift] net=%d walkedTo=%d authTile=%d\n", obj->netId, obj->tile, tile);
            }
            objectSetLocation(obj, tile, elev, nullptr);
            reconciledPos = true;
        }
        if (hasAp) {
            obj->data.critter.combat.ap = ap;
        }
    }
    e.moveHold = !e.holdFrames.empty();
    if (e.dHasSnapPos && !reconciledPos) {
        // Commit the coupled knockback position now the slide has finished animating from
        // the true origin (the action frame). Normally the recorded/replayed slide already
        // walked obj to this tile → a confirming no-op; a divergence snaps to authority
        // (state-never-lost). Cleared so a later non-move replay exit doesn't re-apply it.
        // Skipped when a recorded-walk frame just committed a position (reconciledPos): that
        // walk owns the authoritative tile, and a stale snap here would fight it (snap-back).
        if (getenv("F2_TRACE_EVENTS") != nullptr && obj->tile != e.dSnapTile) {
            fprintf(stderr, "[knock-commit] net=%d slidTo=%d authTile=%d\n", obj->netId, obj->tile, e.dSnapTile);
        }
        objectSetLocation(obj, e.dSnapTile, e.dSnapElev, nullptr);
    }
    e.dHasSnapPos = false; // consumed (or superseded by a recorded-walk pos) — never linger
    if (e.dHasFid) {
        objectSetFid(obj, e.dFid, nullptr);
        // Reset the frame to 0. objectSetFid changes only obj->fid, never obj->frame,
        // and the held fid is a SETTLED pose — most importantly the single-frame SF
        // corpse a kill produces. The just-finished fall/hit animation left obj->frame
        // on a high index; on a 1-frame corpse art that index is out of range and the
        // body renders as NOTHING (vanilla critterKill resets the frame for exactly
        // this reason). Frame 0 is the correct rest frame for every settled fid.
        objectSetFrame(obj, 0, nullptr);
    }
    if (e.dHasFlags) {
        // Same lifecycle-bit rule as the immediate path (client_net's delta
        // apply): a HELD flags delta lands here later, and would otherwise
        // resurrect the server's NO_REMOVE on a viewer-side player actor.
        objectApplyWireFlags(obj, e.dFlags);
    }
    if (e.dHasRot) {
        objectSetRotation(obj, e.dRot, nullptr);
    }
    e.dHasFid = false;
    e.dHasFlags = false;
    e.dHasRot = false;
    if (resume && glideLive(e) && !e.posed) {
        e.parkedFid = obj->fid; // adopt the settled (armed) fid as the tripwire baseline
        walkApplyOffset(e, 0); // re-park to origin, syncing obj->x/y == appliedX/Y
    }
}

// Mark obj's replay Active (at play time). Get-or-create the entry; a reserved→active
// transition keeps any held state (_action_attack's own reg_anim_clear resets the
// animation). Coexists with a live glide concern on the same entry.
void enterReplay(Object* obj, unsigned int now)
{
    if (obj == nullptr) {
        return;
    }
    PresEntry& e = gEntries[obj->netId];
    e.obj = obj;
    // Only (re)start the cap clock on a transition INTO Active. If the entry is ALREADY
    // Active, a fresh enterReplay must NOT reset replaySince — otherwise a rapid replay
    // flood (repeat lockpick/skill clicks each emitting an actionAnim gesture; the empty-
    // weapon attack spin) keeps pushing replaySince forward so the kReplayCapMs safety cap
    // NEVER expires. The replay then stays Active indefinitely → glideSuspended() freezes
    // the sprite AND the presentation drain is held → combat input locks → the ~62s idle
    // timeout. Measuring the cap from the FIRST activation bounds any stuck/flooded replay
    // to kReplayCapMs. Normal sequential animations still reap early via animationIsBusy;
    // the cap is only a backstop, so this changes behaviour solely in the stuck/flood case.
    if (e.replay != Replay::Active) {
        e.replaySince = now;
        // A fresh activation owns no move frame until a MOVE op claims one (below). A
        // wield/throw seq never claims, so its completion reaps no frame (FrameReap::None).
        e.activeIsMove = false;
    }
    e.replay = Replay::Active;
}

// Release a still-RESERVED hold now (its replay will never play — skipped throw,
// dropped attack). No-op if obj has no entry or its replay is already Active. resume=
// false: this path must not touch the glide (final state lands via server deltas).
void resolveReservedNow(Object* obj)
{
    if (obj == nullptr) {
        return;
    }
    PresEntry* e = entryFind(obj);
    if (e != nullptr && e->replay == Replay::Reserved) {
        resolveHeld(*e, /*resume=*/false);
        e->replay = Replay::None;
        eraseIfEmpty(obj->netId);
    }
}

// ============================================================================
// GLIDE advance
// ============================================================================

void advanceGlides()
{
    // Bump every frame, even with no entries: it marks a frame boundary for the
    // same-frame-burst test in clientAnimOnMove, which must hold whether or not a
    // glide is in flight.
    gAdvanceGen++;

    if (!gEnabled || gEntries.empty()) {
        return;
    }

    unsigned int now = getTicks();

    // Snapshot the ids with a live glide so we can erase entries freely without
    // invalidating an iterator mid-walk. The advance body never CREATES entries.
    std::vector<int> ids;
    ids.reserve(gEntries.size());
    for (auto& kv : gEntries) {
        if (!kv.second.hops.empty()) {
            ids.push_back(kv.first);
        }
    }

    for (int id : ids) {
        auto it = gEntries.find(id);
        if (it == gEntries.end()) {
            continue;
        }
        PresEntry& e = it->second;
        if (e.hops.empty()) {
            continue;
        }
        Object* obj = e.obj;

        // OFFSET authority tripwire. Something wrote obj->x/y we didn't. Two cases,
        // distinguished by obj->tile — ONLY objectSetLocation moves it, and it zeroes x/y:
        //   (a) obj->tile moved OFF our presented bucket (front hop's toTile) → an
        //       authoritative objectSetLocation relocated the sprite → KILL: drop the glide,
        //       authority stands (endGlide keeps the newer external tile).
        //   (b) obj->tile still our bucket → a FOREIGN PRESENTER nudged only the sub-tile
        //       offset — an attack/hit replay's recoil residue on a critter that then flees.
        //       walkRebucket bounds the offset to ≤1 tile so the tile check is exact:
        //       RE-ASSERT our offset (undo the residue) and keep gliding — the offset-
        //       channel analog of the fid route-don't-kill. The flee plays smoothly.
        if (!glideSuspended(e)
            && (obj->x != e.appliedX || obj->y != e.appliedY)) {
            bool repositioned = e.hops.empty() || obj->tile != e.hops.front().toTile;
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[walk] %s net=%d tile=%d hopTo=%d x=%d/%d y=%d/%d\n",
                    repositioned ? "SNAP-KILL" : "OFFSET-FOREIGN",
                    obj->netId, obj->tile,
                    e.hops.empty() ? -1 : e.hops.front().toTile,
                    obj->x, e.appliedX, obj->y, e.appliedY);
            }
            if (repositioned) {
                endGlide(e);
                eraseIfEmpty(id);
                continue;
            }
            // Foreign residue only — snap obj->x/y back to the offset the glide owns and
            // keep playing (the glide re-asserts ownership, as the fid channel does).
            _obj_offset(obj, e.appliedX - obj->x, e.appliedY - obj->y, nullptr);
        }
        // FID divergence (ROUTE, NOT KILL — PRESENTATION_FSM_DESIGN §5.5/§6 step 1). A
        // posed glide expects walkFid; an unposed held glide wears parkedFid.
        // Authoritative fids ROUTE to pendingFid (onObjectDelta → clientAnimDeferFid),
        // so a mismatch here is an UNKNOWN foreign writer. Do NOT kill (that was the
        // whack-a-mole): LOG it, and re-assert our pose so the run keeps playing. Any
        // routed/authoritative fid still lands at drain.
        int expectFid = e.posed ? e.walkFid : e.parkedFid;
        if (!glideSuspended(e) && obj->fid != expectFid) {
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[walk] FID-FOREIGN net=%d got=0x%X exp=0x%X posed=%d (routed, glide kept)\n",
                    obj->netId, obj->fid, expectFid, e.posed ? 1 : 0);
            }
            if (e.posed) {
                clientApplyPose(obj, e.walkFid); // re-assert; the run keeps animating
            }
        }

        int t256 = 0;
        if (now >= e.readyAt) {
            if (e.startedAt == 0) {
                e.startedAt = now;
                e.frameAt = now;
            }

            // Pop finished PLAYABLE hops; chain start times arithmetically (not =
            // now) so back-to-back hops don't accumulate per-hop rounding drift. A
            // held tail-hop is never popped — it waits for its queued release.
            while ((int)e.hops.size() > e.heldHops
                && now - e.startedAt >= (unsigned int)e.hops.front().durMs) {
                e.startedAt += e.hops.front().durMs;
                e.hops.pop_front();
                if ((int)e.hops.size() > e.heldHops) {
                    const Hop& next = e.hops.front();
                    objectSetRotation(obj, tileGetRotationTo(next.fromTile, next.toTile), nullptr);
                }
            }

            // A hop (or several) finished: the front hop changed, so step the render
            // bucket to the new front's toTile — obj->tile advances tile-by-tile with
            // the glide instead of sitting at the far destination (#6/#9). No-op when
            // nothing popped. Empty = fully drained; handled just below.
            if (!e.hops.empty()) {
                walkRebucket(e, e.hops.front().toTile);
            }

            if (e.hops.empty()) {
                // Glide drained: stand the critter down, THEN land the server's deferred
                // fid/facing (captured before endGlide clears them). Applying them AFTER
                // endGlide lets walkRetract's stand-down run first; objectSetRotation then
                // re-faces the stand pose (doing it before would change obj->fid and make
                // walkRetract skip the stand reset).
                bool hasPendingRot = e.hasPendingRot;
                int pendingRot = e.pendingRot;
                bool hasPendingFid = e.hasPendingFid;
                int pendingFid = e.pendingFid;
                endGlide(e); // stands the critter down (synthesized STAND); clears glide
                // If an authoritative fid arrived mid-glide it was ROUTED (clientAnimDeferFid),
                // not written through — land it now, overriding the stand pose; then the facing.
                if (hasPendingFid) {
                    clientApplyPose(obj, pendingFid);
                }
                if (hasPendingRot) {
                    objectSetRotation(obj, pendingRot, nullptr);
                }
                eraseIfEmpty(id);
                continue;
            }

            if ((int)e.hops.size() <= e.heldHops) {
                // Starved: every remaining hop is held, awaiting a queued release.
                // Park at the boundary (t256=0 of the first held hop) and force a re-clock
                // so the released hop glides from the start. Snap forward ONLY if the whole
                // presentation has been frozen (no progress anywhere) for the stall window
                // — a hold patiently waiting its turn sees recent progress and never trips.
                if (getTicksBetween(now, gProgressTick) >= kHeldStallMs) {
                    // SILENT drop path (a teleport suspect): a held glide gives up after
                    // the whole presentation froze for kHeldStallMs and snaps to authority.
                    if (getenv("F2_TRACE_EVENTS") != nullptr) {
                        fprintf(stderr, "[walk] STALL-SNAP net=%d presTile=%d authTile=%d heldHops=%d hops=%zu\n",
                            obj->netId, e.hops.front().toTile, e.hops.back().toTile,
                            e.heldHops, e.hops.size());
                    }
                    endGlide(e);
                    eraseIfEmpty(id);
                    continue;
                }
                e.startedAt = 0;
                walkApplyOffset(e, 0);
                continue;
            }
            // First PLAYABLE frame of a held glide: now (and only now) adopt the walk
            // pose + travel facing the sprite deferred while parked. `startedAt` was just
            // re-clocked by the release, so no hop has popped yet — the front hop is still
            // the one walkFid was built for, so the fid matches the tripwire.
            if (!e.posed) {
                const Hop& h = e.hops.front();
                objectSetRotation(obj, tileGetRotationTo(h.fromTile, h.toTile), nullptr);
                clientApplyPose(obj, e.walkFid);
                e.posed = true;
                e.frameAt = now;
            }
            clientAnimNotePresentationProgress(); // a playable hop is advancing = progress

            // Frame SELECTION at art fps; duration is the wire's, above.
            while (now - e.frameAt >= (unsigned int)e.ticksPerFrame) {
                objectSetNextFrame(obj, nullptr);
                e.frameAt += e.ticksPerFrame;
            }

            t256 = (int)((now - e.startedAt) * 256 / (unsigned int)e.hops.front().durMs);
            if (t256 > 256) t256 = 256;
        }

        walkApplyOffset(e, t256);
    }
}

// ============================================================================
// REPLAY advance
// ============================================================================
//
// The viewer registers the vanilla attack choreography by calling the REAL
// _action_attack with an Attack rebuilt from EVENT_ATTACK_RESULT, then lets
// _object_animate (driven from presAdvance) step the sequence with vanilla timing.
// Double-apply-safe: the damage/ammo/XP path is gated on _combat_cleanup_enabled,
// which the viewer never arms. With scripts disabled this replay driver is the ONLY
// registrar of reg_anim sequences, so completion reads straight off animationIsBusy.

void advanceReplays()
{
    if (!gEnabled) {
        return;
    }
    // (The old cross-module per-frame "suspend the participant's walk" loop is GONE —
    // suspension is now derived via glideSuspended(), read directly by the glide
    // tripwires above, which also covers a glide created after the replay started.)
    unsigned int now = getTicks();

    std::vector<int> ids;
    ids.reserve(gEntries.size());
    for (auto& kv : gEntries) {
        if (kv.second.replay != Replay::None) {
            ids.push_back(kv.first);
        }
    }

    for (int id : ids) {
        auto it = gEntries.find(id);
        if (it == gEntries.end()) {
            continue;
        }
        PresEntry& e = it->second;
        Object* obj = e.obj;

        if (e.replay == Replay::Reserved) {
            // Reserved, awaiting its replay. Do NOT resolve on idleness — no sequence is
            // registered yet, so animationIsBusy is trivially 0. Snap the held final
            // state only if the whole presentation has stalled (its attack was dropped /
            // the world was lost); a reserved entry patiently waiting for prior turns to
            // animate sees progress and holds indefinitely, correctly.
            if (getTicksBetween(now, gProgressTick) >= kReserveStallMs) {
                resolveHeld(e, /*resume=*/true);
                e.replay = Replay::None;
                eraseIfEmpty(id);
            }
            continue;
        }

        // Active.
        unsigned int capMs = e.replayCapMs != 0 ? e.replayCapMs : kReplayCapMs;
        bool capped = getTicksBetween(now, e.replaySince) >= capMs;
        if (animationIsBusy(obj) == 0 || capped) {
            if (capped && animationIsBusy(obj) != 0) {
                // The cap fired while this replay's reg_anim is STILL registered. Dropping
                // the entry without clearing it would leave animationIsBusy(obj) latched
                // true, and gate (1e) (client_net serialization) would hold every FOLLOWING
                // recorded seq for this actor forever (combatAnim=0 but the queue never
                // drains — the un-clearable wedge, Fable review A5/C.2). Clear it so the
                // serializer's ground truth is bounded to the cap.
                reg_anim_clear(obj);
            }
            // Reap ONLY this seq's frame: a completed MOVE pops the front frame it owns; a
            // completed wield/throw (activeIsMove=false) leaves the pending move frames of
            // LATER queued seqs untouched. resume=true re-baselines a still-parked held
            // approach glide to the settled (armed) pose so the pump can release it next.
            resolveHeld(e, /*resume=*/true, e.activeIsMove ? FrameReap::One : FrameReap::None);
            e.activeIsMove = false;
            if (!e.holdFrames.empty()) {
                // More recorded seqs for this actor are still queued to execute (gate (1e)
                // serialized them behind this one). Hold their frames — drop back to
                // Reserved so this Active branch won't re-reap on the idle window before the
                // next seq executes; each seq reaps its own frame when it completes.
                e.replay = Replay::Reserved;
                e.replaySince = now;
                e.replayCapMs = 0; // the next seq sets its own cap when it MarkActives
            } else {
                e.replay = Replay::None;
                eraseIfEmpty(id);
            }
        }
    }
}
} // namespace

// ============================================================================
// Public lifecycle
// ============================================================================

void presSetEnabled(bool enabled)
{
    gEnabled = enabled;
    if (!enabled) {
        gEntries.clear();
    }
}

void presReset()
{
    // The object list is being discarded (map transition / new join blob). End any
    // in-flight reg_anim (attack + door slides) so projectile transients and other
    // self-disposing objects are cleaned up before their owners are freed, THEN drop
    // every entry unapplied — the incoming baseline defines the new truth. (Glides need
    // no stop; they are pure render offsets.)
    if (gEnabled) {
        animationStop();
    }
    gEntries.clear();
}

void presStandDownAll()
{
    // Stand every in-flight glide down (retract offset + restore stand fid/frame at the
    // authoritative tile) then drop the glide concern. For combat ENTER: a critter caught
    // mid-run must not freeze wearing its running fid ([[frame-index-render-gotcha]]).
    // Replay/door concerns are left alone (combat can't overlap one on enter today).
    if (!gEnabled) {
        return;
    }
    std::vector<int> ids;
    ids.reserve(gEntries.size());
    for (auto& kv : gEntries) {
        if (!kv.second.hops.empty()) {
            ids.push_back(kv.first);
        }
    }
    for (int id : ids) {
        auto it = gEntries.find(id);
        if (it == gEntries.end()) {
            continue;
        }
        PresEntry& e = it->second;
        if (e.hops.empty()) {
            continue;
        }
        // obj->tile may be lagging mid-glide; combat enter must leave every critter
        // standing at its AUTHORITATIVE tile. Reconcile to the last decoded hop first.
        walkRebucket(e, e.hops.back().toTile);
        walkRetract(e); // stand the sprite down to its base pose first...
        // ...THEN land any authoritative fid/facing the server sent MID-GLIDE. Those were
        // routed to pendingFid/pendingRot by clientAnimDeferFid (NOT written through, or they'd
        // kill the run). Combat enter must APPLY them, not drop them — else a critter that
        // equipped a weapon while moving stands down wearing its PRE-equip pose, and its first
        // attack renders zero-frame with the new weapon (owner-found: rocket-launcher equip
        // lost on combat enter while moving). Mirrors the normal glide drain (advanceGlides
        // applies pendingFid then pendingRot after endGlide's stand-down). This is the "we
        // accept the server's response god-knows-when" bug: the response was accepted, then
        // discarded at the combat barrier.
        if (e.hasPendingFid) {
            clientApplyPose(e.obj, e.pendingFid);
        }
        if (e.hasPendingRot) {
            objectSetRotation(e.obj, e.pendingRot, nullptr);
        }
        e.hops.clear();
        e.heldHops = 0;
        e.posed = false;
        e.hasPendingFid = false;
        e.hasPendingRot = false;
        e.appliedX = 0;
        e.appliedY = 0;
        eraseIfEmpty(id);
    }
}

void presEndGlideFor(Object* obj)
{
    if (!gEnabled || obj == nullptr) {
        return;
    }
    PresEntry* self = entryFind(obj);
    if (self == nullptr || self->hops.empty()) {
        return; // nothing gliding
    }
    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        fprintf(stderr, "[walk] DEATH-END-GLIDE net=%d presTile=%d authTile=%d hops=%zu\n",
            obj->netId, self->hops.front().toTile, self->hops.back().toTile, self->hops.size());
    }
    // Land on the authoritative destination first (obj->tile lags authority mid-glide),
    // then end the glide — which zeroes the applied offset, so the corpse settles ON
    // its tile instead of a fraction of a tile off it.
    walkRebucket(*self, self->hops.back().toTile);
    endGlide(*self);
    eraseIfEmpty(obj->netId);
}

void presForgetObject(Object* obj)
{
    if (!gEnabled || obj == nullptr) {
        return;
    }
    PresEntry* self = entryFind(obj);
    if (self == nullptr) {
        return; // no presentation references obj
    }

    // Drop obj's glide concern (it must not outlive the object).
    if (!self->hops.empty()) {
        // obj->tile lags authority mid-glide; a plain drop wants it at the true decoded
        // destination first. (Same reconcile clientAnimCancel does.)
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[walk] FORGET net=%d presTile=%d authTile=%d hops=%zu\n",
                obj->netId, self->hops.front().toTile, self->hops.back().toTile, self->hops.size());
        }
        walkRebucket(*self, self->hops.back().toTile);
        endGlide(*self);
    }

    // If obj is a replay participant, its reg_anim sequence must not outlive it. End the
    // whole in-flight batch (objectDestroy does NOT clear anims), snap the SURVIVORS to
    // their held final state (the dying obj is dropped unapplied), and clear every
    // replay concern. No glide re-baseline for survivors (matches the old forgetObject's
    // plain resolve()).
    if (self->replay != Replay::None) {
        // Cancel ONLY obj's reg_anim batch, NOT the global animationStop() this used to call.
        // animationStop() ends EVERY in-flight sequence, so forgetting one object (a thrown
        // weapon picked up, a corpse, a consumed item — routine mid-combat) collateral-killed
        // every OTHER actor's walk/attack and force-drained their held frames → the drift-
        // teleport (traced: forgetting spear net=381 drained walker net=380's frame). Per-
        // object clear leaves unrelated actors' sequences and hold frames intact.
        reg_anim_clear(obj);
        std::vector<int> ids;
        ids.reserve(gEntries.size());
        for (auto& kv : gEntries) {
            if (kv.second.replay != Replay::None) {
                ids.push_back(kv.first);
            }
        }
        for (int id : ids) {
            auto it = gEntries.find(id);
            if (it == gEntries.end()) {
                continue;
            }
            PresEntry& e = it->second;
            if (e.replay == Replay::None) {
                continue;
            }
            if (e.obj != obj) {
                // A participant that shared obj's just-cleared batch (an attack's other side)
                // is now idle → snap its held final pose. One still animating is an UNRELATED
                // actor on its OWN batch (a walker) — leave its replay + hold frames alone.
                // FrameReap::None: never drain another actor's move frames from here.
                if (animationIsBusy(e.obj) != 0) {
                    continue;
                }
                resolveHeld(e, /*resume=*/false, FrameReap::None);
            } else {
                e.dHasFid = false;
                e.dHasFlags = false;
                e.dHasRot = false;
                e.holdFrames.clear(); // the dying obj's own frames die with it
                e.moveHold = false;
            }
            e.replay = Replay::None;
            eraseIfEmpty(id);
        }
    }

    eraseIfEmpty(obj->netId);
}

void presAdvance()
{
    // Step the glides (pure render offsets/frames; sim state was already applied by the
    // decoder). THEN step the attack/hit/death/door reg_anim sequences via
    // _object_animate, THEN reap completed replays, then completed door slides. The
    // §E puppet removed the _object_animate ticker, so it is driven explicitly here.
    // Order is LOAD-BEARING: the glide advance runs before _object_animate lands foreign
    // reg_anim writes (see glideSuspended's first-frame note).
    advanceGlides();
    _object_animate();
    advanceReplays();
}

// ============================================================================
// Public: glide (GLIDE)
// ============================================================================

void clientAnimOnMove(Object* obj, int fromTile, int toTile, int fromElevation, int toElevation, int durMs, bool hold, bool run)
{
    if (!gEnabled || obj == nullptr) {
        return;
    }

    PresEntry* ep = entryFind(obj);
    bool hasGlide = ep != nullptr && !ep->hops.empty();

    // Snap cases: teleport/scripted placement (durMs 0), elevation change, or a
    // non-adjacent "hop" (must not exist per §2 — treat as teleport if it does).
    // objectSetLocation already put the object exactly where authority says; just drop
    // any in-flight glide.
    if (durMs <= 0 || fromElevation != toElevation
        || tileDistanceBetween(fromTile, toTile) != 1
        || FID_TYPE(obj->fid) != OBJ_TYPE_CRITTER) {
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[walk] SNAP net=%d dur=%d dElev=%d dist=%d type=%d\n",
                obj->netId, durMs, (fromElevation != toElevation) ? 1 : 0,
                tileDistanceBetween(fromTile, toTile), FID_TYPE(obj->fid));
        }
        if (hasGlide) {
            endGlide(*ep);
            eraseIfEmpty(obj->netId);
        }
        return;
    }

    if (hasGlide) {
        PresEntry& e = *ep;
        // A burst (this push shares the last push's advance-generation, i.e. no
        // advanceGlides ran between them) is combat choreography — allow the larger cap.
        // A cross-generation push is a trickle that outran playback: the lag cap stands
        // and a deeper queue snaps forward.
        bool burst = e.lastPushGen == gAdvanceGen;
        size_t cap = burst ? kMaxBurstHops : kMaxQueuedHops;
        if (e.elevation != toElevation || e.hops.size() >= cap) {
            // Behind the wire (or inconsistent): snap forward.
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[walk] CAP-ERASE net=%d hops=%zu cap=%zu burst=%d\n",
                    obj->netId, e.hops.size(), cap, burst ? 1 : 0);
            }
            endGlide(e);
            eraseIfEmpty(obj->netId);
            return;
        }
        e.lastPushGen = gAdvanceGen;
        e.hops.push_back({ fromTile, toTile, durMs });
        if (hold) {
            e.heldHops++; // appended at the tail — the held region only grows there
        }
        // The decoder jumped obj->tile to this new hop's FAR destination (state first)
        // and zeroed obj->x/y. Re-bucket back to the hop we are still PLAYING (the
        // unchanged front) so the sprite z-sorts where it is gliding, not where it is
        // headed; then re-anchor the pixel offset at its current playback position.
        walkRebucket(e, e.hops.front().toTile);
        e.appliedX = 0;
        e.appliedY = 0;
        int t256 = 0;
        if (e.startedAt != 0) {
            t256 = (int)((getTicks() - e.startedAt) * 256 / (unsigned int)e.hops.front().durMs);
            if (t256 > 256) t256 = 256;
        }
        walkApplyOffset(e, t256);
        return;
    }

    // Fresh glide. run/walk is AUTHORITATIVE — it rides EVENT_MOVE from the server,
    // which is the only place that actually knows. It used to be inferred here as
    // `durMs <= kRunThresholdMs`, but durMs is wall-clock pacing, so the animation
    // became a function of server load: critters at different speeds fell on
    // opposite sides of one constant ("some guards run, some walk") and an idle
    // wanderer drawn with the run cycle read as panic.
    //
    // The artExists fallback below STAYS. That one is not a guess: plenty of
    // critters ship no run frames at all, and walking them is correct.
    int anim = run ? ANIM_RUNNING : ANIM_WALK;
    int rotation = tileGetRotationTo(fromTile, toTile);
    int fid = animFid(obj, anim, rotation);
    if (anim == ANIM_RUNNING && !artExists(fid)) {
        anim = ANIM_WALK;
        fid = animFid(obj, ANIM_WALK, rotation);
    }
    if (!artExists(fid)) {
        return;
    }

    int fps = 10;
    CacheEntry* artKey;
    Art* art = artLock(fid, &artKey);
    if (art != nullptr) {
        fps = artGetFramesPerSecond(art);
        artUnlock(artKey);
    }

    // Get-or-create the entry (it may already exist for a REPLAY/DOOR concern — this
    // populates its glide fields without touching those). Reset every glide field so a
    // stale value from a prior drained glide on the same entry can't leak in.
    PresEntry& e = gEntries[obj->netId];
    e.obj = obj;
    e.hops.clear();
    e.hops.push_back({ fromTile, toTile, durMs });
    e.startedAt = 0;
    e.readyAt = getTicks() + kStartDelayMs;
    e.frameAt = 0;
    e.ticksPerFrame = fps > 0 ? 1000 / fps : 100;
    e.walkFid = fid;
    e.elevation = toElevation;
    e.lastPushGen = gAdvanceGen;
    e.heldHops = hold ? 1 : 0;
    e.posed = !hold;
    e.parkedFid = obj->fid; // the stand fid worn while held/unposed (tripwire baseline)
    e.hasPendingFid = false;
    e.hasPendingRot = false;
    e.appliedX = 0;
    e.appliedY = 0;

    // A held glide does NOT touch the pose yet — it must stay in its origin stand fid +
    // facing until the pump releases it (posed at first playable advance), else it snaps
    // to face its destination while parked. An out-of-combat glide poses immediately.
    if (!hold) {
        objectSetRotation(obj, rotation, nullptr);
        clientApplyPose(obj, fid);
    }

    // Park the sprite at the hop origin immediately so the frame rendered between now
    // and the first advance doesn't flash the snapped position.
    walkApplyOffset(e, 0);
    debugPrint("client_present: glide start net=%d fid=0x%X anim=%d dur=%d\n", obj->netId, fid, anim, durMs);
}

void clientAnimCancel(Object* obj)
{
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->hops.empty()) {
        return;
    }
    // SILENT drop path (a teleport suspect): an attack pose takes over, or an
    // authoritative reposition (onConnect) / backlog overflow drops the glide. A large
    // presTile→authTile gap here = a visible warp; a 1-hop gap = benign attack-takeover.
    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        fprintf(stderr, "[walk] CANCEL net=%d presTile=%d authTile=%d hops=%zu heldHops=%d posed=%d\n",
            obj->netId, e->hops.front().toTile, e->hops.back().toTile,
            e->hops.size(), e->heldHops, e->posed ? 1 : 0);
    }
    // obj->tile LAGS authority mid-glide (walkRebucket steps it hop-by-hop). The callers
    // — an attack pose taking over, an authoritative reposition, a backlog snap — want
    // the object at its true decoded destination. Reconcile to the last decoded hop's
    // toTile before dropping. (SNAP-KILL / CAP-ERASE erase without this: an external
    // objectSetLocation already set the newer authoritative tile.)
    walkRebucket(*e, e->hops.back().toTile);
    endGlide(*e);
    eraseIfEmpty(obj->netId);
}

void clientApplyPose(Object* obj, int fid)
{
    // The ONE render-channel write helper (PRESENTATION_FSM_DESIGN §4.3). objectSetFid
    // never resets obj->frame; a stale high frame index on the new art renders the wrong
    // frame or NOTHING ([[frame-index-render-gotcha]]). Every fid write on a synced sprite
    // must go through here so the frame reset can never be forgotten.
    objectSetFid(obj, fid, nullptr);
    objectSetFrame(obj, 0, nullptr);
}

bool clientAnimDeferFid(Object* obj, int fid)
{
    // Route an authoritative fid onto an in-flight glide (applied at drain) instead of
    // letting the decoder write it straight to obj->fid, which would trip the mid-glide
    // fid check and kill the run. Returns false (→ caller writes through) if no glide.
    PresEntry* e = entryFind(obj);
    if (!gEnabled || e == nullptr || e->hops.empty()) {
        return false;
    }
    e->hasPendingFid = true;
    e->pendingFid = fid;
    return true;
}

bool clientAnimDeferRotation(Object* obj, int rotation)
{
    PresEntry* e = entryFind(obj);
    if (!gEnabled || e == nullptr || e->hops.empty()) {
        return false;
    }
    e->hasPendingRot = true;
    e->pendingRot = rotation;
    return true;
}

bool clientAnimActiveFor(Object* obj)
{
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    return e != nullptr && !e->hops.empty();
}

bool clientAnimPlayableActiveFor(Object* obj)
{
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    return e != nullptr && (int)e->hops.size() > e->heldHops;
}

bool clientAnimAnyPlayableActive()
{
    if (!gEnabled) {
        return false;
    }
    for (const auto& kv : gEntries) {
        if ((int)kv.second.hops.size() > kv.second.heldHops) {
            return true;
        }
    }
    return false;
}

int clientAnimHopsRemaining(Object* obj)
{
    if (!gEnabled) {
        return -1;
    }
    PresEntry* e = entryFind(obj);
    return (e == nullptr || e->hops.empty()) ? -1 : (int)e->hops.size();
}

// Release up to `hops` held tail-hops of obj's glide for playback (a queued
// kMoveRelease reached the front of the presentation pump). Un-starve re-clocks so the
// resumed segment glides fresh rather than snapping through skipped time.
void clientAnimRelease(Object* obj, int hops)
{
    if (!gEnabled || hops <= 0) {
        return;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->hops.empty()) {
        return;
    }
    bool wasStarved = (int)e->hops.size() <= e->heldHops;
    e->heldHops -= hops;
    if (e->heldHops < 0) {
        e->heldHops = 0;
    }
    if (wasStarved && (int)e->hops.size() > e->heldHops) {
        e->startedAt = 0;
        e->readyAt = getTicks();
    }
}

// Release every hold. The pump calls this whenever the presentation queue is empty and
// no attack is animating: a hold with nothing left to order against would be a bug, so
// the system self-heals to "play everything" each such frame.
void clientAnimReleaseAll()
{
    for (auto& kv : gEntries) {
        PresEntry& e = kv.second;
        if (e.hops.empty() || e.heldHops <= 0) {
            continue;
        }
        bool wasStarved = (int)e.hops.size() <= e.heldHops;
        e.heldHops = 0;
        if (wasStarved) {
            e.startedAt = 0;
            e.readyAt = getTicks();
        }
    }
}

void clientAnimNotePresentationProgress()
{
    gProgressTick = getTicks();
}

unsigned int clientAnimLastProgressTick()
{
    return gProgressTick;
}

// ============================================================================
// Public: combat replay (REPLAY)
// ============================================================================

bool clientCombatAnimActive()
{
    if (!gEnabled) {
        return false;
    }
    // Only ACTIVE replays count — a reserved-but-not-yet-played entry must NOT block the
    // presentation pump (the pump has to reach and start that attack). Active entries
    // include weapon-draw replays (kTakeOut routes through the same machinery), so the
    // pump blocks a following shot until the draw finishes.
    for (const auto& kv : gEntries) {
        if (kv.second.replay == Replay::Active) {
            return true;
        }
    }
    return false;
}

void clientCombatAnimReserve(Object* obj)
{
    if (!gEnabled || obj == nullptr) {
        return;
    }
    // Take obj under deferral BEFORE its replay plays (ATTACK_RESULT / TAKE_OUT decode).
    // Idempotent: an existing reserved/active entry keeps its held state (only a fresh
    // reserve initializes the held bucket). Coexists with a live glide on the same entry.
    PresEntry& e = gEntries[obj->netId];
    e.obj = obj;
    if (e.replay == Replay::None) {
        e.replay = Replay::Reserved;
        e.replaySince = getTicks();
        e.dHasFid = false;
        e.dHasFlags = false;
        e.dHasRot = false;
    }
}

void clientCombatAnimMarkActive(Object* obj, unsigned int capMs, bool ownsMoveFrame)
{
    // Promote a recorded-seq participant to Active at PLAY time (the generic recorded-seq
    // interpreter does not, unlike clientCombatAnimPlay/TakeOut/ActionAnim). Used by the
    // in-combat MOVE ops so the walk (1) holds the pump via clientCombatAnimActive() — the
    // next turn event waits out the walk — and (2) resolves the mover's held deltas
    // (position, AP, face-target rotation) on completion via advanceReplays, instead of only
    // on the reserve stall backstop. capMs raises the per-entry force-resolve cap above the
    // generic kReplayCapMs so a multi-tile walk isn't killed mid-stride (0 = keep generic).
    // ownsMoveFrame=true (the MOVE ops) claims the front FIFO frame for this activation, so
    // this seq's completion reaps its own endpoint and not a later queued seq's.
    if (!gEnabled || obj == nullptr) {
        return;
    }
    enterReplay(obj, getTicks());
    PresEntry* e = entryFind(obj);
    if (e != nullptr) {
        if (capMs != 0) {
            e->replayCapMs = capMs;
        }
        if (ownsMoveFrame) {
            e->activeIsMove = true;
        }
    }
}

void clientCombatAnimArmMoveHold(Object* obj)
{
    // At the recorded MOVE seq's DECODE: reserve the mover (holds its same-beat fid/rot
    // deltas) AND push a fresh per-seq hold frame, so onMove / the AP delta DEFER this
    // seq's authoritative position + AP into its OWN frame until the replayed walk completes
    // (COMBAT_MOVE_RECORD_DESIGN.md + Fable review C.1). VIEWER-only; tolerant of a null/
    // unknown mover. One frame per MOVE seq — decode-order == execute-order (gate (1e)).
    if (!gEnabled || obj == nullptr) {
        return;
    }
    clientCombatAnimReserve(obj);
    PresEntry* e = entryFind(obj);
    if (e != nullptr) {
        e->holdFrames.emplace_back();
        e->moveHold = true;
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[frame-push] net=%d frames=%d\n", obj->netId, (int)e->holdFrames.size());
        }
    }
}

bool clientCombatAnimDeferMove(Object* obj, int tile, int elev)
{
    // Hold the mover's authoritative MOVE into the CURRENTLY-decoding seq's frame (the back
    // of the FIFO; the recorded walk owns the motion, applied at resolveHeld). Last write
    // wins WITHIN a seq — its multiple authoritative hops collapse to the final tile. False
    // unless the mover is a reserved+armed recorded-walk participant.
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->replay == Replay::None || !e->moveHold || e->holdFrames.empty()) {
        return false;
    }
    MoveHoldFrame& f = e->holdFrames.back();
    f.hasPos = true;
    f.tile = tile;
    f.elev = elev;
    return true;
}

bool clientCombatAnimDeferSnapMove(Object* obj, int tile, int elev)
{
    // Hold a durMs<=0 authoritative MOVE (a knockback snap) when this object has a LIVE
    // replay — i.e. the coupled slide that animates this displacement is reserved/playing.
    // Committed at resolveHeld (the slide's completion / action frame) so the slide runs
    // from the true origin instead of the client snapping ahead of it (bug J). Returns
    // false — snap as today — when there is no replay entry, which bounds the blast radius
    // to combat displacements that actually have a coupled sequence. Last write wins (one
    // knockback per victim). NOT for durMs>0 stepped hops (those ride the glide/holdFrames
    // path) — the caller gates on durMs<=0.
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->replay == Replay::None) {
        return false;
    }
    // Do NOT hold an object that is running its OWN recorded walk — its authoritative
    // position is already owned by the holdFrames reconcile FIFO. A knockback victim is
    // PASSIVELY displaced and never mid-walk; the local player's own in-combat move IS
    // mid-walk, and holding its snap here lets dHasSnapPos overwrite the walk's committed
    // destination with a stale tile at resolveHeld → the sprite snaps back to the pre-move
    // tile while the server (and NPC targeting) has it at the destination (owner-observed
    // 2026-07-22 regression). Recorded walks snap/reconcile through their own path.
    if (e->moveHold || !e->holdFrames.empty()) {
        return false;
    }
    e->dHasSnapPos = true;
    e->dSnapTile = tile;
    e->dSnapElev = elev;
    return true;
}

int clientCombatAnimHeldMoveTile(Object* obj)
{
    // The tile the server's walk ACTUALLY reached for the currently-executing move seq (the
    // front FIFO frame's held position). The MOVE replay targets this instead of the recorded
    // intent dest so an approach toward an occupied tile (e.g. the guard running onto the
    // dude's tile to throw) doesn't pathfind to nothing and snap-teleport. -1 = no held
    // position (fall back to the recorded dest).
    if (!gEnabled || obj == nullptr) {
        return -1;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->holdFrames.empty()) {
        return -1;
    }
    const MoveHoldFrame& f = e->holdFrames.front();
    return f.hasPos ? f.tile : -1;
}

bool clientCombatAnimDeferAp(Object* obj, int ap)
{
    // Hold the mover's authoritative AP into the currently-decoding seq's frame so the client
    // re-walks the identical tiles (the real engine charges AP per step from the PRE-walk
    // pool). Applied at resolveHeld.
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->replay == Replay::None || !e->moveHold || e->holdFrames.empty()) {
        return false;
    }
    MoveHoldFrame& f = e->holdFrames.back();
    f.hasAp = true;
    f.ap = ap;
    return true;
}

bool clientCombatAnimDeferDelta(Object* obj, bool hasFid, int fid, bool hasFlags, unsigned int flags,
    bool hasRot, int rot)
{
    if (!gEnabled) {
        return false;
    }
    PresEntry* e = entryFind(obj);
    if (e == nullptr || e->replay == Replay::None) {
        return false;
    }
    if (hasFid) {
        e->dHasFid = true;
        e->dFid = fid;
    }
    if (hasFlags) {
        e->dHasFlags = true;
        e->dFlags = flags;
    }
    if (hasRot) {
        e->dHasRot = true;
        e->dRot = rot;
    }
    return true;
}

void clientCombatAnimNotifyReposition(Object* obj)
{
    if (!gEnabled) {
        return;
    }
    PresEntry* e = entryFind(obj);
    if (e != nullptr && e->replay != Replay::None) {
        // The MOVE already placed obj authoritatively; stop holding its pose. resume=false
        // matches the old plain resolve() here (a knockback MOVE just built a fresh glide;
        // do not re-baseline it).
        resolveHeld(*e, /*resume=*/false);
        e->replay = Replay::None;
        eraseIfEmpty(obj->netId);
    }
}

void clientCombatAnimPlay(Attack* attack)
{
    if (!gEnabled || attack == nullptr || attack->attacker == nullptr || attack->defender == nullptr) {
        return;
    }

    // v1 SKIP — thrown weapons (grenades / knives / spears). _action_ranged's throw
    // branch mutates the attacker's inventory synchronously OUTSIDE the double-apply gate,
    // which would consume the item from the mirror; it also spawns the multi-object
    // explosion cluster. The server's inventory + damage deltas carry the real outcome;
    // the throw animation is a banked v1 gap. Guarding here means the hazardous swap block
    // is never reached on the viewer.
    if (critterGetAnimationForHitMode(attack->attacker, attack->hitMode) == ANIM_THROW_ANIM) {
        // We reserved these participants at decode; their replay is skipped, so release
        // the holds now (final state lands via the server's deltas) rather than stranding
        // them until the reserve cap.
        resolveReservedNow(attack->attacker);
        resolveReservedNow(attack->defender);
        for (int i = 0; i < attack->extrasLength; i++) {
            resolveReservedNow(attack->extras[i]);
        }
        return;
    }

    unsigned int now = getTicks();

    // A participant PLAYING a walk-glide (posed, actively moving) that now fights: the
    // attack pose is authoritative, so drop the glide or its fid/offset tripwires fight
    // the sequence this registers. But a HELD/parked walk is a FUTURE move not yet
    // presented — most importantly a defender's FLEE queued in the same beat it is hit.
    // Cancelling that snaps it to the flee destination before the hit even plays (= the
    // #9 teleport-on-hit). Leave it: enterReplay below marks the participant Active, so
    // glideSuspended() suspends the parked glide while the hit plays IN PLACE, and its
    // queued kMoveRelease then releases it to glide AFTER — the vanilla hit-then-flee order.
    if (clientAnimPlayableActiveFor(attack->attacker)) {
        clientAnimCancel(attack->attacker);
    }
    if (clientAnimPlayableActiveFor(attack->defender)) {
        clientAnimCancel(attack->defender);
    }
    enterReplay(attack->attacker, now);
    enterReplay(attack->defender, now);
    for (int i = 0; i < attack->extrasLength; i++) {
        if (clientAnimPlayableActiveFor(attack->extras[i])) {
            clientAnimCancel(attack->extras[i]);
        }
        enterReplay(attack->extras[i], now);
    }

    // Arm the attacker's fid to match its weapon BEFORE registering the attack.
    // _action_attack builds the attack animation from the weapon code embedded in the
    // attacker's CURRENT fid, NOT from attack->weapon. A critter that wielded its weapon
    // mid-fight has its armed-pose fid delta DEFERRED here (it is a reserved participant
    // this same beat), so it is still unarmed — the fire fid would resolve to
    // body+FIRE+unarmed, which has no art and renders NOTHING. Set the armed stand fid.
    // Unarmed (weapon == null) → code 0 → unarmed stand, so melee/punch is unchanged.
    int weaponCode = (attack->weapon != nullptr && itemGetType(attack->weapon) == ITEM_TYPE_WEAPON)
        ? weaponGetAnimationCode(attack->weapon)
        : 0;
    objectSetFid(attack->attacker,
        buildFid(OBJ_TYPE_CRITTER, attack->attacker->fid & 0xFFF, ANIM_STAND, weaponCode, attack->attacker->rotation + 1),
        nullptr);
    objectSetFrame(attack->attacker, 0, nullptr);

    // Register the vanilla melee/ranged sequence. This only builds + starts the reg_anim
    // descriptions (advancing happens in _object_animate per frame); it never applies
    // damage on the viewer (cleanup gate is disarmed).
    _action_attack(attack);
    debugPrint("client_present: replay attacker=%d defender=%d hitMode=%d dFlags=0x%X\n",
        attack->attacker->netId, attack->defender->netId, attack->hitMode, attack->defenderFlags);
}} // namespace fallout
