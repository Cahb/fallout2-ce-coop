#include "server_boot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "combat.h"
#include "combat_ai.h"
#include "critter.h"
#include "debug.h"
#include "game.h"
#include "game_memory.h"
#include "inventory.h"
#include "item.h"
#include "map.h"
#include "map_defs.h"
#include "map_render.h"
#include "message.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "platform_compat.h"
#include "proto.h"
#include "queue.h"
#include "random.h"
#include "savegame.h"
#include "script_request_handler_server.h"
#include "scripts.h"
#include "server_players.h"
#include "server_script_rules.h"
#include "settings.h"
#include "sfall_arrays.h"
#include "sfall_config.h"
#include "sfall_global_scripts.h"
#include "sfall_global_vars.h"
#include "sfall_lists.h"
#include "skill.h"
#include "stat.h"
#include "art.h"
#include "tile.h"
#include "trait.h"
#include "worldmap.h"

namespace fallout {

// Synthetic object-window dimensions for the headless objectsInit() (the client
// uses the real iso window's screen size; the sim is size-agnostic — these only
// feed render-area bookkeeping the server never blits).
static const int kServerObjWidth = 640;
static const int kServerObjHeight = 480;

// Headless tile-window refresh: presentation only (the client repaints the dirty
// iso rect). The server never blits, so this is a no-op.
static void serverTileRefreshNoop(Rect* rect)
{
    (void)rect;
}

// The sim-core subset of gameInitWithOptions() (game_lifecycle.cc). Every call
// kept here is resident in f2_core; every call the client boot makes that we
// DROP is presentation bring-up the headless server has no use for:
//   presenterInstallClient / scriptRequestHandlerInstallClient (server installs
//   its own null seams in serverInstall), programWindowSetTitle, _initWindow,
//   paletteInit, keyboardSetLayout, showSplash, _debug_register_*,
//   interfaceFontsInit, fontManagerAdd/fontSetCurrent, screenshotHandlerConfigure,
//   gameSoundInit, movieInit/gameMoviesInit/movieEffectsInit, isoInit,
//   gameMouseInit, animationInit (server_anim.cc IS the runtime backend),
//   characterEditorInit, pipboyInit, _InitLoadSave/lsgInit, gameDialogInit,
//   automapInit, _init_options_menu, endgameDeathEndingInit, premadeCharactersInit.
// Returns 0 on success, -1 on a failed required init.
static int serverInitSubsystems(int argc, char** argv)
{
    if (gameMemoryInit() == -1) {
        return -1;
    }

    // sfall config first (can override the game config file name), then game
    // settings — both parse INI files (core file I/O), no SDL.
    sfallConfigInit(argc, argv);
    settingsInit(false, argc, argv);

    // Mount master.dat / patches / the data tree — the asset backing for every
    // proto/map/script/message load below.
    if (gameDbInit() == -1) {
        debugPrint("server-boot: gameDbInit failed\n");
        return -1;
    }

    messageListRepositoryInit();

    // Disable tile-grid refresh bookkeeping (there is no display to refresh).
    tileDisable();

    randomInit();
    badwordsInit();
    skillsInit();
    statsInit();

    if (partyMembersInit() != 0) {
        debugPrint("server-boot: partyMembersInit failed\n");
        return -1;
    }

    perksInit();
    traitsInit();
    itemsInit();
    queueInit();
    critterInit();
    aiInit();
    _inven_reset_dude();

    // The sim-core subset of isoInit() (map_render.cc), which the client runs
    // here (between the movie inits and protoInit) with the real iso window. We
    // supply a synthetic 640x480 buffer and a no-op refresh callback:
    //   * artInit()      — the art index (sets _art_vault_guy_num, which the
    //                      objectsInit dude-fid build needs). Must precede objects.
    //   * tileInit()     — the hex/square tile grid the whole sim addresses
    //                      space through; the refresh proc is presentation (no-op).
    //   * objectsInit()  — creates gDude/gEgg + the per-tile object lists (its
    //                      three render/text calls are benign no-ops, server_stubs).
    //   * mapSetEnteringLocation(-1,-1,-1) — the map-entry sim marker.
    // Dropped isoInit chrome: the window/rect bring-up, colorCycleInit,
    // tileScrollBlockingEnable/LimitingEnable, interfaceInit, elevatorsInit,
    // mapMakeMapsDirectory (a save-dir concern, deferred to the save pipeline).
    square_init();

    if (artInit() != 0) {
        debugPrint("server-boot: artInit failed\n");
        return -1;
    }

    static unsigned char sObjectsWindowBuffer[kServerObjWidth * kServerObjHeight];
    if (tileInit(_square, SQUARE_GRID_WIDTH, SQUARE_GRID_HEIGHT, HEX_GRID_WIDTH, HEX_GRID_HEIGHT,
            sObjectsWindowBuffer, kServerObjWidth, kServerObjHeight, kServerObjWidth,
            serverTileRefreshNoop)
        != 0) {
        debugPrint("server-boot: tileInit failed\n");
        return -1;
    }

    if (objectsInit(sObjectsWindowBuffer, kServerObjWidth, kServerObjHeight, kServerObjWidth) != 0) {
        debugPrint("server-boot: objectsInit failed\n");
        return -1;
    }

    mapSetEnteringLocation(-1, -1, -1);

    if (protoInit() != 0) {
        debugPrint("server-boot: protoInit failed\n");
        return -1;
    }

    if (scriptsInit() != 0) {
        debugPrint("server-boot: scriptsInit failed\n");
        return -1;
    }

    if (gameLoadGlobalVars() != 0) {
        debugPrint("server-boot: gameLoadGlobalVars failed\n");
        return -1;
    }

    if (_scr_game_init() != 0) {
        debugPrint("server-boot: _scr_game_init failed\n");
        return -1;
    }

    if (wmWorldMap_init() != 0) {
        debugPrint("server-boot: wmWorldMap_init failed\n");
        return -1;
    }

    if (combatInit() != 0) {
        debugPrint("server-boot: combatInit failed\n");
        return -1;
    }

    if (!messageListInit(&gMiscMessageList)) {
        debugPrint("server-boot: messageListInit failed\n");
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "misc.msg");
    if (!messageListLoad(&gMiscMessageList, path)) {
        debugPrint("server-boot: messageListLoad(misc.msg) failed\n");
        return -1;
    }

    // Scripts start disabled (enabled per-map after the load, mirroring the
    // client's scriptsDisable() at boot then scriptsEnable() in mainLoop()).
    if (scriptsDisable() != 0) {
        debugPrint("server-boot: scriptsDisable failed\n");
        return -1;
    }

    // sfall runtime state (global vars/lists/arrays + global scripts) — core,
    // needed by any map whose scripts use sfall opcodes.
    if (!sfall_gl_vars_init()) {
        debugPrint("server-boot: sfall_gl_vars_init failed\n");
        return -1;
    }
    if (!sfallListsInit()) {
        debugPrint("server-boot: sfallListsInit failed\n");
        return -1;
    }
    if (!sfallArraysInit()) {
        debugPrint("server-boot: sfallArraysInit failed\n");
        return -1;
    }
    if (!sfall_gl_scr_init()) {
        debugPrint("server-boot: sfall_gl_scr_init failed\n");
        return -1;
    }

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_MISC, &gMiscMessageList);

