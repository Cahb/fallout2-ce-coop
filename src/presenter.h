#ifndef FALLOUT_PRESENTER_H_
#define FALLOUT_PRESENTER_H_

#include "geometry.h"
#include "obj_types.h"

namespace fallout {

// Full combat-attack outcome (combat_defs.h). Forward-declared so the seam can
// carry it by const pointer without presenter.h depending on combat internals;
// only the presenters that consume attackResult include combat_defs.h.
struct Attack;

// One row of EVENT_PLAYER_ROSTER (MP_PROPOSAL.md Ch 5.4): which session, if any,
// controls the player actor in a given registry slot this baseline generation.
struct PlayerRosterRow {
    // Registry slot — the only STABLE identity for a player actor. Slot 0 is the
    // host.
    int slot;
    // The actor's netId for THIS baseline generation only (re-minted on every
    // rebaseline). 0 = no actor resolved, defensive.
    int actorNetId;
    // Owning session, or 0 = unbound (never claimed, or claimant disconnected).
    int sessionId;
    // Courtesy liveness at emit time; object deltas remain authoritative.
    bool alive;
};

// Bits for Presenter::objectDelta's changedFields mask (MP_PROTOCOL.md §6.2).
// Which syncable scalar fields of an object changed during a resolved beat.
enum ObjectDeltaField {
    OBJECT_DELTA_FID = 1 << 0,
    OBJECT_DELTA_ROTATION = 1 << 1,
    OBJECT_DELTA_FLAGS = 1 << 2,
    OBJECT_DELTA_HP = 1 << 3,
    OBJECT_DELTA_RADIATION = 1 << 4,
    OBJECT_DELTA_POISON = 1 << 5,
    OBJECT_DELTA_AP = 1 << 6,
    OBJECT_DELTA_COMBAT_RESULTS = 1 << 7,
    // The object's (top-level) inventory changed: an item added/removed, a stack
    // quantity changed, an item's flags (equip/worn) flipped, OR an item's intra-item
    // fields changed (a wielded weapon's ammoQuantity/ammoTypePid, an ammo/misc item's
    // charges) — the last catches firing/depletion with no membership change. Detected
    // by a fingerprint diff; the presenter reads owner->data.inventory to serialize.
    OBJECT_DELTA_INVENTORY = 1 << 8,
};

// Bits for Presenter::worldDelta (MP_PROTOCOL.md §2 worldDelta). Global sim
// state not owned by any object.
enum WorldDeltaField {
    // In-game clock (gameTimeGetTime) advanced. gvars/mvars are server-only in
    // v1 (MP_PROTOCOL.md §2), so they are not a wire field yet.
    WORLD_DELTA_GAMETIME = 1 << 0,
};

// Phase 1 presenter seam (REWRITE_PLAN.md 1.2, WORKLIST_P1.md).
//
// The sim core emits presentation through this interface instead of calling
// render/HUD/sound functions directly. v1 is a MECHANICAL MIRROR of the
// legacy callees: each method corresponds 1:1 to the call pattern it
// replaces, and ClientPresenter (presenter_client.cc) reproduces the old
// behavior exactly — golden replays must stay byte-identical.
//
// The base class is the null presenter (all no-ops): what a dedicated
// server runs. Do not add methods that return presentation state to the
// sim — this seam is one-way by design; the eventual wire protocol is a
// serialization of these calls.
class Presenter {
public:
    virtual ~Presenter() = default;

    // Scrolling message log (display monitor).
    virtual void consoleMessage(const char* text) {}

    // Console text addressed to ONE actor: feedback that answers that player's own
    // input and means nothing to anybody else — "you don't have enough action
    // points", "you cannot use that skill in combat", "you cannot get there".
    //
    // The split is by whether the message describes the WORLD or a REFUSAL. World
    // text (damage, deaths, what someone hit) stays on plain consoleMessage and
    // reaches every viewer, because that shared narration is most of what makes a
    // co-op fight readable. A refusal is the opposite: nothing happened, so there
    // is nothing for the other players to see, and at N=4 in a firefight it is
    // pure log noise about non-events.
    //
    // Defaults to forwarding, so the local/narrating presenters — where there is
    // exactly one player and addressing is meaningless — need no override and
    // behave exactly as before. actorNetId 0 means "no address" = broadcast.
    virtual void consoleMessageFor(int actorNetId, const char* text) { consoleMessage(text); }

    // The same line, tagged with WHAT KIND of message it is (msg_channel.h). The
    // sim names a channel; the viewer decides what that looks like. Never put a
    // colour on this seam — presentation is the client's, and a mod adding a
    // channel must not have to teach the wire about fonts.
    //
    // actorNetId 0 = broadcast, exactly as consoleMessageFor. Defaults to dropping
    // the channel and forwarding, so the local/narrating presenters (one player, no
    // styling) need no override.
    virtual void consoleMessageStyled(int actorNetId, int channel, const char* text)
    {
        consoleMessageFor(actorNetId, text);
    }

