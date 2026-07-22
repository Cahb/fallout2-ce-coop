#include "main.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "actions.h"
#include "skilldex.h"
#include "animation.h"
#include "art.h"
#include "autorun.h"
#include "barter_intent.h"
#include "character_editor.h" // characterEditorShow — the creation screen, reused for co-op joins
#include "character_selector.h"
#include "character_transaction.h"
#include "client_present.h"
#include "client_dialog.h"
#include "client_barter.h"
#include "client_net.h"
#include "combat.h" // _combat_attack_this + combatSetViewerAttackHook (§3.b crosshair attack)
#include "color.h"
#include "combat_intent.h"
#include "command.h"
#include "credits.h"
#include "critter.h"
#include "cycle.h"
#include "db.h"
#include "debug.h"
#include "dialog_intent.h"
#include "draw.h"
#include "endgame.h"
#include "game.h"
#include "game_dialog.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "input.h"
#include "input_replay.h"
#include "interface.h" // weapon-slot hit-mode cycle / hand swap (viewer attack UX)
#include "inventory.h" // inventoryOpen — viewer inventory screen (player-UI Slice 3)
#include "item.h"
#include "kb.h"
#include "loadsave.h"
#include "mainmenu.h"
#include "map.h"
#include "mouse.h"
#include "object.h"
#include "palette.h"
#include "party_member.h"
#include "perk.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "preferences.h"
#include "proto.h"
#include "proto_instance.h"
#include "random.h"
#include "stat.h"
#include "scripts.h"
#include "server_loop.h"
#include "settings.h"
#include "sim_clock.h"
#include "skill.h"
#include "sfall_config.h"
#include "sfall_global_scripts.h"
#include "state_dump.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "trait.h"
#include "window.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "word_wrap.h"
#include "worldmap.h"
#include "worldmap_ui.h"

namespace fallout {

#define DEATH_WINDOW_WIDTH 640
#define DEATH_WINDOW_HEIGHT 480

static bool falloutInit(int argc, char** argv);
static int main_reset_system();
static void main_exit_system();
static int _main_load_new(char* fname);
static int main_loadgame_new();
static void main_unload_new();
static void mainLoop();
static void showDeath();
static int mainHeadlessProbe();
static int mainClientViewer(const char* connectSpec);
static void _main_death_voiceover_callback();
static int _mainDeathGrabTextFile(const char* fileName, char* dest);
static int _mainDeathWordWrap(char* text, int width, short* beginnings, short* count);

// 0x5194C8
static char _mainMap[] = "artemple.map";

// 0x5194D8
static int _main_game_paused = 0;

// 0x5194E8
static bool _main_show_death_scene = false;

// 0x614838
static bool _main_death_voiceover_done;

// 0x48099C
int falloutMain(int argc, char** argv)
{
    if (!autorunMutexCreate()) {
        return 1;
    }

    if (!falloutInit(argc, argv)) {
        return 1;
    }

    // Phase 0 headless probe (REWRITE_PLAN.md): boot straight into a map,
    // tick the simulation, dump canonical state, exit. Bypasses movies,
    // main menu and character selector entirely.
    if (getenv("F2_HEADLESS_PROBE") != nullptr) {
        int probeRc = mainHeadlessProbe();
        main_exit_system();
        autorunMutexClose();
        return probeRc;
    }

    // STEP-4 live viewer (CLIENT_JOIN_DESIGN.md §A/§E): F2_CLIENT_CONNECT=host:port
    // connects to a running f2_server, receives the binary join snapshot, becomes
    // present on the server's map, and renders the live wire stream as a read-only
    // viewer. Bypasses the menu/character selector like the headless probe.
    if (const char* connectSpec = getenv("F2_CLIENT_CONNECT")) {
        int viewerRc = mainClientViewer(connectSpec);
        main_exit_system();
        autorunMutexClose();
        return viewerRc;
    }

    // SFALL: Allow to skip intro movies
    int skipOpeningMovies;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_SKIP_OPENING_MOVIES_KEY, &skipOpeningMovies);
    if (skipOpeningMovies < 1) {
        gameMoviePlay(MOVIE_IPLOGO, GAME_MOVIE_FADE_IN);
        gameMoviePlay(MOVIE_INTRO, 0);
        gameMoviePlay(MOVIE_CREDITS, 0);
    }

    if (mainMenuWindowInit() == 0) {
        bool done = false;
        while (!done) {
            keyboardReset();
            _gsound_background_play_level_music("07desert", 11);
            mainMenuWindowUnhide(1);

            mouseShowCursor();
            int mainMenuRc = mainMenuWindowHandleEvents();
            mouseHideCursor();

            switch (mainMenuRc) {
            case MAIN_MENU_INTRO:
                mainMenuWindowHide(true);
                gameMoviePlay(MOVIE_INTRO, GAME_MOVIE_STOP_MUSIC);
                gameMoviePlay(MOVIE_CREDITS, 0);
                break;
            case MAIN_MENU_NEW_GAME:
                mainMenuWindowHide(true);
                mainMenuWindowFree();
                if (characterSelectorOpen() == 2) {
                    gameMoviePlay(MOVIE_ELDER, GAME_MOVIE_STOP_MUSIC);
                    randomSeedPrerandom(-1);

                    // SFALL: Override starting map.
                    char* mapName = nullptr;
                    if (configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_STARTING_MAP_KEY, &mapName)) {
                        if (*mapName == '\0') {
                            mapName = nullptr;
                        }
                    }

                    char* mapNameCopy = compat_strdup(mapName != nullptr ? mapName : _mainMap);
                    _main_load_new(mapNameCopy);
                    free(mapNameCopy);

                    // SFALL: AfterNewGameStartHook.
                    sfall_gl_scr_exec_start_proc();

                    mainLoop();
                    paletteFadeTo(gPaletteWhite);

                    // NOTE: Uninline.
                    main_unload_new();

                    // NOTE: Uninline.
                    main_reset_system();

                    if (_main_show_death_scene != 0) {
                        showDeath();
                        _main_show_death_scene = 0;
                    }
                }

                mainMenuWindowInit();

                break;
            case MAIN_MENU_LOAD_GAME:
                if (1) {
                    int win = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), _colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
                    mainMenuWindowHide(true);
                    mainMenuWindowFree();

                    // NOTE: Uninline.
                    main_loadgame_new();

                    colorPaletteLoad("color.pal");
                    paletteFadeTo(_cmap);
                    int loadGameRc = lsgLoadGame(LOAD_SAVE_MODE_FROM_MAIN_MENU);
                    if (loadGameRc == -1) {
                        debugPrint("\n ** Error running LoadGame()! **\n");
                    } else if (loadGameRc != 0) {
                        windowDestroy(win);
                        win = -1;
                        mainLoop();
                    }
                    paletteFadeTo(gPaletteWhite);
                    if (win != -1) {
                        windowDestroy(win);
                    }

                    // NOTE: Uninline.
                    main_unload_new();

                    // NOTE: Uninline.
                    main_reset_system();

                    if (_main_show_death_scene != 0) {
                        showDeath();
                        _main_show_death_scene = 0;
                    }
                    mainMenuWindowInit();
                }
                break;
            case MAIN_MENU_TIMEOUT:
                debugPrint("Main menu timed-out\n");
                // FALLTHROUGH
            case MAIN_MENU_SCREENSAVER:
                mainMenuWindowHide(true);
                gameMoviePlay(MOVIE_INTRO, GAME_MOVIE_PAUSE_MUSIC);
                break;
            case MAIN_MENU_OPTIONS:
                mainMenuWindowHide(true);
                doPreferences(true);
                break;
            case MAIN_MENU_CREDITS:
                mainMenuWindowHide(true);
                creditsOpen("credits.txt", -1, false);
                break;
            case MAIN_MENU_QUOTES:
                // NOTE: There is a strange cmp at 0x480C50. Both operands are
                // zero, set before the loop and do not modify afterwards. For
                // clarity this condition is omitted.
                mainMenuWindowHide(true);
                creditsOpen("quotes.txt", -1, true);
                break;
            case MAIN_MENU_EXIT:
            case -1:
                done = true;
                mainMenuWindowHide(true);
                mainMenuWindowFree();
                backgroundSoundDelete();
                break;
            case MAIN_MENU_SELFRUN:
                break;
            }
        }
    }

    // NOTE: Uninline.
    main_exit_system();

    autorunMutexClose();

    return 0;
}

// 0x480CC0
static bool falloutInit(int argc, char** argv)
{
    if (gameInitWithOptions("FALLOUT II", false, 0, 0, argc, argv) == -1) {
        return false;
    }

    return true;
}

// NOTE: Inlined.
//
// 0x480D0C
static int main_reset_system()
{
    gameReset();

    return 1;
}

// NOTE: Inlined.
//
// 0x480D18
static void main_exit_system()
{
    backgroundSoundDelete();

    gameExit();
}

// 0x480D4C
static int _main_load_new(char* mapFileName)
{
    _game_user_wants_to_quit = 0;
    _main_show_death_scene = 0;
    gDude->flags &= ~OBJECT_FLAT;
    objectShow(gDude, nullptr);
    mouseHideCursor();

    int win = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), _colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    windowRefresh(win);

    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);
    _map_init();
    gameMouseSetCursor(MOUSE_CURSOR_NONE);
    mouseShowCursor();
    mapLoadByName(mapFileName);
    wmMapMusicStart();
    paletteFadeTo(gPaletteWhite);
    windowDestroy(win);
    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);
    return 0;
}

// NOTE: Inlined.
//
// 0x480DF8
static int main_loadgame_new()
{
    _game_user_wants_to_quit = 0;
    _main_show_death_scene = 0;

    gDude->flags &= ~OBJECT_FLAT;

    objectShow(gDude, nullptr);
    mouseHideCursor();

    _map_init();

    gameMouseSetCursor(MOUSE_CURSOR_NONE);
    mouseShowCursor();

    return 0;
}

// 0x480E34
static void main_unload_new()
{
    objectHide(gDude, nullptr);
    _map_exit();
}

