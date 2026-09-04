#include "presenter_network.h"

#include "object_delta.h" // objectDeltaScan — flush the change a fade is hiding
#include "state_audit.h" // StateAuditRecord — the live mirror-divergence oracle
#include "game_dialog.h" // GAME_DIALOG_REACTION_* — per-option reaction on the wire

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "combat.h" // _combat_free_move (TURN_START bonus-move field, §3.a)
#include "combat_defs.h"
#include "light.h" // lightGetAmbientIntensity — global ambient-light worldDelta
#include "scripts.h"
#include "map.h" // mapGetLoadGeneration (WireFrameMeta.mapGeneration)
#include "object.h"
#include "pres_record.h" // kPresStreamVersion / presRecordCostMs
#include "msg_channel.h"
#include "presenter.h"
#include "server_loop.h" // serverDedicatedActive — feature A marks only on the real server
#include "server_players.h" // serverActorBusyMarkByNetId — the action-gate busy mark
#include "sim_clock.h"
#include "wire_defs.h"

namespace fallout {

// ============================================================================
// WIRE FORMAT v1 (MP_PROTOCOL.md §2). Little-endian, byte-packed, no padding.
//
// Stream once:   magic "F2NS" | u16 version=1
// Frame:         u32 seq | u32 simTs | u32 payloadLen | u16 eventCount | payload
// Event:         u8 type | u8 flags | u16 len | payload[len]
//
// FRAMING (§1): the FRAME is the atomic transport unit and carries the sequence
// number; events inherit (seq, simTs) from their frame's header. A per-event seq
// would be redundant state that must never disagree with frame order. Gap
// detection survives intact: a missed frame seq means every event in it was
// missed — exactly the "request snapshot + resync" case. There is no partial-frame
// case, because payloadLen makes a frame length-prefixed and atomically applicable.
// The frame boundary is the BEAT (Presenter::beatEnd) — the sim's resolution
// quantum, and already what objectDeltaScan batches on, so no finer boundary exists.
//
// FORWARD COMPAT: every event is length-prefixed, so an unknown type is skipped
// whole (skip-unknown-T). New fields append; readers stop at len.
//
// STRINGS ARE **NOT UTF-8**. u16 length + RAW bytes. Fallout .msg text is
// codepage-encoded, so consoleMessage/floatText/errorBox carry high-ASCII bytes
// verbatim (this is also why the golden gates need `grep -a` and why replay.py
// opens with errors="replace"). A consumer MUST treat these as opaque bytes and
// transcode at the presentation layer, never at the wire layer.
//
// move.durMs HAS A PRODUCER since the stepped-walk engine (F2_SERVER_SMOOTH_WALK,
// server_anim.cc): stepped hops stamp their sim-ms-per-tile pace via
// presenterSetNextMoveDurationMs; every other mover — teleports, scripted
// placement, within-beat sync moves, all golden paths — leaves it 0 = snap. The
// client uses durMs>0 as its animate-vs-snap discriminator and slaves per-hop
// animation DURATION to it; art fps selects frames only (§7). turnStart.deadlineMs
// remains a reserved always-0 slot with no producer; do not "fix" that zero.
// ============================================================================

namespace {

// kWireVersion / kWireMagic now live in wire_defs.h: the transport writes the
// same preamble bytes and a local copy here is exactly the drift this pair used
// to invite.

// Event type tags. Append only — these are wire values.
enum EventType : unsigned char {
    // STATE (authoritative, always applied).
    EVENT_SPAWN = 1,
    EVENT_MOVE = 2,
    EVENT_DESTROY = 3,
    EVENT_CONNECT = 4,
    EVENT_DISCONNECT = 5,
    EVENT_OBJECT_DELTA = 6,
    EVENT_WORLD_DELTA = 7,
    EVENT_SNAPSHOT_OBJECT = 8,

    // SESSION / MAP control.
    EVENT_SNAPSHOT_BEGIN = 9,
    EVENT_SNAPSHOT_END = 10,
    EVENT_MAP_TRANSITION = 11,

    // COMBAT control.
    EVENT_COMBAT_ENTER = 12,
    EVENT_COMBAT_EXIT = 13,
    EVENT_TURN_START = 14,
    EVENT_ATTACK_RESULT = 15,

    // PRESENTATION (skippable cues).
    EVENT_CONSOLE = 16,
    EVENT_FLOAT_TEXT = 17,
    EVENT_SFX = 18,
    EVENT_SFX_AT = 19,
    EVENT_FADE_OUT = 20,
    EVENT_FADE_IN = 21,
    EVENT_ERROR_BOX = 22,
    EVENT_MUSIC_STOP = 23,

    // STEP-4 join snapshot blob (CLIENT_JOIN_DESIGN.md §2). The save-pipeline bytes
    // a joining viewer loads to become present. BEGIN carries the guard fields;
    // CHUNK carries a slice of the blob (map body then dude, self-delimiting on
    // load); END closes it. Append-only, no version bump — a pre-STEP-4 consumer
    // skips them whole (skip-unknown-T).
    EVENT_SNAPSHOT_BLOB_BEGIN = 24,
    EVENT_SNAPSHOT_BLOB_CHUNK = 25,
    EVENT_SNAPSHOT_BLOB_END = 26,

    // Weapon draw/ready cue (COMBAT_CLIENT_DESIGN.md §6). A critter wielded a weapon
    // mid-combat; the viewer plays ANIM_TAKE_OUT before the shot. PRESENTATION-only —
    // the wield state rides objectDelta's inventory field. Append-only (skip-unknown-T).
    EVENT_WEAPON_TAKE_OUT = 27,

    // Door open/close slide cue. The open/closed STATE rides objectDelta (flags); the
    // art FRAME does not, so the viewer replays the vanilla frame slide and holds a
    // crossing critter until it finishes. PRESENTATION-only. Append-only.
    EVENT_DOOR_STATE = 28,

    // Out-of-combat interaction GESTURE cue (ANIM_MAGIC_HANDS_* crouch/reach). Emitted
    // before a use/get/skill outcome so the actor animates instead of the world just
    // changing. PRESENTATION-only (the outcome STATE rides its own events). Append-only.
    EVENT_ACTION_ANIM = 29,

    // Explosion boom/cloud/sfx cue. The blast STATE (damage/deaths/knockback/scenery
    // destroy) rides objectDelta / EVENT_DESTROY / MOVE; the engine-welded cloud anim
    // does not, so the viewer replays it (7 transient art objects + "whn1xxx1" via the
    // real animation handlers) at the blast tile. PRESENTATION-only. Append-only.
    EVENT_EXPLOSION_FX = 30,

    // Recorded presentation command stream (PRESENTATION_RECORD_REPLAY_SPEC.md POC):
    // u8 streamVersion, u16 opCount, then the flat op bytes (pres_record.h PresOp).
    // The viewer replays it through its own reg_anim; the STATE rides objectDelta /
    // EVENT_DESTROY / MOVE. PRESENTATION-only, skip-unknown-T safe. Append-only.
    EVENT_PRES_SEQ = 31,

    // Dialog option streaming (DIALOG_STREAMING_PLAN.md). NODE ships one presented
    // conversation node (speaker + driver netIds, reaction, reply, N option strings);
    // END tears the window down. PRESENTATION + input-barrier: while a dialog is open
    // the server tick BLOCKS in the conversation barrier, so these are force-flushed
    // (no beatEnd will carry them). Append-only, skip-unknown-T safe.
    EVENT_DIALOG_NODE = 32,
    EVENT_DIALOG_END = 33,

    // Worldmap travel streaming. BEGIN enters the worldmap modal on viewers;
    // END tears it down; STATE carries the mutable sim subset of wmGenData
    // (position, walking, fuel, area — no chrome/rendering fields). Append-only.
    EVENT_WORLDMAP_BEGIN = 34,
    EVENT_WORLDMAP_END = 35,
    EVENT_WORLDMAP_STATE = 36,
    EVENT_WORLDMAP_SUBTILES = 37,

    // Player-actor roster (MP_PROPOSAL.md Ch 5.4): slot → (netId, owning session,
    // alive), re-announced after every baseline because netIds are re-minted on
    // every rebaseline. A viewer matches sessionId against the one it learned in
    // the accept preamble to find its own actor. Append-only, skip-unknown-T safe
    // (a pre-co-op viewer ignores it and keeps rendering the host).
    EVENT_PLAYER_ROSTER = 38,

    // In-combat inventory grant (Stage 4): the addressed actor asked to open the
    // inventory on their own turn and paid the AP entry fee, so their viewer
    // opens the screen. Broadcast + addressed by netId — the wire has no
    // per-session event channel — and ignored by every viewer whose local actor
    // is someone else. Append-only, skip-unknown-T safe.
    EVENT_INVENTORY_GRANT = 39,

    // The addressed actor's turn ended while their granted inventory screen was
    // still open (idle deadline, death, script-terminated combat) — close it.
    EVENT_INVENTORY_REVOKE = 40,

