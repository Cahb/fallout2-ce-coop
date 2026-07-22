#include "server_loop.h"

#include <cstdlib>
#include <vector>

#include "animation_scheduler.h"
#include "db.h"
#include "barter_intent.h"
#include "worldmap_intent.h"
#include "client_net.h" // clientViewerActive/SetActive — core-reachable role flag
#include "combat.h"
#include "combat_intent.h"
#include "critter.h" // critterIsDead — cstart dead-actor guard
#include "dialog_intent.h"
#include "game.h" // GameMode — cstart modal-state guard
#include "game_movie.h" // gameMoviesSeenData/MOVIE_COUNT — co-op movie-seen world-state sync
#include "input.h"
#include "invariants.h"
#include "map.h"
#include "object_delta.h"
#include "presenter.h"
#include "presenter_narrate.h"
#include "presenter_network.h"
#include "proto.h" // protoPlayerActorsUpdateLook — co-op vault-suit re-derive
#include "scripts.h"
#include "server_players.h"
#include "sim_clock.h"
#include "worldmap.h" // wmMapMusicStart — baseline music re-announce
#include "object.h"

namespace fallout {

// See server_loop.h for the activation contract.

// Set for the duration of a server loop (serverRun() or serverServe(), via the
// shared serverInstall()/serverRestore()); the interlocked seams branch on it.
static bool gServerLoopActive = false;

bool serverLoopActive()
{
    return gServerLoopActive;
}

// The client-viewer counterpart of the loop flag: set once for the lifetime of a
// network viewer process (mainClientViewer, main.cc). It lives HERE, in f2_core,
// not in client-only client_net.cc, because core code branches on it too — e.g.
// object.cc's combat-mirror guards (COMBAT_CLIENT_DESIGN.md §5.1), which the
// server build links. Declared in client_net.h. Always false on server/probe
// paths (never set there), so those seams keep their original behavior.
static bool gClientViewerActive = false;

void clientViewerSetActive(bool active)
{
    gClientViewerActive = active;
}

bool clientViewerActive()
{
    return gClientViewerActive;
}

// Control-plane claim query, installed by f2_server (server_control.cc). Null on
// every client/probe/golden path → serverClaimantConnected() reports false.
static bool (*gClaimantQuery)() = nullptr;

void serverSetClaimantQuery(bool (*query)())
{
    gClaimantQuery = query;
}

bool serverClaimantConnected()
{
    return gClaimantQuery != nullptr && gClaimantQuery();
}

// Per-slot binding query, installed by f2_server (server_control.cc). Same
// bridge idiom as the claim query above — f2_core cannot call into the control
// plane directly, so it reads bindings through a pointer that is null on every
// client/probe/golden path (→ every slot reports UNBOUND, sessionId 0, which is
// the honest answer when there is no control plane at all).
static int (*gSlotSessionQuery)(int slot) = nullptr;

void serverSetSlotSessionQuery(int (*query)(int slot))
{
    gSlotSessionQuery = query;
}

int serverSessionForSlot(int slot)
{
    return gSlotSessionQuery != nullptr ? gSlotSessionQuery(slot) : 0;
}

// In-combat interaction executor, installed by f2_server (server_control.cc).
static bool (*gCombatInteractRunner)(Object*, int, int, int) = nullptr;

void serverSetCombatInteractRunner(bool (*runner)(Object* actor, int verb, int targetNetId, int arg))
{
    gCombatInteractRunner = runner;
}

bool serverRunCombatInteract(Object* actor, int verb, int targetNetId, int arg)
{
    return gCombatInteractRunner != nullptr
        ? gCombatInteractRunner(actor, verb, targetNetId, arg)
        : false;
}

// Open-inventory-screen query, installed by f2_server (server_control.cc).
static bool (*gSlotModalQuery)(int slot) = nullptr;

void serverSetSlotModalQuery(bool (*query)(int slot))
{
    gSlotModalQuery = query;
}

bool serverSlotInModal(int slot)
{
    return gSlotModalQuery != nullptr && gSlotModalQuery(slot);
}

// Player-initiated combat-start latch consumer, installed by f2_server
// (server_control.cc, fed by the cstart verb). Same bridge idiom as the claim
// query: server_control.cc is f2_server-only, so f2_core reaches it through a
// pointer that is null on every client/probe/golden path (→ no combat-start).
static int (*gCombatStartConsumer)() = nullptr;

void serverSetCombatStartConsumer(int (*consumer)())
{
    gCombatStartConsumer = consumer;
}

// Registry slot of the player asking to start a fight this beat, or -1 for none.
static int serverConsumePendingCombatStart()
{
    return gCombatStartConsumer != nullptr ? gCombatStartConsumer() : -1;
}

// Estimated AI-only combat presentation backlog in sim-ms, accumulated by the emit
// path (server_loop.h contract). Never read outside the resumable player barrier.
static unsigned int gPresentationCostMs = 0;

void serverAddPresentationCostMs(unsigned int ms)
{
    gPresentationCostMs += ms;
}

unsigned int serverTakePresentationCostMs()
{
    unsigned int v = gPresentationCostMs;
    gPresentationCostMs = 0;
    return v;
}

// Set once at probe entry (main.cc mainHeadlessProbe); never cleared for the
// life of the process. See server_loop.h for the contract.
static bool gHeadlessProbeActive = false;

bool headlessProbeActive()
{
    return gHeadlessProbeActive;
}

bool serverDedicatedActive()
{
    return serverLoopActive() && !headlessProbeActive();
}

void headlessProbeSetActive(bool active)
{
    gHeadlessProbeActive = active;
}

// The map-load generation the last baseline described. 0 = none emitted yet
// (mapGetLoadGeneration is >=1 once any map has loaded).
//
// Generation, NOT map index (map.h): re-entering the SAME map tears down and
// rebuilds every object — with netId 0 — while the index stays put, so an
// index-equality check silently skips the rebaseline the wire depends on.
// Measured on a same-index re-entry without this: matched=2 not_in_dump=2864.
static unsigned int gBaselineGeneration = 0;

// crc32 (IEEE 802.3, reflected) over the join blob — a fail-loud wire guard (§2).
// Table-free bitwise form; the blob is emitted at most once per baseline, so speed
// is irrelevant.
static unsigned int joinBlobCrc32(const unsigned char* data, int length)
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

// STEP-4 join blob (CLIENT_JOIN_DESIGN.md §B/§2): serialize the live map+dude via
// the save pipeline and stream it (BLOB_BEGIN/CHUNK/END) so a joining viewer loads
// it and becomes fully present. Skipped entirely unless the presenter wants it
// (null/narrate return false) → every golden path is inert. Runs from
// serverEmitBaseline, AFTER objectDeltaReset has (re)assigned the netIds, so the
// walk the client reproshapes matches this blob's object set (§C).
static void serverEmitJoinBlob()
{
    if (!presenter()->wantsSnapshotBlob()) {
        return;
    }

    const char* tmpPath = getenv("F2_JOIN_TMP");
    if (tmpPath == nullptr) {
        tmpPath = "/tmp/f2ce_join_srv.bin";
    }

    // mapSaveToStream stamps the saved bit and _map_save_file rewrites darkness /
    // elevation flags / var counts on gMapHeader — a live-state perturbation on the
    // RUNNING server (nit #6). Snapshot and restore the header around it.
    MapHeader savedHeader = gMapHeader;

    File* out = fileOpen(tmpPath, "wb");
    if (out == nullptr) {
        gMapHeader = savedHeader;
        return;
    }
    int mapBodyLen = 0;
    int rc = mapSaveToStream(out, &mapBodyLen);
    int totalLen = (int)fileTell(out);
    int mapSaveVersion = gMapHeader.version;
    fileClose(out);
    gMapHeader = savedHeader;

    if (rc == -1 || totalLen <= 0) {
        return;
    }

    File* in = fileOpen(tmpPath, "rb");
    if (in == nullptr) {
        return;
    }
    std::vector<unsigned char> blob(totalLen);
    size_t got = fileRead(blob.data(), 1, (size_t)totalLen, in);
    fileClose(in);
    if ((int)got != totalLen) {
        return;
    }

    // dudeBlobLen is the whole post-map APPENDIX (dude + any extra actors); the
    // sections self-delimit, and actorCount tells the viewer how many to read.
    presenter()->snapshotBlobBegin(mapGetCurrentMap(), gElevation,
        gDude != nullptr ? gDude->netId : 0, gameTimeGetTime(),
        mapSaveVersion, mapBodyLen, totalLen - mapBodyLen,
        joinBlobCrc32(blob.data(), totalLen), playerActorCount());

    const int kChunk = 32768; // within the u16 event-len ceiling (0xFFFB)
    for (int off = 0; off < totalLen; off += kChunk) {
        int n = totalLen - off;
        if (n > kChunk) {
            n = kChunk;
        }
        presenter()->snapshotBlobChunk(blob.data() + off, n);
    }
    presenter()->snapshotBlobEnd();
}

// Announce slot → (netId, owning session, alive) to every viewer
// (MP_PROPOSAL.md Ch 5.4). Called at the baseline tail — netIds are re-minted on
// every rebaseline, so a roster is only meaningful for the generation it rides —
// and again from the control plane whenever a binding changes.
//
// Pure read + a presenter call that is a no-op everywhere but the network
// presenter, so every golden path stays byte-identical.
void serverEmitPlayerRoster()
{
    PlayerRosterRow rows[kMaxPlayerActors];
    int rowCount = 0;

    for (int slot = 0; slot < playerActorCount() && rowCount < kMaxPlayerActors; slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            continue;
        }
        // A despawned (offline) body is off-map with a stale netId — a row for
        // it would point viewers at nothing. No row is the "vacant" signal.
        if (!playerActorOnline(slot)) {
            continue;
        }

        PlayerRosterRow& row = rows[rowCount++];
        row.slot = slot;
        row.actorNetId = actor->netId;
        row.sessionId = serverSessionForSlot(slot);
        row.alive = !critterIsDead(actor);
    }