    return 0;
}

// The sim-core subset of the client's _main_load_new() (game_lifecycle.cc /
// main.cc): the state changes that bring a freshly-created dude onto a loaded
// map. Drops the window/palette/mouse chrome (windowCreate/paletteFadeTo/
// mouseHideCursor/gameMouseSetCursor) — all presentation.
//
// wmMapMusicStart is NO LONGER dropped: it used to be pure client audio, but it
// is now a sim-side decision the server puts on the wire, and mapLoadByName only
// starts music for a map with a .SAV (mapLoadById does it for transitions). Skip
// it and the FIRST map of a session is silent while every later one plays.
static int serverLoadMap(const char* mapName)
{
    _game_user_wants_to_quit = 0;

    gDude->flags &= ~OBJECT_FLAT;
    objectShow(gDude, nullptr);

    _map_init();

    char* mapNameCopy = compat_strdup(mapName);
    int rc = mapLoadByName(mapNameCopy);
    free(mapNameCopy);

    if (rc != -1) {
        wmMapMusicStart();
    }

    return rc;
}

// First unblocked tile in a growing ring around `center` on `elevation`, or -1.
// Same primitives the AI flee path uses (tileGetTileInDirection + the blocking
// probe); deliberately conservative — a spawn that lands inside a wall would
// leave an actor unable to move, which is worse than failing the boot.
static int serverFindFreeTileNear(int center, int elevation)
{
    for (int dist = 1; dist <= 6; dist++) {
        for (int rot = 0; rot < ROTATION_COUNT; rot++) {
            int tile = tileGetTileInDirection(center, rot, dist);
            if (tile == -1 || tile == center) {
                continue;
            }

            if (_obj_blocking_at(nullptr, tile, elevation) == nullptr) {
                return tile;
            }
        }
    }

    return -1;
}

