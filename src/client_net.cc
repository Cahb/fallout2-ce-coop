#include "client_net.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Transport portability: platform_net.h owns the Winsock-vs-BSD differences so the
// code below is written once. Previously this whole file's transport was compiled
// OUT on Windows (`#ifndef _WIN32` ... `return false`), which meant a Windows build
// ran single player fine but could never join a server.
#include "platform_net.h"

#include "client_present.h" // one owner for glide / attack-replay / door-slide presentation
#include "client_barter.h"
#include "client_dialog.h" // dialog viewer render (A3 — DIALOG_STREAMING_PLAN Stage 3)
#include "client_steal.h" // viewer half of a server-owned steal session
#include "worldmap_ui.h" // wmGenData + gWorldmapStreaming/gPendingWorldmapEnter/gWorldmapStateDirty
#include "color.h" // _colorTable — float-text styling (COMBAT_CLIENT_DESIGN.md §3.e)
#include "combat.h" // gCombatState mirror + COMBAT_STATE_* (§3.0)
#include "combat_defs.h" // Attack / EXPLOSION_TARGET_COUNT — ATTACK_RESULT reconstruct
#include "elevator.h" // elevatorPickLevel — the co-op elevator panel
#include "db.h"
#include "debug.h"
#include "display_monitor.h" // combat message log (§3.e S2)
#include "game.h" // GameMode — modal-screen detection for the viewer service ticker
#include "game_mouse.h" // wait-watch cursor over combat transitions (§3.a)
#include "game_sound.h" // sfx (§3.e S2)
#include "audio_engine.h"
#include "input.h" // enqueueInputEvent / tickersAdd — viewer modal service ticker
#include "interface.h" // combat HUD bar hooks (§3.a)
#include "inventory.h" // _inven_reset_dude — re-anchor the inventory on a local-actor rebind
#include "inventory_ui.h" // gInventory{Left,Right}HandItem/Armor — parked-equip detach check (Slice 3b)
#include "item.h" // critterGetWeaponForHitMode — ATTACK_RESULT reconstruct (§3.c)
#include "dbox.h" // showDialogBox / DIALOG_BOX_* — streamed random-encounter prompt
#include "kb.h" // KEY_ESCAPE — force-close a viewer modal on combat/rebaseline
#include "light.h" // lightSetAmbientIntensity — streamed global ambient-light delta
#include "map.h"
#include "actions.h" // actionExplodeReplay + actionPresReplayShowDeath — viewer explosion replay
#include "automap.h" // automapSaveCurrent — a viewer records its OWN pipboy map
#include "animation.h" // reg_anim_* / animationRegister* — the real engine the recorded stream drives
#include "object.h"
#include "pipboy.h" // pipboyServerHolodisk* — server-authored holodisks
#include "perk.h" // perkPlayerActorSeedRanks — per-actor sheet rows
#include "player_sheet.h"
#include "pres_record.h" // PresOp / PresCallbackTag — the recorded op stream vocabulary
#include "game_movie.h" // gameMoviePlay — the viewer owns the playback pipeline
#include "movie.h" // _movieStop — break the blocking playback loop on a room-wide skip
#include "msg_channel.h"
#include "palette.h" // paletteFadeTo / gPaletteBlack — fades apply at decode, in wire order
#include "presenter.h"
#include "proto.h" // protoPlayerActorSheetsSeed — per-actor sheet rows
#include "scripts.h"
#include "state_audit.h" // StateAuditRecord / stateAuditCompare — the mirror divergence oracle
#include "server_players.h"
#include "settings.h" // target_highlight pref — vanilla outline gate (#8)
#include "stat.h" // pcPlayerActorSeedStats — per-actor sheet rows
#include "critter.h" // critterPlayerActorSeedNames — per-actor sheet rows
#include "skill.h" // skillsPlayerActorSeed — per-actor sheet rows
#include "trait.h" // traitsPlayerActorSeed — per-actor sheet rows
#include "text_object.h" // floating combat text (§3.e S2)
#include "tile.h" // tileWindowRefreshRect (float-text redraw)
#include "wire_defs.h" // kWireVersion / kWirePreambleLen / kNoSessionId