    // Background music: play this level track (name + fade steps). The map→track
    // decision is SIM-side (wmMapMusicStart reads MapInfo::music, sourced from
    // data\maps.txt and overridable by op_set_map_music), so the server owns the
    // "when" and the viewer owns the audio. Pairs with EVENT_MUSIC_STOP.
    // PRESENTATION-only. Append-only, skip-unknown-T safe.
    EVENT_MUSIC_PLAY = 41,

    // Play a full-screen movie (MOVIE_* index + GAME_MOVIE_* flags). The SEEN
    // LEDGER is sim state the server already owns truthfully (game_movie_state.cc),
    // and scripts branch on it — so this event is the PROJECTION only, and a viewer
    // that ignores it diverges cosmetically, never in sim. The server BLOCKS in the
    // movie barrier while it plays, so this is force-flushed like the dialog node.
    // PRESENTATION-only. Append-only, skip-unknown-T safe.
    EVENT_MOVIE_PLAY = 42,

    // Barter. BEGIN carries the merchant + the driving actor; STATE is the whole
    // visible trade re-sent after every accepted move (both tables as (pid,qty)
    // rows, plus the two server-computed valuations); END closes it.
    //
    // STATE is a full SNAPSHOT rather than a diff on purpose: a trade is small
    // (a handful of stacks), it makes a dropped event self-healing, and it is the
    // only shape a late joiner could be handed. PRESENTATION-only — the
    // authoritative item movement still rides OBJECT_DELTA_INVENTORY when the
    // tick resumes; this is what the trade LOOKS like while it is open.
    // Append-only, skip-unknown-T safe.
    EVENT_BARTER_BEGIN = 43,
    EVENT_BARTER_STATE = 44,
    EVENT_BARTER_END = 45,

    // Co-op WORLD-STATE: the movie SEEN ledger, shipped on every baseline so a late
    // joiner's local ledger matches the world's. Without it a client that joined
    // after the vault-suit movie derives its own dude + inventory art as tribal
    // ([[vault-suit-appearance-gap]]). STATE-flagged, rides the baseline frame.
    EVENT_MOVIE_SEEN_STATE = 46,

    // Co-op: stop local movie playback on every viewer. Emitted when the movie
    // barrier frees (first ack), so one player's skip ends the cutscene for all.
    EVENT_MOVIE_STOP = 47,

    // Co-op: a per-actor character-sheet row changed at runtime (chem stat mod,
    // level-up, ...). Payload = slot + a one-slot PSHT block. The join blob is the
    // durable source; this is the LIVE delta so a viewer sees the change without
    // waiting for a transition.
    EVENT_PLAYER_SHEET = 48,
    EVENT_ENCOUNTER_PROMPT = 49, // random-encounter accept/decline prompt (title+body); ack via encaccept/encdecline
    EVENT_ENCOUNTER_CLOSE = 50, // first viewer answered — break the others out of the prompt box

    // Global variables that changed this beat (index/value pairs). A NEW type rather
    // than extra worldDelta fields: unknown types are skipped whole via the event
    // length by both the client and tools/replay.py, so this cannot move a gate.
    EVENT_GVAR_DELTA = 51,

    // Server-authored holodisks: CLEAR then one ADD per disk (pipboy.h). Text on the wire
    // because the client has no data file for these.
    EVENT_HOLODISK_CLEAR = 52,
    EVENT_HOLODISK_ADD = 53,
    EVENT_ELEVATOR_PROMPT = 54, // co-op: show the rider vanilla's elevator panel
    EVENT_AUTOMAP_OPEN = 55, // co-op: the Motion Sensor's user opens ITS OWN automap

    // Co-op STEAL: the server owns the steal screen (the roll is a skill check,
    // and a client may not roll its own dice). BEGIN names the thief and the
    // victim — both real world objects, so both are netId-addressable, which is
    // why STATE carries no payload: the item movement itself rides the ordinary
    // OBJECT_DELTA_INVENTORY reconcile and STATE is only the flush heartbeat for
    // a tick parked in the steal barrier. See Presenter::stealState.
    EVENT_STEAL_BEGIN = 56,
    EVENT_STEAL_STATE = 57,
    EVENT_STEAL_END = 58,
    EVENT_LOOT_GRANT = 59, // co-op: the server opened a container for THIS actor — show the screen
    EVENT_STATE_AUDIT = 60, // authoritative per-object state, for mirror divergence checking
    EVENT_UI_LOCK = 61, // scripted cutscene input lock (op_game_ui_disable/_enable), addressed
    EVENT_WORLDMAP_AREAS = 62, // worldmap city table: known/visited/entrances for every area
};

// Event flag bits.
enum EventFlags : unsigned char {
    // Set = STATE-DELTA (authoritative, apply even when stale — dropping state is
    // permanent desync). Clear = PRESENTATION (skippable under lag). §1.
    EVENT_FLAG_STATE = 1 << 0,
};

// The delta masks ride the wire as u16 (putU16 below). The enums are documented
// "append only", so pin the assumption rather than silently truncating bit 16.
static_assert(OBJECT_DELTA_INVENTORY <= 0xFFFF, "ObjectDeltaField no longer fits the u16 wire field");
static_assert(WORLD_DELTA_GAMETIME <= 0xFFFF, "WorldDeltaField no longer fits the u16 wire field");

// A netId of 0 = "no object" (netids are minted from 1; object.cc:4462).
const int kNoNetId = 0;

int netIdOf(Object* obj)
{
    return obj != nullptr ? obj->netId : kNoNetId;
}

// ---------------------------------------------------------------------------
// File sink — the STEP 2 stream source that tools/replay.py reads.
// ---------------------------------------------------------------------------
class FileByteSink : public ByteSink {
public:
    bool open(const char* path)
    {
        close(); // never leak a FILE* on a re-open
        _stream = fopen(path, "wb");
        return _stream != nullptr;
    }

    void close()
    {
        if (_stream != nullptr) {
            fclose(_stream);
            _stream = nullptr;
        }
    }

    void write(const void* data, unsigned int size) override
    {
        if (_stream != nullptr && size != 0) {
            fwrite(data, 1, size, _stream);
        }
    }

    void flush() override
    {
        if (_stream != nullptr) {
            fflush(_stream);
        }
    }

private:
    FILE* _stream = nullptr;
};

// Appearance/relocation event trace (F2_TRACE_EVENTS=1). A DIAGNOSTIC for the
// "objects warp through doors / spawn from thin air" complaint: prints every
// spawn/connect/move/destroy the server emits, so an aggro on a dense map shows
// whether a pop-in is a genuine SPAWN (script reinforcement), a CONNECT (item
// re-parent), or a MOVE — and for a MOVE whether durMs>0 (should glide) or 0
// (snaps = teleport). Off by default → zero cost on the gates.
inline bool eventTraceEnabled()
{
    static bool enabled = getenv("F2_TRACE_EVENTS") != nullptr;
    return enabled;
}

} // namespace

// ---------------------------------------------------------------------------
// The presenter.
// ---------------------------------------------------------------------------
class NetworkPresenter : public Presenter {
public:
    void begin(ByteSink* sink)
    {
        _sink = sink;
        _seq = 0;
        _entryBase = 0;
        _frame.clear();
        _eventCount = 0;
        _frameMoveCostByActor.clear();
        _frameSeqCostMs = 0;

        // Stream preamble — file sinks only. The socket sink writes it per
        // client at accept, because from version 2 it carries that client's
        // sessionId (see ByteSink::wantsStreamPreamble).
        if (_sink->wantsStreamPreamble()) {
            // A version-2 preamble is ALWAYS kWirePreambleLen bytes, whichever
            // sink it lands in — a file log writes sessionId 0 ("not a client
            // session"). Making the length depend on the sink would leave a
            // reader unable to know how many bytes to consume from the version
            // it just read, which is precisely the ambiguity a fixed-size
            // preamble exists to prevent.
            unsigned char preamble[kWirePreambleLen] = {};
            memcpy(preamble, kWireMagic, sizeof(kWireMagic));
            // Hand-rolled LE, like every other putter — NOT fwrite(&kWireVersion): a
            // native-endian write here would be the one endian-dependent byte in an
            // otherwise portable encoder.
            preamble[4] = (unsigned char)(kWireVersion & 0xFF);
            preamble[5] = (unsigned char)((kWireVersion >> 8) & 0xFF);
            // preamble[6..9] = LE i32 sessionId = kNoSessionId (0), already zeroed.
            _sink->write(preamble, sizeof(preamble));
        }
        _sink->flush();
    }

    void end()
    {
        // Never strand a partially-accumulated frame (e.g. a run that stops
        // between beats): flush whatever is pending, then drop the sink.
        flushFrame();
        if (_sink != nullptr) {
            _sink->flush();
            _sink = nullptr;
        }
    }

    // -- STATE lifecycle ----------------------------------------------------
    // EVERY override below checks presenterEmissionsSuppressed() as its first
    // statement. The flag is ADVISORY — nothing in the base class or presenter.cc
    // enforces it, so an override that forgets silently ships the whole map-enter
    // flood (~100 events straddling mapTransition, MP_PROTOCOL.md §4/§5) onto the
    // wire. mapTransition is the ONE deliberate exemption (see below).