// All six sheet classes for ONE slot, in the same order the bulk seeders run.
// ⚠ SEED ALL SIX OR THE ACTOR IS A CHIMERA (PLAYER_SHEET_DESIGN.md stage 2):
// seeding skills but not level is not cosmetic — the level a player stands at
// drives the next level-up's HP award.
void playerActorSeedSheetFromHost(int slot)
{
    if (slot < 1 || slot >= kMaxPlayerActors) {
        return;
    }

    protoPlayerActorSheetSeedSlot(slot);
    perkPlayerActorSeedRanksSlot(slot);
    pcPlayerActorSeedStatsSlot(slot);
    traitsPlayerActorSeedSlot(slot);
    skillsPlayerActorSeedSlot(slot);
    critterPlayerActorSeedNameSlot(slot);
}

// Spawn ONE extra player actor beside the host into `slot` and register it.
// Returns the registered slot, or -1.
//
// THE one spawn path: boot (serverSpawnExtraActors, below) and spawn-at-login
// (the control plane's pending-login drain) must produce actors that are
// indistinguishable, or a player who joined dynamically differs from one the
// server pre-spawned in ways nothing tests. ACCOUNT_IDENTITY_DESIGN.md §3.
//
// ⚠ The SHEET is seeded per-slot by the CALLER before this runs, not here: the
// bulk seeders rewrite every extra's row from the host and would wipe the
// progression of players already connected (trap 1).
int serverSpawnPlayerActor(int slot)
{
    if (slot < 1 || slot >= kMaxPlayerActors) {
        return -1;
    }

    Object* extra = nullptr;
    if (_obj_copy(&extra, gDude) == -1 || extra == nullptr) {
        debugPrint("server-boot: extra actor %d copy failed\n", slot);
        return -1;
    }

    // _obj_copy memcpy'd the dude's flags, so the lifecycle class comes
    // along; assert it rather than inherit it silently, and make sure the
    // copy is visible (the dude is objectShow'n before the map load).
    extra->flags |= (OBJECT_NO_SAVE | OBJECT_NO_REMOVE);
    extra->flags &= ~OBJECT_HIDDEN;

    // _obj_copy mints a fresh sid from the proto. The dude proto's script
    // slot is the PC script bound by scriptsSetDudeScript — a second live
    // instance of it is an untested aliasing hazard, and extras have no need
    // for one. Scriptless, like most critters.
    if (extra->sid != -1) {
        scriptRemove(extra->sid);
        extra->sid = -1;
    }

    // _obj_copy inserted the copy at the SOURCE's tile (it never places),
    // so without this every actor stands inside the host.
    int tile = serverFindFreeTileNear(gDude->tile, gDude->elevation);
    if (tile == -1) {
        debugPrint("server-boot: no free tile near the host for actor %d\n", slot);
        return -1;
    }

    objectSetLocation(extra, tile, gDude->elevation, nullptr);
    objectSetRotation(extra, gDude->rotation, nullptr);

    // Point the actor at its own sheet. This is the whole observable change
    // of stage 2: obj_pid() on an extra no longer equals the dude pid, and
    // set_critter_stat / skill writes on one actor stop bleeding to all.
    //
    // LAST, after the copy and the placement — _obj_copy took the dude's pid
    // along with everything else, and until this line the actor is a literal
    // second host as far as every pid-keyed lookup is concerned.
    extra->pid = playerActorSheetPid(slot);

    int got = playerActorRegister(extra);
    if (got != slot) {
        debugPrint("server-boot: actor %d registration failed (got %d)\n", slot, got);
        return -1;
    }

    return got;
}

