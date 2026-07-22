#include "map.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "animation.h"
#include "art.h"
#include "automap.h"
#include "platform_compat.h"
#include "color.h"
#include "combat.h"
#include "critter.h"
#include "cycle.h"
#include "debug.h"
#include "draw.h"
#include "elevator.h"
#include "game.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "light.h"
#include "loadsave.h"
#include "map_render.h"
#include "memory.h"
#include "object.h"
#include "palette.h"
#include "party_member.h"
#include "pipboy.h"
#include "presenter.h"
#include "player_sheet.h"
#include "proto.h"
#include "proto_instance.h"
#include "queue.h"
#include "random.h"
#include "script_request_handler.h"
#include "scripts.h"
#include "server_players.h"
#include "settings.h"
#include "svga.h"
#include "text_object.h"
#include "tile.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "worldmap.h"

namespace fallout {

static char* mapBuildPath(char* name);
// mapLoad(File*) is the inner stream loader (scripts-before-objects, gvars/lvars,
// map-enter regen). Public (declared in map.h) for the STEP-4 join path, which
// loads a snapshot blob directly (CLIENT_JOIN_DESIGN.md §B4) rather than through
// mapLoadSaved (which ages/heals critters + touches the on-disk .SAV).
static int _map_age_dead_critters();
void _map_fix_critter_combat_data();
static int _map_save();
static int _map_save_file(File* stream);
static int mapGlobalVariablesInit(int count);
static void mapGlobalVariablesFree();
static int mapGlobalVariablesLoad(File* stream);
static int mapLocalVariablesInit(int count);
static void mapLocalVariablesFree();
static int mapLocalVariablesLoad(File* stream);
static void _map_place_dude_and_mouse();
static void _square_reset();
static int _square_load(File* stream, int a2);
static int mapHeaderWrite(MapHeader* ptr, File* stream);
static int mapHeaderRead(MapHeader* ptr, File* stream);

// 0x50B058
static char byte_50B058[] = "";

// 0x50B30C
static char _aErrorF2[] = "ERROR! F2";

// 0x519540
// De-static'd for the map_render.cc TU split: core _map_init assigns
// isoWindowRefreshRectMapper here and render mapScroll reads it. The definition
// stays in core; map_render.h re-exposes it via extern.
IsoWindowRefreshProc* _map_scroll_refresh = isoWindowRefreshRectGame;

// 0x519544
static const int _map_data_elev_flags[ELEVATION_COUNT] = {
    2,
    4,
    8,
};

// 0x519554
static bool gIsoEnabled = false;

// 0x519558
static int gEnteringElevation = 0;

// 0x51955C
static int gEnteringTile = -1;

// 0x519560
static int gEnteringRotation = ROTATION_NE;

// 0x519564
int gMapSid = -1;

// local_vars
// 0x519568
int* gMapLocalVars = nullptr;

// map_vars
// 0x51956C
int* gMapGlobalVars = nullptr;

// local_vars_num
// 0x519570
int gMapLocalVarsLength = 0;

// map_vars_num
// 0x519574
int gMapGlobalVarsLength = 0;

// Current elevation.
//
// 0x519578
int gElevation = 0;

// 0x51957C
static char* _errMapName = byte_50B058;

// 0x519584
static int _wmMapIdx = -1;

// 0x614868
static TileData _square_data[ELEVATION_COUNT];

// 0x631D28
static MapTransition gMapTransition;

// map.msg
//
// map_msg_file
// 0x631D48
MessageList gMapMessageList;

// 0x631D54
MapHeader gMapHeader;

// STEP-4 viewer join: when set, mapLoad leaves scripts disabled so the map-enter
// procs do not run (CLIENT_JOIN_DESIGN.md §E). Set only by the join-blob loader.
static bool gMapViewerLoad = false;

void mapSetViewerLoad(bool viewerLoad)
{
    gMapViewerLoad = viewerLoad;
}

// 0x631E40
TileData* _square[ELEVATION_COUNT];

// 0x631E50
static char _scratchStr[40];

// CE: Basically the same problem described in |gMapLocalPointers|, but this
// time Olympus folks use global map variables to store objects (looks like
// only `self_obj`).
static std::vector<void*> gMapGlobalPointers;

// CE: There is a bug in the user-space scripting where they want to store
// pointers to |Object| instances in local vars. This is obviously wrong as it's
// meaningless to save these pointers in file. As a workaround use second array
// to store these pointers.
static std::vector<void*> gMapLocalPointers;

// 0x481ED4
void isoReset()
{
    // NOTE: Uninline.
    mapGlobalVariablesFree();

    // NOTE: Uninline.
    mapLocalVariablesFree();

    artReset();
    tileReset();
    objectsReset();
    colorCycleReset();
    interfaceReset();

    // NOTE: Uninline.
    mapSetEnteringLocation(-1, -1, -1);
}

// Ledger H-17 (extracted from isoExit): map-variable teardown is sim state —
// the server must free MVARs/LVARs regardless of any render window. Called by
// gameExit right after isoExit, preserving the original teardown order.
void mapVariablesFree()
{
    // NOTE: Uninline.
    mapGlobalVariablesFree();

    // NOTE: Uninline.
    mapLocalVariablesFree();
}

// 0x481FB4
void _map_init()
{
    if (compat_stricmp(settings.system.executable.c_str(), "mapper") == 0) {
        _map_scroll_refresh = isoWindowRefreshRectMapper;
    }

    if (messageListInit(&gMapMessageList)) {
        char path[COMPAT_MAX_PATH];
        snprintf(path, sizeof(path), "%smap.msg", asc_5186C8);

        if (!messageListLoad(&gMapMessageList, path)) {
            debugPrint("\nError loading map_msg_file!");
        }
    } else {
        debugPrint("\nError initing map_msg_file!");
    }

    mapNewMap();
    tickersAdd(gameMouseRefresh);
    _gmouse_disable(0);
    windowShow(gIsoWindow);

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_MAP, &gMapMessageList);
}

// 0x482084
void _map_exit()
{
    windowHide(gIsoWindow);
    presenter()->cursorSet(MOUSE_CURSOR_ARROW);
    tickersRemove(gameMouseRefresh);

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_MAP, nullptr);
    if (!messageListFree(&gMapMessageList)) {
        debugPrint("\nError exiting map_msg_file!");
    }
}

// Ledger H-18: the world-freeze (_scr_enable_critters/_scr_disable_critters)
// and gIsoEnabled are sim authority and stay core; the ticker/mouse/text chrome
// here is future client-side and moves only once the client-init hook lands.
//
// 0x4820C0
void isoEnable()
{
    if (!gIsoEnabled) {
        // Chrome (float-text / mouse / frame-animation tickers) via the presenter
        // seam; the critter freeze + gIsoEnabled are sim authority and stay core.
        // Order matches the legacy body exactly. See Presenter::worldEnable.
        presenter()->worldEnable();
        _scr_enable_critters();
        gIsoEnabled = true;
    }
}