    presenter()->playerRoster(rows, rowCount);
}

// Emit the ground-truth baseline for the CURRENT map: one snapshotObject per
// object present, bracketed by snapshotBegin/snapshotEnd (MP_PROTOCOL.md §2/§7).
// The emitted set MIRRORS the state dump exactly — the objectFindFirst/Next tile
// walk skipping OBJECT_NO_SAVE, plus gDude explicitly (it is OBJECT_NO_SAVE but
// the one such object the dump lists). Pure read + no-op under the null/client/
// narrate presenters (none override the delimiters) → byte-identical.
//
// MUST be called after objectDeltaReset() has assigned netids, never before.
static void serverEmitBaseline()
{
    gBaselineGeneration = mapGetLoadGeneration();

    // Co-op: re-derive every EXTRA player actor's vault-suit look from the world
    // MOVIE_VSUIT flag + their own gender, BEFORE the blob/snapshot below serialize
    // their bodies. This is THE choke every map switch / load / restart / join /
    // reconnect funnels through, so every baseline carries the correct fid and only
    // the host-gets-the-suit bug is fixed for all N ([[vault-suit-appearance-gap]]).
    // gDude's own look is already current here (vanilla _proto_dude_update_gender on
    // map load / restore). No-op at N==1, so goldens stay byte-identical.
    protoPlayerActorsUpdateLook();

    // STEP 4: the full-world blob rides its own frame(s) BEFORE the SNAPSHOT_OBJECT
    // baseline, which then serves as a per-object tripwire over the blob-loaded
    // world (§D). Inert unless a network consumer wants it.
    serverEmitJoinBlob();

    presenter()->snapshotBegin(mapGetCurrentMap(), gElevation);
    // Player actors first, in slot order — the same order objectAssignAllNetIds
    // numbered them and mapSaveToStream wrote them. With an empty registry this
    // is the single snapshotObject(gDude) it replaces. An OFFLINE (despawned)
    // body is skipped: it is off the map at tile -1, and a snapshot of it would
    // paint a ghost on every viewer. The join blob still carries the body (that
    // is persistence truth — a returning player's inventory lives on it), but
    // absent a snapshot the viewer never places it in the world.
    for (int slot = 0; slot < playerActorCount(); slot++) {
        if (!playerActorOnline(slot)) {
            continue;
        }
        presenter()->snapshotObject(playerActorAt(slot));
    }
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        // The NO_SAVE test already skips player actors (they carry the flag, as
        // the dude always has); !playerActorIs states the "emitted exactly once,
        // from the slot loop above" invariant in code rather than leaving it to
        // be inferred from a flag.
        if ((obj->flags & OBJECT_NO_SAVE) == 0 && !playerActorIs(obj)) {
            presenter()->snapshotObject(obj);
        }
    }
    presenter()->snapshotEnd();

    // Re-announce the map's music. EVENT_MUSIC_PLAY is a transient cue, not state
    // carried in the blob, so a viewer that joined after the track started would
    // hear nothing until the next map change — which in co-op is EVERY joiner but
    // the first. The wire is broadcast, so this reaches viewers already playing it
    // too; they de-duplicate on the track name rather than restart mid-listen
    // (backgroundSoundLoad has no "already playing" check of its own).
    wmMapMusicStart();

    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* a = playerActorAt(slot);
            fprintf(stderr, "[actors] srv slot=%d obj=%p netId=%d pid=0x%X tile=%d elev=%d flags=0x%X fid=0x%X invLen=%d\n",
                slot, (void*)a, a ? a->netId : -1, a ? a->pid : 0, a ? a->tile : -1,
                a ? a->elevation : -1, a ? a->flags : 0, a ? a->fid : 0,
                a ? a->data.inventory.length : -1);
        }
    }

    serverEmitPlayerRoster();

    // Co-op WORLD-STATE: ship the movie SEEN ledger so a late joiner's local ledger
    // matches the world's. The client's own _proto_dude_update_gender derives its
    // dude + inventory art from gameMovieIsSeen(MOVIE_VSUIT); a viewer that joined
    // after the vault-suit movie never marked it seen and rendered itself tribal on
    // its OWN screen (while others saw it correctly off the wire). No-op under the
    // null/file presenter, so goldens are unaffected. [[vault-suit-appearance-gap]]
    presenter()->movieSeenState(gameMoviesSeenData(), MOVIE_COUNT);
}