    void objectCreated(Object* obj) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[evt] SPAWN   net=%d pid=%d tile=%d elev=%d\n", obj->netId, obj->pid, obj->tile, obj->elevation);
        beginEvent(EVENT_SPAWN, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(obj->pid);
        putI32(obj->tile);
        putI32(obj->elevation);
        putI32(obj->fid);
        // Birth flags: object_delta emits NO delta for a just-created object (it is
        // already announced here), so a spawned object's non-default flags (e.g. a
        // combat corpse's FLAT|NO_BLOCK) would otherwise never reach the client.
        // Appended after fid — a pre-existing reader that stops at fid is unaffected.
        putI32((int)obj->flags);
        endEvent();
    }

    void objectMoved(Object* obj, int fromTile, int fromElevation, int toTile, int toElevation) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        // netId 0 = "not in the syncable domain" (objectAssignAllNetIds zeroes every
        // NO_SAVE object). The decoder already drops these — lookup(0) returns null —
        // so emitting them is pure waste, and gEgg alone (the invisible wall-transparency
        // helper that objectSetLocation drags along with the dude, object.cc:1386) doubles
        // the MOVE traffic of every step the player takes. Dropping it here is
        // behaviourally identical and makes the [evt] trace readable: a phantom
        // "MOVE net=0" shadowing every dude hop reads exactly like a real desync.
        if (obj->netId == 0) return;
        // Presentation-cost tally (outbox, §8.6): this hop costs the mover its glide
        // duration. Summed PER ACTOR within the frame; the frame's cost is the MAX over
        // actors (concurrent movers overlap — a crowd doesn't cost the sum of everyone's
        // walks). durMs=0 snaps cost nothing. Passive: emits no wire byte, unused until
        // the outbox schedules on it (increment 3); inert for the file/golden sink.
        _frameMoveCostByActor[obj->netId] += (unsigned int)presenterNextMoveDurationMs();
        if (eventTraceEnabled()) fprintf(stderr, "[evt] MOVE    net=%d %d->%d durMs=%d%s\n", obj->netId, fromTile, toTile, presenterNextMoveDurationMs(), presenterNextMoveDurationMs() == 0 ? " (SNAP)" : "");
        beginEvent(EVENT_MOVE, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(fromTile);
        putI32(toTile);
        putI32(fromElevation);
        putI32(toElevation);
        // durMs: stamped by the stepped-walk engine around its objectSetLocation
        // (presenterSetNextMoveDurationMs); 0 = snap for every other mover.
        putI32(presenterNextMoveDurationMs());
        // run: 1 = RUN cycle, 0 = WALK. Appended after durMs — a reader that stops
        // at durMs is unaffected (same additive pattern as objectCreated's birth
        // flags). Authoritative; the viewer must not re-derive this from durMs.
        putI32(presenterNextMoveRun() ? 1 : 0);
        endEvent();
    }

    void objectDestroyed(Object* obj) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[evt] DESTROY net=%d pid=%d\n", obj->netId, obj->pid);
        beginEvent(EVENT_DESTROY, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(obj->pid);
        endEvent();
    }

    void objectConnected(Object* obj, int tile, int elevation) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[evt] CONNECT net=%d pid=%d tile=%d elev=%d\n", obj->netId, obj->pid, tile, elevation);
        beginEvent(EVENT_CONNECT, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(obj->pid);
        putI32(tile);
        putI32(elevation);
        endEvent();
    }

    void objectDisconnected(Object* obj) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        // ►► The missing half of the pair. objectConnected has traced since it was
        // written; this one never did, so "the item vanished off the floor and I do not
        // know who took it" was unanswerable from a server log — and on the viewer a
        // DISCONNECT for an adopt-entangled netId DESTROYS the mirror outright
        // (client_net.cc onDisconnect), so it is the single most consequential state
        // event we were not printing. `_obj_disconnect` has ~20 call sites (pickup,
        // consume, unload, script obj_disconnect), which is exactly why the netId+pid
        // alone is worth having: it names WHICH object left the world and when.
        if (eventTraceEnabled()) fprintf(stderr, "[evt] DISCONNECT net=%d pid=%d tile=%d elev=%d\n", obj->netId, obj->pid, obj->tile, obj->elevation);
        beginEvent(EVENT_DISCONNECT, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(obj->pid);
        endEvent();
    }

