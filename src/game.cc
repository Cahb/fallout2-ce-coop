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

// 0x501C9C
static char _aGame_0[] = "game\\";

// 0x5186B8
static int gGameState = GAME_STATE_0;

// 0x5186BC
bool gIsMapper = false;

// 0x5186C0
int* gGameGlobalVars = nullptr;

// 0x5186C4
int gGameGlobalVarsLength = 0;

// 0x5186C8
const char* asc_5186C8 = _aGame_0;

// 0x5186CC
int _game_user_wants_to_quit = 0;

// See game.h — 1 is the combat-break signal, not a quit.
bool gameTerminalQuitRequested()
{
    return _game_user_wants_to_quit > 1;
}

// misc.msg
//
// 0x58E940
MessageList gMiscMessageList;

// CE: Sonora folks like to store objects in global variables.
static void** gGameGlobalPointers = nullptr;

// 0x443C68
int gameGetGlobalVar(int var)
{
    if (var < 0 || var >= gGameGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference global var out of range: %d", var);
        return 0;
    }

    return gGameGlobalVars[var];
}

// 0x443C98
int gameSetGlobalVar(int var, int value)
{
    if (var < 0 || var >= gGameGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference global var out of range: %d", var);
        return -1;
    }

    // SFALL: Display karma changes.
    if (var == GVAR_PLAYER_REPUTATION) {
        bool shouldDisplayKarmaChanges = false;
        configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_DISPLAY_KARMA_CHANGES_KEY, &shouldDisplayKarmaChanges);
        if (shouldDisplayKarmaChanges) {
            int diff = value - gGameGlobalVars[var];
            if (diff != 0) {
                char formattedMessage[80];
                if (diff > 0) {
                    snprintf(formattedMessage, sizeof(formattedMessage), "You gained %d karma.", diff);
                } else {
                    snprintf(formattedMessage, sizeof(formattedMessage), "You lost %d karma.", -diff);
                }
                presenter()->consoleMessage(formattedMessage);
            }
        }
    }

    gGameGlobalVars[var] = value;

    return 0;
}

// game_load_info
// 0x443CC8
int gameLoadGlobalVars()
{
    if (globalVarsRead("data\\vault13.gam", "GAME_GLOBAL_VARS:", &gGameGlobalVarsLength, &gGameGlobalVars) != 0) {
        return -1;
    }

    gGameGlobalPointers = reinterpret_cast<void**>(internal_malloc(sizeof(*gGameGlobalPointers) * gGameGlobalVarsLength));
    if (gGameGlobalPointers == nullptr) {
        return -1;
    }

    memset(gGameGlobalPointers, 0, sizeof(*gGameGlobalPointers) * gGameGlobalVarsLength);

    return 0;
}

// 0x443CE8
int globalVarsRead(const char* path, const char* section, int* variablesListLengthPtr, int** variablesListPtr)
{
    _inven_reset_dude();

    File* stream = fileOpen(path, "rt");
    if (stream == nullptr) {
        return -1;
    }

    if (*variablesListLengthPtr != 0) {
        internal_free(*variablesListPtr);
        *variablesListPtr = nullptr;
        *variablesListLengthPtr = 0;
    }

    char string[260];
    if (section != nullptr) {
        while (fileReadString(string, 258, stream)) {
            if (strncmp(string, section, 16) == 0) {
                break;
            }
        }
    }

    while (fileReadString(string, 258, stream)) {
        if (string[0] == '\n') {
            continue;
        }

        if (string[0] == '/' && string[1] == '/') {
            continue;
        }

        char* semicolon = strchr(string, ';');
        if (semicolon != nullptr) {
            *semicolon = '\0';
        }

        *variablesListLengthPtr = *variablesListLengthPtr + 1;
        *variablesListPtr = (int*)internal_realloc(*variablesListPtr, sizeof(int) * *variablesListLengthPtr);

        if (*variablesListPtr == nullptr) {
            exit(1);
        }

        char* equals = strchr(string, '=');
        if (equals != nullptr) {
            sscanf(equals + 1, "%d", *variablesListPtr + *variablesListLengthPtr - 1);
        } else {
            *variablesListPtr[*variablesListLengthPtr - 1] = 0;
        }
    }

    fileClose(stream);

    return 0;
}

// 0x443E2C
int gameGetState()
{
    return gGameState;
}

// 0x443E34
int gameRequestState(int newGameState)
{
    switch (newGameState) {
    case GAME_STATE_0:
        newGameState = GAME_STATE_1;
        break;
    case GAME_STATE_2:
        newGameState = GAME_STATE_3;
        break;
    case GAME_STATE_4:
        newGameState = GAME_STATE_5;
        break;
    }

    if (gGameState == GAME_STATE_4 && newGameState == GAME_STATE_5) {
        return -1;
    }

    gGameState = newGameState;
    return 0;
}

