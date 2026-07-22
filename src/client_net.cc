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
#include "worldmap_ui.h" // wmGenData + gWorldmapStreaming/gPendingWorldmapEnter/gWorldmapStateDirty
#include "color.h" // _colorTable — float-text styling (COMBAT_CLIENT_DESIGN.md §3.e)
#include "combat.h" // gCombatState mirror + COMBAT_STATE_* (§3.0)
#include "combat_defs.h" // Attack / EXPLOSION_TARGET_COUNT — ATTACK_RESULT reconstruct
#include "db.h"
#include "debug.h"
#include "display_monitor.h" // combat message log (§3.e S2)
#include "game.h" // GameMode — modal-screen detection for the viewer service ticker
#include "game_mouse.h" // wait-watch cursor over combat transitions (§3.a)
#include "game_sound.h" // sfx (§3.e S2)
#include "input.h" // enqueueInputEvent / tickersAdd — viewer modal service ticker
#include "interface.h" // combat HUD bar hooks (§3.a)
#include "inventory.h" // _inven_reset_dude — re-anchor the inventory on a local-actor rebind
#include "inventory_ui.h" // gInventory{Left,Right}HandItem/Armor — parked-equip detach check (Slice 3b)
#include "item.h" // critterGetWeaponForHitMode — ATTACK_RESULT reconstruct (§3.c)
#include "kb.h" // KEY_ESCAPE — force-close a viewer modal on combat/rebaseline
#include "map.h"
#include "actions.h" // actionExplodeReplay + actionPresReplayShowDeath — viewer explosion replay
#include "animation.h" // reg_anim_* / animationRegister* — the real engine the recorded stream drives
#include "object.h"
#include "perk.h" // perkPlayerActorSeedRanks — per-actor sheet rows
#include "player_sheet.h"
#include "pres_record.h" // PresOp / PresCallbackTag — the recorded op stream vocabulary
#include "game_movie.h" // gameMoviePlay — the viewer owns the playback pipeline
#include "movie.h" // _movieStop — break the blocking playback loop on a room-wide skip
#include "msg_channel.h"
#include "presenter.h" // ObjectDeltaField / WorldDeltaField masks
#include "proto.h" // protoPlayerActorSheetsSeed — per-actor sheet rows
#include "scripts.h"
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