namespace fallout {

// clientViewerActive()/clientViewerSetActive() are defined in server_loop.cc
// (f2_core) so core code (object.cc combat-mirror guards) can link them on the
// server build too; declared in client_net.h.

// Player-UI Slice 3b — state shared between the dude-inventory reconcile (decoder,
// onObjectDelta) and the viewer inventory screen (inventory_ui.cc / main.cc):
//   • gDudeInvDirty — set when a live reconcile mutated the dude mirror while a
//     screen is open, so the open inventory list redraws (it only repaints on user
//     events, so an async drop would otherwise linger visibly).
//   • gDudeDeferredItemFrees — items unlinked by the reconcile while a modal is open
//     are unlinked immediately (so they leave the list) but their objectDestroy is
//     DEFERRED until the modal closes: the inventory drag / ctx-menu handlers hold a
//     raw Object* across their inner pump loops, and freeing it mid-handler (the
//     ticker pumps the wire there) would dangle it. Flushed by main.cc after the
//     screen closes and at ticker teardown.
static bool gDudeInvDirty = false;
static std::vector<Object*> gDudeDeferredItemFrees;

// Sheet slice (PLAYER_SHEET_DESIGN.md §9) — set when a player-sheet row lands off
// the wire, drained by the character screen's loop so an open screen repaints from
// the new row. It is the RETURN PATH for a spend: the screen sends an intent, keeps
// no optimistic state, and shows the server's answer when this flips.
static bool gPlayerSheetDeltaDirty = false;

// Loot slice — the container/corpse the viewer's loot screen is currently open on
// (0 = none). Set by inventoryOpenLooting; its inventory delta gets a FULL contents
// reconcile (below) so items taken/added show live, instead of the equip-flags-only
// path the generic non-dude reconcile uses. gLootTargetInvDirty tells the open loot
// loop to repaint its panels when that reconcile lands.
static int gViewerLootTargetNetId = 0;
static bool gLootTargetInvDirty = false;

// Steal slice — the THIEF of the open steal session (0 = none). Same job as the
// loot target above, for the other panel: on a spectator's screen the left-hand
// panel is another player's pack, and without this it would take the generic
// equip-flags-only path and sit frozen while items visibly left the victim.
// Set from EVENT_STEAL_BEGIN, cleared on END.
//
// ►► SAFE FOR THE SAME REASON THE LOOT TARGET IS, and only for that reason: the
// hazard that keeps the generic critter path equip-flags-only is an in-flight
// attack replay holding a pointer to an item being freed, and stealing is
// out-of-combat by construction (actions.cc refuses SKILL_STEAL in combat, and
// the verbs are refused there too). Removed items are deferred-freed while a
// modal is open, exactly like the loot target's.
static int gViewerStealThiefNetId = 0;

// ►► AND THE VICTIM, WHICH IS THE PANEL YOU ACTUALLY STEAL FROM. The thief above
// got this treatment and the target was overlooked, which is exactly the bug the
// owner hit: the server DETACHES the victim's equipped weapon/armor and moves its
// ITEM_HIDDEN gear into a hidden box before the session opens (lootTargetDetach —
// vanilla: you cannot lift what someone is holding), but a critter that is neither
// the dude nor a plain container takes the equip-flags-only path, so the viewer went
// on drawing the pre-detach mirror. Every click on that phantom 10mm Pistol sent a
// `stake` for a pid the server no longer had, and the honest answer came back as
// "That item is gone." Reconciling the victim in full makes the right-hand panel
// show exactly what is stealable — the same list the server is willing to move.
static int gViewerStealTargetNetId = 0;

// ── Per-client actor binding (MP_PROPOSAL.md Ch 5.6) ────────────────────────
// On a viewer, gDude means "the actor I control" — a per-client ROLE, not a
// world identity. When the roster says this session owns a non-host actor, gDude
// is REPOINTED at it, and the HP/AP bars, inventory/char screens, crosshair,
// camera, myTurn keying and dialog-editability gate all follow for free (every
// one of them is already gDude-keyed).
//
// gClientHostDude is the process's ORIGINAL dude object — the NO_REMOVE one made
// at objectsInit, the only actor object that survives mapLoad.
//
// ⚠ It exists because of a specific memory hazard: _obj_load_dude memcpy's the
// blob's dude INTO *gDude. Running that while gDude aims at a foreign (or
// already-freed) actor corrupts memory. So applyBlob restores gDude =
// gClientHostDude BEFORE mapLoad and re-derives the binding after seedNetMap.
static Object* gClientHostDude = nullptr;

// Last roster received, kept so a binding change arriving in a different frame
// than the blob can still be resolved (the two orderings are both legal).
static std::vector<PlayerRosterRow> gClientRoster;

// Feature A: latched when the server streams a REFUSAL aimed at THIS actor (a
// kMsgChannelRefusal console line addressed to our netId). The out-of-combat input
// block polls it (clientViewerTakeRefusal) to undo an optimistic local hand flip the
// server rejected as busy — see main.cc. A plain edge flag (not a queue): the block
// allows only one pending hand switch at a time, so at most one refusal is relevant.
static bool gViewerRefusalPending = false;

namespace {

// Wire event tags — MUST match presenter_network.cc's EventType enum.
enum : unsigned char {
    EVENT_SPAWN = 1,
    EVENT_MOVE = 2,
    EVENT_DESTROY = 3,
    EVENT_CONNECT = 4,
    EVENT_DISCONNECT = 5,
    EVENT_OBJECT_DELTA = 6,
    EVENT_WORLD_DELTA = 7,
    EVENT_SNAPSHOT_OBJECT = 8,
    EVENT_SNAPSHOT_BEGIN = 9,
    EVENT_SNAPSHOT_END = 10,
    EVENT_MAP_TRANSITION = 11,
    EVENT_COMBAT_ENTER = 12,
    EVENT_COMBAT_EXIT = 13,
    EVENT_TURN_START = 14,
    EVENT_ATTACK_RESULT = 15, // causal envelope for attack/hit/death replay (S4)
    EVENT_CONSOLE = 16, // combat message log / floating text / sfx (S2)
    EVENT_FLOAT_TEXT = 17,
    EVENT_SFX = 18,
    EVENT_SFX_AT = 19,
    EVENT_FADE_OUT = 20, // screen fade to black, addressed (0 = everyone)
    EVENT_FADE_IN = 21, // screen fade back in, addressed. Watchdogged — see onScreenFade
    EVENT_MUSIC_STOP = 23, // background music: stop (emitted since fade/errorbox; decoded only now)
    EVENT_SNAPSHOT_BLOB_BEGIN = 24,
    EVENT_SNAPSHOT_BLOB_CHUNK = 25,
    EVENT_SNAPSHOT_BLOB_END = 26,
    EVENT_PRES_SEQ = 31, // recorded presentation command stream — replay through the real reg_anim
    EVENT_DIALOG_NODE = 32, // dialog node — render the gdialog window seeded from the wire (Stage 3)
    EVENT_DIALOG_END = 33, // dialog ended — tear the window down (Stage 3)
    EVENT_WORLDMAP_BEGIN = 34, // worldmap travel started — render the worldmap modal
    EVENT_WORLDMAP_END = 35, // worldmap travel ended — tear down
    EVENT_WORLDMAP_STATE = 36, // worldmap state sync — position, walking, fuel, area
    EVENT_WORLDMAP_SUBTILES = 37, // worldmap fog of war — flattened per-subtile state grid
    EVENT_PLAYER_ROSTER = 38, // slot -> (netId, owning session, alive); drives the gDude rebind
    EVENT_INVENTORY_GRANT = 39, // in-combat inventory screen granted to one actor (AP already paid)
    EVENT_INVENTORY_REVOKE = 40, // that actor's turn ended with the screen still open — close it
    EVENT_MUSIC_PLAY = 41, // background music: play this level track (name + fade)
    EVENT_MOVIE_PLAY = 42, // full-screen movie; we ack with `movdone` when it ends
    EVENT_BARTER_BEGIN = 43, // trade opened — merchant + the actor driving it
    EVENT_BARTER_STATE = 44, // both offer tables (pid/qty rows) + the server's two valuations
    EVENT_BARTER_END = 45, // trade closed — tear the mirrors down
    EVENT_MOVIE_SEEN_STATE = 46, // co-op world state: the movie seen ledger (vault-suit look)
    EVENT_MOVIE_STOP = 47, // stop local movie playback (another player skipped the cutscene)
    EVENT_PLAYER_SHEET = 48, // co-op: live per-actor sheet-row delta (chem stat mod, level-up)
    EVENT_ENCOUNTER_PROMPT = 49, // random-encounter accept/decline prompt (title+body) — we answer encaccept/encdecline
    EVENT_ENCOUNTER_CLOSE = 50,
    EVENT_GVAR_DELTA = 51, // global variables that changed this beat (index/value pairs)
    EVENT_HOLODISK_CLEAR = 52, // server-authored holodisks: drop the set, an ADD run follows
    EVENT_HOLODISK_ADD = 53, // one server holodisk: name + body lines // another viewer answered — break out of our prompt box
    EVENT_ELEVATOR_PROMPT = 54, // co-op: show THIS actor vanilla's elevator panel
    EVENT_AUTOMAP_OPEN = 55, // co-op: WE used a Motion Sensor — open our own automap
    EVENT_STEAL_BEGIN = 56, // co-op: a steal session opened — thief + victim, everyone watches
    EVENT_STEAL_STATE = 57, // the server moved (or refused) something; deltas already applied — repaint
    EVENT_STEAL_END = 58, // session over (closed, caught, or bailed) — close the screen
    EVENT_LOOT_GRANT = 59, // the server opened a container FOR US — open the loot screen
    EVENT_STATE_AUDIT = 60, // authoritative object state — diff it against our mirror
    EVENT_UI_LOCK = 61, // scripted cutscene input lock, addressed (0 = everyone)
    EVENT_WORLDMAP_AREAS = 62, // worldmap city table: known/visited/entrances for every area
};

// crc32 (IEEE, reflected) — MUST match server_loop.cc's joinBlobCrc32.
unsigned int crc32Of(const unsigned char* data, int length)
{
    unsigned int crc = 0xFFFFFFFFu;
    for (int i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            unsigned int mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// Modal screens that run their own blocking loop on the viewer and hold Object*s into
// gDude — the wire must keep pumping while one is open, and it must force-close (and
// rebaselines must defer) on combat/world-rebuild. See viewerServiceTicker().
static const int kViewerModalMask = GameMode::kInventory | GameMode::kSkilldex
    | GameMode::kEditor | GameMode::kPipboy | GameMode::kLoot | GameMode::kUseOn
    | GameMode::kDialog | GameMode::kWorldmap | GameMode::kBarter
    | GameMode::kPreferences | GameMode::kAutomap;

// ─── Combat outlines on the wire viewer (COMBAT_CLIENT_DESIGN #8) ────────────
// Vanilla draws colored critter outlines in combat (red=hostile / green=friendly by
// team; dim=within perception but LOS-blocked) via _combat_update_critter_outline_for_los.
// Its drivers (_combat_outline_on / _combat_update_critters_in_los) iterate _combat_list,
// which is EMPTY on the viewer (only _combat_begin/_combat_load fill it, neither runs
// client-side). So we drive the per-critter vanilla decision over the REAL mirrored
// object list — the exact shape of _combat_outline_on's out-of-combat branch
// (combat_drain.cc:748). Purely client-side: team + LOS geometry ride the join/rebaseline
// blob (proto.cc:547 serializes team); outline is never on the wire (objectRead forces it
// to 0). PERCEPTION comes from the premade dude proto (v1 accepts a slightly-off dim
// range; streaming the real PER is banked). See [[p5-server-plan]] #8.
void clientOutlineRefresh()
{
    Object** critters;
    int count = objectListCreate(-1, gElevation, OBJ_TYPE_CRITTER, &critters);
    for (int index = 0; index < count; index++) {
        Object* critter = critters[index];
        if (critter == gDude) {
            continue;
        }
        if ((critter->data.critter.combat.results & DAM_DEAD) != 0) {
            // Vanilla clears the outline in _combat_delete_critter on death
            // (combat.cc:5945), but that runs server-side (empty _combat_list here),
            // so a corpse would keep its outline. Clear it on the next refresh.
            objectDisableOutline(critter, nullptr);
            objectClearOutline(critter, nullptr);
        } else {
            _combat_update_critter_outline_for_los(critter, true);
        }
    }
    if (count != 0) {
        objectListFree(critters);
    }
    tileWindowRefresh();
}

// Clear every critter outline over the live object list (mirror of _combat_outline_off's
// out-of-combat branch). Needed only on in-session combat exit: a rebaseline/rejoin
// rebuilds the object list from scratch (objectRead forces outline=0), so those paths
// self-clear, and vanilla's _combat_over clear is a no-op here (_combat_list empty).
void clientOutlineClearAll()
{
    Object** critters;
    int count = objectListCreate(-1, gElevation, OBJ_TYPE_CRITTER, &critters);
    for (int index = 0; index < count; index++) {
        objectDisableOutline(critters[index], nullptr);
        objectClearOutline(critters[index], nullptr);
    }
    if (count != 0) {
        objectListFree(critters);
    }
    tileWindowRefresh();
}

// Little-endian byte reader with bounds checks. Every accessor returns 0 past the
// end and trips `overflow`, so a truncated stream fails loud instead of reading
// garbage.
class Reader {
public:
    Reader(const unsigned char* data, size_t size)
        : _data(data), _size(size), _off(0), _overflow(false) {}

    bool overflow() const { return _overflow; }
    size_t remaining() const { return _off <= _size ? _size - _off : 0; }
    void skip(size_t n) { _off += n; if (_off > _size) _overflow = true; }
    const unsigned char* here() const { return _data + _off; }

    unsigned char u8()
    {
        if (_off + 1 > _size) { _overflow = true; return 0; }
        return _data[_off++];
    }
    unsigned short u16()
    {
        if (_off + 2 > _size) { _overflow = true; _off = _size; return 0; }
        unsigned short v = (unsigned short)(_data[_off] | (_data[_off + 1] << 8));
        _off += 2;
        return v;
    }
    unsigned int u32()
    {
        if (_off + 4 > _size) { _overflow = true; _off = _size; return 0; }
        unsigned int v = (unsigned int)_data[_off] | ((unsigned int)_data[_off + 1] << 8)
            | ((unsigned int)_data[_off + 2] << 16) | ((unsigned int)_data[_off + 3] << 24);
        _off += 4;
        return v;
    }
    int i32() { return (int)u32(); }

    // Matches presenter_network.cc putString: u16 length + that many raw bytes
    // (codepage, NOT null-terminated). Returns a null-terminated copy; on overflow
    // returns empty and trips the flag.
    std::string str()
    {
        unsigned short len = u16();
        if (_overflow || len > remaining()) { _overflow = true; return std::string(); }
        std::string s((const char*)here(), (size_t)len);
        skip(len);
        return s;
    }

private:
    const unsigned char* _data;
    size_t _size;
    size_t _off;
    bool _overflow;
};

// Set while this viewer is parked in the streamed encounter prompt box. The
// dismiss (onEncounterClose) only injects its force-close ESC when this is set,
// so a CLOSE that arrives late (RTT: another viewer answered, but by the time the
// broadcast reaches us we already answered and moved on) can NEVER close an
// unrelated modal — or the player's game — that they opened afterward. General
// rule for any broadcast force-close: gate the ESC on "am I actually in THAT modal".
// TEMP DIAGNOSTIC [wmenc]: every enqueueInputEvent(KEY_ESCAPE) in this file names
// itself while a traced dialog box is open. The encounter prompt is being closed by a
// real KEY_ESCAPE ~1 frame after it opens; these sites plus the two producers in
// _process_bk are the complete set of ways one can arrive, so whichever line prints IS
// the cause. Silence from all of them means the keystroke is genuinely the player's.
static void wmencTagEscInjection(const char* site)
{
    const char* tag = dialogBoxTraceActiveTag();
    if (tag != nullptr) {
        fprintf(stderr, "[%s] ESC INJECTED by %s\n", tag, site);
    }
}

static bool gEncounterPromptActive = false;

// The decoder's live index: wire netId -> local Object*. Seeded from the loaded
// blob's post-walk objects, maintained by SPAWN/DESTROY.
class Decoder {
public:
    // One ATTACK_RESULT held for serialized replay (§3.c). netIds, not pointers, so
    // dequeue re-resolves against the live world and skips freed participants.
    struct PendingAttack {
        int attackerNetId, defenderNetId, hitMode, defenderHitLocation;
        int defenderDamage, defenderFlags, attackerDamage, attackerFlags;
        int extraCount;
        int extraNetId[EXPLOSION_TARGET_COUNT];
        int extraDamage[EXPLOSION_TARGET_COUNT];
        int extraFlags[EXPLOSION_TARGET_COUNT];
        int weaponNetId = 0; // bug D: the fired weapon, resolved/recreated on the viewer
        int weaponPid = -1;  // -1 = unarmed
    };

    // ONE ordered combat-presentation queue (§3.c). The server resolves a whole
    // combat turn — sometimes the whole fight on a killing blow — in one beat and
    // flushes TURN_START, every attack, its console/float/sfx, and the next
    // TURN_START in a single pump. Applying them as they arrive races the animation
    // playback: the AP dots flip to "my turn" green while the enemy is still visibly
    // attacking, and the message log spoils the whole turn up front. So the viewer
    // does NOT apply combat framing/feedback on arrival — it queues these events and
    // releases them in wire order, LOCKSTEP with the attack animations
    // (presentationPump). Numeric state (hp/ap on the objects) still rides
    // OBJECT_DELTA immediately — only the presentation is paced. Headless never
    // queues (applies inline, byte-identical).
    enum class PresKind {
        kTurnStart, // whose turn — sets _myTurn + AP dots/lights (blocks on anim)
        kAttack, // an attack replay (starts an animation; blocks the queue)
        kExit, // end-of-combat chrome (blocks on anim → death plays out first)
        kConsole, // message-log line (feedback; released with its attack)
        kFloat, // floating combat text over a critter (feedback)
        kSfx, // combat sound effect (feedback)
        kMoveRelease, // release N held glide hops of a mover (in-combat move, §3.d)
        kRecordedSeq, // a recorded presentation command stream (replaces kTakeOut/kDoor/kActionAnim/kExplosionFx)
        // ►► A STATE-LANE EVENT PARKED ONTO THIS FIFO so it applies in WIRE ORDER
        // relative to the sequences around it, instead of at decode time
        // (PRESENTATION_PACING_DESIGN.md §12). The wire is totally ordered, but this
        // client historically applied state events on one clock (decode) and recorded
        // sequences on another (pump drain) — a per-lane speed difference IS a reorder,
        // which the never-lossy invariant forbids. It shows up as lifetime corruption
        // for the one object whose life BOTH lanes write: a thrown weapon's flight
        // transient, destroyed by its own pickup DISCONNECT before the flight plays.
        // Carrying the raw payload is cheap because every event is length-prefixed and
        // the reader is already bounded to it (see event(), below).
        // ►► NO FEEDER YET (§12.5 step 1): nothing enqueues this today, so the path is
        // inert and the goldens must be byte-identical. Step 2 adds the feeder.
        kDeferredEvent,
    };
    struct PresEvent {
        PresKind kind;
        PendingAttack attack; // kAttack
        int tsNetId = 0, tsIsPlayer = 0, tsAp = 0, tsDeadline = 0, tsFreeMove = 0; // kTurnStart
        int floatNetId = 0; // kFloat owner
        int moveNetId = 0, moveHops = 0; // kMoveRelease
        std::vector<unsigned char> seqOps; // kRecordedSeq — the raw op buffer (played at pump time)
        int seqActorNetId = 0; // kRecordedSeq — actor whose approach glide must drain before play (0 = none)
        // kRecordedSeq — the adopt netIds this sequence INCREMENTED in _pendingAdopts at
        // decode and will decrement when it executes. Carried so that DROPPING this entry
        // on backlog can release them: a stranded count never decrements, and then every
        // later state event for that netId defers against it and force-applies a full cap
        // late, forever (§12.6 trap 2, reached by a second route).
        std::vector<int> seqAdopts;
        std::string text; // kConsole / kFloat / kSfx
        int consoleChannel = kMsgChannelDefault; // kConsole — message-log style (msg_channel.h)
        // kDeferredEvent — the parked state event, re-dispatched verbatim at drain.
        unsigned char defEvType = 0; // the EVENT_* type byte
        std::vector<unsigned char> defBytes; // its length-bounded payload, copied
        unsigned int defEntryId = 0; // wire v4 total-order id, replayed into event()
        int defNetId = 0; // the object this event addresses — what the gate waits on
        unsigned int defCapAt = 0; // getTicks() deadline; 0 = set on first gate check
    };
    // ►► netId -> count of OBJ_CREATEs adopting it that have been DECODED but not yet
    // EXECUTED (§12.2). Half of the "presentation-entangled" test the feeder uses; both
    // teardown paths clear it (§12.6 trap 2: miss the clear and later events defer against
    // a count that never decrements, then force-apply a full cap late, forever).
    std::unordered_map<int, int> _pendingAdopts;

    // Set while the pump (or the overflow backstop) is re-dispatching a parked event
    // through event(). Without it the drain would re-park the same event forever: the
    // netId is STILL entangled at that instant — draining the event is precisely what
    // un-entangles it. Saved/restored rather than blind-cleared, because an inline
    // overflow apply can nest inside a drain.
    bool _applyingDeferredEvent = false;

    // See everBoundToSlot().
    bool _everBoundToSlot = false;

    // Backlog safety cap. The player-turn barrier bounds the queue in practice (the
    // server blocks on the claimant, who cannot act until the queue drains), so this
    // only guards a pathological run; when hit, the oldest droppable (non-turn,
    // non-exit) event is discarded so turn boundaries never desync.
    static constexpr size_t kMaxQueuedPresEvents = 1024;

    // ►► Wall-clock backstop for a parked state event (§12.2). If the animation it waits
    // on never finishes (wedged, cancelled, or the object died), the event FORCE-APPLIES
    // rather than waiting forever: failure direction stays "play/snap, never freeze", and
    // never "drop" — the server's event is always applied, only late. Sibling of the
    // move-replay cap in client_present.cc.
    static constexpr unsigned int kDeferredEventCapMs = 4000;

    // A server holodisk is meant to be a page or two of text. Bounds an untrusted
    // wire line count before anything is allocated for it.
    static constexpr int kMaxHolodiskLines = 512;

    Decoder()
        : _loaded(false), _tripwireOk(0), _tripwireBad(0) {}

    int tripwireOk() const { return _tripwireOk; }
    int tripwireBad() const { return _tripwireBad; }

    // A rebaseline arrived while a viewer modal was open and is buffered pending its close
    // (onBlobEnd deferred it). The main loop applies it via applyDeferredBlob once no modal
    // is up, so mapLoad never frees gDude under the modal's static pointers.
    bool blobDeferred() const { return _blobDeferred; }
    void applyDeferredBlob() { applyBlob(); }

    // This viewer's sessionId, handed over by the frame walker the moment it
    // parses the accept preamble — the one per-client fact in the protocol, and
    // what the roster is matched against to find our own actor (Ch 5.5/5.6).
    void setSessionId(int sessionId) { _mySessionId = sessionId; }

    // Combat presentation state decoded from the wire (P3). The viewer reads these
    // to route a click (mv vs cmove) and to know when its turn's live. Purely
    // presentational — the authoritative combat runs on the server; nothing here
    // gates state application (state rides objectDelta as always).
    bool inCombat() const { return _inCombat; }
    bool myTurn() const { return _myTurn; }

    // Did this client EVER appear in a player roster under its own session? False means
    // the server never bound us to a slot — which is what a REFUSED LOGIN looks like from
    // this side, since the wire has no per-session channel to explain a refusal over.
    bool everBoundToSlot() const { return _everBoundToSlot; }

    // In-combat inventory grant (Stage 4). `take` consumes the one-shot latch set
    // by onInventoryGrant; `open` tracks whether the granted screen is currently
    // up, which is what stops the service ticker force-ESCing it (see
    // viewerServiceTicker — closing the screen the server just charged 4 AP for
    // would take the AP and give nothing back).
    // One-shot: the container the server just opened for us, or 0.
    int takeLootGrant()
    {
        int netId = _lootGrantNetId;
        _lootGrantNetId = 0;
        return netId;
    }

    bool takeInventoryGrant()
    {
        bool granted = _invGrantPending;
        _invGrantPending = false;
        return granted;
    }
    // ►►►► APPLIED HERE, AT DECODE, IN WIRE ORDER — and that placement is the whole
    // point. Latching the fade for the main loop to run later meant every objectDelta
    // in the frame (which IS applied at decode) landed BEFORE it, so the change the
    // fade exists to hide was already on screen when the screen went black: the grave
    // popped open, then the fade played. Order on the wire is now FADE_OUT, the
    // change, FADE_IN (the server flushes its deltas at each boundary), and honouring
    // that order requires applying the fade where the deltas are applied.
    //
    // Safe to block here, unlike a modal: paletteFadeTo spins the palette for a few
    // hundred ms with a sound-continue callback. It reads no input, opens no window
    // and pumps no wire, so it cannot apply later events from under its own feet —
    // the hazard the latch rule exists for.
    void applyFade(bool toBlack)
    {
        if (toBlack) {
            paletteFadeTo(gPaletteBlack);
            _fadeBlackSinceMs = getTicks();
            if (_fadeBlackSinceMs == 0) _fadeBlackSinceMs = 1; // 0 = "not black"
        } else {
            paletteFadeTo(_cmap);
            _fadeBlackSinceMs = 0;
        }
    }

    // For the main loop's watchdog: when we went black, or 0.
    unsigned int fadeBlackSince() const { return _fadeBlackSinceMs; }
    void clearFadeBlack() { _fadeBlackSinceMs = 0; }

    // Fade QUEUE, drained in order — and it must be a queue, not a latch.
    //
    // ►►►► "LATEST WINS" IS WHY NOBODY EVER SAW A FADE. This used to keep one pending
    // value and let a fade-in overwrite an un-applied fade-out, reasoning that a pair
    // arriving together meant "a sequence that already finished, don't blink". But on a
    // dedicated server EVERY pair arrives together: the work between the two calls (dig
    // the grave, pass the time, move the actor) is instantaneous headless, so the out and
    // the in are emitted in the SAME beat and land in the same pump batch. The collapse
    // therefore fired on the normal case and dropped the fade-out every single time — the
    // screen never went black, and all the player got was the sim visibly stopping and
    // starting again (owner: "people freeze and unfreeze a split second later, no black
    // screen"). The freeze was the server's parked tick; the fade that was supposed to
    // COVER it had been optimized away.
    //
    // Applying both in order is also what single player looks like: paletteFadeTo blocks
    // and steps the palette, so out-then-in is a real fade down and back up, not a blink.
    // The fade itself is applied at decode (see applyFade); all that is left for the
    // main loop is the watchdog, because a fade-out whose matching fade-in never
    // arrives — the script errored, the actor died mid-sequence, the connection
    // dropped — leaves the player staring at a black screen with no way out. Fading
    // back early costs a visual beat; staying black costs the session.
    bool fadeWatchdogExpired(unsigned int nowMs, unsigned int maxBlackMs) const
    {
        return _fadeBlackSinceMs != 0 && getTicksBetween(nowMs, _fadeBlackSinceMs) > maxBlackMs;
    }

    // Automap latch (same one-shot shape, same reason — a modal screen must not be
    // opened from inside pump()).
    bool takeAutomapOpen(bool* usingScanner)
    {
        if (!_automapPending) {
            return false;
        }
        _automapPending = false;
        *usingScanner = _automapUsingScanner;
        return true;
    }

    // Encounter accept/decline latch (same one-shot shape and the same reason as the
    // two above — see onEncounterPrompt). Unlike those, the SERVER IS BLOCKED on the
    // answer, so this one cannot wait for the main loop's no-modal-open point: the
    // viewer is inside its worldmap modal when the prompt lands. viewerServiceTicker
    // takes it, which runs in every modal loop AND the main loop, and — crucially —
    // outside drain().
    bool takeEncounterPrompt(std::string* title, std::string* body)
    {
        if (!_encPromptPending) {
            return false;
        }
        _encPromptPending = false;
        *title = _encPromptTitle;
        *body = _encPromptBody;
        return true;
    }

    // Elevator panel latch (same one-shot shape, same reason — see onElevatorPrompt).
    bool takeElevatorPrompt(int* elevator, int* startLevel)
    {
        if (!_elevatorPending) {
            return false;
        }
        _elevatorPending = false;
        *elevator = _elevatorType;
        *startLevel = _elevatorStartLevel;
        return true;
    }

    void setCombatModalOpen(bool open) { _combatModalOpen = open; }
    bool combatModalOpen() const { return _combatModalOpen; }


    void setInCombat(bool v) { _inCombat = v; }

    // Single owner of the viewer's combat outlines (#8). Fully recomputed from
    // (in-combat, whose-turn, mouse-mode) on every call, so it is idempotent and any
    // drive point — turn start, move, mouse-mode switch, mid-fight resync — just calls
    // this. Reproduces vanilla, whose own drivers run server-side / no-op here (empty
    // _combat_list): your turn highlights every in-LOS critter ONLY while the attack
    // (crosshair) cursor is up (game_mouse.cc:1443 gates _combat_outline_on on
    // GAME_MOUSE_MODE_CROSSHAIR + target_highlight); another actor's turn outlines ONLY
    // the acting critter (combat.cc:3247 objectEnableOutline(obj)); otherwise nothing.
    void recomputeCombatOutlines()
    {
        if (!clientViewerActive()) {
            return;
        }
        clientOutlineClearAll(); // clean slate → idempotent regardless of prior state
        if (!_inCombat) {
            return;
        }
        if (_myTurn) {
            if (gameMouseGetMode() == GAME_MOUSE_MODE_CROSSHAIR
                && settings.preferences.target_highlight != TARGET_HIGHLIGHT_OFF) {
                clientOutlineRefresh();
            }
        } else if (_combatActorNetId != 0) {
            Object* actor = lookup(_combatActorNetId);
            if (actor != nullptr && (actor->data.critter.combat.results & DAM_DEAD) == 0) {
                _combat_update_critter_outline_for_los(actor, true);
                tileWindowRefresh();
            }
        }
    }

    // Per-frame combat-presentation driver (viewer only), called from the render
    // loop. Walks the ordered queue, releasing events in wire order and pacing them
    // to the animation: an attack starts a replay and BLOCKS the queue until it is
    // idle; a turn-start / end-of-combat also blocks (so the AP dots flip and the
    // doors close only after the last animation plays); feedback (console/float/sfx)
    // is non-blocking and rides out right after the attack it captions (§3.c).
    // Per-hex AP tick (viewer only), run every frame from presentationPump. While a
    // combat move's AP is deferred (§3.d), poll the dude's remaining glide hops: each
    // hop consumed drops the SHOWN AP one dot (clamped at auth — free-move hexes cost
    // 0), and when the glide ends (no walk left) the bar reconciles to authoritative.
    // ►► GIVE THE AP DOTS A RE-DERIVATION PATH, which is the one thing they never had.
    //
    // Every other element of the interface bar is idempotently re-derived from state:
    // interfaceBarRefresh() and interfaceBarShow() both re-render items, HP and AC and
    // then push the window. NEITHER renders action points (interface.cc) — the dots are
    // only ever whatever the last explicit interfaceRenderActionPoints() left in the
    // buffer. So AP is write-only-on-event, and ANY path that changes it without
    // painting, or paints it while the bar is covered by a modal, leaves a stale number
    // on screen until something unrelated happens to repaint. Two such paths exist:
    //   - the in-combat inventory SCREEN, priced by the server at open (4 AP, item.cc
    //     inventoryApCostApply) — the delta lands around the moment the modal takes over
    //     the frame loop (the service ticker keeps the wire pumping INSIDE that loop);
    //   - resolveHeld (client_present.cc), which commits a HELD move AP straight onto
    //     the object and repaints nothing.
    // Rather than teach each site to repaint — that is whack-a-mole, and the list is
    // open-ended — converge here, every frame, exactly as rollDudeHp does for HP two
    // functions down. Then no caller has to remember and the whole class is closed.
    //
    // ►► DOWNWARD ONLY, and that is not an optimization. onObjectDelta deliberately
    // REFUSES to paint an AP increase, because an increase is the round reset that
    // precedes the next turn and painting it would flash a full green bar before the
    // paced TURN_START flips the bar to the next actor (see the comment there). An
    // unconditional converge-to-authority would reintroduce exactly that flash. Showing
    // MORE AP than authority is the bug; showing less is either that deliberate
    // suppression or a move mid-tick-down, and both have an owner already.
    void tickApBar()
    {
        if (!clientViewerActive() || !_inCombat || !_myTurn || gDude == nullptr) {
            return;
        }
        if (_dudeApDeferring) {
            return; // a move's per-hex tick owns the shown value until it lands
        }
        int authoritative = gDude->data.critter.combat.ap;
        if (_dudeApShown <= authoritative) {
            return;
        }
        _dudeApShown = authoritative;
        _dudeApAuth = authoritative;
        interfaceRenderActionPoints(_dudeApShown, _combat_free_move);
        interfaceBarRefresh();
    }

    void tickCombatMoveAp()
    {
        if (!clientViewerActive() || !_dudeApDeferring) {
            return;
        }
        int hops = clientAnimHopsRemaining(gDude);
        if (hops < 0) {
            // Glide ended — reconcile the bar to the authoritative AP.
            if (_dudeApShown != _dudeApAuth) {
                _dudeApShown = _dudeApAuth;
                interfaceRenderActionPoints(_dudeApShown, 0);
                interfaceBarRefresh();
            }
            _dudeApDeferring = false;
            return;
        }
        if (hops < _dudeApMoveHops) {
            int shown = _dudeApShown - (_dudeApMoveHops - hops);
            _dudeApMoveHops = hops;
            if (shown < _dudeApAuth) {
                shown = _dudeApAuth; // free-move hexes don't cost AP; never undershoot
            }
            if (shown != _dudeApShown) {
                _dudeApShown = shown;
                interfaceRenderActionPoints(_dudeApShown, 0);
                interfaceBarRefresh();
            }
        }
    }

    // Ease the viewer's SHOWN dude HP (gDude->hp, the value interfaceRenderHitPoints
    // reads) toward authority (_dudeHpAuth) a fraction per frame, so a hit COUNTS the
    // counter down instead of snapping — vanilla rolls it; our decoder hard-set it
    // (PRESENTATION_PACING_DESIGN.md §2, the keyframe/tween model). Display-only:
    // _dudeHpAuth is the truth every decision reads. Bidirectional (heals roll up).
    // Runs every pump frame; converges then no-ops. Uses interfaceRenderHitPoints(false)
    // (NOT the animate=true variant, which spins its own blocking loop and would stall
    // the pump — that is exactly why the counter was hard-set in the first place).
    void rollDudeHp()
    {
        if (!clientViewerActive() || gDude == nullptr) {
            return;
        }
        if (!_dudeHpSeeded) {
            // First pump before any hp delta: adopt the current (blob) value so we
            // never roll down from a stale 0. Order-independent with the delta seed.
            _dudeHpAuth = gDude->data.critter.hp;
            _dudeHpSeeded = true;
            return;
        }
        int shown = gDude->data.critter.hp;
        if (shown == _dudeHpAuth) {
            return;
        }
        int remaining = _dudeHpAuth - shown;
        int step = remaining / 6; // ease-out
        if (step == 0) {
            step = remaining > 0 ? 1 : -1; // always make progress / land
        }
        shown += step;
        if ((remaining > 0) == (shown > _dudeHpAuth)) {
            shown = _dudeHpAuth; // clamp; never overshoot
        }
        gDude->data.critter.hp = shown;
        interfaceRenderHitPoints(false);
        interfaceBarRefresh();
    }

    void presentationPump()
    {
        if (!clientViewerActive()) {
            return;
        }
        tickCombatMoveAp();
        tickApBar(); // AFTER the per-hex tick, which owns the shown value while it runs
        rollDudeHp();
        // While an animation plays, release NOTHING — not the next attack, and not
        // the next attack's leading console/float/sfx (the server emits an attack's
        // captions right before its ATTACK_RESULT, so a non-blocking feedback pass
        // would leak the NEXT attack's "miss/hit" text out over the CURRENT swing).
        // Each idle window then drains one attack together with its own captions: the
        // leading feedback, the ATTACK_RESULT (which re-arms the animation and stops
        // the drain), and nothing beyond it.
        for (;;) {
            if (clientCombatAnimActive()) {
                // An attack is animating — hold everything.
                clientAnimNotePresentationProgress();
                break;
            }
            // The attack that was playing (if any) has finished — apply its dude damage
            // now, so the bar drops as the blow LANDS, not as the swing began.
            if (_pendingDudeTick > 0) {
                tickDudeHp(gDude, _pendingDudeTick);
                _pendingDudeTick = 0;
            }
            if (_presQueue.empty()) {
                break;
            }
            // Movement sequencing (§3.d). A move's glide is HELD on decode and
            // released here in wire order, so an approach never glides over the turn
            // it belongs to. The blocks below keep the fight strictly sequential:
            const PresEvent& front = _presQueue.front();
            // (1) An attacker approaches by gliding into range; hold its attack until
            //     the participants stop PLAYABLE-gliding — else clientCombatAnimPlay
            //     would cancel the glide instantly and the approach would teleport.
            if (front.kind == PresKind::kAttack
                && attackParticipantsGliding(front.attack)) {
                break;
            }
            // (1b) A weapon draw must NOT play over the critter's OWN pending approach.
            //      The draw animates the sprite's fid + sub-tile offset in place, which
            //      fights a parked/gliding walk and teleports it (whack-a-mole with the
            //      walk tripwire). So if the critter is moving this turn, play the
            //      approach FIRST and draw at the destination: if the approach is still
            //      HELD (its kMoveRelease sits behind this draw in wire order), rotate the
            //      draw to just after that release; if it is already gliding, wait it out.
            //      No walk → stationary wield-and-fire, draw in place now.
            // (1d) A recorded sequence with a primary actor (out-of-combat gesture/
            //      door) waits out that actor's approach glide, exactly like (1c): the
            //      outcome fired server-side on arrival, so the stream sits behind the
            //      approach MOVE events — play it at the destination, not mid-stride.
            //      Draining that glide IS the authority snap (walkSnapToAuthority).
            if (front.kind == PresKind::kRecordedSeq
                && front.seqActorNetId != 0
                && clientAnimPlayableActiveFor(lookup(front.seqActorNetId))) {
                break;
            }
            // (1e) SERIALIZE consecutive recorded seqs of ONE actor by its real animation
            //      state: a move right after a throw (or draw→attack) must wait for the prior
            //      seq's reg_anim to finish, else the two sequences stomp each other and the
            //      later one teleports. Keyed on animationIsBusy (not a shared Active mark)
            //      so the prior seq's completion doesn't prematurely resolve THIS actor's
            //      held move deltas (the throw→move snap). COMBAT_MOVE_RECORD_DESIGN.md.
            if (front.kind == PresKind::kRecordedSeq && front.seqActorNetId != 0) {
                Object* seqActor = lookup(front.seqActorNetId);
                if (seqActor != nullptr && animationIsBusy(seqActor) != 0) {
                    break;
                }
            }
            // (1f) ►► A PARKED STATE EVENT waits for the object it addresses to stop
            //      animating, so a thrown weapon's pickup DISCONNECT lands AFTER its own
            //      flight/landing anim instead of destroying the transient mid-air
            //      (§12.2). Keyed on the EVENT's netId, deliberately NOT the sequence
            //      actor: the thrower's follow-through is not "the spear has landed", and
            //      keying on the actor breaks when the thrower has been freed. A NULL
            //      lookup can only mean "already gone" under FIFO order (its sequence
            //      drained first), so null → apply now, do not hold.
            if (front.kind == PresKind::kDeferredEvent) {
                // Stamp the arrival on first sight (front is const here, so poke the queue
                // slot), then age it with getTicksSince — the same wraparound-safe helper
                // the glide caps use.
                if (_presQueue.front().defCapAt == 0) {
                    _presQueue.front().defCapAt = getTicks();
                }
                bool expired = getTicksSince(_presQueue.front().defCapAt) >= kDeferredEventCapMs;
                Object* subject = lookup(front.defNetId);
                bool busy = subject != nullptr && animationIsBusy(subject) != 0;
                if (busy && !expired) {
                    break;
                }
                if (busy && expired) {
                    debugPrint("client_net: deferred event type=%d net=%d force-applied at cap\n",
                        (int)front.defEvType, front.defNetId);
                }
            }
            // (2) A turn flip / end-of-combat waits for the OUTGOING actor to finish
            //     gliding, so the AP dots flip (and the doors close) only after the
            //     last approach/retreat plays out. In combat the only playable glides
            //     are the presented turn's (free-roam walks are frozen server-side).
            if ((front.kind == PresKind::kTurnStart || front.kind == PresKind::kExit)
                && clientAnimAnyPlayableActive()) {
                break;
            }
            PresEvent ev = _presQueue.front();
            _presQueue.pop_front();
            clientAnimNotePresentationProgress(); // the queue drained an event = progress
            switch (ev.kind) {
            case PresKind::kAttack: playPending(ev.attack); break;
            case PresKind::kTurnStart: applyTurnStart(ev.tsNetId, ev.tsIsPlayer, ev.tsAp, ev.tsDeadline, ev.tsFreeMove); break;
            case PresKind::kExit: applyCombatExit(); break;
            case PresKind::kConsole: applyConsole(ev.text, ev.consoleChannel); break;
            case PresKind::kFloat: applyFloat(ev.floatNetId, ev.text); break;
            case PresKind::kSfx: applySfx(ev.text); break;
            case PresKind::kMoveRelease: clientAnimRelease(lookup(ev.moveNetId), ev.moveHops); break;
            case PresKind::kDeferredEvent: {
                // Re-dispatch verbatim through the SAME handler the decoder would have
                // used. event() expects a reader bounded to exactly this payload, which
                // is what we copied, so the handler cannot tell it was parked.
                Reader dr(ev.defBytes.data(), ev.defBytes.size());
                bool prevDeferring = _applyingDeferredEvent;
                _applyingDeferredEvent = true;
                event(ev.defEvType, dr, ev.defEntryId);
                _applyingDeferredEvent = prevDeferring;
                break;
            }
            case PresKind::kRecordedSeq:
                presPlayRecordedSeq(ev.seqOps.data(), (int)ev.seqOps.size(), true);
                // Uniform Active marking (Fable review A5/C.2): a throw/attack/wield seq's
                // ops don't self-promote to Active (only MOVE/TAKE_OUT do), so combatAnim
                // read 0 while such a seq was still animating — the turn-flip gate could
                // play over it and the `[busy] STUCK combatAnim=0` misreport. Mark the actor
                // Active (capMs=0, ownsMoveFrame=false) so every executing seq holds the
                // pump and reaps via advanceReplays. In combat only (out-of-combat gesture/
                // door keep their existing lifecycle). Idempotent for a MOVE seq that already
                // marked itself (enterReplay no-ops when already Active; capMs 0 won't shrink
                // the move's cap; ownsMoveFrame false won't clear its frame claim).
                if (_inCombat) {
                    clientCombatAnimMarkActive(lookup(ev.seqActorNetId), 0);
                }
                break;
            }
        }
        // Self-heal (§3.d anti-wedge): with nothing queued and no attack animating, a
        // held glide has nothing left to sequence against — release every hold so a
        // lost/dropped release can never strand a sprite. Failure direction is always
        // "play/snap", never "freeze".
        if (_presQueue.empty() && !clientCombatAnimActive()) {
            clientAnimReleaseAll();
        }
    }

    // True while the viewer still owes combat presentation: a replay in flight or any
    // event still queued (turn flip / attacks / feedback / end-of-combat). The frame
    // loop shows the wait cursor and locks combat input while this holds.
    bool combatPresentationBusy() const
    {
        return clientCombatAnimActive() || !_presQueue.empty();
    }

    // Apply one length-prefixed event payload (the reader is positioned at the
    // event's first byte and bounded to its len by the caller).
    void event(unsigned char type, Reader& r, unsigned int entryId)
    {
        // Wire v4 total-order id of this entry (PRESENTATION_PACING_DESIGN.md §8.1).
        // Tracked as the highest id decoded — the value the state-hash ack will report
        // ("applied through N", §4 P2b) and the outbox keys releases on (§4 P2). Phase
        // 1 only STAMPS it; deferral/outbox/ack consume it in later phases.
        _lastEntryId = entryId;
        // A mid-stream joiner can receive the tail of the beat it connected
        // during — events addressing a world it hasn't loaded yet. The
        // rebaseline blob that follows (same beat, C.4) carries all of that
        // state; apply nothing before the world exists. Same rule covers the
        // MAP_TRANSITION→blob window (transition clears _loaded).
        //
        // ►►►► EXCEPT A CONSOLE LINE, WHICH ADDRESSES NO WORLD. This gate is why "you
        // enter a random encounter and are never told what it is" survived a server-side
        // fix that demonstrably put the line on the wire: the arrival description is
        // emitted immediately after mapLoadById, i.e. INSIDE the MAP_TRANSITION→blob
        // window where _loaded is false, so it was dropped here — before onConsole,
        // silently, with no gap and no error to show for it. Measured: 20 console lines
        // decoded in the same session, that one never arriving.
        //
        // The rule this gate enforces is "apply nothing that references a world which
        // does not exist yet", and it is right for the object/state family. A console
        // message is text for the interface bar's log; it owns no netId, touches no
        // object, and the display monitor outlives every map. The one variant that DOES
        // carry an address (a refusal aimed at one actor) is still safe, because
        // onConsole drops it when gDude is null — so nothing here can act on a body that
        // has not been rebuilt. The flood case this gate also guarded against is already
        // handled a layer up, by mapLoad's emission-suppression window on the server.
        if (!_loaded && type != EVENT_SNAPSHOT_BLOB_BEGIN
            && type != EVENT_SNAPSHOT_BLOB_CHUNK && type != EVENT_SNAPSHOT_BLOB_END
            && type != EVENT_CONSOLE) {
            return;
        }
        // ►► §12.2 FEEDER — the ONE rule: a state event addressing a presentation-
        // entangled netId rides _presQueue in wire order instead of applying here on the
        // decode clock. These five are the object-lifecycle/state family, i.e. the events
        // that can write the life of a thrown weapon's flight transient; deferring them is
        // what stops its pickup DISCONNECT from destroying it mid-air. Nothing else defers
        // — a caption or a turn flip has no object whose animation it could outrun.
        // ⚠ _lastEntryId was already stamped above (decoded), and the re-dispatch at drain
        // stamps it again with the same id. Inert today; when the state-hash ack consumes
        // it (§4 P2b) it must report APPLIED-through, so it belongs on the drain side.
        switch (type) {
        case EVENT_MOVE:
        case EVENT_DESTROY:
        case EVENT_CONNECT:
        case EVENT_DISCONNECT:
        case EVENT_OBJECT_DELTA:
            if (deferStateEvent(type, r, entryId)) {
                return;
            }
            break;
        default:
            break;
        }
        switch (type) {
        case EVENT_SNAPSHOT_BLOB_BEGIN: onBlobBegin(r); break;
        case EVENT_SNAPSHOT_BLOB_CHUNK: onBlobChunk(r); break;
        case EVENT_SNAPSHOT_BLOB_END: onBlobEnd(r); break;
        case EVENT_SPAWN: onSpawn(r); break;
        case EVENT_MOVE: onMove(r); break;
        case EVENT_DESTROY: onDestroy(r); break;
        case EVENT_CONNECT: onConnect(r); break;
        case EVENT_DISCONNECT: onDisconnect(r); break;
        case EVENT_OBJECT_DELTA: onObjectDelta(r); break;
        case EVENT_WORLD_DELTA: onWorldDelta(r); break;
        case EVENT_SNAPSHOT_OBJECT: onSnapshotObject(r); break;
        case EVENT_SNAPSHOT_END: onSnapshotEnd(r); break;
        case EVENT_MAP_TRANSITION: onMapTransition(r); break;
        case EVENT_COMBAT_ENTER: onCombatEnter(r); break;
        case EVENT_COMBAT_EXIT: onCombatExit(r); break;
        case EVENT_TURN_START: onTurnStart(r); break;
        case EVENT_ATTACK_RESULT: onAttackResult(r); break;
        case EVENT_PRES_SEQ: onPresSeq(r); break;
        case EVENT_CONSOLE: onConsole(r); break;
        case EVENT_FLOAT_TEXT: onFloatText(r); break;
        case EVENT_SFX: onSfx(r); break;
        case EVENT_SFX_AT: onSfxAt(r); break;
        case EVENT_BARTER_BEGIN: onBarterBegin(r); break;
        case EVENT_BARTER_STATE: onBarterState(r); break;
        case EVENT_BARTER_END: onBarterEnd(r); break;
        case EVENT_STEAL_BEGIN: onStealBegin(r); break;
        case EVENT_STEAL_STATE: onStealState(r); break;
        case EVENT_STEAL_END: onStealEnd(r); break;
        case EVENT_DIALOG_NODE: onDialogNode(r); break;
        case EVENT_DIALOG_END: onDialogEnd(r); break;
        case EVENT_WORLDMAP_BEGIN: onWorldmapBegin(r); break;
        case EVENT_WORLDMAP_END: onWorldmapEnd(r); break;
        case EVENT_WORLDMAP_STATE: onWorldmapState(r); break;
        case EVENT_WORLDMAP_SUBTILES: onWorldmapSubtiles(r); break;
        case EVENT_WORLDMAP_AREAS: onWorldmapAreas(r); break;
        case EVENT_PLAYER_ROSTER: onPlayerRoster(r); break;
        case EVENT_INVENTORY_GRANT: onInventoryGrant(r); break;
        case EVENT_INVENTORY_REVOKE: onInventoryRevoke(r); break;
        case EVENT_MOVIE_PLAY: onMoviePlay(r); break;
        case EVENT_MOVIE_SEEN_STATE: onMovieSeenState(r); break;
        case EVENT_MOVIE_STOP: onMovieStop(r); break;
        case EVENT_PLAYER_SHEET: onPlayerSheet(r); break;
        case EVENT_ENCOUNTER_PROMPT: onEncounterPrompt(r); break;
        case EVENT_ENCOUNTER_CLOSE: onEncounterClose(r); break;
        case EVENT_GVAR_DELTA: onGvarDelta(r); break;
        case EVENT_HOLODISK_CLEAR: onHolodiskClear(r); break;
        case EVENT_HOLODISK_ADD: onHolodiskAdd(r); break;
        case EVENT_ELEVATOR_PROMPT: onElevatorPrompt(r); break;
        case EVENT_AUTOMAP_OPEN: onAutomapOpen(r); break;
        case EVENT_LOOT_GRANT: onLootGrant(r); break;
        case EVENT_STATE_AUDIT: onStateAudit(r); break;
        case EVENT_FADE_OUT: onScreenFade(r, true); break;
        case EVENT_FADE_IN: onScreenFade(r, false); break;
        case EVENT_UI_LOCK: onScreenInputLock(r); break;
        case EVENT_MUSIC_PLAY: onMusicPlay(r); break;
        case EVENT_MUSIC_STOP: onMusicStop(r); break;
        // SNAPSHOT_BEGIN/END are pure brackets; presentation cues are cosmetic and
        // ignored headless. All are skipped whole via the event length.
        default: break;
        }
    }

    bool loaded() const { return _loaded; }
    int loadCount() const { return _loadCount; }

private:
    Object* lookup(int netId)
    {
        if (netId == 0) return nullptr; // 0 = "no object"
        auto it = _net.find(netId);
        return it != _net.end() ? it->second : nullptr;
    }

    // Drop EVERY registry reference to `obj` before it is freed (or queued for a
    // deferred free). onDestroy already does this by netId; the inventory
    // reconciles remove items by POINTER and so must erase by value.
    //
    // ►► This is load-bearing, and the comment it replaces was wrong. The
    // reconcile paths used to free items on the belief that "inventory items are
    // never in the _net registry" — true only before the netId map started
    // indexing inventories (see onDestroy's own note, which records exactly that
    // change). Since then a carried item CAN be in _net, so freeing it without
    // erasing left the map pointing at freed memory; the next objectDestroyed
    // event for that netId looked it up and freed it again. Live repro: pick up a
    // bomb, plant it, let it explode — the explosion's destroy event lands on an
    // item the reconcile already freed, and the viewer dies in memoryBlockValidate.
    void forgetObjectRefs(Object* obj)
    {
        if (obj == nullptr) return;
        presForgetObject(obj); // glide + combat replay must not outlive the object
        for (auto it = _net.begin(); it != _net.end();) {
            if (it->second == obj) {
                _adoptTransients.erase(it->first);
                it = _net.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Mirror the wire's per-item ammo onto a reconciled inventory item (wire v3). The
    // ItemObjectData union aliases its first int across weapon.ammoQuantity /
    // ammo.quantity / misc.charges, so writing .weapon.ammoQuantity sets the loaded-
    // round / charge count for ANY item type; assign directly (not ammoSetQuantity)
    // to mirror the server value exactly — it was already clamped server-side, and the
    // client must not re-clamp against its own proto. ammoTypePid is the union's second
    // int and only meaningful for a weapon, so apply it weapon-only. This is what makes
    // the client's own _combat_check_bad_shot see a dry weapon as empty (so it stops
    // offering "fire" locally) and the HUD show the true count.
    static void applyWireItemAmmo(Object* item, int ammoQuantity, int ammoTypePid)
    {
        if (item == nullptr) return;
        item->data.item.weapon.ammoQuantity = ammoQuantity;
        if (itemGetType(item) == ITEM_TYPE_WEAPON) {
            item->data.item.weapon.ammoTypePid = ammoTypePid;
        }
    }

    // ►► AN ITEM'S pid CAN CHANGE UNDER A STABLE netId, and the mirror has to follow.
    // Arming a charge is a pid SWAP, not a new item: explosiveActivate rewrites
    // plastic explosives 85 -> 209 (dynamite 51 -> 206) in place, same object, same
    // netId. matchInventorySlot binds netId FIRST — deliberately, so two stacks of one
    // pid stop being interchangeable — which means an armed charge matched its old
    // mirror slot and the pid write never happened: the mirror kept saying "Plastic
    // Explosives" (unarmed) forever, and every pid-addressed verb it sent named an
    // item the server no longer had ("That item is gone" on the steal screen's plant).
    // Rewrite pid AND fid: the inventory sprite comes from the proto's inventory fid,
    // captured at create time, so a pid written alone leaves the old art on screen.
    static void applyWireItemPid(Object* item, int pid)
    {
        if (item == nullptr || pid < 0 || item->pid == pid) return;
        Proto* proto;
        if (protoGetProto(pid, &proto) == -1) return;
        item->pid = pid;
        item->fid = proto->fid;
    }

    // Append a combat-presentation event, enforcing the safety cap. On overflow drop
    // the oldest DROPPABLE event (never a turn-start / exit, so turn boundaries and
    // the fight's end stay intact) — its state already rode the deltas; only a
    // caption or one animation is lost.
    void enqueue(const PresEvent& e)
    {
        _presQueue.push_back(e);
        if (_presQueue.size() <= kMaxQueuedPresEvents) {
            return;
        }
        for (auto it = _presQueue.begin(); it != _presQueue.end(); ++it) {
            // ►►►► NEVER DROP A PARKED STATE EVENT (§12.6 trap 1). Dropping one is not a
            // lost caption — it is a lost DESTROY, i.e. a permanently phantom object: the
            // exact bug §12 exists to fix, reintroduced under backlog. It is also a
            // never-lossy violation (the wire said the object dies; we would silently
            // decide otherwise). Skipped here and force-applied below instead.
            if (it->kind == PresKind::kDeferredEvent) {
                continue;
            }
            if (it->kind != PresKind::kTurnStart && it->kind != PresKind::kExit) {
                if (it->kind == PresKind::kMoveRelease) {
                    // Its held glide must not outlive its release: snap the mover to
                    // its (already authoritative) position instead of stranding it.
                    clientAnimCancel(lookup(it->moveNetId));
                }
                if (it->kind == PresKind::kRecordedSeq) {
                    // This sequence will never execute, so the mints it promised at decode
                    // will never happen — release them, or the netIds stay entangled with
                    // nothing left to un-entangle them and every later event for them waits
                    // out the full cap. The state is unaffected (the pickup DISCONNECT still
                    // drains and the CONNECT self-heal materializes the real item); only the
                    // flight visual is lost, which is what dropping a sequence has always
                    // meant.
                    for (int adoptNetId : it->seqAdopts) {
                        releasePendingAdopt(adoptNetId);
                    }
                }
                _presQueue.erase(it);
                debugPrint("client_net: combat presentation backlog, dropped an event\n");
                return;
            }
        }
        // Nothing droppable left. If a parked state event is holding the line, APPLY it
        // now (do not merely skip it: with the queue at cap and no droppable entries, the
        // gate would never be reached and the event would starve behind its own cap).
        for (auto it = _presQueue.begin(); it != _presQueue.end(); ++it) {
            if (it->kind == PresKind::kDeferredEvent) {
                PresEvent forced = *it;
                _presQueue.erase(it);
                debugPrint("client_net: presentation backlog at cap — deferred event type=%d "
                           "net=%d applied inline\n",
                    (int)forced.defEvType, forced.defNetId);
                Reader dr(forced.defBytes.data(), forced.defBytes.size());
                bool prevDeferring = _applyingDeferredEvent;
                _applyingDeferredEvent = true;
                event(forced.defEvType, dr, forced.defEntryId);
                _applyingDeferredEvent = prevDeferring;
                return;
            }
        }
        _presQueue.pop_front(); // all turn/exit (degenerate) — drop the oldest anyway
    }

    // One promised OBJ_CREATE for this netId has been fulfilled (executed) or cancelled
    // (its sequence was dropped). Erase at zero so `count()` stays a clean predicate.
    void releasePendingAdopt(int netId)
    {
        auto it = _pendingAdopts.find(netId);
        if (it == _pendingAdopts.end()) {
            return;
        }
        if (--it->second <= 0) {
            _pendingAdopts.erase(it);
        }
    }

    // ►► IS THIS netId'S PRESENTATION STILL OWED TO THE VIEWER? (§12.2) Two states, and
    // together they cover the whole life of an adopt transient with no gap:
    //   _pendingAdopts   — an OBJ_CREATE adopting it was DECODED but has not EXECUTED yet
    //                      (the sequence is still sitting on the pump);
    //   _adoptTransients — it HAS executed, the transient exists, and its flight/landing
    //                      is playing or queued.
    // The gap between those two is exactly the window the old decode-mint existed to
    // paper over. Note the ordering property this buys for free: once ONE event for netId
    // X defers, X is entangled for as long as the FIFO holds it, so every LATER event for
    // X defers too — per-netId wire order is preserved end to end by construction, not by
    // a sort.
    bool presEntangled(int netId) const
    {
        if (netId <= 0) {
            return false;
        }
        return _pendingAdopts.count(netId) != 0 || _adoptTransients.count(netId) != 0;
    }

    // Park a state event onto _presQueue when the object it addresses is presentation-
    // entangled, so it applies in wire order relative to the sequences around it instead
    // of on the decode clock (§12.2). Returns true when parked — the caller must then NOT
    // apply it. The handlers' own guards are not bypassed, only postponed: they run
    // unchanged when the drain re-dispatches through event() (§12.6 trap 3).
    bool deferStateEvent(unsigned char type, Reader& r, unsigned int entryId)
    {
        if (_applyingDeferredEvent) {
            return false; // the drain itself — apply for real this time
        }
        // Headless is already inert (onPresSeq returns before the dry pass when the viewer
        // is off, so neither map is ever fed), but say so structurally: no future feeder
        // can move a golden through this path.
        if (!clientViewerActive()) {
            return false;
        }
        // Every deferrable event carries netId as its FIRST field — MOVE, DESTROY,
        // CONNECT, DISCONNECT, OBJECT_DELTA — which is why this check can live once at
        // the dispatcher instead of five times inside the handlers. Peek it on a copy so
        // the caller's reader is untouched when we decline.
        const unsigned char* payload = r.here();
        size_t len = r.remaining();
        Reader peek(payload, len);
        int netId = peek.i32();
        if (peek.overflow() || !presEntangled(netId)) {
            return false;
        }
        PresEvent e;
        e.kind = PresKind::kDeferredEvent;
        e.defEvType = type;
        e.defBytes.assign(payload, payload + len); // length-prefixed, so this is the whole event
        e.defEntryId = entryId;
        e.defNetId = netId;
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[adopt] PARK type=%d net=%d bytes=%d pendingMints=%d live=%d queued=%d\n",
                (int)type, netId, (int)len,
                _pendingAdopts.count(netId) != 0 ? _pendingAdopts[netId] : 0,
                _adoptTransients.count(netId) != 0 ? 1 : 0, (int)_presQueue.size());
        }
        enqueue(e);
        return true;
    }

    void onBlobBegin(Reader& r)
    {
        // Per-baseline tripwire counts: each blob's SNAPSHOT_OBJECT walk gets a
        // fresh score, so the summary line names THIS baseline's alignment.
        _tripwireOk = 0;
        _tripwireBad = 0;
        _blobMapIndex = r.i32();
        _blobElevation = r.i32();
        _blobDudeNetId = r.i32();
        _blobGameTime = r.u32();
        _blobMapVersion = r.u16();
        _blobMapLen = r.u32();
        _blobDudeLen = r.u32();
        _blobCrc = r.u32();
        // Appended after crc32 (MP_PROPOSAL.md Ch 5.3). A pre-co-op server sends
        // nothing here; remaining()==0 then means the one host actor.
        _blobActorCount = r.remaining() >= 2 ? (int)r.u16() : 1;
        if (_blobActorCount < 1) {
            _blobActorCount = 1;
        }
        _blob.clear();
        _blob.reserve(_blobMapLen + _blobDudeLen);
    }

    void onBlobChunk(Reader& r)
    {
        size_t n = r.remaining();
        _blob.insert(_blob.end(), r.here(), r.here() + n);
        r.skip(n);
    }

    void onBlobEnd(Reader&)
    {
        // A world rebuild (applyBlob -> mapLoad -> _obj_remove_all frees gDude) while a
        // viewer MODAL screen is open would dangle the static _pud/_stack pointers the
        // inventory/char UI holds into gDude (crash on the modal's exit tail). Defer until
        // the modal closes; the service ticker force-closes it (ESC) the moment it sees this
        // pending. _blob stays buffered; the main loop applies it once no modal is up.
        if (clientViewerActive() && (GameMode::getCurrentGameMode() & kViewerModalMask) != 0) {
            _blobDeferred = true;
            return;
        }
        applyBlob();
    }

    // The server granted an in-combat inventory screen. It is BROADCAST and
    // addressed by netId (the wire has no per-session event channel), so ignore
    // it unless it names OUR actor — every other viewer sees this same event for
    // the player whose turn it is.
    //
    // Only latches a flag: the inventory screen runs its own blocking loop, and
    // opening it from inside the decoder would re-enter the pump that is
    // currently decoding. The main loop consumes the flag at a safe point.
    void onInventoryGrant(Reader& r)
    {
        int actorNetId = r.i32();
        if (gDude == nullptr || actorNetId != gDude->netId) {
            return;
        }
        _invGrantPending = true;
    }

    // Our turn ended with the inventory still open. Drop the exemption that keeps
    // the service ticker from force-closing it and enqueue the ESC ourselves, so
    // the screen closes at its next top-of-loop check exactly like combat entry
    // closes every other modal.
    void onInventoryRevoke(Reader& r)
    {
        int actorNetId = r.i32();
        if (gDude == nullptr || actorNetId != gDude->netId) {
            return;
        }
        _combatModalOpen = false;
        wmencTagEscInjection("onInventoryRevoke");
        enqueueInputEvent(KEY_ESCAPE);
    }

    void onPlayerRoster(Reader& r)
    {
        int rowCount = (int)r.u16();
        gClientRoster.clear();
        gClientRoster.reserve(rowCount);
        // A row is EXACTLY 15 bytes: slot(4) + actorNetId(4) + sessionId(4) +
        // alive(1) + nameLen(2), matching the encoder in presenter_network.cc.
        // This guard read 17 and so dropped the LAST row of every roster — with a
        // single row it dropped them all. That stayed invisible because a viewer
        // with no matching row falls back to the host actor, which is the correct
        // answer at N=1; the first extra player is where it bites (its row is the
        // one lost, so it renders and equips as a SPECTATOR of P1). Keep this in
        // step with the encoder if a name ever occupies that reserved length.
        constexpr size_t kRosterRowBytes = 15;
        for (int i = 0; i < rowCount && r.remaining() >= kRosterRowBytes; i++) {
            PlayerRosterRow row = {};
            row.slot = r.i32();
            row.actorNetId = r.i32();
            row.sessionId = r.i32();
            row.alive = r.u8() != 0;
            int nameLen = (int)r.u16(); // reserved; v1 emits 0
            if (nameLen > 0) {
                r.skip((size_t)nameLen > r.remaining() ? r.remaining() : (size_t)nameLen);
            }
            gClientRoster.push_back(row);
        }

        // A roster can arrive in the same frame as a blob but AFTER it, or on its
        // own when a binding changes with no rebaseline. Re-deriving here as well
        // as at the end of applyBlob covers both orderings; the operation is
        // idempotent, so doing it twice costs nothing.
        rebindLocalActor();
    }

    // Point gDude at the actor this session owns (MP_PROPOSAL.md Ch 5.6). A
    // session with no roster row is a SPECTATOR: gDude stays the host actor and
    // it renders P1's view, which is exactly what extra viewers do today.
    void rebindLocalActor()
    {
        if (!clientViewerActive() || gClientHostDude == nullptr) {
            return;
        }

        // Resolve MY actor by SLOT, not by the roster's netId. ►► ROOT of a
        // rebaseline crash (ASAN heap-buffer-overflow in critterGetStat): netIds are
        // re-minted every baseline and valid for ONE generation, and the roster can
        // be stale for THIS generation (it arrives as its own event, after the blob).
        // Resolving a stale netId through _net then returns whatever OTHER object now
        // holds that id — a map critter/item, not our actor — and binding gDude to it
        // makes gDude->pid a non-actor pid that the stat/proto machinery dereferences
        // out of bounds. The SLOT is the DURABLE identity (sessionId->slot never
        // changes), and playerActorAt() reads the registry applyBlob just repopulated,
        // so it is always a real player actor for this generation or nothing.
        int mySlot = -1;
        for (const PlayerRosterRow& row : gClientRoster) {
            if (row.sessionId == _mySessionId && _mySessionId != kNoSessionId) {
                mySlot = row.slot;
                break;
            }
        }

        if (mySlot >= 0) {
            _everBoundToSlot = true; // we appeared in a roster under our own session
        }

        Object* mine = mySlot >= 0 ? playerActorAt(mySlot) : nullptr;
        if (mine == nullptr) {
            mine = gClientHostDude; // spectator, or our slot is not present this generation
        }

        if (mine == gDude) {
            return;
        }

        gDude = mine;

        // The inventory screen does NOT read gDude — it operates through its own
        // _inven_dude anchor, set only by _inven_reset_dude(). Every rebaseline
        // runs one of those from _obj_load_dude, and it lands BEFORE this rebind,
        // so the anchor is captured while gDude is still the host actor. Without
        // this re-anchor an extra player's inventory screen reads AND WRITES the
        // host's object (itemAdd/equipmentApply take _inven_dude), so P2 sees and
        // edits P1's gear. Re-anchoring here is what pairs the rebind with the
        // reset, exactly as the camera below pairs it with gElevation.
        //
        // Deliberately NOT guarded on an open inventory modal: a rebind cannot
        // reach this point with one open (blobs defer behind kViewerModalMask),
        // and skipping the reset would strand the anchor until the next rebind.
        // Clear the equipped-slot caches BEFORE re-anchoring. critterGetItem1/2/
        // Armor short-circuit to these globals whenever `critter == _inven_dude`,
        // so re-anchoring _inven_dude at gDude ARMS that path for the rebound
        // actor — a cache left over from the previous body then outranks the real
        // inventory scan and the bar renders an item this actor is not holding.
        // They are only meaningful while an inventory screen is open (detached on
        // open, applied on close), and a rebind cannot land inside one, so
        // dropping them here is the correct pairing, not a workaround.
        gInventoryLeftHandItem = nullptr;
        gInventoryRightHandItem = nullptr;
        gInventoryArmor = nullptr;
        _inven_reset_dude();

        // The HUD bar reads gDude LIVE, so its source was always right — but only
        // whoever CALLS it redraws it, and the only caller is the dude-inventory
        // delta apply. So an extra player's bar kept rendering whatever was drawn
        // while gDude was still the host, until a delta happened to arrive for the
        // rebound actor. Repaint the gDude-derived HUD here so the rebind is what
        // makes it correct, not a later coincidence. All three self-guard on
        // gInterfaceBarWindow == -1, so this is a safe no-op before the bar exists
        // (rebinds can land during a blob apply, ahead of interface init).
        int leftAction, rightAction;
        interfaceGetItemActions(&leftAction, &rightAction);
        interfaceUpdateItems(false, leftAction, rightAction);
        interfaceRenderHitPoints(false);
        interfaceRenderArmorClass(false);

        // The camera and the render elevation belong to MY actor, not the host's
        // — without this a rebound viewer keeps looking at P1.
        if (gDude != nullptr) {
            if (gDude->elevation != gElevation) {
                mapSetElevation(gDude->elevation);
            }
            tileSetCenter(gDude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
        }

        debugPrint("client_net: session %d bound to actor netId %d\n", _mySessionId,
            gDude != nullptr ? gDude->netId : 0);
    }

    void applyBlob()
    {
        _blobDeferred = false;
        unsigned int expected = _blobMapLen + _blobDudeLen;
        if (_blob.size() != expected) {
            debugPrint("client_net: blob length mismatch got=%zu expected=%u\n", _blob.size(), expected);
            return;
        }
        if (crc32Of(_blob.data(), (int)_blob.size()) != _blobCrc) {
            debugPrint("client_net: blob crc32 mismatch\n");
            return;
        }

        // Load the blob straight from memory (no scratch file), through the SAME
        // sequence as the S1 loader (main.cc F2_CLIENT_BLOB_IN): adopt the server
        // clock, drop any current map, viewer-load (no map-enter procs), apply the
        // dude, fix combat back-pointers, reproduce the netId walk, freeze scripts.
        // ►► A RAM-backed stream is mandatory here, not a convenience: rebaselines are
        // broadcast, so several viewers on one machine would otherwise write-then-read
        // a SHARED scratch file and one client's truncate would corrupt another's
        // in-flight mapLoad (the co-op "black screen" root, 2026-07-23). See
        // fileOpenMemory / XFILE_TYPE_MEMORY.

        // ►► UNDO ANY PRIOR REBIND BEFORE THE LOAD (MP_PROPOSAL.md Ch 5.3).
        // _obj_load_dude memcpy's the blob's dude INTO *gDude; if gDude still
        // aims at a previous generation's extra actor, that write lands on an
        // object mapLoad is about to free — or has freed — and corrupts memory.
        // gClientHostDude is the objectsInit-made NO_REMOVE dude, the one actor
        // object that survives every load, so it is always a safe target.
        // The binding is re-derived after seedNetMap, below.
        if (gClientHostDude == nullptr) {
            gClientHostDude = gDude;
        }
        gDude = gClientHostDude;

        presReset(); // stale Object* die with the world; ends in-flight replay/door reg_anim first

        // ►► CLEAR THE netId MAP BEFORE mapLoad FREES THE WORLD (APPLYBLOB_TEARDOWN_PLAN
        // step 2 — the root fix for the rebaseline UAF cluster). _net used to be cleared
        // only inside seedNetMap, AFTER the load; but mapLoad → _obj_remove_all frees the
        // entire previous world below, so for the whole teardown window _net held pointers
        // into freed objects. applyBlob is not yet re-entrancy-safe (step 5), so any wire
        // event that lands mid-teardown calls lookup() → resolves a stale netId to freed
        // memory → the heap-use-after-free seen in onSnapshotObject. Emptying the map here
        // makes that lookup miss (returns null, event no-ops) instead of faulting.
        // seedNetMap below is now populate-only.
        _net.clear();
        _adoptTransients.clear(); // adopt transients die with the old world (Fable review A3)
        _pendingAdopts.clear(); // ...and so do pending adopt mints (§12.6 trap 2)

        // Drop the previous blob's actor registrations BEFORE mapLoad frees the
        // objects they point at (MP_PROPOSAL.md Ch 5.3). Same reasoning as
        // presReset one line up: a registry entry that outlives its object is
        // not merely dead, it silently resolves to whatever gets allocated next.
        // Cleared here rather than after the load so nothing running INSIDE
        // mapLoad can consult a dangling entry; an empty registry degenerates to
        // `obj == gDude`, which is exactly the pre-co-op behavior.
        playerActorClear();

        gameTimeSetTime(_blobGameTime);
        gMapHeader.name[0] = '\0';

        File* rd = fileOpenMemory(_blob.data(), _blob.size());
        if (rd == nullptr) { debugPrint("client_net: cannot open blob memory stream\n"); return; }

        mapSetViewerLoad(true);
        int loadRc = mapLoad(rd);
        mapSetViewerLoad(false);
        if (loadRc == -1) {
            debugPrint("client_net: blob map load failed\n");
            fileClose(rd);
            return;
        }
        if (_obj_load_dude(rd) == -1) {
            debugPrint("client_net: blob dude load failed\n");
            return; // _obj_load_dude closed the stream on its error path
        }

        // Re-seed the registry from THIS blob (cleared above, before mapLoad).
        // Slot 0 is the local gDude object, which is NO_REMOVE and made once at
        // objectsInit, so it is the one actor object that survives the load.
        //
        // ORDER IS LOAD-BEARING: registration must complete BEFORE
        // objectAssignAllNetIds, because that walk numbers the registry first and
        // in slot order. Register out of order and every netId after the actors
        // shifts — the wire would then silently address the wrong objects.
        // The blob's actors carry sheet pids (kPlayerActorPidBase+slot), so the
        // rows they name must exist before _obj_load_player_actor resolves one —
        // an unseeded row renders a nameless body with fid 0.
        //
        // These seeds are the BASE, not the answer: they fill the parts of a row
        // that never differ between player actors (fid, messageId, flags, the AI
        // packet) from this viewer's own gDudeProto. The sheet proper — skills,
        // SPECIAL, perks, XP/level, traits — is then overwritten from the blob's
        // sheet block below, which is the server's truth. Seeding alone was the
        // whole story before that block existed, and it goes stale the moment
        // anyone levels.
        protoPlayerActorSheetsSeed();
        perkPlayerActorSeedRanks();
        pcPlayerActorSeedStats();
        traitsPlayerActorSeed();
        skillsPlayerActorSeed();
        critterPlayerActorSeedNames();

        playerActorRegister(gDude);
        for (int slot = 1; slot < _blobActorCount; slot++) {
            Object* actor = nullptr;
            if (_obj_load_player_actor(rd, &actor) == -1 || actor == nullptr) {
                debugPrint("client_net: blob actor %d load failed\n", slot);
                fileClose(rd);
                return;
            }
            if (playerActorRegister(actor) != slot) {
                debugPrint("client_net: blob actor %d registration failed\n", slot);
                fileClose(rd);
                return;
            }
        }

        // The sheets ride at the tail, from slot 0 — AFTER the registry is
        // populated, because the block is applied by slot. Fail loud: a sheet
        // read that goes wrong writes into gDudeProto itself (slot 0 is the
        // literal struct, not a copy), so continuing with a half-applied block
        // means playing on a corrupted character rather than a missing one.
        if (playerSheetBlockRead(rd) == -1) {
            debugPrint("client_net: blob sheet block load failed\n");
            fileClose(rd);
            return;
        }
        fileClose(rd);

        _map_fix_critter_combat_data();
        objectAssignAllNetIds();
        scriptsDisable();

        seedNetMap();

        // ACTOR-LEAK TRIPWIRE. Every player actor carries the dude proto's pid, and
        // after a load the world must hold exactly the registered ones — no more.
        // A survivor of the teardown (anything that got OBJECT_NO_REMOVE back on it)
        // is otherwise SILENT: its netId is zeroed by the walk, so nothing on the wire
        // ever addresses it and no other check notices; it just stands on the map
        // forever as a duplicate body, one more per rebaseline. Cheap (one walk, once
        // per baseline) and it names the class directly.
        // The pid set is read off the REGISTRY rather than assuming the host's proto:
        // co-op v1 shares one gDudeProto, so today this is a one-element set, but
        // per-player protos (own name/appearance/sheet) are an explicit direction and
        // would silently blind a `pid == gDude->pid` test to every non-host body.
        std::unordered_set<int> actorPids;
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* a = playerActorAt(slot);
            if (a != nullptr) {
                actorPids.insert(a->pid);
            }
        }
        int actorPidObjects = 0;
        for (Object* o = objectFindFirst(); o != nullptr; o = objectFindNext()) {
            if (actorPids.count(o->pid) != 0) {
                actorPidObjects++;
            }
        }
        // ►► COMPARE AGAINST THE *PLACED* ACTORS, NOT ALL REGISTERED ONES. A slot whose
        // owner is offline is PARKED at tile -1 (the presence model: "slot N body parked"),
        // and a parked body is not in any tile list — so the walk above cannot see it and
        // the count can NEVER match while anyone is away. The result was a LEAK warning on
        // literally every join of a session with an absent player, which is the worst thing
        // a leak detector can do: cry wolf so reliably that a real leak reads as normal.
        int placedActors = 0;
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* a = playerActorAt(slot);
            if (a != nullptr && a->tile != -1) {
                placedActors++;
            }
        }
        if (actorPidObjects != placedActors) {
            debugPrint("client_net: ACTOR LEAK — %d actor-pid objects in the world, %d placed "
                       "(%d registered incl. parked)\n",
                actorPidObjects, placedActors, playerActorCount());
        }

        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[actors] blobActorCount=%d registered=%d worldActorPidObjects=%d\n",
                _blobActorCount, playerActorCount(), actorPidObjects);
            for (int slot = 0; slot < playerActorCount(); slot++) {
                Object* a = playerActorAt(slot);
                fprintf(stderr, "[actors] slot=%d obj=%p netId=%d pid=0x%X tile=%d elev=%d flags=0x%X fid=0x%X\n",
                    slot, (void*)a, a ? a->netId : -1, a ? a->pid : 0, a ? a->tile : -1,
                    a ? a->elevation : -1, a ? a->flags : 0, a ? a->fid : 0);
            }
        }

        // Cross-check the HOST actor's netId, not gDude's: the rebind below may
        // move gDude off slot 0, and _blobDudeNetId names the host.
        if (gClientHostDude != nullptr && gClientHostDude->netId != _blobDudeNetId) {
            debugPrint("client_net: dude netId %d != blob dudeNetId %d\n",
                gClientHostDude->netId, _blobDudeNetId);
        }

        // Re-derive this session's actor now that _net can resolve netIds. Runs
        // here AND in the roster decoder because the two can arrive in either
        // order; it is idempotent.
        rebindLocalActor();

        _loaded = true;
        _loadCount++;
        _dudeHpAuth = (gDude != nullptr) ? gDude->data.critter.hp : 0; // per-hit HP baseline
        _dudeHpSeeded = (gDude != nullptr); // shown == auth at (re)baseline; no spurious roll
        // Combat framing across a rebaseline. A mid-fight re-sync (we were still in
        // combat, no map transition ended it) KEEPS the framing — _inCombat/_myTurn,
        // the end buttons, the AP dots — so the server's re-emitted combatEnter is
        // ignored as a duplicate (onCombatEnter early-out) instead of replaying the
        // door-slide fanfare on an already-fighting client. Only the transient
        // presentation tied to the old object list is dropped (the in-flight anim was
        // already cleared above; the queue re-resolves netIds but its pre-reload
        // attacks are cosmetic and stale). A fresh join / map transition does a full
        // reset. (Root cause — existing clients being force-reloaded on another
        // client's join — is banked server-side, STEP-5 netId-sidecar.)
        bool reassert = clientViewerActive() && _inCombat;
        if (reassert) {
            // Drop the stale attack/feedback events (they belong to the pre-reload
            // world), but PRESERVE a pending end-of-combat. The server's rebaseline
            // re-emit is combatEnter+turnStart gated on isInCombat(), so a dropped
            // kExit is NEVER re-sent — discarding it would strand us _inCombat forever
            // (combatBusy stuck via !myTurn, and onCombatEnter early-outs so even a new
            // fight can't recover). Re-queue the exit so presentationPump ends combat.
            bool exitPending = false;
            for (const PresEvent& e : _presQueue) {
                if (e.kind == PresKind::kExit) {
                    exitPending = true;
                    break;
                }
            }
            _presQueue.clear();
            // Parked state events die with the queue here, which is CORRECT — the blob is
            // truth for the new world, so a deferred DISCONNECT addressing the old one has
            // nothing left to destroy. The matching _pendingAdopts.clear() rides the
            // adopt-transient clear above; both must go together (§12.6 trap 2).
            _pendingDudeTick = 0;
            _dudeApDeferring = false;
            if (exitPending) {
                PresEvent e;
                e.kind = PresKind::kExit;
                _presQueue.push_back(e);
            }
            // The reload rebuilt the object list with outline=0 (objectRead clears it)
            // and RE-MINTED netIds, so the stored actor id is stale — reset it (the
            // server re-emits TURN_START after the rebaseline, which re-sets it). The
            // recompute re-lights your crosshair highlight over the fresh list now; the
            // acting-critter outline returns on that TURN_START. No stale-netId leak (#8).
            _combatActorNetId = 0;
            recomputeCombatOutlines();
        } else {
            setInCombat(false);
            _myTurn = false;
            if (clientViewerActive()) {
                clearCombatMirror();
            }
        }
        debugPrint("client_net: world loaded (load #%d)\n", _loadCount);

    }

    // Index an object's inventory (recursively) into the netId map. MIRRORS the
    // server's objectAssignInventoryNetIds (object.cc): the numbering walk gives
    // every CARRIED item a netId too, and the viewer reproduces that same walk at
    // load, so these netIds already exist on our copies — they were simply never
    // indexed, because objectFindFirst/Next enumerates the TILE BUCKETS and a
    // carried item sits in no bucket.
    //
    // Load-bearing for anything that moves an item from an inventory into the
    // world: the server drops a stack and ships EVENT_CONNECT for the item's
    // netId, but lookup() missed it and onConnect returns SILENTLY on a null
    // object — the item vanished from the inventory and never appeared on the
    // ground, with nothing logged. Classic "right state, nothing addresses it".
    void indexInventoryNetIds(Object* owner)
    {
        Inventory* inv = &(owner->data.inventory);
        for (int i = 0; i < inv->length; ++i) {
            Object* item = inv->items[i].item;
            if (item == nullptr) {
                continue;
            }
            if (item->netId != 0) {
                _net[item->netId] = item;
            }
            indexInventoryNetIds(item);
        }
    }

    // Unlink `item` from whatever critter/container inventory currently holds it,
    // if any. Called before an item is connected into the WORLD so it never sits
    // in an inventory and the world list at the same time (see onConnect).
    //
    // Searches rather than consulting a cached owner map on purpose: a map would
    // go stale on every inventory delta, and this runs only on the rare
    // inventory->world transition, where being right beats being quick.
    void unlinkFromAnyInventory(Object* item)
    {
        auto tryOwner = [&](Object* owner) -> bool {
            if (owner == nullptr || owner == item) {
                return false;
            }
            Inventory* inv = &(owner->data.inventory);
            for (int i = 0; i < inv->length; i++) {
                if (inv->items[i].item == item) {
                    itemRemove(owner, item, inv->items[i].quantity);
                    return true;
                }
            }
            return false;
        };

        if (tryOwner(gDude)) {
            return;
        }
        for (Object* o = objectFindFirst(); o != nullptr; o = objectFindNext()) {
            if (tryOwner(o)) {
                return;
            }
        }
    }

    // Bind a mirror item to the server's netId for it, and make it addressable.
    // Items the mirror creates locally from a delta carry no netId until the next
    // rebaseline renumbers everything; without this they stay anonymous, so the
    // inventory verbs cannot name them and matching falls back to pid forever.
    void adoptItemNetId(Object* item, int netId)
    {
        if (item == nullptr || netId == 0) {
            return;
        }
        if (item->netId != 0 && item->netId != netId) {
            _net.erase(item->netId); // re-minted: drop the stale key rather than alias it
        }
        item->netId = netId;
        _net[netId] = item;
    }

    // Find the mirror slot a wire stack refers to. netId is the real identity;
    // pid is a fallback for stacks that have not been bound yet (pre-adoption
    // mirrors, and any path that still ships pid alone). `claimed` keeps
    // duplicate pids mapping 1:1 instead of all collapsing onto one slot.
    int matchInventorySlot(Inventory* inv, int origLen, std::vector<char>& claimed,
        int wantNetId, int wantPid)
    {
        if (wantNetId != 0) {
            for (int i = 0; i < origLen; i++) {
                if (!claimed[i] && inv->items[i].item != nullptr
                    && inv->items[i].item->netId == wantNetId) {
                    return i;
                }
            }
        }
        for (int i = 0; i < origLen; i++) {
            if (!claimed[i] && inv->items[i].item != nullptr
                && inv->items[i].item->pid == wantPid) {
                return i;
            }
        }
        return -1;
    }

    void seedNetMap()
    {
        // Populate-only: _net/_adoptTransients are now cleared at the TOP of applyBlob's
        // teardown (before mapLoad frees the world), not here — see APPLYBLOB_TEARDOWN_PLAN
        // step 2. This still runs after every rebaseline's load, so the map is rebuilt from
        // the fresh world exactly as before; it just no longer owns the clear.
        for (Object* o = objectFindFirst(); o != nullptr; o = objectFindNext()) {
            if (o->netId != 0) {
                _net[o->netId] = o;
            }
            indexInventoryNetIds(o);
        }
        if (gDude != nullptr && gDude->netId != 0) {
            _net[gDude->netId] = gDude;
        }
        // The dude is added explicitly above because it can be absent from the
        // bucket walk; its CARRIED items need the same explicit treatment, and
        // they are the ones a player actually drops.
        if (gDude != nullptr) {
            indexInventoryNetIds(gDude);
        }
    }

    void onSpawn(Reader& r)
    {
        int netId = r.i32();
        int pid = r.i32();
        int tile = r.i32();
        int elev = r.i32();
        int fid = r.i32();
        unsigned int flags = (unsigned int)r.i32(); // birth flags (encoder appends)
        if (netId == 0) return; // NO_SAVE transient — not addressable (§C, C2)
        Object* obj = nullptr;
        if (objectCreateWithFidPid(&obj, fid, pid) == -1 || obj == nullptr) return;
        // Reject a spawn whose pid does not resolve to a proto. objectCreateWithFidPid
        // returns success and stores the bad pid anyway, which would arm every
        // unchecked protoGetProto(obj->pid)-> deref downstream (the item.cc getters).
        // protoGetProto auto-loads a valid-but-unloaded proto, so this only rejects a
        // genuinely corrupt/unknown wire pid — and it is a cheap lookup hit here since
        // objectCreateWithFidPid just resolved the same pid.
        Proto* spawnProto = nullptr;
        if (protoGetProto(pid, &spawnProto) == -1 || spawnProto == nullptr) {
            objectDestroy(obj, nullptr);
            debugPrint("client_net: SPAWN rejected — pid 0x%X does not resolve\n", pid);
            return;
        }
        objectSetLocation(obj, tile, elev, nullptr);
        // Birth flags, minus the server's lifetime classification: a viewer-side
        // spawn must die with the world like every other blob-loaded object.
        objectApplyWireFlags(obj, flags);
        obj->netId = netId;
        _net[netId] = obj;
        debugPrint("client_net: SPAWN netId=%d pid=0x%X tile=%d fid=0x%X\n", netId, pid, tile, fid);
    }

    void onMove(Reader& r)
    {
        int netId = r.i32();
        int fromTile = r.i32();
        int toTile = r.i32();
        int fromElev = r.i32();
        int toElev = r.i32();
        int durMs = r.i32(); // >0 = stepped hop, animate over ~durMs (§2)
        // run: appended after durMs. AUTHORITATIVE — the viewer must not infer the
        // walk/run cycle from durMs (that made the animation depend on server load).
        // remaining()>=4 keeps a server that predates the field readable: absent =>
        // walk, which is what the old heuristic produced for every unstamped mover.
        bool run = r.remaining() >= 4 ? (r.i32() != 0) : false;
        Object* obj = lookup(netId);
        if (obj != nullptr) {
            // In-combat recorded MOVE: this mover's walk is being replayed from the record
            // channel, which owns its motion. HOLD the authoritative position (applied at
            // walk completion, resolveHeld) — do NOT snap/glide/kMoveRelease/notifyReposition
            // it here, or the sprite jumps to the destination and the replayed walk (from the
            // origin) has nothing left to animate. Only an armed recorded-walk mover defers;
            // every other object (incl. knockback) keeps the path below bit-for-bit.
            if (clientViewerActive() && clientCombatAnimDeferMove(obj, toTile, toElev)) {
                if (getenv("F2_TRACE_EVENTS") != nullptr) {
                    fprintf(stderr, "[cmove-hold] net=%d authTile=%d (held for recorded walk)\n", netId, toTile);
                }
                return;
            }
            // Coupled knockback commit (Pillar 1 / bug J): a durMs<=0 snap for an object
            // with a LIVE replay is the displacement its recorded/replayed slide animates —
            // HOLD it (leave obj at the origin) so the slide runs from there, and let
            // resolveHeld commit the tile at the slide's action frame. Only in combat, only
            // a real snap (durMs<=0); a stepped hop takes the glide path below. No replay
            // entry → not held → snaps exactly as before (bounded blast radius).
            if (clientViewerActive() && durMs <= 0 && _inCombat
                && clientCombatAnimDeferSnapMove(obj, toTile, toElev)) {
                if (getenv("F2_TRACE_EVENTS") != nullptr) {
                    fprintf(stderr, "[knock-hold] net=%d authTile=%d (held for coupled slide)\n", netId, toTile);
                }
                return;
            }
            // MY actor changed floor → follow it (MP_PROPOSAL Ch 14.3). gElevation
            // is this viewer's CAMERA, so each client follows its own actor and
            // nobody else's; without this the actor walks up the stairs and simply
            // vanishes, because the viewer keeps rendering the old floor. Keyed on
            // gDude, which on the viewer is the per-client ROLE, not the host.
            if (clientViewerActive() && obj == gDude && toElev != gElevation) {
                mapSetElevation(toElev);
                tileSetCenter(toTile, TILE_SET_CENTER_REFRESH_WINDOW);
            }
            // ►► MY actor TELEPORTED on the same floor → follow it too. In single-player
            // the recenter for a scripted relocation lives inside opMoveTo's
            // `object == gDude` branch (interpreter_extra.cc), which on a dedicated server
            // runs server-side where there is no camera, and is never streamed.
            // objectSetLocation only recenters on an ELEVATION change (object.cc), and the
            // branch above mirrors exactly that — so a same-floor script teleport left the
            // camera on the old spot with the player off-screen.
            //
            // Re-derived here rather than pushed as an event on purpose: every relocation
            // path (script move_to, critter_attempt_placement, the co-op fan-out that
            // places OTHER players, exit grids, elevators) arrives as a position change, so
            // one condition covers them all and each viewer times it off its own actor. An
            // emit per relocation site would be the whack-a-mole this replaces.
            //
            // Gated on a non-adjacent jump so ordinary walking is untouched: a walk is a
            // sequence of 1-tile hops and already scrolls the camera by its own machinery.
            else if (clientViewerActive() && obj == gDude
                && tileDistanceBetween(fromTile, toTile) > 1) {
                tileSetCenter(toTile, TILE_SET_CENTER_REFRESH_WINDOW);
            }
            // State first, always (authoritative, never lags the wire); the
            // presentation layer then decides whether the RENDERING glides
            // (durMs>0 hop) or stays snapped. No-op unless the viewer enabled it.
            objectSetLocation(obj, toTile, toElev, nullptr);
            // In-combat glide sequencing (§3.d): an in-combat stepped hop (durMs>0)
            // is HELD and released in wire order by the presentation pump, so a
            // not-yet-presented turn's approach cannot glide over the current turn.
            // Out of combat (free-roam) and headless keep the immediate glide/snap;
            // knockback/teleport hops (durMs<=0) never hold (they snap regardless).
            // ALSO hold (in EITHER mode) when a door slide is pending/active on this
            // beat: the server opens the door in the same step it moves through, so the
            // DOOR_STATE event decodes just before this MOVE — holding the hop lets the
            // door finish opening before the crosser glides through (no warp).
            bool holdGlide = clientViewerActive() && durMs > 0
                && _inCombat;
            if (clientViewerActive() && getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[move] net=%d from=%d to=%d dElev=%d dur=%d hold=%d inCombat=%d\n",
                    netId, fromTile, toTile, (fromElev != toElev) ? 1 : 0, durMs, holdGlide ? 1 : 0, _inCombat ? 1 : 0);
            }
            clientAnimOnMove(obj, fromTile, toTile, fromElev, toElev, durMs, holdGlide, run);
            if (holdGlide) {
                // Coalesce consecutive hops of one move (they decode back-to-back
                // with nothing between) into a single release event.
                if (!_presQueue.empty() && _presQueue.back().kind == PresKind::kMoveRelease
                    && _presQueue.back().moveNetId == netId) {
                    _presQueue.back().moveHops++;
                } else {
                    PresEvent e;
                    e.kind = PresKind::kMoveRelease;
                    e.moveNetId = netId;
                    e.moveHops = 1;
                    enqueue(e);
                }
            }
            // A MOVE authoritatively repositioned obj; if it was mid attack-replay
            // (e.g. knockback rides MOVE, §3.c), stop holding its pose and let its
            // final fid/flags land now (client_combat_anim tripwire).
            clientCombatAnimNotifyReposition(obj);
            _moveHit++;
            // A move changed positions → LOS may have changed. Recompute (your crosshair
            // highlight tracks LOS as you round a corner; the acting critter's outline
            // tracks its own move). Positions are authoritative here; the recompute leads
            // the glide slightly, which is imperceptible. (Coalesced multi-hop moves
            // recompute per hop — cheap and turn-sparse; batch-debounce banked.) (#8)
            if (_inCombat) {
                recomputeCombatOutlines();
            }
        } else {
            _moveMiss++;
        }
        if (((_moveHit + _moveMiss) % 200) == 0) {
            debugPrint("client_net: MOVE applied=%d missed=%d (last netId=%d tile=%d)\n",
                _moveHit, _moveMiss, netId, toTile);
        }
    }