// 0x480E48
static void mainLoop()
{
    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    _main_game_paused = 0;

    scriptsEnable();

    while (_game_user_wants_to_quit == 0) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        // SFALL: MainLoopHook.
        sfall_gl_scr_process_main();

        gameHandleKey(keyCode, false);

        scriptsHandleRequests();

        mapHandleTransition();

        if (_main_game_paused != 0) {
            _main_game_paused = 0;
        }

        if ((gDude->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
            endgameSetupDeathEnding(ENDGAME_DEATH_ENDING_REASON_DEATH);
            _main_show_death_scene = 1;
            _game_user_wants_to_quit = 2;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    scriptsDisable();

    if (cursorWasHidden) {
        mouseHideCursor();
    }
}

// Phase 0 probe: new game with a premade character on a given map, N sim
// iterations of the same pump mainLoop() uses (minus input/render), then a
// canonical state dump. Controlled via env vars:
//   F2_PROBE_MAP   map file name (default "artemple.map")
//   F2_PROBE_TICKS loop iterations (default 100)
//   F2_PROBE_DUMP  dump file path (default "state_dump.txt")
//   F2_PROBE_SEED  RNG seed (default 1337)
static int mainHeadlessProbe()
{
    // Mark the whole probe run headless so modal UI drivers (e.g. the pipboy
    // rest loop) skip window/render/real-time pacing on BOTH the legacy
    // F2_PROBE_ACTIONS pump and the F2_SERVER_LOOP path.
    headlessProbeSetActive(true);

    const char* mapName = getenv("F2_PROBE_MAP");
    if (mapName == nullptr) {
        mapName = "artemple.map";
    }

    const char* ticksValue = getenv("F2_PROBE_TICKS");
    int ticks = ticksValue != nullptr ? atoi(ticksValue) : 100;

    const char* dumpPath = getenv("F2_PROBE_DUMP");
    if (dumpPath == nullptr) {
        dumpPath = "state_dump.txt";
    }

    const char* seedValue = getenv("F2_PROBE_SEED");
    int seed = seedValue != nullptr ? atoi(seedValue) : 1337;

    // Same dude setup the character selector performs on "Take".
    char gcdPath[COMPAT_MAX_PATH];
    strcpy(gcdPath, "premade\\combat.gcd");
    if (_proto_dude_init(gcdPath) == -1) {
        debugPrint("headless-probe: dude init failed\n");
        return 1;
    }

    randomSeedPrerandom(seed);

    char* mapNameCopy = compat_strdup(mapName);
    _main_load_new(mapNameCopy);
    free(mapNameCopy);

    debugPrint("headless-probe: map '%s' loaded, running %d ticks (pump iteration %u)\n",
        mapName, ticks, inputReplayGetIteration());

    // F2_PROBE_AGGRO=N: make the N critters nearest to the dude hostile and
    // request combat — a deterministic combat fixture exercising turn order,
    // AI, to-hit, damage and death without any UI-coordinate scripting.
    // Dude turns must be ended by the input trace (SPACE key events).
    const char* aggroValue = getenv("F2_PROBE_AGGRO");
    if (aggroValue != nullptr) {
        probeApplyAggro(atoi(aggroValue));
    }

    // F2_PROBE_LISTMAPS=1: print the worldmap's map index -> file name table
    // and exit (helper for authoring golden cases).
    if (getenv("F2_PROBE_LISTMAPS") != nullptr) {
        char mapName16[32];
        for (int mapIdx = 0; mapIdx < wmMapMaxCount(); mapIdx++) {
            if (wmMapIdxToName(mapIdx, mapName16, sizeof(mapName16)) == 0) {
                debugPrint("headless-probe: map %d = %s\n", mapIdx, mapName16);
            }
        }
        return 0;
    }

    // F2_PROBE_ACTIONS="tick:action:arg,..." — invoke core entry points at
    // scheduled loop iterations so golden traces cover sim paths that have
    // no UI-free trigger (coverage-before-conversion rule, WORKLIST_P1.md).
    // Actions: xp:N, rad:N, poison:N, drug:PID, hurt:N, useskill:SKILL,
    // useskillon:SKILL (use skill on nearest wounded critter),
    // usedoor:0, give:PID, drop:PID, usedrug:PID, useitem:PID, unload:PID,
    // wmtravel:XXXXYYYY (worldmap travel to destX*10000+destY),
    // rest:N (rest N game minutes), restopt:N (rest-menu option N).
    struct ProbeAction {
        int tick;
        char name[16];
        int arg;
        // Optional 4th field (tick:name:arg:arg2). Barter verbs use it as the
        // item quantity; -1 means "unspecified" (the drain moves a full stack).
        int arg2;
    };
    std::vector<ProbeAction> probeActions;

    const char* actionsValue = getenv("F2_PROBE_ACTIONS");
    if (actionsValue != nullptr) {
        char* copy = compat_strdup(actionsValue);
        for (char* token = strtok(copy, ","); token != nullptr; token = strtok(nullptr, ",")) {
            ProbeAction action;
            action.arg2 = -1;
            if (sscanf(token, "%d:%15[^:]:%d:%d", &(action.tick), action.name, &(action.arg), &(action.arg2)) >= 3) {
                probeActions.push_back(action);
            } else {
                debugPrint("headless-probe: bad action '%s'\n", token);
            }
        }
        free(copy);
    }

    scriptsEnable();

    // Apply the probe actions scheduled for tick `i` by calling core entry
    // points directly (no key/mouse events). Shared by the legacy pump loop and
    // the server loop, where it IS the intent-drain step (SERVER_LOOP_DESIGN.md
    // §4: F2_PROBE_ACTIONS is the tick-indexed intent queue v0).
    auto applyProbeActions = [&](int i) {
        for (const ProbeAction& action : probeActions) {
            if (action.tick == i) {
                // Reify the scheduled action as a Command and dispatch it
                // through the unified handler (command.{h,cc}) — the single
                // choke point that later becomes the network command reader.
                Command command;
                command.tick = i;
                strncpy(command.name, action.name, sizeof(command.name));
                command.arg = action.arg;
                command.arg2 = action.arg2;
                commandDispatch(command);
            }
        }
    };

    // STEP-4 join-snapshot round-trip modes (CLIENT_JOIN_DESIGN.md build slice
    // S1). Both short-circuit the tick loop — they exercise the blob save/load
    // pipeline, not the sim — and dump with netid= (state_dump gate) so the
    // harness can assert the §C walk reproduces IDENTICAL netIds on both sides.
    if (const char* blobOut = getenv("F2_SERVER_BLOB_OUT")) {
        // Producer ("server truth"): the map is loaded; number the syncable set
        // (§C), then serialize [map body][dude] and dump the resulting state.
        objectAssignAllNetIds();
        File* stream = fileOpen(blobOut, "wb");
        if (stream == nullptr || mapSaveToStream(stream) == -1) {
            debugPrint("headless-probe: blob write to '%s' FAILED\n", blobOut);
            if (stream != nullptr) {
                fileClose(stream);
            }
            return 1;
        }
        fileClose(stream);
        if (!stateDumpWrite(dumpPath)) {
            return 1;
        }
        main_unload_new();
        return 0;
    }
    if (const char* blobIn = getenv("F2_CLIENT_BLOB_IN")) {
        // Loader ("client"): drop the throwaway probe map and become present on
        // the blob's map. Null the current map name so the inner mapLoad's
        // _map_save_in_game early-returns (no .SAV write / exit procs on the
        // probe map — B4). S4: adopt the server clock before load so map-enter
        // procs read it, not the client's.
        if (const char* blobTime = getenv("F2_CLIENT_BLOB_TIME")) {
            gameTimeSetTime((unsigned int)strtoul(blobTime, nullptr, 10));
        }
        gMapHeader.name[0] = '\0';
        File* stream = fileOpen(blobIn, "rb");
        if (stream == nullptr) {
            debugPrint("headless-probe: blob open '%s' FAILED\n", blobIn);
            return 1;
        }
        mapSetViewerLoad(true); // §E: no map-enter procs — the viewer is a puppet
        int loadRc = mapLoad(stream);
        mapSetViewerLoad(false);
        if (loadRc == -1) {
            debugPrint("headless-probe: blob map load FAILED\n");
            fileClose(stream); // mapLoad does not close on error (err: label)
            return 1;
        }
        // Stream is positioned exactly at the dude blob (mapLoad reads only the
        // map body; the caller owns the handle — map.cc:602). Apply the dude and
        // repoint gDude (B1). _obj_load_dude closes the stream on its OWN error
        // paths, so only close here on success.
        if (_obj_load_dude(stream) == -1) {
            debugPrint("headless-probe: dude blob load FAILED\n");
            return 1;
        }
        fileClose(stream);
        // The saved-bit blob skips mapLoad's first-run _map_fix_critter_combat_data;
        // run it so dangling whoHitMe back-pointers are nulled (matches the
        // producer, whose first-run load nulled them). No active combat on a
        // fresh join → every whoHitMeCid is -1 → all nulled.
        _map_fix_critter_combat_data();
        objectAssignAllNetIds(); // §C: reproduce the walk → identical netIds
        scriptsDisable();        // S1: the viewer is a puppet, sim stays frozen
        if (!stateDumpWrite(dumpPath)) {
            return 1;
        }
        main_unload_new();
        return 0;
    }

    if (const char* streamIn = getenv("F2_CLIENT_STREAM_IN")) {
        // S2 headless joining client (CLIENT_JOIN_DESIGN.md §D): decode a captured
        // F2NS wire — load the blob, apply the live event stream — then dump the
        // reconstructed state. The blob-load path already dumps with netid=.
        const char* blobTmp = getenv("F2_JOIN_TMP_CLIENT");
        if (blobTmp == nullptr) {
            blobTmp = "/tmp/f2ce_join_cli.bin";
        }
        if (!clientApplyStreamFile(streamIn, blobTmp)) {
            debugPrint("headless-probe: stream apply from '%s' FAILED\n", streamIn);
            return 1;
        }
        if (!stateDumpWrite(dumpPath)) {
            return 1;
        }
        main_unload_new();
        return 0;
    }

    // F2_SERVER_LOOP=1: run the fixed-timestep server loop
    // (SERVER_LOOP_DESIGN.md) instead of the legacy frame-driven pump. The
    // server loop installs the sim clock + InstantAnimationScheduler +
    // NullPresenter + movie guard + combat auto-end-turn as one interlocked
    // whole; state dumps legitimately shift (game_time / queue timestamps / RNG
    // cadence) and are re-blessed against the semantic-diff gate. The legacy
    // path stays the default and is byte-identical to before.
    if (getenv("F2_SERVER_SERVE") != nullptr) {
        // P5-A serverServe() smoke: the same interlocked server loop as
        // F2_SERVER_LOOP, but driven by the OPEN-ENDED serve loop instead of a
        // fixed tick count. The command queue (applyProbeActions) is fed
        // identically; the stop condition stands in for a real server's
        // "shutdown command / no clients": keep serving until the game requests
        // quit (e.g. the endgame verb sets _game_user_wants_to_quit), with
        // `ticks` retained as a hard safety cap so a headless run can't hang.
        // For any non-quitting case this drains the same beats as serverRun, so
        // its dump matches the F2_SERVER_LOOP golden; the endgame case shuts
        // down early on the quit signal — the server-shaped path's smoke test.
        serverServe(applyProbeActions, [&](int tick) {
            if (_game_user_wants_to_quit != 0) {
                return false;
            }
            return tick + 1 < ticks;
        });
    } else if (getenv("F2_SERVER_LOOP") != nullptr) {
        serverRun(ticks, applyProbeActions);
    } else {
        for (int i = 0; i < ticks; i++) {
            applyProbeActions(i);

            // mainLoop() pump minus rendering. gameHandleKey also dispatches
            // mouse events (event code -2) — required for replayed clicks.
            int keyCode = inputGetInput();
            gameHandleKey(keyCode, false);
            scriptsHandleRequests();
            mapHandleTransition();
        }
    }
    scriptsDisable();

    if (!stateDumpWrite(dumpPath)) {
        debugPrint("headless-probe: state dump to '%s' FAILED\n", dumpPath);
        return 1;
    }
    debugPrint("headless-probe: state dumped to '%s'\n", dumpPath);

    // NOTE: Uninline.
    main_unload_new();

    return 0;
}

// ── Loot open-on-approach (loot slice) ──────────────────────────────────────
// Opening a container/corpse is a two-step on the wire: (1) send the `loot` verb so
// the SERVER walks the dude adjacent (authoritative), (2) once the viewer sees the
// dude arrive (it has the mirror + positions), open the loot modal LOCALLY. The
// modal reads the mirrored container inventory and its take/put reroute to wire
// verbs. No new wire event is needed — the client polls arrival itself. netId==0 =
// no pending loot.
static int gViewerPendingLootNetId = 0;
static unsigned int gViewerPendingLootSince = 0;
// A little longer than the server's approach cap (kInteractionBeatsCap ~15 s): if the
// dude can't path to the target, drop the pending open rather than wait forever.
static const unsigned int kViewerLootApproachTimeoutMs = 16000;

static void viewerArmPendingLoot(ClientConnection& conn, int netId)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "loot %d", netId);
    conn.sendLine(cmd);
    gViewerPendingLootNetId = netId;
    gViewerPendingLootSince = getTicks();
}

