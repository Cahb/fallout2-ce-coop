// Client game lifecycle orchestrators, split out of game.cc so that f2_core no
// longer names the window/palette/font/mouse/movie/dialog/pipboy/char-editor/
// automap/loadsave client-init symbols at link time. These three functions are
// BY NATURE the client boot/reset/teardown sequences: they interleave ~25
// client-presentation init calls with ~35 core-init calls in an ordering-
// sensitive order, so per-site seams can't preserve the interleave — the whole
// function is the only clean cut. The headless server writes its OWN boot path
// calling the same core primitives (gameDbInit / gameLoadGlobalVars / ... which
// remain exported from game.cc in f2_core).

#include "game.h"

#include <stdio.h>
#include <string.h>

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "automap.h"
#include "character_editor.h"
#include "character_selector.h"
#include "color.h"
#include "combat.h"
#include "combat_ai.h"
#include "critter.h"
#include "cycle.h"
#include "db.h"
#include "dbox.h"
#include "debug.h"
#include "display_monitor.h"
#include "draw.h"
#include "endgame.h"
#include "font_manager.h"
#include "game_dialog.h"
#include "game_memory.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "game_ui.h"
#include "input.h"
#include "interface.h"
#include "inventory.h"
#include "item.h"
#include "kb.h"
#include "loadsave.h"
#include "map.h"
#include "memory.h"
#include "mouse.h"
#include "movie.h"
#include "movie_effect.h"
#include "object.h"
#include "options.h"
#include "palette.h"
#include "party_member.h"
#include "presenter_client.h"
#include "script_request_handler_client.h"
#include "perk.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "preferences.h"
#include "presenter.h"
#include "proto.h"
#include "queue.h"
#include "random.h"
#include "savegame.h"
#include "scripts.h"
#include "settings.h"
#include "sfall_arrays.h"
#include "sfall_config.h"
#include "sfall_global_scripts.h"
#include "sfall_global_vars.h"
#include "sfall_ini.h"
#include "sfall_lists.h"
#include "skill.h"
#include "skilldex.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "trait.h"
#include "version.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "worldmap.h"

namespace fallout {

static int gameTakeScreenshot(int width, int height, unsigned char* buffer, unsigned char* palette);

// 0x442580
int gameInitWithOptions(const char* windowTitle, bool isMapper, int font, int a4, int argc, char** argv)
{
    char path[COMPAT_MAX_PATH];

    if (gameMemoryInit() == -1) {
        return -1;
    }

    // Phase 1 presenter seam: this build always runs the client presenter
    // (legacy behavior). The headless server installs a different one.
    presenterInstallClient();

    // Script-request seam (P5 H5): route scriptsHandleRequests /
    // mapHandleTransition UI-bound requests through the client handler. The
    // headless server leaves the null handler installed (drops them).
    scriptRequestHandlerInstallClient();

    // Sfall config should be initialized before game config, since it can
    // override it's file name.
    sfallConfigInit(argc, argv);

    settingsInit(isMapper, argc, argv);

    gIsMapper = isMapper;

    if (gameDbInit() == -1) {
        settingsExit(false);
        sfallConfigExit();
        return -1;
    }

    // Message list repository is considered a specialized file manager, so
    // it should be initialized early in the process.
    messageListRepositoryInit();

    programWindowSetTitle(windowTitle);
    _initWindow(1, a4);
    paletteInit();

    const char* language = settings.system.language.c_str();
    if (compat_stricmp(language, FRENCH) == 0) {
        keyboardSetLayout(KEYBOARD_LAYOUT_FRENCH);
    } else if (compat_stricmp(language, GERMAN) == 0) {
        keyboardSetLayout(KEYBOARD_LAYOUT_GERMAN);
    } else if (compat_stricmp(language, ITALIAN) == 0) {
        keyboardSetLayout(KEYBOARD_LAYOUT_ITALIAN);
    } else if (compat_stricmp(language, SPANISH) == 0) {
        keyboardSetLayout(KEYBOARD_LAYOUT_SPANISH);
    }

    // SFALL: Allow to skip splash screen
    int skipOpeningMovies = 0;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_SKIP_OPENING_MOVIES_KEY, &skipOpeningMovies);