    void onDestroy(Reader& r)
    {
        int netId = r.i32();
        r.i32(); // pid
        Object* obj = lookup(netId);
        if (obj != nullptr) {
            // Never free the local dude from a wire DESTROY on the viewer: gDude is in _net,
            // and freeing it would dangle every pointer the interface/inventory UI holds into
            // it (the same lifetime hazard the blob path defers, but this free was NOT). The
            // dude's lifetime is owned by the blob/load path (mapLoad rebuilds it); a stray
            // destroy for the claimed actor is anomalous — ignore it. (Review HIGH, Slice 3a.)
            //
            // Co-op: gDude may have been REPOINTED at a non-host actor (Ch 5.6),
            // so this test now protects the BOUND actor — correct, that is the
            // one the UI holds pointers into. The host-actor object needs the
            // same protection independently: it is the process's one NO_REMOVE
            // dude object, so freeing it is fatal whether or not this viewer
            // happens to be driving it.
            if (clientViewerActive() && (obj == gDude || obj == gClientHostDude)) {
                return;
            }
            // If this is the container/corpse an open loot screen is bound to, freeing it
            // now dangles _target_pud/target/critters[] in inventoryOpenLooting — its exit
            // path still reads `target` (lootTargetReattach). DEFER the free and force-close
            // the loot modal (ESC), like the dude / combat-enter guards; the free is flushed
            // after the screen closes (main.cc viewerPollPendingLoot →
            // clientViewerFlushDeferredItemFrees). Rare (needs a scripted removal of the
            // exact looted object mid-modal; combat entry already force-closes). Review HIGH H1.
            if (clientViewerActive() && gViewerLootTargetNetId != 0
                && obj->netId == gViewerLootTargetNetId) {
                presForgetObject(obj);
                _net.erase(netId);
                gViewerLootTargetNetId = 0; // stop full-reconciling a target about to be freed
                gDudeDeferredItemFrees.push_back(obj); // freed on modal close, not now
                wmencTagEscInjection("loot screen close");
                enqueueInputEvent(KEY_ESCAPE); // close the loot screen at its top-of-loop check
                return;
            }
            presForgetObject(obj); // glide + combat replay must not outlive the object
            // A CARRIED item must leave its owner's inventory before it is freed,
            // or that inventory keeps a dangling InventoryItem::item and the next
            // world teardown walks it (_obj_remove_all -> _obj_inven_free) and
            // dies on freed memory. Carried items became reachable here only once
            // the netId map started indexing inventories, so before that this
            // DESTROY silently did nothing and the hazard could not arise.
            // Live repro: `give 7 2` merges stacks server-side and destroys the
            // merged-away duplicate, which is an item inside an actor.
            unlinkFromAnyInventory(obj);
            objectDestroy(obj, nullptr);
            _net.erase(netId);
            _adoptTransients.erase(netId); // drop any bridge ref to a now-freed transient
        }
    }