// Called once per viewer-loop iteration. When the dude reaches the pending loot
// target, open the (blocking) loot modal; the service ticker keeps the wire pumping
// inside it and force-closes on combat entry / rebaseline (kLoot is in the modal
// mask). Drops the pending open on timeout, a vanished target, or combat.
static void viewerPollPendingLoot(ClientConnection& conn)
{
    if (gViewerPendingLootNetId == 0) {
        return;
    }
    Object* container = objectFindByNetId(gViewerPendingLootNetId);
    if (container == nullptr || gDude == nullptr || container->elevation != gDude->elevation) {
        gViewerPendingLootNetId = 0;
        return;
    }
    if (objectGetDistanceBetween(gDude, container) <= 1) {
        gViewerPendingLootNetId = 0;
        // The client's belief at the moment it decides to open the screen, to be read
        // against the server's "not adjacent" line (server_control.cc). If the tiles
        // here disagree with the server's, the mirror is wrong and the sprite is drawn
        // somewhere the object is not; if they AGREE, the desync is elsewhere and the
        // glide/offset theory is dead. Also prints the sub-tile pixel offset, since a
        // stale glide offset moves the SPRITE without moving obj->tile — the case where
        // the tiles match yet the body still looks misplaced.
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            fprintf(stderr,
                "[loot-open] net=%d dude tile=%d elev=%d off=%d/%d |"
                " target tile=%d elev=%d off=%d/%d | dist=%d\n",
                container->netId, gDude->tile, gDude->elevation, gDude->x, gDude->y,
                container->tile, container->elevation, container->x, container->y,
                objectGetDistanceBetween(gDude, container));
        }
        // In combat the server already opened the container authoritatively and
        // charged the 3 AP for it (server_control.cc kInteractLoot), so this
        // screen is sanctioned and must survive the service ticker's combat
        // force-ESC — otherwise looting mid-fight would cost AP and show nothing.
        bool inCombat = conn.inCombat();
        if (inCombat) {
            conn.setCombatModalOpen(true);
        }
        inventoryOpenLooting(gDude, container);
        if (inCombat) {
            conn.setCombatModalOpen(false);
        }
        // Reap items the reconcile unlinked but deferred freeing while the modal could
        // still hold them (same lifetime safety as the 'I' inventory screen). A blob
        // buffered while the modal was open is drained by the main loop below.
        clientViewerFlushDeferredItemFrees();
    } else if (getTicksSince(gViewerPendingLootSince) >= kViewerLootApproachTimeoutMs) {
        gViewerPendingLootNetId = 0;
    }
}

// Map a picked vanilla action-menu item to a wire verb for `target` and send it
// upstream (INTERACTION_UX_DESIGN.md §3.2/§5). The menu is built + run locally by
// the shared game_mouse helpers; the viewer never runs the local action — it sends
// the claim-gated verb and the authoritative result returns on the wire. Items that
// still need streaming we don't have (use-item-on inventory, talk dialog options)
// fall back or no-op with a note. CANCEL(0) and "menu not shown" (-1) do nothing.
static void viewerSendActionMenuVerb(ClientConnection& conn, int menuItem, Object* target)
{
    char cmd[48];
    switch (menuItem) {
    case GAME_MOUSE_ACTION_MENU_ITEM_USE:
        // Vanilla USE forks on target type: scenery=use, item=pickup-or-loot, critter=loot.
        switch (FID_TYPE(target->fid)) {
        case OBJ_TYPE_SCENERY:
            snprintf(cmd, sizeof(cmd), "use %d", target->netId);
            conn.sendLine(cmd);
            break;
        case OBJ_TYPE_ITEM:
            // A container ITEM opens the loot screen (walk-then-open); any other item
            // is a ground pickup.
            if (itemGetType(target) == ITEM_TYPE_CONTAINER) {
                viewerArmPendingLoot(conn, target->netId);
            } else {
                snprintf(cmd, sizeof(cmd), "get %d", target->netId);
                conn.sendLine(cmd);
            }
            break;
        default:
            // Critter USE = loot a corpse (loot slice); a live critter is not lootable
            // this way (pickpocket/steal is a separate path).
            if (critterIsDead(target)) {
                viewerArmPendingLoot(conn, target->netId);
            }
            break;
        }
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_LOOK:
        snprintf(cmd, sizeof(cmd), "look %d", target->netId);
        conn.sendLine(cmd);
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_ROTATE:
        conn.sendLine("rot");
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_PUSH:
        snprintf(cmd, sizeof(cmd), "push %d", target->netId);
        conn.sendLine(cmd);
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_USE_SKILL: {
        // Run the local skilldex modal (pure UI); map its choice to the wire verb.
        // SNEAK is a self-toggle (no target) — deferred; the seven targeted skills
        // route to `skill <netId> <skillId>`.
        int skill = -1;
        switch (skilldexOpen()) {
        case SKILLDEX_RC_LOCKPICK: skill = SKILL_LOCKPICK; break;
        case SKILLDEX_RC_STEAL: skill = SKILL_STEAL; break;
        case SKILLDEX_RC_TRAPS: skill = SKILL_TRAPS; break;
        case SKILLDEX_RC_FIRST_AID: skill = SKILL_FIRST_AID; break;
        case SKILLDEX_RC_DOCTOR: skill = SKILL_DOCTOR; break;
        case SKILLDEX_RC_SCIENCE: skill = SKILL_SCIENCE; break;
        case SKILLDEX_RC_REPAIR: skill = SKILL_REPAIR; break;
        default: break; // canceled / sneak / error
        }
        if (skill != -1) {
            snprintf(cmd, sizeof(cmd), "skill %d %d", target->netId, skill);
            conn.sendLine(cmd);
        }
        break;
    }
    case GAME_MOUSE_ACTION_MENU_ITEM_TALK:
        snprintf(cmd, sizeof(cmd), "talk %d", target->netId);
        conn.sendLine(cmd);
        break;
    case GAME_MOUSE_ACTION_MENU_ITEM_INVENTORY:
        // Use-an-inventory-item-ON the target (the vanilla backpack action). Open the
        // local use-item-on picker modal (pure UI over the dude's mirror); its picked-
        // item leaf sends `useitemon <netId> <pid>` and skips the local action
        // (inventory_ui.cc clientViewerActive gate). The service ticker keeps the wire
        // pumping inside the modal + force-closes on combat entry / rebaseline (kUseOn ∈
        // kViewerModalMask). Out-of-combat only (the server drops the verb in combat —
        // in-combat item-use needs the AP barrier, a later slice). Reap deferred frees
        // after; a rebaseline buffered while open drains next main-loop iteration.
        if (!conn.inCombat()) {
            inventoryOpenUseItemOn(target);
            clientViewerFlushDeferredItemFrees();
        }
        break;
    // CANCEL / not-shown: nothing.
    default:
        break;
    }
}

// The single-click (primary action) verb for `target` in ARROW mode — the verb the
// vanilla hover icon represents (game_mouse.cc primary-action switch). Same wire
// vocabulary as the menu; a subset chosen by target type.
static void viewerSendPrimaryVerb(ClientConnection& conn, Object* target)
{
    char cmd[48];
    switch (FID_TYPE(target->fid)) {
    case OBJ_TYPE_ITEM:
        // Container → loot (walk-then-open); any other item → ground pickup.
        if (itemGetType(target) == ITEM_TYPE_CONTAINER) {
            viewerArmPendingLoot(conn, target->netId);
        } else {
            snprintf(cmd, sizeof(cmd), "get %d", target->netId);
            conn.sendLine(cmd);
        }
        break;
    case OBJ_TYPE_SCENERY:
        if (_obj_action_can_use(target)) {
            snprintf(cmd, sizeof(cmd), "use %d", target->netId);
        } else {
            snprintf(cmd, sizeof(cmd), "look %d", target->netId);
        }
        conn.sendLine(cmd);
        break;
    case OBJ_TYPE_CRITTER:
        if (target == gDude) {
            conn.sendLine("rot");
        } else if (critterIsDead(target)) {
            // Primary on a corpse is LOOT (loot slice).
            viewerArmPendingLoot(conn, target->netId);
        } else {
            // Primary on a live critter is TALK in vanilla.
            snprintf(cmd, sizeof(cmd), "talk %d", target->netId);
            conn.sendLine(cmd);
        }
        break;
    default: // WALL and the rest → examine.
        snprintf(cmd, sizeof(cmd), "look %d", target->netId);
        conn.sendLine(cmd);
        break;
    }
}