// 0x482104
bool isoDisable()
{
    if (!gIsoEnabled) {
        return false;
    }

    _scr_disable_critters();
    presenter()->worldDisable();

    gIsoEnabled = false;

    return true;
}

// 0x482148
bool isoIsDisabled()
{
    return gIsoEnabled == false;
}

// map_set_elevation
// 0x482158
int mapSetElevation(int elevation)
{
    if (!elevationIsValid(elevation)) {
        return -1;
    }

    bool gameMouseWasVisible = false;
    if (gameMouseGetCursor() != MOUSE_CURSOR_WAIT_PLANET) {
        gameMouseWasVisible = gameMouseObjectsIsVisible();
        presenter()->mouseObjectsHide();
        presenter()->cursorSet(MOUSE_CURSOR_NONE);
    }

    if (elevation != gElevation) {
        wmMapMarkMapEntranceState(gMapHeader.index, elevation, 1);
    }

    gElevation = elevation;

    reg_anim_clear(gDude);
    _dude_stand(gDude, gDude->rotation, gDude->fid);
    _partyMemberSyncPosition();

    if (gMapSid != -1) {
        scriptsExecMapUpdateProc();
    }

    if (gameMouseWasVisible) {
        presenter()->mouseObjectsShow();
    }

    return 0;
}

// 0x482220
int mapSetGlobalVar(int var, ProgramValue& value)
{
    if (var < 0 || var >= gMapGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference map var out of range: %d", var);
        return -1;
    }

    if (value.opcode == VALUE_TYPE_PTR) {
        gMapGlobalVars[var] = 0;
        gMapGlobalPointers[var] = value.pointerValue;
    } else {
        gMapGlobalVars[var] = value.integerValue;
        gMapGlobalPointers[var] = nullptr;
    }

    return 0;
}

// 0x482250
int mapGetGlobalVar(int var, ProgramValue& value)
{
    if (var < 0 || var >= gMapGlobalVarsLength) {
        debugPrint("ERROR: attempt to reference map var out of range: %d", var);
        return -1;
    }

    if (gMapGlobalPointers[var] != nullptr) {
        value.opcode = VALUE_TYPE_PTR;
        value.pointerValue = gMapGlobalPointers[var];
    } else {
        value.opcode = VALUE_TYPE_INT;
        value.integerValue = gMapGlobalVars[var];
    }

    return 0;
}

// 0x482280
int mapSetLocalVar(int var, ProgramValue& value)
{
    if (var < 0 || var >= gMapLocalVarsLength) {
        debugPrint("ERROR: attempt to reference local var out of range: %d", var);
        return -1;
    }

    if (value.opcode == VALUE_TYPE_PTR) {
        gMapLocalVars[var] = 0;
        gMapLocalPointers[var] = value.pointerValue;
    } else {
        gMapLocalVars[var] = value.integerValue;
        gMapLocalPointers[var] = nullptr;
    }

    return 0;
}

// 0x4822B0
int mapGetLocalVar(int var, ProgramValue& value)
{
    if (var < 0 || var >= gMapLocalVarsLength) {
        debugPrint("ERROR: attempt to reference local var out of range: %d", var);
        return -1;
    }

    if (gMapLocalPointers[var] != nullptr) {
        value.opcode = VALUE_TYPE_PTR;
        value.pointerValue = gMapLocalPointers[var];
    } else {
        value.opcode = VALUE_TYPE_INT;
        value.integerValue = gMapLocalVars[var];
    }

    return 0;
}

// Make a room to store more local variables.
//
// 0x4822E0
int _map_malloc_local_var(int a1)
{
    int oldMapLocalVarsLength = gMapLocalVarsLength;
    gMapLocalVarsLength += a1;

    int* vars = (int*)internal_realloc(gMapLocalVars, sizeof(*vars) * gMapLocalVarsLength);
    if (vars == nullptr) {
        debugPrint("\nError: Ran out of memory!");
    }

    gMapLocalVars = vars;
    memset((unsigned char*)vars + sizeof(*vars) * oldMapLocalVarsLength, 0, sizeof(*vars) * a1);

    gMapLocalPointers.resize(gMapLocalVarsLength);

    return oldMapLocalVarsLength;
}

// 0x48234C
void mapSetStart(int tile, int elevation, int rotation)
{
    gMapHeader.enteringTile = tile;
    gMapHeader.enteringElevation = elevation;
    gMapHeader.enteringRotation = rotation;
}

// 0x4824CC
char* mapGetName(int map, int elevation)
{
    if (map < 0 || map >= wmMapMaxCount()) {
        return nullptr;
    }

    if (!elevationIsValid(elevation)) {
        return nullptr;
    }

    MessageListItem messageListItem;
    return getmsg(&gMapMessageList, &messageListItem, map * 3 + elevation + 200);
}

// TODO: Check, probably returns true if map1 and map2 represents the same city.
//
// 0x482528
bool _is_map_idx_same(int map1, int map2)
{
    if (map1 < 0 || map1 >= wmMapMaxCount()) {
        return 0;
    }

    if (map2 < 0 || map2 >= wmMapMaxCount()) {
        return 0;
    }

    if (!wmMapIdxIsSaveable(map1)) {
        return 0;
    }

    if (!wmMapIdxIsSaveable(map2)) {
        return 0;
    }

    int city1;
    if (wmMatchAreaContainingMapIdx(map1, &city1) == -1) {
        return 0;
    }

    int city2;
    if (wmMatchAreaContainingMapIdx(map2, &city2) == -1) {
        return 0;
    }

    return city1 == city2;
}

// 0x4825CC
int _get_map_idx_same(int map1, int map2)
{
    int city1 = -1;
    if (wmMatchAreaContainingMapIdx(map1, &city1) == -1) {
        return -1;
    }

    int city2 = -2;
    if (wmMatchAreaContainingMapIdx(map2, &city2) == -1) {
        return -1;
    }

    if (city1 != city2) {
        return -1;
    }

    return city1;
}

// 0x48261C
char* mapGetCityName(int map)
{
    int city;
    if (wmMatchAreaContainingMapIdx(map, &city) == -1) {
        return _aErrorF2;
    }

    MessageListItem messageListItem;
    char* name = getmsg(&gMapMessageList, &messageListItem, 1500 + city);
    return name;
}

// 0x48268C
char* _map_get_description_idx_(int map)
{
    int city;
    if (wmMatchAreaContainingMapIdx(map, &city) == 0) {
        wmGetAreaIdxName(city, _scratchStr);
    } else {
        strcpy(_scratchStr, _errMapName);
    }

    return _scratchStr;
}

// 0x4826B8
int mapGetCurrentMap()
{
    return gMapHeader.index;
}