    void onConnect(Reader& r)
    {
        int netId = r.i32();
        int pid = r.i32();
        int tile = r.i32();
        int elev = r.i32();
        Object* obj = lookup(netId);
        if (obj == nullptr) {
            // ►► SELF-HEAL: an ITEM just appeared at a world tile and this viewer has no
            // object for it. Dropping the event (what this did) leaves the item lying on
            // the server's ground and INVISIBLE here, forever — owner-reported as "removed
            // 1 from inv, nothing on ground", and as two players seeing different rocks on
            // the same floor. The binding can legitimately be missing: a thrown weapon's
            // ground object is a viewer-local adopt transient that onDisconnect DESTROYS
            // and un-binds on pickup, and a partial drop peels a NEW server object whose
            // SPAWN this viewer may never have had a reason to hold. CONNECT carries
            // pid+tile+elevation, which is everything needed to materialize it, so build
            // the mirror rather than diverge from the server.
            //
            // ITEMS ONLY, deliberately: this event is the item<->world lifecycle signal
            // (object.cc _obj_connect). A missing critter/scenery binding is a different
            // (and worse) bug — inventing a body would hide it, so trace and drop.
            if (netId > 0 && PID_TYPE(pid) == OBJ_TYPE_ITEM) {
                Object* healed = nullptr;
                if (objectCreateWithPid(&healed, pid) == 0 && healed != nullptr) {
                    objectSetLocation(healed, tile, elev, nullptr);
                    healed->netId = netId;
                    _net[netId] = healed;
                    // Async wire repaint: nothing else redraws this frame, so an item
                    // materialized here stays invisible until some other event forces a
                    // repaint ([[viewer-netid-map-indexes-inventories]]).
                    Rect rect;
                    objectGetRect(healed, &rect);
                    tileWindowRefreshRect(&rect, elev);
                    debugPrint("client_net: CONNECT healed unknown netId=%d pid=0x%X tile=%d\n",
                        netId, pid, tile);
                } else {
                    debugPrint("client_net: CONNECT netId=%d pid=0x%X could not materialize\n",
                        netId, pid);
                }
            } else {
                debugPrint("client_net: CONNECT for unknown netId=%d pid=0x%X (not an item) dropped\n",
                    netId, pid);
            }
            return;
        }
        {
            // A thrown weapon's flight transient owns its OWN placement via the recorded seq
            // (created + located at DECODE, then flown by the seq's MOVE ops). The server
            // ships the throw seq BEFORE the weapon's EVENT_CONNECT, so by here the transient
            // already has a world list node. _obj_connect does NOT check for that — it would
            // add a SECOND node for the same Object: one flies, the stranded one renders the
            // phantom spear at the origin AND corrupts the object list, crashing objectsExit
            // on teardown (Fable review A1/B). The seq is authoritative for this object's
            // position, so drop the CONNECT entirely.
            if (_adoptTransients.count(netId) != 0) {
                return;
            }
            // A CONNECT authoritatively repositions the object (no glide) — a teleport
            // suspect if it fires on a critter that is mid-combat and on-screen. Trace the
            // tile jump so a live warp names itself (obj->tile is the pre-connect value).
            if (clientViewerActive() && getenv("F2_TRACE_EVENTS") != nullptr && obj->tile != tile) {
                fprintf(stderr, "[connect] net=%d tile=%d->%d elev=%d (authoritative reposition)\n",
                    netId, obj->tile, tile, elev);
            }
            clientAnimCancel(obj); // authoritative reposition outranks a glide
            // SINGLE MEMBERSHIP, and it is a crash-safety invariant, not tidiness.
            // _obj_connect gives this object a WORLD list node; if it is also still
            // listed in some critter's inventory, teardown frees it TWICE — once
            // through _obj_remove_all's world walk and once through _obj_inven_free
            // — which corrupts the allocator guard and segfaults in memoryBlock-
            // Validate (observed: a rebaseline when a second viewer joins).
            // Unlink from the owning inventory FIRST, so the object is only ever in
            // one place. Doing it here also covers a stale/re-minted netId that
            // happens to resolve to a carried item.
            unlinkFromAnyInventory(obj);
            // ►► REPAINT THE ARRIVAL TILE, for the same reason onDisconnect repaints the
            // vacated one: _obj_connect only links the object into the tile list, it draws
            // nothing, and an async wire event has no frame-level repaint riding along. With
            // nullptr here a dropped item was really on the ground and simply not PAINTED
            // until some unrelated event forced a redraw — indistinguishable from "my drop
            // did nothing".
            Rect rect;
            if (_obj_connect(obj, tile, elev, &rect) == 0) {
                tileWindowRefreshRect(&rect, elev);
            }
        }
    }

    void onDisconnect(Reader& r)
    {
        int netId = r.i32();
        r.i32(); // pid
        Object* obj = lookup(netId);
        if (clientViewerActive() && getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[disc] net=%d found=%d\n", netId, obj != nullptr ? 1 : 0);
        }
        if (obj != nullptr) {
            presForgetObject(obj);
            auto it = _adoptTransients.find(netId);
            if (it != _adoptTransients.end()) {
                // Viewer-local adopt transient (a thrown weapon's flight object). It was
                // never a real synced object, so _obj_disconnect (which frees only the list
                // node, leaks the Object, and strands _net) is wrong. Destroy it outright and
                // drop every reference — no leak, no dangling _net / _adoptTransients / freed
                // node to crash teardown (Fable review A2/B). The viewer's inventory mirror is
                // rebuilt from OBJECT_DELTA, so nothing references the transient afterward.
                //
                // ►►►► DESTROY THE TRANSIENT WE RECORDED, NOT WHATEVER netId RESOLVES TO NOW,
                // and this is a crash, not a nicety. `_net[netId]` is not stable across the
                // throw: pick the weapon back up and the server moves that SAME netId into
                // the dude's pockets, so the inventory reconcile mints a mirror item and
                // adoptItemNetId REBINDS the netId to it — legitimately, it is the real
                // object now. This handler then looked the netId up, got the ITEM, and freed
                // an object still linked in gDude's inventory. Nothing noticed until the next
                // walk of that inventory: open the pack and inventoryRenderSummary reads a
                // freed proto (owner-reported SIGSEGV in itemGetWeight, throw a rock → pick
                // it up → 'I'). The bridge already holds the pointer; use it.
                Object* transient = it->second;
                _adoptTransients.erase(it);
                if (transient != nullptr) {
                    // Only surrender the netId if it is still OURS. Rebound to a real
                    // object, the entry belongs to that object and erasing it would
                    // un-address a live item.
                    auto netIt = _net.find(netId);
                    if (netIt != _net.end() && netIt->second == transient) {
                        _net.erase(netIt);
                    }
                    presForgetObject(transient);
                    // Belt, the same one onDestroy wears: a carried object must leave its
                    // owner's inventory before it is freed. A transient should never BE
                    // carried — but that was equally true of the object this used to free.
                    unlinkFromAnyInventory(transient);
                    objectDestroy(transient, nullptr);
                }
                // `obj` is the netId's CURRENT owner. If that is no longer the transient it
                // is a real object (the picked-up item), and a flight object's disconnect
                // says nothing about it — it is not in the world to be disconnected from.
                return;
            } else {
                // ►► REPAINT THE VACATED TILE. _obj_disconnect only unlinks the
                // object; it does not redraw anything. Passing nullptr for the Rect
                // (as this did) means the sprite stays painted on screen forever,
                // so a picked-up item still LOOKS like it is lying on the ground —
                // the "I press pickup and nothing happens" report. It really was
                // picked up: the FIRE succeeded, the inventory delta arrived, and
                // this very handler removed it from the world. Only the pixels lied.
                //
                // The server does exactly this pairing at its own pickup site
                // (proto_instance.cc _obj_pickup: _obj_disconnect(&rect) then
                // worldInvalidateRect). An async wire event has no frame-level
                // repaint riding along, so it must ask for one itself.
                Rect rect;
                int elevation = obj->elevation;
                _obj_disconnect(obj, &rect);
                tileWindowRefreshRect(&rect, elevation);
            }
        }
    }

