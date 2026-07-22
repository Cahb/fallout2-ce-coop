#ifndef PRES_RECORD_H
#define PRES_RECORD_H

#include "obj_types.h"

// Presentation record/replay — the POC seam (PRESENTATION_RECORD_REPLAY_SPEC.md).
//
// The dedicated server runs the engine's animate branch it normally SKIPS (the
// !serverLoopActive() gate) inside a record section; the animationRegister* LEAVES,
// instead of no-op'ing (server_anim.cc), append a typed command to a flat buffer.
// The buffer ships as EVENT_PRES_SEQ; the viewer replays it through its OWN real
// reg_anim engine (client_net.cc presPlayRecordedSeq) — vanilla-faithful by
// construction, no per-action replay function. This proves the theory on the
// explosion; actionExplodeReplay is the debt it would eventually delete.
//
// Everything here is a no-op unless F2_SERVER_PRES_RECORD is set AND a record
// section is open — so goldens stay byte-identical (the server keeps emitting the
// old EVENT_EXPLOSION_FX cue).

namespace fallout {

// ===========================================================================
// COVERAGE MANIFEST — "move ALL animation into record/replay" (scope, not impl)
// ===========================================================================
// The POC proved the mechanism on the explosion. This header is the SCOPE MAP
// for widening it to every animation family. Read this before touching the .cc.
//
// KEY REFRAME (why this is smaller than it looks): the leaf VOCABULARY below is
// already ~complete — every animationRegister* leaf in server_anim.cc already has
// a record op + emitter (Table A, spec §4.1). Widening coverage is therefore NOT
// "invent N opcodes per family"; it is three bounded kinds of work:
//   (W1) a few more CALLBACK TAGS  (this file — PresCallbackTag; spec Table B)
//   (W2) relax the serverLoopActive() GATE at each composite SITE so its animate
//        branch runs under a record section (actions.cc / combat.cc /
//        proto_instance.cc — NOT this file; see the FAMILY TABLE for the sites)
//   (W3) clear per-family HAZARDS (register-time state mutation; throw branch).
//
// FAMILY TABLE — what each migration step folds in, and the bespoke quirk it
// RETIRES (spec §8). Status: [POC]=done in the POC, [ ]=scope, [KEEP]=stays as-is.
//
//   Family            Composite site (gate to relax)        Ops / callbacks used                    Retires (bespoke quirk)               Status
//   ---------------   -----------------------------------   -------------------------------------   -----------------------------------   ------
//   Explosion         actionExplode  actions.cc:1838         OBJ_CREATE, ANIMATE_AND_HIDE, SFX,      actionExplodeReplay + EVENT_          [POC]
//                                                            UNSET_FLAG(cloud reveal), CALL{DEATH}   EXPLOSION_FX(id30) + playExplosion
//   Receive-damage    actionDamage   actions.cc:~2235        ANIMATE(hit-react), PING, SFX,          (adds the whc1xxx1 hit cue that       [ ]
//    / hit-react                                             CALL{DEATH}                             was never built — net-new coverage)
//   Death / corpse    (rides attack + explosion + damage)   CALL{SHOW_DEATH} -> fid/flat/NO_BLOCK   the corpse-finalize welded to the     [POC]
//                                                            + itemDropAll(guarded, §4.3)           skipped _show_death anim callback
//   Door slide        _obj_use_door / objectOpenClose        SET_FID/ANIMATE frames from            clientDoorAnimPlay + EVENT_DOOR_      [ ]
//                     proto_instance.cc:1763/2090            _check_door_state                       STATE(id28) + doorState emit
//   Weapon take-out   (in-combat wield / _action_attack)    TAKE_OUT{weaponAnimCode}                bespoke EVENT_WEAPON_TAKE_OUT(id27)   [ ]
//    / put-away / swap
//   Attack / fire     _action_attack (combat pipeline)      OBJ_CREATE(projectile), MOVE_STRAIGHT   Attack reconstruction + local        [ ]
//                     — LAST (most tuned: pacing/throws)     (projectile), SET_FID, HIDE_FORCED,     _action_attack call in client_combat_
//                                                            PING, SFX, CALL{HIDE_PROJ, DEATH}       anim + EVENT_ATTACK_RESULT replay half
//   Stance / fid      (rides every family above)            SET_FID, ROTATE (mid-sequence order)    ad-hoc fid-authority tripwires;       [ ]
//    transitions                                                                                     [[frame-index-render-gotcha]] class
//   Gesture (use/get  interactionGestureAnim (stage-3)      ANIMATE / TAKE_OUT template             client_present kActionAnim arm +      [ ]
//    /skill anims)                                                                                   EVENT_ACTION_ANIM(id29)
//   Knockdown / fall  actionKnockdown actions.cc:168        MOVE_STRAIGHT_WAIT                       (leaf-only, low risk — spec §5.4)     [ ]
//   Locomotion        animationRegister{Move,Run}To*        recorded like any leaf (end tile         bunched sync-apply MOVE + local walk- [ ]
//    IN COMBAT        server_anim.cc:526/590 (synchronous    authoritative; turn-serial = no race)   frame guess in combat
//                     branch when isInCombat)
//   Locomotion        animationRegister{Move,Run}To*        (NONE — STATE lane, MOVE events w/      KEEP: the ONE soft/non-authoritative  [KEEP]
//    FREE-ROAM        server_anim.cc:526/590 (glide branch)  durMs); recorder emits SYNC_MOVE only   presentation; real-time/concurrent
//   Ambient / fidget  _object_fidget animation.cc:2595       (NONE — client-ticker-local)            KEEP: never on the wire               [KEEP]
//   Bucket C (script  raw op_reg_anim_* opcodes             ALL of Table A, by construction         the reason for a stream vs replicas   [ ]
//    cutscenes/mods)  (no composite — records by running)                                           (IDEAS.md modding; not Temple demo)
//
// ---------------------------------------------------------------------------
// TARGET ARCHITECTURE — the recorder is the SINGLE presentation engine for every
// DISCRETE action; the ONLY non-authoritative presentation is real-time free-roam
// locomotion glide.
//
// A "discrete action" (attack, open door, loot, use-item-on, skill-on-target,
// weapon switch, gesture, death) is ALWAYS a server-authored recorded sequence the
// client replays through its real reg_anim. Two entry gates, ONE replay engine:
//   - IN COMBAT: turn-serial. The barrier already paces it; one actor at a time,
//     no simultaneity → locomotion folds INTO the sequence here too (the combat
//     move is recorded like any other leaf; end tile is authoritative).
//   - OUT OF COMBAT: gated by the approach-latch (PendingInteraction), lifecycle:
//
//        free-roam GLIDE (soft, non-authoritative, real-time, interruptible)
//              │  server walks you adjacent (existing latch)
//              ▼
//        ARRIVAL → server fires the action:
//              (1) AUTHORITY SNAP  — drain glide, sprite at true tile+rotation.
//                  This is a RECONCILE snap (walkSnapToAuthority / #6/#9 drain),
//                  NOT a teleport: the server already walked you adjacent, so it is
//                  normally sub-tile. Believed ALREADY IMPLICIT in today's drive
//                  (glide, then play the server-told anim) — VERIFY whether it is
//                  explicit before piping; if not, the mechanism exists to pipe.
//              (2) push recorded SEQUENCE (was: bespoke EVENT_DOOR_STATE / _ACTION_ANIM)
//              ▼
//        GLUED — client replays the authoritative sequence, movement locked
//              ▼
//        RELEASE → free-roam glide again
//
//   Self-actions (weapon switch, use-on-self, sneak-toggle) are the degenerate
//   zero-approach case: empty glide segment, snap-in-place + sequence + release.
//
// THE ONE EXCEPTION (soft by design): out-of-combat tile-crossing walk/run GLIDE.
// Genuinely hard — real-time, concurrent actors, interruptible, re-pathed — and the
// least fidelity-critical (the "travel" state). It stays STATE-lane (MOVE events
// with durMs, the glide machinery, kept exactly as built); its walk-frame anim
// stays locally-derived (accepting bugs #2/#7 as forgivable travel-state cosmetics).
// The recorder's only interaction with a live free-roam walk is a SYNC_MOVE marker
// so a following recorded sequence waits out the approach glide (spec §6.1).
// ===========================================================================

// The op set. u8 opcode; args are LE i32 unless noted (matches presenter_network's
// putI32 / client_net Reader). Append-only within this versioned tag space.
//
// Ops 1..PRES_OP_CALL are LIVE (emitted + interpreted by the POC). Ops after the
// LIVE marker are DECLARED FOR SCOPE — enumerated so the wire vocabulary is visible
// and reserved, but not yet emitted by any leaf. Adding a scope op to the LIVE set
// (wiring its emitter + client switch) is the only change that bumps
// kPresStreamVersion.
enum PresOp : unsigned char {
    PRES_OP_SEQ_BEGIN = 1,       // i32 flags (reg_anim_begin requestOptions)
    PRES_OP_SEQ_END,             // (none)
    PRES_OP_PRIORITY,            // i32 n (_register_priority)
    PRES_OP_OBJ_CREATE,          // i32 handle(<0), i32 fid, i32 tile, i32 elev, i32 flags,
                                 //   i32 rotation, i32 adoptNetId (v4: 0 = pure transient;
                                 //   >0 = also register under this real netId for a later
                                 //   state-lane connect/disconnect, e.g. a thrown weapon)
    PRES_OP_ANIMATE,             // i32 ref, i32 anim, i32 delay
    PRES_OP_ANIMATE_REV,         // i32 ref, i32 anim, i32 delay
    PRES_OP_ANIMATE_FOREVER,     // i32 ref, i32 anim, i32 delay
    PRES_OP_ANIMATE_AND_HIDE,    // i32 ref, i32 anim, i32 delay
    PRES_OP_HIDE_FORCED,         // i32 ref
    PRES_OP_SET_FID,             // i32 ref, i32 fid, i32 delay
    PRES_OP_ROTATE,              // i32 ref, i32 tile
    PRES_OP_UNSET_FLAG,          // i32 ref, i32 flag, i32 delay
    PRES_OP_MOVE_STRAIGHT,       // i32 ref, i32 tile, i32 elev, i32 anim, i32 delay
    PRES_OP_MOVE_STRAIGHT_WAIT,  // i32 ref, i32 tile, i32 elev, i32 anim, i32 delay
    PRES_OP_SFX,                 // i32 ref, str name (u16 len + bytes), i32 delay
    PRES_OP_SET_LIGHT,           // i32 ref, i32 dist, i32 intensity, i32 delay
    PRES_OP_TAKE_OUT,            // i32 ref, i32 weaponAnimCode, i32 delay
    PRES_OP_PING,                // i32 flags, i32 delay
    PRES_OP_CALL,                // u8 tag, i32 ref, i32 arg (defunctionalized callback)
    PRES_OP_MOVE_TO_TILE,        // i32 ref, i32 tile, i32 elev, i32 anim, i32 actionPoints,
                                 //   i32 preWalkAp, i32 delay — PATHED walk/run to a tile,
                                 //   replayed AS-IS through the REAL animationRegisterRunTo/
                                 //   MoveToTile. Records the leaf's REAL args (not a
                                 //   synthesized endpoint). preWalkAp = the mover's AP BEFORE
                                 //   the walk; the client sets obj->ap to it before replaying
                                 //   so the per-step AP charge (animation.cc:2117) reproduces
                                 //   the server's stop point instead of dying on the local
                                 //   (stale/drained) pool. anim = ANIM_WALK/ANIM_RUNNING.
                                 //   Authoritative position rides the STATE lane, HELD +
                                 //   reconciled on the client (COMBAT_MOVE_RECORD_DESIGN.md).
    PRES_OP_MOVE_TO_OBJ,         // i32 ref, i32 targetRef, i32 anim, i32 actionPoints,
                                 //   i32 preWalkAp, i32 delay — PATHED walk to an OBJECT
                                 //   (multihex stop-short + trailing RotateToTile handled by
                                 //   the real registrar on replay). Sibling of MOVE_TO_TILE.

