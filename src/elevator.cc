#include "elevator.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

#include "art.h"
#include "cycle.h"
#include "debug.h"
#include "delay.h"
#include "draw.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "map.h"
#include "pipboy.h"
#include "scripts.h"
#include "sfall_config.h"
#include "svga.h"
#include "window_manager.h"

namespace fallout {

// The maximum number of elevator levels.
#define ELEVATOR_LEVEL_MAX (4)

// Max number of elevators that can be loaded from elevators.ini. This limit is
// emposed by Sfall.
#define ELEVATORS_MAX 50

typedef enum ElevatorFrm {
    ELEVATOR_FRM_BUTTON_DOWN,
    ELEVATOR_FRM_BUTTON_UP,
    ELEVATOR_FRM_GAUGE,
    ELEVATOR_FRM_COUNT,
} ElevatorFrm;

typedef struct ElevatorBackground {
    int backgroundFrmId;
    int panelFrmId;
} ElevatorBackground;


static int elevatorWindowInit(int elevator);
static void elevatorWindowFree();
static int elevatorGetLevelFromKeyCode(int elevator, int keyCode);

// 0x43E950
static const int gElevatorFrmIds[ELEVATOR_FRM_COUNT] = {
    141, // ebut_in.frm - map elevator screen
    142, // ebut_out.frm - map elevator screen
    149, // gaj000.frm - map elevator screen
};

// 0x43E95C
static ElevatorBackground gElevatorBackgrounds[ELEVATORS_MAX] = {
    { 143, -1 },
    { 143, 150 },
    { 144, -1 },
    { 144, 145 },
    { 146, -1 },
    { 146, 147 },
    { 146, -1 },
    { 146, 151 },
    { 148, -1 },
    { 146, -1 },
    { 146, -1 },
    { 146, 147 },
    { 388, -1 },
    { 143, 150 },
    { 148, -1 },
    { 148, -1 },
    { 148, -1 },
    { 143, 150 },
    { 143, 150 },
    { 143, 150 },
    { 143, 150 },
    { 143, 150 },
    { 143, 150 },
    { 143, 150 },
};



// NOTE: These values are also used as key bindings.
//
// 0x43EEFC
static char gElevatorLevelLabels[ELEVATORS_MAX][ELEVATOR_LEVEL_MAX] = {
    { '1', '2', '3', '4' },
    { 'G', '1', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '6', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '3', '4', '\0', '\0' },
    { '1', '2', '3', '4' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '3', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
    { '1', '2', '\0', '\0' },
};

// 0x51862C
static const char* gElevatorSoundEffects[ELEVATOR_LEVEL_MAX - 1][ELEVATOR_LEVEL_MAX] = {
    {
        "ELV1_1",
        "ELV1_1",
        "ERROR",
        "ERROR",
    },
    {
        "ELV1_2",
        "ELV1_2",
        "ELV1_1",
        "ERROR",
    },
    {
        "ELV1_3",
        "ELV1_3",
        "ELV2_3",
        "ELV1_1",
    },
};

// 0x570A54
static int gElevatorWindow;

// 0x570A6C
static unsigned char* gElevatorWindowBuffer;

// 0x570A70
static bool gElevatorWindowIsoWasEnabled;

static FrmImage _elevatorFrmImages[ELEVATOR_FRM_COUNT];
static FrmImage _elevatorBackgroundFrmImage;
static FrmImage _elevatorPanelFrmImage;


// Presents elevator dialog for player to pick a desired level.
//
// 0x43EF5C
int elevatorSelectLevel(int elevator, int* mapPtr, int* elevationPtr, int* tilePtr)
{
    int startLevel = elevatorResolveStartLevel(elevator, *mapPtr, *elevationPtr);
    int level = elevatorPickLevel(elevator, startLevel);
    if (level < 0) {
        return level == -2 ? -1 : 0; // -2 = could not open the screen; -1 = escaped
    }

    elevatorResolveDestination(elevator, level, mapPtr, elevationPtr, tilePtr);

    return 0;
}

// THE PICKER, split out of elevatorSelectLevel so the co-op client can ask "which
// button did the player press" WITHOUT resolving what that button means.
//
// ►► That separation is the trust boundary (elevator.h): in co-op the server owns the
// destination table and resolves the level itself, so this returns a LEVEL INDEX and
// nothing else. Returns -1 if the player escaped, -2 if the screen could not open.
//
// The gauge animation stays — it is local, already written, and it is what makes the
// ride read as a ride. The RIDE itself is instant either way.
int elevatorPickLevel(int elevator, int startLevel)
{
    if (elevator < 0 || elevator >= ELEVATORS_MAX) {
        return -2;
    }

    // SFALL
    if (elevatorWindowInit(elevator) == -1) {
        return -2;
    }

    int elevationValue = startLevel;
    int* elevationPtr = &elevationValue;

    debugPrint("\n the start elev level %d\n", *elevationPtr);

    int v18 = (_elevatorFrmImages[ELEVATOR_FRM_GAUGE].getWidth() * _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getHeight()) / 13;
    float v42 = 12.0f / (float)(elevatorLevelCount(elevator) - 1);
    blitBufferToBuffer(
        _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getData() + v18 * (int)((float)(*elevationPtr) * v42),
        _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getWidth(),
        _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getHeight() / 13,
        _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getWidth(),
        gElevatorWindowBuffer + _elevatorBackgroundFrmImage.getWidth() * 41 + 121,
        _elevatorBackgroundFrmImage.getWidth());
    windowRefresh(gElevatorWindow);

    bool done = false;
    int keyCode;
    while (!done) {
        sharedFpsLimiter.mark();

        keyCode = inputGetInput();
        if (keyCode == KEY_ESCAPE) {
            done = true;
        }

        if (keyCode >= 500 && keyCode < 504) {
            done = true;
        }

        if (keyCode > 0 && keyCode < 500) {
            int level = elevatorGetLevelFromKeyCode(elevator, keyCode);
            if (level != 0) {
                keyCode = 500 + level - 1;
                done = true;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (keyCode != KEY_ESCAPE) {
        keyCode -= 500;

        if (*elevationPtr != keyCode) {
            float v43 = (float)(elevatorLevelCount(elevator) - 1) / 12.0f;

            unsigned int delay = (unsigned int)(v43 * 276.92307);

            if (keyCode < *elevationPtr) {
                v43 = -v43;
            }

            int numberOfLevelsTravelled = keyCode - *elevationPtr;
            if (numberOfLevelsTravelled < 0) {
                numberOfLevelsTravelled = -numberOfLevelsTravelled;
            }

            soundPlayFile(gElevatorSoundEffects[elevatorLevelCount(elevator) - 2][numberOfLevelsTravelled]);

            float v41 = (float)keyCode * v42;
            float v44 = (float)(*elevationPtr) * v42;
            do {
                sharedFpsLimiter.mark();

                unsigned int tick = getTicks();
                v44 += v43;
                blitBufferToBuffer(
                    _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getData() + v18 * (int)v44,
                    _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getWidth(),
                    _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getHeight() / 13,
                    _elevatorFrmImages[ELEVATOR_FRM_GAUGE].getWidth(),
                    gElevatorWindowBuffer + _elevatorBackgroundFrmImage.getWidth() * 41 + 121,
                    _elevatorBackgroundFrmImage.getWidth());

                windowRefresh(gElevatorWindow);

                delay_ms(delay - (getTicks() - tick));

                renderPresent();
                sharedFpsLimiter.throttle();
            } while ((v43 <= 0.0 || v44 < v41) && (v43 > 0.0 || v44 > v41));

            inputPauseForTocks(200);
        }
    }

    elevatorWindowFree();

    return keyCode != KEY_ESCAPE ? keyCode : -1;
}

// 0x43F324
static int elevatorWindowInit(int elevator)
{
    gElevatorWindowIsoWasEnabled = isoDisable();
    colorCycleDisable();

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    gameMouseObjectsHide();

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    scriptsDisable();

    int index;
    for (index = 0; index < ELEVATOR_FRM_COUNT; index++) {
        int fid = buildFid(OBJ_TYPE_INTERFACE, gElevatorFrmIds[index], 0, 0, 0);
        if (!_elevatorFrmImages[index].lock(fid)) {
            break;
        }
    }

    if (index != ELEVATOR_FRM_COUNT) {
        for (int reversedIndex = index - 1; reversedIndex >= 0; reversedIndex--) {
            _elevatorFrmImages[reversedIndex].unlock();
        }

        if (gElevatorWindowIsoWasEnabled) {
            isoEnable();
        }

        colorCycleEnable();
        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    const ElevatorBackground* elevatorBackground = &(gElevatorBackgrounds[elevator]);
    bool backgroundsLoaded = true;

    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, elevatorBackground->backgroundFrmId, 0, 0, 0);
    if (_elevatorBackgroundFrmImage.lock(backgroundFid)) {
        if (elevatorBackground->panelFrmId != -1) {
            int panelFid = buildFid(OBJ_TYPE_INTERFACE, elevatorBackground->panelFrmId, 0, 0, 0);
            if (!_elevatorPanelFrmImage.lock(panelFid)) {
                backgroundsLoaded = false;
            }
        }
    } else {
        backgroundsLoaded = false;
    }

    if (!backgroundsLoaded) {
        _elevatorBackgroundFrmImage.unlock();
        _elevatorPanelFrmImage.unlock();

        for (int index = 0; index < ELEVATOR_FRM_COUNT; index++) {
            _elevatorFrmImages[index].unlock();
        }

        if (gElevatorWindowIsoWasEnabled) {
            isoEnable();
        }

        colorCycleEnable();
        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    int elevatorWindowX = (screenGetWidth() - _elevatorBackgroundFrmImage.getWidth()) / 2;
    int elevatorWindowY = (screenGetHeight() - INTERFACE_BAR_HEIGHT - 1 - _elevatorBackgroundFrmImage.getHeight()) / 2;
    gElevatorWindow = windowCreate(
        elevatorWindowX,
        elevatorWindowY,
        _elevatorBackgroundFrmImage.getWidth(),
        _elevatorBackgroundFrmImage.getHeight(),
        256,
        WINDOW_MODAL | WINDOW_DONT_MOVE_TOP);
    if (gElevatorWindow == -1) {
        _elevatorBackgroundFrmImage.unlock();
        _elevatorPanelFrmImage.unlock();

        for (int index = 0; index < ELEVATOR_FRM_COUNT; index++) {
            _elevatorFrmImages[index].unlock();
        }

        if (gElevatorWindowIsoWasEnabled) {
            isoEnable();
        }

        colorCycleEnable();
        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
        return -1;
    }

    gElevatorWindowBuffer = windowGetBuffer(gElevatorWindow);
    memcpy(gElevatorWindowBuffer, _elevatorBackgroundFrmImage.getData(), _elevatorBackgroundFrmImage.getWidth() * _elevatorBackgroundFrmImage.getHeight());

    if (_elevatorPanelFrmImage.isLocked()) {
        blitBufferToBuffer(_elevatorPanelFrmImage.getData(),
            _elevatorPanelFrmImage.getWidth(),
            _elevatorPanelFrmImage.getHeight(),
            _elevatorPanelFrmImage.getWidth(),
            gElevatorWindowBuffer + _elevatorBackgroundFrmImage.getWidth() * (_elevatorBackgroundFrmImage.getHeight() - _elevatorPanelFrmImage.getHeight()),
            _elevatorBackgroundFrmImage.getWidth());
    }

    int y = 40;
    for (int level = 0; level < elevatorLevelCount(elevator); level++) {
        int btn = buttonCreate(gElevatorWindow,
            13,
            y,
            _elevatorFrmImages[ELEVATOR_FRM_BUTTON_DOWN].getWidth(),
            _elevatorFrmImages[ELEVATOR_FRM_BUTTON_DOWN].getHeight(),
            -1,
            -1,
            -1,
            500 + level,
            _elevatorFrmImages[ELEVATOR_FRM_BUTTON_UP].getData(),
            _elevatorFrmImages[ELEVATOR_FRM_BUTTON_DOWN].getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (btn != -1) {
            buttonSetCallbacks(btn, _gsound_red_butt_press, nullptr);
        }
        y += 60;
    }

    return 0;
}

// 0x43F6D0
static void elevatorWindowFree()
{
    windowDestroy(gElevatorWindow);

    _elevatorBackgroundFrmImage.unlock();
    _elevatorPanelFrmImage.unlock();

    for (int index = 0; index < ELEVATOR_FRM_COUNT; index++) {
        _elevatorFrmImages[index].unlock();
    }

    scriptsEnable();

    if (gElevatorWindowIsoWasEnabled) {
        isoEnable();
    }

    colorCycleEnable();

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
}

// 0x43F73C
static int elevatorGetLevelFromKeyCode(int elevator, int keyCode)
{
    // TODO: Check if result is really unused?
    toupper(keyCode);

    for (int index = 0; index < ELEVATOR_LEVEL_MAX; index++) {
        char c = gElevatorLevelLabels[elevator][index];
        if (c == '\0') {
            break;
        }

        if (c == (char)(keyCode & 0xFF)) {
            return index + 1;
        }
    }
    return 0;
}

// The ART half of the sfall elevators.ini override; elevatorsDataInit()
// (elevator_data.cc, f2_core) owns the level count and the destination table, and
// is called from here so the one existing call site still initialises both halves.
// The server calls the data half directly.
void elevatorsInit()
{
    elevatorsDataInit();

    char* elevatorsFileName;
    configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_ELEVATORS_FILE_KEY, &elevatorsFileName);
    if (elevatorsFileName != nullptr && *elevatorsFileName == '\0') {
        elevatorsFileName = nullptr;
    }

    if (elevatorsFileName != nullptr) {
        Config elevatorsConfig;
        if (configInit(&elevatorsConfig)) {
            if (configRead(&elevatorsConfig, elevatorsFileName, false)) {
                char sectionKey[4];
                char key[32];
                for (int index = 0; index < ELEVATORS_MAX; index++) {
                    snprintf(sectionKey, sizeof(sectionKey), "%d", index);

                    configGetInt(&elevatorsConfig, sectionKey, "MainFrm", &(gElevatorBackgrounds[index].backgroundFrmId));
                    configGetInt(&elevatorsConfig, sectionKey, "ButtonsFrm", &(gElevatorBackgrounds[index].panelFrmId));

                }

                // NOTE: Sfall implementation is slightly different. It uses one
                // loop and stores `type` value in a separate lookup table. This
                // value is then used in the certain places to remap from
                // requested elevator to the new one.
                for (int index = 0; index < ELEVATORS_MAX; index++) {
                    snprintf(sectionKey, sizeof(sectionKey), "%d", index);

                    int type;
                    if (configGetInt(&elevatorsConfig, sectionKey, "Image", &type)) {
                        type = std::clamp(type, 0, ELEVATORS_MAX - 1);
                        if (index != type) {
                            memcpy(&(gElevatorBackgrounds[index]), &(gElevatorBackgrounds[type]), sizeof(*gElevatorBackgrounds));
                            memcpy(&(gElevatorLevelLabels[index]), &(gElevatorLevelLabels[type]), sizeof(*gElevatorLevelLabels));
                        }
                    }
                }
            }

            configFree(&elevatorsConfig);
        }
    }
}

} // namespace fallout