    void onObjectDelta(Reader& r)
    {
        int netId = r.i32();
        unsigned int mask = r.u16();
        // Field order MUST match presenter_network.cc objectDelta (bit order).
        int fid = 0, rot = 0, hp = 0, rad = 0, poison = 0, ap = 0, results = 0;
        unsigned int flags = 0;
        bool hasFid = false, hasRot = false, hasFlags = false, hasHp = false;
        bool hasRad = false, hasPoison = false, hasAp = false, hasResults = false;
        if (mask & OBJECT_DELTA_FID) { fid = r.i32(); hasFid = true; }
        if (mask & OBJECT_DELTA_ROTATION) { rot = r.i32(); hasRot = true; }
        if (mask & OBJECT_DELTA_FLAGS) { flags = (unsigned int)r.i32(); hasFlags = true; }
        if (mask & OBJECT_DELTA_HP) { hp = r.i32(); hasHp = true; }
        if (mask & OBJECT_DELTA_RADIATION) { rad = r.i32(); hasRad = true; }
        if (mask & OBJECT_DELTA_POISON) { poison = r.i32(); hasPoison = true; }
        if (mask & OBJECT_DELTA_AP) { ap = r.i32(); hasAp = true; }
        if (mask & OBJECT_DELTA_COMBAT_RESULTS) { results = r.i32(); hasResults = true; }
        // INVENTORY: the trailing bytes are the owner's full top-level list (per item:
        // netId, pid, quantity, flags, ammoQuantity, ammoTypePid — see
        // presenter_network putInventory). Read it now so the reader stays aligned;
        // applied to the mirror below. As of wire v3 the per-item ammo count rides
        // along, so a weapon fired dry / reloaded replicates its real ammo LIVE
        // (applyWireItemAmmo below); before v3 the rebuild reset it to the proto
        // default and a dry weapon still read as full.
        struct WireItem {
            int netId; int pid; int quantity; unsigned int flags;
            int ammoQuantity; int ammoTypePid;
        };
        std::vector<WireItem> invItems;
        bool hasInventory = (mask & OBJECT_DELTA_INVENTORY) != 0;
        if (hasInventory) {
            int count = r.u16();
            for (int i = 0; i < count; i++) {
                WireItem wi;
                wi.netId = r.i32();
                wi.pid = r.i32();
                wi.quantity = r.i32();
                wi.flags = (unsigned int)r.i32();
                wi.ammoQuantity = r.i32();
                wi.ammoTypePid = r.i32();
                invItems.push_back(wi);
            }
        }
        // FRAME then LIGHT (bits 9,10), after the variable-length inventory — matches the writer.
        int frame = 0;
        bool hasFrame = (mask & OBJECT_DELTA_FRAME) != 0;
        if (hasFrame) frame = r.i32();
        int lightDistance = 0, lightIntensity = 0;
        bool hasLight = (mask & OBJECT_DELTA_LIGHT) != 0;
        if (hasLight) { lightDistance = r.i32(); lightIntensity = r.i32(); }
        // PID (bit 11), last — matches the writer.
        int newPid = -1;
        bool hasPid = (mask & OBJECT_DELTA_PID) != 0;
        if (hasPid) newPid = r.i32();

        if (hasFid && getenv("F2_TRACE_EVENTS") != nullptr) {
            Object* traced = lookup(netId);
            // The other half of the wield question: the server can say "armed", but what
            // matters on screen is the fid this viewer settles on. Nibble 0 = unarmed body,
            // so a server-armed critter arriving here with nibble 0 is a REPLICATION gap,
            // while one arriving armed that still shows no weapon is a RENDER gap. Logging
            // both sides is what separates them.
            fprintf(stderr, "[fid] net=%d %s 0x%x -> 0x%x (weapAnimNibble %d -> %d)\n", netId,
                traced != nullptr ? "apply" : "DROP(no object)",
                traced != nullptr ? traced->fid : 0, fid,
                traced != nullptr ? ((traced->fid & 0xF000) >> 12) : -1,
                (fid & 0xF000) >> 12);
        }
        Object* obj = lookup(netId);
        if (obj == nullptr) {
            // ►► SAY SO. A delta for a netId this viewer has no object for used to vanish
            // here without a word, which makes a whole bug class invisible: the server is
            // healthy, the wire is healthy, and the player simply never sees the object or
            // any change to it. That is exactly how the car-trunk hunt burned an evening —
            // the trunk was fine server-side and the viewer had nothing to apply it to.
            // Once per netId, so a genuinely absent object cannot flood the log.
            static std::unordered_set<int> warned;
            if (warned.insert(netId).second) {
                debugPrint("client_net: objectDelta for UNKNOWN netId=%d (mask 0x%x) dropped — "
                           "this viewer has no such object\n",
                    netId, mask);
            }
            return;
        }
        int prevAp = obj->data.critter.combat.ap; // for the dude AP-flash guard below
        // S4 deferred-final-state (§3.c): while obj is an attack participant (reserved
        // at decode, or under active replay) HOLD its fid/flags AND rotation so the
        // fall animation starts with it standing and an attacker doesn't snap to face
        // its target before its turn presents; the held values land verbatim when the
        // replay finishes. Numeric fields are never held — hp/ap/etc. never wait on
        // pixels.
        bool held = clientViewerActive()
            && clientCombatAnimDeferDelta(obj, hasFid, fid, hasFlags, flags, hasRot, rot);
        if (hasFid && getenv("F2_TRACE_EVENTS") != nullptr && obj == gDude) {
            const char* path = held ? "HELD(replay)" : ((clientViewerActive() && clientAnimActiveFor(obj)) ? "DEFERRED(glide)" : "APPLIED(now)");
            fprintf(stderr, "[dude-fid] net=%d newFid=0x%x oldFid=0x%x path=%s\n", netId, fid, obj->fid, path);
        }
        if (!held) {
            if (hasFid) {
                // A gliding critter's authoritative fid is ROUTED onto its walk (landed at
                // drain) instead of written through — a mid-glide write would trip the walk's
                // fid check and stand-slide the run (PRESENTATION_FSM_DESIGN §4.1a). A
                // non-gliding fid applies immediately via the pose helper (frame-gotcha safe).
                if (!(clientViewerActive() && clientAnimDeferFid(obj, fid))) {
                    clientApplyPose(obj, fid);
                }
            }
            if (hasFlags) objectApplyWireFlags(obj, flags);
            if (hasRot) {
                // A gliding critter's authoritative end-facing is DEFERRED onto its glide
                // (applied when the glide drains) so a parked / mid-glide sprite doesn't
                // snap-turn to its post-move direction before it moves — the server faces
                // the critter toward each step, so this delta carries the end facing. A
                // non-gliding rotation (facing without moving) applies immediately.
                if (!(clientViewerActive() && clientAnimDeferRotation(obj, rot))) {
                    objectSetRotation(obj, rot, nullptr);
                }
            }
        }
        // PID: the object became a different KIND of thing (see OBJECT_DELTA_PID). Applied
        // BEFORE the frame/light writes below, because every proto-derived answer — the
        // name, the item type, can-use, and objectSetFrame's own frame-count validation —
        // resolves through it, and applying the new art against the old proto is precisely
        // the mismatch this field exists to end. No re-creation: identity (netId, tile,
        // inventory, script binding) belongs to the object, not to its proto.
        if (hasPid && newPid >= 0 && obj->pid != newPid) {
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[pid] net=%d pid %d -> %d\n", netId, obj->pid, newPid);
            }
            obj->pid = newPid;
        }
        // FRAME: scripted art-frame swaps (dug graves, opened doors/containers, levers)
        // that never streamed before — only fixed on a map reload. Apply to non-critters
        // immediately: scenery/items are never attack participants, so no in-flight replay
        // holds them, and objectSetFrame validates frame < art frameCount (fails safe vs
        // the frame-index-render gotcha). A critter's frame stays owned by its own local
        // animation (the fid/pose path above), so it is deliberately not forced here.
        if (hasFrame && PID_TYPE(obj->pid) != OBJ_TYPE_CRITTER && obj->frame != frame) {
            Rect rect;
            if (objectSetFrame(obj, frame, &rect) == 0) {
                tileWindowRefreshRect(&rect, obj->elevation);
            }
        }
        // LIGHT: scripted per-object emission (op_obj_set_light_level — lamps/glow). Never
        // streamed before. objectSetLight recomputes the tile light + returns the dirtied rect.
        if (hasLight && (obj->lightDistance != lightDistance || obj->lightIntensity != lightIntensity)) {
            Rect rect;
            if (objectSetLight(obj, lightDistance, lightIntensity, &rect) == 0) {
                tileWindowRefreshRect(&rect, obj->elevation);
            }
        }
        // Rebuild the mirror inventory from the wire — the ROOT fix for stale NPC gear:
        // an AI critter that equips/switches its weapon mid-fight flips an in-hand flag,
        // which fires OBJECT_DELTA_INVENTORY; without applying it the mirror stays frozen
        // at join-blob state and critterGetWeaponForHitMode returns the wrong weapon
        // (wrong attack animation AND wrong rendered weapon). _obj_inven_free unlinks +
        // frees the old item objects — via forgetObjectRefs, which erases the registry
        // entry first. (This used to say items "are never in the _net registry"; that
        // stopped being true when the netId map started indexing inventories, and the
        // stale claim is what left freed items reachable by netId. See
        // forgetObjectRefs.) Then recreate from the wire. gDude is SKIPPED: the interface/inventory UI hold
        // Object*s into its inventory, so tearing it down here would dangle them; the
        // dude is server-authoritative but its gear is left to the join blob in v1.
        // A NON-CRITTER container (footlocker, crate, desk, bookshelf) gets the full
        // contents reconcile ALWAYS, not just while its loot screen happens to be
        // open. The loot-target gate below was the only way contents were ever
        // applied, so any change made while your screen was SHUT was discarded and
        // never recovered: another player dropping something into a chest left your
        // mirror permanently stale, and opening it showed an empty container whose
        // "take all" nonetheless worked (the transfer is server-authoritative — the
        // data was right, only the mirror was wrong).
        //
        // A newly-dead corpse is also reconciled in full immediately. Death scripts
        // commonly CREATE loot at the death edge (radscorpions add PID 92, the tail).
        // Waiting until the corpse is already an open loot target loses that one-shot
        // delta: the later window opens empty, while Take All still succeeds because
        // the server inventory was correct. Removal remains lifetime-safe below by
        // unlinking now and deferring the actual free while combat replay is busy.
        bool plainContainer = obj != gDude && PID_TYPE(obj->pid) != OBJ_TYPE_CRITTER;
        bool deadCritter = obj != gDude && PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER
            && (critterIsDead(obj)
                || (hasResults && (results & DAM_DEAD) != 0)
                || (hasHp && hp <= 0));
        if (hasInventory && obj != gDude
            && clientViewerActive()
            && (plainContainer
                || deadCritter
                || (gViewerLootTargetNetId != 0 && obj->netId == gViewerLootTargetNetId)
                // The thief of an open steal session: another player's pack, drawn
                // in the left panel of everyone's screen. See the declaration.
                || (gViewerStealThiefNetId != 0 && obj->netId == gViewerStealThiefNetId)
                // The victim of an open steal session: the right panel, minus the
                // gear the server detached. See the declaration.
                || (gViewerStealTargetNetId != 0 && obj->netId == gViewerStealTargetNetId))) {
            // The container/corpse the viewer is actively LOOTING: reconcile its FULL
            // top-level contents (qty + ADD + REMOVE), so items taken out disappear and
            // items put in appear live in the right-hand panel. This is the dude
            // reconcile pattern (below) applied to the loot target. It is SAFE to
            // free/recreate here — unlike the generic non-dude path, a looted container/
            // corpse is out of combat, so no in-flight attack replay references its
            // items (the exact double-free hazard that keeps that path equip-flags-only).
            // Removed items are deferred-freed while the loot modal holds pointers
            // (flushed on close), mirroring the dude removal. Item identity is by pid
            // (v1); exact-instance targeting + the concurrent-freer race are the banked
            // item-instance-id / container-deferred-free co-op work.
            Inventory* inv = &obj->data.inventory;
            int origLen = inv->length;
            std::vector<char> claimed(origLen, 0);
            for (const WireItem& wi : invItems) {
                if (wi.pid < 0) continue;
                int qty = wi.quantity > 0 ? wi.quantity : 1;
                int m = -1;
                for (int i = 0; i < origLen; i++) {
                    if (!claimed[i] && inv->items[i].item != nullptr
                        && inv->items[i].item->pid == wi.pid) {
                        m = i;
                        break;
                    }
                }
                if (m >= 0) {
                    claimed[m] = 1;
                    inv->items[m].quantity = qty;
                    applyWireItemAmmo(inv->items[m].item, wi.ammoQuantity, wi.ammoTypePid);
                } else {
                    Object* item = nullptr;
                    if (objectCreateWithPid(&item, wi.pid) == 0 && item != nullptr) {
                        _obj_disconnect(item, nullptr); // inventory-only, not in the world
                        applyWireItemAmmo(item, wi.ammoQuantity, wi.ammoTypePid);
                        itemAdd(obj, item, qty);
                    }
                }
            }
            bool anyModalOpen = (GameMode::getCurrentGameMode() & kViewerModalMask) != 0;
            std::vector<Object*> toRemove;
            std::vector<int> toRemoveQty;
            for (int i = 0; i < origLen; i++) {
                if (!claimed[i] && inv->items[i].item != nullptr) {
                    toRemove.push_back(inv->items[i].item);
                    toRemoveQty.push_back(inv->items[i].quantity);
                }
            }
            for (size_t k = 0; k < toRemove.size(); k++) {
                itemRemove(obj, toRemove[k], toRemoveQty[k]);
                // Erase the registry refs FIRST — and do it whether the free happens
                // now or is deferred, because a deferred item is already unlinked and
                // must not be reachable by netId in the meantime either.
                forgetObjectRefs(toRemove[k]);
                if (anyModalOpen || _inCombat) {
                    gDudeDeferredItemFrees.push_back(toRemove[k]);
                } else {
                    objectDestroy(toRemove[k], nullptr);
                }
            }
            // Only the container actually on screen needs a repaint. A background
            // container reconciled by the plainContainer rule above has no visible
            // panel to refresh, and raising the flag for it would make an open loot
            // screen repaint some OTHER container's changes as if they were its own.
            if (gViewerLootTargetNetId != 0 && obj->netId == gViewerLootTargetNetId) {
                gLootTargetInvDirty = true;
            }
        } else if (hasInventory && obj != gDude) {
            // Reconcile EQUIP FLAGS in place — do NOT free/recreate items. An AI critter
            // that wields its gun mid-fight (the case that matters) already carries that
            // weapon in its mirror inventory from the join blob; the wield only moves the
            // in-hand flag from one item to another. Rebuilding by freeing the old item
            // objects double-frees when an in-flight attack replay's reg_anim still
            // references the weapon (it fires, then a same-object delta arrives mid-
            // animation) — the object-lifetime hazard behind the observed crash. So:
            // clear every item's equip flags, then re-assert the wire's equip flags on
            // the matching pid. critterGetWeaponForHitMode scans for the in-hand flag, so
            // this alone fixes the resolved weapon. (Not handled here, acceptable v1: a
            // weapon the mirror is missing entirely, exact ammo, and items the server
            // removed — none affect the attack animation.)
            Inventory* inv = &obj->data.inventory;
            for (int i = 0; i < inv->length; i++) {
                if (inv->items[i].item != nullptr) {
                    inv->items[i].item->flags &= ~(OBJECT_IN_ANY_HAND | OBJECT_WORN);
                }
            }
            for (const WireItem& wi : invItems) {
                if (wi.pid < 0) continue;
                unsigned int equip = wi.flags & (OBJECT_IN_ANY_HAND | OBJECT_WORN);
                // Apply ammo to the matching item even when it is not equipped — a
                // remote critter's carried spare weapon can change ammo too. (This
                // branch stays equip-flags-only for item lifecycle: no free/recreate,
                // just scalar writes on items the mirror already holds.)
                for (int i = 0; i < inv->length; i++) {
                    if (inv->items[i].item != nullptr && inv->items[i].item->pid == wi.pid) {
                        if (equip != 0) inv->items[i].item->flags |= equip;
                        applyWireItemAmmo(inv->items[i].item, wi.ammoQuantity, wi.ammoTypePid);
                        break;
                    }
                }
            }
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                Object* rh = critterGetWeaponForHitMode(obj, HIT_MODE_RIGHT_WEAPON_PRIMARY);
                fprintf(stderr, "[inv-apply] net=%d items=%d rhandPid=%d\n",
                    obj->netId, inv->length, rh != nullptr ? rh->pid : -1);
            }
        } else if (hasInventory && clientViewerActive()) {
            // DUDE live inventory (player-UI Slice 2/3b). The dude's inventory delta IS
            // authoritative (encoder includes gDude, object_delta.cc:130); reconcile the
            // mirror to it: update stack quantities, reconcile equip flags, ADD new items,
            // and REMOVE ones the server dropped/consumed. Initial inventory is already
            // correct from the join blob (_obj_load_dude); ammo/charges now ride the wire
            // per item (v3 — applyWireItemAmmo below), so a fired/reloaded weapon updates
            // live; nested container contents are still not walked (recursive-fingerprint gap).
            //
            // While the inventory SCREEN is open, _setup_inventory (equipmentDetach) has
            // pulled the dude's equipped items OUT of this mirror and parked them in the UI
            // hand/armor statics. Re-attach them first so the reconcile sees the COMPLETE
            // inventory, then re-detach from the now server-correct flags — that renders
            // equip/unequip LIVE in the slots (no reopen needed) and avoids phantom
            // duplicates. equipmentApply/Detach are the same vetted helpers the screen runs
            // at open/close.
            Inventory* inv = &obj->data.inventory;
            // _setup_inventory (equipmentDetach) parks the dude's equipped items in
            // the UI statics for EVERY inventory-family screen — NORMAL, loot,
            // use-on and barter — not just kInventory. Bracket the reconcile for all
            // of them, or the wire's equipped item matches nothing in the mirror and
            // gets recreated as a phantom duplicate (the wielded weapon "appearing"
            // in the loot/use/barter left pane after an action).
            bool invScreenOpen = (GameMode::getCurrentGameMode()
                & (GameMode::kInventory | GameMode::kLoot | GameMode::kUseOn | GameMode::kBarter)) != 0;
            bool anyModalOpen = (GameMode::getCurrentGameMode() & kViewerModalMask) != 0;
            if (invScreenOpen) {
                equipmentApply(obj, gInventoryLeftHandItem, gInventoryRightHandItem, gInventoryArmor);
                // Clear the statics now: the items are back in the mirror, and the removal
                // below could free one the server dropped, which would dangle a static.
                // equipmentDetach re-populates them from the reconciled flags.
                gInventoryLeftHandItem = nullptr;
                gInventoryRightHandItem = nullptr;
                gInventoryArmor = nullptr;
            }
            int origLen = inv->length;
            for (int i = 0; i < origLen; i++) {
                if (inv->items[i].item != nullptr) {
                    inv->items[i].item->flags &= ~(OBJECT_IN_ANY_HAND | OBJECT_WORN);
                }
            }
            // Match each wire stack to an existing mirror stack by netId, falling back to
            // pid (claimed so duplicate pids map 1:1); update quantity + equip flags, or
            // CREATE the item (the vanilla give pattern: objectCreateWithPid +
            // _obj_disconnect + itemAdd).
            //
            // IDENTITY, NOT KIND: pid answers "what kind of thing is this", and matching on
            // it makes every stack of one pid interchangeable — with a spear in each hand
            // plus loose ones, the wire's stacks bind to arbitrary mirror slots and equip
            // flags land on the wrong object. The wire has always carried a per-item netId;
            // it was simply ignored here.
            std::vector<char> claimed(origLen, 0);
            for (const WireItem& wi : invItems) {
                if (wi.pid < 0) continue;
                unsigned int equip = wi.flags & (OBJECT_IN_ANY_HAND | OBJECT_WORN);
                int qty = wi.quantity > 0 ? wi.quantity : 1;
                int m = matchInventorySlot(inv, origLen, claimed, wi.netId, wi.pid);
                if (m >= 0) {
                    claimed[m] = 1;
                    inv->items[m].quantity = qty;
                    inv->items[m].item->flags |= equip;
                    // A netId match can still be a DIFFERENT kind of thing now (an
                    // explosive that was armed) — see applyWireItemPid.
                    applyWireItemPid(inv->items[m].item, wi.pid);
                    applyWireItemAmmo(inv->items[m].item, wi.ammoQuantity, wi.ammoTypePid);
                    // Adopt the authoritative netId: a stack the mirror created locally on
                    // an earlier delta has none until the next rebaseline, which would keep
                    // it unaddressable and force the pid fallback forever.
                    adoptItemNetId(inv->items[m].item, wi.netId);
                } else {
                    Object* item = nullptr;
                    if (objectCreateWithPid(&item, wi.pid) == 0 && item != nullptr) {
                        _obj_disconnect(item, nullptr); // inventory-only, not in the world
                        item->flags |= equip;
                        applyWireItemAmmo(item, wi.ammoQuantity, wi.ammoTypePid);
                        adoptItemNetId(item, wi.netId);
                        itemAdd(obj, item, qty);
                    }
                }
            }
            // SAFE REMOVAL (Slice 3b): items the server dropped/consumed are absent from
            // the wire list — drop them from the mirror so `invdrop` actually empties the
            // slot (the world copy arrives separately as a SPAWN). Collect first, since
            // itemRemove compacts the array; then unlink (exact stack qty) + free. GATED
            // !_inCombat to dodge the reg_anim double-free — an in-flight attack replay may
            // still hold a weapon Object*; a mid-combat consumption self-heals at the next
            // out-of-combat delta / rebaseline. NOTE (object-lifetime): removal can run while
            // a screen is open (the ticker pumps the wire there). itemRemove only UNLINKS
            // (compacts the array; the full-stack qty path never frees), so the item leaves
            // the list immediately; but the objectDestroy could dangle a raw Object* an open
            // inventory handler holds across its inner pump (the drag / ctx-menu locals), so
            // while a modal is open we DEFER the free (flushed after the screen closes) and
            // only unlink now. With no modal open, free immediately.
            //
            // ►►►► UNLINKING AND FREEING ARE DIFFERENT RISKS, AND THIS USED TO SKIP BOTH IN
            // COMBAT. The reg_anim hazard is about FREEING an item an in-flight attack
            // replay still points at; unlinking one from the mirror's item list endangers
            // nothing (the animation holds an Object*, not an inventory slot). Skipping the
            // whole block in combat left every mid-fight removal as a PHANTOM the player can
            // still see and click — throw your only rock, and the mirror goes on listing it.
            // The server then refuses the verb aimed at it ("You don't have that item.")
            // while the screen insists otherwise, which reads as the equip being broken.
            // So: always unlink; defer the FREE while in combat, exactly as for a modal.
            {
                std::vector<Object*> toRemove;
                std::vector<int> toRemoveQty;
                for (int i = 0; i < origLen; i++) {
                    if (!claimed[i] && inv->items[i].item != nullptr) {
                        toRemove.push_back(inv->items[i].item);
                        toRemoveQty.push_back(inv->items[i].quantity);
                    }
                }
                for (size_t k = 0; k < toRemove.size(); k++) {
                    itemRemove(obj, toRemove[k], toRemoveQty[k]);
                    forgetObjectRefs(toRemove[k]); // see the container path above
                    if (anyModalOpen || _inCombat) {
                        gDudeDeferredItemFrees.push_back(toRemove[k]);
                    } else {
                        objectDestroy(toRemove[k], nullptr);
                    }
                }
            }
            // Re-detach the now server-correct equipped items back into the UI statics, so
            // the open screen's hand/armor slots render the live equip state (mirrors the
            // equipmentApply at the top of this branch).
            if (invScreenOpen) {
                equipmentDetach(obj, &gInventoryLeftHandItem, &gInventoryRightHandItem, &gInventoryArmor);
            }
            // A live reconcile touched the mirror (add / remove / qty / equip-flag): flag
            // the open inventory screen to repaint its list (it otherwise repaints only on
            // user events, so an async give/drop/consume would linger visibly).
            gDudeInvDirty = true;
            // Refresh the bar off the updated inventory (equip flags may have moved),
            // preserving the current attack-mode cycle position. Do NOT touch gDude->fid —
            // the character sprite is server-authoritative (a local equip anim is Slice 3,
            // server-driven).
            int leftAction, rightAction;
            interfaceGetItemActions(&leftAction, &rightAction);
            interfaceUpdateItems(false, leftAction, rightAction);
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[inv-apply] DUDE net=%d items=%d wire=%zu\n",
                    obj->netId, inv->length, invItems.size());
            }
        }
        // Dude HP: apply as it streams in, in or out of combat. The old combat DEFERRAL
        // ticked the bar per-blow from the decoder-mirror (playPending / tickDudeHp) — but
        // recorded attacks (the record channel) bypass that path, so a deferred bar looked
        // FROZEN mid-fight (only reconcileDudeHp at the turn boundary caught it up). Under
        // resumable combat each blow lands on its own beat, so the per-blow hp OBJECT_DELTA
        // already ticks the bar down naturally as the recorded hits play. _dudeHpAuth still
        // tracks authority (reconcile stays a backstop for poison/fire with no attack cue).
        bool dudeCombatHp = false;
        if (hasHp && clientViewerActive() && obj == gDude) {
            // The viewer's own HP is a DISPLAY value. Track authority in _dudeHpAuth
            // and let rollDudeHp() (pump) EASE the shown gDude->hp toward it — vanilla
            // rolls the counter; hard-writing it here snapped the number (the "HP
            // hard-set" symptom, PRESENTATION_PACING_DESIGN.md §2, keyframe/tween model).
            // So skip both the shown write and the immediate render below; the roll owns
            // them. Safe because death is read off DAM_DEAD (line ~1896), not hp<=0, so a
            // few frames of display lag never affects a decision.
            //
            // SCOPE (this slice): the roll only SMOOTHS the motion; it still STARTS at
            // decode (~swing start), not the blow's action frame. Action-frame commit is
            // Pillar 1 / phase 3 — deliberately left for the deferred-commit FIFO.
            _dudeHpAuth = hp;
            _dudeHpSeeded = true;
            dudeCombatHp = true;
        }
        if (hasHp && !dudeCombatHp) obj->data.critter.hp = hp;
        if (hasRad) obj->data.critter.radiation = rad;
        if (hasPoison) obj->data.critter.poison = poison;
        // In-combat recorded MOVE: HOLD the mover's authoritative AP until the replayed walk
        // completes (the client's real engine charges AP per step from the pre-walk pool, so
        // it re-walks the identical tiles instead of dying on step 1 with a drained pool).
        // _dudeApAuth still tracks the authoritative value; the per-step charge ticks the HUD.
        bool apDeferred = hasAp && clientViewerActive() && clientCombatAnimDeferAp(obj, ap);
        if (hasAp && !apDeferred) obj->data.critter.combat.ap = ap;
        if (apDeferred && obj == gDude) _dudeApAuth = ap;
        if (hasResults) {
            // A critter that dies MID-WALK must not keep its walk glide: it would
            // outlive the death, re-asserting a sub-tile offset onto the corpse and
            // drawing the body off its own tile (see presEndGlideFor). Fire on the
            // 0->1 edge of DAM_DEAD only, so this runs once per death rather than on
            // every subsequent results delta for an already-dead body.
            bool wasDead = (obj->data.critter.combat.results & DAM_DEAD) != 0;
            bool nowDead = (results & DAM_DEAD) != 0;
            obj->data.critter.combat.results = results;
            if (nowDead && !wasDead && clientViewerActive()) {
                presEndGlideFor(obj);
            }
        }

        // S1 combat HUD: reflect the controlled dude's own hp/ap onto the bar as
        // they stream in — hp on any damage, AP dots while it's our turn (attacks
        // and moves charge AP server-side; the delta arrives the same beat, §3.a).
        // Non-animated renders (the animated variants block on their own loop).
        if (clientViewerActive() && obj == gDude) {
            bool touched = false;
            // TEMP DIAGNOSTIC (inventory-AP-not-repainted): print the AP gate's INPUTS,
            // so a run says which term rejected the repaint instead of leaving a guess.
            if (hasAp && getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[apgate] wireAp=%d prevAp=%d objAp=%d deferred=%d myTurn=%d"
                                " animActive=%d hops=%d shown=%d auth=%d deferring=%d\n",
                    ap, prevAp, obj->data.critter.combat.ap, apDeferred ? 1 : 0, _myTurn ? 1 : 0,
                    clientAnimActiveFor(gDude) ? 1 : 0, clientAnimHopsRemaining(gDude),
                    _dudeApShown, _dudeApAuth, _dudeApDeferring ? 1 : 0);
            }
            if (hasHp && !dudeCombatHp) { interfaceRenderHitPoints(false); touched = true; }
            // Only reflect AP being SPENT (a decrease) during my turn. An AP INCREASE
            // is the round reset-to-max that precedes the next turn; painting it green
            // would flash a full green bar between spending my last point and the
            // paced TURN_START flipping the bar to the next actor's red. Leave the
            // reset to the queued applyTurnStart, which repaints at the right moment.
            if (hasAp && !apDeferred && _myTurn && obj->data.critter.combat.ap <= prevAp) {
                _dudeApAuth = obj->data.critter.combat.ap;
                // Per-hex AP: if this spend belongs to a combat MOVE (an in-combat glide
                // is registered for the dude this same beat), DON'T drop the bar now —
                // hold the shown value and let tickCombatMoveAp tick it down per glide
                // hop. A non-move spend (attack, or a snap-move with no glide) drops
                // immediately, exactly as before.
                if (obj->data.critter.combat.ap < prevAp && clientAnimActiveFor(gDude)) {
                    _dudeApDeferring = true;
                    _dudeApMoveHops = clientAnimHopsRemaining(gDude);
                    interfaceRenderActionPoints(_dudeApShown, 0); // unchanged (pre-move)
                } else {
                    _dudeApShown = obj->data.critter.combat.ap;
                    _dudeApDeferring = false;
                    interfaceRenderActionPoints(_dudeApShown, 0);
                }
                touched = true;
            }
            if (touched) interfaceBarRefresh();
        }
    }

    // ►► APPLY STREAMED GLOBAL VARIABLES. Before this channel existed a viewer's gvars
    // were frozen at its last baseline, so everything the CLIENT renders from a gvar went
    // stale the moment the server changed it — the pipboy's holodisk list and quest list,
    // the character screen's karma/reputation/addictions. Picking up a holodisk mid-session
    // simply never showed up.
    //
    // Applied unconditionally (headless included, no clientViewerActive gate): these are
    // authoritative sim values, not presentation, and the headless probe's own state dump
    // reports gvars — a viewer-only gate would make the reconstruction diverge from the
    // server for exactly the state this event exists to carry.
    // Server-authored holodisks (pipboy.h). CLEAR-then-ADD, so a rebaseline
    // re-announcement replaces the set instead of duplicating it.
    void onHolodiskClear(Reader&)
    {
        if (!clientViewerActive()) return;
        pipboyServerHolodiskClear();
    }

    void onHolodiskAdd(Reader& r)
    {
        std::string name = r.str();
        int lineCount = (int)r.u16();
        // Bound the line count before allocating: it is untrusted wire data, and a disk
        // is meant to be a page or two of text, not a memory-exhaustion vector.
        if (lineCount < 0 || lineCount > kMaxHolodiskLines) {
            debugPrint("client_net: holodisk '%s' line count %d out of range — dropped\n",
                name.c_str(), lineCount);
            return;
        }
        std::vector<std::string> lines;
        std::vector<const char*> linePtrs;
        lines.reserve((size_t)lineCount);
        for (int i = 0; i < lineCount; i++) {
            lines.push_back(r.str());
        }
        if (r.overflow() || !clientViewerActive()) return;
        for (const std::string& line : lines) {
            linePtrs.push_back(line.c_str());
        }
        pipboyServerHolodiskAdd(name.c_str(), linePtrs.data(), (int)linePtrs.size());
    }

    void onGvarDelta(Reader& r)
    {
        int count = (int)r.u16();
        for (int i = 0; i < count && !r.overflow(); i++) {
            int index = r.i32();
            int value = r.i32();
            // The index is UNTRUSTED wire data indexing a heap array — validate every
            // one. A wild index here is an arbitrary write, and the array's length is
            // whatever the loaded world happens to define.
            if (gGameGlobalVars == nullptr || index < 0 || index >= gGameGlobalVarsLength) {
                debugPrint("client_net: gvar delta out of range index=%d (len=%d) dropped\n",
                    index, gGameGlobalVarsLength);
                continue;
            }
            gGameGlobalVars[index] = value;
        }
    }

    void onWorldDelta(Reader& r)
    {
        unsigned int mask = r.u16();
        if (mask & WORLD_DELTA_GAMETIME) {
            gameTimeSetTime(r.u32());
        }
        if (mask & WORLD_DELTA_LIGHT) {
            // Global ambient light (scripted map darkness/brightness). true = repaint now.
            lightSetAmbientIntensity(r.i32(), true);
        }
    }

    void onSnapshotObject(Reader& r)
    {
        // A tripwire over the blob-loaded world (§D): the server's authoritative
        // baseline must line up with what the client independently reconstructed.
        int netId = r.i32();
        int pid = r.i32();
        int tile = r.i32();
        r.i32(); // elevation
        r.i32(); // fid
        r.i32(); // flags
        Object* obj = lookup(netId);
        if (obj != nullptr && obj->pid == pid && obj->tile == tile) {
            _tripwireOk++;
        } else {
            _tripwireBad++;
            if (getenv("F2_TRACE_EVENTS") != nullptr) {
                fprintf(stderr, "[tripbad] net=%d srvPid=0x%X srvTile=%d -> local=%p pid=0x%X tile=%d\n",
                    netId, pid, tile, (void*)obj, obj ? obj->pid : 0, obj ? obj->tile : -1);
            }
        }
    }

    void onSnapshotEnd(Reader&)
    {
        audioNotice();
        // The baseline walk just finished scoring against the blob-loaded world
        // (§D tripwire). ok>0 && bad==0 is the mid-join gate's oracle line.
        debugPrint("client_net: baseline tripwire ok=%d bad=%d (load #%d)\n",
            _tripwireOk, _tripwireBad, _loadCount);
    }

    void onMapTransition(Reader& r)
    {
        r.i32(); // mapIndex
        r.i32(); // elevation

        // ►► RECORD THE MAP WE ARE LEAVING FOR OUR OWN PIPBOY. The automap is
        // viewer-local by design — the server keeps none, and both of vanilla's
        // recording hooks (automapSaveCurrent on a map save, automapSetDisplayMap from
        // the worldmap) are deliberate no-ops on the core-only server. But nothing told a
        // VIEWER to record its own either, so the pipboy's MAPS tab had no entries for
        // any map and reading it did nothing at all.
        //
        // Vanilla records at map EXIT, when the explored state is complete, which is
        // exactly here: the world is about to be replaced. Best-effort and result-
        // ignored — a failed write costs a map in the pipboy list, never the transition.
        if (clientViewerActive() && _loaded) {
            automapSaveCurrent();
        }

        // v1: a fresh blob + baseline always follows (§C.4). Drop the index; the
        // next BLOB_BEGIN rebuilds the world. (Full mid-run transition = S3+.)
        presReset();
        _net.clear();
        _adoptTransients.clear(); // adopt transients die with the old world (Fable review A3)
        _pendingAdopts.clear(); // ...and so do pending adopt mints (§12.6 trap 2)
        _loaded = false;
        setInCombat(false); // a transition ends any local combat framing
        _myTurn = false;
        if (clientViewerActive()) {
            clearCombatMirror();
        }
    }

    // Reset the viewer's mirrored combat framing to the vanilla resting state and
    // hide the end buttons. Called on any world (re)load — map transition or a
    // mid-fight rebaseline (the blob carries no combat state; the next
    // COMBAT_ENTER/TURN_START re-derives it — COMBAT_CLIENT_DESIGN.md §3.0/risk-2).
    void clearCombatMirror()
    {
        // Full reset to the vanilla resting state (buttons hidden, AP unlit). Called on
        // a REAL combat reset — first join or a map transition — never on a mid-fight
        // re-sync (that path keeps the framing; see onBlobEnd).
        _presQueue.clear();
        _pendingDudeTick = 0;
        _dudeApDeferring = false;
        gCombatState &= ~(COMBAT_STATE_0x01 | COMBAT_STATE_0x02);
        gCombatState |= COMBAT_STATE_0x02;
        interfaceBarEndButtonsHide(false);
        interfaceRenderActionPoints(0, 0);
        interfaceBarRefresh();
    }

    // -- Combat framing (P3, presentation-only) -----------------------------
    // These mirror the server's combat lifecycle. Two layers, both fed here:
    //   (1) _inCombat / _myTurn: the decoder's own routing bools (out-of-combat
    //       mv vs in-combat cmove; gate 10). Maintained ALWAYS, headless or not.
    //   (2) The gCombatState MIRROR + interface-bar hooks (COMBAT_CLIENT_DESIGN.md
    //       §3.0/§3.a): the decoder writes the REAL engine global so the vanilla
    //       combat UI compiled into the viewer lights up, and drives the bar
    //       directly (the wire carries no hud* chrome events, MP_PROTOCOL §7d).
    //       VIEWER-ONLY (clientViewerActive): headless never touches windows, and
    //       state_dump does not include gCombatState, so the mirror is
    //       golden-invisible by construction. Nothing here advances combat — the
    //       globals change ONLY under decode, so the mirror cannot drift.

    void onCombatEnter(Reader& r)
    {
        r.i32(); // initiator netId (may be 0 for a scripted start) — unused v1
        struct CombatPosition {
            int netId;
            int tile;
            int elevation;
            int rotation;
        };
        std::vector<CombatPosition> positions;
        if (r.remaining() >= 2) {
            int count = r.u16();
            positions.reserve(count);
            for (int i = 0; i < count && r.remaining() >= 16; i++) {
                positions.push_back({ r.i32(), r.i32(), r.i32(), r.i32() });
            }
        }
        if (clientViewerActive() && _inCombat) {
            // Already in combat: this is the server re-emitting combatEnter after a
            // forced mid-fight rebaseline (another client joined). We kept our framing
            // across the reload (onBlobEnd), so IGNORE the duplicate — no replayed
            // door-slide + "iciboxx1" fanfare. A fresh client had _inCombat=false and
            // falls through to the real enter below.
            return;
        }
        setInCombat(true);
        _myTurn = false;
        if (clientViewerActive()) {
            // Snap any in-flight FREE-ROAM glide to its authoritative tile before the
            // fight opens. The presentation pump blocks a TURN_START while ANY walk is
            // playable-gliding (it assumes the only combat glides are the presented
            // turn's) — but an out-of-combat `mv` glide still playing when combat opens
            // (moved toward a target, then entered combat) would wedge the FIRST
            // TURN_START, so `_myTurn` never flips and the wait cursor sticks forever.
            // The authoritative tile is already applied, so this is a clean snap.
            // Stand any mid-run critter DOWN (retract to stand fid + frame 0), not a
            // bare clear: a wholesale drop would leave a critter caught mid-glide
            // frozen wearing its running fid (run -> combat-enter must show STAND).
            presStandDownAll();
            // The stand-down lands the last locally decoded glide endpoint. The combat
            // fence then wins with the server's exact tile/elevation for every critter,
            // closing free-roam→combat range/position disagreement even when the target
            // was not a combatant when A was pressed.
            for (const CombatPosition& pos : positions) {
                Object* obj = lookup(pos.netId);
                if (obj == nullptr) {
                    continue;
                }
                objectSetLocation(obj, pos.tile, pos.elevation, nullptr);
                objectSetRotation(obj, pos.rotation, nullptr);
                if (obj == gDude && pos.elevation != gElevation) {
                    mapSetElevation(pos.elevation);
                    tileSetCenter(pos.tile, TILE_SET_CENTER_REFRESH_WINDOW);
                }
            }
            // 0x01 = in combat; 0x02 (free to act) stays clear until our TURN_START.
            gCombatState |= COMBAT_STATE_0x01;
            gCombatState &= ~COMBAT_STATE_0x02;
            // Cursor is owned entirely by the frame loop's combat-busy latch — do NOT
            // set the watch cursor here. A player-initiated fight (cstart) opens on the
            // DUDE's own turn, so combatBusy is false from the first frame; a watch set
            // here would be orphaned (the loop only clears a watch it set itself) and
            // stick forever. The loop shows the watch iff it is genuinely someone else's
            // turn / presentation is busy, which covers the AI-first case correctly.
            // Animated = the vanilla door-slide reveal (its blocking render loop is
            // render-only, so it is safe to run from inside a decode pump — the
            // same loop vanilla runs from its game loop). Show() also wires the
            // keycode-32/13 end buttons (S3 input) and renders red lights.
            interfaceBarEndButtonsShow(true);
            interfaceRenderActionPoints(0, 0); // unlit until TURN_START says whose turn
            interfaceBarRefresh();
            // No actor yet — the first TURN_START drives the outlines (#8).
            _combatActorNetId = 0;
            recomputeCombatOutlines();
        }
        debugPrint("client_net: COMBAT ENTER\n");
    }

    void onCombatExit(Reader&)
    {
        // Combat truth is STATE, not presentation. Stop routing combat verbs now;
        // only the animated button-door close remains queued behind death/attack
        // playback. This also makes a duplicate COMBAT_EXIT an idempotent repair.
        bool wasInCombat = _inCombat;
        setInCombat(false);
        _myTurn = false;
        if (clientViewerActive()) {
            gCombatState &= ~(COMBAT_STATE_0x01 | COMBAT_STATE_0x02);
            gCombatState |= COMBAT_STATE_0x02;
        }
        if (clientViewerActive()) {
            if (!wasInCombat) {
                return;
            }
            // Queue the end-of-combat chrome behind everything still pending, so a
            // killing blow's death animation (and any trailing attacks) play out
            // BEFORE combat visibly ends (§3.c ordering). The routing bools stay
            // in-combat until the queue reaches this, which keeps input locked and
            // the wait cursor up through the death animation.
            PresEvent e;
            e.kind = PresKind::kExit;
            enqueue(e);
            debugPrint("client_net: COMBAT EXIT (queued behind replay)\n");
            return;
        }
        debugPrint("client_net: COMBAT EXIT\n");
    }

    // The end-of-combat chrome, run by presentationPump once the queue reaches it.
    // Viewer-only (headless applies exit inline in onCombatExit).
    void applyCombatExit()
    {
        reconcileDudeHp(); // the fight's last blows have played — pin exact HP
        setInCombat(false);
        _myTurn = false;
        // Vanilla resting state (mirror of _combat_over): 0x01 clear, 0x02 set.
        gCombatState &= ~(COMBAT_STATE_0x01 | COMBAT_STATE_0x02);
        gCombatState |= COMBAT_STATE_0x02;
        // Doors slide shut over the buttons (vanilla), THEN the AP bar goes unlit —
        // not red (red is the in-combat not-your-turn state, §3.a).
        interfaceBarEndButtonsHide(true);
        interfaceRenderActionPoints(0, 0);
        interfaceBarRefresh();
        // Combat is visibly over (queued behind the last death anim) — drop all
        // outlines (recompute clears since _inCombat is now false). Rebaseline/rejoin
        // self-clear (fresh object list), so this is the only path needing a clear (#8).
        _combatActorNetId = 0;
        recomputeCombatOutlines();
        debugPrint("client_net: COMBAT EXIT applied\n");
    }

    void onTurnStart(Reader& r)
    {
        int netId = r.i32();
        int isPlayer = r.u8();
        int ap = r.i32();
        int deadline = r.i32(); // deadlineMs — a turn-timer HUD cue, unused by v1 routing
        int freeMove = r.i32(); // bonus-move budget (§3.a); appended field, see producer
        // TURN_START is a complete authoritative checkpoint, not a presentation cue.
        // It must be sufficient to repair a missed/late COMBAT_ENTER and must never sit
        // behind a stuck glide/replay: routing, turn ownership and AP are needed to send
        // the very input that lets the server progress the turn.
        bool wasInCombat = _inCombat;
        setInCombat(true);
        if (!clientViewerActive()) {
            // Headless routing: apply _myTurn inline, byte-identical to before.
            _myTurn = isPlayer != 0 && gDude != nullptr && netId == gDude->netId;
            return;
        }
        if (!wasInCombat) {
            // A mid-fight join/recovery may have missed COMBAT_ENTER entirely. Clear
            // free-roam motion before establishing combat chrome, exactly as enter does.
            presStandDownAll();
        }
        applyTurnStart(netId, isPlayer, ap, deadline, freeMove);
    }

    // Apply TURN_START: flip _myTurn and paint the AP dots / lights. Run by
    // presentationPump in lockstep with the animations, so "my turn" green appears
    // only after the previous actor's attacks have visibly played out.
    void applyTurnStart(int netId, int isPlayer, int ap, int deadline, int freeMove)
    {
        (void)deadline;
        // NOTE: deliberately NO reconcileDudeHp() here. The damaging turn's net hp
        // delta arrives in the SAME batch as its TURN_START/attacks, so _dudeHpAuth is
        // already the post-turn value by the time this fires — reconciling would snap
        // the bar to the final HP before the attacks animate (the 46→20 jump). The
        // per-hit ticks (clamped at _dudeHpAuth) drive the bar down across the turn on
        // their own; combat exit does the only safety reconcile.
        // "My" turn = a player turn for the actor this viewer controls. gDude is
        // netId 1 always (server assigns walk numbers dude-first, [[p5-server-plan]]).
        // Keyed on netId, never isPlayer alone (another player's turn is isPlayer
        // too — [[mp-actor-architecture-principle]] UI-driving corollary).
        _myTurn = isPlayer != 0 && gDude != nullptr && netId == gDude->netId;
        gCombatState |= COMBAT_STATE_0x01;
        interfaceBarEndButtonsShow(true); // idempotent: animates only the first reveal
        if (_myTurn) {
            gCombatState |= COMBAT_STATE_0x02; // free to act
            // Mirror the bonus-move budget into the real engine global (§3.0) so the
            // AP dots draw the green bonus-move dots and game_mouse's in-combat path
            // preview reads the right move budget. Derived state only — the viewer
            // never advances combat, so this can only change under decode.
            _combat_free_move = freeMove;
            // TURN_START's AP is the authority. OBJECT_DELTA normally converges the
            // object first, but a lost/deferred delta must not leave the input checks or
            // bar reading stale AP until the player spends one point and triggers redraw.
            gDude->data.critter.combat.ap = ap;
            // A fresh turn resets the AP baseline; any half-ticked move deferral from
            // the previous turn is void (per-hex AP).
            _dudeApShown = ap;
            _dudeApAuth = ap;
            _dudeApDeferring = false;
            interfaceRenderActionPoints(ap, freeMove);
            interfaceBarEndButtonsRenderGreenLights();
        } else {
            gCombatState &= ~COMBAT_STATE_0x02; // another actor's turn
            interfaceRenderActionPoints(-1, -1); // all dots red — not your turn
            interfaceBarEndButtonsRenderRedLights();
        }
        interfaceBarRefresh();
        // New turn: record the actor and recompute outlines (your-turn crosshair
        // highlight, or the single acting-critter outline) (#8).
        _combatActorNetId = netId;
        recomputeCombatOutlines();
        if (_myTurn) {
            debugPrint("client_net: YOUR TURN ap=%d\n", ap);
        }
    }

    // -- Attack replay (S4, COMBAT_CLIENT_DESIGN.md §3.c) --------------------
    // The causal envelope of one attack. Reconstruct a vanilla Attack from it and
    // replay the real choreography (client_combat_anim → _action_attack). Damage/
    // death STATE rides OBJECT_DELTA as always; this only draws the motion, and is
    // double-apply-safe because the viewer never arms _combat_cleanup_enabled.
    // VIEWER-ONLY: the reconstruction resolves netId→Object* and reads the mirrored
    // inventory, both meaningful only on a rendering client; headless skips it after
    // the bounded read below (which must still run so the frame walker stays synced).

    void onAttackResult(Reader& r)
    {
        PendingAttack pa;
        pa.attackerNetId = r.i32();
        pa.defenderNetId = r.i32();
        pa.hitMode = r.i32();
        pa.defenderHitLocation = r.i32();
        pa.defenderDamage = r.i32();
        pa.defenderFlags = r.i32();
        pa.attackerDamage = r.i32();
        pa.attackerFlags = r.i32();
        unsigned short extrasLength = r.u16();

        // Read the extras[] set unconditionally so the reader is fully advanced even
        // when we won't replay (headless, overflow).
        pa.extraCount = 0;
        for (int i = 0; i < extrasLength; i++) {
            int nid = r.i32();
            int dmg = r.i32();
            int flg = r.i32();
            if (pa.extraCount < EXPLOSION_TARGET_COUNT) {
                pa.extraNetId[pa.extraCount] = nid;
                pa.extraDamage[pa.extraCount] = dmg;
                pa.extraFlags[pa.extraCount] = flg;
                pa.extraCount++;
            }
        }
        // Fired weapon (bug D) — read unconditionally so the reader advances even
        // headless/overflow, exactly like the extras block above.
        pa.weaponNetId = r.i32();
        pa.weaponPid = r.i32();

        if (!clientViewerActive() || r.overflow()) {
            return; // headless never replays; state rides OBJECT_DELTA
        }

        // Reserve the deferral for every participant NOW, at decode — the corpse fid
        // and the attacker's face-target rotation ride this SAME beat's OBJECT_DELTA
        // (right after this event), so the hold must begin here, not when the replay
        // finally plays (pump time), or they'd apply first and the critter would flash
        // dead / pre-rotate before its animation (§3.c same-beat leak). Held state
        // lands when the replay finishes (or is released if the replay is skipped).
        clientCombatAnimReserve(lookup(pa.attackerNetId));
        clientCombatAnimReserve(lookup(pa.defenderNetId));
        for (int i = 0; i < pa.extraCount; i++) {
            clientCombatAnimReserve(lookup(pa.extraNetId[i]));
        }

        // Queue rather than play immediately: a fight resolves several attacks in a
        // beat, so they arrive same-frame. presentationPump starts them ONE AT A TIME
        // (each after the prior replay is idle) so each hit shows its own animation
        // instead of collapsing into one (§3.c serialization). Reconstruction is
        // deferred to dequeue so a participant destroyed before its turn is skipped.
        PresEvent e;
        e.kind = PresKind::kAttack;
        e.attack = pa;
        enqueue(e);
    }





    // ---- PRESENTATION RECORD/REPLAY POC (PRESENTATION_RECORD_REPLAY_SPEC.md) ----
    // The generic replacement for per-action replays like playExplosion above: the
    // server recorded an action's animate branch as a flat op stream (EVENT_PRES_SEQ);
    // here we replay it through the viewer's OWN real reg_anim engine — vanilla-
    // faithful by construction, no action-specific code.

    // ref encoding: >0 = live netId (decoder map, resolved at PLAY time so a freed
    // participant drops its ops); <0 = a stream-scoped transient handle minted by
    // OBJ_CREATE this replay; 0 = null.
    // Per-sequence tallies for the [preplay] line below: how much of a recorded
    // sequence actually found something to act on. Every op is `if (o) register...`,
    // so an unresolvable ref is silently skipped — which is precisely how a throw can
    // animate the thrower while nothing flies.
    int _seqRefsOk = 0;
    int _seqRefsDropped = 0;
    int _seqTransients = 0;
    // Adopt netIds the DRY pass just promised (§12.2). Read by onPresSeq straight after
    // the pass and carried on the queued entry, so a dropped sequence can release them.
    std::vector<int> _seqAdoptIds;

    Object* resolveSeqRef(int ref, std::unordered_map<int, Object*>& handles)
    {
        if (ref > 0) {
            Object* o = lookup(ref);
            (o != nullptr ? _seqRefsOk : _seqRefsDropped)++;
            return o;
        }
        if (ref < 0) {
            auto it = handles.find(ref);
            Object* o = it != handles.end() ? it->second : nullptr;
            (o != nullptr ? _seqRefsOk : _seqRefsDropped)++;
            return o;
        }
        return nullptr;
    }

    // Reserve a live participant at DECODE so its death/fid deltas (which land later
    // this same beat) are held until the recorded anim plays — the same same-beat-leak
    // guard onExplosionFx/onAttackResult use. lookup() may be null (freed/unknown);
    // clientCombatAnimReserve tolerates it.
    void reserveSeqRef(int ref)
    {
        if (ref > 0) clientCombatAnimReserve(lookup(ref));
    }

    // Walk the op stream. execute=false = DRY reserve pass at decode (reserve live
    // participants only). execute=true = register the sequence on the real engine
    // (ticked by presAdvance -> _object_animate, exactly like the other replays).
    void presPlayRecordedSeq(const unsigned char* data, int size, bool execute)
    {
        if (data == nullptr || size <= 0) {
            return;
        }
        Reader r(data, (size_t)size);
        unsigned char version = r.u8();
        unsigned short opCount = r.u16();
        if (version != kPresStreamVersion) {
            return; // unknown stream version -> drop (presentation is skippable)
        }
        _seqRefsOk = 0;
        _seqRefsDropped = 0;
        _seqTransients = 0;
        _seqAdoptIds.clear();
        std::unordered_map<int, Object*> handles; // stream handle -> local transient
        for (int i = 0; i < opCount && !r.overflow(); i++) {
            unsigned char op = r.u8();
            switch (op) {
            case PRES_OP_SEQ_BEGIN: {
                int flags = r.i32();
                if (execute) reg_anim_begin(flags);
                break;
            }
            case PRES_OP_SEQ_END:
                if (execute) reg_anim_end();
                break;
            case PRES_OP_PRIORITY: {
                int n = r.i32();
                if (execute) _register_priority(n);
                break;
            }
            case PRES_OP_OBJ_CREATE: {
                int handle = r.i32();
                int fid = r.i32();
                int tile = r.i32();
                int elev = r.i32();
                int flags = r.i32();
                int rotation = r.i32(); // stream v2: the transient's facing (else 0 = "up")
                int adoptNetId = r.i32(); // stream v4: real netId to ALSO map onto this transient
                (void)flags; // transients are always born hidden+NO_SAVE below
                auto spawnTransient = [&]() -> Object* {
                    Object* obj = nullptr;
                    if (objectCreateWithFidPid(&obj, fid, -1) == 0 && obj != nullptr) {
                        _seqTransients++;
                        objectHide(obj, nullptr); // born hidden — the ANIMATE/MOVE ops reveal it
                        obj->flags |= OBJECT_NO_SAVE;
                        objectSetLocation(obj, tile, elev, nullptr);
                        objectSetRotation(obj, rotation, nullptr);
                    }
                    return obj;
                };
                if (adoptNetId > 0) {
                    // ►► A thrown weapon's flight transient IS its ground item: it adopts the
                    // real netId, so it is the one object whose LIFETIME both the state lane
                    // and the record lane write. It therefore mints on ONE clock — EXECUTE,
                    // the same clock that flies it (§12.2).
                    //
                    // This deliberately REVERSES the earlier decode-minting fix. That fix was
                    // right about the disease (the pickup DISCONNECT arriving while the
                    // sequence still sat on the pump, resolving to nothing and leaving a
                    // phantom spear) and wrong about the cure: moving creation to the fast
                    // clock just put destruction on the wrong side of it, so the spear
                    // vanished mid-flight instead. The seesaw existed only because creation
                    // and destruction sat on DIFFERENT clocks. Now the DISCONNECT rides this
                    // same FIFO behind the sequence (see deferStateEvent), so it cannot
                    // outrun a creation that no longer needs to run early.
                    if (!execute) {
                        // DRY: mint nothing, just promise it. From this instant the netId is
                        // presentation-entangled, so every state event for it parks — which
                        // is what makes "the DISCONNECT arrives before the sequence plays"
                        // an ordering fact rather than a lifetime bug.
                        _pendingAdopts[adoptNetId]++;
                        _seqAdoptIds.push_back(adoptNetId);
                    } else {
                        releasePendingAdopt(adoptNetId); // promise kept (or the mint failed)
                        auto it = _adoptTransients.find(adoptNetId);
                        // Snapshot the answer: inserting below can REHASH the map, which
                        // invalidates `it` (comparing a stale iterator against a fresh end()
                        // is UB, even in a trace).
                        bool reused = it != _adoptTransients.end();
                        if (reused) {
                            // Already live under this netId. Reuse it rather than minting a
                            // second: ONE object per netId is a teardown invariant (a rebind
                            // strands the old one in the world list and _net erases by
                            // pointer). Reachable when a sequence replays the same adopt
                            // twice; a throw→pickup→re-throw burst does NOT reach it, because
                            // the pickup DISCONNECT is parked BETWEEN the two sequences and so
                            // destroys the first transient before the second executes.
                            handles[handle] = it->second;
                        } else {
                            Object* obj = spawnTransient();
                            if (obj != nullptr) {
                                obj->netId = adoptNetId;
                                _net[adoptNetId] = obj;
                                _adoptTransients[adoptNetId] = obj;
                                handles[handle] = obj;
                            }
                            // Mint failure leaves the handle unset, so the flight ops
                            // harmlessly no-op instead of dangling. State still converges:
                            // the parked CONNECT finds no transient and self-heals the real
                            // ground item.
                        }
                        // Do NOT erase the bridge here: onConnect/onDisconnect/onDestroy
                        // consult _adoptTransients to recognize this netId as a viewer-local
                        // adopt transient and OWN its lifecycle against the state lane (skip
                        // the redundant CONNECT that would double-node it; objectDestroy it on
                        // DISCONNECT instead of leaking it). The entry is erased there, when
                        // the object actually leaves.
                        if (getenv("F2_TRACE_EVENTS") != nullptr) {
                            Object* t = handles.count(handle) != 0 ? handles[handle] : nullptr;
                            fprintf(stderr, "[adopt] MINT net=%d fid=0x%X tile=%d reused=%d obj=%s\n",
                                adoptNetId, fid, tile, reused ? 1 : 0, t != nullptr ? "ok" : "FAILED");
                        }
                    }
                } else if (execute) {
                    // Plain transient (explosion cloud, hitscan projectile) — no netId to
                    // adopt, nothing off-lane resolves to it, so create only at play time.
                    Object* obj = spawnTransient();
                    if (obj != nullptr) handles[handle] = obj;
                }
                break;
            }
            case PRES_OP_ANIMATE:
            case PRES_OP_ANIMATE_REV:
            case PRES_OP_ANIMATE_FOREVER:
            case PRES_OP_ANIMATE_AND_HIDE: {
                int ref = r.i32();
                int anim = r.i32();
                int delay = r.i32();
                if (execute) {
                    Object* o = resolveSeqRef(ref, handles);
                    if (o != nullptr) {
                        if (op == PRES_OP_ANIMATE) animationRegisterAnimate(o, anim, delay);
                        else if (op == PRES_OP_ANIMATE_REV) animationRegisterAnimateReversed(o, anim, delay);
                        else if (op == PRES_OP_ANIMATE_FOREVER) animationRegisterAnimateForever(o, anim, delay);
                        else animationRegisterAnimateAndHide(o, anim, delay);
                    }
                } else {
                    reserveSeqRef(ref);
                }
                break;
            }
            case PRES_OP_HIDE_FORCED: {
                int ref = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterHideObjectForced(o); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_SET_FID: {
                int ref = r.i32(); int fid = r.i32(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterSetFid(o, fid, delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_ROTATE: {
                int ref = r.i32(); int tile = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterRotateToTile(o, tile); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_UNSET_FLAG: {
                int ref = r.i32(); int flag = r.i32(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterUnsetFlag(o, flag, delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_MOVE_STRAIGHT: {
                int ref = r.i32(); int tile = r.i32(); int elev = r.i32(); int anim = r.i32(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterMoveToTileStraight(o, tile, elev, anim, delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_MOVE_STRAIGHT_WAIT: {
                int ref = r.i32(); int tile = r.i32(); int elev = r.i32(); int anim = r.i32(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterMoveToTileStraightAndWaitForComplete(o, tile, elev, anim, delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_MOVE_TO_TILE: {
                // In-combat pathed walk, replayed AS-IS through the REAL registrar (the exact
                // RunTo/MoveTo leaf vanilla combat uses — A* walk/run frames + rotation). The
                // sprite is still at its ORIGIN tile here (the authoritative MOVE deltas are
                // HELD by the mover's moveHold arm), so the walk plays from where the server
                // started. The RECORDED actionPoints (not -1) + the DEFERRED AP pool make the
                // client re-walk the identical tiles/endpoint. COMBAT_MOVE_RECORD_DESIGN.md.
                int ref = r.i32(); int tile = r.i32(); int elev = r.i32(); int anim = r.i32();
                int ap = r.i32(); int preWalkAp = r.i32(); int delay = r.i32();
                if (execute) {
                    Object* o = resolveSeqRef(ref, handles);
                    if (o) {
                        // Fuel the replayed walk with the server's PRE-walk AP: the real engine
                        // charges AP per step (animation.cc:2117) and the mover's LOCAL AP is
                        // stale/drained, so without this the walk dies after ~1 tile and snaps.
                        if (preWalkAp >= 0 && FID_TYPE(o->fid) == OBJ_TYPE_CRITTER) {
                            o->data.critter.combat.ap = preWalkAp;
                        }
                        // Walk to the tile the server's walk ACTUALLY reached (the held
                        // authoritative end), not the recorded INTENT dest. The intent can be
                        // the target's own OCCUPIED tile (an approach to throw/attack); a
                        // RunToTile there pathfinds to nothing → the walk never moves → the
                        // completion reap snap-teleports it. The held end tile is always a
                        // free, reachable tile (the server stood there). Falls back to the
                        // recorded dest when no position is held.
                        int heldTile = clientCombatAnimHeldMoveTile(o);
                        int walkTile = heldTile >= 0 ? heldTile : tile;
                        // ►► KEEP THE REGISTRATION RESULT. It used to be discarded, which made
                        // the most common way a recorded walk fails INVISIBLE: every register
                        // below can refuse (no free sequence slot, the actor already owns a live
                        // description, zero AP, or — the one that bites — `_anim_preload`'s
                        // artLock returning null because the critter HAS NO ART for this anim),
                        // and on refusal `_anim_cleanup` throws the whole stream away. We then
                        // called clientCombatAnimMarkActive unconditionally, so the entry went
                        // Active with nothing playing, animationIsBusy said 0 on the very next
                        // advanceReplays, and the reap snapped the sprite to the held tile. The
                        // only symptom was `[cmove-drift]` with walkedTo == curTile — a teleport
                        // that looked identical to a missing hold frame, which is a completely
                        // different bug. rc=-1 here separates the two in one live run.
                        // heldTile is logged for the same reason: heldTile=-1 means walkTile is
                        // the recorded INTENT dest, so a dist far above ap is the other cause.
                        int walkRc = (anim == ANIM_RUNNING)
                            ? animationRegisterRunToTile(o, walkTile, elev, ap, delay)
                            : animationRegisterMoveToTile(o, walkTile, elev, ap, delay);
                        if (getenv("F2_TRACE_EVENTS") != nullptr) {
                            fprintf(stderr, "[cmove-play] net=%d curTile=%d destTile=%d walkTile=%d dist=%d anim=%d ap=%d preAp=%d heldTile=%d rc=%d\n",
                                o->netId, o->tile, tile, walkTile, tileDistanceBetween(o->tile, walkTile), anim, ap, preWalkAp, heldTile, walkRc);
                        }
                        clientCombatAnimMarkActive(o, kMoveReplayCapMs, /*ownsMoveFrame=*/true);
                    }
                } else {
                    clientCombatAnimArmMoveHold(lookup(ref)); // reserve + flag: hold this mover's pos/AP deltas
                }
                break;
            }
            case PRES_OP_MOVE_TO_OBJ: {
                // Pathed walk toward an OBJECT (multihex stop-short + trailing RotateToTile
                // handled by the real registrar). Same hold discipline as MOVE_TO_TILE.
                int ref = r.i32(); int targetRef = r.i32(); int anim = r.i32();
                int ap = r.i32(); int preWalkAp = r.i32(); int delay = r.i32();
                if (execute) {
                    Object* o = resolveSeqRef(ref, handles);
                    Object* target = resolveSeqRef(targetRef, handles);
                    if (o && preWalkAp >= 0 && FID_TYPE(o->fid) == OBJ_TYPE_CRITTER) {
                        o->data.critter.combat.ap = preWalkAp;
                    }
                    // Prefer the held authoritative END tile (where the server's walk stopped
                    // adjacent to the target) over RunToObject: it walks to a known-good tile
                    // even when the target object was already removed on the state lane (a
                    // thrown weapon picked up = disconnected before this gesture drains). Only
                    // RunToObject when there is no held position (fall back to the recorded
                    // target's multihex approach).
                    int heldTile = o ? clientCombatAnimHeldMoveTile(o) : -1;
                    if (o && heldTile >= 0) {
                        if (anim == ANIM_RUNNING) animationRegisterRunToTile(o, heldTile, o->elevation, ap, delay);
                        else animationRegisterMoveToTile(o, heldTile, o->elevation, ap, delay);
                        clientCombatAnimMarkActive(o, kMoveReplayCapMs, /*ownsMoveFrame=*/true);
                    } else if (o && target) {
                        if (anim == ANIM_RUNNING) animationRegisterRunToObject(o, target, ap, delay);
                        else animationRegisterMoveToObject(o, target, ap, delay);
                        clientCombatAnimMarkActive(o, kMoveReplayCapMs, /*ownsMoveFrame=*/true);
                    } else if (o) {
                        // No held position AND no target — nothing to walk to; the reap snaps
                        // (fail-to-snap, never freeze). Still mark Active so the hold resolves
                        // promptly rather than stalling.
                        clientCombatAnimMarkActive(o, kMoveReplayCapMs, /*ownsMoveFrame=*/true);
                    }
                } else {
                    clientCombatAnimArmMoveHold(lookup(ref)); // hold the MOVER only (not the target)
                }
                break;
            }
            case PRES_OP_SFX: {
                int ref = r.i32(); std::string name = r.str(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterPlaySoundEffect(o, name.c_str(), delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_SET_LIGHT: {
                int ref = r.i32(); int dist = r.i32(); int inten = r.i32(); int delay = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o) animationRegisterSetLightIntensity(o, dist, inten, delay); }
                else reserveSeqRef(ref);
                break;
            }
            case PRES_OP_TAKE_OUT: {
                int ref = r.i32(); int code = r.i32(); int delay = r.i32();
                if (execute) {
                    Object* o = resolveSeqRef(ref, handles);
                    if (getenv("F2_TRACE_EVENTS") != nullptr) {
                        fprintf(stderr, "[ctakeout] net=%d code=%d fid=0x%x resolved=%d\n",
                            o ? o->netId : -1, code, o ? o->fid : 0, o != nullptr ? 1 : 0);
                    }
                    if (o) {
                        animationRegisterTakeOutWeapon(o, code, delay);
                        // Mark the drawing critter Active so (1) the pump HOLDS the following
                        // attack until the draw finishes (else the attack's reg_anim cancels
                        // the take-out mid-draw → the wield is never seen), and (2) the held
                        // armed-fid OBJECT_DELTA resolves when the draw completes (not on the
                        // 5 s reserve-stall backstop). The generic recorded-seq path doesn't
                        // promote to Active on its own. 0 = generic replay cap (a draw is <1 s).
                        clientCombatAnimMarkActive(o, 0);
                    }
                } else {
                    reserveSeqRef(ref);
                }
                break;
            }
            case PRES_OP_PING: {
                int flags = r.i32(); int delay = r.i32();
                if (execute) animationRegisterPing(flags, delay);
                break;
            }
            case PRES_OP_CALL: {
                unsigned char tag = r.u8(); int ref = r.i32(); int arg = r.i32();
                if (execute) { Object* o = resolveSeqRef(ref, handles); if (o && tag == PRES_CB_SHOW_DEATH) actionPresReplayShowDeath(o, arg); }
                else reserveSeqRef(ref);
                break;
            }
            default:
                // Unknown op: its length is unknown, so parsing cannot continue. Drop
                // the rest (presentation is skippable — newer server, older viewer).
                return;
            }
        }
    
        // ►► ONE LINE THAT ANSWERS "why did nothing happen?". Every recorded op is
        // `if (o) animationRegister...`, so an unresolvable ref is skipped in silence:
        // a throw whose flight transient does not resolve animates the THROWER and
        // nothing else — no projectile, exactly the owner's symptom. refsDropped>0 on a
        // throw means the viewer could not find what the sequence wanted to move.
        if (execute && getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[preplay] ops=%d refsOk=%d refsDROPPED=%d transientsMinted=%d\n",
                (int)opCount, _seqRefsOk, _seqRefsDropped, _seqTransients);
        }
}

    // Decode EVENT_PRES_SEQ: buffer the ops, reserve live participants NOW (decode
    // time), then play immediately (out of combat) or queue in wire order (in combat,
    // exactly like onExplosionFx).
    void onPresSeq(Reader& r)
    {
        int actorNetId = r.i32(); // primary actor to wait for (precedes the op blob)
        size_t n = r.remaining();
        std::vector<unsigned char> ops(r.here(), r.here() + n);
        r.skip(n);
        if (!clientViewerActive() || ops.empty()) {
            return; // headless never replays; STATE rides OBJECT_DELTA
        }
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr, "[presseq] RECV bytes=%d actor=%d inCombat=%d\n", (int)ops.size(), actorNetId, _inCombat ? 1 : 0);
        }
        // DRY pass: reserve every live participant before this beat's death-fid deltas land
        // (§12.6 trap 6 — reserve/move-hold arming STAYS at decode; only OBJ_CREATE minting
        // moved to execute, or the same-beat corpse-fid leak regresses for every attack).
        // It also PROMISES this sequence's adopt mints, which entangles those netIds from
        // here on — the parking rule for everything that follows on the state lane.
        presPlayRecordedSeq(ops.data(), (int)ops.size(), false);
        std::vector<int> promisedAdopts = _seqAdoptIds;
        // In combat: always ride the pump (turn-serial ordering). Out of combat: an
        // ACTORED sequence (gesture/door) rides the pump too so it waits out that
        // actor's approach glide (pump gate 1d); an actor-less sequence (explosion)
        // has nothing to approach and plays immediately, exactly as before.
        if (_inCombat || actorNetId != 0) {
            PresEvent e;
            e.kind = PresKind::kRecordedSeq;
            e.seqOps = std::move(ops);
            e.seqActorNetId = actorNetId;
            e.seqAdopts = std::move(promisedAdopts);
            enqueue(e);
        } else {
            // Plays now, so the promise is kept immediately (the execute pass releases it).
            presPlayRecordedSeq(ops.data(), (int)ops.size(), true);
        }
    }



    // Is any participant of this queued attack still PLAYABLE-gliding into position
    // (§3.d)? Held (not-yet-released) hops don't count — an attack must not deadlock
    // waiting on a future segment it precedes in the queue. Resolved fresh each poll;
    // a participant already freed simply isn't gliding.
    bool attackParticipantsGliding(const PendingAttack& pa)
    {
        if (clientAnimPlayableActiveFor(lookup(pa.attackerNetId))
            || clientAnimPlayableActiveFor(lookup(pa.defenderNetId))) {
            return true;
        }
        for (int i = 0; i < pa.extraCount; i++) {
            if (clientAnimPlayableActiveFor(lookup(pa.extraNetId[i]))) {
                return true;
            }
        }
        return false;
    }

    // Reconstruct one queued attack and start its replay. netIds are resolved HERE
    // (dequeue time), so a participant freed since the event is simply skipped.
    void playPending(const PendingAttack& pa)
    {
        Object* attacker = lookup(pa.attackerNetId);
        Object* defender = lookup(pa.defenderNetId);
        if (attacker == nullptr || defender == nullptr) {
            return;
        }

        // Rebuild the Attack: wire fills the causal fields; DEFAULT the rest via a
        // zero-fill, and CRUCIALLY zero the knockbacks — the server applied knockback
        // authoritatively and it rides MOVE, so replaying the knockdown would fight
        // it (§3.c; the 3 actionKnockdown sites self-gate on != 0). weapon comes from
        // the mirrored inventory by hit mode; tile defaults to the defender for the
        // miss-projectile path.
        Attack attack;
        memset(&attack, 0, sizeof(attack));
        attack.attacker = attacker;
        attack.hitMode = pa.hitMode;
        // Weapon comes from the mirrored inventory by hit mode. This is authoritative
        // because onObjectDelta applies OBJECT_DELTA_INVENTORY — an AI critter that
        // equips/switches its weapon mid-fight updates the mirror, so a ranged NPC
        // resolves its real gun instead of a stale join-blob weapon (or none).
        //
        // Bug D: if the mirror is MISSING the fired weapon (the attacker acquired it
        // after this viewer joined — the remote-critter reconcile is equip-flags-only
        // and never creates), that resolve returns null/wrong and the swing replays as
        // a punch. The wire now carries the fired weapon's netId + pid; resolve it, or
        // lazily recreate it in the mirror (CREATE-ONLY, never free — so the in-flight-
        // replay double-free hazard the equip-flags path avoids stays untouched) and
        // seat it in the firing hand so _action_attack's inventory re-resolve finds it.
        Object* weapon = critterGetWeaponForHitMode(attacker, pa.hitMode);
        if (pa.weaponPid < 0) {
            weapon = nullptr; // authoritative unarmed
        } else if (weapon == nullptr || weapon->pid != pa.weaponPid) {
            int handFlag = 0;
            switch (pa.hitMode) {
            case HIT_MODE_LEFT_WEAPON_PRIMARY:
            case HIT_MODE_LEFT_WEAPON_SECONDARY:
            case HIT_MODE_LEFT_WEAPON_RELOAD:
                handFlag = OBJECT_IN_LEFT_HAND;
                break;
            case HIT_MODE_RIGHT_WEAPON_PRIMARY:
            case HIT_MODE_RIGHT_WEAPON_SECONDARY:
            case HIT_MODE_RIGHT_WEAPON_RELOAD:
                handFlag = OBJECT_IN_RIGHT_HAND;
                break;
            }
            // Find the intended weapon in the mirror (by netId, then by pid).
            Object* found = pa.weaponNetId != 0 ? lookup(pa.weaponNetId) : nullptr;
            if (found == nullptr || found->pid != pa.weaponPid) {
                found = nullptr;
                Inventory* inv = &attacker->data.inventory;
                for (int i = 0; i < inv->length; i++) {
                    if (inv->items[i].item != nullptr && inv->items[i].item->pid == pa.weaponPid) {
                        found = inv->items[i].item;
                        break;
                    }
                }
            }
            // Not in the mirror at all → recreate it (the gDude-reconcile idiom).
            if (found == nullptr && handFlag != 0) {
                Object* item = nullptr;
                if (objectCreateWithPid(&item, pa.weaponPid) == 0 && item != nullptr) {
                    _obj_disconnect(item, nullptr); // inventory-only, not in the world
                    itemAdd(attacker, item, 1);
                    if (pa.weaponNetId != 0) {
                        item->netId = pa.weaponNetId;
                        _net[pa.weaponNetId] = item; // so a thrown-weapon adopt matches
                    }
                    found = item;
                }
            }
            if (found != nullptr && handFlag != 0) {
                // Seat it in the firing hand so critterGetWeaponForHitMode /
                // _action_attack resolve it. Free that hand on any other item first.
                for (int i = 0; i < attacker->data.inventory.length; i++) {
                    Object* it = attacker->data.inventory.items[i].item;
                    if (it != nullptr && it != found) {
                        it->flags &= ~handFlag;
                    }
                }
                found->flags = (found->flags & ~OBJECT_IN_ANY_HAND) | handFlag;
                weapon = found;
            }
        }
        attack.weapon = weapon;
        attack.attackerDamage = pa.attackerDamage;
        attack.attackerFlags = pa.attackerFlags;
        attack.defender = defender;
        attack.tile = defender->tile;
        attack.defenderHitLocation = pa.defenderHitLocation;
        attack.defenderDamage = pa.defenderDamage;
        attack.defenderFlags = pa.defenderFlags;
        // Compact out unmapped extras (a null Object* would crash _show_damage_extras,
        // which dereferences obj->fid). Keep damage/flags aligned to survivors.
        attack.extrasLength = 0;
        for (int i = 0; i < pa.extraCount; i++) {
            Object* extra = lookup(pa.extraNetId[i]);
            if (extra == nullptr) {
                continue;
            }
            int j = attack.extrasLength;
            attack.extras[j] = extra;
            attack.extrasDamage[j] = pa.extraDamage[i];
            attack.extrasFlags[j] = pa.extraFlags[i];
            attack.extrasLength = j + 1;
        }

        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            // Root-fix verification: after OBJECT_DELTA_INVENTORY is applied, the
            // resolved weapon pid should match the server's fired weapon and animCode
            // should be a FIRE_* (ANIM_FIRE_SINGLE=45) for a gun, not ANIM_STAND (0).
            fprintf(stderr, "[atk] REPLAY attacker=%d hitMode=%d resolvedWeaponPid=%d resolvedFid=0x%X animCode=%d\n",
                pa.attackerNetId, pa.hitMode,
                attack.weapon != nullptr ? attack.weapon->pid : -1,
                attack.weapon != nullptr ? attack.weapon->fid : 0,
                weaponGetAnimationForHitMode(attack.weapon, pa.hitMode));
        }
        clientCombatAnimPlay(&attack);

        // Per-hit HP: accumulate THIS blow's damage to the dude (defender + any extras
        // that are the dude) and apply it when the animation FINISHES — presentationPump
        // flushes _pendingDudeTick on the next idle — so the bar drops as the hit lands,
        // not as the swing begins (and a fatal blow reads its full swing before 0 HP).
        int dudeDamage = 0;
        if (defender == gDude) {
            dudeDamage += pa.defenderDamage;
        }
        for (int i = 0; i < pa.extraCount; i++) {
            if (lookup(pa.extraNetId[i]) == gDude) {
                dudeDamage += pa.extraDamage[i];
            }
        }
        _pendingDudeTick = dudeDamage;
    }

    // SUPERSEDED by rollDudeHp(): the shown HP now eases toward _dudeHpAuth every pump
    // frame, and _dudeHpAuth is set by the per-blow OBJECT_DELTA_HP, so the old one-step
    // drop here would only fight the roll (a visible jump). Kept as a no-op so the
    // _pendingDudeTick plumbing (onAttackResult, the non-recorded melee/ranged path)
    // stays wired without double-applying. The blow's timing (drop-as-it-lands) folds
    // into the action-frame commit of Pillar 1 / phase 3.
    void tickDudeHp(Object* /*victim*/, int /*damage*/)
    {
    }

    // Pin the dude's shown HP to the authoritative value. Run at every turn boundary
    // (applyTurnStart) and at end of combat (applyCombatExit): by then the previous
    // actor's blows have all ticked, so this only corrects rounding / any non-attack
    // HP change (poison, fire) that has no ATTACK_RESULT to tick from.
    void reconcileDudeHp()
    {
        if (gDude == nullptr || gDude->data.critter.hp == _dudeHpAuth) {
            return;
        }
        gDude->data.critter.hp = _dudeHpAuth;
        interfaceRenderHitPoints(false);
        interfaceBarRefresh();
    }

    // -- Presentation feedback (S2, COMBAT_CLIENT_DESIGN.md §3.e) ------------
    // Console/float/sfx cues the server already streams (combat damage lines,
    // taunts, weapon/hit/death sounds). Mirror of ClientPresenter's SP handlers
    // (presenter_client.cc). VIEWER-ONLY: headless has no display monitor/sound
    // and these carry no object state, so gating keeps every gate byte-identical.

    // In combat these are queued so they release WITH the attack they caption (§3.c);
    // out of combat (ambient look/console) they apply immediately as before.

    void onConsole(Reader& r)
    {
        std::string text = r.str();
        // TRAILING address field, present only on a refusal aimed at one actor
        // (presenter_network.cc consoleMessageFor). Absent on every broadcast
        // message, including every one emitted before this field existed — hence
        // the remaining() test rather than an unconditional read.
        int actorNetId = r.remaining() >= 4 ? r.i32() : 0;
        // SECOND trailing field: the message CHANNEL (msg_channel.h). Absent on the
        // default channel — which is every line the engine emitted before channels
        // existed — so the same remaining() test, one field further along.
        int channel = r.remaining() >= 4 ? r.i32() : kMsgChannelDefault;
        if (!clientViewerActive()) return;
        if (actorNetId != 0 && (gDude == nullptr || actorNetId != gDude->netId)) {
            return; // somebody else's refusal — nothing happened, so show nothing
        }
        // Feature A: a refusal aimed at us (addressed + on the refusal channel) lets the
        // out-of-combat input block revert an optimistic hand flip the server rejected.
        if (actorNetId != 0 && channel == kMsgChannelRefusal) {
            gViewerRefusalPending = true;
        }
        // A REFUSAL IS NOT NARRATION AND MUST NOT BE PACED. Everything else on this
        // channel captions an event and belongs beside it, but a refusal answers an
        // INPUT — the player's click, right now — and the whole reason it exists
        // (bugs list U) is to stop them from concluding the game is lagging and
        // spam-clicking. Releasing it behind a long combat replay would deliver the
        // explanation seconds after the confusion it was meant to prevent, and would
        // caption whatever animation happened to be playing when it drained.
        // TEMP DIAGNOSTIC [cons]: "I entered several random encounters and never got the
        // line saying what they were." The line is PROVEN on the wire (teed headless
        // run), so it dies on this side, and there are exactly two doors out of here.
        // The paced door is the suspect: a queued console event is discarded outright by
        // clearCombatMirror()'s _presQueue.clear() on the next world (re)load — and
        // arriving in a random encounter IS a world load, one that also flips combat on.
        // So a line that lands while the viewer's combat MIRROR is still true (e.g. left
        // over from the fight we just walked out of) is enqueued and then thrown away.
        fprintf(stderr, "[cons] rx inCombat=%d channel=%d -> %s: \"%.60s\"\n",
            _inCombat ? 1 : 0, channel,
            (_inCombat && channel != kMsgChannelRefusal) ? "QUEUED (paced)" : "shown now",
            text.c_str());
        if (_inCombat && channel != kMsgChannelRefusal) {
            PresEvent e;
            e.kind = PresKind::kConsole;
            e.text = text;
            e.consoleChannel = channel;
            enqueue(e);
        } else {
            applyConsole(text, channel);
        }
    }

    void applyConsole(const std::string& text, int channel)
    {
        // The interface bar's message log — "You were hit for N points…".
        displayMonitorAddMessageStyled(const_cast<char*>(text.c_str()), channel);
    }

    void onFloatText(Reader& r)
    {
        int netId = r.i32();
        std::string text = r.str();
        if (!clientViewerActive()) return;
        if (_inCombat) {
            PresEvent e;
            e.kind = PresKind::kFloat;
            e.floatNetId = netId;
            e.text = text;
            enqueue(e);
        } else {
            applyFloat(netId, text);
        }
    }

    void applyFloat(int netId, const std::string& text)
    {
        Object* owner = lookup(netId);
        if (owner == nullptr) return; // may have died/left since the event was queued
        // font/color are client-local styling, dropped by the wire (§3.e); use the
        // vanilla combat-float convention (font 101, yellow on a black outline —
        // actions.cc). AI taunt colors aren't reconstructable without the AI packet.
        Rect rect;
        if (textObjectAdd(owner, const_cast<char*>(text.c_str()), 101,
                _colorTable[32747], _colorTable[0], &rect) == 0) {
            tileWindowRefreshRect(&rect, owner->elevation);
        }
    }

    void onSfx(Reader& r)
    {
        std::string name = r.str();
        if (!clientViewerActive()) return;
        if (_inCombat) {
            PresEvent e;
            e.kind = PresKind::kSfx;
            e.text = name;
            enqueue(e);
        } else {
            applySfx(name);
        }
    }

    // Background music. NOT queued behind the combat presentation pacer like sfx —
    // music is ambient, not a beat in the action, and delaying a map's track until
    // the fight's cues drain would start it minutes late. Applied immediately.
    void onEncounterPrompt(Reader& r)
    {
        std::string title = r.str();
        std::string body = r.str();
        if (!clientViewerActive()) return;

        // ►►►► LATCH, DO NOT OPEN — the third handler to need this rule and the one
        // that ignored it. onElevatorPrompt and onAutomapOpen both say it in full: we
        // are standing INSIDE pump() -> drain(), and a blocking screen opened here runs
        // an input loop whose service ticker calls pump() again. That re-entry is not
        // merely untidy, it is fatal, and this is the "the prompt appears for one frame
        // and declines itself" bug in its entirety:
        //
        //   drain() is mid-frame. It has already done _expectSeq++ but not yet
        //   _pos += 18 + payloadLen, so the frame is still at the head of _buf. The
        //   nested drain() re-parses that SAME frame, sees seq == _expectSeq - 1,
        //   reports "frame seq gap" and returns FALSE. pump() propagates the false, and
        //   viewerServiceTicker reads it as "server gone" and injects KEY_ESCAPE —
        //   which lands in the box we just opened. rc = 0 = decline, ~16 ms, every
        //   single time. Owner trace: `[wmenc] ESC INJECTED by ticker: pump() failed /
        //   server gone` immediately followed by `keyCode=27`, with the server merrily
        //   streaming state on either side of it.
        //
        // So the box moves out of the decoder. The server is BLOCKED on the answer, so
        // unlike the elevator this cannot wait for the main loop's no-modal-open point
        // (the viewer is inside its worldmap modal here) — viewerServiceTicker takes the
        // latch instead, one frame later, outside drain().
        _encPromptPending = true;
        _encPromptTitle = title;
        _encPromptBody = body;
    }

    void onEncounterClose(Reader& r)
    {
        if (!clientViewerActive()) return;
        // Another viewer answered; the server moved on. ONLY break out if we are still
        // in the encounter box — otherwise a late (high-RTT) CLOSE would inject an ESC
        // into whatever modal the player opened after their own answer, or their game.
        // The re-entrant socket poll runs inside showDialogBox's inputGetInput, so when
        // the box is open this flag is set and the ESC lands in that box's loop.
        if (gEncounterPromptActive) {
            wmencTagEscInjection("onEncounterClose");
            enqueueInputEvent(KEY_ESCAPE);
        }
    }

    void onMoviePlay(Reader& r)
    {
        int movie = r.i32();
        int flags = r.i32();
        if (!clientViewerActive()) return;

        // gameMoviePlay BLOCKS in its own pump until the movie ends or the user
        // skips it, so nothing of ours runs for its duration — the server is parked
        // in the movie barrier meanwhile and is not producing world state to miss.
        //
        // Marking seen locally too keeps the viewer's own ledger honest for anything
        // client-side that reads it; the server's copy is the authority and the one
        // that reaches the save.
        gameMoviePlay(movie, flags);

        // Release the room. FIRST ACK WINS by design (game_movie.h): whoever gets
        // here first — finished or skipped — ends it for everybody.
        clientViewerMovieAck();
    }

    // Another viewer skipped/finished, so the server freed the movie barrier and told
    // the room to stop. Break OUT of this viewer's own blocking playback loop by
    // requesting the movie stop; the loop's next _movieUpdate clears _running and
    // gameMoviePlay returns (which then acks — harmless, the barrier is already free).
    // Serviced mid-movie because the viewer's socket ticker runs inside the same
    // inputGetInput pump the movie loop spins on. [[movie-playback-coop]]
    void onMovieStop(Reader& r)
    {
        if (!clientViewerActive() || !gameMovieIsPlaying()) {
            return;
        }
        _movieStop();
    }

    // Co-op WORLD-STATE (baseline): the movie SEEN ledger. A viewer that joined
    // AFTER the vault-suit movie played never marked MOVIE_VSUIT seen, so its own
    // _proto_dude_update_gender derived its LOCAL dude body AND the inventory paper-
    // doll art (_art_vault_guy_num) as TRIBAL — even though the wire body from the
    // server was correct, map.cc's client-side derive clobbered gDude back on every
    // map load. Sync the ledger, then re-derive so both self-view and inventory
    // match the world. [[vault-suit-appearance-gap]]
    void onPlayerSheet(Reader& r)
    {
        int slot = r.i32();
        int len = (int)r.u16();
        std::vector<unsigned char> buf(len > 0 ? (size_t)len : 0);
        for (int i = 0; i < len; i++) {
            buf[i] = r.u8();
        }
        if (!clientViewerActive() || r.overflow() || len <= 0) {
            return;
        }
        // The block carries its own firstSlot; `slot` on the wire is redundant but
        // keeps the event self-describing for tracing. playerSheetBlockRead writes
        // the row into the per-actor proto (gPlayerActorProtos / gDudeProto), which
        // is what the character screen reads live — so the change shows on the next
        // open even mid-session. Idempotent: a join blob later overwrites the row,
        // so an out-of-order delta converges.
        File* stream = fileOpenMemory(buf.data(), buf.size());
        if (stream == nullptr) {
            return;
        }
        int applyRc = playerSheetBlockRead(stream);
        // TEMP DIAGNOSTIC [psht]: the other two cuts of "only shows up if I reconnect".
        // Paired with the server's [psht] emit line: a server emit with NO line here
        // means the event never reached the decoder; rc=-1 here means the row arrived
        // and was REFUSED; rc=0 means it was applied and the remaining suspect is the
        // repaint (the open character screen polls clientViewerConsumeSheetDirty).
        // f2_server drops debugPrint, so this goes to stderr deliberately.
        fprintf(stderr, "[psht] apply slot=%d len=%d rc=%d mySlot=%d\n",
            slot, len, applyRc, gDude != nullptr ? playerActorSlotOf(gDude) : -1);
        if (applyRc == -1) {
            debugPrint("client_net: player-sheet delta apply failed (slot %d)\n", slot);
        } else {
            gPlayerSheetDeltaDirty = true;
            // ►► RE-DERIVE THE INDICATOR BAR from the row that just arrived. SNEAK,
            // LEVEL and ADDICT are read straight out of this proto row by
            // indicatorBarRefresh, and nothing else on a viewer would repaint them —
            // so without this the server flipping our sneak state (or granting a
            // level-up badge) is invisible until some unrelated repaint happens to
            // run. Only for OUR row: another player's flags are not on our bar.
            // [[no-re-derivation-path-bug-class]]
            if (gDude != nullptr && slot == playerActorSlotOf(gDude)) {
                indicatorBarRefresh();
            }
        }
        fileClose(stream);
    }

    // ELEVATORS (docs/COOP_COVERAGE.md — this was one of the silent holes). The
    // server owns the destination table and has no screen; we own the screen and know
    // nothing about destinations. So: show vanilla's panel, send back the BUTTON, and
    // let the server ride us. Addressed — every other viewer drops it.
    void onElevatorPrompt(Reader& r)
    {
        int actorNetId = r.i32();
        int elevator = r.i32();
        int startLevel = r.i32();
        if (r.overflow() || !clientViewerActive() || gDude == nullptr) {
            return;
        }
        if (actorNetId != gDude->netId) {
            return; // somebody else is in the elevator
        }

        // ►► LATCH, DO NOT OPEN. We are inside pump(): the panel runs its own
        // blocking input loop, and that loop's service ticker pumps the wire, so
        // opening it here would re-enter the decoder we are standing in. Exactly the
        // hazard the in-combat inventory grant documents ("the screen runs a blocking
        // loop and must not be entered from inside pump()"). The main loop opens it at
        // its no-modal-open point.
        _elevatorPending = true;
        _elevatorType = elevator;
        _elevatorStartLevel = startLevel;
    }

    // THE MOTION SENSOR. Using it consumed a charge server-side; the effect is entirely
    // our screen (the automap is viewer-local — the server keeps no automap at all), so
    // the server just tells the user's client to open it. Addressed: every other viewer
    // drops it. Same LATCH-DO-NOT-OPEN rule as the elevator panel above — automapShow
    // runs a blocking input loop and we are standing inside pump().
    void onAutomapOpen(Reader& r)
    {
        int actorNetId = r.i32();
        bool usingScanner = r.i32() != 0;
        if (r.overflow() || !clientViewerActive() || gDude == nullptr) {
            return;
        }
        if (actorNetId != gDude->netId) {
            return; // somebody else waved the scanner around
        }

        _automapPending = true;
        _automapUsingScanner = usingScanner;
    }

    // SCREEN FADES — emitted since the beginning and, until now, DROPPED ON THE FLOOR:
    // there was no case for them in this switch, so a script's gfade_out/gfade_in (grave
    // digging, a book, a Doctor session) did nothing at all on a viewer.
    //
    // Addressed: 0 means everyone, otherwise only the actor it names. Latched rather
    // than applied here — paletteFadeTo animates over frames, and pump() must not sit
    // inside a visual loop.
    void onScreenFade(Reader& r, bool toBlack)
    {
        int actorNetId = r.i32();
        if (r.overflow() || !clientViewerActive()) {
            return;
        }
        if (actorNetId != 0 && (gDude == nullptr || actorNetId != gDude->netId)) {
            return; // somebody else's time is passing
        }

        // Collapse CONSECUTIVE duplicates only (black over black, in over in): those
        // are genuinely redundant, and a repeat fade-to-black is a few hundred wasted
        // milliseconds the player reads as a stall. An out followed by an in is NOT
        // redundant — it IS the fade, and collapsing that pair was the bug where fades
        // never appeared at all.
        if (toBlack == (_fadeBlackSinceMs != 0)) {
            return;
        }
        applyFade(toBlack);
    }

    // Scripted cutscene input lock. Addressed exactly like onScreenFade — the script
    // issues the two together, so they must agree on who is watching.
    //
    // gameUiDisable/Enable are idempotent (both early-out on gGameUiDisabled), so a
    // duplicate or a lock we somehow never paired is not cumulative. That matters:
    // this is the one event that can take the player's hands away, so the failure
    // direction has to be "unlock wins".
    void onScreenInputLock(Reader& r)
    {
        bool locked = r.i32() != 0;
        int actorNetId = r.i32();
        if (r.overflow() || !clientViewerActive()) {
            return;
        }
        if (actorNetId != 0 && (gDude == nullptr || actorNetId != gDude->netId)) {
            return; // somebody else's cutscene
        }
        if (locked) {
            gameUiDisable(0);
        } else {
            gameUiEnable();
        }
    }

    void onMovieSeenState(Reader& r)
    {
        int count = (int)r.u16();
        bool changed = false;
        for (int i = 0; i < count; i++) {
            if (r.u8() != 0 && !gameMovieIsSeen(i)) {
                gameMovieMarkSeen(i); // bounds-guarded; monotonic (seen never clears)
                changed = true;
            }
        }
        if (!changed || !clientViewerActive() || gDude == nullptr) {
            return;
        }
        // Re-derive the host-side look now the flag is present. objectSetFid inside
        // does NOT reset obj->frame; a tribal->jumpsuit swap can strand a frame index
        // past the new art's count and render nothing (frame-index-render-gotcha), so
        // reset it. Then repaint — this arrives async off the wire.
        _proto_dude_update_gender();
        objectSetFrame(gDude, 0, nullptr);
        tileWindowRefresh();
    }

    void onMusicPlay(Reader& r)
    {
        std::string name = r.str();
        int fadeIn = r.u8();
        if (!clientViewerActive()) return;

        // DE-DUPLICATE. The server re-announces the track on every baseline so late
        // joiners get music at all, and backgroundSoundLoad has no "already playing
        // this" check — it deletes and reloads unconditionally, so without this a
        // rebaseline would restart the track from the top under everyone already
        // listening. Cleared by STOP so a retune to the same name still replays.
        // ...but only while that track is actually still playing. If it died
        // (see musicWatchdog) the re-announce is exactly what brings it back.
        if (name == _musicTrack && backgroundSoundIsPlaying()) return;
        _musicTrack = name;
        _musicRetryAtMs = 0;

        _gsound_background_play_level_music(name.c_str(), fadeIn);
    }

    void onMusicStop(Reader& r)
    {
        (void)r; // no payload
        if (!clientViewerActive()) return;
        _musicTrack.clear();
        backgroundSoundDelete();
    }

    void onSfxAt(Reader& r)
    {
        std::string name = r.str();
        r.i32(); // source netId — positional volume is a banked v1 refinement (§3.e)
        if (!clientViewerActive()) return;
        if (_inCombat) {
            PresEvent e;
            e.kind = PresKind::kSfx;
            e.text = name;
            enqueue(e);
        } else {
            applySfx(name);
        }
    }

    // ---- Barter ------------------------------------------------------------
    void onBarterBegin(Reader& r)
    {
        int merchantNetId = r.i32();
        int driverNetId = r.i32();
        if (!clientViewerActive() || r.overflow()) return;
        clientBarterOnBegin(merchantNetId, driverNetId);
    }

    void onBarterState(Reader& r)
    {
        // Counts precede their rows, so nothing here hand-counts a row size --
        // that drift is what silently dropped the last row of every roster.
        constexpr int kMaxRows = 64;
        static int pids[4][kMaxRows];
        static int qtys[4][kMaxRows];
        int counts[4] = { 0, 0, 0, 0 };

        // Order: driver inventory, merchant inventory, player table, merchant table.
        for (int list = 0; list < 4; list++) {
            int n = r.i32();
            if (n < 0 || n > kMaxRows || r.overflow()) return;
            for (int i = 0; i < n; i++) {
                pids[list][i] = r.i32();
                qtys[list][i] = r.i32();
            }
            counts[list] = n;
        }
        int offerValue = r.i32();
        int askingValue = r.i32();
        int resultCode = r.i32(); // last-commit result (append-only, see presenter_network)
        // Bounds-check BEFORE applying: a truncated frame would otherwise rebuild
        // the mirrors from half-read garbage pids.
        if (!clientViewerActive() || r.overflow()) return;

        ClientBarterList lists[4];
        for (int i = 0; i < 4; i++) {
            lists[i].pids = pids[i];
            lists[i].qtys = qtys[i];
            lists[i].count = counts[i];
        }
        clientBarterOnState(lists[0], lists[1], lists[2], lists[3], offerValue, askingValue, resultCode);
    }

    void onBarterEnd(Reader&)
    {
        if (!clientViewerActive()) return;
        clientBarterOnEnd();
    }

    // ---- Steal / pickpocket / plant -----------------------------------------
    // The screen itself is opened from the MAIN loop off clientStealActive(), not
    // here: this runs inside the wire pump, and a blocking modal entered from the
    // pump applies later events from under its own feet.
    // The server fired a loot interaction for THIS actor and opened the container.
    // Latched, never acted on here: the loot screen is a blocking modal and the
    // decoder runs inside the pump (opening one here would apply later events from
    // under its own feet — the same rule the steal/elevator/automap latches follow).
    // ►► THE ANSWER TO "IT LOOKS FINE ON MY SCREEN". A chunk of the server's own
    // per-object state; accumulate until the final one, then diff the whole thing
    // against this mirror and print every field that disagrees. The comparison runs
    // HERE, inside the pump, deliberately: it is pure reads (no modal, no allocation
    // the world depends on), and doing it at decode means the mirror it judges is the
    // one the audit was built against, not one several beats further on.
    void onStateAudit(Reader& r)
    {
        int isFinal = r.u8();
        int count = (int)r.u16();
        if (count < 0 || count > 4096) return;
        for (int i = 0; i < count; i++) {
            StateAuditRecord rec {};
            rec.netId = r.i32();
            rec.pid = r.i32();
            rec.tile = r.i32();
            rec.elevation = r.i32();
            rec.fid = r.i32();
            rec.frame = r.i32();
            rec.rotation = r.i32();
            rec.flags = (unsigned int)r.i32();
            rec.lightDistance = r.i32();
            rec.lightIntensity = r.i32();
            rec.hp = r.i32();
            rec.radiation = r.i32();
            rec.poison = r.i32();
            rec.ap = r.i32();
            rec.combatResults = r.i32();
            rec.inventoryCount = r.i32();
            rec.inventoryHash = (unsigned int)r.i32();
            _auditRecords.push_back(rec);
        }
        if (r.overflow()) {
            // A short read means the accumulated set is not the server's set; comparing
            // it would invent divergences. Drop the whole audit and say so.
            fprintf(stderr, "[audit] ABORTED — truncated chunk, %d records discarded\n",
                (int)_auditRecords.size());
            _auditRecords.clear();
            return;
        }
        if (isFinal == 0) {
            return;
        }
        int divergences = stateAuditCompare(_auditRecords, stderr);
        _auditRecords.clear();
        (void)divergences;
    }

    void onLootGrant(Reader& r)
    {
        int actorNetId = r.i32();
        int containerNetId = r.i32();
        if (r.overflow() || !clientViewerActive()) {
            return;
        }
        if (gDude == nullptr || actorNetId != gDude->netId) {
            return; // somebody else's container
        }
        _lootGrantNetId = containerNetId;
    }

    void onStealBegin(Reader& r)
    {
        int thiefNetId = r.i32();
        int targetNetId = r.i32();
        if (!clientViewerActive() || r.overflow()) return;
        // The thief's pack is the LEFT panel on every screen, including the
        // spectators' — so their mirror of it must be reconciled in full, not
        // left on the equip-flags-only path every other critter takes. See the
        // gate in the inventory delta below.
        gViewerStealThiefNetId = thiefNetId;
        gViewerStealTargetNetId = targetNetId;
        clientStealOnBegin(thiefNetId, targetNetId);
    }

    void onStealState(Reader&)
    {
        // Payload-free by design: the item movement arrived as ordinary inventory
        // deltas in this very frame (the server scans and then emits this to
        // flush them out of a parked tick). Just repaint.
        if (!clientViewerActive()) return;
        clientStealOnState();
    }

    void onStealEnd(Reader&)
    {
        if (!clientViewerActive()) return;
        gViewerStealThiefNetId = 0;
        gViewerStealTargetNetId = 0;
        clientStealOnEnd();
    }

    void onDialogNode(Reader& r)
    {
        int speakerNetId = r.i32();
        int driverNetId = r.i32();
        int reaction = r.i32();
        std::string reply = r.str();
        std::string audioFileName = r.str();
        int headFid = r.i32();
        int optionCount = (int)r.u16();
        if (optionCount < 0 || optionCount > 64 || r.overflow()) return;
        const char* optionPtrs[64];
        std::string optionStorage[64];
        int optionReactions[64];
        for (int i = 0; i < optionCount; i++) {
            optionStorage[i] = r.str();
            optionPtrs[i] = optionStorage[i].c_str();
            optionReactions[i] = r.i32(); // per-option reaction — the Empathy colours
        }
        bool speakerIsPartyMember = r.remaining() >= 1 ? r.u8() != 0 : false; // trailing, optional
        if (!clientViewerActive() || r.overflow()) return;
        clientDialogOnNode(speakerNetId, driverNetId, reaction,
            reply.c_str(), optionPtrs, optionCount, audioFileName.c_str(), headFid,
            optionReactions, speakerIsPartyMember);
    }

    void onDialogEnd(Reader& r)
    {
        r.i32(); // driver netId — not needed for teardown
        if (!clientViewerActive() || r.overflow()) return;
        clientDialogOnEnd();
    }

    void onWorldmapBegin(Reader&)
    {
        if (!clientViewerActive()) return;
        debugPrint("client_net: onWorldmapBegin — pending enter\n");
        gPendingWorldmapEnter = true;
    }

    void onWorldmapEnd(Reader&)
    {
        if (!clientViewerActive()) return;
        debugPrint("client_net: onWorldmapEnd — exiting\n");
        // [wmend] on stderr, not debugPrint: this is the line that says whether the
        // viewer ever learned the trip ended. Its ABSENCE next to a server-side
        // `[wmsrv] driver exit` is the whole diagnosis — the end never decoded (a
        // stalled pump; alt-tab is one way) rather than the loop failing to act on it.
        fprintf(stderr, "[wmend] decoded worldmapEnd: streaming=%d mode=0x%X\n",
            gWorldmapStreaming ? 1 : 0, GameMode::getCurrentGameMode());
        gWorldmapStreaming = false;
        gWorldmapStateDirty = false;
        // Clear a still-pending enter: on an aborted/instant trip (server bails with
        // map == -1, or the player escaped) begin+end can arrive in one pump() batch
        // before the main loop consumes the latch at main.cc:1317, which would make the
        // viewer enter wmWorldMap() for a trip the server has already ended. A no-op in
        // the normal path (the latch was consumed on entry).
        gPendingWorldmapEnter = false;
    }

    void onWorldmapState(Reader& r)
    {
        if (!clientViewerActive() || r.overflow()) return;
        int posX = r.i32();
        int posY = r.i32();
        int destX = r.i32();
        int destY = r.i32();
        bool walking = r.u8() != 0;
        int walkDist = r.i32();
        wmGenData.worldPosX = posX;
        wmGenData.worldPosY = posY;
        wmGenData.walkDestinationX = destX;
        wmGenData.walkDestinationY = destY;
        wmGenData.isWalking = walking;
        wmGenData.walkDistance = walkDist;
        wmGenData.carFuel = r.i32();
        wmGenData.currentAreaId = r.i32();
        wmGenData.isInCar = r.u8() != 0;
        int visitedState = r.i32();
        unsigned int entranceMask = (unsigned int)r.i32();
        if (r.overflow()) return;

        // ►► ADOPT THE AUTHORITY'S VIEW OF THIS CITY. Our own CityInfo holds whatever
        // worldmap.txt started with, which is not what the party has discovered — so the
        // town map either would not open at all or would offer gates nobody has found.
        // Only the area underfoot is touched, and only while streaming: this is a mirror
        // of server state, not local progress.
        if (wmGenData.currentAreaId != -1) {
            CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);
            city->visitedState = visitedState;
            int count = city->entrancesLength;
            if (count > 32) {
                count = 32;
            }
            for (int index = 0; index < count; index++) {
                city->entrances[index].state = (entranceMask & (1u << index)) != 0 ? 1 : 0;
            }
        }

        debugPrint("client_net: onWorldmapState pos=%d,%d dst=%d,%d walk=%d dist=%d area=%d visited=%d entrances=0x%X\n",
            posX, posY, destX, destY, walking ? 1 : 0, walkDist, wmGenData.currentAreaId,
            visitedState, entranceMask);
        gWorldmapStateDirty = true;
    }

    // Worldmap fog of war. The server is authoritative for which subtiles the
    // party has visited/knows; the viewer's own grid is whatever its last local
    // session left behind. Scatter the flattened grid back into wmTileInfoList
    // (same tile-major/row/column order the server flattened it in) and mark the
    // worldmap dirty so the modal repaints with the new fog.
    void onWorldmapAreas(Reader& r)
    {
        if (!clientViewerActive() || r.overflow()) return;
        int count = r.u16();
        if (count != wmMaxAreaNum || wmAreaInfoList == nullptr) {
            debugPrint("client_net: worldmap areas size mismatch got=%d want=%d\n", count, wmMaxAreaNum);
            return;
        }
        for (int i = 0; i < count; i++) {
            int state = r.u8();
            int visited = r.u8();
            unsigned int mask = (unsigned int)r.i32();
            if (r.overflow()) return;
            CityInfo* city = &(wmAreaInfoList[i]);
            city->state = state;
            city->visitedState = visited;
            int entrances = city->entrancesLength;
            if (entrances > 32) entrances = 32;
            for (int e = 0; e < entrances; e++) {
                city->entrances[e].state = (mask >> e) & 1u;
            }
        }
        debugPrint("client_net: onWorldmapAreas applied n=%d\n", count);
        gWorldmapStateDirty = true;
    }

    void onWorldmapSubtiles(Reader& r)
    {
        if (!clientViewerActive() || r.overflow()) return;
        int count = r.u16();
        int expected = wmMaxTileNum * SUBTILE_GRID_HEIGHT * SUBTILE_GRID_WIDTH;
        if (count != expected) {
            // Both sides parse the same worldmap.txt, so this should be
            // impossible — drop rather than scatter a misaligned grid.
            debugPrint("client_net: worldmap subtiles size mismatch got=%d want=%d\n",
                count, expected);
            return;
        }
        int i = 0;
        for (int tileIndex = 0; tileIndex < wmMaxTileNum; tileIndex++) {
            TileInfo* tile = &(wmTileInfoList[tileIndex]);
            for (int column = 0; column < SUBTILE_GRID_HEIGHT; column++) {
                for (int row = 0; row < SUBTILE_GRID_WIDTH; row++, i++) {
                    tile->subtiles[column][row].state = r.u8();
                }
            }
        }
        if (r.overflow()) return;
        debugPrint("client_net: onWorldmapSubtiles applied n=%d\n", count);
        gWorldmapStateDirty = true;
    }

    void applySfx(const std::string& name)
    {
        soundPlayFile(name.c_str());
    }

    bool _blobDeferred = false; // rebaseline buffered while a modal was open (apply on close)
    bool _loaded;
    int _loadCount = 0;
    bool _inCombat = false; // P3 combat framing (presentation-only)
    std::string _musicTrack; // currently-playing background track, for MUSIC_PLAY dedupe
    unsigned int _musicRetryAtMs = 0; // musicWatchdog backoff deadline (getTicks)
    bool _audioNoticeShown = false; // one-time why-is-there-no-sound line on the HUD

public:
    // Background music watchdog, run once per pump. The track is a streamed,
    // looping Sound; a long stall on this thread (a rebaseline blob, a modal,
    // a map change) starves the mixer and the sound library retires the track
    // as finished. Nothing restarted it: the server only re-announces music on
    // baselines and onMusicPlay used to drop that by name. Vanilla music never
    // stops, so re-arm the expected track whenever it is not playing, with a
    // backoff so a missing music file cannot spin the load.
    void musicWatchdog()
    {
        if (_musicTrack.empty() || backgroundSoundIsPlaying()) return;
        unsigned int now = getTicks();
        if (_musicRetryAtMs != 0 && now < _musicRetryAtMs) return;
        _musicRetryAtMs = now + 10000;
        _gsound_background_play_level_music(_musicTrack.c_str(), 12);
    }

    // One-time HUD line explaining silence. Shown after the first snapshot so
    // the display monitor exists; a player with no sound otherwise has no clue.
    void audioNotice()
    {
        if (_audioNoticeShown) return;
        _audioNoticeShown = true;
        static char line[320];
        if (!settings.sound.initialize) {
            snprintf(line, sizeof(line), "Sound is OFF in fallout2.cfg ([sound] initialize=0)");
        } else if (audioEngineInitError() != nullptr) {
            snprintf(line, sizeof(line), "Audio failed to start (%s): no sound this session", audioEngineInitError());
        } else if (!gameSoundIsInitialized()) {
            snprintf(line, sizeof(line), "Sound engine failed to initialize: no sound this session");
        } else {
            return;
        }
        displayMonitorAddMessage(line);
    }

private:
    bool _myTurn = false;
    bool _invGrantPending = false; // server granted an in-combat inventory; main loop opens it
    bool _encPromptPending = false; // encounter prompt latched out of the decoder
    std::string _encPromptTitle;
    std::string _encPromptBody;
    bool _elevatorPending = false;
    // When the screen went black (0 = not black). The fade is applied at decode; this
    // is only the watchdog's clock.
    unsigned int _fadeBlackSinceMs = 0;
    // Audit chunks accumulated so far (cleared after each completed comparison).
    std::vector<StateAuditRecord> _auditRecords;
    // Container netId the server opened for this actor (0 = none pending).
    int _lootGrantNetId = 0;
    bool _automapPending = false;
    bool _automapUsingScanner = false;
    int _elevatorType = -1;
    int _elevatorStartLevel = 0;
    bool _combatModalOpen = false; // that granted screen is up right now
    int _combatActorNetId = 0; // whose turn it is — drives the acting-critter outline (#8)
    int _dudeHpAuth = 0; // authoritative dude HP; the shown gDude->hp eases toward it (rollDudeHp)
    bool _dudeHpSeeded = false; // false until _dudeHpAuth holds a real value (delta or lazy blob seed)
    unsigned int _lastEntryId = 0; // wire v4: highest total-order entry id decoded (§8.1 seq-stamp)
    int _pendingDudeTick = 0; // this attack's dude damage, applied when its anim ends
    // Per-hex AP (sibling of per-hit HP): a combat MOVE charges its whole AP cost in one
    // beat delta; rather than dropping the bar at once, hold the SHOWN value and tick it
    // down one dot per glide hop (tickCombatMoveAp), reconciling to auth at move end. Only
    // the SHOWN value lags — gDude->ap stays authoritative (attacks / path preview read it).
    int _dudeApShown = 0; // AP value currently painted on the bar
    int _dudeApAuth = 0; // authoritative dude AP (tracks every delta)
    bool _dudeApDeferring = false; // a combat move's AP is being spread over its glide
    int _dudeApMoveHops = 0; // hops remaining at the last tick, to detect consumption
    std::deque<PresEvent> _presQueue; // ordered combat presentation (viewer only)
    std::unordered_map<int, Object*> _net;
    // Bridge for OBJ_CREATE transients that ADOPT a real netId (a thrown weapon = its
    // ground item). The transient is created at the seq's DECODE (dry) pass so _net holds
    // it before the pickup's state-lane DISCONNECT arrives (which can precede this seq
    // draining off the pump). Keyed by adoptNetId; entry is handed to the EXECUTE pass and
    // erased there (or on an early disconnect). COMBAT_MOVE_RECORD_DESIGN.md.
    std::unordered_map<int, Object*> _adoptTransients;

    // In-flight blob.
    std::vector<unsigned char> _blob;
    int _blobMapIndex = 0, _blobElevation = 0, _blobDudeNetId = 0;
    unsigned int _blobGameTime = 0, _blobCrc = 0;
    int _blobMapVersion = 0;
    unsigned int _blobMapLen = 0, _blobDudeLen = 0;
    // Total player actors carried by the blob's appendix (1 = host only).
    int _blobActorCount = 1;
    // This viewer's session, from the accept preamble (0 = file stream / unknown).
    int _mySessionId = kNoSessionId;

    int _tripwireOk;
    int _tripwireBad;

    // Live-apply diagnostics (STEP-4 S3 debugging): MOVE events whose netId hit a
    // known object vs missed the map. A steady stream of misses = a broken §C netId
    // alignment (the wire addresses objects the client never reproduced).
    int _moveHit = 0;
    int _moveMiss = 0;
};

// The source-agnostic F2NS frame walker. Bytes are fed in (from a whole file at
// once, or a socket recv at a time); drain() extracts and applies every COMPLETE
// frame currently buffered, leaving any partial-frame tail for the next feed. It
// owns the Decoder, so the file (S2) and socket (S3) paths share one decode path.
class IncrementalStream {
public:
    IncrementalStream()
        : _decoder()
    {
    }

    void musicWatchdog() { _decoder.musicWatchdog(); }

    void feed(const unsigned char* data, size_t n)
    {
        _buf.insert(_buf.end(), data, data + n);
    }

    // Apply all complete frames now buffered. Returns false on a fatal framing
    // error (bad magic, seq gap, truncated event) — the caller must stop.
    bool drain()
    {
        // Consume the one-time stream preamble: magic "F2NS" | u16 version |
        // i32 sessionId (wire_defs.h). Over a socket the sessionId is OURS —
        // it is the only per-client byte run in the protocol, and the viewer
        // needs it to find its own row in EVENT_PLAYER_ROSTER and bind to its
        // actor. In a netstream FILE it is 0 (a log has no session).
        if (!_magicDone) {
            if (_buf.size() - _pos < (size_t)kWirePreambleLen) {
                return true; // not enough bytes yet
            }
            if (_buf[_pos] != 'F' || _buf[_pos + 1] != '2' || _buf[_pos + 2] != 'N' || _buf[_pos + 3] != 'S') {
                debugPrint("client_net: bad magic\n");
                return false;
            }
            unsigned short version = (unsigned short)(_buf[_pos + 4] | (_buf[_pos + 5] << 8));
            if (version != kWireVersion) {
                // No compat shim by design: server and viewer ship together, so
                // a mismatch is a build/deploy mistake and must fail loudly
                // rather than misparse every frame after this point.
                debugPrint("client_net: wire version %u != %u (server and viewer must ship together)\n",
                    version, kWireVersion);
                return false;
            }
            _mySessionId = (int)readU32(_buf.data() + _pos + 6);
            _decoder.setSessionId(_mySessionId);
            _pos += kWirePreambleLen;
            _magicDone = true;
        }

        for (;;) {
            if (_buf.size() - _pos < 18) {
                break; // incomplete frame header (wire v4 = 18 bytes)
            }
            const unsigned char* h = _buf.data() + _pos;
            unsigned int seq = readU32(h + 0);
            // h + 4 = simTs (unused headless)
            unsigned int payloadLen = readU32(h + 8);
            unsigned short eventCount = (unsigned short)(h[12] | (h[13] << 8));
            // Wire v4: entryBase = total-order id of this frame's first event
            // (PRESENTATION_PACING_DESIGN.md §8.1). On the wire so a joiner agrees
            // with the server on entry ids from its first frame.
            unsigned int entryBase = readU32(h + 14);

            // Sanity cap: a real frame is at most a few MiB (a full-map baseline).
            // Rejecting an absurd length also keeps `18 + payloadLen` from wrapping a
            // 32-bit size_t (the file path builds on win32), which would otherwise
            // turn the bounds check below into an out-of-bounds read.
            if (payloadLen > (64u << 20)) {
                debugPrint("client_net: absurd frame payloadLen=%u (seq=%u)\n", payloadLen, seq);
                return false;
            }
            if (_buf.size() - _pos < (size_t)18 + payloadLen) {
                break; // frame body not fully arrived yet
            }
            if (!_seqSeeded) {
                // A mid-stream joiner's first frame carries whatever the global
                // stream counter has reached (per-client framing was rejected —
                // one encoder, one byte stream); seed from it. Gap detection
                // still holds from here on. A from-the-start client seeds 0.
                _expectSeq = seq;
                _seqSeeded = true;
            }
            if (seq != _expectSeq) {
                // §1: a frame-seq gap = a whole missed frame = resync territory.
                // On a reliable TCP stream this must never happen; fail loud.
                debugPrint("client_net: frame seq gap expected=%u got=%u\n", _expectSeq, seq);
                return false;
            }
            _expectSeq++;

            // Bound a sub-reader to this frame's payload so a malformed event
            // length cannot read into the next frame.
            Reader frame(h + 18, payloadLen);
            for (unsigned short e = 0; e < eventCount; e++) {
                unsigned char type = frame.u8();
                frame.u8(); // flags
                unsigned short len = frame.u16();
                if (frame.overflow() || len > frame.remaining()) {
                    debugPrint("client_net: truncated event in frame %u\n", seq);
                    return false;
                }
                Reader ev(frame.here(), len);
                _decoder.event(type, ev, entryBase + e);
                frame.skip(len); // advance past the event regardless of what ev read
            }

            _pos += (size_t)18 + payloadLen;
            _frames++;
        }

        // Reclaim the consumed prefix so a long-lived socket buffer stays bounded
        // (a partial-frame tail, if any, slides to the front).
        if (_pos != 0) {
            _buf.erase(_buf.begin(), _buf.begin() + (std::ptrdiff_t)_pos);
            _pos = 0;
        }
        return true;
    }

    bool blobLoaded() const { return _decoder.loaded(); }
    int loadCount() const { return _decoder.loadCount(); }
    bool inCombat() const { return _decoder.inCombat(); }
    bool myTurn() const { return _decoder.myTurn(); }
    void presentationPump() { _decoder.presentationPump(); }
    void recomputeCombatOutlines() { _decoder.recomputeCombatOutlines(); }
    bool everBoundToSlot() const { return _decoder.everBoundToSlot(); }
    bool combatPresentationBusy() const { return _decoder.combatPresentationBusy(); }
    bool blobDeferred() const { return _decoder.blobDeferred(); }
    void applyDeferredBlob() { _decoder.applyDeferredBlob(); }
    bool takeInventoryGrant() { return _decoder.takeInventoryGrant(); }
    int takeLootGrant() { return _decoder.takeLootGrant(); }
    bool takeEncounterPrompt(std::string* title, std::string* body) { return _decoder.takeEncounterPrompt(title, body); }
    bool takeElevatorPrompt(int* elevator, int* startLevel) { return _decoder.takeElevatorPrompt(elevator, startLevel); }
    bool takeAutomapOpen(bool* usingScanner) { return _decoder.takeAutomapOpen(usingScanner); }
    bool fadeWatchdogExpired(unsigned int nowMs, unsigned int maxBlackMs) const
    { return _decoder.fadeWatchdogExpired(nowMs, maxBlackMs); }
    void clearFadeBlack() { _decoder.clearFadeBlack(); }
    void setCombatModalOpen(bool open) { _decoder.setCombatModalOpen(open); }
    bool combatModalOpen() const { return _decoder.combatModalOpen(); }
    int frames() const { return _frames; }
    int tripwireOk() const { return _decoder.tripwireOk(); }
    int tripwireBad() const { return _decoder.tripwireBad(); }

private:
    static unsigned int readU32(const unsigned char* p)
    {
        return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
            | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    }

    Decoder _decoder;
    std::vector<unsigned char> _buf;
    size_t _pos = 0;
    bool _magicDone = false;
    // This viewer's session, learned from the accept preamble. 0 = a file
    // stream (no session) or nothing read yet.
    int _mySessionId = kNoSessionId;
    unsigned int _expectSeq = 0;
    bool _seqSeeded = false;
    int _frames = 0;
};

// TCP source (mirror of server_net.cc's SocketByteSink transport). Portable via
// platform_net.h — the live viewer works on Windows as well as POSIX.
NetSocket clientSocketConnect(const char* host, int port)
{
    // Transport init: suppresses SIGPIPE on POSIX (a dead peer must never kill us,
    // same discipline as server_net) and runs WSAStartup on Windows.
    if (!netPlatformInit()) {
        debugPrint("client_net: socket layer init failed\n");
        return kNetInvalidSocket;
    }

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host, portStr, &hints, &res);
    if (gai != 0 || res == nullptr) {
        debugPrint("client_net: resolve '%s' failed: %s\n", host, gai_strerror(gai));
        return kNetInvalidSocket;
    }

    NetSocket fd = kNetInvalidSocket;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!netSocketValid(fd)) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            break;
        }
        netCloseSocket(fd);
        fd = kNetInvalidSocket;
    }
    freeaddrinfo(res);

    if (!netSocketValid(fd)) {
        debugPrint("client_net: connect to %s:%d failed: %s\n", host, port,
            netErrorString(netLastError()));
        return kNetInvalidSocket;
    }

    // The wire is many small frames; disable Nagle for latency over throughput.
    netSetNoDelay(fd);
    // Non-blocking is a socket MODE here, not a per-recv MSG_DONTWAIT flag: Windows
    // has no such flag, so the mode is what makes pump()'s drain loop portable.
    netSetNonBlocking(fd, true);
    return fd;
}

} // namespace

// ---------------------------------------------------------------------------
// S2 file path — feed the whole file to the shared walker.
// ---------------------------------------------------------------------------
bool clientApplyStreamFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        debugPrint("client_net: cannot open stream '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 6) {
        fclose(f);
        return false;
    }
    std::vector<unsigned char> buf((size_t)size);
    size_t got = fread(buf.data(), 1, (size_t)size, f);
    fclose(f);
    if ((long)got != size) {
        return false;
    }

    IncrementalStream stream;
    stream.feed(buf.data(), buf.size());
    if (!stream.drain()) {
        return false;
    }

    debugPrint("client_net: applied %d frames; blob %s; tripwire ok=%d bad=%d\n",
        stream.frames(), stream.blobLoaded() ? "loaded" : "MISSING",
        stream.tripwireOk(), stream.tripwireBad());
    return stream.blobLoaded();
}

// ---------------------------------------------------------------------------
// S3 live viewer connection.
// ---------------------------------------------------------------------------
struct ClientConnection::Impl {
    NetSocket fd = kNetInvalidSocket;
    IncrementalStream* stream = nullptr;
};

ClientConnection::ClientConnection()
    : _impl(new Impl)
{
}

ClientConnection::~ClientConnection()
{
    close();
    delete _impl;
}

bool ClientConnection::connect(const char* host, int port)
{
    close();
    NetSocket fd = clientSocketConnect(host, port);
    if (!netSocketValid(fd)) {
        return false;
    }
    _impl->fd = fd;
    _impl->stream = new IncrementalStream();
    return true;
}

bool ClientConnection::pump()
{
    if (!netSocketValid(_impl->fd) || _impl->stream == nullptr) {
        return false;
    }
    _impl->stream->musicWatchdog();
    unsigned char buf[8192];
    // Bound the bytes ingested per pump so a peer that streams as fast as the socket
    // delivers cannot spin this loop indefinitely, starving render/ESC and letting
    // _buf grow to the whole rcvbuf. Leftover bytes are drained on the next pump.
    const size_t kPumpByteCap = 1u << 22; // 4 MiB
    size_t pumped = 0;
    for (;;) {
        // Socket is in non-blocking mode (set at connect), so this returns
        // immediately with WOULDBLOCK when drained — the portable MSG_DONTWAIT.
        long n = netRecv(_impl->fd, buf, sizeof(buf));
        if (n > 0) {
            _impl->stream->feed(buf, (size_t)n);
            pumped += (size_t)n;
            if (pumped >= kPumpByteCap) {
                break;
            }
            continue;
        }
        if (n == 0) {
            debugPrint("client_net: server closed the connection\n");
            return false;
        }
        int err = netLastError();
        if (netErrorInterrupted(err)) {
            continue;
        }
        if (netErrorWouldBlock(err)) {
            break; // nothing more pending this pump
        }
        debugPrint("client_net: recv error: %s\n", netErrorString(err));
        return false;
    }
    return _impl->stream->drain();
}

bool ClientConnection::sendLine(const char* line)
{
    if (!netSocketValid(_impl->fd) || line == nullptr) {
        return false;
    }
    // Assemble "line\n" and write it all, retrying short writes / EINTR. On any
    // genuine error close the fd so the next pump() sees the dead connection and
    // tears the viewer down (matches pump()'s failure contract).
    std::string buf(line);
    buf.push_back('\n');
    const char* p = buf.data();
    size_t remaining = buf.size();
    while (remaining != 0) {
        long n = netSend(_impl->fd, p, remaining);
        if (n < 0) {
            int err = netLastError();
            if (netErrorInterrupted(err)) {
                continue;
            }
            if (netErrorWouldBlock(err)) {
                continue; // socket write buffer full; the v1 control line is tiny
            }
            debugPrint("client_net: sendLine failed: %s\n", netErrorString(err));
            netCloseSocket(_impl->fd);
            _impl->fd = kNetInvalidSocket;
            return false;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return true;
}

bool ClientConnection::blobLoaded() const
{
    return _impl->stream != nullptr && _impl->stream->blobLoaded();
}

int ClientConnection::loadCount() const
{
    return _impl->stream != nullptr ? _impl->stream->loadCount() : 0;
}

bool ClientConnection::inCombat() const
{
    return _impl->stream != nullptr && _impl->stream->inCombat();
}

bool ClientConnection::myTurn() const
{
    return _impl->stream != nullptr && _impl->stream->myTurn();
}

bool ClientConnection::everBoundToSlot() const
{
    return _impl->stream != nullptr && _impl->stream->everBoundToSlot();
}

void ClientConnection::presentationTick()
{
    if (_impl->stream != nullptr) {
        _impl->stream->presentationPump();
    }
}

void ClientConnection::recomputeCombatOutlines()
{
    if (_impl->stream != nullptr) {
        _impl->stream->recomputeCombatOutlines();
    }
}

bool ClientConnection::combatPresentationBusy() const
{
    return _impl->stream != nullptr && _impl->stream->combatPresentationBusy();
}

bool ClientConnection::blobDeferred() const
{
    return _impl->stream != nullptr && _impl->stream->blobDeferred();
}

bool ClientConnection::takeInventoryGrant()
{
    return _impl->stream != nullptr && _impl->stream->takeInventoryGrant();
}

int ClientConnection::takeLootGrant()
{
    return _impl->stream != nullptr ? _impl->stream->takeLootGrant() : 0;
}

bool ClientConnection::takeEncounterPrompt(std::string* title, std::string* body)
{
    return _impl->stream != nullptr && _impl->stream->takeEncounterPrompt(title, body);
}

bool ClientConnection::takeElevatorPrompt(int* elevator, int* startLevel)
{
    return _impl->stream != nullptr && _impl->stream->takeElevatorPrompt(elevator, startLevel);
}

bool ClientConnection::takeAutomapOpen(bool* usingScanner)
{
    return _impl->stream != nullptr && _impl->stream->takeAutomapOpen(usingScanner);
}

bool ClientConnection::fadeWatchdogExpired(unsigned int nowMs, unsigned int maxBlackMs) const
{
    return _impl->stream != nullptr && _impl->stream->fadeWatchdogExpired(nowMs, maxBlackMs);
}

void ClientConnection::clearFadeBlack()
{
    if (_impl->stream != nullptr) _impl->stream->clearFadeBlack();
}

void ClientConnection::setCombatModalOpen(bool open)
{
    if (_impl->stream != nullptr) {
        _impl->stream->setCombatModalOpen(open);
    }
}

bool ClientConnection::combatModalOpen() const
{
    return _impl->stream != nullptr && _impl->stream->combatModalOpen();
}

void ClientConnection::applyDeferredBlob()
{
    if (_impl->stream != nullptr) {
        _impl->stream->applyDeferredBlob();
    }
}

bool ClientConnection::connected() const
{
    return netSocketValid(_impl->fd);
}

void ClientConnection::close()
{
    if (netSocketValid(_impl->fd)) {
        netCloseSocket(_impl->fd);
        _impl->fd = kNetInvalidSocket;
    }
    delete _impl->stream;
    _impl->stream = nullptr;
}

// ---- Viewer upstream verb bridge (COMBAT_CLIENT_DESIGN.md §3.b) ----------------
// The active viewer connection, registered by mainClientViewer. Shared combat code
// (combat.cc's commit fork) reaches the wire through this without linking f2_client
// into f2_core/f2_server — the fork calls a core-side function pointer whose target
// is clientViewerCommitAttack, and that target uses this connection.
static ClientConnection* gViewerConn = nullptr;

// Set true when clientViewerCommitAttack actually sends a cattack, so the caller can
// arm its input-lock (actionPending) ONLY when a verb went out. _combat_attack_this
// often returns without committing (bad-shot messages, out of range, picker cancel);
// arming the lock unconditionally would freeze combat input for the round-trip
// timeout on a click that sent nothing.
static bool gViewerAttackCommitted = false;

void clientViewerSetConnection(ClientConnection* conn)
{
    gViewerConn = conn;
}

// Per-iteration service run INSIDE every viewer modal's blocking loop (inventory /
// skilldex / char / pipboy). Those loops never call the main loop's conn.pump(), so
// without this the wire would stall while a screen is open and a server COMBAT_ENTER /
// rebaseline would be missed. Registered once via clientViewerInstallServiceTicker();
// tickersExecute (reached from inputGetInput -> _process_bk) fires it every iteration of
// every such loop AND the main loop, but it self-gates to modal-only. On combat entry (or
// a deferred rebaseline, or disconnect) it enqueues ESC so the modal force-closes at its
// next top-of-loop check — vanilla closes UI on combat entry.
// Open the latched random-encounter prompt, if one is waiting, and answer the server.
// Called ONLY from viewerServiceTicker — never from the decoder (see onEncounterPrompt).
static void showPendingEncounterPrompt()
{
    if (gViewerConn == nullptr || !clientViewerActive() || gEncounterPromptActive) {
        return; // re-entry guard: our own box's ticker runs this again every frame
    }

    std::string title;
    std::string body;
    if (!gViewerConn->takeEncounterPrompt(&title, &body)) {
        return;
    }

    // Same blocking YES/NO widget the single-player path uses. showDialogBox spins
    // inputGetInput, which still services our socket — so if another viewer answers
    // first the server's EVENT_ENCOUNTER_CLOSE lands mid-wait and injects ESC
    // (onEncounterClose), returning 0 here. FIRST ANSWER WINS: whichever verb we
    // send, a late one (after the barrier freed) is ignored server-side.
    const char* bodyPtr = body.c_str();
    gEncounterPromptActive = true;
    // ►► FLUSH QUEUED INPUT FIRST. The prompt opens unannounced on top of a screen the
    // player is actively clicking — the worldmap — and anything still queued is eaten by
    // showDialogBox's first inputGetInput, where an ESC-equivalent reads as DECLINE.
    // This is NOT what caused the self-decline (that was the re-entrant drain above),
    // but it is a real hazard on its own and costs nothing: by car an encounter can fire
    // a beat or two after the click that started the trip. keyboardReset +
    // inputEventQueueReset is the pairing main.cc uses before its own blocking prompt.
    keyboardReset();
    inputEventQueueReset();
    // TEMP DIAGNOSTIC [wmenc]: kept until this has a live-play run behind it. An answer
    // in a few ms is still the box answering ITSELF, and dialogBoxTraceNext names the
    // input that did it.
    unsigned int openedAt = getTicks();
    fprintf(stderr, "[wmenc] prompt box OPEN: \"%s\"\n", title.c_str());
    dialogBoxTraceNext("wmenc");
    int rc = showDialogBox(title.c_str(), &bodyPtr, 1, 169, 116,
        _colorTable[32328], nullptr, _colorTable[32328],
        DIALOG_BOX_LARGE | DIALOG_BOX_YES_NO);
    fprintf(stderr, "[wmenc] prompt box CLOSED after %ums: rc=%d -> %s\n",
        getTicksSince(openedAt), rc, rc != 0 ? "encaccept" : "encdecline");
    gEncounterPromptActive = false;
    clientViewerEncounterAnswer(rc != 0);
}

static void viewerServiceTicker()
{
    if (gViewerConn == nullptr) {
        return;
    }
    // ►►►► THE ENCOUNTER PROMPT OPENS HERE, ABOVE THE MODAL GATE, AND NOWHERE ELSE.
    // Above the gate because the prompt can land whether or not a modal is up (during
    // travel the worldmap is open; on a spectator it may not be) and this ticker runs in
    // the main loop as well. HERE rather than in the decoder because opening it inside
    // pump() re-enters drain(), and the nested call reports a phantom "frame seq gap"
    // that the branch below then reads as "server gone" and answers with an ESC — see
    // onEncounterPrompt for the whole chain. By this point the outer drain() has
    // returned, so the box's own service ticker re-enters a quiescent stream.
    //
    // The server is BLOCKED in its barrier until somebody answers, so this must not be
    // gated on anything that can stay false: no combat test, no claim test, no modal
    // test. showPendingEncounterPrompt guards its own re-entry.
    showPendingEncounterPrompt();
    if ((GameMode::getCurrentGameMode() & kViewerModalMask) == 0) {
        return; // not in a modal — the main loop pumps the wire itself
    }
    if (!gViewerConn->pump()) {
        wmencTagEscInjection("ticker: pump() failed / server gone");
        enqueueInputEvent(KEY_ESCAPE); // server gone — close the modal, main loop handles it
        return;
    }
    if (gViewerConn->blobDeferred()) {
        // ►►►► THE THIRD DOOR ON A CLASS WE HAVE ALREADY SHUT TWICE. The two branches
        // below both carefully exclude the worldmap — ESC there is not a local close,
        // it is the `wmesc` INTENT that cancels the authority's travel session. This
        // one did not, and it is the branch that fires on the one event that always
        // follows a trip: the server loads the destination map and ships a rebaseline,
        // kWorldmap is in kViewerModalMask so the blob DEFERS, and this line then
        // injects ESC every frame for as long as it stays deferred.
        //
        // That is self-sustaining, which is why it reads as a hard softlock rather
        // than a glitch: the blob cannot apply until the modal closes, the modal will
        // not close because ESC only sends an intent, and the server discards the
        // intent because it has no worldmap session any more. Observed as `wmesc
        // ignored (no active worldmap)` spamming with nobody touching a key.
        //
        // A deferred blob MEANS the server has loaded a different map, so the trip is
        // definitionally over — the honest action is the local close, not a keystroke
        // that means something else to the authority.
        if ((GameMode::getCurrentGameMode() & GameMode::kWorldmap) != 0) {
            gWorldmapStreaming = false; // the loop's own exit test; no keystroke involved
            return;
        }
        wmencTagEscInjection("ticker: blobDeferred (non-worldmap modal)");
        enqueueInputEvent(KEY_ESCAPE); // mapLoad must not free gDude under an open modal
        return;
    }
    if (gPendingWorldmapEnter && (GameMode::getCurrentGameMode() & GameMode::kWorldmap) == 0) {
        // The host left the map — the party is being carried to the worldmap. A viewer
        // sitting in a local modal (inventory, etc.) must drop it so the main loop can
        // enter the worldmap; otherwise the modal blocks the transition until the player
        // closes it by hand (owner-reported: P2's inventory stayed up while P1 travelled,
        // syncing only once P2 closed it). gPendingWorldmapEnter is consumed only in the
        // main loop, so it stays set while this modal blocks — same treatment as combat
        // entry below. Excludes the worldmap's own modal, which must not ESC itself.
        wmencTagEscInjection("ticker: gPendingWorldmapEnter");
        enqueueInputEvent(KEY_ESCAPE);
        return;
    }
    if (gViewerConn->inCombat()) {
        // Combat entry closes UI — EXCEPT a screen the SERVER itself sanctioned
        // this fight (Stage 4): the inventory we paid 4 AP to open, or the loot
        // screen the server opened a container for at 3 AP. ESCing those would
        // take the AP and hand back nothing. Every other modal, and an inventory
        // or loot screen opened any other way, still closes as before.
        int mode = GameMode::getCurrentGameMode() & kViewerModalMask;
        bool sanctioned = gViewerConn->combatModalOpen()
            && (mode == GameMode::kInventory || mode == GameMode::kLoot);
        // ►►►► AND NEVER THE WORLDMAP, for the same reason the gPendingWorldmapEnter
        // branch above already excludes it: ESC there is NOT a local close. The worldmap
        // is a SERVER-DRIVEN modal, and its ESC handler sends the `wmesc` INTENT
        // (worldmap_ui.cc), which cancels the authority's travel session. Force-closing a
        // local screen costs nothing; cancelling the server's worldmap is a state change
        // made on the viewer's behalf without anyone asking.
        //
        // And the flag that drives it is a MIRROR that is legitimately stale here. Leaving
        // a map mid-fight ends combat and opens the worldmap in the same server beat, so
        // the viewer can still be holding inCombat()==true from before the COMBAT_EXIT it
        // has not decoded yet. Result, reproduced with scripts/exitgrid_smoke.sh: four
        // `wmesc` with NOBODY TOUCHING A KEY, the driver exiting `areaId=-1 map=-1`
        // immediately, and the trip silently cancelled — read as "the worldmap appears and
        // then nothing works". It also fed the far worse failure, because until the
        // server_worldmap fix that map==-1 exit dismantled the map it left behind.
        //
        // Safe to skip: the server owns this screen in both directions. Its own worldmap
        // pump bails on isInCombat(), so a fight that is genuinely still running closes the
        // modal from the authority side via worldmapEnd — which is where that decision
        // belongs.
        if (mode == GameMode::kWorldmap) {
            sanctioned = true;
        }
        if (!sanctioned) {
            wmencTagEscInjection("ticker: inCombat() force-close");
            enqueueInputEvent(KEY_ESCAPE);
            return; // closing anyway — don't animate a world we are about to leave
        }
    }

    // ►► KEEP PRESENTING WHILE THE MODAL IS UP. Restores a property VANILLA HAD and our
    // own port removed: isoEnable registered _object_animate as a TICKER, and tickers run
    // inside every modal loop (inputGetInput -> _process_bk), so vanilla's world animated
    // behind an open inventory. Commit 7dffbaf moved that registration into
    // Presenter::worldEnable, and the viewer then strips it outright (main.cc:1160) because
    // local animation fights authoritative wire state — leaving presAdvance() called from
    // the MAIN frame loop only. Inside a modal, therefore, the wire kept delivering (pump()
    // above) while animation and redraw stood still.
    //
    // That is not cosmetic on a dedicated server, because THE SIM NEVER BLOCKS: the whole
    // world keeps resolving while one player browses their inventory. State raced ahead,
    // presentation froze, and everything landed at once on close — the warp/jitter. Worse,
    // a frozen pump with a live wire grows _presQueue toward kMaxQueuedPresEvents, and past
    // it enqueue() DROPS events: a real never-lossy violation, and after §12 a dropped
    // deferred event is a phantom object. Draining here is what keeps a long browse in a
    // busy map from desyncing rather than merely snapping.
    //
    // No full-screen redraw is needed and none is done: advanceGlides (client_present.cc
    // :402) and _object_animate (animation.cc) refresh their OWN dirty rects, which is
    // exactly how vanilla's ticker animated the world with no main-loop help. The modal's
    // window composites above those rects. Kill switch F2_NO_MODAL_PRESENT=1.
    if (getenv("F2_NO_MODAL_PRESENT") != nullptr) {
        return;
    }
    // Only when the isometric world is actually up: the worldmap modal disables it, and
    // stepping glides for a map nobody is looking at is pure waste.
    if (isoIsDisabled()) {
        return;
    }
    gViewerConn->presentationTick(); // start/advance queued replays, drain the queue
    presAdvance(); // glides, reg_anim sequences, reaping — each refreshing its own rects
    // Reap items unlinked mid-fight once nothing can still be pointing at them. Every
    // other flush point is a modal CLOSE, and a fight has none — without this the queue
    // would sit until teardown. The flush re-checks the replay gate itself.
    if ((GameMode::getCurrentGameMode() & kViewerModalMask) == 0) {
        clientViewerFlushDeferredItemFrees();
    }
}

void clientViewerInstallServiceTicker()
{
    tickersAdd(viewerServiceTicker);
}

void clientViewerRemoveServiceTicker()
{
    tickersRemove(viewerServiceTicker);
    clientViewerFlushDeferredItemFrees(); // reap any items deferred while a screen was open
    gViewerConn = nullptr; // the ticker captured this; don't leave it dangling past teardown
}

void clientViewerCommitAttack(Object* target, int hitMode, int hitLocation)
{
    if (gViewerConn == nullptr || target == nullptr) {
        return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "cattack %d %d %d", target->netId, hitMode, hitLocation);
    if (gViewerConn->sendLine(cmd)) {
        gViewerAttackCommitted = true;
    }
}

bool clientViewerTakeAttackCommitted()
{
    bool committed = gViewerAttackCommitted;
    gViewerAttackCommitted = false;
    return committed;
}

bool clientViewerTakeRefusal()
{
    bool refused = gViewerRefusalPending;
    gViewerRefusalPending = false;
    return refused;
}

// Dude inventory verbs (player-UI Slice 3b). The inventory screen's drag-drop /
// ctx-menu DROP resolution routes here instead of mutating the local mirror; the
// server runs the real _inven_wield/_inven_unwield/itemDropStack on the
// authoritative dude and streams the result back (Slice 2 reconcile). Encapsulate
// the wire format here so inventory_ui.cc never formats a control line.
// netId, not pid: the screen knows exactly which Object was clicked, and pid
// only says what KIND it is. With several stacks of one pid (a spear in each
// hand plus loose ones) a pid names an arbitrary slot, so the server wielded or
// dropped the wrong one. A netId of 0 means the item was never bound to the
// wire; refuse rather than fall back to pid and act on some other object.
void clientViewerWield(Object* item, int hand)
{
    if (gViewerConn == nullptr || item == nullptr) {
        return;
    }
    if (item->netId == 0) {
        debugPrint("client_net: invwield on an unbound item (pid %d) ignored\n", item->pid);
        return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "invwield %d %d", item->netId, hand);
    gViewerConn->sendLine(cmd);
}

void clientViewerMovieAck()
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine("movdone");
}

void clientViewerEncounterAnswer(bool accept)
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine(accept ? "encaccept" : "encdecline");
}

// Player chat. The text is sanitised HERE as well as on the server: a newline would
// split one chat line into two forged verbs on the control channel, so the client must
// never put one on the wire even though the server also refuses it. Length is capped by
// the caller (client_say.cc) well inside the server's line buffer.
void clientViewerSay(const char* text)
{
    if (gViewerConn == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "say %s", text);
    for (char* p = cmd; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = ' ';
        }
    }
    gViewerConn->sendLine(cmd);
}

void clientViewerUnwield(int hand)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "invunwield %d", hand);
    gViewerConn->sendLine(cmd);
}

