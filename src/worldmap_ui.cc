#include "worldmap_ui.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "animation.h"
#include "art.h"
#include "automap.h"
#include "client_net.h"
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
#include "game.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "memory.h"
#include "mouse.h"
#include "object.h"
#include "palette.h"
#include "party_member.h"
#include "perk.h"
#include "presenter.h"
#include "proto_instance.h"
#include "queue.h"
#include "random.h"
#include "scripts.h"
#include "settings.h"
#include "sfall_config.h"
#include "sfall_global_scripts.h"
#include "skill.h"
#include "stat.h"
#include "string_parsers.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"
#include "worldmap.h"
#include "worldmap_defs.h"

namespace fallout {

// 0x51DE14
static int wmBkWin = -1;

// 0x51DE24
static unsigned char* wmBkWinBuf = nullptr;

// 0x51DE38
static int wmInterfaceWasInitialized = 0;

bool gPendingWorldmapEnter = false;
bool gWorldmapStreaming = false;
bool gWorldmapStateDirty = false;

// 0x51DE84
static const int wmRndCursorFids[WORLD_MAP_ENCOUNTER_FRM_COUNT] = {
    154,
    155,
    438,
    439,
};

// 0x51DE94
static int* wmLabelList = nullptr;

// 0x51DE98
static int wmLabelCount = 0;

// 0x51DE9C
static int wmTownMapCurArea = -1;

// Buttons for city entrances.
//
// 0x672DD8
static int wmTownMapButtonId[ENTRANCE_LIST_CAPACITY];

// 0x672FD8
static int wmTownMapSubButtonIds[7];

static FrmImage _backgroundFrmImage;

static FrmImage _townFrmImage;

static bool wmFaded = false;

static int wmWorldMapFunc(int a1);
static void wmCheckGameEvents();
static void wmInterfaceScrollTabsStart(int delta);
static void wmInterfaceScrollTabsStop();
static void wmInterfaceScrollTabsUpdate();
static int wmInterfaceInit();
static int wmInterfaceExit();
static int wmInterfaceScroll(int dx, int dy, bool* successPtr);
static void wmMouseBkProc();
static int wmTileGrabArt(int tileIdx);
static int wmInterfaceRefresh();
static int wmInterfaceDrawCircleOverlay(CityInfo* cityInfo, CitySizeDescription* citySizeInfo, unsigned char* buffer, int x, int y);
static void wmInterfaceDrawSubTileRectFogged(unsigned char* dest, int width, int height, int pitch);
static int wmInterfaceDrawSubTileList(TileInfo* tileInfo, int column, int row, int x, int y, int a6);
static int wmDrawCursorStopped();
static int wmTownMapFunc(int* mapIdxPtr);
static int wmTownMapInit();
static int wmTownMapRefresh();
static int wmTownMapExit();
static int wmRefreshInterfaceOverlay(bool shouldRefreshWindow);
static void wmInterfaceRefreshCarFuel();
static int wmRefreshTabs();
static int wmMakeTabsLabelList(int** quickDestinationsPtr, int* quickDestinationsLengthPtr);
static int wmTabsCompareNames(const void* a1, const void* a2);
static int wmFreeTabsLabelList(int** quickDestinationsListPtr, int* quickDestinationsLengthPtr);
static void wmRefreshInterfaceDial(bool shouldRefreshWindow);

// 0x4BFE0C
void wmWorldMap()
{
    wmWorldMapFunc(0);
}

// 0x4BFE10
static int wmWorldMapFunc(int a1)
{
    ScopedGameMode gm(GameMode::kWorldmap);

    wmFadeOut();

    if (wmInterfaceInit() == -1) {
        wmInterfaceExit();
        wmFadeReset();
        return -1;
    }

    wmFadeIn();

    wmMatchWorldPosToArea(wmGenData.worldPosX, wmGenData.worldPosY, &(wmGenData.currentAreaId));

    unsigned int partyHealTime = 0;
    int map = -1;
    int rc = 0;

    bool streamingEntered = gWorldmapStreaming;

    while (true) {
        sharedFpsLimiter.mark();

        // Streaming mode: the server ended the worldmap session; exit.
        if (streamingEntered && !gWorldmapStreaming) {
            break;
        }

        int keyCode = inputGetInput();

        // SFALL: WorldmapLoopHook.
        sfall_gl_scr_process_worldmap();

        unsigned int now = getTicks();

        int mouseX;
        int mouseY;
        mouseGetPositionInWindow(wmBkWin, &mouseX, &mouseY);

        int worldX = wmWorldOffsetX + mouseX - WM_VIEW_X;
        int worldY = wmWorldOffsetY + mouseY - WM_VIEW_Y;

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
        }

        // NOTE: Uninline.
        wmCheckGameEvents();

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        int mouseEvent = mouseGetEvent();

        if (gWorldmapStreaming) {
            // The server owns the travel sim; we just render whatever state the
            // wire last delivered. Repaint on EVERY state change, NOT only while
            // walking: the arrival frame is precisely the one where isWalking
            // flips to false, and refreshing only inside an isWalking branch
            // means that final frame is never drawn — the map keeps showing the
            // travelling cross instead of the arrived-at marker until some other
            // event happens to force a repaint.
            if (gWorldmapStateDirty) {
                gWorldmapStateDirty = false;
                if (wmGenData.isWalking) {
                    // Follow the party while travelling. Vanilla gets this from
                    // wmInterfaceScrollPixel inside wmPartyWalkingStep, which is
                    // suppressed server-side (it is pure chrome).
                    wmInterfaceCenterOnParty();
                }
                wmInterfaceRefresh();
            }
        } else if (wmGenData.isWalking) {
            {
                worldmapTravelStep(worldX, worldY);

                wmInterfaceRefresh();

                if (worldmapTravelRestHeal(now, partyHealTime)) {
                    interfaceRenderHitPoints(false);
                    partyHealTime = now;
                }

                worldmapTravelMarkVisited();

                wmInterfaceRefresh();

                if (worldmapTravelClockTick()) {
                    if (_game_user_wants_to_quit != 0) {
                        break;
                    }
                }

                if (wmGenData.isWalking) {
                    if (worldmapTravelEncounterCheck()) {
                        if (wmGenData.encounterMapId != -1) {
                            wmFadeOut();
                            mapLoadById(wmGenData.encounterMapId);
                        }
                        break;
                    }
                }
            }
        }

        if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0 && (mouseEvent & MOUSE_EVENT_LEFT_BUTTON_REPEAT) == 0) {
            if (mouseHitTestInWindow(wmBkWin, WM_VIEW_X, WM_VIEW_Y, 472, 465)) {
                if (!wmGenData.isWalking && !wmGenData.mousePressed && abs(wmGenData.worldPosX - worldX) < 5 && abs(wmGenData.worldPosY - worldY) < 5) {
                    wmGenData.mousePressed = true;
                    wmInterfaceRefresh();
                    renderPresent();
                }
            } else {
                continue;
            }
        }