    // Play a full-screen movie on every viewer (MOVIE_* / GAME_MOVIE_* flags).
    //
    // PROJECTION ONLY. Which movies have been seen is sim/save state the server
    // already owns (game_movie_state.cc) and scripts branch on it, so a viewer that
    // drops this event is cosmetically behind and nothing more. No-op by default:
    // the client and the golden probe reach the real gameMoviePlay directly.
    virtual void moviePlay(int movie, int flags) {}

    // Co-op WORLD-STATE sync: ship the movie SEEN ledger (seen[0..count)) so a
    // viewer's local ledger matches the world's. Drives the vault-suit dude look
    // + inventory art on a late joiner ([[vault-suit-appearance-gap]]). State, not
    // a story beat — rides the baseline, never blocks.
    virtual void movieSeenState(const unsigned char* seen, int count) {}

    // Co-op: tell every viewer to STOP its local movie playback. FIRST ACK RELEASES
    // THE ROOM (game_movie.h) — the server emits this the instant the barrier frees,
    // so a viewer that skipped ends the cutscene for everyone instead of the others
    // rolling on until each skips itself. [[movie-playback-coop]]
    virtual void movieStop() {}

    // World-state lifecycle (P5-B, MP_PROTOCOL.md §2 STATE primitives). These
    // are the authoritative object spawn/move/destroy deltas — the world-state
    // channel ARCHITECTURE.md assumed the seam already carried but did not.
    // Hooked at the clean single choke points in object.cc (MP_PROTOCOL.md §4);
    // no-op here (and unoverridden by ClientPresenter/NarratePresenter), so the
    // sim emits them on every path but only a NetworkPresenter (P5-C) consumes
    // them. Fieldwise changes (fid/rotation/flags/hp/inventory) are NOT here —
    // they ride a batched objectDelta emitted per resolved action (§4).

    // An object now exists in the world (objectCreateWithFidPid / _obj_copy).
    // Emitted after obj->id is assigned; obj carries the network id.
    virtual void objectCreated(Object* obj) {}

    // An object changed tile/elevation (objectSetLocation). The per-tile stream
    // is coalesced into a move(id, path[], durMs) event downstream; here it is
    // the raw single-hop delta. Pixel-offset interpolation is client-derived.
    virtual void objectMoved(Object* obj, int fromTile, int fromElevation, int toTile, int toElevation) {}

    // An object is being removed from the world (objectDestroy). Emitted before
    // the free, so obj->id is still readable.
    virtual void objectDestroyed(Object* obj) {}

    // Item<->world lifecycle (MP_PROTOCOL.md §4/§6.2b). An object is attached to
    // (_obj_connect) or detached from (_obj_disconnect) a world tile WITHOUT being
    // created or freed — it is re-parented between an inventory (or limbo, tile==-1)
    // and the map: item drop/scatter/unload, ground pickup, script obj_connect/
    // obj_disconnect. This path goes through _obj_connect_to_tile, which BYPASSES
    // both objectCreateWithFidPid (no objectCreated) and objectSetLocation (no
    // objectMoved), so these are the ONLY signals that a dropped item appears at a
    // tile / a picked-up item leaves the world. Kept DISTINCT from objectCreated/
    // objectDestroyed so the wire never lies about lifecycle: the object persists,
    // it just crosses the inventory<->world boundary (the owner's inventory side is
    // separately visible via OBJECT_DELTA_INVENTORY). NOT load-safe: a FRESH .map load
    // fires spawn + disconnect in a burst (map-enter scripts create loot via
    // objectCreateWithPid -> _obj_disconnect), straddling the mapTransition signal on
    // BOTH sides — so the P5-C load-window guard must bracket the WHOLE mapLoad, not
    // anchor on mapTransition (MP_PROTOCOL.md §4/§5). Only savegame restore is silent
    // (objectLoadAllInternal/_obj_load_obj bypass these). Two consumer invariants:
    // (a) objectDisconnected can arrive with NO prior objectConnected (a stack-split /
    //     give creates an item at tile==-1 then disconnects it) — treat it idempotently.
    // (b) objectMoved with fromTile==-1 is ALSO a world-entry signal (objectSetLocation
    //     from limbo), equivalent to objectConnected for a consumer tracking presence.

    // An object attached to a world tile (_obj_connect success). tile/elevation are
    // the committed values; obj->id carries the network id.
    virtual void objectConnected(Object* obj, int tile, int elevation) {}