// `quantity` is how many of the addressed stack to drop (the ctx-menu's count modal
// answer, 1 for a single/equipped item); the server clamps it to the stack it holds,
// so a mirror that is one reconcile behind can't over-drop.
void clientViewerDrop(Object* item, int quantity)
{
    if (gViewerConn == nullptr || item == nullptr) {
        return;
    }
    if (item->netId == 0) {
        debugPrint("client_net: invdrop on an unbound item (pid %d) ignored\n", item->pid);
        return;
    }
    if (quantity < 1) {
        quantity = 1;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "invdrop %d %d", item->netId, quantity);
    gViewerConn->sendLine(cmd);
}

// Eject loaded ammo from a carried weapon back into inventory (inventory ctx-menu
// UNLOAD leaf). Like invdrop, addresses the specific weapon by netId; the server
// runs weaponUnloadIntoInventory on the authoritative dude and streams the emptied
// weapon + returned ammo pack(s) back via OBJECT_DELTA_INVENTORY (Slice 2 reconcile).
void clientViewerUnload(Object* item)
{
    if (gViewerConn == nullptr || item == nullptr) {
        return;
    }
    if (item->netId == 0) {
        debugPrint("client_net: unload on an unbound item (pid %d) ignored\n", item->pid);
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "unload %d", item->netId);
    gViewerConn->sendLine(cmd);
}