    if (!gIsMapper && skipOpeningMovies < 2) {
        showSplash();
    }

    // CE: Handle debug mode (exactly as seen in `mapper2.exe`).
    const char* debugMode = settings.debug.mode.c_str();
    if (compat_stricmp(debugMode, "environment") == 0) {
        _debug_register_env();
    } else if (compat_stricmp(debugMode, "screen") == 0) {
        _debug_register_screen();
    } else if (compat_stricmp(debugMode, "log") == 0) {
        _debug_register_log("debug.log", "wt");
    } else if (compat_stricmp(debugMode, "mono") == 0) {
        _debug_register_mono();
    } else if (compat_stricmp(debugMode, "gnw") == 0) {
        _debug_register_func(_win_debug);
    }

    interfaceFontsInit();
    fontManagerAdd(&gModernFontManager);
    fontSetCurrent(font);

    screenshotHandlerConfigure(KEY_F12, gameTakeScreenshot);

    tileDisable();

    randomInit();
    badwordsInit();
    skillsInit();
    statsInit();

    if (partyMembersInit() != 0) {
        debugPrint("Failed on partyMember_init\n");
        return -1;
    }

    perksInit();
    traitsInit();
    itemsInit();
    queueInit();
    critterInit();
    aiInit();
    _inven_reset_dude();

    if (gameSoundInit() != 0) {
        debugPrint("Sound initialization failed.\n");
    }

    debugPrint(">gsound_init\t");

    movieInit();
    debugPrint(">initMovie\t\t");

    if (gameMoviesInit() != 0) {
        debugPrint("Failed on gmovie_init\n");
        return -1;
    }

    debugPrint(">gmovie_init\t");

    if (movieEffectsInit() != 0) {
        debugPrint("Failed on moviefx_init\n");
        return -1;
    }

    debugPrint(">moviefx_init\t");

    if (isoInit() != 0) {
        debugPrint("Failed on iso_init\n");
        return -1;
    }

    debugPrint(">iso_init\t");

    if (gameMouseInit() != 0) {
        debugPrint("Failed on gmouse_init\n");
        return -1;
    }

    debugPrint(">gmouse_init\t");

    if (protoInit() != 0) {
        debugPrint("Failed on proto_init\n");
        return -1;
    }

    debugPrint(">proto_init\t");

    animationInit();
    debugPrint(">anim_init\t");

    if (scriptsInit() != 0) {
        debugPrint("Failed on scr_init\n");
        return -1;
    }

    debugPrint(">scr_init\t");

    if (gameLoadGlobalVars() != 0) {
        debugPrint("Failed on game_load_info\n");
        return -1;
    }

    debugPrint(">game_load_info\t");

    if (_scr_game_init() != 0) {
        debugPrint("Failed on scr_game_init\n");
        return -1;
    }

    debugPrint(">scr_game_init\t");

    if (wmWorldMap_init() != 0) {
        debugPrint("Failed on wmWorldMap_init\n");
        return -1;
    }

    debugPrint(">wmWorldMap_init\t");

    characterEditorInit();
    debugPrint(">CharEditInit\t");

    pipboyInit();
    debugPrint(">pip_init\t\t");

    _InitLoadSave();
    lsgInit();
    debugPrint(">InitLoadSave\t");

    if (gameDialogInit() != 0) {
        debugPrint("Failed on gdialog_init\n");
        return -1;
    }

    debugPrint(">gdialog_init\t");

    if (combatInit() != 0) {
        debugPrint("Failed on combat_init\n");
        return -1;
    }

    debugPrint(">combat_init\t");

    if (automapInit() != 0) {
        debugPrint("Failed on automap_init\n");
        return -1;
    }

    debugPrint(">automap_init\t");

    if (!messageListInit(&gMiscMessageList)) {
        debugPrint("Failed on message_init\n");
        return -1;
    }