    void worldDelta(unsigned int changedFields) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_WORLD_DELTA, EVENT_FLAG_STATE);
        putU16((unsigned short)changedFields);
        if ((changedFields & WORLD_DELTA_GAMETIME) != 0) {
            putU32(gameTimeGetTime()); // u32: the clock is unsigned and outlives i32
        }
        if ((changedFields & WORLD_DELTA_LIGHT) != 0) {
            putI32(lightGetAmbientIntensity());
        }
        endEvent();
    }

    void holodiskSetBegin() override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_HOLODISK_CLEAR, EVENT_FLAG_STATE);
        endEvent();
    }

    void holodiskAdd(const char* name, const char* const* lines, int lineCount) override
    {
        if (presenterEmissionsSuppressed() || name == nullptr) return;
        beginEvent(EVENT_HOLODISK_ADD, EVENT_FLAG_STATE);
        putString(name);
        putU16((unsigned short)(lineCount > 0 ? lineCount : 0));
        for (int i = 0; i < lineCount; i++) {
            putString(lines[i] != nullptr ? lines[i] : "");
        }
        endEvent();
    }

    void elevatorPrompt(int actorNetId, int elevator, int startLevel) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_ELEVATOR_PROMPT, EVENT_FLAG_STATE);
        putI32(actorNetId);
        putI32(elevator);
        putI32(startLevel);
        endEvent();
    }

    void automapOpen(int actorNetId, bool usingScanner) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_AUTOMAP_OPEN, EVENT_FLAG_STATE);
        putI32(actorNetId);
        putI32(usingScanner ? 1 : 0);
        endEvent();
    }

    void gvarDelta(const int* indices, const int* values, int count) override
    {
        if (presenterEmissionsSuppressed() || indices == nullptr || values == nullptr) return;
        if (count <= 0) return;
        beginEvent(EVENT_GVAR_DELTA, EVENT_FLAG_STATE);
        putU16((unsigned short)count);
        for (int i = 0; i < count; i++) {
            putI32(indices[i]);
            putI32(values[i]);
        }
        endEvent();
    }

    void objectDelta(Object* obj, unsigned int changedFields) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        beginEvent(EVENT_OBJECT_DELTA, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putU16((unsigned short)changedFields);
        // Sparse, in bit order — a reader walks the mask and reads only what is set.
        if (changedFields & OBJECT_DELTA_FID) putI32(obj->fid);
        if (changedFields & OBJECT_DELTA_ROTATION) putI32(obj->rotation);
        // BANKED, not overlooked (adversarial review 2026-07-15): object_delta.cc:152
        // asks this file to mask flags to a syncable subset, since the whole-word diff
        // fires on client-local/render bits too. Measured cost is small (26-43
        // objectDelta events per 600-beat run) and the word is authoritative as sent,
        // so it is wire NOISE, not wire LIES. Deferred deliberately: the syncable
        // subset is not yet decided and guessing it now would bake a wrong constant
        // into a wire field. Revisit with the thin client (STEP 3).
        if (changedFields & OBJECT_DELTA_FLAGS) putI32((int)obj->flags);
        if (changedFields & OBJECT_DELTA_HP) putI32(obj->data.critter.hp);
        if (changedFields & OBJECT_DELTA_RADIATION) putI32(obj->data.critter.radiation);
        if (changedFields & OBJECT_DELTA_POISON) putI32(obj->data.critter.poison);
        if (changedFields & OBJECT_DELTA_AP) putI32(obj->data.critter.combat.ap);
        if (changedFields & OBJECT_DELTA_COMBAT_RESULTS) putI32(obj->data.critter.combat.results);
        if (changedFields & OBJECT_DELTA_INVENTORY) putInventory(obj);
        // FRAME last (highest bit) — appended after the variable-length inventory so the
        // reader stays aligned by walking the mask in bit order.
        if (changedFields & OBJECT_DELTA_FRAME) putI32(obj->frame);
        if (changedFields & OBJECT_DELTA_LIGHT) { putI32(obj->lightDistance); putI32(obj->lightIntensity); }
        // PID last (highest bit), same rule as FRAME/LIGHT: the reader walks the mask in
        // bit order, so a new field appends. See OBJECT_DELTA_PID for why a pid is not
        // the constant it looks like.
        if (changedFields & OBJECT_DELTA_PID) putI32(obj->pid);
        endEvent();
    }

    // -- SESSION / baseline -------------------------------------------------

    void snapshotBegin(int mapIndex, int elevation) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_SNAPSHOT_BEGIN, EVENT_FLAG_STATE);
        putI32(mapIndex);
        putI32(elevation);
        endEvent();
    }

    void snapshotObject(Object* obj) override
    {
        if (presenterEmissionsSuppressed() || obj == nullptr) return;
        beginEvent(EVENT_SNAPSHOT_OBJECT, EVENT_FLAG_STATE);
        putI32(obj->netId);
        putI32(obj->pid);
        putI32(obj->tile);
        putI32(obj->elevation);
        putI32(obj->fid);
        putI32((int)obj->flags);
        endEvent();
    }

    void snapshotEnd() override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_SNAPSHOT_END, EVENT_FLAG_STATE);
        endEvent();
        // Close the frame here so the baseline is never left dangling for a beatEnd
        // that may not come (serverInstall emits it BEFORE beat 0).
        //
        // NOTE for a STEP 3 client author: this does NOT mean the baseline always
        // arrives alone. On a mid-run rebaseline the flush carries whatever the beat
        // already accumulated — measured: one frame holding MAP_TRANSITION + 1837
        // snapshot objects + SNAPSHOT_END together. That is fine and arguably ideal
        // (the transition and its replacement world apply atomically), but do not code
        // against "a snapshot frame contains only snapshot events".
        flushFrame();
    }

    bool wantsSnapshotBlob() override { return true; }

    void snapshotBlobBegin(int mapIndex, int elevation, int dudeNetId,
        unsigned int gameTime, int mapSaveVersion, int mapBlobLen, int dudeBlobLen,
        unsigned int crc32, int actorCount) override
    {
        // The blob rides its OWN frame(s): close whatever the beat accumulated so
        // the (large) blob is not interleaved with unrelated events (§2, S3 note).
        flushFrame();
        beginEvent(EVENT_SNAPSHOT_BLOB_BEGIN, EVENT_FLAG_STATE);
        putI32(mapIndex);
        putI32(elevation);
        putI32(dudeNetId);
        putU32(gameTime);
        putU16((unsigned short)mapSaveVersion);
        putU32((unsigned int)mapBlobLen);
        putU32((unsigned int)dudeBlobLen);
        putU32(crc32);
        // Appended after crc32: events are length-prefixed, so a decoder that
        // predates co-op stops here and reads a 1-actor blob correctly.
        putU16((unsigned short)actorCount);
        endEvent();
    }

    void playerRoster(const PlayerRosterRow* rows, int rowCount) override
    {
        if (rows == nullptr || rowCount < 0) return;
        beginEvent(EVENT_PLAYER_ROSTER, EVENT_FLAG_STATE);
        putU16((unsigned short)rowCount);
        for (int i = 0; i < rowCount; i++) {
            putI32(rows[i].slot);
            putI32(rows[i].actorNetId);
            putI32(rows[i].sessionId);
            putU8(rows[i].alive ? 1 : 0);
            // Reserved for per-actor names (the shared dude proto means every
            // actor reads back as the host's name today — Ch 4.3). Emitting the
            // zero length now keeps the row layout stable when names land.
            putU16(0);
        }
        endEvent();
    }

    void snapshotBlobChunk(const unsigned char* data, int length) override
    {
        if (data == nullptr || length <= 0) return;
        beginEvent(EVENT_SNAPSHOT_BLOB_CHUNK, EVENT_FLAG_STATE);
        _frame.insert(_frame.end(), data, data + length);
        endEvent(); // fails closed (drops the event) if length > 0xFFFF — server caps it
    }

    void snapshotBlobEnd() override
    {
        beginEvent(EVENT_SNAPSHOT_BLOB_END, EVENT_FLAG_STATE);
        endEvent();
        flushFrame(); // the blob is complete; ship it before the SNAPSHOT_OBJECT baseline
    }

    void mapTransition(int mapIndex, int elevation) override
    {
        // NOT suppression-gated — the ONE deliberate exemption. mapLoad brackets
        // itself with the suppression window (map.cc:626..:873) and this fires from
        // INSIDE it (:816), yet the consumer must still hear "drop your world" even
        // though the intervening spawn/connect flood is correctly withheld
        // (MP_PROTOCOL.md §4/§5). Mirrors NarratePresenter's asymmetry.
        //
        // A fresh baseline ALWAYS follows: serverEmitBaselineIfMapChanged re-emits
        // it at the end of this beat, because the transition recycled every netId
        // (§7d). This event alone would leave the consumer holding stale netids.
        beginEvent(EVENT_MAP_TRANSITION, EVENT_FLAG_STATE);
        putI32(mapIndex);
        putI32(elevation);
        endEvent();
    }

    // -- COMBAT control -----------------------------------------------------

    void combatEnter(Object* initiator) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_COMBAT_ENTER, EVENT_FLAG_STATE);
        putI32(netIdOf(initiator)); // may legitimately be absent (scripted start)
        // Combat-entry position fence. Free-roam glides are presentation and may lag
        // the authoritative server tile when somebody else/NPC/script starts combat.
        // Snapshot EVERY replicated critter on the elevation, not `_combat_list`:
        // pressing A with no target legitimately enters combat with zero combatants,
        // yet the next melee target still needs the same range geometry on both ends.
        std::vector<Object*> critters;
        for (Object* obj = objectFindFirstAtElevation(gElevation);
             obj != nullptr;
             obj = objectFindNextAtElevation()) {
            if (obj->netId > 0 && FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER) {
                critters.push_back(obj);
            }
        }
        putU16((unsigned short)critters.size());
        for (Object* obj : critters) {
            putI32(obj->netId);
            putI32(obj->tile);
            putI32(obj->elevation);
            putI32(obj->rotation);
        }
        endEvent();
    }

    void combatExit() override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_COMBAT_EXIT, EVENT_FLAG_STATE);
        endEvent();
    }

    void turnStart(Object* critter, bool isPlayer, int apAvailable, int deadlineMs) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_TURN_START, EVENT_FLAG_STATE);
        putI32(netIdOf(critter));
        putU8(isPlayer ? 1 : 0);
        putI32(apAvailable);
        putI32(deadlineMs); // stubbed 0 upstream: no turn timer exists yet (§2).
        // Bonus-move budget (COMBAT_CLIENT_DESIGN.md §3.0/§3.a): lets the viewer draw
        // the green bonus-move AP dots and the in-combat path preview's move budget.
        // Appended (length-prefixed events make appends forward/backward safe); a
        // client that predates this field simply stops reading before it. For the
        // dude's own turn this is authoritative; other actors' values are unused by
        // the viewer (only our own AP dots consume it).
        putI32(_combat_free_move);
        endEvent();
    }

    void attackResult(const Attack* attack) override
    {
        if (presenterEmissionsSuppressed() || attack == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[atk] SEND attacker=%d defender=%d hitMode=%d weaponPid=%d "
                                          "attackerFid=0x%x weapAnimNibble=%d\n",
            netIdOf(attack->attacker), netIdOf(attack->defender), attack->hitMode,
            attack->weapon != nullptr ? attack->weapon->pid : -1,
            attack->attacker != nullptr ? attack->attacker->fid : 0,
            // The armed state lives in the fid's weapon-animation nibble: 0 = unarmed.
            // weaponPid != -1 with nibble 0 means the sim thinks it has a weapon that its
            // BODY cannot hold — i.e. the inven_wield art failure above.
            attack->attacker != nullptr ? ((attack->attacker->fid & 0xF000) >> 12) : 0);
        // PRESENTATION, deliberately: presenter.h calls this "the causal envelope —
        // who hit whom, with what, where", NOT a second place damage is applied.
        // Every bit of STATE it implies (hp/AP/results/death) already rides
        // objectDelta, so a lagging client may skip it. It stays on the v1 wire
        // because it is the ONLY causal combat cue on the seam (§2's playAnim has no
        // corresponding virtual), so without it a thin client cannot render an
        // attack at all.
        beginEvent(EVENT_ATTACK_RESULT, 0);
        putI32(netIdOf(attack->attacker));
        putI32(netIdOf(attack->defender));
        putI32(attack->hitMode);
        putI32(attack->defenderHitLocation);
        putI32(attack->defenderDamage);
        putI32(attack->defenderFlags);
        putI32(attack->attackerDamage);
        putI32(attack->attackerFlags);
        // The whole multi-victim blast is ONE event (§2): the extras[] set.
        putU16((unsigned short)attack->extrasLength);
        for (int i = 0; i < attack->extrasLength; i++) {
            putI32(netIdOf(attack->extras[i]));
            putI32(attack->extrasDamage[i]);
            putI32(attack->extrasFlags[i]);
        }
        // Bug D: the fired weapon. The viewer replays the swing/fire/throw anim from
        // the ATTACKER's mirror inventory, but a weapon acquired after that viewer
        // joined is absent there → the attack replays as a punch even though the idle
        // fid shows it armed. Carry netId + pid so the consumer can resolve it, or
        // lazily recreate it in the mirror. netId 0 / pid -1 = unarmed. Appended after
        // the extras block; length-prefixed events make the tail-append safe.
        putI32(attack->weapon != nullptr ? netIdOf(attack->weapon) : 0);
        putI32(attack->weapon != nullptr ? attack->weapon->pid : -1);
        endEvent();
    }

    void weaponTakeOut(Object* critter, int weaponAnimationCode) override
    {
        if (presenterEmissionsSuppressed() || critter == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[takeout] SEND critter=%d weaponCode=%d\n",
            netIdOf(critter), weaponAnimationCode);
        beginEvent(EVENT_WEAPON_TAKE_OUT, 0);
        putI32(netIdOf(critter));
        putI32(weaponAnimationCode);
        endEvent();
    }

    void actionAnim(Object* actor, int anim) override
    {
        if (presenterEmissionsSuppressed() || actor == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[actionanim] SEND actor=%d anim=%d\n",
            netIdOf(actor), anim);
        beginEvent(EVENT_ACTION_ANIM, 0);
        putI32(netIdOf(actor));
        putI32(anim);
        endEvent();
    }

    void explosionFx(int tile, int elevation, const Attack* attack) override
    {
        if (presenterEmissionsSuppressed() || tile < 0 || attack == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[explosion] SEND tile=%d elev=%d defender=%d extras=%d\n",
            tile, elevation, netIdOf(attack->defender), attack->extrasLength);
        beginEvent(EVENT_EXPLOSION_FX, 0);
        putI32(tile);
        putI32(elevation);
        // The causal envelope for the victim death anims (mirrors attackResult, minus the
        // attacker — it is a transient the viewer re-creates locally). defender may be null
        // (a blast with no critter on its center tile, e.g. the Temple door) → netId 0.
        putI32(attack->defender != nullptr ? netIdOf(attack->defender) : 0);
        putI32(attack->defenderDamage);
        putI32(attack->defenderFlags);
        putU16((unsigned short)attack->extrasLength);
        for (int i = 0; i < attack->extrasLength; i++) {
            putI32(netIdOf(attack->extras[i]));
            putI32(attack->extrasDamage[i]);
            putI32(attack->extrasFlags[i]);
        }
        endEvent();
    }

    void presSeq(const unsigned char* data, int size, int opCount, int actorNetId) override
    {
        if (presenterEmissionsSuppressed() || data == nullptr || size <= 0) return;
        if (eventTraceEnabled()) fprintf(stderr, "[presseq] SEND ops=%d bytes=%d actor=%d\n", opCount, size, actorNetId);
        // Feature A: this recorded action's on-screen duration is (a) the presentation
        // cost stamped into this frame's outbox meta (§8.6 — was hard-stubbed 0), and
        // (b) the actor's non-blocking busy window. The busy mark is DEDICATED-server
        // only (the headless probe never marks → goldens byte-identical) and skips a
        // seq with no acting player (actorNetId 0 / a non-player critter). presRecordCostMs
        // is valid here: it rides the same buffer as `data`, before the next section.
        int costMs = presRecordCostMs();
        if ((unsigned int)costMs > _frameSeqCostMs) _frameSeqCostMs = (unsigned int)costMs;
        if (serverDedicatedActive() && actorNetId != 0) {
            serverActorBusyMarkByNetId(actorNetId, costMs);
        }
        beginEvent(EVENT_PRES_SEQ, 0);
        putI32(actorNetId); // primary actor to wait for (0 = none); precedes the op blob
        putU8(kPresStreamVersion);
        putU16((unsigned short)opCount);
        // Raw op bytes (already LE-encoded by the recorder, matching the Reader).
        _frame.insert(_frame.end(), data, data + size);
        endEvent();
    }

    void doorState(Object* door, bool opening, int targetFrame) override
    {
        if (presenterEmissionsSuppressed() || door == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[door] SEND net=%d opening=%d targetFrame=%d\n",
            netIdOf(door), opening ? 1 : 0, targetFrame);
        beginEvent(EVENT_DOOR_STATE, 0);
        putI32(netIdOf(door));
        putU8(opening ? 1 : 0);
        putI32(targetFrame);
        endEvent();
    }

    void stateAuditChunk(const void* records, int count, bool isFinal) override
    {
        if (presenterEmissionsSuppressed()) return;
        const StateAuditRecord* rec = (const StateAuditRecord*)records;
        beginEvent(EVENT_STATE_AUDIT, 0);
        putU8(isFinal ? 1 : 0);
        putU16((unsigned short)count);
        for (int i = 0; i < count; i++) {
            putI32(rec[i].netId);
            putI32(rec[i].pid);
            putI32(rec[i].tile);
            putI32(rec[i].elevation);
            putI32(rec[i].fid);
            putI32(rec[i].frame);
            putI32(rec[i].rotation);
            putI32((int)rec[i].flags);
            putI32(rec[i].lightDistance);
            putI32(rec[i].lightIntensity);
            putI32(rec[i].hp);
            putI32(rec[i].radiation);
            putI32(rec[i].poison);
            putI32(rec[i].ap);
            putI32(rec[i].combatResults);
            putI32(rec[i].inventoryCount);
            putI32((int)rec[i].inventoryHash);
        }
        endEvent();
        if (isFinal) flushFrame();
    }

    void lootGrant(int actorNetId, int containerNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[lootgrant] SEND actor=%d container=%d\n",
            actorNetId, containerNetId);
        beginEvent(EVENT_LOOT_GRANT, 0);
        putI32(actorNetId);
        putI32(containerNetId);
        endEvent();
        // Flushed immediately, like the inventory grant: it answers a click that has
        // already been paid for, and the screen it opens is where the player is going.
        flushFrame();
    }

    void inventoryGrant(int actorNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[invgrant] SEND actor=%d\n", actorNetId);
        beginEvent(EVENT_INVENTORY_GRANT, 0);
        putI32(actorNetId);
        endEvent();
        // Flush now rather than at the frame tail: this is a direct answer to a
        // keypress that has already cost the player 4 AP, and the screen it opens
        // is where their turn continues. Same reasoning as the dialog events.
        flushFrame();
    }

    void inventoryRevoke(int actorNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[invgrant] REVOKE actor=%d\n", actorNetId);
        beginEvent(EVENT_INVENTORY_REVOKE, 0);
        putI32(actorNetId);
        endEvent();
        flushFrame(); // the screen must close now, not at the next frame tail
    }

    void dialogNode(Object* speaker, Object* driver, int reaction,
        const char* reply, const char* const* options, int optionCount,
        const char* audioFileName = nullptr, int headFid = -1,
        const int* optionReactions = nullptr, bool speakerIsPartyMember = false) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[dialog] SEND node speaker=%d driver=%d reaction=%d headFid=0x%X audio=%s opts=%d\n",
            netIdOf(speaker), netIdOf(driver), reaction, headFid,
            audioFileName != nullptr && audioFileName[0] != '\0' ? audioFileName : "-", optionCount);
        beginEvent(EVENT_DIALOG_NODE, 0);
        putI32(netIdOf(speaker));
        putI32(netIdOf(driver));
        putI32(reaction);
        putString(reply);
        putString(audioFileName != nullptr ? audioFileName : "");
        putI32(headFid);
        if (optionCount < 0) optionCount = 0;
        if (optionCount > 0xFFFF) optionCount = 0xFFFF;
        putU16((unsigned short)optionCount);
        for (int i = 0; i < optionCount; i++) {
            putString(options != nullptr && options[i] != nullptr ? options[i] : "");
            // Per-option reaction rides WITH its option so the two can never drift out
            // of step, and so a truncated list truncates both halves together.
            putI32(optionReactions != nullptr ? optionReactions[i] : GAME_DIALOG_REACTION_NEUTRAL);
        }
        // TRAILING: the viewer has no party list (that is full-save state), so it
        // cannot tell a companion from an NPC and never built the party-member
        // dialog window (the one with the Combat Control button). Older readers
        // stop before this byte and are unaffected.
        putU8(speakerIsPartyMember ? 1 : 0);
        endEvent();
        // Modal: the server tick is BLOCKED in the dialog barrier for the whole
        // conversation, so no beatEnd will flush this. Ship it now (mirrors
        // snapshotEnd's forced flush).
        flushFrame();
    }

    void moviePlay(int movie, int flags) override
    {
        // ►► DELIBERATELY NOT gated on presenterEmissionsSuppressed(), unlike every
        // other override. Suppression silences per-object DELTAS while a map is torn
        // down and rebuilt (map.cc wraps the whole load in it) — noise nobody needs.
        // A movie is the opposite: right after this returns the server BLOCKS in
        // gameMovieServerBarrier waiting for the viewer's `movdone`. The Temple-exit
        // VSUIT movie is played by a map-ENTER script, i.e. INSIDE that suppression
        // window — so bailing here meant the client never learned a movie was playing,
        // never acked, and the server spun the barrier until the viewer was KILLED
        // (the clientCount()==0 bail), which then jumped straight to the next map. A
        // modal event the tick blocks on must always go out.
        beginEvent(EVENT_MOVIE_PLAY, 0);
        putI32(movie);
        putI32(flags);
        endEvent();
        // Modal, exactly like dialogNode: the server tick is parked in the movie
        // barrier until a viewer reports back, so no beatEnd will carry this.
        flushFrame();
    }

    void movieStop() override
    {
        // Modal, like moviePlay: the server was parked in the movie barrier and just
        // freed on the first ack, so no beatEnd will carry this — ship it now so the
        // OTHER viewers' blocking movie loops break this beat, not whenever they next
        // happen to flush.
        beginEvent(EVENT_MOVIE_STOP, 0);
        endEvent();
        flushFrame();
    }

    void encounterPrompt(const char* title, const char* body) override
    {
        // Modal, like moviePlay/dialogNode: right after this the server BLOCKS in the
        // encounter barrier waiting for encaccept/encdecline, so it must go out now —
        // no beatEnd will carry it. Not gated on suppression (same reasoning as movies).
        beginEvent(EVENT_ENCOUNTER_PROMPT, 0);
        putString(title != nullptr ? title : "");
        putString(body != nullptr ? body : "");
        endEvent();
        flushFrame();
    }

    void encounterClose() override
    {
        // Modal, like movieStop: the barrier just freed on the first answer, so ship
        // it now to break the OTHER viewers' blocking prompt boxes this beat.
        beginEvent(EVENT_ENCOUNTER_CLOSE, 0);
        endEvent();
        flushFrame();
    }

    bool wantsSheetDeltas() override { return true; }

    void playerSheetDelta(int slot, const unsigned char* data, int len) override
    {
        if (data == nullptr || len <= 0 || len > 0xFFFF) return;
        // Rides the beat like objectDelta (beatEnd flushes it); the consumer drops
        // it if the join blob has not landed yet (the blob carries a fresher row).
        beginEvent(EVENT_PLAYER_SHEET, EVENT_FLAG_STATE);
        putI32(slot);
        putU16((unsigned short)len);
        for (int i = 0; i < len; i++) {
            putU8(data[i]);
        }
        endEvent();
    }

    void movieSeenState(const unsigned char* seen, int count) override
    {
        if (seen == nullptr || count < 0 || count > 0xFFFF) return;
        // NOT gated on presenterEmissionsSuppressed(): emitted from serverEmitBaseline
        // (outside a map-load window), and it is authoritative world state a joiner
        // must not miss — same reasoning as the roster it rides beside.
        beginEvent(EVENT_MOVIE_SEEN_STATE, EVENT_FLAG_STATE);
        putU16((unsigned short)count);
        for (int i = 0; i < count; i++) {
            putU8(seen[i]);
        }
        endEvent();
    }

    void dialogEnd(Object* driver) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[dialog] SEND end driver=%d\n", netIdOf(driver));
        beginEvent(EVENT_DIALOG_END, 0);
        putI32(netIdOf(driver));
        endEvent();
        flushFrame();
    }

    // ---- Barter ------------------------------------------------------------
    // All three FLUSH, like the dialog node and for the same reason: the server
    // is BLOCKED in the barter barrier when it emits them, so nothing else will
    // push the frame out and viewers would sit on a stale screen until the trade
    // ended. PRESENTATION-flagged — the authoritative item movement rides
    // OBJECT_DELTA_INVENTORY once the tick resumes.
    void barterBegin(Object* merchant, Object* driver) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[barter] SEND begin merchant=%d driver=%d\n", netIdOf(merchant), netIdOf(driver));
        beginEvent(EVENT_BARTER_BEGIN, 0);
        putI32(netIdOf(merchant));
        putI32(netIdOf(driver));
        endEvent();
        flushFrame();
    }

    void barterState(const BarterView& view) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[barter] SEND state dinv=%d minv=%d pt=%d mt=%d offer=%d asking=%d\n",
            view.driverInvCount, view.merchantInvCount, view.playerTableCount, view.merchantTableCount,
            view.offerValue, view.askingValue);
        beginEvent(EVENT_BARTER_STATE, 0);
        // Counts precede their rows so the decoder never hand-counts a row size —
        // that drift is what silently dropped the last row of every PLAYER_ROSTER
        // (encoder 15 bytes/row vs a decoder guard of 17). One row = exactly two
        // i32s, and the count says how many; nothing here is re-derived.
        auto putList = [&](const BarterStack* rows, int count) {
            putI32(count);
            for (int i = 0; i < count; i++) {
                putI32(rows[i].pid);
                putI32(rows[i].quantity);
            }
        };
        putList(view.driverInv, view.driverInvCount);
        putList(view.merchantInv, view.merchantInvCount);
        putList(view.playerTable, view.playerTableCount);
        putList(view.merchantTable, view.merchantTableCount);
        putI32(view.offerValue);
        putI32(view.askingValue);
        putI32(view.resultCode); // last-commit result, -1 for a plain move (append-only)
        endEvent();
        flushFrame();
    }

    void barterEnd() override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[barter] SEND end\n");
        beginEvent(EVENT_BARTER_END, 0);
        endEvent();
        flushFrame();
    }

    // ---- Steal / pickpocket / plant -----------------------------------------
    // All three FLUSH for the barter reason: the server is BLOCKED in the steal
    // barrier when it emits them. Unlike barter, STATE carries nothing — the item
    // movement rides OBJECT_DELTA_INVENTORY, which the session scans for
    // immediately before this call; the flush here is what ships those deltas.
    void stealBegin(Object* thief, Object* target) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[steal] SEND begin thief=%d target=%d\n", netIdOf(thief), netIdOf(target));
        beginEvent(EVENT_STEAL_BEGIN, 0);
        putI32(netIdOf(thief));
        putI32(netIdOf(target));
        endEvent();
        flushFrame();
    }

    void stealState() override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_STEAL_STATE, 0);
        endEvent();
        flushFrame();
    }

    void stealEnd() override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[steal] SEND end\n");
        beginEvent(EVENT_STEAL_END, 0);
        endEvent();
        flushFrame();
    }

    void worldmapBegin() override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[worldmap] SEND begin\n");
        beginEvent(EVENT_WORLDMAP_BEGIN, 0);
        endEvent();
        flushFrame();
    }

    void worldmapEnd() override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[worldmap] SEND end\n");
        beginEvent(EVENT_WORLDMAP_END, 0);
        endEvent();
        flushFrame();
    }

    void worldmapState(int worldPosX, int worldPosY, int walkDestX, int walkDestY,
        bool isWalking, int walkDistance, int carFuel, int currentAreaId, bool isInCar,
        int currentAreaVisitedState = 0, unsigned int currentAreaEntranceMask = 0) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (eventTraceEnabled()) fprintf(stderr, "[worldmap] SEND state pos=%d,%d dst=%d,%d walk=%d dist=%d area=%d\n",
            worldPosX, worldPosY, walkDestX, walkDestY, isWalking ? 1 : 0, walkDistance, currentAreaId);
        beginEvent(EVENT_WORLDMAP_STATE, 0);
        putI32(worldPosX);
        putI32(worldPosY);
        putI32(walkDestX);
        putI32(walkDestY);
        putU8(isWalking ? 1 : 0);
        putI32(walkDistance);
        putI32(carFuel);
        putI32(currentAreaId);
        putU8(isInCar ? 1 : 0);
        putI32(currentAreaVisitedState);
        putI32((int)currentAreaEntranceMask);
        endEvent();
        flushFrame();
    }

    void worldmapAreas(const int* states, const int* visited, const unsigned int* entranceMasks, int count) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (count <= 0 || states == nullptr || visited == nullptr || entranceMasks == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[worldmap] SEND areas n=%d\n", count);
        beginEvent(EVENT_WORLDMAP_AREAS, 0);
        putU16((unsigned short)count);
        for (int i = 0; i < count; i++) {
            putU8((unsigned char)states[i]);
            putU8((unsigned char)visited[i]);
            putI32((int)entranceMasks[i]);
        }
        endEvent();
    }

    void worldmapSubtiles(const unsigned char* states, int count) override
    {
        if (presenterEmissionsSuppressed()) return;
        if (count <= 0 || states == nullptr) return;
        if (eventTraceEnabled()) fprintf(stderr, "[worldmap] SEND subtiles n=%d\n", count);
        beginEvent(EVENT_WORLDMAP_SUBTILES, 0);
        putU16((unsigned short)count);
        for (int i = 0; i < count; i++) {
            putU8(states[i]);
        }
        endEvent();
        flushFrame();
    }

    // -- PRESENTATION cues --------------------------------------------------
    // These gate on suppression too, which narrate does NOT bother to do. It
    // matters here: mapLoad runs the MAP_ENTER scripts (map.cc:786/832) INSIDE the
    // suppression window, and those scripts emit console/float text. Withholding
    // the lifecycle flood while shipping its chatter would put a joining client's
    // message log out of step with its world.

    void consoleMessage(const char* text) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_CONSOLE, 0);
        putString(text);
        endEvent();
    }

    void consoleMessageFor(int actorNetId, const char* text) override
    {
        if (presenterEmissionsSuppressed()) return;
        // Unaddressed → emit the LEGACY layout byte-for-byte (no trailing field).
        // Every existing broadcast call site keeps its exact wire bytes, so the
        // netstream goldens are untouched by this addition.
        if (actorNetId == 0) {
            consoleMessage(text);
            return;
        }
        beginEvent(EVENT_CONSOLE, 0);
        putString(text);
        putI32(actorNetId); // TRAILING: absent on broadcast, read only if bytes remain
        endEvent();
    }

    void consoleMessageStyled(int actorNetId, int channel, const char* text) override
    {
        if (presenterEmissionsSuppressed()) return;

        // SECOND trailing field, on the same principle as the address above: the
        // default channel emits the SHORTER legacy layout, so every message that
        // existed before channels did keeps its exact bytes and no netstream golden
        // moves. A styled line pays for the address field even when broadcasting
        // (actorNetId 0), because the reader is positional and cannot skip it.
        if (channel == kMsgChannelDefault) {
            consoleMessageFor(actorNetId, text);
            return;
        }

        beginEvent(EVENT_CONSOLE, 0);
        putString(text);
        putI32(actorNetId);
        putI32(channel);
        endEvent();
    }

    void floatText(Object* owner, const char* text, int font, int color, int outlineColor) override
    {
        if (presenterEmissionsSuppressed()) return;
        // Owner by netId, NEVER the pointer (MP_PROTOCOL.md §6.4). font/color are
        // client-local styling and stay off the wire.
        beginEvent(EVENT_FLOAT_TEXT, 0);
        putI32(netIdOf(owner));
        putString(text);
        endEvent();
    }

    void sfxPlay(const char* name) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_SFX, 0);
        putString(name);
        endEvent();
    }

    void sfxPlayAt(const char* name, Object* source) override
    {
        if (presenterEmissionsSuppressed()) return;
        // Positional volume is computed CLIENT-side from the source's position
        // (the legacy _gsound_compute_relative_volume is camera-relative, and the
        // server has no camera). The wire carries the source, not a volume.
        beginEvent(EVENT_SFX_AT, 0);
        putString(name);
        putI32(netIdOf(source));
        endEvent();
    }

    // ►►►► A FADE MUST BRACKET THE CHANGE IT IS HIDING, AND IT WAS BRACKETING NOTHING.
    // The script fades out, mutates the world (digs the grave, moves the actor, passes
    // the time), then fades in — but the MUTATION only reaches the wire as an
    // objectDelta at the END of the beat, i.e. after BOTH fade events. So the frame
    // said: fade out, fade in, and only then "the grave is now open". The owner saw
    // exactly that — the grave popped open and THEN the screen went black, which is
    // the fade doing the opposite of its job.
    //
    // Flushing the pending deltas at each fade boundary puts the beat in the order the
    // script wrote it: FADE_OUT, [everything the script changed], FADE_IN. Same
    // move stealEmitState makes for the same reason — a scan is the only way to get
    // state out of a tick that is parked inside a script. Flushing on the way OUT
    // matters too: whatever happened BEFORE the fade should land while the screen can
    // still show it, not get swept past the black with everything else.
    void screenFadeOut(int actorNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        objectDeltaScan();
        beginEvent(EVENT_FADE_OUT, 0);
        putI32(actorNetId);
        endEvent();
        flushFrame();
    }

    void screenFadeIn(int actorNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        objectDeltaScan(); // see screenFadeOut: the change happens BETWEEN the two
        beginEvent(EVENT_FADE_IN, 0);
        putI32(actorNetId);
        endEvent();
        flushFrame(); // the black period ends here; do not sit on it until the tail
    }

    // Flushed like the fades, and for the same reason: the lock brackets a cutscene the
    // script is about to play, so it has to land BEFORE the things it is meant to stop
    // the player interrupting — sitting in the outbox until the tail defeats the point.
    void screenInputLock(bool locked, int actorNetId) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_UI_LOCK, 0);
        putI32(locked ? 1 : 0);
        putI32(actorNetId);
        endEvent();
        flushFrame();
    }

    void errorBox(const char* text) override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_ERROR_BOX, 0);
        putString(text);
        endEvent();
    }

    void musicStop() override
    {
        if (presenterEmissionsSuppressed()) return;
        beginEvent(EVENT_MUSIC_STOP, 0);
        endEvent();
    }

    int musicPlayLevel(const char* fileName, int fadeIn) override
    {
        if (presenterEmissionsSuppressed()) return 0;
        beginEvent(EVENT_MUSIC_PLAY, 0);
        putString(fileName);
        putU8((unsigned char)fadeIn);
        endEvent();

        // 0, always: the SERVER has no audio device, so it cannot fail and must
        // not report failure — wmMapMusicStart treats -1 as "couldn't start map
        // music" and logs a worldmap error. Whether a viewer actually managed to
        // play the track is not the sim's business.
        return 0;
    }

    // NOT OVERRIDDEN, deliberately (verified 2026-07-15, whole-tree call-site grep):
    //  * ambientSoundLoad — map.cc:628 is its SOLE core call site and every argument
    //    is a compile-time constant: ambientSoundLoad("wind2", 12, 13, 16). The event
    //    carries ZERO information; a client re-derives ambient from mapTransition's
    //    mapIndex. Dropping it also dissolves a causal-order wart: it fires at the TOP
    //    of mapLoad, BEFORE the mapTransition emit, so a client would be told to load
    //    the new map's ambience before being told to drop the old world. Reordering the
    //    core call is not an option — it would break ClientPresenter's legacy call
    //    order, which the goldens pin.
    //    (musicPlayLevel WAS listed here as having zero core call sites. That stopped
    //    being true the moment wmMapMusicStart was routed through the presenter instead
    //    of calling _gsound_background_play_level_music directly — it is carried above
    //    now. Re-grep before trusting any entry in this list.)
    //  * The ~19 client-local chrome virtuals (worldInvalidate*/cursor*/scroll*/
    //    mouseObjects*/hud*/worldClear/worldEnable/worldDisable/movieFadeOut/
    //    mouseResetBouncingCursor) — dropped from the wire by §6.4 / H-31.

    // -- frame boundary -----------------------------------------------------

    void beatEnd(int tick) override
    {
        // NOT suppression-gated: the frame boundary must always close. (A beat whose
        // events were all withheld simply flushes nothing — see flushFrame.)
        flushFrame();
    }

