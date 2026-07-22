#include "pres_record.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>
#include <vector>

#include "actions.h" // actionShowDeathCallbackPtr — the one RECORD callback in v1
#include "obj_types.h"
#include "object.h" // objectGetNextNetId — the created-this-beat watermark
#include "random.h" // RandomState / randomSnapshot / randomRestore

namespace fallout {

// ---- section + buffer state (single-threaded server) ----------------------

static bool gActive = false;
static std::vector<unsigned char> gBuf;
static int gOpCount = 0;
static RandomState gRngSnapshot;

// Per-section transient handle assignment. A record op that references an object
// created THIS action (a transient — explosion cloud, projectile) cannot ship its
// netId: the object is destroyed the same beat, so the netId is dead by the time
// the viewer pumps the stream. Instead the first reference mints a negative
// stream-scoped handle and lazily emits OBJ_CREATE; the interpreter recreates its
// own object under that handle.
//
// DISCRIMINATOR = created-this-BEAT (netId >= the beat watermark) AND OBJECT_NO_SAVE.
// netIds are minted monotonically (objectNextNetId, object.cc:4497) and renumbered
// LOW for persistent objects at every rebaseline (gDude = netId 1). The watermark is
// snapshotted at BEAT start (serverTick top, presRecordBeatBegin) — BEFORE the
// composite's build phase creates its transients — so a transient a composite makes
// before opening its reg_anim section still lands at/after it. (Snapshotting at
// SectionBegin was too late: composites create clouds/synthetic-attacker up front,
// so those fell BELOW a section watermark and shipped by an unresolvable netId — the
// "explosion clouds vanished" regression.) The NO_SAVE conjunct is what excludes the
// persistent NO_SAVE singletons (gDude/gEgg, object.cc:316) AND a same-beat-spawned
// PERSISTENT critter (replicated → resolves by netId), so neither is mis-minted as a
// duplicate. Both signals are required; see presenter-object-ref-model.
static std::unordered_map<Object*, int> gHandles;
static int gNextHandle = -1;
static int gBeatNetIdWatermark = 0; // objectGetNextNetId() at beat start (serverTick top)

static bool gBackendActive = false;

void presRecordSetBackendActive(bool active)
{
    gBackendActive = active;
}

bool presRecordEnabled()
{
    static bool env = getenv("F2_SERVER_PRES_RECORD") != nullptr;
    // Backend gate: only the server_anim.cc recording backend may record. In the
    // animation.cc backend (fallout2-ce) the leaves execute for real, so recording
    // there double-frees `attack` via the explosion callback + the fast-path.
    return gBackendActive && env;
}

bool presRecordActive()
{
    return gActive;
}

// ---- LE encoding primitives (match presenter_network putI32 / client Reader) --

static void appendU8(unsigned char v)
{
    gBuf.push_back(v);
}

static void appendI32(int v)
{
    unsigned int u = (unsigned int)v;
    gBuf.push_back((unsigned char)(u & 0xFF));
    gBuf.push_back((unsigned char)((u >> 8) & 0xFF));
    gBuf.push_back((unsigned char)((u >> 16) & 0xFF));
    gBuf.push_back((unsigned char)((u >> 24) & 0xFF));
}

static void appendStr(const char* text)
{
    size_t length = text != nullptr ? strlen(text) : 0;
    if (length > 0xFFFF) {
        length = 0xFFFF;
    }
    gBuf.push_back((unsigned char)(length & 0xFF));
    gBuf.push_back((unsigned char)((length >> 8) & 0xFF));
    if (length != 0) {
        gBuf.insert(gBuf.end(), (const unsigned char*)text, (const unsigned char*)text + length);
    }
}

static void beginOp(unsigned char op)
{
    appendU8(op);
    gOpCount++;
}

// Resolve an Object* to a wire ref: 0 = null; negative = stream handle (emits a
// lazy OBJ_CREATE the first time); positive = netId. MUST run before the op tag it
// belongs to so a needed OBJ_CREATE precedes the referencing op in the stream.
// A transient that should ALSO be addressable by a real netId on the viewer (a thrown
// weapon: its flight is a NO_SAVE transient, but the STATE lane later connect/disconnects
// the real weapon by netId — so the viewer must map that netId onto this transient, else
// the pickup's disconnect can't remove it and the spear phantoms). Set by the throw before
// the projectile is first referenced; consumed when its OBJ_CREATE mints.
static Object* gAdoptObj = nullptr;
static int gAdoptNetId = 0;

void presRecordSetAdoptNetId(Object* obj, int netId)
{
    gAdoptObj = obj;
    gAdoptNetId = netId;
}

static int resolveRef(Object* obj)
{
    if (obj == nullptr) {
        return 0;
    }
    // Transient (→ OBJ_CREATE) iff the viewer cannot resolve this ref by netId:
    // either unaddressable (netId 0), OR it is BOTH created-this-beat (netId minted
    // at/after the beat watermark → its netId is dead by the time the viewer pumps
    // this stream) AND OBJECT_NO_SAVE (never replicated to the viewer, so no spawn
    // gave it a netId there). A same-beat-created PERSISTENT object (not NO_SAVE) is
    // replicated via the state lane and DOES resolve by netId — it must NOT be
    // re-minted as a duplicate transient (the gDude/same-beat-spawn hazard). See
    // presenter-object-ref-model: NO_SAVE alone mis-caught gDude; the watermark
    // alone missed pre-section clouds (they are created before SectionBegin). Both
    // signals together are exact.
    bool transient = obj->netId == 0
        || (obj->netId >= gBeatNetIdWatermark && (obj->flags & OBJECT_NO_SAVE) != 0);
    if (transient) {
        auto it = gHandles.find(obj);
        if (it != gHandles.end()) {
            return it->second;
        }
        int handle = gNextHandle--;
        gHandles[obj] = handle;
        beginOp(PRES_OP_OBJ_CREATE);
        appendI32(handle);
        appendI32(obj->fid);
        appendI32(obj->tile);
        appendI32(obj->elevation);
        appendI32((int)obj->flags);
        // Rotation: a transient's facing is set by a DIRECT objectSetRotation (not a
        // reg_anim leaf), so it would otherwise be lost — e.g. a ranged projectile
        // (actions.cc:935 sets it toward the target BEFORE the first leaf references it,
        // so obj->rotation is correct here). Without it the viewer spawns the rocket at
        // rotation 0 (facing "up"). Part of the OBJ_CREATE pose (stream v2).
        appendI32(obj->rotation);
        // Adopt netId (stream v4): 0 = pure transient; >0 = the viewer ALSO registers this
        // transient under this real netId so a later state-lane connect/disconnect (a thrown
        // weapon becoming / leaving a ground item) targets it. Consumed here (once per mint).
        int adopt = (obj == gAdoptObj) ? gAdoptNetId : 0;
        appendI32(adopt);
        if (obj == gAdoptObj) {
            gAdoptObj = nullptr;
            gAdoptNetId = 0;
        }
        return handle;
    }
    return obj->netId;
}

// ---- section lifecycle ----------------------------------------------------

// Snapshot the created-this-beat watermark. Called at BEAT start (serverTick top,
// f2_server only) BEFORE any composite's build phase runs — objects minted from here
// through this beat are "created-this-beat" for resolveRef. Snapshotting here rather
// than at SectionBegin is what lets a composite's pre-section transients (explosion
// clouds, the actionDamage synthetic attacker) resolve as OBJ_CREATE. Inert in the
// client/golden binary (never called there → watermark stays 0 → but resolveRef only
// runs while recording, which never happens there anyway).
void presRecordBeatBegin()
{
    gBeatNetIdWatermark = objectGetNextNetId();
}

void presRecordSectionBegin()
{
    if (gActive) {
        fprintf(stderr, "[presrec] WARN nested SectionBegin ignored\n");
        return;
    }
    gActive = true;
    gBuf.clear();
    gOpCount = 0;
    gHandles.clear();
    gNextHandle = -1;
    gAdoptObj = nullptr; // never carry an adopt-netId across sections
    gAdoptNetId = 0;
    randomSnapshot(&gRngSnapshot);
}

void presRecordSectionEnd()
{
    if (!gActive) {
        return;
    }
    gActive = false;
    // Rewind the authoritative RNG: the animate branch's cosmetic rolls (gib pick,
    // fire-dance fling) must leave the sim's stream position untouched.
    randomRestore(&gRngSnapshot);
}

void presRecordSectionAbort()
{
    if (!gActive) {
        return;
    }
    gActive = false;
    randomRestore(&gRngSnapshot);
    gBuf.clear();
    gOpCount = 0;
}

// Ambient section: identical to SectionBegin/End MINUS the RNG snapshot/restore.
// Used by the in-combat MOVE bracket (combat_ai / combat_drain composites). Unlike the
// attack's animate branch — which the flag-off server SKIPS, so its cosmetic rolls must
// be rewound — the move bracket already runs on the flag-off server (it applies the
// authoritative walk) AND contains AUTHORITATIVE rolls (_combatai_msg taunt selection,
// randomBetween in combat_ai.cc). Snapshot/restore here would rewind rolls the flag-off
// path consumes → the record-purity differential (flag-off vs flag-on) would diverge.
// Spec §5.3 context 2 ("ambient recording … no RNG section needed"). Shares gActive /
// gBuf / gOpCount / gHandles with the ordinary section (same nested-begin refusal).
void presRecordAmbientBegin()
{
    if (gActive) {
        fprintf(stderr, "[presrec] WARN nested AmbientBegin ignored\n");
        return;
    }
    gActive = true;
    gBuf.clear();
    gOpCount = 0;
    gHandles.clear();
    gNextHandle = -1;
}

void presRecordAmbientEnd()
{
    gActive = false;
}

// Deferred authoritative-state commit hook. The in-combat MOVE leaf records its op and
// STASHES the authoritative walk (server_anim.cc); the composite site ships the presSeq
// FIRST, then calls presRecordCommitDeferred() to apply the walk — so the presentation
// event precedes its state on the wire (spec §6.3; attack precedent combat.cc:4142 before
// :4170). The hook lives in the recording backend (server_anim.cc, f2_server only) but is
// invoked from f2_core composites that cannot link server_anim symbols; armed at static
// init like the backend armer. Null in the client/golden binary → no-op.
static void (*gDeferredCommitHook)() = nullptr;

void presRecordSetDeferredCommitHook(void (*hook)())
{
    gDeferredCommitHook = hook;
}

void presRecordCommitDeferred()
{
    if (gDeferredCommitHook != nullptr) {
        gDeferredCommitHook();
    }
}

const unsigned char* presRecordData()
{
    return gBuf.empty() ? nullptr : gBuf.data();
}

int presRecordSize()
{
    return (int)gBuf.size();
}

int presRecordOpCount()
{
    return gOpCount;
}

// ---- leaf emitters --------------------------------------------------------

void presRecordSeqBegin(int flags)
{
    beginOp(PRES_OP_SEQ_BEGIN);
    appendI32(flags);
}

void presRecordSeqEnd()
{
    beginOp(PRES_OP_SEQ_END);
}

void presRecordPriority(int n)
{
    beginOp(PRES_OP_PRIORITY);
    appendI32(n);
}

static void recordAnimLike(unsigned char op, Object* owner, int anim, int delay)
{
    int ref = resolveRef(owner);
    beginOp(op);
    appendI32(ref);
    appendI32(anim);
    appendI32(delay);
}

void presRecordAnimate(Object* owner, int anim, int delay) { recordAnimLike(PRES_OP_ANIMATE, owner, anim, delay); }
void presRecordAnimateReversed(Object* owner, int anim, int delay) { recordAnimLike(PRES_OP_ANIMATE_REV, owner, anim, delay); }
void presRecordAnimateForever(Object* owner, int anim, int delay) { recordAnimLike(PRES_OP_ANIMATE_FOREVER, owner, anim, delay); }
void presRecordAnimateAndHide(Object* owner, int anim, int delay) { recordAnimLike(PRES_OP_ANIMATE_AND_HIDE, owner, anim, delay); }

void presRecordHideForced(Object* owner)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_HIDE_FORCED);
    appendI32(ref);
}