// A map LOAD wholesale-replaces the object set AND recycles every netId:
// objectDeltaScan → objectDeltaReset → objectAssignAllNetIds resets the counter to
// 1 and re-walks the new map (object.cc:4489). So after a load, every netId a
// consumer already holds means a DIFFERENT object. Keyed on the load GENERATION so a
// same-index re-entry (which rebuilds every object too) is not missed. mapTransition tells it to drop
// its world; this delivers the replacement. Skipping it does not leave the stream
// incomplete — it leaves it LYING (MP_PROTOCOL.md §7d).
//
// Detected here rather than inside objectDeltaScan so the delta tracker stays a
// pure field-diff and SESSION/baseline concerns stay owned by the server loop.
static void serverEmitBaselineIfMapChanged()
{
    if (mapGetLoadGeneration() != gBaselineGeneration) {
        serverEmitBaseline();
    }
}

// Mid-stream join (STEP 5): set by the transport's accept poll, consumed at the
// tail of the same beat. Broadcast because C.4: the netId re-walk invalidates
// every client's map, so a "targeted" blob is impossible without netIds on the
// wire (rejected sidecar) — existing viewers pay a world-reload hitch per join.
static bool gRebaselineRequested = false;

void serverRequestRebaseline()
{
    gRebaselineRequested = true;
}