// Player-UI skill hotkeys (viewer): 'S' → skilldex, and number keys 2-8 → a skill
// directly, exactly like vanilla game_ui.cc. The chosen skill enters a LOCAL target-
// mode cursor (GAME_MOUSE_MODE_USE_*, pure UI); the click handler then resolves the
// object under the cursor and sends `skill <netId> <skillId>`. The server validates
// the id against the skilldex allow-list, walks the dude adjacent, and runs the REAL
// actionUseSkill (First Aid/Doctor heal + stream HP back, Lockpick opens the door,
// etc). SNEAK (key 1 / skilldex sneak) is a self-TOGGLE (dudeToggleState) with no
// target — a different path, deferred (needs a self-state wire verb).

// Number key → skill target mode (or -1). Mirrors game_ui.cc:349-404; SNEAK (key 1)
// is intentionally absent (self-toggle, deferred).
static int viewerSkillModeForKey(int keyCode)
{
    switch (keyCode) {
    case KEY_2: case KEY_AT: return GAME_MOUSE_MODE_USE_LOCKPICK;
    // KEY_3 (Steal) intentionally parked — see viewerSkillModeForSkilldexRc.
    case KEY_4: case KEY_DOLLAR: return GAME_MOUSE_MODE_USE_TRAPS;
    case KEY_5: case KEY_PERCENT: return GAME_MOUSE_MODE_USE_FIRST_AID;
    case KEY_6: case KEY_CARET: return GAME_MOUSE_MODE_USE_DOCTOR;
    case KEY_7: case KEY_AMPERSAND: return GAME_MOUSE_MODE_USE_SCIENCE;
    case KEY_8: case KEY_ASTERISK: return GAME_MOUSE_MODE_USE_REPAIR;
    default: return -1;
    }
}

// Skilldex result code → skill target mode (or -1 for cancel/error/sneak). Mirrors
// game_ui.cc:274-304; SNEAK returns -1 (deferred self-toggle).
static int viewerSkillModeForSkilldexRc(int rc)
{
    switch (rc) {
    case SKILLDEX_RC_LOCKPICK: return GAME_MOUSE_MODE_USE_LOCKPICK;
    // STEAL parked: it is a request-driven modal (scriptsRequestStealing → the loot
    // modal + _gIsSteal), server-side a no-op today, and needs a server-authoritative
    // steal-roll/caught verb — a "loot slice v2". Deferred like SNEAK so it reads as
    // "not wired" instead of walk-over-then-nothing. See the banked steal plan.
    case SKILLDEX_RC_TRAPS: return GAME_MOUSE_MODE_USE_TRAPS;
    case SKILLDEX_RC_FIRST_AID: return GAME_MOUSE_MODE_USE_FIRST_AID;
    case SKILLDEX_RC_DOCTOR: return GAME_MOUSE_MODE_USE_DOCTOR;
    case SKILLDEX_RC_SCIENCE: return GAME_MOUSE_MODE_USE_SCIENCE;
    case SKILLDEX_RC_REPAIR: return GAME_MOUSE_MODE_USE_REPAIR;
    default: return -1; // SKILLDEX_RC_SNEAK (deferred), _ERROR, cancel
    }
}

// Skill target mode → skill id (or -1 if the mode is not a skilldex skill mode). The
// inverse of the two maps above; used at the click site to name the wire verb, and as
// the "is a skill target mode active?" predicate for the cancel / combat-snap guards.
static int viewerSkillForMode(int mode)
{
    switch (mode) {
    case GAME_MOUSE_MODE_USE_LOCKPICK: return SKILL_LOCKPICK;
    case GAME_MOUSE_MODE_USE_STEAL: return SKILL_STEAL;
    case GAME_MOUSE_MODE_USE_TRAPS: return SKILL_TRAPS;
    case GAME_MOUSE_MODE_USE_FIRST_AID: return SKILL_FIRST_AID;
    case GAME_MOUSE_MODE_USE_DOCTOR: return SKILL_DOCTOR;
    case GAME_MOUSE_MODE_USE_SCIENCE: return SKILL_SCIENCE;
    case GAME_MOUSE_MODE_USE_REPAIR: return SKILL_REPAIR;
    default: return -1;
    }
}

// Enter a local skill target-mode cursor (pure UI); the click handler fires the verb.
static void viewerEnterSkillMode(int mode)
{
    gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
    gameMouseSetMode(mode);
}

// STEP-4 live viewer loop (CLIENT_JOIN_DESIGN.md §E). Connect to a running
// f2_server, receive the join snapshot, and render the live wire stream as a
// read-only puppet: NO scripts, NO AI, NO time advance, NO local animation — the
// server is authoritative and the client only mirrors + draws.
//
// connectSpec is "host:port". The blob load path (client_net's decoder) reuses
// the same mapLoad-with-viewer-suppression sequence the S1/S2 round trip proved,
// so it inherits the puppet map load (no map-enter procs) verbatim.
static int mainClientViewer(const char* connectSpec)
{
    // Parse host:port.
    char host[256];
    int port = 0;
    const char* colon = strrchr(connectSpec, ':');
    if (colon == nullptr || colon == connectSpec || (size_t)(colon - connectSpec) >= sizeof(host)) {
        debugPrint("client-viewer: F2_CLIENT_CONNECT must be host:port (got '%s')\n", connectSpec);
        return 1;
    }
    size_t hostLen = (size_t)(colon - connectSpec);
    memcpy(host, connectSpec, hostLen);
    host[hostLen] = '\0';
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        debugPrint("client-viewer: bad port in '%s'\n", connectSpec);
        return 1;
    }

    // Viewer flag for the process lifetime — gates focus behavior (an unfocused
    // viewer keeps pumping + rendering; see clientViewerActive in client_net.h).
    // Set before ANY inputGetInput (the blob-wait loop below pumps too).
    clientViewerSetActive(true);

    // A valid dude must exist before the snapshot's mapLoad (it references gDude);
    // the blob's dude then replaces it (_obj_load_dude). Mirror the probe/new-game
    // setup so gDude is fully formed.
    char gcdPath[COMPAT_MAX_PATH];
    strcpy(gcdPath, "premade\\combat.gcd");
    if (_proto_dude_init(gcdPath) == -1) {
        debugPrint("client-viewer: dude init failed\n");
        return 1;
    }

    // F2_PLAYER_CREATE=ask: ROLL A CHARACTER before joining, using vanilla's own
    // creation editor (ACCOUNT_IDENTITY_DESIGN.md stage 2). Reusing
    // characterEditorShow(1) rather than building a second creation UI means the
    // player gets the real screen — stat picker, tag skills, traits, the lot.
    //
    // ►► RUNS BEFORE THE PRESENTATION SCAFFOLDING BELOW, and it must: that block's
    // full-screen window is WINDOW_MOVE_ON_TOP, so it stays above every window
    // opened after it — the editor draws underneath and the player gets a black
    // screen with working sound. What the editor DOES need is the setup vanilla's
    // character selector does before calling it (characterSelectorOpen): a shown
    // cursor and color.pal faded in. Without those it is a mouseless keyboard-only
    // screen. Both bugs were live-reported; do not "tidy" this back together.
    //
    // Pre-connection by design: the editor edits the LOCAL gDude and the join blob
    // overwrites that a moment later, so the local edits are only a vehicle for
    // reading the numbers back out. The server owns the character — we send the
    // spec and it builds the sheet.
    //
    // The `create` line is composed now and sent with the login below, once the
    // world is present.
    char createFromUi[192];
    createFromUi[0] = '\0';
    {
        const char* createEnv = getenv("F2_PLAYER_CREATE");
        if (createEnv != nullptr && (strcmp(createEnv, "ask") == 0 || strcmp(createEnv, "ui") == 0)) {
            // Mirrors characterSelectorOpen's bracket exactly, so the editor sees
            // the environment it was written against.
            bool cursorWasHidden = cursorIsHidden();
            if (cursorWasHidden) {
                mouseShowCursor();
            }
            colorPaletteLoad("color.pal");
            paletteFadeTo(_cmap);

            // _ResetPlayer first, exactly as the character selector's "Create"
            // does — otherwise the editor opens on the premade rather than a
            // blank character.
            _ResetPlayer();

            int editorRc = characterEditorShow(1);

            paletteFadeTo(gPaletteBlack);
            if (cursorWasHidden) {
                mouseHideCursor();
            }

            if (editorRc == 0) {
                // SPECIAL is read as DISPLAYED (critterGetStat, trait modifiers
                // included) because that is precisely what the server's applier
                // expects: it sets traits first, and critterSetBaseStat subtracts
                // the trait modifier from the value it is handed. Displayed-in,
                // displayed-out — the round trip is exact.
                int special[7];
                for (int i = 0; i < 7; i++) {
                    special[i] = critterGetStat(gDude, STAT_STRENGTH + i);
                }

                int tagged[NUM_TAGGED_SKILLS];
                for (int i = 0; i < NUM_TAGGED_SKILLS; i++) {
                    tagged[i] = -1;
                }
                skillsGetTagged(tagged, NUM_TAGGED_SKILLS);

                int trait1 = -1;
                int trait2 = -1;
                traitsGetSelected(&trait1, &trait2);

                snprintf(createFromUi, sizeof(createFromUi),
                    "%d %d %d %d %d %d %d %d %d %d %d %d",
                    special[0], special[1], special[2], special[3],
                    special[4], special[5], special[6],
                    tagged[0], tagged[1], tagged[2], trait1, trait2);
                debugPrint("client-viewer: created character -> create %s\n", createFromUi);
            } else {
                // Cancelled: join as whatever the account already is (or a clone
                // of the host for a brand-new name). Never silently invent a
                // character the player backed out of.
                debugPrint("client-viewer: character creation cancelled\n");
            }
        }
    }

    // Presentation scaffolding — the subset of _main_load_new that stands up the
    // iso window + palette WITHOUT loading a map (the blob provides the map).
    // ⚠ `win` is MOVE_ON_TOP: nothing that needs to be SEEN may open after it.
    gDude->flags &= ~OBJECT_FLAT;
    objectShow(gDude, nullptr);
    mouseHideCursor();

    int win = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), _colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    windowRefresh(win);

    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);
    _map_init();
    gameMouseSetCursor(MOUSE_CURSOR_NONE);
    mouseShowCursor();

    const char* blobTmp = getenv("F2_JOIN_TMP_CLIENT");
    if (blobTmp == nullptr) {
        blobTmp = "/tmp/f2ce_join_view.bin";
    }

    ClientConnection conn;
    if (!conn.connect(host, port, blobTmp)) {
        debugPrint("client-viewer: connect to %s:%d failed\n", host, port);
        windowDestroy(win);
        main_unload_new();
        return 1;
    }
    debugPrint("client-viewer: connected to %s:%d — awaiting join snapshot...\n", host, port);

    // Block (while keeping the window alive) until the join snapshot has been
    // received and loaded — the world becomes present via the decoder's mapLoad.
    while (conn.blobLoaded() == false) {
        sharedFpsLimiter.mark();
        if (!conn.pump()) {
            debugPrint("client-viewer: stream error before snapshot\n");
            conn.close();
            windowDestroy(win);
            main_unload_new();
            return 1;
        }
        int keyCode = inputGetInput();
        if (keyCode == KEY_ESCAPE || _game_user_wants_to_quit != 0) {
            conn.close();
            windowDestroy(win);
            main_unload_new();
            return 0;
        }
        renderPresent();
        sharedFpsLimiter.throttle();
    }

    debugPrint("client-viewer: snapshot loaded, entering viewer loop\n");

    // The blob's mapLoad drew the world under the loading backdrop; drop it and
    // fade in, matching _main_load_new's tail.
    windowDestroy(win);
    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);

    // §E puppet levers. scriptsDisable() sets gScriptsEnabled = false, which neuters
    // _doBkProcesses — its AI / timed-event / queue work is all guarded on that flag,
    // so no sim advances even though the ticker stays registered. Then strip the local
    // animation tickers isoEnable re-added during the blob's mapLoad (they fight the
    // authoritative OBJECT_DELTA_FID/MOVE from the wire), and disable the gmouse
    // object-interaction handler worldEnable() turned on: the viewer never routes mouse
    // input through gameHandleKey, but don't let that be the ONLY thing between a viewer
    // and a sim mutation.
    scriptsDisable();
    tickersRemove(_object_animate);
    tickersRemove(_dude_fidget);
    // STEP 6 (controllable client): ENABLE the mouse in a pinned MOVE mode rather
    // than disabling it. The gameMouseRefresh ticker (registered by _map_init) then
    // renders the hex + bouncing cursor, which mutate ONLY the OBJECT_NO_SAVE cursor
    // objects the netId walk skips — presentation-safe. We never call
    // _gmouse_handle_event, so a click never drives _dude_move/_dude_run on the
    // mirror; the frame loop below captures the click and sends it UPSTREAM as an
    // intent. Edge scrolling stays on (_gmouse_enable turns it on) so the camera
    // follows the dude. Mode is pinned MOVE: it can only change via 'M'/right-click,
    // neither of which the viewer routes into the sim.
    _gmouse_enable();
    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
    gameMouseObjectsShow();

    // Presentation layer (client_present): glide critters between tiles on stepped
    // MOVE hops, replay the vanilla attack/hit/death choreography from
    // EVENT_ATTACK_RESULT, and slide doors open/closed from EVENT_DOOR_STATE — so the
    // viewer paces the pixels instead of snapping. Viewer-only — the flag defaults off,
    // so the headless joining client's decode path is untouched.
    presSetEnabled(true);

    // F2_VIEWER_SHOT_EVERY=N: dump a screenshot (scrNNNNN.bmp in the game dir, via
    // the normal screenshot machinery — correct palette) every N rendered frames.
    // A capture hook for verifying the live render off-line; unset = no dumps.
    const char* shotEveryValue = getenv("F2_VIEWER_SHOT_EVERY");
    int shotEvery = shotEveryValue != nullptr ? atoi(shotEveryValue) : 0;
    int frameNo = 0;

    // Register this connection as the viewer's upstream so shared combat code can
    // forward verbs through it: the crosshair click runs the real _combat_attack_this,
    // whose commit fork (combat.cc §3.b) calls clientViewerCommitAttack → `cattack`.
    clientViewerSetConnection(&conn);
    combatSetViewerAttackHook(clientViewerCommitAttack);
    // Keep the wire pumping inside modal screens (inventory/skilldex/char/pipboy) and
    // force-close them on combat entry / rebaseline — those loops don't call conn.pump().
    clientViewerInstallServiceTicker();

    // Claim authoritative control of the actor once, now that the world is present.
    // The server binds the claim to this connection for its lifetime (a rebaseline
    // does not drop it), so this is sent exactly once.
    //
    // F2_PLAYER_NAME set → bind by ACCOUNT NAME via `login <name> [token]`
    // (ACCOUNT_IDENTITY_DESIGN.md): the server maps the name to a slot, so the
    // same human returns to the same character across reconnects. Unset → the
    // legacy bare `claim` (host/dev affordance, slot-0-preferred) — this keeps the
    // headless/golden path byte-identical since no name is ever sent there.
    // Tell the server which OS this client runs on, so the join greeting can show
    // "X joined the game (Linux/Windows/...)". Compile-time; sent before login (and
    // before bare claim) so it is stored by the time the greeting is composed. Extra
    // control line only — it never changes any server-emitted wire bytes the goldens
    // compare.