void presRecordSetFid(Object* owner, int fid, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_SET_FID);
    appendI32(ref);
    appendI32(fid);
    appendI32(delay);
}

void presRecordRotate(Object* owner, int tile)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_ROTATE);
    appendI32(ref);
    appendI32(tile);
}

void presRecordUnsetFlag(Object* owner, int flag, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_UNSET_FLAG);
    appendI32(ref);
    appendI32(flag);
    appendI32(delay);
}

static void recordMoveStraight(unsigned char op, Object* owner, int tile, int elev, int anim, int delay)
{
    int ref = resolveRef(owner);
    beginOp(op);
    appendI32(ref);
    appendI32(tile);
    appendI32(elev);
    appendI32(anim);
    appendI32(delay);
}

void presRecordMoveStraight(Object* owner, int tile, int elev, int anim, int delay) { recordMoveStraight(PRES_OP_MOVE_STRAIGHT, owner, tile, elev, anim, delay); }
void presRecordMoveStraightWait(Object* owner, int tile, int elev, int anim, int delay) { recordMoveStraight(PRES_OP_MOVE_STRAIGHT_WAIT, owner, tile, elev, anim, delay); }
// Pathed in-combat walk to a TILE — records the RunTo/MoveTo leaf's REAL args (dest,
// elevation, actionPoints, run/walk); the viewer replays the same registration through its
// own animationRegisterRunToTile/MoveToTile. anim = ANIM_RUNNING/ANIM_WALK (which leaf).
void presRecordMoveToTile(Object* owner, int tile, int elev, int anim, int actionPoints, int preWalkAp, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_MOVE_TO_TILE);
    appendI32(ref);
    appendI32(tile);
    appendI32(elev);
    appendI32(anim);
    appendI32(actionPoints);
    appendI32(preWalkAp);
    appendI32(delay);
}