void serverTick(int tick, const std::function<void(int)>& intentsDrain, bool advanceSim)
{
    if (intentsDrain) {
        intentsDrain(tick);
    }
    // FROZEN beat (persistent server, no players): the intent drain above already
    // ran — so a connection/login that arrived this beat is accepted and will
    // un-freeze the next one — but the sim itself does not move. No clock, no
    // scripts/NPCs, no id-budget burn, and no frame emit (there is nobody to send
    // it to; a joiner's rebaseline is deferred to the first live beat).
    if (!advanceSim) {
        return;
    }
    simClockAdvance(kServerTickDelta);
    _process_bk();
    scriptsHandleRequests();

    // Player-initiated combat start (cstart verb): honor the claimant's intent to
    // enter combat on the idle tick — the wire equivalent of vanilla's 'A' toggle
    // calling _combat(nullptr) (game_ui.cc). This is the ONLY safe combat-entry site
    // outside scripts/AI: right here, alongside where a SCRIPT_REQUEST_COMBAT would
    // have entered (scriptsHandleRequests just ran) and BEFORE the combatSessionActive
    // advance below, so a fight started this beat takes its first turn same-beat. We
    // always consume the latch (so it never lingers) but enter only if not already in
    // combat — if AI/a script started combat this same beat, the request is moot.
    // Wire-only: gPendingCombatStart is set exclusively by the cstart verb, so this is
    // a hard no-op on every probe/golden path (they never send it). _combat internally
    // routes to combatSessionBegin under the resumable flag, exactly like a script's.
    //
    // Latch hygiene (the one real hazard, per design review): the flag is consumed
    // UNCONDITIONALLY every tick it is set, so a cstart that raced an AI/script fight
    // (or any state below) is dropped this beat and can never resurface as a ghost
    // combat later. Entry is then refused unless it is genuinely safe: not already in
    // combat, not inside a modal driver (dialog/barter — vanilla can't toggle combat
    // mid-conversation), and the controlled actor is alive. (Actor is gDude in v1; the
    // forward-correct framing per [[mp-actor-architecture-principle]] is "the client's
    // bound actor requests combat, initiative credit to that actor" — the claimant gate
    // on the verb is a leash-era convenience, not a deep assumption.)
    int combatStartSlot = serverConsumePendingCombatStart();
    Object* combatInitiator = combatStartSlot >= 0 ? playerActorAt(combatStartSlot) : nullptr;
    if (combatStartSlot >= 0
        && !isInCombat()
        && !GameMode::isInGameMode(GameMode::kDialog | GameMode::kBarter)
        && combatInitiator != nullptr && !critterIsDead(combatInitiator)) {
        // INITIATIVE GOES TO WHOEVER PRESSED A. _combat_sequence_init places the
        // CombatStartData attacker at the head of the roster, so passing the
        // requesting actor is the whole fix for "P2 starts the fight and P1 moves
        // first" (owner-reported) — with a null csd the sequence falls through to
        // its place-the-dude branch and the host always led. The defender is
        // genuinely unknown here (this is the 'A' toggle, not an attack), and a
        // null defender is already the shape every consumer handles: the initial-
        // attack replay in the player-turn BEGIN no-ops on it.
        CombatStartData csd {};
        csd.attacker = combatInitiator;
        csd.defender = nullptr;
        _combat(&csd);
    }

    // Resumable combat (F2_SERVER_RESUMABLE_COMBAT): advance the in-flight fight
    // one beat. AFTER scriptsHandleRequests so a fight started THIS beat takes its
    // first turn(s) same-beat (mirroring legacy timing); BEFORE mapHandleTransition
    // (its branches are !isInCombat()-gated and now wait for real combat beats).
    // Inert unless a session is active — never on the client/probe/golden paths.
    if (combatSessionActive()) {
        combatSessionAdvance();
    }

    mapHandleTransition();

    // Beat resolved: emit the batched fieldwise object deltas for everything
    // that changed this tick (MP_PROTOCOL.md §6.2). Side-effect-free + no-op
    // under the null presenter, so goldens are unaffected; auto-rebaselines on
    // the map change mapHandleTransition may have just performed.
    objectDeltaScan();

    // Correctness self-check: the goldens pin determinism, not correctness.
    // Pure read on the success path (byte-identical); aborts non-zero with the
    // offending beat if a hard invariant breaks (invariants.cc).
    invariantsCheck(tick);

    // If mapHandleTransition changed maps this beat, objectDeltaScan has just
    // silently rebaselined — which RECYCLED every netId (see serverEmitBaseline).
    // Re-emit the baseline before the frame closes, so the beat that announces the
    // new world also carries it.
    bool baselineEmitted = mapGetLoadGeneration() != gBaselineGeneration;
    serverEmitBaselineIfMapChanged();

    // A viewer joined mid-stream this beat: re-walk netIds NOW (fresh tile order —
    // exactly what the joiner's blob-load walk reproduces; the live assignments
    // from the LAST reset are in stale tile order and would misalign, §C) and
    // broadcast blob + baseline. Skipped when a map-change baseline just went out:
    // that one already carried a fresh walk to everyone this beat.
    bool rebaselined = baselineEmitted;
    if (gRebaselineRequested) {
        gRebaselineRequested = false;
        if (!baselineEmitted) {
            objectDeltaReset();
            serverEmitBaseline();
            rebaselined = true;
        }
    }

    // A rebaseline re-seeds every client's world from the join blob, but the blob
    // (a save-pipeline snapshot) carries NO combat state — gCombatState and whose-
    // turn are not in it. During an active fight that leaves a mid-combat joiner
    // unaware it is combat, and it WIPES the existing viewers' combat mirror (the
    // client clears it on every reload, COMBAT_CLIENT_DESIGN.md §3.0/risk-2), which
    // DEADLOCKS the controller: its client reverts to free-roam `mv`, the server
    // rejects that mid-combat, and the turn barrier stalls until the idle timer.
    // Re-assert the combat framing through the normal presentation events so every
    // client re-derives it — no state replay needed (HP/AP/positions already rode
    // the blob + deltas), only the tiny in-combat/whose-turn framing. Emits land in
    // this same frame, after the blob, so clients apply blob then re-derive. These
    // are pure wire emits; no sim mutation. Gated on isInCombat() → only mid-fight
    // joins pay for it (combat never spans a map change, so the map-change baseline
    // never trips this).
    if (rebaselined && isInCombat()) {
        Object* cur = _combat_whose_turn();
        presenter()->combatEnter(nullptr); // re-assert the in-combat bit (initiator unused)
        if (cur != nullptr) {
            presenter()->turnStart(cur, playerActorIs(cur), cur->data.critter.combat.ap, 0);
        }
    }

    // Close the frame: the beat's accumulated events flush as one sequenced unit.
    // After invariantsCheck by design — an aborting beat emits no partial frame.
    presenter()->beatEnd(tick);
}