// Loot slice — the container/corpse the viewer's loot screen is currently open on
// (0 = none). Set by inventoryOpenLooting; its inventory delta gets a FULL contents
// reconcile (below) so items taken/added show live, instead of the equip-flags-only
// path the generic non-dude reconcile uses. gLootTargetInvDirty tells the open loot
// loop to repaint its panels when that reconcile lands.
static int gViewerLootTargetNetId = 0;
static bool gLootTargetInvDirty = false;

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
    | GameMode::kDialog | GameMode::kWorldmap | GameMode::kBarter;

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
    };
    struct PresEvent {
        PresKind kind;
        PendingAttack attack; // kAttack
        int tsNetId = 0, tsIsPlayer = 0, tsAp = 0, tsDeadline = 0, tsFreeMove = 0; // kTurnStart
        int floatNetId = 0; // kFloat owner
        int moveNetId = 0, moveHops = 0; // kMoveRelease
        std::vector<unsigned char> seqOps; // kRecordedSeq — the raw op buffer (played at pump time)
        int seqActorNetId = 0; // kRecordedSeq — actor whose approach glide must drain before play (0 = none)
        std::string text; // kConsole / kFloat / kSfx
        int consoleChannel = kMsgChannelDefault; // kConsole — message-log style (msg_channel.h)
    };
    // Backlog safety cap. The player-turn barrier bounds the queue in practice (the
    // server blocks on the claimant, who cannot act until the queue drains), so this
    // only guards a pathological run; when hit, the oldest droppable (non-turn,
    // non-exit) event is discarded so turn boundaries never desync.
    static constexpr size_t kMaxQueuedPresEvents = 1024;

    explicit Decoder(const char* blobTmpPath)
        : _blobTmpPath(blobTmpPath), _loaded(false), _tripwireOk(0), _tripwireBad(0) {}

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

    // In-combat inventory grant (Stage 4). `take` consumes the one-shot latch set
    // by onInventoryGrant; `open` tracks whether the granted screen is currently
    // up, which is what stops the service ticker force-ESCing it (see
    // viewerServiceTicker — closing the screen the server just charged 4 AP for
    // would take the AP and give nothing back).
    bool takeInventoryGrant()
    {
        bool granted = _invGrantPending;
        _invGrantPending = false;
        return granted;
    }
    void setCombatModalOpen(bool open) { _combatModalOpen = open; }
    bool combatModalOpen() const { return _combatModalOpen; }

    // DEBUG scaffolding (combat-state desync hunt, 2026-07-18): route EVERY _inCombat
    // write through here so `grep [cbtstate]` names which handler flipped it. Combat
    // EXIT is deferred behind the replay queue, so a spurious/early flip-to-false mid-
    // turn strands the player on the out-of-combat move path (client sends `mv` -> server
    // "control mv dropped (in combat)") until the next TURN_START re-asserts true — which
    // only the 62s idle timeout forced. Logs only on an actual change (TURN_START re-
    // asserting true every enemy turn is silent). Remove once the desync is pinned.
    void setInCombat(bool v, const char* site)
    {
        if (_inCombat != v) {
            fprintf(stderr, "[cbtstate] %-16s inCombat %d->%d myTurn=%d\n",
                site, _inCombat ? 1 : 0, v ? 1 : 0, _myTurn ? 1 : 0);
        }
        _inCombat = v;
    }

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
        if (!_loaded && type != EVENT_SNAPSHOT_BLOB_BEGIN
            && type != EVENT_SNAPSHOT_BLOB_CHUNK && type != EVENT_SNAPSHOT_BLOB_END) {
            return;
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
        case EVENT_DIALOG_NODE: onDialogNode(r); break;
        case EVENT_DIALOG_END: onDialogEnd(r); break;
        case EVENT_WORLDMAP_BEGIN: onWorldmapBegin(r); break;
        case EVENT_WORLDMAP_END: onWorldmapEnd(r); break;
        case EVENT_WORLDMAP_STATE: onWorldmapState(r); break;
        case EVENT_WORLDMAP_SUBTILES: onWorldmapSubtiles(r); break;
        case EVENT_PLAYER_ROSTER: onPlayerRoster(r); break;
        case EVENT_INVENTORY_GRANT: onInventoryGrant(r); break;
        case EVENT_INVENTORY_REVOKE: onInventoryRevoke(r); break;
        case EVENT_MOVIE_PLAY: onMoviePlay(r); break;
        case EVENT_MOVIE_SEEN_STATE: onMovieSeenState(r); break;
        case EVENT_MOVIE_STOP: onMovieStop(r); break;
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
            if (it->kind != PresKind::kTurnStart && it->kind != PresKind::kExit) {
                if (it->kind == PresKind::kMoveRelease) {
                    // Its held glide must not outlive its release: snap the mover to
                    // its (already authoritative) position instead of stranding it.
                    clientAnimCancel(lookup(it->moveNetId));
                }
                _presQueue.erase(it);
                debugPrint("client_net: combat presentation backlog, dropped an event\n");
                return;
            }
        }
        _presQueue.pop_front(); // all turn/exit (degenerate) — drop the oldest anyway
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

        // Persist to scratch and load through the SAME sequence as the S1 loader
        // (main.cc F2_CLIENT_BLOB_IN): adopt the server clock, drop any current
        // map, viewer-load (no map-enter procs), apply the dude, fix combat
        // back-pointers, reproduce the netId walk, freeze scripts.
        File* w = fileOpen(_blobTmpPath, "wb");
        if (w == nullptr) { debugPrint("client_net: cannot open blob scratch\n"); return; }
        fileWrite(_blob.data(), 1, _blob.size(), w);
        fileClose(w);

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

        File* rd = fileOpen(_blobTmpPath, "rb");
        if (rd == nullptr) { debugPrint("client_net: cannot reopen blob scratch\n"); return; }

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
        if (actorPidObjects != playerActorCount()) {
            debugPrint("client_net: ACTOR LEAK — %d actor-pid objects in the world, %d registered\n",
                actorPidObjects, playerActorCount());
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
            setInCombat(false, "REBASELINE-fresh");
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
        r.i32(); // pid
        int tile = r.i32();
        int elev = r.i32();
        Object* obj = lookup(netId);
        if (obj != nullptr) {
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
            _obj_connect(obj, tile, elev, nullptr);
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
                _adoptTransients.erase(it);
                _net.erase(netId);
                objectDestroy(obj, nullptr);
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

        Object* obj = lookup(netId);
        if (obj == nullptr) return;
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
        // Safe for the same reason the loot-target path is safe, minus the timing
        // caveat: the double-free hazard that keeps the generic path equip-flags-only
        // is a CRITTER problem — an in-flight attack replay holding a pointer to the
        // weapon being reconciled. A footlocker has no attack animation. CORPSES are
        // critters and keep the old gating, so that hazard is untouched.
        bool plainContainer = obj != gDude && PID_TYPE(obj->pid) != OBJ_TYPE_CRITTER;
        if (hasInventory && obj != gDude
            && clientViewerActive()
            && (plainContainer
                || (gViewerLootTargetNetId != 0 && obj->netId == gViewerLootTargetNetId))) {
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
                if (anyModalOpen) {
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
            bool invScreenOpen = (GameMode::getCurrentGameMode() & GameMode::kInventory) != 0;
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
            if (!_inCombat) {
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
                    if (anyModalOpen) {
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

    void onWorldDelta(Reader& r)
    {
        unsigned int mask = r.u16();
        if (mask & WORLD_DELTA_GAMETIME) {
            gameTimeSetTime(r.u32());
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
        // The baseline walk just finished scoring against the blob-loaded world
        // (§D tripwire). ok>0 && bad==0 is the mid-join gate's oracle line.
        debugPrint("client_net: baseline tripwire ok=%d bad=%d (load #%d)\n",
            _tripwireOk, _tripwireBad, _loadCount);
    }

    void onMapTransition(Reader& r)
    {
        r.i32(); // mapIndex
        r.i32(); // elevation
        // v1: a fresh blob + baseline always follows (§C.4). Drop the index; the
        // next BLOB_BEGIN rebuilds the world. (Full mid-run transition = S3+.)
        presReset();
        _net.clear();
        _adoptTransients.clear(); // adopt transients die with the old world (Fable review A3)
        _loaded = false;
        setInCombat(false, "MAP_TRANSITION"); // a transition ends any local combat framing
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
        if (clientViewerActive() && _inCombat) {
            // Already in combat: this is the server re-emitting combatEnter after a
            // forced mid-fight rebaseline (another client joined). We kept our framing
            // across the reload (onBlobEnd), so IGNORE the duplicate — no replayed
            // door-slide + "iciboxx1" fanfare. A fresh client had _inCombat=false and
            // falls through to the real enter below.
            return;
        }
        setInCombat(true, "ENTER");
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
        if (clientViewerActive()) {
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
        setInCombat(false, "EXIT-immediate");
        _myTurn = false;
        debugPrint("client_net: COMBAT EXIT\n");
    }

    // The end-of-combat chrome, run by presentationPump once the queue reaches it.
    // Viewer-only (headless applies exit inline in onCombatExit).
    void applyCombatExit()
    {
        reconcileDudeHp(); // the fight's last blows have played — pin exact HP
        setInCombat(false, "EXIT-applied");
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
        // A TURN_START implies we are in combat — set the routing bool immediately so
        // a mid-fight joiner/rebaseline that missed COMBAT_ENTER still gates input
        // (§3.0). Whose turn it is (and the AP dots) is DEFERRED through the queue so
        // it flips only when the animations reach this point (see PresEvent).
        setInCombat(true, "TURN_START");
        if (!clientViewerActive()) {
            // Headless routing: apply _myTurn inline, byte-identical to before.
            _myTurn = isPlayer != 0 && gDude != nullptr && netId == gDude->netId;
            return;
        }
        PresEvent e;
        e.kind = PresKind::kTurnStart;
        e.tsNetId = netId;
        e.tsIsPlayer = isPlayer;
        e.tsAp = ap;
        e.tsDeadline = deadline;
        e.tsFreeMove = freeMove;
        enqueue(e);
    }

    // Apply a queued TURN_START: flip _myTurn and paint the AP dots / lights. Run by
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
    Object* resolveSeqRef(int ref, std::unordered_map<int, Object*>& handles)
    {
        if (ref > 0) return lookup(ref);
        if (ref < 0) {
            auto it = handles.find(ref);
            return it != handles.end() ? it->second : nullptr;
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
                        objectHide(obj, nullptr); // born hidden — the ANIMATE/MOVE ops reveal it
                        obj->flags |= OBJECT_NO_SAVE;
                        objectSetLocation(obj, tile, elev, nullptr);
                        objectSetRotation(obj, rotation, nullptr);
                    }
                    return obj;
                };
                if (adoptNetId > 0) {
                    // A thrown weapon's flight transient IS its ground item. Create it at the
                    // DECODE (dry) pass and register it under the real netId NOW, so the
                    // STATE-lane DISCONNECT (pickup) — which can arrive turns before this seq
                    // drains off the pump — resolves to it and removes it. Play-time creation
                    // was the "phantom spear survives pickup" bug when the pickup gesture
                    // backed up the pump. The object stays NO_SAVE/local; the netId is just a
                    // handle for the disconnect that follows on the state lane.
                    if (!execute) {
                        if (_adoptTransients.find(adoptNetId) == _adoptTransients.end()) {
                            Object* obj = spawnTransient();
                            if (obj != nullptr) {
                                obj->netId = adoptNetId;
                                _net[adoptNetId] = obj;
                                _adoptTransients[adoptNetId] = obj;
                            }
                        }
                    } else {
                        // Reuse the decode-created transient so the flight ops animate the
                        // SAME object the disconnect can find. Missing = disconnected in the
                        // decode->execute window (guarded below); leave the handle unset so
                        // the flight ops harmlessly no-op rather than dangle.
                        auto it = _adoptTransients.find(adoptNetId);
                        if (it != _adoptTransients.end()) {
                            handles[handle] = it->second;
                            // Do NOT erase the bridge here: onConnect/onDisconnect/onDestroy
                            // consult _adoptTransients to recognize this netId as a viewer-
                            // local adopt transient and OWN its lifecycle against the state
                            // lane (skip the redundant CONNECT that would double-node it;
                            // objectDestroy it on DISCONNECT instead of leaking it). The
                            // entry is erased there, when the object actually leaves.
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
                        if (getenv("F2_TRACE_EVENTS") != nullptr) {
                            fprintf(stderr, "[cmove-play] net=%d curTile=%d destTile=%d walkTile=%d dist=%d anim=%d ap=%d preAp=%d\n",
                                o->netId, o->tile, tile, walkTile, tileDistanceBetween(o->tile, walkTile), anim, ap, preWalkAp);
                        }
                        if (anim == ANIM_RUNNING) animationRegisterRunToTile(o, walkTile, elev, ap, delay);
                        else animationRegisterMoveToTile(o, walkTile, elev, ap, delay);
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
        // DRY pass: reserve every live participant before this beat's death-fid deltas land.
        presPlayRecordedSeq(ops.data(), (int)ops.size(), false);
        // In combat: always ride the pump (turn-serial ordering). Out of combat: an
        // ACTORED sequence (gesture/door) rides the pump too so it waits out that
        // actor's approach glide (pump gate 1d); an actor-less sequence (explosion)
        // has nothing to approach and plays immediately, exactly as before.
        if (_inCombat || actorNetId != 0) {
            PresEvent e;
            e.kind = PresKind::kRecordedSeq;
            e.seqOps = std::move(ops);
            e.seqActorNetId = actorNetId;
            enqueue(e);
        } else {
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
        attack.weapon = critterGetWeaponForHitMode(attacker, pa.hitMode);
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
        // A REFUSAL IS NOT NARRATION AND MUST NOT BE PACED. Everything else on this
        // channel captions an event and belongs beside it, but a refusal answers an
        // INPUT — the player's click, right now — and the whole reason it exists
        // (bugs list U) is to stop them from concluding the game is lagging and
        // spam-clicking. Releasing it behind a long combat replay would deliver the
        // explanation seconds after the confusion it was meant to prevent, and would
        // caption whatever animation happened to be playing when it drained.
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
        if (name == _musicTrack) return;
        _musicTrack = name;

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
        for (int i = 0; i < optionCount; i++) {
            optionStorage[i] = r.str();
            optionPtrs[i] = optionStorage[i].c_str();
        }
        if (!clientViewerActive() || r.overflow()) return;
        clientDialogOnNode(speakerNetId, driverNetId, reaction,
            reply.c_str(), optionPtrs, optionCount, audioFileName.c_str(), headFid);
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
        gWorldmapStreaming = false;
        gWorldmapStateDirty = false;
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
        debugPrint("client_net: onWorldmapState pos=%d,%d dst=%d,%d walk=%d dist=%d area=%d\n",
            posX, posY, destX, destY, walking ? 1 : 0, walkDist, wmGenData.currentAreaId);
        gWorldmapStateDirty = true;
    }

    // Worldmap fog of war. The server is authoritative for which subtiles the
    // party has visited/knows; the viewer's own grid is whatever its last local
    // session left behind. Scatter the flattened grid back into wmTileInfoList
    // (same tile-major/row/column order the server flattened it in) and mark the
    // worldmap dirty so the modal repaints with the new fog.
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

    const char* _blobTmpPath;
    bool _blobDeferred = false; // rebaseline buffered while a modal was open (apply on close)
    bool _loaded;
    int _loadCount = 0;
    bool _inCombat = false; // P3 combat framing (presentation-only)
    std::string _musicTrack; // currently-playing background track, for MUSIC_PLAY dedupe
    bool _myTurn = false;
    bool _invGrantPending = false; // server granted an in-combat inventory; main loop opens it
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
    explicit IncrementalStream(const char* blobTmpPath)
        : _decoder(blobTmpPath)
    {
    }

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
    bool combatPresentationBusy() const { return _decoder.combatPresentationBusy(); }
    bool blobDeferred() const { return _decoder.blobDeferred(); }
    void applyDeferredBlob() { _decoder.applyDeferredBlob(); }
    bool takeInventoryGrant() { return _decoder.takeInventoryGrant(); }
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
bool clientApplyStreamFile(const char* path, const char* blobTmpPath)
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

    IncrementalStream stream(blobTmpPath);
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

bool ClientConnection::connect(const char* host, int port, const char* blobTmpPath)
{
    close();
    NetSocket fd = clientSocketConnect(host, port);
    if (!netSocketValid(fd)) {
        return false;
    }
    _impl->fd = fd;
    _impl->stream = new IncrementalStream(blobTmpPath);
    return true;
}

bool ClientConnection::pump()
{
    if (!netSocketValid(_impl->fd) || _impl->stream == nullptr) {
        return false;
    }
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
static void viewerServiceTicker()
{
    if (gViewerConn == nullptr) {
        return;
    }
    if ((GameMode::getCurrentGameMode() & kViewerModalMask) == 0) {
        return; // not in a modal — the main loop pumps the wire itself
    }
    if (!gViewerConn->pump()) {
        enqueueInputEvent(KEY_ESCAPE); // server gone — close the modal, main loop handles it
        return;
    }
    if (gViewerConn->blobDeferred()) {
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
        if (!sanctioned) {
            enqueueInputEvent(KEY_ESCAPE);
        }
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

void clientViewerUnwield(int hand)
{
    if (gViewerConn == nullptr) {
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "invunwield %d", hand);
    gViewerConn->sendLine(cmd);
}

void clientViewerDrop(Object* item)
{
    if (gViewerConn == nullptr || item == nullptr) {
        return;
    }
    if (item->netId == 0) {
        debugPrint("client_net: invdrop on an unbound item (pid %d) ignored\n", item->pid);
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "invdrop %d", item->netId);
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
    for (Object* item : gDudeDeferredItemFrees) {
        objectDestroy(item, nullptr);
    }
    gDudeDeferredItemFrees.clear();
}

} // namespace fallout