// Bumped once per mapLoad (below). See map.h for why the index cannot serve.
static unsigned int gMapLoadGeneration = 0;

unsigned int mapGetLoadGeneration()
{
    return gMapLoadGeneration;
}

// 0x482900
static char* mapBuildPath(char* name)
{
    // 0x631E78
    static char map_path[COMPAT_MAX_PATH];

    if (*name != '\\') {
        // NOTE: Uppercased from "maps".
        snprintf(map_path, sizeof(map_path), "MAPS\\%s", name);
        return map_path;
    }
    return name;
}

// 0x482924
int mapSetEnteringLocation(int elevation, int tile_num, int orientation)
{
    gEnteringElevation = elevation;
    gEnteringTile = tile_num;
    gEnteringRotation = orientation;
    return 0;
}

// 0x482938
void mapNewMap()
{
    mapSetElevation(0);
    tileSetCenter(20100, TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
    memset(&gMapTransition, 0, sizeof(gMapTransition));
    gMapHeader.enteringElevation = 0;
    gMapHeader.enteringRotation = 0;
    gMapHeader.localVariablesCount = 0;
    gMapHeader.version = 20;
    gMapHeader.name[0] = '\0';
    gMapHeader.enteringTile = 20100;
    _obj_remove_all();
    animationStop();

    // NOTE: Uninline.
    mapGlobalVariablesFree();

    // NOTE: Uninline.
    mapLocalVariablesFree();

    _square_reset();
    _map_place_dude_and_mouse();
    presenter()->worldInvalidate();
}

// 0x482A68
int mapLoadByName(char* fileName)
{
    int rc;

    compat_strupr(fileName);

    rc = -1;

    char* extension = strstr(fileName, ".MAP");
    if (extension != nullptr) {
        strcpy(extension, ".SAV");

        const char* filePath = mapBuildPath(fileName);

        File* stream = fileOpen(filePath, "rb");

        strcpy(extension, ".MAP");

        if (stream != nullptr) {
            fileClose(stream);
            rc = mapLoadSaved(fileName);
            wmMapMusicStart();
        }
    }

    if (rc == -1) {
        const char* filePath = mapBuildPath(fileName);
        File* stream = fileOpen(filePath, "rb");
        if (stream != nullptr) {
            rc = mapLoad(stream);
            fileClose(stream);
        }

        if (rc == 0) {
            strcpy(gMapHeader.name, fileName);
            gDude->data.critter.combat.whoHitMe = nullptr;
        }
    }

    return rc;
}

// 0x482B34
int mapLoadById(int map)
{
    scriptSetFixedParam(gMapSid, map);

    char name[16];
    if (wmMapIdxToName(map, name, sizeof(name)) == -1) {
        return -1;
    }

    _wmMapIdx = map;

    int rc = mapLoadByName(name);

    wmMapMusicStart();

    return rc;
}

// 0x482B74
int mapLoad(File* stream)
{
    presenterSetEmissionsSuppressed(true);
    // A new world is being built: every object is torn down and rebuilt (with
    // netId 0) regardless of whether the map INDEX changes. Consumers rebaseline
    // off this, not the index (map.h).
    gMapLoadGeneration++;
    _map_save_in_game(true);
    presenter()->ambientSoundLoad("wind2", 12, 13, 16);
    isoDisable();
    _partyMemberPrepLoad();
    presenter()->scrollDisable();

    int savedMouseCursorId = gameMouseGetCursor();
    presenter()->cursorSet(MOUSE_CURSOR_WAIT_PLANET);
    fileSetReadProgressHandler(gameMouseRefreshImmediately, 32768);
    tileDisable();

    int rc = 0;

    presenter()->worldClear();
    animationStop();
    scriptsDisable();

    gMapSid = -1;

    const char* error = nullptr;

    error = "Invalid file handle";
    if (stream == nullptr) {
        goto err;
    }

    error = "Error reading header";
    if (mapHeaderRead(&gMapHeader, stream) != 0) {
        goto err;
    }

    error = "Invalid map version";
    if (gMapHeader.version != 19 && gMapHeader.version != 20) {
        goto err;
    }

    if (gEnteringElevation == -1) {
        // NOTE: Uninline.
        mapSetEnteringLocation(gMapHeader.enteringElevation, gMapHeader.enteringTile, gMapHeader.enteringRotation);
    }

    _obj_remove_all();

    if (gMapHeader.globalVariablesCount < 0) {
        gMapHeader.globalVariablesCount = 0;
    }

    if (gMapHeader.localVariablesCount < 0) {
        gMapHeader.localVariablesCount = 0;
    }

    error = "Error allocating global vars";
    // NOTE: Uninline.
    if (mapGlobalVariablesInit(gMapHeader.globalVariablesCount) != 0) {
        goto err;
    }

    error = "Error loading global vars";
    // NOTE: Uninline.
    if (mapGlobalVariablesLoad(stream) != 0) {
        goto err;
    }

    error = "Error allocating local vars";
    // NOTE: Uninline.
    if (mapLocalVariablesInit(gMapHeader.localVariablesCount) != 0) {
        goto err;
    }

    error = "Error loading local vars";
    // NOTE: Uninline.
    if (mapLocalVariablesLoad(stream) != 0) {
        goto err;
    }

    if (_square_load(stream, gMapHeader.flags) != 0) {
        goto err;
    }

    error = "Error reading scripts";
    if (scriptLoadAll(stream) != 0) {
        goto err;
    }

    error = "Error reading objects";
    if (objectLoadAll(stream) != 0) {
        goto err;
    }

    if ((gMapHeader.flags & 1) == 0) {
        _map_fix_critter_combat_data();
    }

    error = "Error setting map elevation";
    if (mapSetElevation(gEnteringElevation) != 0) {
        goto err;
    }

    error = "Error setting tile center";
    if (tileSetCenter(gEnteringTile, TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS) != 0) {
        goto err;
    }

    lightSetAmbientIntensity(LIGHT_INTENSITY_MAX, false);
    objectSetLocation(gDude, gCenterTile, gElevation, nullptr);
    objectSetRotation(gDude, gEnteringRotation, nullptr);
    gMapHeader.index = wmMapMatchNameToIdx(gMapHeader.name);

    if ((gMapHeader.flags & 1) == 0) {
        char path[COMPAT_MAX_PATH];
        snprintf(path, sizeof(path), "maps\\%s", gMapHeader.name);

        char* extension = strstr(path, ".MAP");
        if (extension == nullptr) {
            extension = strstr(path, ".map");
        }

        if (extension != nullptr) {
            *extension = '\0';
        }

        strcat(path, ".GAM");
        globalVarsRead(path, "MAP_GLOBAL_VARS:", &gMapGlobalVarsLength, &gMapGlobalVars);
        gMapHeader.globalVariablesCount = gMapGlobalVarsLength;

        // CE: globalVarsRead() reallocates gMapGlobalVars and rewrites the
        // length from the .GAM file, but knows nothing about the parallel
        // gMapGlobalPointers vector — sized above from the .MAP header's count.
        // When the .GAM declares more vars than the header does (cowbomb.map:
        // header 0, .GAM 1) the vector stays short, and since mapGetGlobalVar/
        // mapSetGlobalVar bounds-check only against gMapGlobalVarsLength, the
        // first map-var access indexes it out of bounds and segfaults. Keep the
        // two in lockstep.
        gMapGlobalPointers.resize(gMapGlobalVarsLength);
    }

    // STEP-4 viewer join (CLIENT_JOIN_DESIGN.md §E): a joining viewer is a puppet
    // and must run NO scripts. Leaving scripts disabled here makes every map-enter/
    // update/start proc below a no-op (scriptExecProc gates on gScriptsEnabled,
    // scripts.cc:1264), so the client adopts the blob's post-server-enter state
    // verbatim instead of re-running a second, divergent map-enter (which mutates
    // lvars / object flags / script bindings on non-idempotent maps). The map
    // script is still REGISTERED (scriptAdd below is not gated), only its proc
    // execution is suppressed. Default path (client + server) unchanged.
    if (!gMapViewerLoad) {
        scriptsEnable();
    }

    if (gMapHeader.scriptIndex > 0) {
        error = "Error creating new map script";
        if (scriptAdd(&gMapSid, SCRIPT_TYPE_SYSTEM) == -1) {
            goto err;
        }

        Object* object;
        int fid = buildFid(OBJ_TYPE_MISC, 12, 0, 0, 0);
        objectCreateWithFidPid(&object, fid, -1);
        object->flags |= (OBJECT_LIGHT_THRU | OBJECT_NO_SAVE | OBJECT_HIDDEN);
        objectSetLocation(object, 1, 0, nullptr);
        object->sid = gMapSid;
        scriptSetFixedParam(gMapSid, (gMapHeader.flags & 1) == 0);

        Script* script;
        scriptGetScript(gMapSid, &script);
        script->index = gMapHeader.scriptIndex - 1;
        script->flags |= SCRIPT_FLAG_0x08;
        object->id = scriptsNewObjectId();
        script->ownerId = object->id;
        script->owner = object;
        _scr_spatials_disable();
        scriptExecProc(gMapSid, SCRIPT_PROC_MAP_ENTER);
        _scr_spatials_enable();

        error = "Error Setting up random encounter";
        if (wmSetupRandomEncounter() == -1) {
            goto err;
        }
    }

    error = nullptr;

err:

    if (error != nullptr) {
        char message[100]; // TODO: Size is probably wrong.
        snprintf(message, sizeof(message), "%s while loading map.", error);
        debugPrint(message);
        mapNewMap();
        rc = -1;
    } else {
        _obj_preload_art_cache(gMapHeader.flags);

        // MP_PROTOCOL.md §2/§5: the active tactical map is now fully loaded — the
        // universal choke every in-game map change funnels through (exit grids and
        // elevators via mapHandleTransition, worldmap arrivals/encounters via
        // worldmap_ui.cc mapLoadById). No-op under the null/client presenter, so
        // byte-identical. gMapHeader.index / gElevation are the freshly-loaded
        // values. NOTE: new-game (main.cc) and save-restore (mapLoadSaved) also
        // reach here — that is snapshot territory; the NetworkPresenter adds the
        // §5 load-window guard in P5-C. See Presenter::mapTransition (presenter.h).
        presenter()->mapTransition(gMapHeader.index, gElevation);
    }

    _partyMemberRecoverLoad();
    presenter()->hudBarShow();
    _proto_dude_update_gender();
    _map_place_dude_and_mouse();
    fileSetReadProgressHandler(nullptr, 0);
    isoEnable();
    presenter()->scrollDisable();
    presenter()->cursorSet(MOUSE_CURSOR_WAIT_PLANET);

    if (scriptsExecStartProc() == -1) {
        debugPrint("\n   Error: scr_load_all_scripts failed!");
    }

    scriptsExecMapEnterProc();
    scriptsExecMapUpdateProc();
    tileEnable();

    if (gMapTransition.map > 0) {
        if (gMapTransition.rotation >= 0) {
            objectSetRotation(gDude, gMapTransition.rotation, nullptr);
        }
    } else {
        presenter()->worldInvalidate();
    }

    gameTimeScheduleUpdateEvent();

    if (_gsound_sfx_q_start() == -1) {
        rc = -1;
    }

    wmMapMarkVisited(gMapHeader.index);
    wmMapMarkMapEntranceState(gMapHeader.index, gElevation, 1);

    if (wmCheckGameAreaEvents() != 0) {
        rc = -1;
    }

    fileSetReadProgressHandler(nullptr, 0);

    if (gameUiIsDisabled() == 0) {
        presenter()->scrollEnable();
    }

    presenter()->cursorSet(savedMouseCursorId);

    // NOTE: Uninline.
    mapSetEnteringLocation(-1, -1, -1);

    presenter()->movieFadeOut();

    gMapHeader.version = 20;

    presenterSetEmissionsSuppressed(false);
    return rc;
}

// 0x483188
int mapLoadSaved(char* fileName)
{
    debugPrint("\nMAP: Loading SAVED map.");

    char mapName[16]; // TODO: Size is probably wrong.
    _strmfe(mapName, fileName, "SAV");

    int rc = mapLoadByName(mapName);

    if (gameTimeGetTime() >= gMapHeader.lastVisitTime) {
        if (((gameTimeGetTime() - gMapHeader.lastVisitTime) / GAME_TIME_TICKS_PER_HOUR) >= 24) {
            objectUnjamAll();
        }

        if (_map_age_dead_critters() == -1) {
            debugPrint("\nError: Critter aging failed on map load!");
            return -1;
        }
    }

    if (!wmMapIsSaveable()) {
        debugPrint("\nDestroying RANDOM encounter map.");

        char v15[16];
        strcpy(v15, gMapHeader.name);

        _strmfe(gMapHeader.name, v15, "SAV");

        _MapDirEraseFile_("MAPS\\", gMapHeader.name);

        strcpy(gMapHeader.name, v15);
    }

    return rc;
}

// 0x48328C
static int _map_age_dead_critters()
{
    if (!wmMapDeadBodiesAge()) {
        return 0;
    }

    int hoursSinceLastVisit = (gameTimeGetTime() - gMapHeader.lastVisitTime) / GAME_TIME_TICKS_PER_HOUR;
    if (hoursSinceLastVisit == 0) {
        return 0;
    }

    Object* obj = objectFindFirst();
    while (obj != nullptr) {
        if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER
            && obj != gDude
            && !objectIsPartyMember(obj)
            && !critterIsDead(obj)) {
            obj->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;
            if (critterGetKillType(obj) != KILL_TYPE_ROBOT && !_critter_flag_check(obj->pid, CRITTER_NO_HEAL)) {
                _critter_heal_hours(obj, hoursSinceLastVisit);
            }
        }
        obj = objectFindNext();
    }

    int agingType;
    if (hoursSinceLastVisit > 6 * 24) {
        agingType = 1;
    } else if (hoursSinceLastVisit > 14 * 24) {
        agingType = 2;
    } else {
        return 0;
    }

    int capacity = 100;
    int count = 0;
    Object** objects = (Object**)internal_malloc(sizeof(*objects) * capacity);

    obj = objectFindFirst();
    while (obj != nullptr) {
        int type = PID_TYPE(obj->pid);
        if (type == OBJ_TYPE_CRITTER) {
            if (obj != gDude && critterIsDead(obj)) {
                if (critterGetKillType(obj) != KILL_TYPE_ROBOT && !_critter_flag_check(obj->pid, CRITTER_NO_HEAL)) {
                    objects[count++] = obj;

                    if (count >= capacity) {
                        capacity *= 2;
                        objects = (Object**)internal_realloc(objects, sizeof(*objects) * capacity);
                        if (objects == nullptr) {
                            debugPrint("\nError: Out of Memory!");
                            return -1;
                        }
                    }
                }
            }
        } else if (agingType == 2 && type == OBJ_TYPE_MISC && obj->pid == 0x500000B) {
            objects[count++] = obj;
            if (count >= capacity) {
                capacity *= 2;
                objects = (Object**)internal_realloc(objects, sizeof(*objects) * capacity);
                if (objects == nullptr) {
                    debugPrint("\nError: Out of Memory!");
                    return -1;
                }
            }
        }
        obj = objectFindNext();
    }

    int rc = 0;
    for (int index = 0; index < count; index++) {
        Object* obj = objects[index];
        if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
            if (!_critter_flag_check(obj->pid, CRITTER_NO_DROP)) {
                itemDropAll(obj, obj->tile);
            }

            Object* blood;
            if (objectCreateWithPid(&blood, 0x5000004) == -1) {
                rc = -1;
                break;
            }

            objectSetLocation(blood, obj->tile, obj->elevation, nullptr);

            Proto* proto;
            protoGetProto(obj->pid, &proto);

            int frame = randomBetween(0, 3);
            if ((proto->critter.flags & CRITTER_FLAT)) {
                frame += 6;
            } else {
                if (critterGetKillType(obj) != KILL_TYPE_RAT
                    && critterGetKillType(obj) != KILL_TYPE_MANTIS) {
                    frame += 3;
                }
            }

            objectSetFrame(blood, frame, nullptr);
        }

        reg_anim_clear(obj);
        objectDestroy(obj, nullptr);
    }

    internal_free(objects);

    return rc;
}