// Pathed in-combat walk to an OBJECT (multihex-aware; the real registrar appends the
// trailing RotateToTile itself). Records both refs + the leaf's real args.
void presRecordMoveToObject(Object* owner, Object* target, int anim, int actionPoints, int preWalkAp, int delay)
{
    int ref = resolveRef(owner);
    int targetRef = resolveRef(target);
    beginOp(PRES_OP_MOVE_TO_OBJ);
    appendI32(ref);
    appendI32(targetRef);
    appendI32(anim);
    appendI32(actionPoints);
    appendI32(preWalkAp);
    appendI32(delay);
}

void presRecordSfx(Object* owner, const char* name, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_SFX);
    appendI32(ref);
    appendStr(name);
    appendI32(delay);
}

void presRecordSetLight(Object* owner, int dist, int intensity, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_SET_LIGHT);
    appendI32(ref);
    appendI32(dist);
    appendI32(intensity);
    appendI32(delay);
}

void presRecordTakeOut(Object* owner, int weaponAnimCode, int delay)
{
    int ref = resolveRef(owner);
    beginOp(PRES_OP_TAKE_OUT);
    appendI32(ref);
    appendI32(weaponAnimCode);
    appendI32(delay);
}

void presRecordPing(int flags, int delay)
{
    beginOp(PRES_OP_PING);
    appendI32(flags);
    appendI32(delay);
}