// Spawn F2_SERVER_PLAYERS-1 extra player actors beside the host and register
// them (MP_PROPOSAL.md Ch 4). Unset/1 spawns nothing and leaves the registry as
// { gDude } — the whole co-op feature is dark behind this env var.
//
// Extras are DUDE-CLASS objects (NO_SAVE | NO_REMOVE), not plain world critters:
// map teardown spares them (so the registry's raw Object* stay valid across
// transitions) and objectSaveAll skips them (so they never duplicate into a map
// .SAV). Both properties are exactly why the dude carries those flags.
static int serverSpawnExtraActors()
{
    int want = 1;
    const char* value = getenv("F2_SERVER_PLAYERS");
    if (value != nullptr) {
        want = atoi(value);
    }
    if (want < 1) {
        want = 1;
    }
    if (want > kMaxPlayerActors) {
        debugPrint("server-boot: F2_SERVER_PLAYERS=%d clamped to %d\n", want, kMaxPlayerActors);
        want = kMaxPlayerActors;
    }

    // Give the extras their own sheet rows BEFORE any of them exists, so no
    // window opens in which an actor carries a sheet pid that resolves to an
    // unseeded row (PLAYER_SHEET_DESIGN.md stage 2). Seeding is from the host's
    // sheet, which by now is the loaded .gcd — _proto_dude_init ran at the top
    // of serverBoot, and perksInit/perkResetRanks with it.
    protoPlayerActorSheetsSeed();
    perkPlayerActorSeedRanks();
    pcPlayerActorSeedStats();
    traitsPlayerActorSeed();
    skillsPlayerActorSeed();
    critterPlayerActorSeedNames();

    for (int slot = 1; slot < want; slot++) {
        if (serverSpawnPlayerActor(slot) != slot) {
            return -1;
        }
    }

    if (want > 1) {
        // "Which sheet does this actor read" is the one question the stage-2
        // failure mode turns on — a slot still holding the dude pid here is the
        // silent host-corruption case, not a crash — so it goes on the record at
        // boot. A NEW line, never folded into the one below: gate scripts match
        // these by prefix.
        //
        // fprintf and not debugPrint: f2_server drops _debug_register_* as client
        // chrome (see serverInitSubsystems), so with no print proc registered and
        // NDEBUG set, every debugPrint in this file is inert. The stderr lines in
        // server_main.cc are what actually reaches an operator.
        for (int slot = 1; slot < want; slot++) {
            fprintf(stderr, "f2_server: actor %d sheet pid 0x%X\n", slot, playerActorAt(slot)->pid);
        }
        debugPrint("server-boot: %d player actors registered\n", want);
    }

    return 0;
}

int serverBootSubsystems(int argc, char** argv)
{
    return serverInitSubsystems(argc, argv) != 0 ? -1 : 0;
}

int serverBootNewWorld(const char* mapName)
{
    // Create the authoritative dude exactly as the character selector's "Take"
    // does (mainHeadlessProbe uses the same premade). This must precede the map
    // load: serverLoadMap references gDude.
    char gcdPath[COMPAT_MAX_PATH];
    strcpy(gcdPath, "premade\\combat.gcd");
    if (_proto_dude_init(gcdPath) == -1) {
        debugPrint("server-boot: dude init failed\n");
        return -1;
    }

    const char* seedValue = getenv("F2_SERVER_SEED");
    randomSeedPrerandom(seedValue != nullptr ? atoi(seedValue) : 1337);

    if (serverLoadMap(mapName) == -1) {
        debugPrint("server-boot: map '%s' load failed\n", mapName);
        return -1;
    }

    return 0;
}