// 0x48358C
int _map_target_load_area()
{
    int city = -1;
    if (wmMatchAreaContainingMapIdx(gMapHeader.index, &city) == -1) {
        city = -1;
    }
    return city;
}

// 0x4835B4
// Set while a NON-TRANSIT-AUTHORITY player actor's spatial procs run
// (scripts.cc). Everything an exit grid does funnels through mapSetTransition,
// so suppressing there is one choke point instead of auditing every script that
// can call op_load_map — and it drops only the transition, leaving the rest of
// the script's effects intact (an extra still trips the trap, just does not
// travel). MP_PROPOSAL Ch 14.2.
static bool gMapTransitionSuppressed = false;

void mapSetTransitionSuppressed(bool suppressed)
{
    gMapTransitionSuppressed = suppressed;
}

int mapSetTransition(MapTransition* transition)
{
    if (transition == nullptr) {
        return -1;
    }

    if (gMapTransitionSuppressed) {
        debugPrint("map: transition request dropped (actor may not transit)\n");
        return 0;
    }

    memcpy(&gMapTransition, transition, sizeof(gMapTransition));

    if (gMapTransition.map == 0) {
        gMapTransition.map = -2;
    }

    if (isInCombat()) {
        _game_user_wants_to_quit = 1;
    }

    return 0;
}