    // An object detached from its world tile (_obj_disconnect success) into an
    // inventory or limbo. Emitted after obj->tile is set to -1; obj->id is intact.
    virtual void objectDisconnected(Object* obj) {}

    // Global sim state changed (MP_PROTOCOL.md §2 worldDelta). `changedFields` is
    // a bitmask of WorldDeltaField; read current values (e.g. gameTimeGetTime())
    // for the set bits. Emitted once per beat by the per-beat delta scan.
    virtual void worldDelta(unsigned int changedFields) {}

    // A batched fieldwise delta on an existing object (MP_PROTOCOL.md §2/§6.2).
    // ONE per resolved action (per beat): fid/rotation/flags/hp/radiation/poison/
    // AP/combat-results that have no clean per-call choke point are captured by a
    // per-beat shadow-diff (object_delta.{h,cc}) rather than micro-hooks.
    // `changedFields` is a bitmask of ObjectDeltaField; read the current values
    // off obj for the set bits. Position (tile/elevation) is NOT here — that is
    // objectMoved. Includes a top-level inventory-changed bit; worldDelta
    // (gametime/gvars) is separate (deferred).
    virtual void objectDelta(Object* obj, unsigned int changedFields) {}

    // The active tactical map finished loading (MP_PROTOCOL.md §2 SESSION/MAP
    // control). Hooked at the UNIVERSAL choke point mapLoad (map.cc:628, the site
    // §5 names): every map-index change funnels here — exit grids/elevators (via
    // mapHandleTransition→mapLoadById), worldmap arrivals and encounters (via
    // worldmap_ui.cc mapLoadById), new-game and save-restore. A load wholesale-
    // replaces the object set (objectDeltaScan rebaselines on the same signal), so
    // downstream this tells the client to drop its world and expect a fresh
    // snapshot rather than a spawn/delta flood. mapIndex/elevation are the freshly-
    // loaded values. NOT emitted for same-map elevation changes (ladders/stairs) —
    // those keep the object set and ride objectMoved (fromElevation != toElevation).
    // New-game and save-restore ALSO reach this hook (inert here); the P5-C
    // NetworkPresenter suppresses those with the §5 load-window guard.
    virtual void mapTransition(int mapIndex, int elevation) {}

    // Join baseline snapshot (MP_PROTOCOL.md §2/§7 SESSION control). Emitted once
    // per object at server-loop install, AFTER the world is fully loaded and net
    // ids are assigned but BEFORE the first beat — the authoritative ground truth
    // for every object already present that no lifecycle event will ever announce
    // (the initial map loads before any presenter installs; pre-placed objects load
    // silently via _obj_load_obj). A consumer seeds its world from these, then
    // applies the spawn/move/... event stream on top. This is the LOGICAL baseline
    // the file replayer consumes (tools/replay.py); the binary network snapshot a
    // real joining client loads is a SEPARATE, later P5-C artifact that reuses the
    // save pipeline (_map_save_file + mapLoad), NOT this per-object text channel.
    // Base no-op → byte-identical on every non-narrate path.
    virtual void snapshotObject(Object* obj) {}

    // Baseline section delimiters (MP_PROTOCOL.md §2 SESSION control's
    // `snapshot begin|chunk|end`). Bracket every run of snapshotObject emissions
    // (serverEmitBaseline, server_loop.cc) so a consumer knows where the ground-truth
    // baseline starts/ends and can seed its world atomically.
    //
    // Emitted TWICE in a session's life, and the second is load-bearing:
    //   (1) at server-loop install, before beat 0 — the join baseline; and
    //   (2) after ANY mid-run map change — because a map transition RECYCLES every
    //       netId (objectAssignAllNetIds resets the counter to 1 and re-walks the new
    //       object set, object.cc:4489, driven by objectDeltaReset), so every netId a
    //       consumer already holds silently comes to mean a DIFFERENT object.
    //       mapTransition says "drop your world"; THIS is what replaces it. Without
    //       the re-emission the stream does not merely go incomplete, it LIES.
    //       See MP_PROTOCOL.md §7d.
    // mapIndex/elevation describe the map the baseline covers.
    //
    // Base no-op, and deliberately NOT overridden by ClientPresenter/NarratePresenter
    // → byte-identical on every legacy/probe path.
    virtual void snapshotBegin(int mapIndex, int elevation) {}
    virtual void snapshotEnd() {}