#if defined(_WIN32)
    conn.sendLine("platform Windows");
#elif defined(__APPLE__)
    conn.sendLine("platform macOS");
#elif defined(__linux__)
    conn.sendLine("platform Linux");
#else
    conn.sendLine("platform Unknown");
#endif

    const char* playerName = getenv("F2_PLAYER_NAME");
    if (playerName != nullptr && playerName[0] != '\0') {
        // F2_PLAYER_CREATE="S P E C I A L [tag tag tag] [trait trait]": state the
        // character to BE, for a name the server has never seen. Sent BEFORE the
        // login that commits it, and ignored by the server for an existing account
        // (you do not re-roll by reconnecting).
        //
        // This is the headless/CLI way to express creation intent; a creation
        // SCREEN would fill in the same spec and send the same line. Unset = join
        // as a copy of the host, which is the unchanged default.
        // Either the spec the creation editor produced above (F2_PLAYER_CREATE=ask)
        // or a literal spec passed straight in the env (headless / scripted).
        const char* createSpec = createFromUi[0] != '\0' ? createFromUi : getenv("F2_PLAYER_CREATE");
        if (createSpec != nullptr && createSpec[0] != '\0'
            && strcmp(createSpec, "ask") != 0 && strcmp(createSpec, "ui") != 0) {
            char createLine[224];
            snprintf(createLine, sizeof(createLine), "create %s", createSpec);
            conn.sendLine(createLine);
        }

        const char* playerToken = getenv("F2_PLAYER_TOKEN");
        char loginLine[128];
        if (playerToken != nullptr && playerToken[0] != '\0') {
            snprintf(loginLine, sizeof(loginLine), "login %s %s", playerName, playerToken);
        } else {
            snprintf(loginLine, sizeof(loginLine), "login %s", playerName);
        }
        conn.sendLine(loginLine);
    } else {
        conn.sendLine("claim");
    }

    _game_user_wants_to_quit = 0;
    int puppetLoadCount = conn.loadCount();

    // Combat presentation gate (COMBAT_CLIENT_DESIGN.md §3.c). While the viewer owes
    // combat animation — it is not our turn, an attack replay is playing/queued, or
    // we just committed an action the server hasn't answered yet — vanilla shows the
    // animated pocket-watch cursor and eats input. `actionPending` is the local
    // send-prediction that blocks AP-spam in the network round-trip gap: set when we
    // send a combat verb, cleared once the world confirms it (presentation busy /
    // turn changed) or a short timeout elapses.
    bool combatBusy = false;
    bool watchCursorShown = false;
    bool actionPending = false;
    unsigned int actionPendingSince = 0;
    // A weapon-slot left-click issued out of combat: cstart sent, switch to the crosshair
    // (target) cursor once combat actually opens (the per-frame guard force-resets
    // CROSSHAIR→MOVE until then). Mirrors vanilla left-click-weapon = enter combat + arm.
    bool pendingEnterCrosshair = false;
    const unsigned int kActionPendingTimeoutMs = 1200;

    while (_game_user_wants_to_quit == 0) {
        sharedFpsLimiter.mark();

        // ►► RECONCILE DIALOG WINDOWS TO THE LATCHED WIRE STATE (viewer-modal I2/I6).
        // The wire decoder only latches "conversation live / node payload / trade
        // open"; this single owner builds and tears down the dialog session + node
        // subwindows to match, and then applies any pending node content. Runs before
        // the barter-enter check below so node subwindows are already down (or up)
        // when the trade window opens/closes, replacing the old order-of-arrival
        // guesswork that made the barter<->dialog transition flicker and orphan windows.
        clientModalWindowsSync();
        clientDialogRenderPendingNode();

        // Barter streaming: the decoder opened a trade session (mirrors built from
        // EVENT_BARTER_STATE). Put the vanilla trade window up over the dialog
        // window and hold it until the server ends the trade. Entered here, beside
        // the worldmap, because both are server-driven modals that must run from
        // the main loop rather than from inside the decoder — applying wire events
        // from within a blocking modal is how the world gets freed under an open
        // screen. Driver and spectators alike; the window is read-only for now.
        if (clientBarterActive() && (GameMode::getCurrentGameMode() & GameMode::kBarter) == 0) {
            inventoryOpenTradeViewer(clientBarterMerchant(),
                clientBarterPlayerTable(), clientBarterMerchantTable());
        }
        // Tear down a trade the server ENDED. onBarterEnd only latches the end (it
        // is decoded from inside the trade loop, where freeing the mirrors would
        // dangle the drag gesture / quantity dial); the trade loop above has since
        // broken and returned, so this is the safe point to actually free them.
        clientBarterFinalize();

        // Worldmap streaming: the wire decoder sets gPendingWorldmapEnter when the
        // server enters the worldmap travel driver. Enter the worldmap modal loop
        // (blocking, with viewerServiceTicker wire pump via kViewerModalMask) before
        // consuming input for this frame.
        if (gPendingWorldmapEnter) {
            gPendingWorldmapEnter = false;
            gWorldmapStreaming = true;
            wmWorldMap();
            gWorldmapStreaming = false;

            // wmInterfaceInit black-fills gIsoWindow and disables the tile engine;
            // wmInterfaceExit only re-ENABLES it — it never repaints. Vanilla gets
            // away with that because the worldmap can only be left by entering a
            // location, so mapLoadById always follows and redraws. Our streaming
            // exit has paths that change NO map (the player escaped, or the server
            // driver bailed with map == -1), and then nothing ever redraws the
            // world: the viewer sits on a black window over a perfectly good map.
            // Repaint here — but not when a rebaseline is queued, since that path
            // reloads the map and repaints on its own a few lines below.
            if (!conn.blobDeferred()) {
                tileWindowRefresh();
            }
        }

        // Loot open-on-approach: if a `loot` verb walked the dude to a container/corpse,
        // open the loot modal now that it has arrived (blocking; the ticker pumps the
        // wire inside). Checked before input so it opens the same frame the dude lands.
        viewerPollPendingLoot(conn);

        // Pump input so the window stays responsive and ESC quits. No gameplay keys
        // or clicks are dispatched (gameHandleKey is deliberately NOT called), so the
        // viewer can never mutate the sim; the standard mouse pump still runs harmlessly.
        int keyCode = inputGetInput();

        if (clientDialogActive()) {
            clientDialogHandleKey(keyCode);
        } else if (keyCode == KEY_ESCAPE) {
            break;
        }

        if (!clientDialogActive()) {

        // Vanilla combat input, driven from the viewer frame loop (COMBAT_CLIENT_DESIGN.md
        // §3.a/§3.b). No bespoke keys/widgets: the end-turn / end-combat interface-bar
        // buttons post keycodes 32 (SPACE) / 13 (RETURN) into the SAME input queue as the
        // vanilla keyboard keys, so handling the keycodes covers the buttons AND the keys
        // for free. Verbs are claim-gated upstream and drained by the resumable barrier on
        // the dude's turn. Gated on !combatBusy so you cannot queue another action while an
        // animation plays or before the last is answered — vanilla blocks input for the
        // whole attack (this also stops spamming AP away in a blink, §3.c).
        if (conn.inCombat()) {
            if (conn.myTurn() && !combatBusy) {
                if (keyCode == KEY_SPACE) {
                    conn.sendLine("cendturn"); // 32: end-turn button / SPACE
                    actionPending = true;
                    actionPendingSince = getTicks();
                } else if (keyCode == KEY_RETURN) {
                    // 13: end-combat button / RETURN — vanilla "attempt to end combat"
                    // (the server refuses with a console message if hostiles remain).
                    conn.sendLine("cendcombat");
                    actionPending = true;
                    actionPendingSince = getTicks();
                }
            }
        } else if (keyCode == KEY_LOWERCASE_A || keyCode == KEY_UPPERCASE_A) {
            // Out of combat, vanilla 'A' toggles combat (game_ui.cc → _combat(nullptr)).
            // The wire equivalent is cstart; the server enters combat on its idle tick.
            conn.sendLine("cstart");
        }

        // Weapon interface-bar controls, in OR out of combat (vanilla lets you set these
        // anytime): 'N' / right-click the weapon slot cycles the active hand's attack
        // mode (primary→aimed→secondary→burst; e.g. punch↔kick is the two empty hands),
        // 'B' swaps the active hand. Both mutate ONLY local interface state
        // (gInterfaceCurrentHand / .action) — the same state the local _combat_attack_this
        // reads and forwards in `cattack` (hitMode + aimed hitLocation, which the server
        // validates and applies), so nothing races the sim and no desync is possible.
        // Non-animated swap: the animated put-away/take-out mutates gDude->fid, which would
        // fight the server-authoritative fid stream (§3). A swap can drop crosshair→move
        // mode (interface.cc:1195), and cycling changes what a highlighted target means, so
        // recompute outlines after either while in combat (#8).
        if (keyCode == KEY_LOWERCASE_N || keyCode == KEY_UPPERCASE_N) {
            interfaceCycleItemAction();
            if (conn.inCombat()) {
                conn.recomputeCombatOutlines();
            }
        } else if (keyCode == KEY_LOWERCASE_B || keyCode == KEY_UPPERCASE_B) {
            interfaceBarSwapHands(false);
            if (conn.inCombat()) {
                conn.recomputeCombatOutlines();
            }
        } else if (keyCode == -20) {
            // Left-click the weapon/attack slot (interface.cc:505) = vanilla "activate
            // weapon" (_intface_use_item): arm the crosshair + enter combat. We can't run
            // _intface_use_item locally — it also reloads / _obj_use_item / calls _combat,
            // all sim mutations the server owns — so replicate only the safe intent: enter
            // combat via cstart if needed, then switch to the crosshair (target) cursor.
            // In combat it sticks immediately; out of combat the CROSSHAIR→MOVE guard below
            // holds until COMBAT_ENTER, so defer via pendingEnterCrosshair. (Reload/use-item
            // branch of the slot is deferred to the inventory verb slices.)
            if (conn.inCombat()) {
                gameMouseSetMode(GAME_MOUSE_MODE_CROSSHAIR);
                conn.recomputeCombatOutlines();
            } else {
                conn.sendLine("cstart");
                pendingEnterCrosshair = true;
            }
        } else if ((keyCode == KEY_LOWERCASE_I || keyCode == KEY_UPPERCASE_I) && conn.inCombat()) {
            // IN COMBAT the open is the priced act (4 AP, 2 with Quick Pockets) and
            // only the server can rule on it, so ask instead of opening: it checks
            // that it is our turn, charges, and answers with EVENT_INVENTORY_GRANT,
            // which the branch below turns into the actual screen. A refusal
            // (someone else's turn, or not enough AP) simply never grants, and the
            // server streams the reason to the console.
            conn.sendLine("invopen");
        } else if ((keyCode == KEY_LOWERCASE_I || keyCode == KEY_UPPERCASE_I) && !conn.inCombat()) {
            // Open the vanilla inventory screen (player-UI Slice 3). Equip/drop are wired
            // (Slice 3b): each drop-resolution leaf fires a claim-gated wire verb and skips
            // the local mutation, so the server-authoritative inventory stays the source of
            // truth; USE/UNLOAD are still deferred (3c). It runs its own blocking loop; the
            // service ticker keeps the wire pumping inside it and force-closes on combat
            // entry / rebaseline (a rebaseline is deferred so mapLoad can't free gDude under
            // the open screen).
            inventoryOpen();
            // The screen is closed now: reap items the reconcile unlinked but deferred
            // freeing while a handler could still hold them (Slice 3b lifetime safety).
            clientViewerFlushDeferredItemFrees();
            // A rebaseline buffered while the screen was open is drained by the main loop
            // below (conn.blobDeferred), uniformly for every modal.
        } else if (keyCode == KEY_UPPERCASE_C || keyCode == KEY_LOWERCASE_C) {
            // 'C' → the character sheet. The interface bar's character button is
            // registered with KEY_LOWERCASE_C (interface.cc), so this one branch
            // serves the hotkey AND the button.
            //
            // VIEW-ONLY (characterEditorShowViewOnly): the server owns the sheet,
            // so local edits are drift the next rebaseline reverts — see that
            // function. It shows THIS client's actor because the viewer's gDude is
            // the local actor role, and the stats resolve through the actor's own
            // sheet pid, not the host's.
            //
            // Allowed in combat: vanilla's character screen is free, and kEditor is
            // already in kViewerModalMask so the wire keeps pumping underneath.
            characterEditorShowViewOnly();
            // Uniform with 'I'/'S': reap deferred frees, let the main loop drain a
            // blob that buffered while the screen blocked.
            clientViewerFlushDeferredItemFrees();
        } else if (keyCode == KEY_UPPERCASE_S || keyCode == KEY_LOWERCASE_S) {
            // 'S' → skilldex (player-UI skill hotkeys). Runs the vanilla read-only
            // selector; its blocking loop is pump-and-bailed by the service ticker
            // (kSkilldex ∈ kViewerModalMask). Map the chosen skill to a local target
            // mode; the click handler below fires the `skill` wire verb.
            //
            // Allowed IN COMBAT since Stage 4. The selector itself is read-only and
            // free — vanilla prices the inventory screen, not this one — and the
            // skill it arms is answered honestly on use: msg 902 for the seven
            // combat-forbidden skills, a real toggle for Sneak.
            int mode = viewerSkillModeForSkilldexRc(skilldexOpen());
            // The ticker may have applied dude-inv deltas while the modal blocked — reap
            // deferred frees + let the main loop drain a deferred blob (uniform with 'I').
            clientViewerFlushDeferredItemFrees();
            if (mode != -1) {
                viewerEnterSkillMode(mode);
            }
        } else if (viewerSkillModeForKey(keyCode) != -1) {
            // Number keys 2-8 → skill target mode directly (vanilla game_ui.cc:349).
            viewerEnterSkillMode(viewerSkillModeForKey(keyCode));
        }

        // Mouse. inputGetInput returns -2 on a mouse-button event (input.cc). We keep the
        // capture-don't-dispatch discipline (never _gmouse_handle_event — its ARROW branch
        // mutates the sim); each button is interpreted here into an upstream verb, and the
        // authoritative result returns on the wire like any other.
        if (keyCode == -2) {
            int mouseEvent = mouseGetEvent();
            int mx = 0;
            int my = 0;
            mouseGetPosition(&mx, &my);

            bool inCombat = conn.inCombat();
            int mode = gameMouseGetMode();

            // Right-click cycles the cursor mode. In combat: MOVE→CROSSHAIR→ARROW
            // (move / attack / view — ARROW is view+LOOK only in combat; the acting
            // verbs are AP-gated and deferred). Out of combat: MOVE↔ARROW (ARROW
            // unlocks the vanilla action menu + primary-action hover icon; CROSSHAIR
            // is combat-only).
            if ((mouseEvent & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0) {
                if (viewerSkillForMode(mode) != -1) {
                    // Right-click cancels an active skill target mode → back to MOVE
                    // (vanilla drops the skill cursor on a non-target action).
                    gameMouseSetCursor(MOUSE_CURSOR_NONE);
                    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
                } else if (inCombat) {
                    gameMouseSetMode(mode == GAME_MOUSE_MODE_MOVE ? GAME_MOUSE_MODE_CROSSHAIR
                            : mode == GAME_MOUSE_MODE_CROSSHAIR ? GAME_MOUSE_MODE_ARROW
                                                               : GAME_MOUSE_MODE_MOVE);
                    // Outlines follow the cursor mode: crosshair (attack) highlights all
                    // in-LOS critters, move/view clears them (vanilla, #8).
                    conn.recomputeCombatOutlines();
                } else {
                    gameMouseSetMode(mode == GAME_MOUSE_MODE_ARROW
                            ? GAME_MOUSE_MODE_MOVE
                            : GAME_MOUSE_MODE_ARROW);
                }
            } else if (mode == GAME_MOUSE_MODE_ARROW
                && (mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) == MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT
                && windowGetAtPoint(mx, my) == gIsoWindow) {
                // ARROW-mode left-HOLD → the vanilla action menu. Build + run it
                // locally (pure-read on the mirror, via the shared game_mouse helpers),
                // then map the picked item to a wire verb (§3.2). Works in combat too:
                // since Stage 4 the acting verbs LAND there — the server queues them
                // as combat intents and runs them on our own turn, charging the
                // vanilla AP. (Before Stage 4 only LOOK landed and the rest were
                // dropped at the wire.) The run loop owns its own input/render spin;
                // conn.pump() catches up on release. netId 0 = unsynced → ignore.
                Object* targetObj = gameMouseGetObjectUnderCursor(-1, true, gElevation);
                if (targetObj != nullptr && targetObj->netId > 0) {
                    int actionMenuItems[6];
                    int count = gameMouseBuildActionMenu(targetObj, actionMenuItems);
                    int selectedItem = gameMouseRunActionMenu(mx, my, actionMenuItems, count);
                    viewerSendActionMenuVerb(conn, selectedItem, targetObj);
                }
            } else if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0
                && windowGetAtPoint(mx, my) == gIsoWindow) {
                // Only clicks inside the iso window act (game_mouse.cc pattern); the
                // viewer's gIsoWindow spans the whole screen at (0,0).
                if (inCombat && mode == GAME_MOUSE_MODE_CROSSHAIR) {
                    // Crosshair attack. Resolve the target exactly as vanilla's in-window
                    // handler (game_mouse.cc:1001: critter first, then any object) and run
                    // the REAL _combat_attack_this — its whole selection UX (turn guard, hit
                    // mode from the bar, bad-shot messages, called-shot picker) runs locally,
                    // and its commit fork forwards `cattack` upstream (§3.b).
                    if (conn.myTurn() && !combatBusy) {
                        Object* targetObj = gameMouseGetObjectUnderCursor(OBJ_TYPE_CRITTER, false, gElevation);
                        if (targetObj == nullptr) {
                            targetObj = gameMouseGetObjectUnderCursor(-1, false, gElevation);
                        }
                        if (targetObj != nullptr) {
                            // _combat_attack_this runs the vanilla selection UX and
                            // only forwards a `cattack` at its commit point. Arm the
                            // input-lock ONLY if a verb actually went out — a bad-shot
                            // message / out-of-range / picker-cancel sends nothing and
                            // must not freeze input for the round-trip timeout.
                            _combat_attack_this(targetObj);
                            if (clientViewerTakeAttackCommitted()) {
                                actionPending = true;
                                actionPendingSince = getTicks();
                            }
                        }
                    }
                } else if (viewerSkillForMode(mode) != -1) {
                    // Skill target-mode click (player-UI skill hotkeys): resolve the
                    // object under the cursor and fire `skill <netId> <skillId>`. The
                    // server validates the id, walks the dude adjacent, and runs the
                    // real actionUseSkill. Clicking your own dude self-casts (First
                    // Aid/Doctor on self). Drop back to MOVE like vanilla afterward
                    // (game_mouse.cc:1192-1196), whether or not a target was hit.
                    //
                    // Allowed in combat since Stage 4: the verb is queued for our own
                    // turn, where actionUseSkill answers seven of the eight skills
                    // with msg 902 and toggles Sneak. The refusal is the feedback.
                    Object* targetObj = gameMouseGetObjectUnderCursor(-1, true, gElevation);
                    if (targetObj != nullptr && targetObj->netId > 0) {
                        char cmd[48];
                        snprintf(cmd, sizeof(cmd), "skill %d %d", targetObj->netId, viewerSkillForMode(mode));
                        conn.sendLine(cmd);
                    }
                    gameMouseSetCursor(MOUSE_CURSOR_NONE);
                    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
                } else if (mode == GAME_MOUSE_MODE_ARROW) {
                    // ARROW-mode left-CLICK → the target's primary verb (the action
                    // the hover icon shows), in OR out of combat. In combat the verb
                    // is queued for our own turn and charged the vanilla AP (Stage 4);
                    // it used to degrade to LOOK here because the server dropped every
                    // acting verb mid-fight, which made view mode a dead end once a
                    // fight started.
                    Object* targetObj = gameMouseGetObjectUnderCursor(-1, true, gElevation);
                    if (targetObj != nullptr && targetObj->netId > 0) {
                        viewerSendPrimaryVerb(conn, targetObj);
                    }
                } else {
                    // MOVE-mode left click → a move. Run iff shift XOR the running
                    // preference; mirror ONLY the SELECTION half of game_mouse.cc's move
                    // logic (never _dude_move/_dude_run, which animate the local mirror).
                    // Objects (doors included) are used via ARROW mode now — the slice-1
                    // MOVE-mode door special case is retired; MOVE means move (vanilla).
                    int tile = tileFromScreenXY(mx, my, gElevation);
                    if (tile >= 0) {
                        bool shift = gPressedPhysicalKeys[SDL_SCANCODE_LSHIFT]
                            || gPressedPhysicalKeys[SDL_SCANCODE_RSHIFT];
                        bool run = shift != (settings.preferences.running != 0);
                        char cmd[32];
                        if (inCombat) {
                            // In-combat MOVE (AP-limited) → cmove for the barrier. Gated
                            // like the attack: no moves while busy (§3.c).
                            if (!combatBusy) {
                                snprintf(cmd, sizeof(cmd), "cmove %d %d", tile, run ? 1 : 0);
                                conn.sendLine(cmd);
                                actionPending = true;
                                actionPendingSince = getTicks();
                            }
                        } else {
                            // VANILLA PARITY: game_mouse's click-to-move preamble
                            // (_dude_move_prep, animation.cc) runs reg_anim_clear(gDude)
                            // under settings.system.interrupt_walk — that is what cancels
                            // the dude's in-flight IDLE (fidget) animation when you click
                            // to walk. We deliberately don't call _dude_move/_dude_run
                            // (they animate the local mirror), and so we silently skipped
                            // this cancel too.
                            //
                            // Consequence (owner-found "he runs on one leg"): the fidget
                            // SAD stays live in the local animation registry while the
                            // server-driven glide plays, and BOTH drive the same sprite —
                            // _object_animate_pass re-applies the fidget's STAND fid +
                            // frame 0 every pass (animation.cc:2369), the glide re-asserts
                            // its walk fid, and the walk cycle never advances past ~frame 3
                            // of 10. The tell is that the fidget visibly FINISHES after the
                            // run ends: it outlived the walk that should have cancelled it.
                            //
                            // Local presentation only — no sim state, and the glide owns the
                            // sprite from here. Honors the vanilla setting, so a player who
                            // turned interrupt_walk off keeps vanilla's (janky) behavior.
                            if (settings.system.interrupt_walk) {
                                reg_anim_clear(gDude);
                            }
                            snprintf(cmd, sizeof(cmd), "mv %d %d", tile, run ? 1 : 0);
                            conn.sendLine(cmd);
                        }
                    }
                }
            }
        }

        } // if (!clientDialogActive())

        // A rebaseline that arrived while a modal screen was open was buffered (onBlobEnd
        // deferred it so mapLoad couldn't free gDude under the screen). The modal has since
        // closed (we're back in the main loop), so apply it now — BEFORE this frame's pump,
        // so fresh events land on the rebuilt world. Uniform drain for every modal (Slice 3a
        // review MEDIUM: the per-'I'-site drain didn't cover skilldex/char/pipboy).
        if (conn.blobDeferred()) {
            conn.applyDeferredBlob();
        }

        // The server granted the in-combat inventory screen we asked for on our
        // turn, and has already charged the AP. Open it HERE rather than in the
        // decoder that received the grant: the screen runs its own blocking loop,
        // and entering it from inside conn.pump() would re-enter the pump that is
        // still decoding the frame. This is the same no-modal-open point the
        // deferred rebaseline above uses, for the same reason.
        if (conn.takeInventoryGrant()) {
            conn.setCombatModalOpen(true); // exempts it from the ticker's combat force-ESC
            inventoryOpen();
            conn.setCombatModalOpen(false);
            // Tell the server the session is over, so the next 'I' is priced again
            // — vanilla charges per OPEN, and reopening is a second 4 AP.
            conn.sendLine("invclose");
            clientViewerFlushDeferredItemFrees(); // uniform with the out-of-combat 'I' path
        }

        // Apply the live wire: decode every frame that has arrived and mutate the
        // local sim (pure state setters). objectSetLocation etc. invalidate the
        // tile rects, so the next renderPresent redraws the moved objects.
        if (!conn.pump()) {
            debugPrint("client-viewer: server disconnected\n");
            break;
        }

        // A rebaseline blob was applied mid-run (another viewer joined, or a map
        // transition): that mapLoad re-registered the animation tickers and
        // re-enabled gmouse, exactly like the first load did. Re-apply the §E
        // puppet levers or the viewer stops being a puppet.
        if (conn.loadCount() != puppetLoadCount) {
            puppetLoadCount = conn.loadCount();
            scriptsDisable();
            tickersRemove(_object_animate);
            tickersRemove(_dude_fidget);
            // Re-apply the STEP 6 input enablement: the rebaseline's mapLoad
            // re-enabled gmouse and re-added the tickers, exactly like the first
            // load. The server still holds our claim (same connection) — do NOT
            // re-claim.
            _gmouse_enable();
            gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
            gameMouseObjectsShow();
            // The re-pin reset the cursor to the MOVE hex; drop our watch-cursor
            // latch so the combat-busy check below re-applies it if still needed.
            watchCursorShown = false;
        }

        // S4: drive combat presentation. presentationTick starts the next queued
        // attack replay once the previous is idle (serialized, one hit per animation)
        // and applies the deferred end-of-combat chrome after the queue drains.
        conn.presentationTick();

        // Step the whole presentation layer: glides (pure render offsets/frames; sim
        // state was already applied by the decoder above), then the attack/hit/death/
        // door reg_anim sequences via _object_animate, then reap completed replays and
        // door slides. The internal ordering (glide → _object_animate → reap) is
        // load-bearing; presAdvance owns it (client_present.cc).
        presAdvance();

        // CROSSHAIR is a combat-only cursor: on combat exit, snap it back to MOVE so
        // the hex cursor returns and a stray click can't try to attack. MOVE and ARROW
        // are legal in both phases (ARROW is view+LOOK in combat), so neither is
        // force-snapped; the player re-picks CROSSHAIR with right-click in combat.
        if (!conn.inCombat() && gameMouseGetMode() == GAME_MOUSE_MODE_CROSSHAIR) {
            gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
        }

        // Skill target modes survive combat entry since Stage 4: the `skill` verb
        // now lands in combat, where vanilla answers seven of the eight skilldex
        // skills with proto msg 902 ("you cannot use that skill in combat") and
        // toggles the eighth — Sneak — normally (actions.cc:1662ff, 1760). The
        // mode is therefore no longer dead in combat, so it is not snapped back:
        // the refusal message IS the feedback, and silently dropping the cursor
        // gave the player none.

        // Deferred crosshair from an out-of-combat weapon-slot left-click: once the cstart
        // we sent has opened combat, switch to the target cursor (vanilla left-click-weapon
        // enters combat AND arms the crosshair in one gesture). The reset guard above kept
        // us on MOVE until now; runs after it so it wins the frame combat opens.
        if (pendingEnterCrosshair && conn.inCombat()) {
            gameMouseSetMode(GAME_MOUSE_MODE_CROSSHAIR);
            conn.recomputeCombatOutlines();
            pendingEnterCrosshair = false;
        }

        // Recompute the combat-busy gate now that this frame's wire + replay state
        // is settled, and reflect it on the cursor. Busy = in combat AND (not our
        // turn, OR a replay is playing/queued/exit-deferred, OR we have an unanswered
        // action in flight). The animated pocket-watch cursor is a vanilla-faithful
        // "wait" cue; it also stops the hex/crosshair cursor being drawn while busy.
        if (actionPending
            && (conn.combatPresentationBusy() || !conn.myTurn()
                || getTicksBetween(getTicks(), actionPendingSince) >= kActionPendingTimeoutMs)) {
            actionPending = false;
        }
        // clientAnimActiveFor(gDude): S5 in-combat move glide. The dude's committed
        // combat move (cmove) plays as a tile-by-tile glide; lock input for its whole
        // duration (vanilla blocks input while walking), which also covers a long move
        // that outlasts the actionPending round-trip timeout.
        combatBusy = conn.inCombat()
            && (!conn.myTurn() || conn.combatPresentationBusy() || actionPending
                || clientAnimActiveFor(gDude));
        // Softlock diagnostic (F2_TRACE_EVENTS): if the wait cursor holds for a long
        // stretch, name WHICH component keeps combatBusy latched — a stuck myTurn flip,
        // an un-idle replay/door, a queue that won't drain, an unanswered action, or the
        // dude's own glide. One trace beats a stack of hypotheses (viewer has no oracle).
        //
        // The clock measures NO PROGRESS, not "not my turn". Those were the same thing
        // in single player, where !myTurn means AI turns that resolve in seconds — but
        // with a second human on the roster, waiting a minute for them to think is not a
        // softlock, and reporting it as one buries the real thing. Any presentation
        // progress (a drained event, an advancing glide hop — the other player moving
        // one hex) restarts it; a genuine stall makes no progress by definition.
        if (getenv("F2_TRACE_EVENTS") != nullptr) {
            static unsigned int busySince = 0;
            static unsigned int lastBusyTrace = 0;
            static unsigned int busyProgressMark = 0;
            unsigned int nowT = getTicks();
            if (!combatBusy) {
                busySince = 0;
            } else {
                unsigned int progressTick = clientAnimLastProgressTick();
                if (busySince == 0 || progressTick != busyProgressMark) {
                    busySince = nowT;
                    busyProgressMark = progressTick;
                }
                if (getTicksBetween(nowT, busySince) >= 3000
                    && getTicksBetween(nowT, lastBusyTrace) >= 2000) {
                    lastBusyTrace = nowT;
                    fprintf(stderr, "[busy] STUCK %ums myTurn=%d presBusy=%d combatAnim=%d actionPending=%d dudeGlide=%d\n",
                        getTicksBetween(nowT, busySince), conn.myTurn() ? 1 : 0,
                        conn.combatPresentationBusy() ? 1 : 0, clientCombatAnimActive() ? 1 : 0,
                        actionPending ? 1 : 0,
                        clientAnimActiveFor(gDude) ? 1 : 0);
                }
            }
        }
        if (combatBusy && !watchCursorShown) {
            gameMouseSetCursor(MOUSE_CURSOR_WAIT_WATCH);
            watchCursorShown = true;
        } else if (!combatBusy && watchCursorShown) {
            // Hand the cursor back to the pinned MOVE hex (gameMouseRefresh rebuilds
            // it once the animated cursor is cleared and the objects are shown).
            gameMouseSetCursor(MOUSE_CURSOR_NONE);
            gameMouseObjectsShow();
            watchCursorShown = false;
        }

        // Authoritative full-view redraw: the wire may have moved/spawned/connected
        // any object this frame, and the per-event rect refreshes (objectSetLocation
        // etc.) don't cover every path (a freshly _obj_connect'd item, say). A viewer
        // is a pure mirror, so just repaint the whole iso window from current object
        // state each frame — simple and complete.
        tileWindowRefresh();

        renderPresent();

        if (shotEvery > 0 && (frameNo % shotEvery) == 0) {
            takeScreenshot();
        }
        frameNo++;

        sharedFpsLimiter.throttle();
    }

    clientViewerRemoveServiceTicker(); // unregister the modal ticker before conn is destroyed
    conn.close();
    scriptsDisable();
    presReset(); // end any in-flight replay/door reg_anim before the world unloads
    presSetEnabled(false); // also drops any lingering glide (formerly leaked at shutdown)
    main_unload_new();
    return 0;
}