// Is a map transition latched and waiting for the beat tail? mapSetTransition
// only records the request; mapHandleTransition performs it. Anything that keeps
// mutating the world between those two points is working on a map that is about
// to be torn down — the stepped walker uses this to stop walking the moment a
// tile it crossed triggered an exit.
bool mapTransitionPending()
{
    return gMapTransition.map != 0;
}

// 0x4835F8
int mapHandleTransition()
{
    if (gMapTransition.map == 0) {
        return 0;
    }

    presenter()->mouseObjectsHide();

    presenter()->cursorSet(MOUSE_CURSOR_NONE);

    if (gMapTransition.map == -1) {
        if (!isInCombat()) {
            animationStop();
            scriptRequestHandler()->townMap();
            memset(&gMapTransition, 0, sizeof(gMapTransition));
        }
    } else if (gMapTransition.map == -2) {
        if (!isInCombat()) {
            animationStop();
            scriptRequestHandler()->worldMap();
            memset(&gMapTransition, 0, sizeof(gMapTransition));
        }
    } else {
        if (!isInCombat()) {
            if (gMapTransition.map != gMapHeader.index || gElevation == gMapTransition.elevation) {
                // SFALL: Remove text floaters after moving to another map.
                textObjectsReset();

                mapLoadById(gMapTransition.map);
            }

            if (gMapTransition.tile != -1 && gMapTransition.tile != 0
                && gMapHeader.index != MAP_MODOC_BEDNBREAKFAST && gMapHeader.index != MAP_THE_SQUAT_A
                && elevationIsValid(gMapTransition.elevation)) {
                objectSetLocation(gDude, gMapTransition.tile, gMapTransition.elevation, nullptr);
                mapSetElevation(gMapTransition.elevation);
                objectSetRotation(gDude, gMapTransition.rotation, nullptr);

                // Everyone travels with the host (MP_PROPOSAL Ch 14.2). The host
                // is placed on the entry tile above; extras ring it. With an empty
                // registry the loop body never runs, so this is inert everywhere
                // but a co-op server.
                for (int slot = 1; slot < playerActorCount(); slot++) {
                    Object* actor = playerActorAt(slot);
                    // An offline body travels with the group ONLY as data (it is
                    // off the object list; objectSetLocation would fail closed
                    // anyway) — placing it would resurrect a player who left.
                    if (actor == nullptr || !playerActorOnline(slot)) {
                        continue;
                    }
                    int tile = playerActorFindFreeTileNear(gDude->tile, gMapTransition.elevation);
                    if (tile == -1) {
                        // Nowhere free within the ring: co-locate rather than
                        // strand the actor off-map. Two bodies on one tile is
                        // ugly; an actor at a stale tile on a new map is a bug.
                        tile = gDude->tile;
                    }
                    objectSetLocation(actor, tile, gMapTransition.elevation, nullptr);
                    objectSetRotation(actor, gMapTransition.rotation, nullptr);
                }
            }

            if (tileSetCenter(gDude->tile, TILE_SET_CENTER_REFRESH_WINDOW) == -1) {
                debugPrint("\nError: map: attempt to center out-of-bounds!");
            }

            memset(&gMapTransition, 0, sizeof(gMapTransition));

            int city;
            wmMatchAreaContainingMapIdx(gMapHeader.index, &city);
            if (wmTeleportToArea(city) == -1) {
                debugPrint("\nError: couldn't make jump on worldmap for map jump!");
            }
        }
    }

    return 0;
}