// Saved across serverInstall()/serverRestore() (paired, non-reentrant — the
// server loop is not nested). Previously locals of serverRun(); lifted to file
// scope so serverRun() and serverServe() share one install/restore.
static InstantAnimationScheduler gInstantScheduler;
static AnimationScheduler* gPreviousScheduler = nullptr;
static Presenter* gPreviousPresenter = nullptr;

// Install the interlocked state. The instant scheduler completes every
// terminating animation within a single _process_bk pass; the null presenter
// (base Presenter, all no-ops) drops the getTicks-gated HUD slide-in loops that
// would otherwise deadlock or crawl headless.
static void serverInstall()
{
    gPreviousScheduler = animationScheduler();
    gPreviousPresenter = presenter();

    simClockReset();
    combatIntentClear();
    dialogIntentClear();
    barterIntentClear();
    worldmapIntentClear();
    animationSchedulerSet(&gInstantScheduler);
    // Default to the null presenter (all no-ops) so goldens stay deterministic.
    // F2_NARRATE swaps in the narrating presenter, which prints the sim's
    // presentation calls (combat log, float text, errors) to stdout for a live
    // play-by-play. Off by default, so the golden gate is unaffected.
    // F2_NETSTREAM=<path> swaps in the NetworkPresenter, which serializes the sim's
    // event stream to the binary wire encoding (MP_PROTOCOL.md §2) — the dedicated
    // server's real outbound channel, and the thing tools/replay.py consumes via its
    // binary front-end. Checked BEFORE F2_NARRATE (both set = the wire wins; narrate
    // is the human-readable debug channel, the netstream is the product).
    // Off by default, so the golden gate is unaffected.
    // STEP 3: a socket sink pre-registered by f2_server (server_net.cc) wins over
    // everything — it is the dedicated server's live outbound channel, installed
    // here (before the baseline below) so a connect-at-start client is seeded.
    // presenterInstallNetworkSink is null-safe, so this is a no-op when no server
    // sink is registered (every golden run, and the F2_NETSTREAM file path).
    const char* netStreamPath = getenv("F2_NETSTREAM");
    if (presenterInstallNetworkSink(presenterServerSink())) {
        // socket wire installed
    } else if (netStreamPath != nullptr && presenterInstallNetwork(netStreamPath)) {
        // file wire installed
    } else if (getenv("F2_NARRATE") != nullptr) {
        presenterInstallNarrate();
    } else {
        presenterSet(nullptr);
    }
    gServerLoopActive = true;

    // Baseline the object-delta shadow against the freshly-loaded world (the map
    // is loaded before serverRun), so beat 0's changes diff against real initial
    // state rather than emitting the whole map as deltas. Also (re)assigns the
    // deterministic net ids that the snapshot below and the event stream carry.
    objectDeltaReset();

    // Emit the join baseline snapshot (MP_PROTOCOL.md §7): the ground-truth
    // position of every object already present, which no lifecycle event will
    // announce (the map loaded before this presenter installed). A stream consumer
    // (tools/replay.py, and later a joining network client) seeds its world from
    // these before applying the beat events. Pure read + no-op under the null
    // presenter (byte-identical); only the narrating presenter serializes it. The
    // set MIRRORS the state dump exactly: the tile-list walk skipping OBJECT_NO_SAVE
    // (objectFindFirst filters art-hidden types just like the dump walk), plus the
    // dude explicitly (it is OBJECT_NO_SAVE but the one such object the dump lists).
    // Runs at simClockNow()==0 (simClockReset above) → all lines carry [t=0].
    // Shared with the post-map-change re-emission (serverEmitBaselineIfMapChanged):
    // the join baseline and a mid-run rebaseline are the SAME mechanism, per the
    // "snapshot is ONE mechanism" cadence rule (MP_PROTOCOL.md §1).
    serverEmitBaseline(); // (re)sets gBaselineGeneration itself
}