// USE / apply an inventory item (out-of-combat): the inventory ctx-menu USE leaf
// for drugs / misc / weapons routes here instead of mutating the local mirror.
// The server runs the authoritative itemUseDrug / itemUseFromInventory on the
// dude (addressed by pid) and streams the consume/heal/skill-up back (Slice 2
// reconcile). Explosives take the dedicated arm path below, not this verb (the
// plain useitem verb rejects them server-side).
void clientViewerUseItem(int pid)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "useitem %d", pid);
    gViewerConn->sendLine(cmd);
}

// USE an inventory item ON a world target (out-of-combat): the use-item-on picker
// modal (inventoryOpenUseItemOn) reroutes its picked-item leaf here instead of
// running the local _action_use_an_item_on_object. The server walks the dude to the
// target (approach <= 1) and runs the authoritative action — the item's / target's
// USE_OBJ_ON script (the Temple Key unlocking its door) or the default use — and the
// world change (door open/unlock, item consume) streams back like any other event.
void clientViewerUseItemOn(int targetNetId, int pid)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "useitemon %d %d", targetNetId, pid);
    gViewerConn->sendLine(cmd);
}

// Arm a C4 / dynamite over the wire. The viewer runs the SET_TIMER dial
// (inventoryQuantitySelect SET_TIMER) LOCALLY to pick the countdown — a pure UI
// choice with no server state — then sends the chosen seconds here. The server
// arms the charge headless (skipping the blocking modal but running identical
// activate + Traps/Demolition roll + queue logic); the timed explosion + any
// door/scenery destroy stream back like any other world event.
void clientViewerArmExplosive(int pid, int seconds)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "useitem_armexplosive %d %d", pid, seconds);
    gViewerConn->sendLine(cmd);
}

