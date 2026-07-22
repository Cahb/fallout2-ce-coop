#include "map_render.h"

#include <string.h>

#include "art.h"
#include "cycle.h"
#include "debug.h"
#include "draw.h"
#include "elevator.h"
#include "game_mouse.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "map.h"
#include "object.h"
#include "object_render.h"
#include "svga.h"
#include "tile.h"
#include "window_manager.h"

// Client-side render layer extracted from map.cc (REWRITE_PLAN, TU split).
// Pure presentation: creates/owns the iso window, blits and scrolls its buffer,
// and refreshes dirty rects by re-rendering floors/roofs/objects. Core map.cc
// retains ownership of the sim state; it assigns the two refresh procs into its
// _map_scroll_refresh pointer by name and calls square_init/mapMakeMapsDirectory
// (which stay in map.cc). This move is mechanical and replay-identical.

namespace fallout {

static void isoWindowRefreshRect(Rect* rect);

// 0x519550
static unsigned int gIsoWindowScrollTimestamp = 0;

// 0x631D38
static Rect gIsoWindowRect;

// 0x631D50
static unsigned char* gIsoWindowBuffer;

// 0x631E4C
int gIsoWindow;

// iso_init
// 0x481CA0
int isoInit()
{
    tileScrollLimitingDisable();
    tileScrollBlockingDisable();

    // NOTE: Uninline.
    square_init();

    gIsoWindow = windowCreate(0, 0, screenGetWidth(), screenGetVisibleHeight(), 256, 10);
    if (gIsoWindow == -1) {
        debugPrint("win_add failed in iso_init\n");
        return -1;
    }

    gIsoWindowBuffer = windowGetBuffer(gIsoWindow);
    if (gIsoWindowBuffer == nullptr) {
        debugPrint("win_get_buf failed in iso_init\n");
        return -1;
    }

    if (windowGetRect(gIsoWindow, &gIsoWindowRect) != 0) {
        debugPrint("win_get_rect failed in iso_init\n");
        return -1;
    }

    if (artInit() != 0) {
        debugPrint("art_init failed in iso_init\n");
        return -1;
    }

    debugPrint(">art_init\t\t");

    if (tileInit(_square, SQUARE_GRID_WIDTH, SQUARE_GRID_HEIGHT, HEX_GRID_WIDTH, HEX_GRID_HEIGHT, gIsoWindowBuffer, screenGetWidth(), screenGetVisibleHeight(), screenGetWidth(), isoWindowRefreshRect) != 0) {
        debugPrint("tile_init failed in iso_init\n");
        return -1;
    }

    debugPrint(">tile_init\t\t");

    if (objectsInit(gIsoWindowBuffer, screenGetWidth(), screenGetVisibleHeight(), screenGetWidth()) != 0) {
        debugPrint("obj_init failed in iso_init\n");
        return -1;
    }

    debugPrint(">obj_init\t\t");

    colorCycleInit();
    debugPrint(">cycle_init\t\t");

    tileScrollBlockingEnable();
    tileScrollLimitingEnable();

    if (interfaceInit() != 0) {
        debugPrint("intface_init failed in iso_init\n");
        return -1;
    }

    debugPrint(">intface_init\t\t");

    // SFALL
    elevatorsInit();

    mapMakeMapsDirectory();

    // NOTE: Uninline.
    mapSetEnteringLocation(-1, -1, -1);

    return 0;
}

// 0x481F48
void isoExit()
{
    interfaceFree();
    colorCycleFree();
    objectsExit();
    tileExit();
    artExit();

    windowDestroy(gIsoWindow);
}

// 0x4826C0
int mapScroll(int dx, int dy)
{
    if (getTicksSince(gIsoWindowScrollTimestamp) < 33) {
        return -2;
    }

    gIsoWindowScrollTimestamp = getTicks();

    int screenDx = dx * 32;
    int screenDy = dy * 24;

    if (screenDx == 0 && screenDy == 0) {
        return -1;
    }

    gameMouseObjectsHide();

    int centerScreenX;
    int centerScreenY;
    tileToScreenXY(gCenterTile, &centerScreenX, &centerScreenY, gElevation);
    centerScreenX += screenDx + 16;
    centerScreenY += screenDy + 8;

    int newCenterTile = tileFromScreenXY(centerScreenX, centerScreenY, gElevation);
    if (newCenterTile == -1) {
        return -1;
    }

    if (tileSetCenter(newCenterTile, 0) == -1) {
        return -1;
    }

    Rect r1;
    rectCopy(&r1, &gIsoWindowRect);

    Rect r2;
    rectCopy(&r2, &r1);

    int width = screenGetWidth();
    int pitch = width;
    int height = screenGetVisibleHeight();

    if (screenDx != 0) {
        width -= 32;
    }

    if (screenDy != 0) {
        height -= 24;
    }

    if (screenDx < 0) {
        r2.right = r2.left - screenDx;
    } else {
        r2.left = r2.right - screenDx;
    }

    unsigned char* src;
    unsigned char* dest;
    int step;
    if (screenDy < 0) {
        r1.bottom = r1.top - screenDy;
        src = gIsoWindowBuffer + pitch * (height - 1);
        dest = gIsoWindowBuffer + pitch * (screenGetVisibleHeight() - 1);
        if (screenDx < 0) {
            dest -= screenDx;
        } else {
            src += screenDx;
        }
        step = -pitch;
    } else {
        r1.top = r1.bottom - screenDy;
        dest = gIsoWindowBuffer;
        src = gIsoWindowBuffer + pitch * screenDy;

        if (screenDx < 0) {
            dest -= screenDx;
        } else {
            src += screenDx;
        }
        step = pitch;
    }

    for (int y = 0; y < height; y++) {
        memmove(dest, src, width);
        dest += step;
        src += step;
    }

    if (screenDx != 0) {
        _map_scroll_refresh(&r2);
    }

    if (screenDy != 0) {
        _map_scroll_refresh(&r1);
    }

    windowRefresh(gIsoWindow);

    return 0;
}

// 0x483ED0
static void isoWindowRefreshRect(Rect* rect)
{
    windowRefreshRect(gIsoWindow, rect);
}

// 0x483EE4
void isoWindowRefreshRectGame(Rect* rect)
{
    Rect rectToUpdate;
    if (rectIntersection(rect, &gIsoWindowRect, &rectToUpdate) == -1) {
        return;
    }

    // CE: Clear dirty rect to prevent most of the visual artifacts near map
    // edges.
    bufferFill(gIsoWindowBuffer + rectToUpdate.top * rectGetWidth(&gIsoWindowRect) + rectToUpdate.left,
        rectGetWidth(&rectToUpdate),
        rectGetHeight(&rectToUpdate),
        rectGetWidth(&gIsoWindowRect),
        0);

    tileRenderFloorsInRect(&rectToUpdate, gElevation);
    _obj_render_pre_roof(&rectToUpdate, gElevation);
    tileRenderRoofsInRect(&rectToUpdate, gElevation);
    _obj_render_post_roof(&rectToUpdate, gElevation);
}

// 0x483F44
void isoWindowRefreshRectMapper(Rect* rect)
{
    Rect rectToUpdate;
    if (rectIntersection(rect, &gIsoWindowRect, &rectToUpdate) == -1) {
        return;
    }

    bufferFill(gIsoWindowBuffer + rectToUpdate.top * rectGetWidth(&gIsoWindowRect) + rectToUpdate.left,
        rectGetWidth(&rectToUpdate),
        rectGetHeight(&rectToUpdate),
        rectGetWidth(&gIsoWindowRect),
        0);

    tileRenderFloorsInRect(&rectToUpdate, gElevation);
    _grid_render(&rectToUpdate, gElevation);
    _obj_render_pre_roof(&rectToUpdate, gElevation);
    tileRenderRoofsInRect(&rectToUpdate, gElevation);
    _obj_render_post_roof(&rectToUpdate, gElevation);
}

} // namespace fallout