static void serverRestore()
{
    gServerLoopActive = false;
    // Flush + close the wire before the presenter is swapped back out (no-op
    // unless F2_NETSTREAM installed one).
    presenterUninstallNetwork();
    presenterSet(gPreviousPresenter);
    animationSchedulerSet(gPreviousScheduler);
}

void serverRun(int ticks, const std::function<void(int)>& intentsDrain)
{
    serverInstall();

    for (int tick = 0; tick < ticks; tick++) {
        serverTick(tick, intentsDrain);
    }

    serverRestore();
}

void serverServe(const std::function<void(int)>& intentsDrain,
    const std::function<bool(int)>& keepServing,
    const std::function<bool()>& simGate)
{
    serverInstall();

    // Beat-paced open-ended loop: resolve one beat (serverTick), then ask the
    // caller whether to keep serving. No fixed tick bound — the loop shape is
    // the dedicated server's, ready for the network reader to feed intents and
    // a shutdown command to trip `keepServing`. The predicate runs AFTER the
    // beat, so serving ALWAYS resolves at least one beat (a server serves; this
    // is intentional, and unlike serverRun(ticks<=0)'s zero beats — the serve
    // driver only ever passes a positive safety cap). An empty predicate stops
    // after one beat (degenerate, but never spins).
    for (int tick = 0;; tick++) {
        bool advanceSim = !simGate || simGate();
        serverTick(tick, intentsDrain, advanceSim);
        if (!keepServing || !keepServing(tick)) {
            break;
        }
    }

    serverRestore();
}

} // namespace fallout