void presRecordCallback(void* a1, void* a2, void* proc)
{
    // Defunctionalize: map the C function pointer to a wire tag. v1 records only
    // _show_death (the corpse presentation); every other callback is a state
    // outcome the server fast-path already applied (DROP) — record nothing.
    if (proc == actionShowDeathCallbackPtr()) {
        int ref = resolveRef((Object*)a1);
        int anim = (int)(intptr_t)a2; // a2 is (void*)anim at the registration site
        beginOp(PRES_OP_CALL);
        appendU8(PRES_CB_SHOW_DEATH);
        appendI32(ref);
        appendI32(anim);
    } else if (proc == actionHideProjectileCallbackPtr()) {
        // Ranged projectile hide (actions.cc:1077 registers
        // animationRegisterCallbackForced(attack, projectile, hideProjectile, -1) —
        // a1 = attack, a2 = the projectile). Fold into the leaf HIDE_FORCED op rather
        // than spending a callback tag: the viewer already interprets HIDE_FORCED and
        // it sits at the same sequence position, so the action-frame timing is
        // preserved (spec §4.2). PRES_CB_HIDE_PROJ stays scope-only, unused.
        beginOp(PRES_OP_HIDE_FORCED);
        appendI32(resolveRef((Object*)a2));
    }
}

} // namespace fallout