    // STEP-4 join snapshot BLOB (CLIENT_JOIN_DESIGN.md §2). The SNAPSHOT_OBJECT
    // baseline above carries only netId/pid/tile — enough for replay.py's position
    // check, NOT enough to reconstruct a playable world. This carries the real map:
    // the save-pipeline bytes (map header + gvars/lvars + squares + scripts +
    // objects, then the dude) that a joining viewer loads via mapLoad to become
    // fully present. Streamed in chunks (each within the u16 event-len ceiling).
    //
    // wantsSnapshotBlob() lets the server SKIP the serialization entirely (it is
    // non-trivial and briefly mutates gMapHeader) unless a consumer needs it. The
    // null and narrate presenters return false → every golden path is inert. Only
    // the NetworkPresenter (the real outbound wire) returns true.
    virtual bool wantsSnapshotBlob() { return false; }
    // `actorCount` is the TOTAL number of player actors carried by the blob (1 =
    // the host alone = every pre-co-op blob). The appendix holds the dude
    // section then actorCount-1 extra-actor sections in registry slot order;
    // `dudeBlobLen` bounds the whole appendix, and the sections self-delimit
    // (MP_PROPOSAL.md Ch 5.3).
    virtual void snapshotBlobBegin(int mapIndex, int elevation, int dudeNetId,
        unsigned int gameTime, int mapSaveVersion, int mapBlobLen, int dudeBlobLen,
        unsigned int crc32, int actorCount) {}
    virtual void snapshotBlobChunk(const unsigned char* data, int length) {}
    virtual void snapshotBlobEnd() {}

    // One server beat fully resolved (server_loop.cc serverTick, after
    // objectDeltaScan + invariantsCheck). THE FRAME BOUNDARY: a serializer
    // accumulates the beat's events and flushes them as ONE sequenced frame
    // (MP_PROTOCOL.md §1 — the beat IS the resolution quantum, and objectDeltaScan
    // already batches per-beat, so no finer boundary exists to honour).
    //
    // Ordering is deliberate: fires AFTER invariantsCheck, so a beat that trips an
    // invariant abort never flushes a partial frame onto the wire.
    //
    // A sim→presenter NOTIFICATION (void, no reply) → respects the one-way seam.
    // Framing HERE rather than a direct call from server_loop.cc keeps the loop
    // ignorant of any concrete presenter type. Base no-op → byte-identical.
    virtual void beatEnd(int tick) {}

    // Combat control (MP_PROTOCOL.md §2 COMBAT control). Hooked at the combat.cc
    // choke points; no-op here (unoverridden by Client/Narrate presenters) so
    // byte-identical on every path until a NetworkPresenter (P5-C) consumes them.
    // These are the SESSION/turn framing; the per-object STATE they imply (hp/AP/
    // combat.results/death) rides objectDelta, so attackResult in particular is a
    // PRESENTATION cue (the causal envelope: who hit whom, with what, where), NOT
    // a second place damage is applied.

    // A fight has begun (combat.cc _combat, after _combat_begin). `initiator` is
    // the attacker that triggered it (csd->attacker; may be null for a scripted
    // no-attacker start / save-in-combat resume). The combatant roster is derived
    // client-side from the turnStart/objectDelta stream.
    virtual void combatEnter(Object* initiator) {}

    // The fight has ended (combat.cc _combat_over teardown). Final critter states
    // already arrived via objectDelta; the client tears down combat framing.
    virtual void combatExit() {}

    // A combatant's turn begins (combat.cc _combat_turn entry — fires for AI and
    // player, including a turn that is then skipped for DAM_LOSE_TURN/KO/dead;
    // the consumer filters). `isPlayer` is critter == gDude. `apAvailable` is the
    // AP as of turn entry — freshly reset by _combat_set_move_all on a normal
    // round, but the save-restored remaining AP on the load-resume path (where
    // _combat_turn(gDude) runs before the round-boundary reset). `deadlineMs` is
    // the yourTurn(deadline) idle-timer budget — STUBBED 0 in v1: no turn timer
    // exists yet, and the resumable-turn / per-action idle-timer state machine
    // (which would make _combat drivable across beats) is deferred to P5-C.
    virtual void turnStart(Object* critter, bool isPlayer, int apAvailable, int deadlineMs) {}

    // One resolved attack (combat.cc _combat_apply_attack_results, after damage/
    // death are applied). `attack` is the live _main_ctd: attacker/defender/weapon,
    // hitMode, hit location, damage, the DAM_* flags (hit/miss/crit/dead) and the
    // blast `extras[]` set — the whole multi-victim blast is ONE event (§2). Read-
    // only; the P5-C serializer maps every Object* by obj->id (§6.4), never the
    // pointer. Emitted before _main_ctd is re-init'd, so all fields are final.
    virtual void attackResult(const Attack* attack) {}