        if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
            if (wmGenData.mousePressed) {
                wmGenData.mousePressed = false;
                wmInterfaceRefresh();

                if (abs(wmGenData.worldPosX - worldX) < 5 && abs(wmGenData.worldPosY - worldY) < 5) {
                    if (gWorldmapStreaming) {
                        // Send UNCONDITIONALLY — the server resolves the
                        // destination (a city's first valid map, or map 0 for
                        // open wasteland). Gating this on currentAreaId != -1
                        // would make clicking your own position anywhere but a
                        // known town do nothing at all: the local `map = 0`
                        // below is inert here, since only the server may load.
                        clientViewerWmEnter();
                    } else if (wmGenData.currentAreaId != -1) {
                        CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);
                        if (city->visitedState == 2 && city->mapFid != -1) {
                            if (wmTownMapFunc(&map) == -1) {
                                rc = -1;
                                break;
                            }
                        } else {
                            if (wmAreaFindFirstValidMap(&map) == -1) {
                                rc = -1;
                                break;
                            }

                            city->visitedState = 2;
                        }
                    } else {
                        map = 0;
                    }

                    if (map != -1 && !gWorldmapStreaming) {
                        if (wmGenData.isInCar) {
                            wmGenData.isInCar = false;
                            if (wmGenData.currentAreaId == -1) {
                                wmMatchAreaContainingMapIdx(map, &(wmGenData.currentCarAreaId));
                            } else {
                                wmGenData.currentCarAreaId = wmGenData.currentAreaId;
                            }
                        }

                        wmFadeOut();
                        mapLoadById(map);
                        break;
                    }
                }
            } else {
                if (mouseHitTestInWindow(wmBkWin, WM_VIEW_X, WM_VIEW_Y, 472, 465)) {
                    if (gWorldmapStreaming) {
                        clientViewerWmMove(worldX, worldY);
                    } else {
                        wmPartyInitWalking(worldX, worldY);
                    }
                }

                wmGenData.mousePressed = false;
            }
        }

        // NOTE: Uninline.
        wmInterfaceScrollTabsUpdate();

        if (keyCode == KEY_UPPERCASE_T || keyCode == KEY_LOWERCASE_T) {
            if (!wmGenData.isWalking && wmGenData.currentAreaId != -1) {
                if (gWorldmapStreaming) {
                    clientViewerWmEnter();
                } else {
                    CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);
                    if (city->visitedState == 2 && city->mapFid != -1) {
                        if (wmTownMapFunc(&map) == -1) {
                            rc = -1;
                        }

                        if (map != -1) {
                            if (wmGenData.isInCar) {
                                wmGenData.isInCar = false;
                                wmMatchAreaContainingMapIdx(map, &(wmGenData.currentCarAreaId));
                            }

                            wmFadeOut();
                            mapLoadById(map);
                        }
                    }
                }
            }
        } else if (keyCode == KEY_ESCAPE) {
            if (gWorldmapStreaming) {
                clientViewerWmEscape();
            }
        } else if (keyCode == KEY_HOME) {
            wmInterfaceCenterOnParty();
        } else if (keyCode == KEY_ARROW_UP) {
            // NOTE: Uninline.
            wmInterfaceScroll(0, -1, nullptr);
        } else if (keyCode == KEY_ARROW_LEFT) {
            // NOTE: Uninline.
            wmInterfaceScroll(-1, 0, nullptr);
        } else if (keyCode == KEY_ARROW_DOWN) {
            // NOTE: Uninline.
            wmInterfaceScroll(0, 1, nullptr);
        } else if (keyCode == KEY_ARROW_RIGHT) {
            // NOTE: Uninline.
            wmInterfaceScroll(1, 0, nullptr);
        } else if (keyCode == KEY_CTRL_ARROW_UP) {
            wmInterfaceScrollTabsStart(-27);
        } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
            wmInterfaceScrollTabsStart(27);
        } else if (keyCode >= KEY_CTRL_F1 && keyCode <= KEY_CTRL_F7) {
            int quickDestinationIndex = wmGenData.tabsOffsetY / 27 + (keyCode - KEY_CTRL_F1);
            if (quickDestinationIndex < wmLabelCount) {
                int areaIdx = wmLabelList[quickDestinationIndex];
                CityInfo* city = &(wmAreaInfoList[areaIdx]);
                if (wmAreaIsKnown(city->areaId)) {
                    if (wmGenData.currentAreaId != areaIdx) {
                        CitySizeDescription* citySizeDescription = &(wmSphereData[city->size]);
                        int destX = city->x + citySizeDescription->frmImage.getWidth() / 2 - WM_VIEW_X;
                        int destY = city->y + citySizeDescription->frmImage.getHeight() / 2 - WM_VIEW_Y;
                        if (gWorldmapStreaming) {
                            clientViewerWmMove(destX, destY);
                        } else {
                            wmPartyInitWalking(destX, destY);
                        }
                        wmGenData.mousePressed = 0;
                    }
                }
            }
        }

        if ((mouseEvent & MOUSE_EVENT_WHEEL) != 0) {
            int wheelX;
            int wheelY;
            mouseGetWheel(&wheelX, &wheelY);

            if (mouseHitTestInWindow(wmBkWin, WM_VIEW_X, WM_VIEW_Y, 472, 465)) {
                wmInterfaceScrollPixel(20, 20, wheelX, -wheelY, nullptr, true);
            } else if (mouseHitTestInWindow(wmBkWin, 501, 135, 501 + 119, 135 + 178)) {
                if (wheelY != 0) {
                    wmInterfaceScrollTabsStart(wheelY > 0 ? 27 : -27);
                }
            }
        }

        if (map != -1 || rc == -1) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (wmInterfaceExit() == -1) {
        wmFadeReset();
        return -1;
    }

    wmFadeIn();

    return rc;
}

// 0x4C05C4
int wmInterfaceCenterOnParty()
{
    wmWorldOffsetX = std::clamp(wmGenData.worldPosX - 203, 0, wmGenData.viewportMaxX);
    wmWorldOffsetY = std::clamp(wmGenData.worldPosY - 200, 0, wmGenData.viewportMaxY);

    wmInterfaceRefresh();

    return 0;
}

// NOTE: Inlined.
//
// 0x4C0624
static void wmCheckGameEvents()
{
    _scriptsCheckGameEvents(nullptr, wmBkWin);
}

// 0x4C219C
static void wmInterfaceScrollTabsStart(int delta)
{
    // SFALL: Fix world map cities list scrolling bug that might leave buttons
    // in the disabled state.
    if (delta >= 0) {
        if (wmGenData.tabsOffsetY < wmGenData.tabsBackgroundFrmImage.getHeight() - 230) {
            wmGenData.oldTabsOffsetY = std::min(wmGenData.tabsOffsetY + 7 * delta, wmGenData.tabsBackgroundFrmImage.getHeight() - 230);
            wmGenData.tabsScrollingDelta = delta;
        }
    } else {
        if (wmGenData.tabsOffsetY > 0) {
            wmGenData.oldTabsOffsetY = std::max(wmGenData.tabsOffsetY + 7 * delta, 0);
            wmGenData.tabsScrollingDelta = delta;
        }
    }

    if (wmGenData.tabsScrollingDelta == 0) {
        return;
    }

    for (int index = 0; index < 7; index++) {
        buttonDisable(wmTownMapSubButtonIds[index]);
    }

    wmInterfaceScrollTabsUpdate();
}

// 0x4C2270
static void wmInterfaceScrollTabsStop()
{
    wmGenData.tabsScrollingDelta = 0;

    for (int index = 0; index < 7; index++) {
        buttonEnable(wmTownMapSubButtonIds[index]);
    }
}

// NOTE: Inlined.
//
// 0x4C2290
static void wmInterfaceScrollTabsUpdate()
{
    if (wmGenData.tabsScrollingDelta != 0) {
        wmGenData.tabsOffsetY += wmGenData.tabsScrollingDelta;
        wmRefreshInterfaceOverlay(true);

        if (wmGenData.tabsScrollingDelta >= 0) {
            if (wmGenData.oldTabsOffsetY <= wmGenData.tabsOffsetY) {
                // NOTE: Uninline.
                wmInterfaceScrollTabsStop();
            }
        } else {
            if (wmGenData.oldTabsOffsetY >= wmGenData.tabsOffsetY) {
                // NOTE: Uninline.
                wmInterfaceScrollTabsStop();
            }
        }
    }
}