int serverBootLoadSlot(int slot)
{
    // The dude still has to exist before the load: lsgLoadGameInSlot's handler
    // table writes THROUGH gDude (_obj_load_dude) rather than creating it, and
    // slot 0 of the sheet registry IS gDudeProto. The premade seed is overwritten
    // wholesale by the save, so which premade it was does not survive — it is
    // scaffolding for the load to land on, not the loaded character.
    char gcdPath[COMPAT_MAX_PATH];
    strcpy(gcdPath, "premade\\combat.gcd");
    if (_proto_dude_init(gcdPath) == -1) {
        debugPrint("server-boot: dude init failed\n");
        return -1;
    }

    const char* seedValue = getenv("F2_SERVER_SEED");
    randomSeedPrerandom(seedValue != nullptr ? atoi(seedValue) : 1337);

    _map_init();

    savegameRefreshPatchesPath();
    savegameSetSlot(slot);
    if (lsgLoadGameInSlot(slot) == -1) {
        debugPrint("server-boot: load of slot %d failed (error %d)\n", slot + 1, savegameGetErrorCode());
        return -1;
    }

    return 0;
}

int serverBootFinish(bool spawnExtras)
{
    // Slot 0 of the player-actor registry is the host actor (MP_PROPOSAL.md
    // Ch 3/Ch 4). Registering explicitly — even at N == 1, where the registry
    // would resolve to gDude anyway — is what makes the server path stop
    // depending on the empty-registry fallback, so extra actors (M1) can be
    // appended in slot order with nothing else to change.
    playerActorRegister(gDude);

    // Extras spawn AFTER the map load (serverLoadMap places the host) and after
    // map-enter procs, which run inside mapLoad — so map scripts never observe
    // them on a first load. Acceptable: they do not observe the dude's final
    // position either (MP_PROPOSAL.md Ch 4.4).
    //
    // Skip when the registry is ALREADY populated: a co-op disk load reconstructs
    // its extras from the save's tail appendix (playerActorAppendixLoad, inside
    // lsgLoadGameInSlot), so slots 1..N already exist here and cloning a fresh set
    // on top would duplicate them. On a new world — and on a load of a legacy
    // single-player save, which carries no appendix — only slot 0 exists, so this
    // spawns the F2_SERVER_PLAYERS party as before.
    if (spawnExtras && playerActorCount() <= 1 && serverSpawnExtraActors() != 0) {
        return -1;
    }

    // Route SCRIPT_REQUEST_DIALOG to the authoritative dialog engine (A1). The
    // shared serverInstall() never sets a request handler, so this f2_server-only
    // install persists across the serve loop; fallout2-ce's F2_SERVER_LOOP golden
    // path is unaffected (it keeps its own ClientScriptRequestHandler from main()).
    scriptRequestHandlerInstallServer();

    // Co-op script RULES, as taps on the interpreter's opcode hook seam. Server
    // only — policy never goes in the interpreter, and no other binary installs
    // these, so client/probe/golden script behavior is untouched.
    serverScriptRulesInstall();

    // Player bodies other than the host start PARKED (offline). A persisted co-op
    // save reconstructs every extra body, and a fresh F2_SERVER_PLAYERS party
    // pre-spawns them — but at boot NOBODY is driving them, so without this they
    // stand in the world as ghost bodies and get leash-dragged across map
    // transitions (owner-reported: load a slot and P2's body is there, dragged
    // around, before P2 has joined). Disconnect each and mark it offline; the
    // presence drain reattaches it the beat its owner logs in
    // (serverControlDrainPresence), exactly like return-from-leave. Slot 0 is the
    // host anchor and never parks. A lone host (N==1, the golden/probe path) has no
    // extras, so this is a no-op there.
    for (int slot = 1; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor != nullptr && playerActorOnline(slot)) {
            _obj_disconnect(actor, nullptr);
            playerActorSetOnline(slot, false);
        }
    }

    // Scripts run for the loaded map (the serve loop drives them per beat).
    scriptsEnable();

    return 0;
}

int serverBoot(const char* mapName, int argc, char** argv)
{
    if (serverBootSubsystems(argc, argv) != 0) {
        return -1;
    }

    if (serverBootNewWorld(mapName) != 0) {
        return -1;
    }

    if (serverBootFinish(true) != 0) {
        return -1;
    }

    debugPrint("server-boot: map '%s' loaded, ready to serve\n", mapName);
    return 0;
}

void serverShutdown()
{
    // Minimal teardown: drop the loaded map's transient state. Full symmetric
    // exit (queueExit/protoReset/…/gameDbExit) is deferred — a v1 server exits
    // the process at shutdown, so the OS reclaims everything. Kept as a named
    // seam for when a listen-server needs a clean in-process teardown.
    _map_exit();
}

} // namespace fallout