private:
    // ---- encoding primitives (little-endian, packed) ----

    void putU8(unsigned char v) { _frame.push_back(v); }

    void putU16(unsigned short v)
    {
        _frame.push_back((unsigned char)(v & 0xFF));
        _frame.push_back((unsigned char)((v >> 8) & 0xFF));
    }

    void putU32(unsigned int u)
    {
        _frame.push_back((unsigned char)(u & 0xFF));
        _frame.push_back((unsigned char)((u >> 8) & 0xFF));
        _frame.push_back((unsigned char)((u >> 16) & 0xFF));
        _frame.push_back((unsigned char)((u >> 24) & 0xFF));
    }

    void putI32(int v)
    {
        unsigned int u = (unsigned int)v;
        _frame.push_back((unsigned char)(u & 0xFF));
        _frame.push_back((unsigned char)((u >> 8) & 0xFF));
        _frame.push_back((unsigned char)((u >> 16) & 0xFF));
        _frame.push_back((unsigned char)((u >> 24) & 0xFF));
    }

    // u16 length + RAW bytes. NOT UTF-8 — see the header block.
    //
    // The 0xFFFF clamp only keeps the LENGTH FIELD honest; it is not a working
    // truncation path. A string that long pushes the event payload past 0xFFFF, so
    // endEvent drops the whole event rather than emit a corrupt length. No engine
    // string comes near this; both paths are unreachable and fail closed.
    void putString(const char* text)
    {
        size_t length = text != nullptr ? strlen(text) : 0;
        if (length > 0xFFFF) {
            length = 0xFFFF;
        }
        putU16((unsigned short)length);
        if (length != 0) {
            _frame.insert(_frame.end(), (const unsigned char*)text, (const unsigned char*)text + length);
        }
    }

    // The owner's TOP-LEVEL inventory, serialized in full.
    //
    // Why in full: OBJECT_DELTA_INVENTORY is a CHANGE-DETECTION bit, not a payload.
    // The shadow keeps only a 32-bit order-independent fingerprint
    // (objectInventoryHash, object_delta.cc) — it can say THAT an inventory changed,
    // never WHAT changed. NarratePresenter sidesteps this by printing just the item
    // count; the wire cannot. So the bit means "re-send the list".
    //
    // ⚠ TOP-LEVEL ONLY, matching the fingerprint's own scope: an item nested inside a
    // carried container is not walked, so a container's contents can change with NO
    // delta bit raised. Consistent with the snapshot oracle (state_dump is partial
    // the same way) → no join-vs-stream divergence, and NOT a new hole. But it IS
    // wire-visible, and no gate can see it: both serializations share the blind spot.
    // Closing it needs a recursive fingerprint, not a serializer change.
    void putInventory(Object* owner)
    {
        const Inventory* inventory = &(owner->data.inventory);
        int length = inventory->length;
        if (length < 0) {
            length = 0;
        }
        putU16((unsigned short)length);
        for (int i = 0; i < length; i++) {
            const InventoryItem* entry = &(inventory->items[i]);
            const Object* item = entry->item;
            putI32(item != nullptr ? item->netId : kNoNetId);
            putI32(item != nullptr ? item->pid : -1);
            putI32(entry->quantity);
            putI32(item != nullptr ? (int)item->flags : 0);
            // Per-item ammo (wire v3). The ItemObjectData union aliases its first int
            // across weapon.ammoQuantity / ammo.quantity / misc.charges, so reading
            // .weapon.ammoQuantity yields the loaded-round / charge count for ANY item
            // type; the second int (.weapon.ammoTypePid) is meaningful only for a
            // weapon but sits inside the union either way, so it is safe to read
            // unconditionally (the decoder applies it weapon-only). Without this the
            // change-detection hash (object_delta.cc folds ammoQuantity) fires a delta
            // on every fire/reload, but the count itself never crossed the wire, so the
            // client rebuilt items at proto-default ammo — a fired-dry weapon looked
            // full. This is the live-delta half; the join blob already carries ammo.
            putI32(item != nullptr ? item->data.item.weapon.ammoQuantity : 0);
            putI32(item != nullptr ? item->data.item.weapon.ammoTypePid : 0);
        }
    }

    // ---- framing ----

    void beginEvent(EventType type, unsigned char flags)
    {
        _eventStart = _frame.size();
        // Reserve the header; the length is backfilled in endEvent.
        putU8((unsigned char)type);
        putU8(flags);
        putU16(0);
    }

    void endEvent()
    {
        size_t payload = _frame.size() - _eventStart - 4;
        if (payload > 0xFFFF) {
            // Unreachable for every event above; refuse rather than emit a corrupt
            // length that would desynchronize the reader for the rest of the stream.
            _frame.resize(_eventStart);
            return;
        }
        _frame[_eventStart + 2] = (unsigned char)(payload & 0xFF);
        _frame[_eventStart + 3] = (unsigned char)((payload >> 8) & 0xFF);
        _eventCount++;
    }

    // Emit the accumulated beat as ONE sequenced frame.
    //
    // BUFFER-AND-FLUSH IS MANDATORY, not an optimization: objectDelta is emitted
    // from INSIDE objectDeltaScan's objectFindFirst/objectFindNext walk, which uses
    // a GLOBAL find iterator. A callback that spawned/destroyed an object or started
    // its own object walk would invalidate that iterator mid-scan (object_delta.cc
    // says so explicitly). Accumulating bytes touches no engine state, so the
    // callbacks stay pure reads; the sink is only ever touched here, after the walk.
    void flushFrame()
    {
        if (_sink == nullptr || _eventCount == 0) {
            // Nothing resolved this beat: emit no frame at all. Frames are sparse —
            // seq counts FRAMES, not beats, so an idle beat costs zero bytes and the
            // consumer's gap detection is unaffected.
            _frame.clear();
            _eventCount = 0;
            return;
        }

        // NOTE: eventCount is u16. Largest observed frame is a klatoxcv baseline at
        // 3659 events; a >65535-event beat would wrap the count while the payload kept
        // its bytes, which the decoder catches as "trailing bytes" (fails loud, not
        // silent). Documented rather than widened: the wire is versioned.
        unsigned char header[18];
        writeU32(header + 0, _seq);
        writeU32(header + 4, simClockNow());
        writeU32(header + 8, (unsigned int)_frame.size());
        header[12] = (unsigned char)(_eventCount & 0xFF);
        header[13] = (unsigned char)((_eventCount >> 8) & 0xFF);
        // Wire v4: entryBase = total-order id of this frame's first event. Dense over
        // EVENTS (not frames), so entryId(event e) = entryBase + e is monotonic across
        // the whole stream — the id the outbox + hash-ack key on (wire_defs.h).
        writeU32(header + 14, _entryBase);

        // Presentation cost of this frame = MAX over actor lanes (concurrent movers
        // overlap) — plus any presSeq cost stamped this frame. The socket sink uses it to
        // defer the NEXT frame's release; the file sink ignores the whole meta (§8.6).
        //
        // METERING IS COMBAT-ONLY (§8.6 weld (b)). Out of combat the stepped-walk engine
        // mutates positions at wall-clock cadence (serverWalkBeatsPerStep, server_anim.cc
        // :635/:710 gate on !isInCombat()), so the sim ALREADY paces each hop 1:1 real time
        // and the deltas leave the encoder correctly spaced. Chaining their glide durMs in
        // the outbox on top of that double-paces: with a mover in almost every beat (a
        // populated map), the single per-client release chain (server_net.cc:297) advances
        // ~durMs per beat while wall advances one beat interval, so the backlog — and the
        // player's own click latency behind it — grows without bound. §8.6(b) called this
        // exactly: "deferring delivery diverges the live view from the live sim linearly."
        // In combat the whole AI turn BURSTS out in one beat (§8.6 weld (a)); there the
        // outbox is the only thing pacing it, so costMs must chain. The hop's durMs still
        // rides the wire below either way — metering gates emission timing, not playback.
        WireFrameMeta meta;
        meta.seq = _seq;
        meta.entryBase = _entryBase;
        meta.eventCount = _eventCount;
        meta.simTs = simClockNow();
        meta.mapGeneration = mapGetLoadGeneration();
        unsigned int costMs = 0;
        if (isInCombat()) {
            costMs = _frameSeqCostMs;
            for (const auto& kv : _frameMoveCostByActor) {
                if (kv.second > costMs) costMs = kv.second;
            }
        }
        meta.costMs = costMs;

        _sink->writeFrame(header, sizeof(header), _frame.data(), (unsigned int)_frame.size(), meta);
        _sink->flush();

        _seq++;
        _entryBase += _eventCount;
        _frame.clear();
        _eventCount = 0;
        _frameMoveCostByActor.clear();
        _frameSeqCostMs = 0;
    }

    static void writeU32(unsigned char* out, unsigned int v)
    {
        out[0] = (unsigned char)(v & 0xFF);
        out[1] = (unsigned char)((v >> 8) & 0xFF);
        out[2] = (unsigned char)((v >> 16) & 0xFF);
        out[3] = (unsigned char)((v >> 24) & 0xFF);
    }

    ByteSink* _sink = nullptr;
    std::vector<unsigned char> _frame;
    unsigned int _seq = 0;
    unsigned int _entryBase = 0; // wire v4: total-order id of the next frame's first event
    unsigned short _eventCount = 0;
    size_t _eventStart = 0;
    // Presentation-cost tally for the current frame (outbox §8.6). Per-actor summed move
    // duration; frame cost = max over actors (+ _frameSeqCostMs). Cleared each flushFrame.
    std::unordered_map<int, unsigned int> _frameMoveCostByActor;
    unsigned int _frameSeqCostMs = 0; // presSeq cost stamped this frame (0 until scheduling increment)
};