// 0x483784
void _map_fix_critter_combat_data()
{
    for (Object* object = objectFindFirst(); object != nullptr; object = objectFindNext()) {
        if (object->pid == -1) {
            continue;
        }

        if (PID_TYPE(object->pid) != OBJ_TYPE_CRITTER) {
            continue;
        }

        // Null the runtime-only whoHitMe POINTER on load, regardless of the persistent
        // whoHitMeCid. Out of combat whoHitMe is never re-resolved, so a critter loaded
        // with cid != -1 otherwise keeps a bad pointer — on 64-bit a 4-byte -1 sitting in
        // the 8-byte field — which _damage_object derefs (combat.cc:5425, guarded only
        // against nullptr) when an OUT-OF-COMBAT explosion (C4 / la bombia) kills it → a
        // ~50% server SIGSEGV. Combat re-resolves whoHitMe from whoHitMeCid via _find_cid
        // at _combat_begin (combat.cc:2063-2091), so nulling here is safe; the persistent
        // whoHitMeCid is untouched. (Vanilla nulled only when cid == -1 — a latent bug the
        // 64-bit pointer width makes reliably fatal.)
        object->data.critter.combat.whoHitMe = nullptr;
    }
}

// map_save
// 0x483850
static int _map_save()
{
    char temp[80];
    temp[0] = '\0';

    strcat(temp, settings.system.master_patches_path.c_str());
    compat_mkdir(temp);

    strcat(temp, "\\MAPS");
    compat_mkdir(temp);

    int rc = -1;
    if (gMapHeader.name[0] != '\0') {
        char* mapFileName = mapBuildPath(gMapHeader.name);
        File* stream = fileOpen(mapFileName, "wb");
        if (stream != nullptr) {
            rc = _map_save_file(stream);
            fileClose(stream);
        } else {
            snprintf(temp, sizeof(temp), "Unable to open %s to write!", gMapHeader.name);
            debugPrint(temp);
        }

        if (rc == 0) {
            snprintf(temp, sizeof(temp), "%s saved.", gMapHeader.name);
            debugPrint(temp);
        }
    } else {
        debugPrint("\nError: map_save: map header corrupt!");
    }

    return rc;
}

// 0x483980
static int _map_save_file(File* stream)
{
    if (stream == nullptr) {
        return -1;
    }

    scriptsDisable();

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        int tile;
        for (tile = 0; tile < SQUARE_GRID_SIZE; tile++) {
            int fid;

            fid = buildFid(OBJ_TYPE_TILE, _square[elevation]->field_0[tile] & 0xFFF, 0, 0, 0);
            if (fid != buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0)) {
                break;
            }

            fid = buildFid(OBJ_TYPE_TILE, (_square[elevation]->field_0[tile] >> 16) & 0xFFF, 0, 0, 0);
            if (fid != buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0)) {
                break;
            }
        }

        if (tile == SQUARE_GRID_SIZE) {
            Object* object = objectFindFirstAtElevation(elevation);
            if (object != nullptr) {
                // TODO: Implementation is slightly different, check in debugger.
                while (object != nullptr && (object->flags & OBJECT_NO_SAVE)) {
                    object = objectFindNextAtElevation();
                }

                if (object != nullptr) {
                    gMapHeader.flags &= ~_map_data_elev_flags[elevation];
                } else {
                    gMapHeader.flags |= _map_data_elev_flags[elevation];
                }
            } else {
                gMapHeader.flags |= _map_data_elev_flags[elevation];
            }
        } else {
            gMapHeader.flags &= ~_map_data_elev_flags[elevation];
        }
    }

    gMapHeader.localVariablesCount = gMapLocalVarsLength;
    gMapHeader.globalVariablesCount = gMapGlobalVarsLength;
    gMapHeader.darkness = 1;

    mapHeaderWrite(&gMapHeader, stream);

    if (gMapHeader.globalVariablesCount != 0) {
        fileWriteInt32List(stream, gMapGlobalVars, gMapHeader.globalVariablesCount);
    }

    if (gMapHeader.localVariablesCount != 0) {
        fileWriteInt32List(stream, gMapLocalVars, gMapHeader.localVariablesCount);
    }

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if ((gMapHeader.flags & _map_data_elev_flags[elevation]) == 0) {
            _db_fwriteLongCount(stream, _square[elevation]->field_0, SQUARE_GRID_SIZE);
        }
    }

    char err[80];

    if (scriptSaveAll(stream) == -1) {
        snprintf(err, sizeof(err), "Error saving scripts in %s", gMapHeader.name);
        presenter()->errorBox(err);
    }

    if (objectSaveAll(stream) == -1) {
        snprintf(err, sizeof(err), "Error saving objects in %s", gMapHeader.name);
        presenter()->errorBox(err);
    }

    scriptsEnable();

    return 0;
}

// Delete one file under the patches dir. Vanilla homed this in loadsave.cc, but
// it is pure filesystem housekeeping with no save-UI dependency whatsoever, and
// map.cc is its ONLY caller (both sites below discard a random encounter map's
// .SAV). Left in loadsave.cc it made a core map path reach a client-only symbol:
// the dedicated server aborted with "client symbol '_MapDirEraseFile_' called on
// the core-only server" the moment a player LEFT a random encounter map — a path
// unreachable until worldmap encounters started working. Moved here rather than
// stubbed or #ifdef'd, per the cut-list rule: restructure the reference out.
//
// Reads the patches path straight from settings; loadsave.cc's `_patches` static
// was only ever a cached copy of the same value.
//
// 0x4800C8
int _MapDirEraseFile_(const char* a1, const char* a2)
{
    char path[COMPAT_MAX_PATH];

    snprintf(path, sizeof(path), "%s\\%s%s",
        settings.system.master_patches_path.c_str(), a1, a2);
    if (compat_remove(path) != 0) {
        return -1;
    }

    return 0;
}