    // A critter drew/readied a weapon while IN COMBAT (inventory.cc _invenWieldFunc,
    // the ANIM_TAKE_OUT branch). PRESENTATION only: the wield's state (in-hand flags)
    // already rides objectDelta's inventory field, so this is purely the draw-motion
    // cue — without it a critter that wields mid-fight snaps to its armed pose. The
    // viewer replays animationRegisterTakeOutWeapon(critter, weaponAnimationCode)
    // SEQUENCED before the shot. `weaponAnimationCode` is the item's weapon anim code
    // (0 = non-weapon / unarmed swap, never emitted). No-op here (unoverridden by
    // Client/Narrate presenters) → byte-identical on every non-network path.
    virtual void weaponTakeOut(Object* critter, int weaponAnimationCode) {}

    // An actor played an out-of-combat interaction GESTURE (the ANIM_MAGIC_HANDS_*
    // crouch/reach vanilla plays before a use/get/skill outcome, actions.cc). Emitted
    // by the server interaction executor (server_control.cc) just before the outcome
    // fires. PRESENTATION only: the outcome's STATE (door frame via doorState, item
    // removal via objectDelta, etc.) rides its own events, so this is purely the
    // actor's motion cue — without it the door/item just changes with no gesture.
    // `anim` is an ANIM_MAGIC_HANDS_GROUND/MIDDLE code. No-op here → byte-identical on
    // every non-network path (INTERACTION_UX_DESIGN.md §4).
    virtual void actionAnim(Object* actor, int anim) {}

    // An explosion detonated (actionExplode server branch). PRESENTATION only: the
    // STATE — damage/HP, deaths, knockback tile moves, and the scenery it destroys (a
    // Temple door) — all ride objectDelta / EVENT_DESTROY / MOVE, but the boom CLOUD +
    // sound are engine reg_anim-welded into the branch the server skips (actions.cc:1838),
    // so without this cue the door just vanishes silently. The viewer replays the vanilla
    // cloud: 7 transient art objects (blast tile + 6 ring) play ANIM_STAND once and
    // self-destroy, with the "whn1xxx1" boom sfx — using the REAL animation handlers, not
    // a bespoke renderer. `tile`/`elevation` are the blast center. No-op here →
    // byte-identical on every non-network path (PRESENTATION_SEAM_DESIGN.md Stage 1).
    // The `attack` carries the blast's causal envelope — the defender (critter on the
    // blast tile, may be null) + the extras[] set (ring victims) with their per-victim
    // damage/flags — so the viewer also replays each caught critter's gib/knockdown
    // (_show_damage / _show_damage_extras), not just the cloud. The victim STATE (death
    // flags, corpse fid, knockback tile) still rides objectDelta; this only adds the
    // ANIMATION. PRESENTATION only. No-op here → byte-identical on every non-network path.
    virtual void explosionFx(int tile, int elevation, const Attack* attack) {}

    // A recorded presentation command stream (PRESENTATION_RECORD_REPLAY_SPEC.md,
    // POC). The server ran an action's animate branch inside a record section
    // (pres_record.cc) so its reg_anim LEAVES recorded a flat op stream; this ships
    // it, and the viewer replays it through its own real reg_anim engine — one
    // generic mechanism instead of a per-action replay (actionExplodeReplay). The
    // op STATE (damage/death/scenery) still rides objectDelta/EVENT_DESTROY; this is
    // PRESENTATION only. `data`/`size` is the raw op buffer; `opCount` is the op
    // count for the reader. `actorNetId` is the sequence's PRIMARY actor — the
    // critter whose out-of-combat approach glide the viewer must drain before it may
    // play the stream (0 = no such actor, e.g. an explosion, which plays on arrival).
    // No-op here → byte-identical on every non-network path (and only emitted at all
    // under F2_SERVER_PRES_RECORD).
    virtual void presSeq(const unsigned char* data, int size, int opCount, int actorNetId = 0) {}

    // A door (or openable scenery) opened/closed on a server-driven path
    // (proto_instance.cc _obj_use_door / objectOpenClose serverLoopActive branch).
    // PRESENTATION only: the open/closed STATE (OBJECT_OPEN_DOOR flag + blocking/LOS)
    // rides objectDelta, but the door's art FRAME does NOT, so without this cue the
    // door snaps between closed/open art and critters appear to warp through it. The
    // viewer replays the vanilla frame slide (ANIM_STAND forward/reversed + door sfx)
    // and holds a crossing critter's glide until it finishes. `opening` = the door is
    // opening (else closing); `targetFrame` = the terminal art frame (last for open,
    // 0 for closed) for a fallback snap if the replay can't build. No-op here →
    // byte-identical on every non-network path.
    virtual void doorState(Object* door, bool opening, int targetFrame) {}