// Static storage, mirroring the other two presenters (presenterSet never owns).
static NetworkPresenter gNetworkPresenter;
static FileByteSink gFileSink;
static bool gFileSinkOpen = false;

// STEP 3: the socket sink f2_server pre-registers (server_net.cc). Never owned
// here; f2_server's server_main creates it, keeps it alive across the serve
// loop, and clears this before destroying it.
static ByteSink* gServerSink = nullptr;

void presenterSetServerSink(ByteSink* sink)
{
    gServerSink = sink;
}

ByteSink* presenterServerSink()
{
    return gServerSink;
}

// See presenter_network.h. Pending durMs for the next objectMoved; the stepped-
// walk engine brackets its objectSetLocation with set(ms)/set(0), so it can never
// go stale onto an unrelated move.
static int gNextMoveDurationMs = 0;

void presenterSetNextMoveDurationMs(int ms)
{
    gNextMoveDurationMs = ms;
}

int presenterNextMoveDurationMs()
{
    return gNextMoveDurationMs;
}

// Same bracketed pending-value seam; see presenter_network.h. Default false:
// every mover that does not declare itself a runner is a walker, which is what
// the old durMs==0 (snap) movers effectively were.
static bool gNextMoveRun = false;

void presenterSetNextMoveRun(bool run)
{
    gNextMoveRun = run;
}