// Serialize the current map + the dude to `stream` as a STEP-4 join snapshot blob
// (CLIENT_JOIN_DESIGN.md §B). The caller runs objectAssignAllNetIds() first (§C)
// so the object ORDER the walk numbered is the order objectSaveAll writes; a
// joining client re-runs the same walk after loading and gets identical netIds.
//
// B2: stamp the "saved map" header bits so the loader takes the saved-map branch
// (no first-run .GAM re-read, map_first_run=0 → no one-time spawn logic). These
// are the two benign lines of _map_save_in_game; we do NOT call it (it runs
// scriptsExecMapExitProc, which mutates, and writes an on-disk .SAV).
// B1: append the dude — objectSaveAll skips OBJECT_NO_SAVE (object.cc:674), and
// the dude is NO_SAVE, so _obj_save_dude carries him after the map body.
int mapSaveToStream(File* stream, int* mapBodyLenOut)
{
    gMapHeader.flags |= 0x01;
    gMapHeader.lastVisitTime = gameTimeGetTime();

    if (_map_save_file(stream) == -1) {
        return -1;
    }

    // The map body ends here; the dude blob follows. The two sub-streams
    // self-delimit on load, but the split length is a fail-loud wire guard (§2).
    if (mapBodyLenOut != nullptr) {
        *mapBodyLenOut = (int)fileTell(stream);
    }

    if (_obj_save_dude(stream) == -1) {
        return -1;
    }

    // B1-N: the extra player actors follow the host, in registry slot order
    // (MP_PROPOSAL.md Ch 5.3). They are NO_SAVE for the same reason the dude is,
    // so objectSaveAll skipped them too. Slot order IS wire order: the viewer
    // registers them in this order and then reproduces objectAssignAllNetIds,
    // which numbers the registry before the tile walk — get the order wrong and
    // every netId after the actors shifts.
    //
    // The sections self-delimit on load (each is one _obj_save_obj tree), so the
    // header carries the actor COUNT rather than per-section lengths; the
    // existing dudeBlobLen guard still bounds the whole appendix and the crc32
    // still covers every byte.
    for (int slot = 1; slot < playerActorCount(); slot++) {
        if (_obj_save_player_actor(stream, playerActorAt(slot)) == -1) {
            return -1;
        }
    }

    // B1-S: the CHARACTER SHEETS follow the actor objects, from slot 0 — the
    // sheet lives off the Object (in the proto row and the PC-globals), so
    // nothing above carries it. Before this the viewer seeded every sheet from
    // its own gDudeProto, which is right only until someone levels: a joiner
    // then sees the host's character as it was at boot, and every extra as a
    // copy of that (PLAYER_SHEET_DESIGN.md §5).
    //
    // Last in the appendix on purpose: the sections above self-delimit as
    // _obj_save_obj trees, so a reader that predates this block simply stops
    // early and ignores the tail (the headless F2_CLIENT_BLOB_IN probe does
    // exactly that, and is unaffected).
    if (playerSheetBlockWrite(stream, 0) == -1) {
        return -1;
    }

    return 0;
}

// 0x483C98
int _map_save_in_game(bool a1)
{
    if (gMapHeader.name[0] == '\0') {
        return 0;
    }

    animationStop();
    _partyMemberSaveProtos();

    if (a1) {
        _queue_leaving_map();
        _partyMemberPrepLoad();
        _partyMemberPrepItemSaveAll();
        scriptsExecMapExitProc();

        if (gMapSid != -1) {
            Script* script;
            scriptGetScript(gMapSid, &script);
        }

        gameTimeScheduleUpdateEvent();
        _obj_reset_roof();
    }

    gMapHeader.flags |= 0x01;
    gMapHeader.lastVisitTime = gameTimeGetTime();

    char name[16];

    if (a1 && !wmMapIsSaveable()) {
        debugPrint("\nNot saving RANDOM encounter map.");

        strcpy(name, gMapHeader.name);
        _strmfe(gMapHeader.name, name, "SAV");
        _MapDirEraseFile_("MAPS\\", gMapHeader.name);
        strcpy(gMapHeader.name, name);
    } else {
        debugPrint("\n Saving \".SAV\" map.");

        strcpy(name, gMapHeader.name);
        _strmfe(gMapHeader.name, name, "SAV");
        if (_map_save() == -1) {
            return -1;
        }

        strcpy(gMapHeader.name, name);

        automapSaveCurrent();

        if (a1) {
            gMapHeader.name[0] = '\0';
            _obj_remove_all();
            _proto_remove_all();
            _square_reset();
            gameTimeScheduleUpdateEvent();
        }
    }

    return 0;
}

// De-static'd for the map_render.cc TU split: called by the moved isoInit.
// The definition stays in core; map_render.h exposes the prototype.
//
// 0x483E28
void mapMakeMapsDirectory()
{
    char path[COMPAT_MAX_PATH];

    strcpy(path, settings.system.master_patches_path.c_str());
    compat_mkdir(path);

    strcat(path, "\\MAPS");
    compat_mkdir(path);
}

// NOTE: Inlined.
//
// 0x483FE4
static int mapGlobalVariablesInit(int count)
{
    mapGlobalVariablesFree();

    if (count != 0) {
        gMapGlobalVars = (int*)internal_malloc(sizeof(*gMapGlobalVars) * count);
        if (gMapGlobalVars == nullptr) {
            return -1;
        }

        gMapGlobalPointers.resize(count);
    }

    gMapGlobalVarsLength = count;

    return 0;
}

// 0x484038
static void mapGlobalVariablesFree()
{
    if (gMapGlobalVars != nullptr) {
        internal_free(gMapGlobalVars);
        gMapGlobalVars = nullptr;
        gMapGlobalVarsLength = 0;
    }

    gMapGlobalPointers.clear();
}

// NOTE: Inlined.
//
// 0x48405C
static int mapGlobalVariablesLoad(File* stream)
{
    if (fileReadInt32List(stream, gMapGlobalVars, gMapGlobalVarsLength) != 0) {
        return -1;
    }

    return 0;
}

// NOTE: Inlined.
//
// 0x484080
static int mapLocalVariablesInit(int count)
{
    mapLocalVariablesFree();

    if (count != 0) {
        gMapLocalVars = (int*)internal_malloc(sizeof(*gMapLocalVars) * count);
        if (gMapLocalVars == nullptr) {
            return -1;
        }

        gMapLocalPointers.resize(count);
    }

    gMapLocalVarsLength = count;

    return 0;
}

// 0x4840D4
static void mapLocalVariablesFree()
{
    if (gMapLocalVars != nullptr) {
        internal_free(gMapLocalVars);
        gMapLocalVars = nullptr;
        gMapLocalVarsLength = 0;
    }

    gMapLocalPointers.clear();
}

// NOTE: Inlined.
//
// 0x4840F8
static int mapLocalVariablesLoad(File* stream)
{
    if (fileReadInt32List(stream, gMapLocalVars, gMapLocalVarsLength) != 0) {
        return -1;
    }

    return 0;
}