// 0x4C2324
static int wmInterfaceInit()
{
    int fid;

    wmLastRndTime = getTicks();

    // SFALL: Fix default worldmap font.
    // CE: This setting affects only city names. In Sfall it's configurable via
    // WorldMapFontPatch and is turned off by default.
    wmGenData.oldFont = fontGetCurrent();
    fontSetCurrent(101);

    wmTransitionSaveMap();

    const char* backgroundSoundFileName = wmGenData.isInCar ? "20car" : "23world";
    _gsound_background_play_level_music(backgroundSoundFileName, 12);

    // CE: Hide entire interface, not just indicator bar, and disable tile
    // engine.
    interfaceBarHide();
    tileDisable();
    isoDisable();
    colorCycleDisable();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    // CE: Clear map window.
    windowFill(gIsoWindow,
        0,
        0,
        windowGetWidth(gIsoWindow),
        windowGetHeight(gIsoWindow),
        _colorTable[0]);
    windowRefresh(gIsoWindow);

    // CE: Stop all animations.
    animationStop();

    int worldmapWindowX = (screenGetWidth() - WM_WINDOW_WIDTH) / 2;
    int worldmapWindowY = (screenGetHeight() - WM_WINDOW_HEIGHT) / 2;
    wmBkWin = windowCreate(worldmapWindowX, worldmapWindowY, WM_WINDOW_WIDTH, WM_WINDOW_HEIGHT, _colorTable[0], WINDOW_MOVE_ON_TOP);
    if (wmBkWin == -1) {
        return -1;
    }

    fid = buildFid(OBJ_TYPE_INTERFACE, 136, 0, 0, 0);
    if (!_backgroundFrmImage.lock(fid)) {
        return -1;
    }

    wmBkWinBuf = windowGetBuffer(wmBkWin);
    if (wmBkWinBuf == nullptr) {
        return -1;
    }

    blitBufferToBuffer(_backgroundFrmImage.getData(),
        _backgroundFrmImage.getWidth(),
        _backgroundFrmImage.getHeight(),
        _backgroundFrmImage.getWidth(),
        wmBkWinBuf,
        WM_WINDOW_WIDTH);

    for (int citySize = 0; citySize < CITY_SIZE_COUNT; citySize++) {
        CitySizeDescription* citySizeDescription = &(wmSphereData[citySize]);
        if (!citySizeDescription->frmImage.lock(citySizeDescription->fid)) {
            return -1;
        }
    }

    // hotspot1.frm - town map selector shape #1
    fid = buildFid(OBJ_TYPE_INTERFACE, 168, 0, 0, 0);
    if (!wmGenData.hotspotNormalFrmImage.lock(fid)) {
        return -1;
    }

    // hotspot2.frm - town map selector shape #2
    fid = buildFid(OBJ_TYPE_INTERFACE, 223, 0, 0, 0);
    if (!wmGenData.hotspotPressedFrmImage.lock(fid)) {
        return -1;
    }

    // wmaptarg.frm - world map move target maker #1
    fid = buildFid(OBJ_TYPE_INTERFACE, 139, 0, 0, 0);
    if (!wmGenData.destinationMarkerFrmImage.lock(fid)) {
        return -1;
    }

    // wmaploc.frm - world map location marker
    fid = buildFid(OBJ_TYPE_INTERFACE, 138, 0, 0, 0);
    if (!wmGenData.locationMarkerFrmImage.lock(fid)) {
        return -1;
    }

    for (int index = 0; index < WORLD_MAP_ENCOUNTER_FRM_COUNT; index++) {
        fid = buildFid(OBJ_TYPE_INTERFACE, wmRndCursorFids[index], 0, 0, 0);
        if (!wmGenData.encounterCursorFrmImages[index].lock(fid)) {
            return -1;
        }
    }

    for (int index = 0; index < wmMaxTileNum; index++) {
        wmTileInfoList[index].handle = INVALID_CACHE_ENTRY;
    }

    // wmtabs.frm - worldmap town tabs underlay
    fid = buildFid(OBJ_TYPE_INTERFACE, 364, 0, 0, 0);
    if (!wmGenData.tabsBackgroundFrmImage.lock(fid)) {
        return -1;
    }

    // wmtbedge.frm - worldmap town tabs edging overlay
    fid = buildFid(OBJ_TYPE_INTERFACE, 367, 0, 0, 0);
    if (!wmGenData.tabsBorderFrmImage.lock(fid)) {
        return -1;
    }

    // wmdial.frm - worldmap night/day dial
    fid = buildFid(OBJ_TYPE_INTERFACE, 365, 0, 0, 0);
    wmGenData.dialFrm = artLock(fid, &(wmGenData.dialFrmHandle));
    if (wmGenData.dialFrm == nullptr) {
        return -1;
    }

    wmGenData.dialFrmWidth = artGetWidth(wmGenData.dialFrm, 0, 0);
    wmGenData.dialFrmHeight = artGetHeight(wmGenData.dialFrm, 0, 0);

    // wmscreen - worldmap overlay screen
    fid = buildFid(OBJ_TYPE_INTERFACE, 363, 0, 0, 0);
    if (!wmGenData.carOverlayFrmImage.lock(fid)) {
        return -1;
    }

    // wmglobe.frm - worldmap globe stamp overlay
    fid = buildFid(OBJ_TYPE_INTERFACE, 366, 0, 0, 0);
    if (!wmGenData.globeOverlayFrmImage.lock(fid)) {
        return -1;
    }

    // lilredup.frm - little red button up
    fid = buildFid(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    wmGenData.redButtonNormalFrmImage.lock(fid);

    // lilreddn.frm - little red button down
    fid = buildFid(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    wmGenData.redButtonPressedFrmImage.lock(fid);

    // months.frm - month strings for pip boy
    fid = buildFid(OBJ_TYPE_INTERFACE, 129, 0, 0, 0);
    if (!wmGenData.monthsFrmImage.lock(fid)) {
        return -1;
    }

    // numbers.frm - numbers for the hit points and fatigue counters
    fid = buildFid(OBJ_TYPE_INTERFACE, 82, 0, 0, 0);
    if (!wmGenData.numbersFrmImage.lock(fid)) {
        return -1;
    }

    // create town/world switch button
    int switchBtn = buttonCreate(wmBkWin,
        WM_TOWN_WORLD_SWITCH_X,
        WM_TOWN_WORLD_SWITCH_Y,
        wmGenData.redButtonNormalFrmImage.getWidth(),
        wmGenData.redButtonNormalFrmImage.getHeight(),
        -1,
        -1,
        -1,
        KEY_UPPERCASE_T,
        wmGenData.redButtonNormalFrmImage.getData(),
        wmGenData.redButtonPressedFrmImage.getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);

    // SFALL: Add missing button sounds.
    if (switchBtn != -1) {
        buttonSetCallbacks(switchBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    }

    for (int index = 0; index < 7; index++) {
        wmTownMapSubButtonIds[index] = buttonCreate(wmBkWin,
            508,
            138 + 27 * index,
            wmGenData.redButtonNormalFrmImage.getWidth(),
            wmGenData.redButtonNormalFrmImage.getHeight(),
            -1,
            -1,
            -1,
            KEY_CTRL_F1 + index,
            wmGenData.redButtonNormalFrmImage.getData(),
            wmGenData.redButtonPressedFrmImage.getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);

        // SFALL: Add missing button sounds.
        if (wmTownMapSubButtonIds[index] != -1) {
            buttonSetCallbacks(wmTownMapSubButtonIds[index], _gsound_red_butt_press, _gsound_red_butt_release);
        }
    }

    for (int index = 0; index < WORLDMAP_ARROW_FRM_COUNT; index++) {
        // 200 - uparwon.frm - character editor
        // 199 - uparwoff.frm - character editor
        // SFALL: Fix images for scroll buttons.
        fid = buildFid(OBJ_TYPE_INTERFACE, 199 + index, 0, 0, 0);
        if (!wmGenData.scrollUpButtonFrmImages[index].lock(fid)) {
            return -1;
        }
    }

    for (int index = 0; index < WORLDMAP_ARROW_FRM_COUNT; index++) {
        // 182 - dnarwon.frm - character editor
        // 181 - dnarwoff.frm - character editor
        // SFALL: Fix images for scroll buttons.
        fid = buildFid(OBJ_TYPE_INTERFACE, 181 + index, 0, 0, 0);
        if (!wmGenData.scrollDownButtonFrmImages[index].lock(fid)) {
            return -1;
        }
    }

    // Scroll up button.
    int scrollUpBtn = buttonCreate(wmBkWin,
        WM_TOWN_LIST_SCROLL_UP_X,
        WM_TOWN_LIST_SCROLL_UP_Y,
        wmGenData.scrollUpButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getWidth(),
        wmGenData.scrollUpButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getHeight(),
        -1,
        -1,
        -1,
        KEY_CTRL_ARROW_UP,
        wmGenData.scrollUpButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getData(),
        wmGenData.scrollUpButtonFrmImages[WORLDMAP_ARROW_FRM_PRESSED].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);

    // SFALL: Add missing button sounds.
    if (scrollUpBtn != -1) {
        buttonSetCallbacks(scrollUpBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    }

    // Scroll down button.
    int scrollDownBtn = buttonCreate(wmBkWin,
        WM_TOWN_LIST_SCROLL_DOWN_X,
        WM_TOWN_LIST_SCROLL_DOWN_Y,
        wmGenData.scrollDownButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getWidth(),
        wmGenData.scrollDownButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getHeight(),
        -1,
        -1,
        -1,
        KEY_CTRL_ARROW_DOWN,
        wmGenData.scrollDownButtonFrmImages[WORLDMAP_ARROW_FRM_NORMAL].getData(),
        wmGenData.scrollDownButtonFrmImages[WORLDMAP_ARROW_FRM_PRESSED].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);

    // SFALL: Add missing button sounds.
    if (scrollDownBtn != -1) {
        buttonSetCallbacks(scrollDownBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    }

    if (wmGenData.isInCar) {
        // wmcarmve.frm - worldmap car movie
        fid = buildFid(OBJ_TYPE_INTERFACE, 433, 0, 0, 0);
        wmGenData.carImageFrm = artLock(fid, &(wmGenData.carImageFrmHandle));
        if (wmGenData.carImageFrm == nullptr) {
            return -1;
        }

        wmGenData.carImageFrmWidth = artGetWidth(wmGenData.carImageFrm, 0, 0);
        wmGenData.carImageFrmHeight = artGetHeight(wmGenData.carImageFrm, 0, 0);
    }

    tickersAdd(wmMouseBkProc);

    if (wmMakeTabsLabelList(&wmLabelList, &wmLabelCount) == -1) {
        return -1;
    }

    wmInterfaceWasInitialized = 1;

    if (wmInterfaceRefresh() == -1) {
        return -1;
    }

    windowRefresh(wmBkWin);
    wmTransitionSuspendScripts();

    return 0;
}

// 0x4C2E44
static int wmInterfaceExit()
{
    int i;
    TileInfo* tile;

    tickersRemove(wmMouseBkProc);

    _backgroundFrmImage.unlock();

    if (wmBkWin != -1) {
        windowDestroy(wmBkWin);
        wmBkWin = -1;
    }

    wmGenData.hotspotNormalFrmImage.unlock();
    wmGenData.hotspotPressedFrmImage.unlock();

    wmGenData.destinationMarkerFrmImage.unlock();
    wmGenData.locationMarkerFrmImage.unlock();

    for (i = 0; i < 4; i++) {
        wmGenData.encounterCursorFrmImages[i].unlock();
    }

    for (i = 0; i < CITY_SIZE_COUNT; i++) {
        CitySizeDescription* citySizeDescription = &(wmSphereData[i]);
        citySizeDescription->frmImage.unlock();
    }

    for (i = 0; i < wmMaxTileNum; i++) {
        tile = &(wmTileInfoList[i]);
        if (tile->handle != INVALID_CACHE_ENTRY) {
            artUnlock(tile->handle);
            tile->handle = INVALID_CACHE_ENTRY;
            tile->data = nullptr;

            if (tile->walkMaskData != nullptr) {
                internal_free(tile->walkMaskData);
                tile->walkMaskData = nullptr;
            }
        }
    }

    wmGenData.tabsBackgroundFrmImage.unlock();
    wmGenData.tabsBorderFrmImage.unlock();

    if (wmGenData.dialFrm != nullptr) {
        artUnlock(wmGenData.dialFrmHandle);
        wmGenData.dialFrmHandle = INVALID_CACHE_ENTRY;
        wmGenData.dialFrm = nullptr;
    }

    wmGenData.carOverlayFrmImage.unlock();
    wmGenData.globeOverlayFrmImage.unlock();

    wmGenData.redButtonNormalFrmImage.unlock();
    wmGenData.redButtonPressedFrmImage.unlock();

    for (i = 0; i < 2; i++) {
        wmGenData.scrollUpButtonFrmImages[i].unlock();
        wmGenData.scrollDownButtonFrmImages[i].unlock();
    }

    wmGenData.monthsFrmImage.unlock();
    wmGenData.numbersFrmImage.unlock();

    if (wmGenData.carImageFrm != nullptr) {
        artUnlock(wmGenData.carImageFrmHandle);
        wmGenData.carImageFrmHandle = INVALID_CACHE_ENTRY;
        wmGenData.carImageFrm = nullptr;

        wmGenData.carImageFrmWidth = 0;
        wmGenData.carImageFrmHeight = 0;
    }

    wmEncounterStagingClear();

    // CE: Enable tile engine and interface.
    interfaceBarShow();
    tileEnable();
    isoEnable();
    colorCycleEnable();

    fontSetCurrent(wmGenData.oldFont);

    // NOTE: Uninline.
    wmFreeTabsLabelList(&wmLabelList, &wmLabelCount);

    wmInterfaceWasInitialized = 0;

    wmTransitionResumeScripts();

    return 0;
}

// NOTE: Inlined.
//
// 0x4C31E8
static int wmInterfaceScroll(int dx, int dy, bool* successPtr)
{
    return wmInterfaceScrollPixel(20, 20, dx, dy, successPtr, 1);
}

// FIXME: There is small bug in this function. There is [success] flag returned
// by reference so that calling code can update scrolling mouse cursor to invalid
// range. It works OK on straight directions. But in diagonals when scrolling in
// one direction is possible (and in fact occured), it will still be reported as
// error.
//
// 0x4C3200
int wmInterfaceScrollPixel(int stepX, int stepY, int dx, int dy, bool* success, bool shouldRefresh)
{
    if (success != nullptr) {
        *success = true;
    }

    if (dy < 0) {
        if (wmWorldOffsetY > 0) {
            wmWorldOffsetY -= stepY;
            if (wmWorldOffsetY < 0) {
                wmWorldOffsetY = 0;
            }
        } else {
            if (success != nullptr) {
                *success = false;
            }
        }
    } else if (dy > 0) {
        if (wmWorldOffsetY < wmGenData.viewportMaxY) {
            wmWorldOffsetY += stepY;
            if (wmWorldOffsetY > wmGenData.viewportMaxY) {
                wmWorldOffsetY = wmGenData.viewportMaxY;
            }
        } else {
            if (success != nullptr) {
                *success = false;
            }
        }
    }

    if (dx < 0) {
        if (wmWorldOffsetX > 0) {
            wmWorldOffsetX -= stepX;
            if (wmWorldOffsetX < 0) {
                wmWorldOffsetX = 0;
            }
        } else {
            if (success != nullptr) {
                *success = false;
            }
        }
    } else if (dx > 0) {
        if (wmWorldOffsetX < wmGenData.viewportMaxX) {
            wmWorldOffsetX += stepX;
            if (wmWorldOffsetX > wmGenData.viewportMaxX) {
                wmWorldOffsetX = wmGenData.viewportMaxX;
            }
        } else {
            if (success != nullptr) {
                *success = false;
            }
        }
    }

    if (shouldRefresh) {
        if (wmInterfaceRefresh() == -1) {
            return -1;
        }
    }

    return 0;
}

// 0x4C32EC
static void wmMouseBkProc()
{
    // 0x51DEB0
    static unsigned int lastTime = 0;

    // 0x51DEB4
    static bool couldScroll = true;

    int x;
    int y;
    mouseGetPosition(&x, &y);

    int dx = 0;
    if (x == screenGetWidth() - 1) {
        dx = 1;
    } else if (x == 0) {
        dx = -1;
    }

    int dy = 0;
    if (y == screenGetHeight() - 1) {
        dy = 1;
    } else if (y == 0) {
        dy = -1;
    }

    int oldMouseCursor = gameMouseGetCursor();
    int newMouseCursor = oldMouseCursor;

    if (dx != 0 || dy != 0) {
        if (dx > 0) {
            if (dy > 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_SE;
            } else if (dy < 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_NE;
            } else {
                newMouseCursor = MOUSE_CURSOR_SCROLL_E;
            }
        } else if (dx < 0) {
            if (dy > 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_SW;
            } else if (dy < 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_NW;
            } else {
                newMouseCursor = MOUSE_CURSOR_SCROLL_W;
            }
        } else {
            if (dy < 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_N;
            } else if (dy > 0) {
                newMouseCursor = MOUSE_CURSOR_SCROLL_S;
            }
        }

        unsigned int tick = _get_bk_time();
        if (getTicksBetween(tick, lastTime) > 50) {
            lastTime = _get_bk_time();
            // NOTE: Uninline.
            wmInterfaceScroll(dx, dy, &couldScroll);
        }

        if (!couldScroll) {
            newMouseCursor += 8;
        }
    } else {
        if (oldMouseCursor != MOUSE_CURSOR_ARROW) {
            newMouseCursor = MOUSE_CURSOR_ARROW;
        }
    }

    if (oldMouseCursor != newMouseCursor) {
        gameMouseSetCursor(newMouseCursor);
    }
}

// Load tile art if needed.
//
// 0x4C37EC
static int wmTileGrabArt(int tileIdx)
{
    TileInfo* tile = &(wmTileInfoList[tileIdx]);
    if (tile->data != nullptr) {
        return 0;
    }

    tile->data = artLockFrameData(tile->fid, 0, 0, &(tile->handle));
    if (tile->data != nullptr) {
        return 0;
    }

    wmInterfaceExit();

    return -1;
}

// 0x4C3830
static int wmInterfaceRefresh()
{
    if (wmInterfaceWasInitialized != 1) {
        return 0;
    }

    int v17 = wmWorldOffsetX % WM_TILE_WIDTH;
    int v18 = wmWorldOffsetY % WM_TILE_HEIGHT;
    int v20 = WM_TILE_HEIGHT - v18;
    int v21 = WM_TILE_WIDTH * v18;
    int v19 = WM_TILE_WIDTH - v17;

    // Render tiles.
    int y = 0;
    int x = 0;
    int v0 = wmWorldOffsetY / WM_TILE_HEIGHT * wmNumHorizontalTiles + wmWorldOffsetX / WM_TILE_WIDTH % wmNumHorizontalTiles;
    while (y < WM_VIEW_HEIGHT) {
        x = 0;
        int v23 = 0;
        int height;
        while (x < WM_VIEW_WIDTH) {
            if (wmTileGrabArt(v0) == -1) {
                return -1;
            }

            int width = WM_TILE_WIDTH;

            int srcX = 0;
            if (x == 0) {
                srcX = v17;
                width = v19;
            }

            if (width + x > WM_VIEW_WIDTH) {
                width = WM_VIEW_WIDTH - x;
            }

            height = WM_TILE_HEIGHT;
            if (y == 0) {
                height = v20;
                srcX += v21;
            }

            if (height + y > WM_VIEW_HEIGHT) {
                height = WM_VIEW_HEIGHT - y;
            }

            TileInfo* tileInfo = &(wmTileInfoList[v0]);
            blitBufferToBuffer(tileInfo->data + srcX,
                width,
                height,
                WM_TILE_WIDTH,
                wmBkWinBuf + WM_WINDOW_WIDTH * (y + WM_VIEW_Y) + WM_VIEW_X + x,
                WM_WINDOW_WIDTH);
            v0++;

            x += width;
            v23++;
        }

        v0 += wmNumHorizontalTiles - v23;
        y += height;
    }

    // Render cities.
    for (int index = 0; index < wmMaxAreaNum; index++) {
        CityInfo* cityInfo = &(wmAreaInfoList[index]);
        if (cityInfo->state != CITY_STATE_UNKNOWN) {
            CitySizeDescription* citySizeDescription = &(wmSphereData[cityInfo->size]);
            int cityX = cityInfo->x - wmWorldOffsetX;
            int cityY = cityInfo->y - wmWorldOffsetY;
            if (cityX >= 0 && cityX <= 472 - citySizeDescription->frmImage.getWidth()
                && cityY >= 0 && cityY <= 465 - citySizeDescription->frmImage.getHeight()) {
                wmInterfaceDrawCircleOverlay(cityInfo, citySizeDescription, wmBkWinBuf, cityX, cityY);
            }
        }
    }

    // Hide unknown subtiles, dim unvisited.
    int v25 = wmWorldOffsetX / WM_TILE_WIDTH % wmNumHorizontalTiles + wmWorldOffsetY / WM_TILE_HEIGHT * wmNumHorizontalTiles;
    int v30 = 0;
    while (v30 < WM_VIEW_HEIGHT) {
        int v24 = 0;
        int v33 = 0;
        int v29;
        while (v33 < WM_VIEW_WIDTH) {
            int v31 = WM_TILE_WIDTH;
            if (v33 == 0) {
                v31 = WM_TILE_WIDTH - v17;
            }

            if (v33 + v31 > WM_VIEW_WIDTH) {
                v31 = WM_VIEW_WIDTH - v33;
            }

            v29 = WM_TILE_HEIGHT;
            if (v30 == 0) {
                v29 -= v18;
            }

            if (v30 + v29 > WM_VIEW_HEIGHT) {
                v29 = WM_VIEW_HEIGHT - v30;
            }

            int v32;
            if (v30 != 0) {
                v32 = WM_VIEW_Y;
            } else {
                v32 = WM_VIEW_Y - v18;
            }

            int v13 = 0;
            int v34 = v30 + v32;

            for (int row = 0; row < SUBTILE_GRID_HEIGHT; row++) {
                int v35;
                if (v33 != 0) {
                    v35 = WM_VIEW_X;
                } else {
                    v35 = WM_VIEW_X - v17;
                }

                int v15 = v33 + v35;
                for (int column = 0; column < SUBTILE_GRID_WIDTH; column++) {
                    TileInfo* tileInfo = &(wmTileInfoList[v25]);
                    wmInterfaceDrawSubTileList(tileInfo, column, row, v15, v34, 1);

                    v15 += WM_SUBTILE_SIZE;
                    v35 += WM_SUBTILE_SIZE;
                }

                v32 += WM_SUBTILE_SIZE;
                v34 += WM_SUBTILE_SIZE;
            }

            v25++;
            v24++;
            v33 += v31;
        }

        v25 += wmNumHorizontalTiles - v24;
        v30 += v29;
    }

    wmDrawCursorStopped();

    wmRefreshInterfaceOverlay(true);

    return 0;
}

// 0x4C3C9C
void wmInterfaceRefreshDate(bool shouldRefreshWindow)
{
    // CE: Headless guard — the core travel tick (worldmapTravelClockTick,
    // ledger H-13) advances the game clock through wmGameTimeIncrement, which
    // refreshes the date display. Without the worldmap window there is
    // nothing to draw into. Inert in UI runs: the window buffer always exists
    // while the worldmap loop is running.
    if (wmBkWinBuf == nullptr) {
        return;
    }

    int month;
    int day;
    int year;
    gameTimeGetDate(&month, &day, &year);

    month--;

    unsigned char* dest = wmBkWinBuf;

    int numbersFrmWidth = wmGenData.numbersFrmImage.getWidth();
    int numbersFrmHeight = wmGenData.numbersFrmImage.getHeight();
    unsigned char* numbersFrmData = wmGenData.numbersFrmImage.getData();

    dest += WM_WINDOW_WIDTH * 12 + 487;
    blitBufferToBuffer(numbersFrmData + 9 * (day / 10), 9, numbersFrmHeight, numbersFrmWidth, dest, WM_WINDOW_WIDTH);
    blitBufferToBuffer(numbersFrmData + 9 * (day % 10), 9, numbersFrmHeight, numbersFrmWidth, dest + 9, WM_WINDOW_WIDTH);

    int monthsFrmWidth = wmGenData.monthsFrmImage.getWidth();
    unsigned char* monthsFrmData = wmGenData.monthsFrmImage.getData();
    blitBufferToBuffer(monthsFrmData + monthsFrmWidth * 15 * month, 29, 14, 29, dest + WM_WINDOW_WIDTH + 26, WM_WINDOW_WIDTH);

    dest += 98;
    for (int index = 0; index < 4; index++) {
        dest -= 9;
        blitBufferToBuffer(numbersFrmData + 9 * (year % 10), 9, numbersFrmHeight, numbersFrmWidth, dest, WM_WINDOW_WIDTH);
        year /= 10;
    }

    int gameTimeHour = gameTimeGetHour();
    dest += 72;
    for (int index = 0; index < 4; index++) {
        blitBufferToBuffer(numbersFrmData + 9 * (gameTimeHour % 10), 9, numbersFrmHeight, numbersFrmWidth, dest, WM_WINDOW_WIDTH);
        dest -= 9;
        gameTimeHour /= 10;
    }

    if (shouldRefreshWindow) {
        Rect rect;
        rect.left = 487;
        rect.top = 12;
        rect.bottom = numbersFrmHeight + 12;
        rect.right = 630;
        windowRefreshRect(wmBkWin, &rect);
    }
}

// 0x4C3FA8
static int wmInterfaceDrawCircleOverlay(CityInfo* city, CitySizeDescription* citySizeDescription, unsigned char* dest, int x, int y)
{
    _dark_translucent_trans_buf_to_buf(citySizeDescription->frmImage.getData(),
        citySizeDescription->frmImage.getWidth(),
        citySizeDescription->frmImage.getHeight(),
        citySizeDescription->frmImage.getWidth(),
        dest,
        x,
        y,
        WM_WINDOW_WIDTH,
        0x10000,
        circleBlendTable,
        _commonGrayTable);

    // CE: Slightly increase whitespace between cirle and city name.
    int nameY = y + citySizeDescription->frmImage.getHeight() + 3;
    int maxY = 464 - fontGetLineHeight();
    if (nameY < maxY) {
        MessageListItem messageListItem;
        char name[40];
        if (wmAreaIsKnown(city->areaId)) {
            // NOTE: Uninline.
            wmGetAreaName(city, name);
        } else {
            strncpy(name, getmsg(&wmMsgFile, &messageListItem, 1004), 40);
        }

        int width = fontGetStringWidth(name);
        fontDrawText(dest + WM_WINDOW_WIDTH * nameY + x + citySizeDescription->frmImage.getWidth() / 2 - width / 2,
            name,
            width,
            WM_WINDOW_WIDTH,
            _colorTable[992] | FONT_SHADOW);
    }

    return 0;
}

// Helper function that dims specified rectangle in given buffer. It's used to
// slightly darken subtile which is known, but not visited.
//
// 0x4C40A8
static void wmInterfaceDrawSubTileRectFogged(unsigned char* dest, int width, int height, int pitch)
{
    int skipY = pitch - width;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char color = *dest;
            *dest++ = intensityColorTable[color][75];
        }
        dest += skipY;
    }
}

// 0x4C40E4
static int wmInterfaceDrawSubTileList(TileInfo* tileInfo, int column, int row, int x, int y, int a6)
{
    SubtileInfo* subtileInfo = &(tileInfo->subtiles[row][column]);

    int destY = y;
    int destX = x;

    int height = WM_SUBTILE_SIZE;
    if (y < WM_VIEW_Y) {
        if (y < 0) {
            height = y + 29;
        } else {
            height = WM_SUBTILE_SIZE - (WM_VIEW_Y - y);
        }
        destY = WM_VIEW_Y;
    }

    if (height + y > 464) {
        height -= height + y - 464;
    }

    int width = WM_SUBTILE_SIZE * a6;
    if (x < WM_VIEW_X) {
        destX = WM_VIEW_X;
        width -= WM_VIEW_X - x;
    }

    if (width + x > 472) {
        width -= width + x - 472;
    }

    if (width > 0 && height > 0) {
        unsigned char* dest = wmBkWinBuf + WM_WINDOW_WIDTH * destY + destX;
        switch (subtileInfo->state) {
        case SUBTILE_STATE_UNKNOWN:
            bufferFill(dest, width, height, WM_WINDOW_WIDTH, _colorTable[0]);
            break;
        case SUBTILE_STATE_KNOWN:
            wmInterfaceDrawSubTileRectFogged(dest, width, height, WM_WINDOW_WIDTH);
            break;
        }
    }

    return 0;
}

// 0x4C41EC
static int wmDrawCursorStopped()
{
    unsigned char* src;
    int width;
    int height;

    if (wmGenData.walkDestinationX >= 1 || wmGenData.walkDestinationY >= 1) {

        if (wmGenData.encounterIconIsVisible) {
            src = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getData();
            width = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getWidth();
            height = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getHeight();
        } else {
            src = wmGenData.locationMarkerFrmImage.getData();
            width = wmGenData.locationMarkerFrmImage.getWidth();
            height = wmGenData.locationMarkerFrmImage.getHeight();
        }

        if (wmGenData.worldPosX >= wmWorldOffsetX && wmGenData.worldPosX < wmWorldOffsetX + WM_VIEW_WIDTH
            && wmGenData.worldPosY >= wmWorldOffsetY && wmGenData.worldPosY < wmWorldOffsetY + WM_VIEW_HEIGHT) {
            blitBufferToBufferTrans(src, width, height, width, wmBkWinBuf + WM_WINDOW_WIDTH * (WM_VIEW_Y - wmWorldOffsetY + wmGenData.worldPosY - height / 2) + WM_VIEW_X - wmWorldOffsetX + wmGenData.worldPosX - width / 2, WM_WINDOW_WIDTH);
        }

        if (wmGenData.walkDestinationX >= wmWorldOffsetX && wmGenData.walkDestinationX < wmWorldOffsetX + WM_VIEW_WIDTH
            && wmGenData.walkDestinationY >= wmWorldOffsetY && wmGenData.walkDestinationY < wmWorldOffsetY + WM_VIEW_HEIGHT) {
            blitBufferToBufferTrans(wmGenData.destinationMarkerFrmImage.getData(),
                wmGenData.destinationMarkerFrmImage.getWidth(),
                wmGenData.destinationMarkerFrmImage.getHeight(),
                wmGenData.destinationMarkerFrmImage.getWidth(),
                wmBkWinBuf + WM_WINDOW_WIDTH * (WM_VIEW_Y - wmWorldOffsetY + wmGenData.walkDestinationY - wmGenData.destinationMarkerFrmImage.getHeight() / 2) + WM_VIEW_X - wmWorldOffsetX + wmGenData.walkDestinationX - wmGenData.destinationMarkerFrmImage.getWidth() / 2,
                WM_WINDOW_WIDTH);
        }
    } else {
        if (wmGenData.encounterIconIsVisible) {
            src = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getData();
            width = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getWidth();
            height = wmGenData.encounterCursorFrmImages[wmGenData.encounterCursorId].getHeight();
        } else {
            src = wmGenData.mousePressed ? wmGenData.hotspotPressedFrmImage.getData() : wmGenData.hotspotNormalFrmImage.getData();
            width = wmGenData.hotspotNormalFrmImage.getWidth();
            height = wmGenData.hotspotNormalFrmImage.getHeight();
        }

        if (wmGenData.worldPosX >= wmWorldOffsetX && wmGenData.worldPosX < wmWorldOffsetX + WM_VIEW_WIDTH
            && wmGenData.worldPosY >= wmWorldOffsetY && wmGenData.worldPosY < wmWorldOffsetY + WM_VIEW_HEIGHT) {
            blitBufferToBufferTrans(src, width, height, width, wmBkWinBuf + WM_WINDOW_WIDTH * (WM_VIEW_Y - wmWorldOffsetY + wmGenData.worldPosY - height / 2) + WM_VIEW_X - wmWorldOffsetX + wmGenData.worldPosX - width / 2, WM_WINDOW_WIDTH);
        }
    }

    return 0;
}

// 0x4C4490
bool wmCursorIsVisible()
{
    return wmGenData.worldPosX >= wmWorldOffsetX
        && wmGenData.worldPosY >= wmWorldOffsetY
        && wmGenData.worldPosX < wmWorldOffsetX + WM_VIEW_WIDTH
        && wmGenData.worldPosY < wmWorldOffsetY + WM_VIEW_HEIGHT;
}

// 0x4C4850
void wmTownMap()
{
    wmWorldMapFunc(1);
}

// 0x4C485C
static int wmTownMapFunc(int* mapIdxPtr)
{
    *mapIdxPtr = -1;

    if (wmTownMapInit() == -1) {
        wmTownMapExit();
        return -1;
    }

    if (wmGenData.currentAreaId == -1) {
        return -1;
    }

    CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);

    for (;;) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
        }

        if (_game_user_wants_to_quit) {
            break;
        }

        if (keyCode != -1) {
            if (keyCode == KEY_ESCAPE) {
                break;
            }

            if (keyCode >= KEY_1 && keyCode < KEY_1 + city->entrancesLength) {
                EntranceInfo* entrance = &(city->entrances[keyCode - KEY_1]);

                // SFALL: Prevent using number keys to enter unvisited areas on
                // a town map.
                if (gTownMapHotkeysFix) {
                    if (entrance->state == 0 || entrance->x == -1 || entrance->y == -1) {
                        continue;
                    }
                }

                *mapIdxPtr = entrance->map;

                mapSetEnteringLocation(entrance->elevation, entrance->tile, entrance->rotation);

                break;
            }

            if (keyCode >= KEY_CTRL_F1 && keyCode <= KEY_CTRL_F7) {
                int quickDestinationIndex = wmGenData.tabsOffsetY / 27 + keyCode - KEY_CTRL_F1;
                if (quickDestinationIndex < wmLabelCount) {
                    int areaIdx = wmLabelList[quickDestinationIndex];
                    CityInfo* city = &(wmAreaInfoList[areaIdx]);
                    if (!wmAreaIsKnown(city->areaId)) {
                        break;
                    }

                    if (areaIdx != wmGenData.currentAreaId) {
                        // CE: Fix incorrect destination positioning. See
                        // `wmWorldMapFunc` for explanation.
                        CitySizeDescription* citySizeDescription = &(wmSphereData[city->size]);
                        int destX = city->x + citySizeDescription->frmImage.getWidth() / 2 - WM_VIEW_X;
                        int destY = city->y + citySizeDescription->frmImage.getHeight() / 2 - WM_VIEW_Y;
                        wmPartyInitWalking(destX, destY);

                        wmGenData.mousePressed = false;

                        break;
                    }
                }
            } else {
                if (keyCode == KEY_CTRL_ARROW_UP) {
                    wmInterfaceScrollTabsStart(-27);
                } else if (keyCode == KEY_CTRL_ARROW_DOWN) {
                    wmInterfaceScrollTabsStart(27);
                } else if (keyCode == 2069) {
                    if (wmTownMapRefresh() == -1) {
                        return -1;
                    }
                }

                if (keyCode == KEY_UPPERCASE_T || keyCode == KEY_LOWERCASE_T || keyCode == KEY_UPPERCASE_W || keyCode == KEY_LOWERCASE_W) {
                    keyCode = KEY_ESCAPE;
                }

                if (keyCode == KEY_ESCAPE) {
                    break;
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (wmTownMapExit() == -1) {
        return -1;
    }

    return 0;
}

// 0x4C4A6C
static int wmTownMapInit()
{
    wmTownMapCurArea = wmGenData.currentAreaId;

    CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);

    if (!_townFrmImage.lock(city->mapFid)) {
        return -1;
    }

    for (int index = 0; index < city->entrancesLength; index++) {
        wmTownMapButtonId[index] = -1;
    }

    for (int index = 0; index < city->entrancesLength; index++) {
        EntranceInfo* entrance = &(city->entrances[index]);
        if (entrance->state == 0) {
            continue;
        }

        if (entrance->x == -1 || entrance->y == -1) {
            continue;
        }

        wmTownMapButtonId[index] = buttonCreate(wmBkWin,
            entrance->x,
            entrance->y,
            wmGenData.hotspotNormalFrmImage.getWidth(),
            wmGenData.hotspotNormalFrmImage.getHeight(),
            -1,
            2069,
            -1,
            KEY_1 + index,
            wmGenData.hotspotNormalFrmImage.getData(),
            wmGenData.hotspotPressedFrmImage.getData(),
            nullptr,
            BUTTON_FLAG_TRANSPARENT);

        if (wmTownMapButtonId[index] == -1) {
            return -1;
        }
    }

    tickersRemove(wmMouseBkProc);

    if (wmTownMapRefresh() == -1) {
        return -1;
    }

    return 0;
}

// 0x4C4BD0
static int wmTownMapRefresh()
{
    blitBufferToBuffer(_townFrmImage.getData(),
        _townFrmImage.getWidth(),
        _townFrmImage.getHeight(),
        _townFrmImage.getWidth(),
        wmBkWinBuf + WM_WINDOW_WIDTH * WM_VIEW_Y + WM_VIEW_X,
        WM_WINDOW_WIDTH);

    wmRefreshInterfaceOverlay(false);

    CityInfo* city = &(wmAreaInfoList[wmGenData.currentAreaId]);

    for (int index = 0; index < city->entrancesLength; index++) {
        EntranceInfo* entrance = &(city->entrances[index]);
        if (entrance->state == 0) {
            continue;
        }

        if (entrance->x == -1 || entrance->y == -1) {
            continue;
        }

        MessageListItem messageListItem;
        messageListItem.num = 200 + 10 * wmTownMapCurArea + index;
        if (messageListGetItem(&wmMsgFile, &messageListItem)) {
            if (messageListItem.text != nullptr) {
                int width = fontGetStringWidth(messageListItem.text);
                // CE: Slightly increase whitespace between marker and entrance name.
                windowDrawText(wmBkWin,
                    messageListItem.text,
                    width,
                    wmGenData.hotspotNormalFrmImage.getWidth() / 2 + entrance->x - width / 2,
                    wmGenData.hotspotNormalFrmImage.getHeight() + entrance->y + 4,
                    _colorTable[992] | 0x2000000 | FONT_SHADOW);
            }
        }
    }

    windowRefresh(wmBkWin);

    return 0;
}

// 0x4C4D00
static int wmTownMapExit()
{
    _townFrmImage.unlock();

    if (wmTownMapCurArea != -1) {
        CityInfo* city = &(wmAreaInfoList[wmTownMapCurArea]);
        for (int index = 0; index < city->entrancesLength; index++) {
            if (wmTownMapButtonId[index] != -1) {
                buttonDestroy(wmTownMapButtonId[index]);
                wmTownMapButtonId[index] = -1;
            }
        }
    }

    if (wmInterfaceRefresh() == -1) {
        return -1;
    }

    tickersAdd(wmMouseBkProc);

    return 0;
}

// 0x4C50F4
static int wmRefreshInterfaceOverlay(bool shouldRefreshWindow)
{
    blitBufferToBufferTrans(_backgroundFrmImage.getData(),
        _backgroundFrmImage.getWidth(),
        _backgroundFrmImage.getHeight(),
        _backgroundFrmImage.getWidth(),
        wmBkWinBuf,
        WM_WINDOW_WIDTH);

    wmRefreshTabs();

    // NOTE: Uninline.
    wmInterfaceDialSyncTime(false);

    wmRefreshInterfaceDial(false);

    if (wmGenData.isInCar) {
        unsigned char* data = artGetFrameData(wmGenData.carImageFrm, wmGenData.carImageCurrentFrameIndex, 0);
        if (data == nullptr) {
            return -1;
        }

        blitBufferToBuffer(data,
            wmGenData.carImageFrmWidth,
            wmGenData.carImageFrmHeight,
            wmGenData.carImageFrmWidth,
            wmBkWinBuf + WM_WINDOW_WIDTH * WM_WINDOW_CAR_Y + WM_WINDOW_CAR_X,
            WM_WINDOW_WIDTH);

        blitBufferToBufferTrans(wmGenData.carOverlayFrmImage.getData(),
            wmGenData.carOverlayFrmImage.getWidth(),
            wmGenData.carOverlayFrmImage.getHeight(),
            wmGenData.carOverlayFrmImage.getWidth(),
            wmBkWinBuf + WM_WINDOW_WIDTH * WM_WINDOW_CAR_OVERLAY_Y + WM_WINDOW_CAR_OVERLAY_X,
            WM_WINDOW_WIDTH);

        wmInterfaceRefreshCarFuel();
    } else {
        blitBufferToBufferTrans(wmGenData.globeOverlayFrmImage.getData(),
            wmGenData.globeOverlayFrmImage.getWidth(),
            wmGenData.globeOverlayFrmImage.getHeight(),
            wmGenData.globeOverlayFrmImage.getWidth(),
            wmBkWinBuf + WM_WINDOW_WIDTH * WM_WINDOW_GLOBE_OVERLAY_Y + WM_WINDOW_GLOBE_OVERLAY_X,
            WM_WINDOW_WIDTH);
    }

    wmInterfaceRefreshDate(false);

    if (shouldRefreshWindow) {
        windowRefresh(wmBkWin);
    }

    return 0;
}

// 0x4C5244
static void wmInterfaceRefreshCarFuel()
{
    int ratio = (WM_WINDOW_CAR_FUEL_BAR_HEIGHT * wmGenData.carFuel) / CAR_FUEL_MAX;
    if ((ratio & 1) != 0) {
        ratio -= 1;
    }

    unsigned char* dest = wmBkWinBuf + WM_WINDOW_WIDTH * WM_WINDOW_CAR_FUEL_BAR_Y + WM_WINDOW_CAR_FUEL_BAR_X;

    for (int index = WM_WINDOW_CAR_FUEL_BAR_HEIGHT; index > ratio; index--) {
        *dest = 14;
        dest += 640;
    }

    while (ratio > 0) {
        *dest = 196;
        dest += WM_WINDOW_WIDTH;

        *dest = 14;
        dest += WM_WINDOW_WIDTH;

        ratio -= 2;
    }
}

// 0x4C52B0
static int wmRefreshTabs()
{
    unsigned char* v30;
    unsigned char* v0;
    int v31;
    CityInfo* city;
    int v10;
    unsigned char* v11;
    unsigned char* v12;
    int v32;
    unsigned char* v13;
    FrmImage labelFrm;

    // CE: Skip first empty tab (original code does this in the
    // `wmInterfaceInit`).
    unsigned char* src = wmGenData.tabsBackgroundFrmImage.getData() + wmGenData.tabsBackgroundFrmImage.getWidth() * 27;
    blitBufferToBufferTrans(src + wmGenData.tabsBackgroundFrmImage.getWidth() * wmGenData.tabsOffsetY + 9,
        119,
        178,
        wmGenData.tabsBackgroundFrmImage.getWidth(),
        wmBkWinBuf + WM_WINDOW_WIDTH * 135 + 501,
        WM_WINDOW_WIDTH);

    v30 = wmBkWinBuf + WM_WINDOW_WIDTH * 138 + 530;
    v0 = wmBkWinBuf + WM_WINDOW_WIDTH * 138 + 530 - WM_WINDOW_WIDTH * (wmGenData.tabsOffsetY % 27);
    v31 = wmGenData.tabsOffsetY / 27;

    if (v31 < wmLabelCount) {
        city = &(wmAreaInfoList[wmLabelList[v31]]);
        if (city->labelFid != -1) {
            if (!labelFrm.lock(city->labelFid)) {
                return -1;
            }

            v10 = labelFrm.getHeight() - wmGenData.tabsOffsetY % 27;
            v11 = labelFrm.getData() + labelFrm.getWidth() * (wmGenData.tabsOffsetY % 27);

            v12 = v0;
            if (v0 < v30 - WM_WINDOW_WIDTH) {
                v12 = v30 - WM_WINDOW_WIDTH;
            }

            blitBufferToBuffer(v11,
                labelFrm.getWidth(),
                v10,
                labelFrm.getWidth(),
                v12,
                WM_WINDOW_WIDTH);

            labelFrm.unlock();
        }
    }

    v13 = v0 + WM_WINDOW_WIDTH * 27;
    v32 = v31 + 6;

    for (int v14 = v31 + 1; v14 < v32; v14++) {
        if (v14 < wmLabelCount) {
            city = &(wmAreaInfoList[wmLabelList[v14]]);
            if (city->labelFid != -1) {
                if (!labelFrm.lock(city->labelFid)) {
                    return -1;
                }

                blitBufferToBuffer(labelFrm.getData(),
                    labelFrm.getWidth(),
                    labelFrm.getHeight(),
                    labelFrm.getWidth(),
                    v13,
                    WM_WINDOW_WIDTH);

                labelFrm.unlock();
            }
        }
        v13 += WM_WINDOW_WIDTH * 27;
    }

    if (v31 + 6 < wmLabelCount) {
        city = &(wmAreaInfoList[wmLabelList[v31 + 6]]);
        if (city->labelFid != -1) {
            if (!labelFrm.lock(city->labelFid)) {
                return -1;
            }

            blitBufferToBuffer(labelFrm.getData(),
                labelFrm.getWidth(),
                labelFrm.getHeight() - 5,
                labelFrm.getWidth(),
                v13,
                WM_WINDOW_WIDTH);

            labelFrm.unlock();
        }
    }

    blitBufferToBufferTrans(wmGenData.tabsBorderFrmImage.getData(),
        119,
        178,
        119,
        wmBkWinBuf + WM_WINDOW_WIDTH * 135 + 501,
        WM_WINDOW_WIDTH);

    return 0;
}

// Creates array of cities available as quick destinations.
//
// 0x4C55D4
static int wmMakeTabsLabelList(int** quickDestinationsPtr, int* quickDestinationsLengthPtr)
{
    int* quickDestinations = *quickDestinationsPtr;

    // NOTE: Uninline.
    wmFreeTabsLabelList(quickDestinationsPtr, quickDestinationsLengthPtr);

    int capacity = 10;

    quickDestinations = (int*)internal_malloc(sizeof(*quickDestinations) * capacity);
    *quickDestinationsPtr = quickDestinations;

    if (quickDestinations == nullptr) {
        return -1;
    }

    int quickDestinationsLength = *quickDestinationsLengthPtr;
    for (int index = 0; index < wmMaxAreaNum; index++) {
        if (wmAreaIsKnown(index) && wmAreaInfoList[index].labelFid != -1) {
            quickDestinationsLength++;
            *quickDestinationsLengthPtr = quickDestinationsLength;

            if (capacity <= quickDestinationsLength) {
                capacity += 10;

                quickDestinations = (int*)internal_realloc(quickDestinations, sizeof(*quickDestinations) * capacity);
                if (quickDestinations == nullptr) {
                    return -1;
                }

                *quickDestinationsPtr = quickDestinations;
            }

            quickDestinations[quickDestinationsLength - 1] = index;
        }
    }

    qsort(quickDestinations, quickDestinationsLength, sizeof(*quickDestinations), wmTabsCompareNames);

    return 0;
}

// 0x4C56C8
static int wmTabsCompareNames(const void* a1, const void* a2)
{
    int index1 = *(int*)a1;
    int index2 = *(int*)a2;

    CityInfo* city1 = &(wmAreaInfoList[index1]);
    CityInfo* city2 = &(wmAreaInfoList[index2]);

    return compat_stricmp(city1->name, city2->name);
}

// NOTE: Inlined.
//
// 0x4C5710
static int wmFreeTabsLabelList(int** quickDestinationsListPtr, int* quickDestinationsLengthPtr)
{
    if (*quickDestinationsListPtr != nullptr) {
        internal_free(*quickDestinationsListPtr);
        *quickDestinationsListPtr = nullptr;
    }

    *quickDestinationsLengthPtr = 0;

    return 0;
}

// 0x4C5734
static void wmRefreshInterfaceDial(bool shouldRefreshWindow)
{
    unsigned char* data = artGetFrameData(wmGenData.dialFrm, wmGenData.dialFrmCurrentFrameIndex, 0);
    blitBufferToBufferTrans(data,
        wmGenData.dialFrmWidth,
        wmGenData.dialFrmHeight,
        wmGenData.dialFrmWidth,
        wmBkWinBuf + WM_WINDOW_WIDTH * WM_WINDOW_DIAL_Y + WM_WINDOW_DIAL_X,
        WM_WINDOW_WIDTH);

    if (shouldRefreshWindow) {
        Rect rect;
        rect.left = WM_WINDOW_DIAL_X;
        rect.top = WM_WINDOW_DIAL_Y - 1;
        rect.right = rect.left + wmGenData.dialFrmWidth;
        rect.bottom = rect.top + wmGenData.dialFrmHeight;
        windowRefreshRect(wmBkWin, &rect);
    }
}

// NOTE: Inlined.
//
// 0x4C57BC
void wmInterfaceDialSyncTime(bool shouldRefreshWindow)
{
    // CE: Headless guard — see wmInterfaceRefreshDate. Without it this relies
    // on artGetFrameCount(nullptr) returning -1 to no-op.
    if (wmBkWinBuf == nullptr) {
        return;
    }

    int gameHour;
    int frame;

    gameHour = gameTimeGetHour();
    frame = (gameHour / 100 + 12) % artGetFrameCount(wmGenData.dialFrm);
    if (frame != wmGenData.dialFrmCurrentFrameIndex) {
        wmGenData.dialFrmCurrentFrameIndex = frame;
        wmRefreshInterfaceDial(shouldRefreshWindow);
    }
}

void wmFadeOut()
{
    if (!wmFaded) {
        paletteFadeTo(gPaletteBlack);
        wmFaded = true;
    }
}

void wmFadeIn()
{
    if (wmFaded) {
        paletteFadeTo(_cmap);
        wmFaded = false;
    }
}

void wmFadeReset()
{
    wmFaded = false;
    paletteSetEntries(_cmap);
}

void wmBlinkRndEncounterIcon(bool special)
{
    wmGenData.encounterIconIsVisible = true;

    // CE: Original code cycles circled bright and non-circled dark icons.
    int dark;
    int bright;
    if (special) {
        dark = WORLD_MAP_ENCOUNTER_FRM_SPECIAL_DARK;
        bright = WORLD_MAP_ENCOUNTER_FRM_SPECIAL_BRIGHT;
    } else {
        dark = WORLD_MAP_ENCOUNTER_FRM_RANDOM_DARK;
        bright = WORLD_MAP_ENCOUNTER_FRM_RANDOM_BRIGHT;
    }

    for (int index = 0; index < 7; index++) {
        wmGenData.encounterCursorId = index % 2 == 0 ? dark : bright;

        if (wmInterfaceRefresh() == -1) {
            return;
        }

        renderPresent();
        inputBlockForTocks(200);
    }

    wmGenData.encounterIconIsVisible = false;
}

} // namespace fallout