bool presenterNextMoveRun()
{
    return gNextMoveRun;
}

bool presenterInstallNetworkSink(ByteSink* sink)
{
    if (sink == nullptr) {
        return false;
    }
    gNetworkPresenter.begin(sink);
    presenterSet(&gNetworkPresenter);
    return true;
}

bool presenterInstallNetwork(const char* path)
{
    if (path == nullptr) {
        return false;
    }
    if (!gFileSink.open(path)) {
        // Loud, and install NOTHING — the caller falls back to the null presenter.
        //
        // Returning void here was a real bug (adversarial review 2026-07-15): it left
        // serverInstall's if/else with no branch taken, so whatever presenter was live
        // BEFORE the loop stayed installed. In the probe that is ClientPresenter
        // (game_lifecycle.cc:100) — the full presentation layer running headless under
        // the server loop, measured at 2x wall-clock, silently violating the interlock
        // server_loop.cc documents. The dangerous fallback was never "an empty wire";
        // it was the client.
        fprintf(stderr, "f2: F2_NETSTREAM: cannot open '%s' for writing\n", path);
        return false;
    }
    gFileSinkOpen = true;
    return presenterInstallNetworkSink(&gFileSink);
}

void presenterUninstallNetwork()
{
    if (presenter() == &gNetworkPresenter) {
        gNetworkPresenter.end();
    }
    if (gFileSinkOpen) {
        gFileSink.close();
        gFileSinkOpen = false;
    }
}

} // namespace fallout