    // ---- LIVE / SCOPE boundary --------------------------------------------
    // Everything below is reserved-but-unemitted (see the FAMILY TABLE + spec §4.1).

    PRES_OP_SEQ_CLEAR,           // i32 ref (reg_anim_clear) — cancels an object's
                                 //   in-flight client sequence. v1 relies on the
                                 //   interpreter's own reserve/cancel; needed once a
                                 //   recorded sequence can pre-empt another (attack).
    PRES_OP_SYNC_MOVE,           // i32 ref — barrier marker: subsequent recorded ops
                                 //   for `ref` wait until its STATE-lane walk glide
                                 //   completes. Emitted by the Move/Run-To* leaves
                                 //   (which stay STATE, not recorded). Needed by
                                 //   bucket-C / attack-with-approach staging, not v1.
};

// The last opcode the POC actually emits + interprets. Ops strictly above this are
// scope-only. Keep in sync when a scope op is promoted to LIVE.
const unsigned char kPresOpLastLive = PRES_OP_MOVE_TO_OBJ;

// Defunctionalized reg_anim-callback tags (PRESENTATION_RECORD_REPLAY_SPEC.md §4.2,
// §6.4). A closed set (Table B, verified 2026-07-18: 24 targets). Only the RECORD
// class needs a wire tag — the client re-runs the callback BODY under that tag. The
// other classes need NO tag and are handled server-side (they record nothing):
//
//   DROP  — state the server fast-path already applied (_obj_drop, _report_explosion,
//           _report_dmg, _combat_explode_scenery, _set_door_state_*, _ai_print_msg…).
//   EXEC  — a state-bearing outcome the server must RUN at record time, currently
//           carried by the per-action interaction fast-paths (_obj_use, _obj_pickup,
//           _obj_use_item_on, _obj_use_skill_on, _talk_to, scriptsRequestLooting…);
//           spec "stage 3" generalizes these off the bespoke fast-paths.
//   LOGIC — pure gates, no presentation, no state (_is_next_to, _can_talk_to…).
//   LOCAL — client-only HUD callbacks the server never registers (_intface_*).
//
// Append-only. v1 (POC) ships only SHOW_DEATH.
enum PresCallbackTag : unsigned char {
    PRES_CB_SHOW_DEATH = 1,      // actions.cc:579 — corpse fid + flat/NO_BLOCK +
                                 //   outline-off (RECORD) + itemDropAll (guarded STATE
                                 //   half, §4.3). a2 = (void*)anim.