// 0x48118C
static void showDeath()
{
    artCacheFlush();
    colorCycleDisable();
    gameMouseSetCursor(MOUSE_CURSOR_NONE);

    bool oldCursorIsHidden = cursorIsHidden();
    if (oldCursorIsHidden) {
        mouseShowCursor();
    }

    int deathWindowX = (screenGetWidth() - DEATH_WINDOW_WIDTH) / 2;
    int deathWindowY = (screenGetHeight() - DEATH_WINDOW_HEIGHT) / 2;
    int win = windowCreate(deathWindowX,
        deathWindowY,
        DEATH_WINDOW_WIDTH,
        DEATH_WINDOW_HEIGHT,
        0,
        WINDOW_MOVE_ON_TOP);
    if (win != -1) {
        do {
            unsigned char* windowBuffer = windowGetBuffer(win);
            if (windowBuffer == nullptr) {
                break;
            }

            // DEATH.FRM
            FrmImage backgroundFrmImage;
            int fid = buildFid(OBJ_TYPE_INTERFACE, 309, 0, 0, 0);
            if (!backgroundFrmImage.lock(fid)) {
                break;
            }

            while (mouseGetEvent() != 0) {
                sharedFpsLimiter.mark();

                inputGetInput();

                renderPresent();
                sharedFpsLimiter.throttle();
            }

            keyboardReset();
            inputEventQueueReset();

            blitBufferToBuffer(backgroundFrmImage.getData(), 640, 480, 640, windowBuffer, 640);
            backgroundFrmImage.unlock();

            const char* deathFileName = endgameDeathEndingGetFileName();

            if (settings.preferences.subtitles) {
                char text[512];
                if (_mainDeathGrabTextFile(deathFileName, text) == 0) {
                    debugPrint("\n((ShowDeath)): %s\n", text);

                    short beginnings[WORD_WRAP_MAX_COUNT];
                    short count;
                    if (_mainDeathWordWrap(text, 560, beginnings, &count) == 0) {
                        unsigned char* p = windowBuffer + 640 * (480 - fontGetLineHeight() * count - 8);
                        bufferFill(p - 602, 564, fontGetLineHeight() * count + 2, 640, 0);
                        p += 40;
                        for (int index = 0; index < count; index++) {
                            fontDrawText(p, text + beginnings[index], 560, 640, _colorTable[32767]);
                            p += 640 * fontGetLineHeight();
                        }
                    }
                }
            }

            windowRefresh(win);

            colorPaletteLoad("art\\intrface\\death.pal");
            paletteFadeTo(_cmap);

            _main_death_voiceover_done = false;
            speechSetEndCallback(_main_death_voiceover_callback);

            unsigned int delay;
            if (speechLoad(deathFileName, 10, 14, 15) == -1) {
                delay = 3000;
            } else {
                delay = UINT_MAX;
            }

            _gsound_speech_play_preloaded();

            // SFALL: Fix the playback of the speech sound file for the death
            // screen.
            inputBlockForTocks(100);

            unsigned int time = getTicks();
            int keyCode;
            do {
                sharedFpsLimiter.mark();

                keyCode = inputGetInput();

                renderPresent();
                sharedFpsLimiter.throttle();
            } while (keyCode == -1 && !_main_death_voiceover_done && getTicksSince(time) < delay);

            speechSetEndCallback(nullptr);

            speechDelete();

            while (mouseGetEvent() != 0) {
                sharedFpsLimiter.mark();

                inputGetInput();

                renderPresent();
                sharedFpsLimiter.throttle();
            }

            if (keyCode == -1) {
                inputPauseForTocks(500);
            }

            paletteFadeTo(gPaletteBlack);
            colorPaletteLoad("color.pal");
        } while (0);
        windowDestroy(win);
    }

    if (oldCursorIsHidden) {
        mouseHideCursor();
    }

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    colorCycleEnable();
}