    debugPrint(">message_init\t");

    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "misc.msg");

    if (!messageListLoad(&gMiscMessageList, path)) {
        debugPrint("Failed on message_load\n");
        return -1;
    }

    debugPrint(">message_load\t");

    if (scriptsDisable() != 0) {
        debugPrint("Failed on scr_disable\n");
        return -1;
    }

    debugPrint(">scr_disable\t");

    if (_init_options_menu() != 0) {
        debugPrint("Failed on init_options_menu\n");
        return -1;
    }

    debugPrint(">init_options_menu\n");

    if (endgameDeathEndingInit() != 0) {
        debugPrint("Failed on endgameDeathEndingInit");
        return -1;
    }

    debugPrint(">endgameDeathEndingInit\n");

    // SFALL
    premadeCharactersInit();

    if (!sfall_gl_vars_init()) {
        debugPrint("Failed on sfall_gl_vars_init");
        return -1;
    }

    if (!sfallListsInit()) {
        debugPrint("Failed on sfallListsInit");
        return -1;
    }

    if (!sfallArraysInit()) {
        debugPrint("Failed on sfallArraysInit");
        return -1;
    }

    if (!sfall_gl_scr_init()) {
        debugPrint("Failed on sfall_gl_scr_init");
        return -1;
    }

    char* customConfigBasePath;
    configGetString(&gSfallConfig, SFALL_CONFIG_SCRIPTS_KEY, SFALL_CONFIG_INI_CONFIG_FOLDER, &customConfigBasePath);
    sfall_ini_set_base_path(customConfigBasePath);

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_MISC, &gMiscMessageList);

    return 0;
}

// 0x442B84
void gameReset()
{
    gameResetSim();

    // The chrome the sim half has no business in. `automapReset` is here rather
    // than in the sim half only because it is built on a file-static
    // `automapCreate`; skipping it headless is safe because a load restores both
    // halves of the automap anyway — savegame.cc copies the slot's AUTOMAP.DB
    // over, and the flags word comes back through automap_state.cc's handler.
    paletteReset();
    gameSoundReset();
    _movieStop();
    movieEffectsReset();
    gameMoviesReset();
    gameMouseReset();
    characterEditorInit();
    pipboyReset();
    gameDialogReset();
    automapReset();
    _init_options_menu();
}

// 0x442C34
void gameExit()
{
    debugPrint("\nGame Exit\n");

    // SFALL
    sfall_gl_scr_exit();
    sfallArraysExit();
    sfallListsExit();
    sfall_gl_vars_exit();
    premadeCharactersExit();

    tileDisable();
    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_MISC, nullptr);
    messageListFree(&gMiscMessageList);
    combatExit();
    gameDialogExit();
    _scr_game_exit();

    // NOTE: Uninline.
    gameFreeGlobalVars();

    scriptsExit();
    animationExit();
    protoExit();
    gameMouseExit();
    isoExit();
    mapVariablesFree();
    movieEffectsExit();
    movieExit();
    gameSoundExit();
    aiExit();
    critterExit();
    itemsExit();
    queueExit();
    perksExit();
    statsExit();
    skillsExit();
    traitsExit();
    randomExit();
    badwordsExit();
    automapExit();
    paletteExit();
    wmWorldMap_exit();
    partyMembersExit();
    endgameDeathEndingExit();
    interfaceFontsExit();
    _windowClose();
    messageListRepositoryExit();
    dbExit();
    settingsExit(true);
    sfallConfigExit();
}

// 0x443EF0
static int gameTakeScreenshot(int width, int height, unsigned char* buffer, unsigned char* palette)
{
    MessageListItem messageListItem;

    if (screenshotHandlerDefaultImpl(width, height, buffer, palette) != 0) {
        // Error saving screenshot.
        messageListItem.num = 8;
        if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }

        return -1;
    }

    // Saved screenshot.
    messageListItem.num = 3;
    if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
        presenter()->consoleMessage(messageListItem.text);
    }

    return 0;
}

} // namespace fallout