// 0x48411C
static void _map_place_dude_and_mouse()
{
    _obj_clear_seen();

    if (gDude != nullptr) {
        if (FID_ANIM_TYPE(gDude->fid) != ANIM_STAND) {
            objectSetFrame(gDude, 0, nullptr);
            gDude->fid = buildFid(OBJ_TYPE_CRITTER, gDude->fid & 0xFFF, ANIM_STAND, (gDude->fid & 0xF000) >> 12, gDude->rotation + 1);
        }

        if (gDude->tile == -1) {
            objectSetLocation(gDude, gCenterTile, gElevation, nullptr);
            objectSetRotation(gDude, gMapHeader.enteringRotation, nullptr);
        }

        objectSetLight(gDude, 4, 0x10000, nullptr);
        gDude->flags |= OBJECT_NO_SAVE;

        _dude_stand(gDude, gDude->rotation, gDude->fid);
        _partyMemberSyncPosition();
    }

    // Same treatment for every other player actor (MP_PROPOSAL Ch 14.2): the
    // STAND fid fix and the NO_SAVE re-assert are per-ACTOR properties, not
    // per-gDude ones. The flags survive the map load (extras are NO_REMOVE
    // server-side), so re-asserting is belt-and-braces — but stating the
    // invariant here is what keeps it from silently applying to slot 0 only.
    //
    // ⚠ Placement here is UNCONDITIONAL, unlike the dude's `tile == -1` case.
    // Player actors are NO_REMOVE, so they SURVIVE the teardown still holding a
    // tile index from the map we just left — a number that means something else
    // entirely (or nothing) on the new one. There is no "did it get placed"
    // signal to test, because the stale tile looks perfectly valid. So every load
    // re-plants them beside the host. The transition path then moves the whole
    // group again if it has a specific entry tile; landing twice is harmless,
    // landing never is a body standing inside a wall on the wrong map.
    for (int slot = 1; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr || gDude == nullptr) {
            continue;
        }
        // Same offline rule as the transition placement above: a despawned body
        // stays off-map through a load until its player logs back in.
        if (!playerActorOnline(slot)) {
            continue;
        }

        if (FID_ANIM_TYPE(actor->fid) != ANIM_STAND) {
            objectSetFrame(actor, 0, nullptr);
            actor->fid = buildFid(OBJ_TYPE_CRITTER, actor->fid & 0xFFF, ANIM_STAND, (actor->fid & 0xF000) >> 12, actor->rotation + 1);
        }

        int tile = playerActorFindFreeTileNear(gDude->tile, gDude->elevation);
        objectSetLocation(actor, tile != -1 ? tile : gDude->tile, gDude->elevation, nullptr);
        objectSetRotation(actor, gMapHeader.enteringRotation, nullptr);

        objectSetLight(actor, 4, 0x10000, nullptr);
        actor->flags |= OBJECT_NO_SAVE;
        _dude_stand(actor, actor->rotation, actor->fid);
    }

    presenter()->mouseResetBouncingCursor();
    presenter()->mouseObjectsShow();
}

// De-static'd for the map_render.cc TU split: called by the moved isoInit.
// The definition stays in core; map_render.h exposes the prototype.
//
// NOTE: Inlined.
//
// 0x4841F0
void square_init()
{
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        _square[elevation] = &(_square_data[elevation]);
    }
}

// 0x484210
static void _square_reset()
{
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        int* p = _square[elevation]->field_0;
        for (int y = 0; y < SQUARE_GRID_HEIGHT; y++) {
            for (int x = 0; x < SQUARE_GRID_WIDTH; x++) {
                // TODO: Strange math, initially right, but need to figure it out and
                // check subsequent calls.
                int fid = *p;
                fid &= ~0xFFFF;
                *p = (((buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0) & 0xFFF) | (((fid >> 16) & 0xF000) >> 12)) << 16) | (fid & 0xFFFF);

                fid = *p;
                int v3 = (fid & 0xF000) >> 12;
                int v4 = (buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0) & 0xFFF) | v3;

                fid &= ~0xFFFF;

                *p = v4 | ((fid >> 16) << 16);

                p++;
            }
        }
    }
}

// 0x48431C
static int _square_load(File* stream, int flags)
{
    int v6;
    int v7;
    int v8;
    int v9;

    _square_reset();

    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if ((flags & _map_data_elev_flags[elevation]) == 0) {
            int* arr = _square[elevation]->field_0;
            if (_db_freadIntCount(stream, arr, SQUARE_GRID_SIZE) != 0) {
                return -1;
            }

            for (int tile = 0; tile < SQUARE_GRID_SIZE; tile++) {
                v6 = arr[tile];
                v6 &= ~(0xFFFF);
                v6 >>= 16;

                v7 = (v6 & 0xF000) >> 12;
                v7 &= ~(0x01);

                v8 = v6 & 0xFFF;
                v9 = arr[tile] & 0xFFFF;
                arr[tile] = ((v8 | (v7 << 12)) << 16) | v9;
            }
        }
    }

    return 0;
}

// 0x4843B8
static int mapHeaderWrite(MapHeader* ptr, File* stream)
{
    if (fileWriteInt32(stream, ptr->version) == -1) return -1;
    if (fileWriteFixedLengthString(stream, ptr->name, 16) == -1) return -1;
    if (fileWriteInt32(stream, ptr->enteringTile) == -1) return -1;
    if (fileWriteInt32(stream, ptr->enteringElevation) == -1) return -1;
    if (fileWriteInt32(stream, ptr->enteringRotation) == -1) return -1;
    if (fileWriteInt32(stream, ptr->localVariablesCount) == -1) return -1;
    if (fileWriteInt32(stream, ptr->scriptIndex) == -1) return -1;
    if (fileWriteInt32(stream, ptr->flags) == -1) return -1;
    if (fileWriteInt32(stream, ptr->darkness) == -1) return -1;
    if (fileWriteInt32(stream, ptr->globalVariablesCount) == -1) return -1;
    if (fileWriteInt32(stream, ptr->index) == -1) return -1;
    if (fileWriteUInt32(stream, ptr->lastVisitTime) == -1) return -1;
    if (fileWriteInt32List(stream, ptr->field_3C, 44) == -1) return -1;

    return 0;
}

// 0x4844B4
static int mapHeaderRead(MapHeader* ptr, File* stream)
{
    if (fileReadInt32(stream, &(ptr->version)) == -1) return -1;
    if (fileReadFixedLengthString(stream, ptr->name, 16) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->enteringTile)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->enteringElevation)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->enteringRotation)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->localVariablesCount)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->scriptIndex)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->flags)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->darkness)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->globalVariablesCount)) == -1) return -1;
    if (fileReadInt32(stream, &(ptr->index)) == -1) return -1;
    if (fileReadUInt32(stream, &(ptr->lastVisitTime)) == -1) return -1;
    if (fileReadInt32List(stream, ptr->field_3C, 44) == -1) return -1;

    return 0;
}

} // namespace fallout