    // A dialog node is being presented (game_dialog.cc _gdProcess server barrier,
    // DIALOG_STREAMING_PLAN.md). Both a PRESENTATION cue and an INPUT BARRIER: while
    // an actor talks, the server blocks in the conversation and ships each node so
    // every viewer renders the real gdialog window seeded from the wire. The OWNER
    // viewer (whose controlled actor == `driver`) drives it (sends dsay/dend back
    // over the control channel); other viewers render it read-only ("someone is
    // talking"). `speaker` is the NPC being talked to (its headFid is derived
    // client-side); `driver` is the talking actor (the dude in v1) — stamped so each
    // viewer can tell owner from spectator (MP_PROTOCOL §6.4: serialized by netId,
    // never the pointer). `reaction` seeds the head mood. `reply` is the NPC's line;
    // `options` holds `optionCount` resolved option strings (already prefixed, via
    // gameDialogGetOptionText). No-op here → byte-identical on every non-network path.
    // `reaction` is the script's fidget level (FIDGET_GOOD/NEUTRAL/BAD) and `headFid`
    // the script's talking-head fid (-1 = none). Both come from start_gdialog's args and
    // are NOT derivable viewer-side: the critter proto's head_fid is unset for most
    // critters, so a viewer guessing from the proto shows no head at all.
    virtual void dialogNode(Object* speaker, Object* driver, int reaction,
        const char* reply, const char* const* options, int optionCount,
        const char* audioFileName = nullptr, int headFid = -1) {}

    // The conversation ended (every _gdProcess exit: normal end, ESC/dend, or a bail
    // on disconnect/combat/quit). Every viewer tears down its dialog window —
    // spectators cannot dismiss early. `driver` is the talking actor. No-op here.
    virtual void dialogEnd(Object* driver) {}

    // ---- Barter (the trade screen, opened by a dialog option) ----------------
    //
    // ►► WHY THE TABLES ARE SNAPSHOTTED AND NOT STREAMED AS OBJECTS. The two
    // offer tables are `objectCreateWithFidPid(&t, -1, -1)` scratch objects. They
    // DO get netIds, but object.cc early-returns on `pid == -1` BEFORE
    // presenter()->objectCreated(), so no viewer ever learns they exist and any
    // delta addressing them is dropped on a null lookup. They are also destroyed
    // with the trade. So they are ephemeral UI state, not world entities, and the
    // wire carries a SNAPSHOT of what is on them — which sidesteps netId
    // lifetime, the pid==-1 hole and object-lifetime bugs in one move.
    //
    // ►► AND WHY A SNAPSHOT IS THE ONLY OPTION ANYWAY: while a trade is open the
    // server tick is PARKED inside inventoryOpenTrade (the barter barrier), so
    // objectDeltaScan never runs and NOTHING streams. Without these events a
    // spectator sees a player stand motionless for the whole trade.
    struct BarterStack {
        int pid;
        int quantity;
    };

    // A trade opened with `merchant` (a real world object, so a netId addresses
    // it). `driver` is the actor trading — every other viewer renders read-only.
    virtual void barterBegin(Object* merchant, Object* driver) {}

    // The trade's whole visible state, re-sent after every accepted move. Both
    // tables plus the two VALUATIONS.
    //
    // ►► THE VALUES ARE COMPUTED SERVER-SIDE AND SENT, NEVER DERIVED. A viewer
    // cannot reproduce barterComputeValue: it reads partyGetBestSkillValue
    // (party-wide, not the driver's own), perkHasRank(gDude, PERK_MASTER_TRADER)
    // gated on `dude == gDude` (a spectator would evaluate that for THEMSELVES),
    // and a barterMod carrying the script-set reaction, which dialog scripts
    // mutate mid-conversation. Money has one authority, like every other number.
    // ►► ALL FOUR INVENTORIES, not just the two tables. A spectator renders the
    // DRIVER's trade, not their own: the left panel is the driver's pack and the
    // right is the merchant's. Neither can be read off the mirrored objects,
    // because the tick is PARKED for the whole trade -- objectDeltaScan never
    // runs, so every mirror is frozen at whatever it held when the trade opened,
    // and items moving onto a table would never leave the pack on screen.
    // Snapshotting all four makes the view self-contained and correct for
    // everyone, driver and spectator alike.
    struct BarterView {
        const BarterStack* driverInv;
        int driverInvCount;
        const BarterStack* merchantInv;
        int merchantInvCount;
        const BarterStack* playerTable;
        int playerTableCount;
        const BarterStack* merchantTable;
        int merchantTableCount;
        int offerValue;
        int askingValue;
        // Result of the last COMMIT that produced this state: -1 = this state is a
        // plain move/open, not a commit (no feedback); otherwise a BarterResult
        // (0 = OK, else the refusal reason). Lets a viewer confirm/deny the Offer
        // button, which the server otherwise answers only by moving items or not.
        int resultCode = -1;
    };