    // ---- scope (Table B RECORD targets not yet tagged) --------------------
    PRES_CB_HIDE_PROJ,           // hideProjectile actions.cc:2523 — objectHide(transient)
                                 //   at the action frame. MAY fold into HIDE_FORCED
                                 //   instead of a tag (§4.2) — verify the action-frame
                                 //   timing survives the fold before spending a tag.
};

// Bumped when a scope op/tag is promoted to LIVE, or when a LIVE op's wire layout
// changes. v1 = the POC stream; v2 = OBJ_CREATE gained a trailing i32 rotation (so a
// transient projectile faces its travel direction instead of rotation 0); v3 = added
// PRES_OP_MOVE_TO_TILE + PRES_OP_MOVE_TO_OBJ (in-combat pathed locomotion, recorded and
// replayed as-is like every other family).
const unsigned char kPresStreamVersion = 4;

// True iff F2_SERVER_PRES_RECORD is set AND the recording animation backend
// (server_anim.cc) is linked into this binary. Gate for everything. The backend
// requirement matters: the recorder's LEAVES live in server_anim.cc (f2_server
// only); the client/golden binary (fallout2-ce) links the REAL animation.cc reg_anim
// engine instead, where "record mode" would run the real animate branch's completion
// callbacks (freeing `attack`) on top of the state fast-path (also freeing it) — a
// double free. server_anim.cc calls presRecordSetBackendActive(true) at init; without
// it the flag is inert, so the env var can never half-activate the wrong backend.
bool presRecordEnabled();

// Armed by the server_anim.cc recording backend at static-init (f2_server only).
void presRecordSetBackendActive(bool active);

// Snapshot the created-this-beat watermark (resolveRef's transient discriminator).
// Call at the top of every server beat (serverTick), BEFORE command/action dispatch
// creates any transient — so a composite's pre-section transients (explosion clouds,
// the actionDamage synthetic attacker) are classified created-this-beat and ship as
// OBJ_CREATE rather than an unresolvable netId. f2_server only (inert elsewhere).
void presRecordBeatBegin();

// True while a record section is open (leaves record instead of applying/no-op).
bool presRecordActive();

// Open/close a record section around an animate branch. Begin snapshots the RNG
// and clears the buffer; End restores the RNG (so the authoritative stream is
// untouched — the animate branch's cosmetic rolls leave no trace). Abort is End
// without keeping the (partial) buffer meaningful, used on the reg_anim_end()==-1
// failure path. Nesting is not supported (the engine never nests reg_anim); a
// second Begin while active is ignored with a stderr note.
void presRecordSectionBegin();
void presRecordSectionEnd();
void presRecordSectionAbort();

// Ambient section — like Begin/End but WITHOUT the RNG snapshot/restore. Used by the
// in-combat MOVE bracket, whose rolls are authoritative (see pres_record.cc). Shares the
// same buffer/active state as the ordinary section.
void presRecordAmbientBegin();
void presRecordAmbientEnd();

// Deferred authoritative-state commit for the in-combat MOVE: the leaf stashes the walk,
// the composite ships the presSeq, then calls presRecordCommitDeferred() to apply it (so
// presentation precedes state on the wire). The hook is armed by the recording backend
// (server_anim.cc) at static init; null (no-op) in the client/golden binary.
void presRecordSetDeferredCommitHook(void (*hook)());
void presRecordCommitDeferred();

// The recorded buffer (valid between SectionEnd and the next SectionBegin).
const unsigned char* presRecordData();
int presRecordSize();
int presRecordOpCount();

// ---- leaf record emitters (called from server_anim.cc leaves) ----
void presRecordSeqBegin(int flags);
void presRecordSeqEnd();
void presRecordPriority(int n);
void presRecordAnimate(Object* owner, int anim, int delay);
void presRecordAnimateReversed(Object* owner, int anim, int delay);
void presRecordAnimateForever(Object* owner, int anim, int delay);
void presRecordAnimateAndHide(Object* owner, int anim, int delay);
void presRecordHideForced(Object* owner);
void presRecordSetFid(Object* owner, int fid, int delay);
void presRecordRotate(Object* owner, int tile);
void presRecordUnsetFlag(Object* owner, int flag, int delay);
void presRecordMoveStraight(Object* owner, int tile, int elev, int anim, int delay);
void presRecordMoveStraightWait(Object* owner, int tile, int elev, int anim, int delay);
void presRecordMoveToTile(Object* owner, int tile, int elev, int anim, int actionPoints, int preWalkAp, int delay);
void presRecordMoveToObject(Object* owner, Object* target, int anim, int actionPoints, int preWalkAp, int delay);

// Tag the NEXT OBJ_CREATE for `obj` with a real netId the viewer should ALSO map onto the
// created transient (a thrown weapon whose ground item is later connect/disconnected by
// netId). Cleared when consumed or at the next section begin. See pres_record.cc.
void presRecordSetAdoptNetId(Object* obj, int netId);
void presRecordSfx(Object* owner, const char* name, int delay);
void presRecordSetLight(Object* owner, int dist, int intensity, int delay);
void presRecordTakeOut(Object* owner, int weaponAnimCode, int delay);
void presRecordPing(int flags, int delay);

// ---- scope leaf emitters (declared, NOT yet defined in pres_record.cc) ----
// Reserved so the recording seam's full shape is visible. Wiring either one into a
// server_anim.cc leaf (and defining it) is part of promoting its op to LIVE.
void presRecordSeqClear(Object* owner);                 // reg_anim_clear  -> PRES_OP_SEQ_CLEAR
void presRecordSyncMove(Object* owner);                 // Move/RunTo*     -> PRES_OP_SYNC_MOVE

// A reg_anim callback registration. The recorder maps `proc` to a wire tag: only
// _show_death (RECORD) is emitted as PRES_OP_CALL; everything else is DROP (state
// the server already applies) and records nothing. a1/a2 are the callback's raw
// args (a1 = the Object* the body acts on; a2 = the packed int arg for SHOW_DEATH).
void presRecordCallback(void* a1, void* a2, void* proc);

} // namespace fallout

#endif /* PRES_RECORD_H */