// 0x443E90
void gameUpdateState()
{
    switch (gGameState) {
    case GAME_STATE_1:
        gGameState = GAME_STATE_0;
        break;
    case GAME_STATE_3:
        gGameState = GAME_STATE_2;
        break;
    case GAME_STATE_5:
        gGameState = GAME_STATE_4;
        break;
    }
}

// NOTE: Inlined.
//
// 0x443F50
void gameFreeGlobalVars()
{
    gGameGlobalVarsLength = 0;
    if (gGameGlobalVars != nullptr) {
        internal_free(gGameGlobalVars);
        gGameGlobalVars = nullptr;
    }

    if (gGameGlobalPointers != nullptr) {
        internal_free(gGameGlobalPointers);
        gGameGlobalPointers = nullptr;
    }
}

// The sim half of the client's gameReset (game_lifecycle.cc, 0x442B84), split
// out so a headless load can reset the world without a screen to tear down.
// Order within this list is the original order; see the note in game.h.
void gameResetSim()
{
    tileDisable();
    randomReset();
    skillsReset();
    statsReset();
    perksReset();
    traitsReset();
    itemsReset();
    queueExit();
    animationReset();
    lsgInit();
    critterReset();
    aiReset();
    _inven_reset_dude();
    isoReset();
    protoReset();
    _scr_reset();
    gameLoadGlobalVars();
    scriptsReset();
    wmWorldMap_reset();
    partyMembersReset();
    _ResetLoadSave();
    combatReset();
    _game_user_wants_to_quit = 0;

    // SFALL
    sfall_gl_vars_reset();
    sfallListsReset();
    messageListRepositoryReset();
    sfallArraysReset();
    sfall_gl_scr_reset();
}

// 0x44418C
int gameDbInit()
{
    const char* main_file_name;
    const char* patch_file_name;
    int patch_index;
    char filename[COMPAT_MAX_PATH];

    main_file_name = nullptr;
    patch_file_name = nullptr;

    main_file_name = settings.system.master_dat_path.c_str();
    if (*main_file_name == '\0') {
        main_file_name = nullptr;
    }

    patch_file_name = settings.system.master_patches_path.c_str();
    if (*patch_file_name == '\0') {
        patch_file_name = nullptr;
    }

    int master_db_handle = dbOpen(main_file_name, 0, patch_file_name, 1);
    if (master_db_handle == -1) {
        showMesageBox("Could not find the master datafile. Please make sure the FALLOUT CD is in the drive and that you are running FALLOUT from the directory you installed it to.");
        return -1;
    }

    main_file_name = settings.system.critter_dat_path.c_str();
    if (*main_file_name == '\0') {
        main_file_name = nullptr;
    }

    patch_file_name = settings.system.critter_patches_path.c_str();
    if (*patch_file_name == '\0') {
        patch_file_name = nullptr;
    }

    int critter_db_handle = dbOpen(main_file_name, 0, patch_file_name, 1);
    if (critter_db_handle == -1) {
        showMesageBox("Could not find the critter datafile. Please make sure the FALLOUT CD is in the drive and that you are running FALLOUT from the directory you installed it to.");
        return -1;
    }

    // SFALL: custom patch file name.
    const char* path_file_name_template = nullptr;
    {
        char* configVal = nullptr;
        configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_PATCH_FILE, &configVal);
        if (configVal != nullptr && *configVal != '\0') {
            path_file_name_template = configVal;
        }
    }
    if (path_file_name_template == nullptr) {
        path_file_name_template = "patch%03d.dat";
    }

    for (patch_index = 0; patch_index < 1000; patch_index++) {
        snprintf(filename, sizeof(filename), path_file_name_template, patch_index);

        if (compat_access(filename, 0) == 0) {
            dbOpen(filename, 0, nullptr, 1);
        }
    }

    if (compat_access("f2_res.dat", 0) == 0) {
        dbOpen("f2_res.dat", 0, nullptr, 1);
    }

    return 0;
}

void* gameGetGlobalPointer(int var)
{
    if (var < 0 || var >= gGameGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference global pointer out of range: %d", var);
        return nullptr;
    }

    return gGameGlobalPointers[var];
}

int gameSetGlobalPointer(int var, void* value)
{
    if (var < 0 || var >= gGameGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference global var out of range: %d", var);
        return -1;
    }

    gGameGlobalPointers[var] = value;

    return 0;
}

int GameMode::currentGameMode = 0;

void GameMode::enterGameMode(int gameMode)
{
    currentGameMode |= gameMode;
}

void GameMode::exitGameMode(int gameMode)
{
    currentGameMode &= ~gameMode;
}

bool GameMode::isInGameMode(int gameMode)
{
    return (currentGameMode & gameMode) != 0;
}

ScopedGameMode::ScopedGameMode(int gameMode)
{
    this->gameMode = gameMode;
    GameMode::enterGameMode(gameMode);
}

ScopedGameMode::~ScopedGameMode()
{
    GameMode::exitGameMode(gameMode);
}

} // namespace fallout