    virtual void barterState(const BarterView& view) {}

    // The trade ended (done, cancel, or a bail). Every viewer closes its window.
    virtual void barterEnd() {}

    // Who owns which player actor, re-announced after every baseline (netIds are
    // re-minted on every rebaseline, so a roster row is only valid for the
    // generation it arrived in — persistent identity is the SLOT, never the
    // netId). Also emitted on any binding change. A viewer derives its own actor
    // by matching sessionId against the one it learned at accept
    // (MP_PROPOSAL.md Ch 5.4).
    virtual void playerRoster(const PlayerRosterRow* rows, int rowCount) {}

    // Grant an in-combat inventory screen to ONE player actor: they asked for it
    // on their own turn and paid the vanilla AP entry fee, so the server tells
    // their viewer to open the screen. Out of combat the viewer opens it locally
    // with no round trip at all — this exists because in combat the OPEN is the
    // priced act (item.cc inventoryApCostApply), and only the authority can
    // decide whether it was affordable.
    //
    // Broadcast, addressed by netId, because the wire has no per-session event
    // channel (the preamble carries the sessionId; the event stream itself is
    // shared). Every viewer receives this; the one whose local actor matches
    // opens. That is the same addressing the roster rebind uses, so it stays
    // correct across the netId re-mint on rebaseline: this event is consumed
    // immediately, well inside one baseline generation.
    virtual void inventoryGrant(int actorNetId) {}

    // Revoke a granted in-combat inventory screen: this actor's turn ended while
    // they still had it open, so the screen must close. Holding the turn open
    // while the screen is up (server_loop.h serverSlotInModal) covers the normal
    // case, but a turn can still end underneath it — the idle deadline expiring,
    // the actor dying, a script terminating combat. Without this the player would
    // sit in a live inventory screen on somebody else's turn, sending verbs the
    // server now rejects, which is exactly the desync this event prevents.
    // Addressed by netId like the grant.
    virtual void inventoryRevoke(int actorNetId) {}

    // Worldmap travel streaming. The server enters the worldmap travel driver and
    // viewers render the map; intents flow client→server; state flows back.
    virtual void worldmapBegin() {}
    virtual void worldmapEnd() {}
    virtual void worldmapState(int worldPosX, int worldPosY, int walkDestX, int walkDestY,
        bool isWalking, int walkDistance, int carFuel, int currentAreaId, bool isInCar) {}
    // Worldmap fog of war: the flattened per-subtile visited/known state grid
    // (tile-major, then row/column — wmTileInfoList order). The whole grid is
    // only ~840 bytes for FO2's 20 tiles, so it ships whole rather than as
    // per-subtile deltas; the server suppresses it unless something changed.
    virtual void worldmapSubtiles(const unsigned char* states, int count) {}

    // World view invalidation (legacy tileWindowRefresh / ...Rect).
    virtual void worldInvalidate() {}
    virtual void worldInvalidateRect(const Rect* rect, int elevation) {}

    // Tactical-map load / teardown chrome (map.cc mapLoad, _map_place_dude_and_mouse).
    // The sim funnels a map change through mapLoad regardless of presentation; these
    // are the pure-sink UI calls it makes along the way, routed so f2_core does not
    // name the window/sound/interface/movie/mouse symbols at link time. No-op here
    // (a dedicated server has no iso window); ClientPresenter reproduces the legacy
    // calls exactly.

    // Clear the tactical (iso) window to black and refresh it (legacy windowFill of
    // gIsoWindow with _colorTable[0] + windowRefresh) — the pre-load blank frame.
    virtual void worldClear() {}

    // Load the looping ambient background sound for the new map (legacy
    // backgroundSoundLoad, e.g. "wind2").
    virtual void ambientSoundLoad(const char* name, int a2, int a3, int a4) {}

    // Show the bottom interface bar after the map is placed (legacy interfaceBarShow).
    virtual void hudBarShow() {}

    // Fade the movie/black overlay back out at the end of a map load (legacy
    // gameMovieFadeOut).
    virtual void movieFadeOut() {}

    // Reset the bouncing-cursor frame after (re)placing the dude (legacy
    // gameMouseResetBouncingCursorFid).
    virtual void mouseResetBouncingCursor() {}

    // Tactical (iso) view enable / disable CHROME (map.cc isoEnable / isoDisable).
    // The sim authority — gIsoEnabled and the critter freeze (_scr_enable_critters /
    // _scr_disable_critters) — stays in map.cc; this carries only the presentation
    // side: floating-text toggle, mouse enable/disable, and the frame-animation
    // tickers (_object_animate, _dude_fidget). Kept in exact legacy call order.
    // No-op here: a dedicated server registers no per-frame animation ticker
    // (animation is applied synchronously, and _dude_fidget has no headless body),
    // so worldEnable MUST NOT run on the server — the null presenter guarantees it.
    virtual void worldEnable() {}
    virtual void worldDisable() {}

