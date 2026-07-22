#include "savegame.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include "automap.h"
#include "combat.h"
#include "combat_ai.h"
#include "critter.h"
#include "db.h"
#include "debug.h"
#include "display_monitor.h"
#include "file_utils.h"
#include "game.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "interface.h"
#include "item.h"
#include "loadsave.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "player_sheet.h"
#include "preferences.h"
#include "character_editor.h"
#include "proto.h"
#include "queue.h"
#include "random.h"
#include "scripts.h"
#include "settings.h"
#include "sfall_global_scripts.h"
#include "sfall_global_vars.h"
#include "skill.h"
#include "stat.h"
#include "tile.h"
#include "trait.h"
#include "version.h"
#include "worldmap.h"

namespace fallout {

#define LOAD_SAVE_SIGNATURE "FALLOUT SAVE FILE"

#define LS_PREVIEW_WIDTH 224
#define LS_PREVIEW_HEIGHT 133
#define LS_PREVIEW_SIZE ((LS_PREVIEW_WIDTH) * (LS_PREVIEW_HEIGHT))

// NOTE: The following are "normalized" path components for "proto/critters" and
// "proto/items". The original code does not use uniform case for them (as
// opposed to other path components like MAPS, SAVE.DAT, etc). It does not have
// effect on Windows, but it's important on Linux and Mac, where filesystem is
// case-sensitive.

#define PROTO_DIR_NAME "proto"
#define CRITTERS_DIR_NAME "critters"
#define ITEMS_DIR_NAME "items"
#define PROTO_FILE_EXT "pro"

static int _DummyFunc(File* stream);
static int _PrepLoad(File* stream);
static int _EndLoad(File* stream);
static int _GameMap2Slot(File* stream);
static int _SlotMap2Game(File* stream);
static int _mygets(char* dest, File* stream);
static int _copy_file(const char* existingFileName, const char* newFileName);
static int _SaveBackup();
static int _RestoreSave();
static int _LoadObjDudeCid(File* stream);
static int _SaveObjDudeCid(File* stream);

typedef int LoadGameHandler(File* stream);
typedef int SaveGameHandler(File* stream);

// 0x5193EC
static SaveGameHandler* _master_save_list[LOAD_SAVE_HANDLER_COUNT] = {
    _DummyFunc,
    _SaveObjDudeCid,
    scriptsSaveGameGlobalVars,
    _GameMap2Slot,
    scriptsSaveGameGlobalVars,
    _obj_save_dude,
    critterSave,
    killsSave,
    skillsSave,
    randomSave,
    perksSave,
    combatSave,
    aiSave,
    statsSave,
    itemsSave,
    traitsSave,
    automapSave,
    preferencesSave,
    characterEditorSave,
    wmWorldMap_save,
    pipboySave,
    gameMoviesSave,
    skillsUsageSave,
    partyMembersSave,
    queueSave,
    interfaceSave,
    _DummyFunc,
};

// 0x519458
static LoadGameHandler* _master_load_list[LOAD_SAVE_HANDLER_COUNT] = {
    _PrepLoad,
    _LoadObjDudeCid,
    scriptsLoadGameGlobalVars,
    _SlotMap2Game,
    scriptsSkipGameGlobalVars,
    _obj_load_dude,
    critterLoad,
    killsLoad,
    skillsLoad,
    randomLoad,
    perksLoad,
    combatLoad,
    aiLoad,
    statsLoad,
    itemsLoad,
    traitsLoad,
    automapLoad,
    preferencesLoad,
    characterEditorLoad,
    wmWorldMap_load,
    pipboyLoad,
    gameMoviesLoad,
    skillsUsageLoad,
    partyMembersLoad,
    queueLoad,
    interfaceLoad,
    _EndLoad,
};

static const char* _patches = nullptr;
static int _slot_cursor = 0;
static int _map_backup_count = -1;
static bool _automap_db_flag = false;
static bool _loadingGame = false;
// 11, not 10: index 10 (directory SLOT11) is the dedicated-server AUTOSAVE
// slot (server_admin.cc). It sits past everything the vanilla client's save/
// load screens enumerate (SLOT01..10), which is precisely what makes a
// periodic unattended save unable to clobber a real one.
static LoadSaveSlotData _LSData[11];
static unsigned char* _snapshotBuf = nullptr;
static char _gmpath[COMPAT_MAX_PATH];
static char _str[COMPAT_MAX_PATH];
static char _str0[COMPAT_MAX_PATH];
static char _str1[COMPAT_MAX_PATH];
static char _str2[COMPAT_MAX_PATH];
static File* _flptr;
static int _ls_error_code;

void savegameSetSlot(int slot)
{
    _slot_cursor = slot;
}

int savegameGetSlot()
{
    return _slot_cursor;
}

void savegameSetPreviewBuffer(unsigned char* buffer)
{
    _snapshotBuf = buffer;
}

LoadSaveSlotData* savegameSlotData(int slot)
{
    return &(_LSData[slot]);
}

void savegameRefreshPatchesPath()
{
    _patches = settings.system.master_patches_path.c_str();
}

int savegameGetErrorCode()
{
    return _ls_error_code;
}

void savegameResetErrorCode()
{
    _ls_error_code = 0;
}

// 0x47D88C
int lsgPerformSaveGame()
{
    _ls_error_code = 0;
    _map_backup_count = -1;
    gameMouseSetCursor(MOUSE_CURSOR_WAIT_PLANET);

    backgroundSoundPause();

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s", _patches, "SAVEGAME");
    compat_mkdir(_gmpath);

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    compat_mkdir(_gmpath);

    strcat(_gmpath, "\\" PROTO_DIR_NAME);
    compat_mkdir(_gmpath);

    char* protoBasePath = _gmpath + strlen(_gmpath);

    strcpy(protoBasePath, "\\" CRITTERS_DIR_NAME);
    compat_mkdir(_gmpath);

    strcpy(protoBasePath, "\\" ITEMS_DIR_NAME);
    compat_mkdir(_gmpath);

    if (_SaveBackup() == -1) {
        debugPrint("\nLOADSAVE: Warning, can't backup save file!\n");
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcat(_gmpath, "SAVE.DAT");

    debugPrint("\nLOADSAVE: Save name: %s\n", _gmpath);

    _flptr = fileOpen(_gmpath, "wb");
    if (_flptr == nullptr) {
        debugPrint("\nLOADSAVE: ** Error opening save game for writing! **\n");
        _RestoreSave();
        snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
        MapDirErase(_gmpath, "BAK");
        _partyMemberUnPrepSave();
        backgroundSoundResume();
        return -1;
    }

    long pos = fileTell(_flptr);
    if (lsgSaveHeaderInSlot(_flptr, _slot_cursor) == -1) {
        debugPrint("\nLOADSAVE: ** Error writing save game header! **\n");
        debugPrint("LOADSAVE: Save file header size written: %d bytes.\n", fileTell(_flptr) - pos);
        fileClose(_flptr);
        _RestoreSave();
        snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
        MapDirErase(_gmpath, "BAK");
        _partyMemberUnPrepSave();
        backgroundSoundResume();
        return -1;
    }

    for (int index = 0; index < LOAD_SAVE_HANDLER_COUNT; index++) {
        long pos = fileTell(_flptr);
        SaveGameHandler* handler = _master_save_list[index];
        if (handler(_flptr) == -1) {
            debugPrint("\nLOADSAVE: ** Error writing save function #%d data! **\n", index);
            fileClose(_flptr);
            _RestoreSave();
            snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
            MapDirErase(_gmpath, "BAK");
            _partyMemberUnPrepSave();
            backgroundSoundResume();
            return -1;
        }

        debugPrint("LOADSAVE: Save function #%d data size written: %d bytes.\n", index, fileTell(_flptr) - pos);
    }

    debugPrint("LOADSAVE: Total save data written: %ld bytes.\n", fileTell(_flptr));

    // Co-op tail: the extra player actors' bodies + sheets, after the 27 vanilla
    // handlers. Writes nothing (not even a magic) when there are no extras, so a
    // single-player save stays byte-identical to vanilla. No version bump — a
    // stock build still loads this as a host-only game (owner ruling 2026-07-21).
    if (playerActorAppendixSave(_flptr) == -1) {
        debugPrint("\nLOADSAVE: ** Error writing co-op actor appendix! **\n");
        fileClose(_flptr);
        _RestoreSave();
        snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
        MapDirErase(_gmpath, "BAK");
        _partyMemberUnPrepSave();
        backgroundSoundResume();
        return -1;
    }

    fileClose(_flptr);

    // SFALL: Save sfallgv.sav.
    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcat(_gmpath, "sfallgv.sav");

    _flptr = fileOpen(_gmpath, "wb");
    if (_flptr != nullptr) {
        do {
            if (!sfall_gl_vars_save(_flptr)) {
                debugPrint("LOADSAVE (SFALL): ** Error saving global vars **\n");
                break;
            }

            // TODO: For now fill remaining sections with zeros to that Sfall
            // can successfully read our global vars and skip the rest.

            int nextObjectId = 0;
            if (fileWrite(&nextObjectId, sizeof(nextObjectId), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving next object id **\n");
                break;
            }

            int addedYears = 0;
            if (fileWrite(&addedYears, sizeof(addedYears), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving added years **\n");
                break;
            }

            int fakeTraitsCount = 0;
            if (fileWrite(&fakeTraitsCount, sizeof(fakeTraitsCount), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving fake traits **\n");
                break;
            }

            int fakePerksCount = 0;
            if (fileWrite(&fakePerksCount, sizeof(fakePerksCount), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving fake perks **\n");
                break;
            }

            int fakeSelectablePerksCount = 0;
            if (fileWrite(&fakeSelectablePerksCount, sizeof(fakeSelectablePerksCount), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving fake selectable perks **\n");
                break;
            }

            int arraysCountOld = 0;
            if (fileWrite(&arraysCountOld, sizeof(arraysCountOld), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving arrays (old fmt) **\n");
                break;
            }

            int arraysCountNew = 0;
            if (fileWrite(&arraysCountNew, sizeof(arraysCountNew), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving arrays (new fmt) **\n");
                break;
            }

            int drugPidsCount = 0;
            if (fileWrite(&drugPidsCount, sizeof(drugPidsCount), 1, _flptr) != 1) {
                debugPrint("LOADSAVE (SFALL): ** Error saving drug pids **\n");
                break;
            }
        } while (0);

        fileClose(_flptr);
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    MapDirErase(_gmpath, "BAK");

    // The "game saved" notification is raised by the caller: it reads the
    // slot-picker screen's message list, which a headless writer never loads.

    backgroundSoundResume();

    return 0;
}

// 0x47DC60
bool _isLoadingGame()
{
    return _loadingGame;
}

// 0x47DC68
int lsgLoadGameInSlot(int slot)
{
    _loadingGame = true;

    if (isInCombat()) {
        interfaceBarEndButtonsHide(false);
        _combat_over_from_load();
        gameMouseSetCursor(MOUSE_CURSOR_WAIT_PLANET);
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcat(_gmpath, "SAVE.DAT");

    LoadSaveSlotData* ptr = &(_LSData[slot]);
    debugPrint("\nLOADSAVE: Load name: %s\n", ptr->description);

    _flptr = fileOpen(_gmpath, "rb");
    if (_flptr == nullptr) {
        debugPrint("\nLOADSAVE: ** Error opening load game file for reading! **\n");
        _loadingGame = false;
        return -1;
    }

    long pos = fileTell(_flptr);
    if (lsgLoadHeaderInSlot(_flptr, slot) == -1) {
        debugPrint("\nLOADSAVE: ** Error reading save  game header! **\n");
        fileClose(_flptr);
        gameReset();
        _loadingGame = false;
        return -1;
    }

    debugPrint("LOADSAVE: Load file header size read: %d bytes.\n", fileTell(_flptr) - pos);

    for (int index = 0; index < LOAD_SAVE_HANDLER_COUNT; index += 1) {
        long pos = fileTell(_flptr);
        LoadGameHandler* handler = _master_load_list[index];
        if (handler(_flptr) == -1) {
            debugPrint("\nLOADSAVE: ** Error reading load function #%d data! **\n", index);
            int v12 = fileTell(_flptr);
            debugPrint("LOADSAVE: Load function #%d data size read: %d bytes.\n", index, fileTell(_flptr) - pos);
            fileClose(_flptr);
            gameReset();
            _loadingGame = false;
            return -1;
        }

        debugPrint("LOADSAVE: Load function #%d data size read: %d bytes.\n", index, fileTell(_flptr) - pos);
    }

    debugPrint("LOADSAVE: Total load data read: %ld bytes.\n", fileTell(_flptr));

    // Co-op tail: reconstruct the extra player actors and apply every actor's
    // sheet. Reached with the map present (_SlotMap2Game loaded it inside the
    // handler loop) and the host restored (_obj_load_dude), which is what the
    // extras are placed on and sheet slot 0 aliases. A no-op (returns 0) on a
    // vanilla/single-player save, which ends right here at EOF.
    if (playerActorAppendixLoad(_flptr) == -1) {
        debugPrint("\nLOADSAVE: ** Error reading co-op actor appendix! **\n");
        fileClose(_flptr);
        gameReset();
        _loadingGame = false;
        return -1;
    }

    fileClose(_flptr);

    // SFALL: Load sfallgv.sav.
    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcat(_gmpath, "sfallgv.sav");

    _flptr = fileOpen(_gmpath, "rb");
    if (_flptr != nullptr) {
        do {
            if (!sfall_gl_vars_load(_flptr)) {
                debugPrint("LOADSAVE (SFALL): ** Error loading global vars **\n");
                break;
            }

            // TODO: For now silently ignore remaining sections.
        } while (0);

        fileClose(_flptr);
    }

    snprintf(_str, sizeof(_str), "%s\\", "MAPS");
    MapDirErase(_str, "BAK");
    _proto_dude_update_gender();

    // The "game loaded" notification is raised by the caller, for the same
    // reason as the "game saved" one above.

    _loadingGame = false;

    // SFALL: Start global scripts.
    sfall_gl_scr_exec_start_proc();

    return 0;
}

// 0x47DF10
int lsgSaveHeaderInSlot(File* stream, int slot)
{
    _ls_error_code = 4;

    LoadSaveSlotData* ptr = &(_LSData[slot]);
    strncpy(ptr->signature, LOAD_SAVE_SIGNATURE, 24);

    if (fileWrite(ptr->signature, 1, 24, stream) == -1) {
        return -1;
    }

    short temp[3];
    temp[0] = VERSION_MAJOR;
    temp[1] = VERSION_MINOR;

    ptr->versionMinor = temp[0];
    ptr->versionMajor = temp[1];

    if (fileWriteInt16List(stream, temp, 2) == -1) {
        return -1;
    }

    ptr->versionRelease = VERSION_RELEASE;
    if (fileWriteUInt8(stream, VERSION_RELEASE) == -1) {
        return -1;
    }

    char* characterName = critterGetName(gDude);
    strncpy(ptr->characterName, characterName, 32);

    if (fileWrite(ptr->characterName, 32, 1, stream) != 1) {
        return -1;
    }

    if (fileWrite(ptr->description, 30, 1, stream) != 1) {
        return -1;
    }

    time_t now = time(nullptr);
    struct tm* local = localtime(&now);

    temp[0] = local->tm_mday;
    temp[1] = local->tm_mon + 1;
    temp[2] = local->tm_year + 1900;

    ptr->fileDay = temp[0];
    ptr->fileMonth = temp[1];
    ptr->fileYear = temp[2];
    ptr->fileTime = local->tm_hour + local->tm_min;

    if (fileWriteInt16List(stream, temp, 3) == -1) {
        return -1;
    }

    if (_db_fwriteLong(stream, ptr->fileTime) == -1) {
        return -1;
    }

    int month;
    int day;
    int year;
    gameTimeGetDate(&month, &day, &year);

    temp[0] = month;
    temp[1] = day;
    temp[2] = year;
    ptr->gameTime = gameTimeGetTime();

    if (fileWriteInt16List(stream, temp, 3) == -1) {
        return -1;
    }

    if (fileWriteUInt32(stream, ptr->gameTime) == -1) {
        return -1;
    }

    ptr->elevation = gElevation;
    if (fileWriteInt16(stream, ptr->elevation) == -1) {
        return -1;
    }

    ptr->map = mapGetCurrentMap();
    if (fileWriteInt16(stream, ptr->map) == -1) {
        return -1;
    }

    char mapName[128];
    strcpy(mapName, gMapHeader.name);

    // NOTE: Uppercased from "sav".
    char* v1 = _strmfe(_str, mapName, "SAV");
    strncpy(ptr->fileName, v1, 16);
    if (fileWrite(ptr->fileName, 16, 1, stream) != 1) {
        return -1;
    }

    // The preview thumbnail is grabbed off the screen, so a headless writer has
    // none. The block is still part of the format, so emit it blank rather than
    // skipping it — a save written without a preview must stay loadable.
    if (_snapshotBuf != nullptr) {
        if (fileWrite(_snapshotBuf, LS_PREVIEW_SIZE, 1, stream) != 1) {
            return -1;
        }
    } else {
        unsigned char blank[64];
        memset(blank, 0, sizeof(blank));
        for (int remaining = LS_PREVIEW_SIZE; remaining > 0;) {
            int chunk = std::min(remaining, static_cast<int>(sizeof(blank)));
            if (fileWrite(blank, chunk, 1, stream) != 1) {
                return -1;
            }
            remaining -= chunk;
        }
    }

    memset(mapName, 0, 128);
    if (fileWrite(mapName, 1, 128, stream) != 128) {
        return -1;
    }

    _ls_error_code = 0;

    return 0;
}

// 0x47E2E4
int lsgLoadHeaderInSlot(File* stream, int slot)
{
    _ls_error_code = 3;

    LoadSaveSlotData* ptr = &(_LSData[slot]);

    if (fileRead(ptr->signature, 1, 24, stream) != 24) {
        return -1;
    }

    if (strncmp(ptr->signature, LOAD_SAVE_SIGNATURE, 18) != 0) {
        debugPrint("\nLOADSAVE: ** Invalid save file on load! **\n");
        _ls_error_code = 2;
        return -1;
    }

    short v8[3];
    if (fileReadInt16List(stream, v8, 2) == -1) {
        return -1;
    }

    ptr->versionMinor = v8[0];
    ptr->versionMajor = v8[1];

    if (fileReadUInt8(stream, &(ptr->versionRelease)) == -1) {
        return -1;
    }

    if (ptr->versionMinor != 1 || ptr->versionMajor != 2 || ptr->versionRelease != 'R') {
        debugPrint("\nLOADSAVE: Load slot #%d Version: %d.%d%c\n", slot, ptr->versionMinor, ptr->versionMajor, ptr->versionRelease);
        _ls_error_code = 1;
        return -1;
    }

    if (fileRead(ptr->characterName, 32, 1, stream) != 1) {
        return -1;
    }

    if (fileRead(ptr->description, 30, 1, stream) != 1) {
        return -1;
    }

    if (fileReadInt16List(stream, v8, 3) == -1) {
        return -1;
    }

    ptr->fileMonth = v8[0];
    ptr->fileDay = v8[1];
    ptr->fileYear = v8[2];

    if (_db_freadInt(stream, &(ptr->fileTime)) == -1) {
        return -1;
    }

    if (fileReadInt16List(stream, v8, 3) == -1) {
        return -1;
    }

    ptr->gameMonth = v8[0];
    ptr->gameDay = v8[1];
    ptr->gameYear = v8[2];

    if (fileReadUInt32(stream, &(ptr->gameTime)) == -1) {
        return -1;
    }

    if (fileReadInt16(stream, &(ptr->elevation)) == -1) {
        return -1;
    }

    if (fileReadInt16(stream, &(ptr->map)) == -1) {
        return -1;
    }

    if (fileRead(ptr->fileName, 1, 16, stream) != 16) {
        return -1;
    }

    if (fileSeek(stream, LS_PREVIEW_SIZE, SEEK_CUR) != 0) {
        return -1;
    }

    if (fileSeek(stream, 128, 1) != 0) {
        return -1;
    }

    _ls_error_code = 0;

    return 0;
}

// 0x47F48C
static int _DummyFunc(File* stream)
{
    return 0;
}

// 0x47F490
static int _PrepLoad(File* stream)
{
    gameReset();
    gameMouseSetCursor(MOUSE_CURSOR_WAIT_PLANET);
    gMapHeader.name[0] = '\0';
    gameTimeSetTime(_LSData[_slot_cursor].gameTime);
    return 0;
}

// 0x47F4C8
static int _EndLoad(File* stream)
{
    wmMapMusicStart();
    dudeSetName(_LSData[_slot_cursor].characterName);
    interfaceBarRefresh();
    indicatorBarRefresh();
    tileWindowRefresh();
    if (isInCombat()) {
        scriptsRequestCombat(nullptr);
    }
    return 0;
}

// 0x47F510
static int _GameMap2Slot(File* stream)
{
    if (_partyMemberPrepSave() == -1) {
        return -1;
    }

    if (_map_save_in_game(false) == -1) {
        return -1;
    }

    for (int index = 1; index < gPartyMemberDescriptionsLength; index += 1) {
        int pid = gPartyMemberPids[index];
        if (pid == -2) {
            continue;
        }

        char path[COMPAT_MAX_PATH];
        if (_proto_list_str(pid, path) != 0) {
            continue;
        }

        const char* critterItemPath = (pid >> 24) == OBJ_TYPE_CRITTER
            ? PROTO_DIR_NAME "\\" CRITTERS_DIR_NAME
            : PROTO_DIR_NAME "\\" ITEMS_DIR_NAME;
        snprintf(_str0, sizeof(_str0), "%s\\%s\\%s", _patches, critterItemPath, path);
        snprintf(_str1, sizeof(_str1), "%s\\%s\\%s%.2d\\%s\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, critterItemPath, path);
        if (fileCopyCompressed(_str0, _str1) == -1) {
            return -1;
        }
    }

    snprintf(_str0, sizeof(_str0), "%s\\*.%s", "MAPS", "SAV");

    char** fileNameList;
    int fileNameListLength = fileNameListInit(_str0, &fileNameList, 0, 0);
    if (fileNameListLength == -1) {
        return -1;
    }

    if (fileWriteInt32(stream, fileNameListLength) == -1) {
        fileNameListFree(&fileNameList, 0);
        return -1;
    }

    if (fileNameListLength == 0) {
        fileNameListFree(&fileNameList, 0);
        return -1;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);

    if (MapDirErase(_gmpath, "SAV") == -1) {
        fileNameListFree(&fileNameList, 0);
        return -1;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    _strmfe(_str0, "AUTOMAP.DB", "SAV");
    strcat(_gmpath, _str0);
    compat_remove(_gmpath);

    for (int index = 0; index < fileNameListLength; index += 1) {
        char* string = fileNameList[index];
        if (fileWrite(string, strlen(string) + 1, 1, stream) == -1) {
            fileNameListFree(&fileNameList, 0);
            return -1;
        }

        snprintf(_str0, sizeof(_str0), "%s\\%s\\%s", _patches, "MAPS", string);
        snprintf(_str1, sizeof(_str1), "%s\\%s\\%s%.2d\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, string);
        if (fileCopyCompressed(_str0, _str1) == -1) {
            fileNameListFree(&fileNameList, 0);
            return -1;
        }
    }

    fileNameListFree(&fileNameList, 0);

    _strmfe(_str0, "AUTOMAP.DB", "SAV");
    snprintf(_str1, sizeof(_str1), "%s\\%s\\%s%.2d\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, _str0);
    snprintf(_str0, sizeof(_str0), "%s\\%s\\%s", _patches, "MAPS", "AUTOMAP.DB");

    if (fileCopyCompressed(_str0, _str1) == -1) {
        return -1;
    }

    snprintf(_str0, sizeof(_str0), "%s\\%s", "MAPS", "AUTOMAP.DB");
    File* inStream = fileOpen(_str0, "rb");
    if (inStream == nullptr) {
        return -1;
    }

    int fileSize = fileGetSize(inStream);
    if (fileSize == -1) {
        fileClose(inStream);
        return -1;
    }

    fileClose(inStream);

    if (fileWriteInt32(stream, fileSize) == -1) {
        return -1;
    }

    if (_partyMemberUnPrepSave() == -1) {
        return -1;
    }

    return 0;
}

// SlotMap2Game
// 0x47F990
static int _SlotMap2Game(File* stream)
{
    debugPrint("LOADSAVE: in SlotMap2Game\n");

    int fileNameListLength;
    if (fileReadInt32(stream, &fileNameListLength) == -1) {
        debugPrint("LOADSAVE: returning 1\n");
        return -1;
    }

    if (fileNameListLength == 0) {
        debugPrint("LOADSAVE: returning 2\n");
        return -1;
    }

    snprintf(_str0, sizeof(_str0), "%s\\", PROTO_DIR_NAME "\\" CRITTERS_DIR_NAME);

    if (MapDirErase(_str0, PROTO_FILE_EXT) == -1) {
        debugPrint("LOADSAVE: returning 3\n");
        return -1;
    }

    snprintf(_str0, sizeof(_str0), "%s\\", PROTO_DIR_NAME "\\" ITEMS_DIR_NAME);
    if (MapDirErase(_str0, PROTO_FILE_EXT) == -1) {
        debugPrint("LOADSAVE: returning 4\n");
        return -1;
    }

    snprintf(_str0, sizeof(_str0), "%s\\", "MAPS");
    if (MapDirErase(_str0, "SAV") == -1) {
        debugPrint("LOADSAVE: returning 5\n");
        return -1;
    }

    snprintf(_str0, sizeof(_str0), "%s\\%s\\%s", _patches, "MAPS", "AUTOMAP.DB");
    compat_remove(_str0);

    for (int index = 1; index < gPartyMemberDescriptionsLength; index += 1) {
        int pid = gPartyMemberPids[index];
        if (pid != -2) {
            char protoPath[COMPAT_MAX_PATH];
            if (_proto_list_str(pid, protoPath) == 0) {
                const char* basePath = PID_TYPE(pid) == OBJ_TYPE_CRITTER
                    ? PROTO_DIR_NAME "\\" CRITTERS_DIR_NAME
                    : PROTO_DIR_NAME "\\" ITEMS_DIR_NAME;
                snprintf(_str0, sizeof(_str0), "%s\\%s\\%s", _patches, basePath, protoPath);
                snprintf(_str1, sizeof(_str1), "%s\\%s\\%s%.2d\\%s\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, basePath, protoPath);

                if (_gzdecompress_file(_str1, _str0) == -1) {
                    debugPrint("LOADSAVE: returning 6\n");
                    return -1;
                }
            }
        }
    }

    for (int index = 0; index < fileNameListLength; index += 1) {
        char fileName[COMPAT_MAX_PATH];
        if (_mygets(fileName, stream) == -1) {
            break;
        }

        snprintf(_str0, sizeof(_str0), "%s\\%s\\%s%.2d\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, fileName);
        snprintf(_str1, sizeof(_str1), "%s\\%s\\%s", _patches, "MAPS", fileName);

        if (_gzdecompress_file(_str0, _str1) == -1) {
            debugPrint("LOADSAVE: returning 7\n");
            return -1;
        }
    }

    const char* automapFileName = _strmfe(_str1, "AUTOMAP.DB", "SAV");
    snprintf(_str0, sizeof(_str0), "%s\\%s\\%s%.2d\\%s", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1, automapFileName);
    snprintf(_str1, sizeof(_str1), "%s\\%s\\%s", _patches, "MAPS", "AUTOMAP.DB");
    if (fileCopyDecompressed(_str0, _str1) == -1) {
        debugPrint("LOADSAVE: returning 8\n");
        return -1;
    }

    snprintf(_str1, sizeof(_str1), "%s\\%s", "MAPS", "AUTOMAP.DB");

    int v12;
    if (fileReadInt32(stream, &v12) == -1) {
        debugPrint("LOADSAVE: returning 9\n");
        return -1;
    }

    if (mapLoadSaved(_LSData[_slot_cursor].fileName) == -1) {
        debugPrint("LOADSAVE: returning 13\n");
        return -1;
    }

    return 0;
}

// 0x47FE14
static int _mygets(char* dest, File* stream)
{
    int index = 14;
    while (true) {
        int c = fileReadChar(stream);
        if (c == -1) {
            return -1;
        }

        index -= 1;

        *dest = c & 0xFF;
        dest += 1;

        if (index == -1 || c == '\0') {
            break;
        }
    }

    if (index == 0) {
        return -1;
    }

    return 0;
}

// 0x47FE58
static int _copy_file(const char* existingFileName, const char* newFileName)
{
    File* stream1;
    File* stream2;
    int length;
    int chunk_length;
    void* buf;
    int result;

    stream1 = nullptr;
    stream2 = nullptr;
    buf = nullptr;
    result = -1;

    stream1 = fileOpen(existingFileName, "rb");
    if (stream1 == nullptr) {
        goto out;
    }

    length = fileGetSize(stream1);
    if (length == -1) {
        goto out;
    }

    stream2 = fileOpen(newFileName, "wb");
    if (stream2 == nullptr) {
        goto out;
    }

    buf = internal_malloc(0xFFFF);
    if (buf == nullptr) {
        goto out;
    }

    while (length != 0) {
        chunk_length = std::min(length, 0xFFFF);

        if (fileRead(buf, chunk_length, 1, stream1) != 1) {
            break;
        }

        if (fileWrite(buf, chunk_length, 1, stream2) != 1) {
            break;
        }

        length -= chunk_length;
    }

    if (length != 0) {
        goto out;
    }

    result = 0;

out:

    if (stream1 != nullptr) {
        fileClose(stream1);
    }

    if (stream2 != nullptr) {
        fileClose(stream2);
    }

    if (buf != nullptr) {
        internal_free(buf);
    }

    return result;
}

// InitLoadSave
// 0x48000C
//
// Moved here from loadsave.cc (the slot-picker screen) with `_ResetLoadSave`:
// both are pure savegame-scratch cleanup built on MapDirErase, with no screen
// in them, and the headless sim reset has to run them too — a load that leaves
// stale MAPS\*.SAV behind reads those maps back instead of the save's.
void lsgInit()
{
    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s\\", "MAPS");
    MapDirErase(path, "SAV");
}

// 0x47B85C
void _ResetLoadSave()
{
    MapDirErase("MAPS\\", "SAV");
    MapDirErase(PROTO_DIR_NAME "\\" CRITTERS_DIR_NAME "\\", PROTO_FILE_EXT);
    MapDirErase(PROTO_DIR_NAME "\\" ITEMS_DIR_NAME "\\", PROTO_FILE_EXT);
}

// 0x480040
int MapDirErase(const char* relativePath, const char* extension)
{
    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s*.%s", relativePath, extension);

    char** fileList;
    int fileListLength = fileNameListInit(path, &fileList, 0, 0);
    while (--fileListLength >= 0) {
        snprintf(path, sizeof(path), "%s\\%s%s", _patches, relativePath, fileList[fileListLength]);
        compat_remove(path);
    }
    fileNameListFree(&fileList, 0);

    return 0;
}

// 0x480104
static int _SaveBackup()
{
    debugPrint("\nLOADSAVE: Backing up save slot files..\n");

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcpy(_str0, _gmpath);

    strcat(_str0, "SAVE.DAT");

    _strmfe(_str1, _str0, "BAK");

    File* stream1 = fileOpen(_str0, "rb");
    if (stream1 != nullptr) {
        fileClose(stream1);
        if (compat_rename(_str0, _str1) != 0) {
            return -1;
        }
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    snprintf(_str0, sizeof(_str0), "%s*.%s", _gmpath, "SAV");

    char** fileList;
    int fileListLength = fileNameListInit(_str0, &fileList, 0, 0);
    if (fileListLength == -1) {
        return -1;
    }

    _map_backup_count = fileListLength;

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    for (int index = fileListLength - 1; index >= 0; index--) {
        strcpy(_str0, _gmpath);
        strcat(_str0, fileList[index]);

        _strmfe(_str1, _str0, "BAK");
        if (compat_rename(_str0, _str1) != 0) {
            fileNameListFree(&fileList, 0);
            return -1;
        }
    }

    fileNameListFree(&fileList, 0);

    debugPrint("\nLOADSAVE: %d map files backed up.\n", fileListLength);

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);

    char* v1 = _strmfe(_str2, "AUTOMAP.DB", "SAV");
    snprintf(_str0, sizeof(_str0), "%s\\%s", _gmpath, v1);

    char* v2 = _strmfe(_str2, "AUTOMAP.DB", "BAK");
    snprintf(_str1, sizeof(_str1), "%s\\%s", _gmpath, v2);

    _automap_db_flag = false;

    File* stream2 = fileOpen(_str0, "rb");
    if (stream2 != nullptr) {
        fileClose(stream2);

        if (_copy_file(_str0, _str1) == -1) {
            return -1;
        }

        _automap_db_flag = true;
    }

    return 0;
}

// 0x4803D8
static int _RestoreSave()
{
    debugPrint("\nLOADSAVE: Restoring save file backup...\n");

    savegameEraseSlot();

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcpy(_str0, _gmpath);
    strcat(_str0, "SAVE.DAT");
    _strmfe(_str1, _str0, "BAK");
    compat_remove(_str0);

    if (compat_rename(_str1, _str0) != 0) {
        savegameEraseSlot();
        return -1;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    snprintf(_str0, sizeof(_str0), "%s*.%s", _gmpath, "BAK");

    char** fileList;
    int fileListLength = fileNameListInit(_str0, &fileList, 0, 0);
    if (fileListLength == -1) {
        return -1;
    }

    if (fileListLength != _map_backup_count) {
        // FIXME: Probably leaks fileList.
        savegameEraseSlot();
        return -1;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);

    for (int index = fileListLength - 1; index >= 0; index--) {
        strcpy(_str0, _gmpath);
        strcat(_str0, fileList[index]);
        _strmfe(_str1, _str0, "SAV");
        compat_remove(_str1);
        if (compat_rename(_str0, _str1) != 0) {
            // FIXME: Probably leaks fileList.
            savegameEraseSlot();
            return -1;
        }
    }

    fileNameListFree(&fileList, 0);

    if (!_automap_db_flag) {
        return 0;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    char* v1 = _strmfe(_str2, "AUTOMAP.DB", "BAK");
    strcpy(_str0, _gmpath);
    strcat(_str0, v1);

    char* v2 = _strmfe(_str2, "AUTOMAP.DB", "SAV");
    strcpy(_str1, _gmpath);
    strcat(_str1, v2);

    if (compat_rename(_str0, _str1) != 0) {
        savegameEraseSlot();
        return -1;
    }

    return 0;
}

// 0x480710
static int _LoadObjDudeCid(File* stream)
{
    int value;

    if (fileReadInt32(stream, &value) == -1) {
        return -1;
    }

    gDude->cid = value;

    return 0;
}

// 0x480734
static int _SaveObjDudeCid(File* stream)
{
    return fileWriteInt32(stream, gDude->cid);
}

// 0x480754
int savegameEraseSlot()
{
    debugPrint("\nLOADSAVE: Erasing save(bad) slot...\n");

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    strcpy(_str0, _gmpath);
    strcat(_str0, "SAVE.DAT");
    compat_remove(_str0);

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s%.2d\\", "SAVEGAME", "SLOT", _slot_cursor + 1);
    snprintf(_str0, sizeof(_str0), "%s*.%s", _gmpath, "SAV");

    char** fileList;
    int fileListLength = fileNameListInit(_str0, &fileList, 0, 0);
    if (fileListLength == -1) {
        return -1;
    }

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);
    for (int index = fileListLength - 1; index >= 0; index--) {
        strcpy(_str0, _gmpath);
        strcat(_str0, fileList[index]);
        compat_remove(_str0);
    }

    fileNameListFree(&fileList, 0);

    snprintf(_gmpath, sizeof(_gmpath), "%s\\%s\\%s%.2d\\", _patches, "SAVEGAME", "SLOT", _slot_cursor + 1);

    char* v1 = _strmfe(_str1, "AUTOMAP.DB", "SAV");
    strcpy(_str0, _gmpath);
    strcat(_str0, v1);

    compat_remove(_str0);

    return 0;
}

} // namespace fallout