// 0x4814A8
static void _main_death_voiceover_callback()
{
    _main_death_voiceover_done = true;
}

// Read endgame subtitle.
//
// 0x4814B4
static int _mainDeathGrabTextFile(const char* fileName, char* dest)
{
    const char* p = strrchr(fileName, '\\');
    if (p == nullptr) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "text\\%s\\cuts\\%s%s", settings.system.language.c_str(), p + 1, ".TXT");

    File* stream = fileOpen(path, "rt");
    if (stream == nullptr) {
        return -1;
    }

    while (true) {
        int c = fileReadChar(stream);
        if (c == -1) {
            break;
        }

        if (c == '\n') {
            c = ' ';
        }

        *dest++ = (c & 0xFF);
    }

    fileClose(stream);

    *dest = '\0';

    return 0;
}

// 0x481598
static int _mainDeathWordWrap(char* text, int width, short* beginnings, short* count)
{
    while (true) {
        char* sep = strchr(text, ':');
        if (sep == nullptr) {
            break;
        }

        if (sep - 1 < text) {
            break;
        }
        sep[0] = ' ';
        sep[-1] = ' ';
    }

    if (wordWrap(text, width, beginnings, count) == -1) {
        return -1;
    }

    // TODO: Probably wrong.
    *count -= 1;

    for (int index = 1; index < *count; index++) {
        char* p = text + beginnings[index];
        while (p >= text && *p != ' ') {
            p--;
            beginnings[index]--;
        }

        if (p != nullptr) {
            *p = '\0';
            beginnings[index]++;
        }
    }

    return 0;
}

} // namespace fallout