    // Floating text above an object (legacy textObjectAdd + rect refresh).
    virtual void floatText(Object* owner, const char* text, int font, int color, int outlineColor) {}

    // Player HUD.
    virtual void hudHitPoints(bool animate) {}
    virtual void hudArmorClass(bool animate) {}
    virtual void hudActionPoints(int actionPointsLeft, int bonusActionPoints) {}
    virtual void hudItems(bool animated, int leftItemAction, int rightItemAction) {}
    virtual void hudIndicatorBar() {}

    // Sound effects. sfxPlayAt applies positional volume relative to the
    // camera (legacy _gsound_compute_relative_volume + play).
    virtual void sfxPlay(const char* name) {}
    virtual void sfxPlayAt(const char* name, Object* source) {}

    // Screen fades around sim time-skips (legacy paletteFadeTo(gPaletteBlack)
    // and paletteFadeTo(_cmap) pairs: doctor/first-aid, book reading, script
    // fade opcodes).
    virtual void screenFadeOut() {}
    virtual void screenFadeIn() {}

    // Combat HUD end-turn buttons (legacy interfaceBarEndButtons*).
    virtual void hudEndButtonsShow(bool animated) {}
    virtual void hudEndButtonsHide(bool animated) {}
    virtual void hudEndButtonsGreen() {}
    virtual void hudEndButtonsRed() {}

    // Game mouse cursor / mode / world scrolling (legacy game_mouse/_gmouse).
    virtual void cursorSet(int cursor) {}
    virtual void cursorModeSet(int mode) {}
    virtual void cursorRefresh() {}
    virtual void mouseObjectsShow() {}
    virtual void mouseObjectsHide() {}
    virtual void scrollEnable() {}
    virtual void scrollDisable() {}

    // Background music (legacy _gsound_background_* / backgroundSoundDelete).
    //
    // musicPlayLevel returns the legacy rc so wmMapMusicStart keeps its exact
    // error branch: 0 = started (or nothing to do), -1 = failed. The base no-op
    // returns 0 (success), matching server_stubs.cc — "no audio" is not an error.
    virtual int musicPlayLevel(const char* fileName, int fadeIn) { return 0; }
    virtual void musicStop() {}

    // Modal error box (legacy showMesageBox / _win_msg fatal errors).
    virtual void errorBox(const char* text) {}
};

// Current presenter; never null (defaults to the built-in null presenter).
Presenter* presenter();

// Install a presenter (nullptr restores the null presenter).
void presenterSet(Presenter* newPresenter);

bool presenterEmissionsSuppressed();
void presenterSetEmissionsSuppressed(bool suppressed);

// -- TIME-SKIP MOVE COALESCING --------------------------------------------
// A script game-time skip (game_time_advance) advances the world by minutes or
// days inside ONE server tick: every queued event and every NPC AI catches up
// synchronously, and each tile step of that catch-up would otherwise emit its
// own paced objectMoved. Measured worst case: 320 MOVE events in a single beat
// (denbus2 / Sheila; next-busiest beat in the same run: 74). The viewer has no
// backpressure, so it then owes minutes of glide animation for a world that
// already finished moving — which is what the denbus2 dialog "softlock" is.
//
// Backpressure can never fix this: the avalanche is emitted in one tick, before
// any feedback from the viewer could arrive. The world advanced UNWATCHED, so
// the correct presentation is final positions, not the walk that produced them.
//
// Inside a skip window objectSetLocation records each object's ORIGIN tile
// (first move only) instead of emitting, and presenterTimeSkipEnd() emits one
// objectMoved per object that actually ended up somewhere else — durMs unstamped,
// i.e. a snap. Reentrant (nested Begin/End nest; only the outermost emits).
void presenterTimeSkipBegin();
void presenterTimeSkipEnd();
bool presenterTimeSkipActive();

// Called from the objectSetLocation seam while a skip is active: stash where
// this object was BEFORE the skip touched it. Ignores every later move of the
// same object, so the pair (origin, current) stays authoritative.
void presenterTimeSkipRecordMove(Object* obj, int fromTile, int fromElevation);

// Object lifetime: an object destroyed or torn down mid-skip must not be read at
// End. Called from the objectDestroy / _obj_remove_all seams.
void presenterTimeSkipForget(Object* obj);
void presenterTimeSkipForgetAll();

} // namespace fallout

#endif /* FALLOUT_PRESENTER_H_ */