// Loot-container verbs (loot slice). The loot screen's take/put transfers route
// here instead of mutating the local mirror; the server runs the authoritative
// itemMove between the dude and the container (addressed by wire netId) and
// streams the result back (Slice 2 reconcile). take = container→dude, put =
// dude→container; the whole matched top-level pid stack moves.
void clientViewerLootTake(int containerNetId, int pid, int quantity)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "take %d %d %d", containerNetId, pid, quantity);
    gViewerConn->sendLine(cmd);
}

void clientViewerLootPut(int containerNetId, int pid, int quantity)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "put %d %d %d", containerNetId, pid, quantity);
    gViewerConn->sendLine(cmd);
}

void clientViewerLootTakeAll(int containerNetId)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "takeall %d", containerNetId);
    gViewerConn->sendLine(cmd);
}

// Steal-screen verbs. Deliberately WITHOUT a netId: the server's session already
// knows whose pockets these are, and letting the client name the victim would
// turn `stake` into "reach into any critter in the world" (see the verb block in
// server_control.cc). Only the thief's client sends these; a spectator's clicks
// never reach here.
void clientViewerStealVerb(const char* verb, int pid, int quantity)
{
    if (gViewerConn == nullptr || verb == nullptr) {
        return;
    }
    char cmd[64];
    if (pid < 0) {
        snprintf(cmd, sizeof(cmd), "%s", verb); // sdone takes no arguments
    } else {
        snprintf(cmd, sizeof(cmd), "%s %d %d", verb, pid, quantity);
    }
    gViewerConn->sendLine(cmd);
}

void clientViewerBarterVerb(const char* verb, int pid, int quantity)
{
    if (gViewerConn == nullptr || verb == nullptr) {
        return;
    }
    char cmd[64];
    if (pid < 0) {
        // The arg-less verbs (bcommit/bdone/bcancel). The server's parser takes
        // missing args as defaults, so sending none is the honest encoding.
        snprintf(cmd, sizeof(cmd), "%s", verb);
    } else {
        snprintf(cmd, sizeof(cmd), "%s %d %d", verb, pid, quantity);
    }
    gViewerConn->sendLine(cmd);
}

bool clientViewerConsumeDudeInvDirty()
{
    bool dirty = gDudeInvDirty;
    gDudeInvDirty = false;
    return dirty;
}

bool clientViewerConsumeSheetDirty()
{
    bool dirty = gPlayerSheetDeltaDirty;
    gPlayerSheetDeltaDirty = false;
    return dirty;
}

// ── Character-sheet edit intents (PLAYER_SHEET_DESIGN.md §9.5) ────────────────
// One line each, no local mutation anywhere: the server rules on the spend and the
// authoritative row comes back on EVENT_PLAYER_SHEET. A refusal arrives as a console
// line on the refusal channel and the row simply does not change.
static void clientViewerSheetSend(const char* cmd)
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine(cmd);
}

void clientViewerRest(int option)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "restopt %d", option);
    gViewerConn->sendLine(cmd);
}

void clientViewerElevatorRide(int level)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "elev %d", level);
    gViewerConn->sendLine(cmd);
}

void clientViewerSheetOpen()
{
    clientViewerSheetSend("sheetopen");
}

void clientViewerSheetClose()
{
    clientViewerSheetSend("sheetclose");
}

void clientViewerSkillUp(int skill)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "skillup %d", skill);
    clientViewerSheetSend(cmd);
}

void clientViewerSkillDown(int skill)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "skilldown %d", skill);
    clientViewerSheetSend(cmd);
}

void clientViewerPerkPick(int perk)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "perkpick %d", perk);
    clientViewerSheetSend(cmd);
}

void clientViewerTagPick(int skill)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "tagpick %d", skill);
    clientViewerSheetSend(cmd);
}

void clientViewerMutatePick(int dropTrait, int gainTrait)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "mutpick %d %d", dropTrait, gainTrait);
    clientViewerSheetSend(cmd);
}

void clientViewerSetLootTarget(int netId)
{
    gViewerLootTargetNetId = netId;
    // Clearing the target (loot screen closed) also drops any pending dirty; setting a
    // real target must NOT clobber a dirty raised by a reconcile pumped this same beat.
    if (netId == 0) {
        gLootTargetInvDirty = false;
    }
}

bool clientViewerConsumeLootTargetInvDirty()
{
    bool dirty = gLootTargetInvDirty;
    gLootTargetInvDirty = false;
    return dirty;
}

void clientViewerDialogSay(int index)
{
    if (gViewerConn == nullptr || index < 0) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "dsay %d", index);
    gViewerConn->sendLine(cmd);
}

void clientViewerDialogEnd()
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine("dend");
}

void clientViewerDialogParty()
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine("dparty");
}

void clientViewerWmMove(int x, int y)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wmmove %d %d", x, y);
    gViewerConn->sendLine(cmd);
}

void clientViewerWmEnter()
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine("wmenter");
}

// Same verb, plus the town-map entrance we picked. We send an INDEX and nothing else —
// the server owns the entrance table and resolves where it leads (worldmap.cc
// wmAreaResolveEntrance).
void clientViewerWmEnterEntrance(int entranceIndex)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "wmenter %d", entranceIndex);
    gViewerConn->sendLine(cmd);
}

void clientViewerWmEscape()
{
    if (gViewerConn == nullptr) {
        return;
    }
    gViewerConn->sendLine("wmesc");
}

void clientViewerFlushDeferredItemFrees()
{
    // Called by main.cc once the inventory screen has closed (no handler holds an
    // inventory Object* anymore) and at ticker teardown. The parked equipped items
    // are re-added by equipmentApply on close and are never in this list, so freeing
    // here only reaps items the server dropped/consumed while the screen was up.
    //
    // ►► AND NOT WHILE A COMBAT REPLAY IS RUNNING. Now that a mid-fight removal unlinks
    // immediately and only defers the free, this list can hold a weapon an in-flight
    // attack's reg_anim still references — the exact double-free the old blanket
    // in-combat skip was avoiding. Closing a screen mid-fight is not proof that the
    // animation is done, so hold them: the queue is drained by the service ticker the
    // moment the presentation goes idle, and by teardown regardless.
    if (gViewerConn != nullptr && gViewerConn->combatPresentationBusy()) {
        return;
    }
    for (Object* item : gDudeDeferredItemFrees) {
        objectDestroy(item, nullptr);
    }
    gDudeDeferredItemFrees.clear();
}

} // namespace fallout
