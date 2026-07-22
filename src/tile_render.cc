#include "tile_render.h"

#include <string.h>

#include <algorithm>

#include "art.h"
#include "color.h"
#include "draw.h"
#include "geometry.h"
#include "light.h"
#include "map.h"
#include "object.h"
#include "object_render.h"
#include "tile.h"

// Client-side render layer extracted from tile.cc (REWRITE_PLAN, TU split).
// Pure presentation: blits floors/roofs/grid into the iso window buffer owned
// by tile.cc (core sim). tile.cc retains ownership of the shared state read
// here and wires the two elevation-refresh procs by name via its refresh proc
// pointer. This move is mechanical and replay-identical.

namespace fallout {

typedef struct RightsideUpTableEntry {
    int field_0;
    int field_4;
} RightsideUpTableEntry;

typedef struct UpsideDownTableEntry {
    int field_0;
    int field_4;
} UpsideDownTableEntry;

typedef struct STRUCT_51DA6C {
    int field_0;
    int offsets[2];
    int intensity;
} STRUCT_51DA6C;

typedef struct RightsideUpTriangle {
    int field_0;
    int field_4;
    int field_8;
} RightsideUpTriangle;

typedef struct UpsideDownTriangle {
    int field_0;
    int field_4;
    int field_8;
} UpsideDownTriangle;

static void tileRenderRoof(int fid, int x, int y, Rect* rect, int light);
static void _draw_grid(int tile, int elevation, Rect* rect);
static void tileRenderFloor(int fid, int x, int y, Rect* rect);

// 0x51D99C
static RightsideUpTableEntry _rightside_up_table[13] = {
    { -1, 2 },
    { 78, 2 },
    { 76, 6 },
    { 73, 8 },
    { 71, 10 },
    { 68, 14 },
    { 65, 16 },
    { 63, 18 },
    { 61, 20 },
    { 58, 24 },
    { 55, 26 },
    { 53, 28 },
    { 50, 32 },
};

// 0x51DA04
static UpsideDownTableEntry _upside_down_table[13] = {
    { 0, 32 },
    { 48, 32 },
    { 49, 30 },
    { 52, 26 },
    { 55, 24 },
    { 57, 22 },
    { 60, 18 },
    { 63, 16 },
    { 65, 14 },
    { 67, 12 },
    { 70, 8 },
    { 73, 6 },
    { 75, 4 },
};

// 0x51DA6C
static STRUCT_51DA6C _verticies[10] = {
    { 16, -1, -201, 0 },
    { 48, -2, -2, 0 },
    { 960, 0, 0, 0 },
    { 992, 199, -1, 0 },
    { 1024, 198, 198, 0 },
    { 1936, 200, 200, 0 },
    { 1968, 399, 199, 0 },
    { 2000, 398, 398, 0 },
    { 2912, 400, 400, 0 },
    { 2944, 599, 399, 0 },
};

// 0x51DB0C
static RightsideUpTriangle _rightside_up_triangles[5] = {
    { 2, 3, 0 },
    { 3, 4, 1 },
    { 5, 6, 3 },
    { 6, 7, 4 },
    { 8, 9, 6 },
};

// 0x51DB48
static UpsideDownTriangle _upside_down_triangles[5] = {
    { 0, 3, 1 },
    { 2, 5, 3 },
    { 3, 6, 4 },
    { 5, 8, 6 },
    { 6, 9, 7 },
};

// 0x668224
static int _intensity_map[3280];

// 0x4B12C0
void tileWindowRefreshRect(Rect* rect, int elevation)
{
    if (gTileEnabled) {
        if (elevation == gElevation) {
            gTileWindowRefreshElevationProc(rect, elevation);
        }
    }
}

// 0x4B12D8
void tileWindowRefresh()
{
    if (gTileEnabled) {
        gTileWindowRefreshElevationProc(&gTileWindowRect, gElevation);
    }
}

// 0x4B1554
void tileRefreshMapper(Rect* rect, int elevation)
{
    Rect rectToUpdate;

    if (rectIntersection(rect, &gTileWindowRect, &rectToUpdate) == -1) {
        return;
    }

    bufferFill(gTileWindowBuffer + gTileWindowPitch * rectToUpdate.top + rectToUpdate.left,
        rectToUpdate.right - rectToUpdate.left + 1,
        rectToUpdate.bottom - rectToUpdate.top + 1,
        gTileWindowPitch,
        0);

    tileRenderFloorsInRect(&rectToUpdate, elevation);
    _grid_render(&rectToUpdate, elevation);
    _obj_render_pre_roof(&rectToUpdate, elevation);
    tileRenderRoofsInRect(&rectToUpdate, elevation);
    _obj_render_post_roof(&rectToUpdate, elevation);
    gTileWindowRefreshProc(&rectToUpdate);
}

// 0x4B15E8
void tileRefreshGame(Rect* rect, int elevation)
{
    Rect rectToUpdate;

    if (rectIntersection(rect, &gTileWindowRect, &rectToUpdate) == -1) {
        return;
    }

    // CE: Clear dirty rect to prevent most of the visual artifacts near map
    // edges.
    bufferFill(gTileWindowBuffer + rectToUpdate.top * gTileWindowPitch + rectToUpdate.left,
        rectGetWidth(&rectToUpdate),
        rectGetHeight(&rectToUpdate),
        gTileWindowPitch,
        0);

    tileRenderFloorsInRect(&rectToUpdate, elevation);
    _obj_render_pre_roof(&rectToUpdate, elevation);
    tileRenderRoofsInRect(&rectToUpdate, elevation);
    _obj_render_post_roof(&rectToUpdate, elevation);
    gTileWindowRefreshProc(&rectToUpdate);
}

// 0x4B20E8
void tileRenderRoofsInRect(Rect* rect, int elevation)
{
    if (!gTileRoofIsVisible) {
        return;
    }

    int temp;
    int minY;
    int minX;
    int maxX;
    int maxY;

    squareTileScreenToCoordRoof(rect->left, rect->top, elevation, &temp, &minY);
    squareTileScreenToCoordRoof(rect->right, rect->top, elevation, &minX, &temp);
    squareTileScreenToCoordRoof(rect->left, rect->bottom, elevation, &maxX, &temp);
    squareTileScreenToCoordRoof(rect->right, rect->bottom, elevation, &temp, &maxY);

    if (minX < 0) {
        minX = 0;
    }

    if (minX >= gSquareGridWidth) {
        minX = gSquareGridWidth - 1;
    }

    if (minY < 0) {
        minY = 0;
    }

    // FIXME: Probably a bug - testing X, then changing Y.
    if (minX >= gSquareGridHeight) {
        minY = gSquareGridHeight - 1;
    }

    int light = lightGetAmbientIntensity();

    int baseSquareTile = gSquareGridWidth * minY;

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            int squareTile = baseSquareTile + x;
            int frmId = gTileSquares[elevation]->field_0[squareTile];
            frmId >>= 16;
            if ((((frmId & 0xF000) >> 12) & 0x01) == 0) {
                int fid = buildFid(OBJ_TYPE_TILE, frmId & 0xFFF, 0, 0, 0);
                if (fid != buildFid(OBJ_TYPE_TILE, 1, 0, 0, 0)) {
                    int screenX;
                    int screenY;
                    squareTileToRoofScreenXY(squareTile, &screenX, &screenY, elevation);
                    tileRenderRoof(fid, screenX, screenY, rect, light);
                }
            }
        }
        baseSquareTile += gSquareGridWidth;
    }
}

// 0x4B24E0
static void tileRenderRoof(int fid, int x, int y, Rect* rect, int light)
{
    CacheEntry* tileFrmHandle;
    Art* tileFrm = artLock(fid, &tileFrmHandle);
    if (tileFrm == nullptr) {
        return;
    }

    int tileWidth = artGetWidth(tileFrm, 0, 0);
    int tileHeight = artGetHeight(tileFrm, 0, 0);

    Rect tileRect;
    tileRect.left = x;
    tileRect.top = y;
    tileRect.right = x + tileWidth - 1;
    tileRect.bottom = y + tileHeight - 1;

    if (rectIntersection(&tileRect, rect, &tileRect) == 0) {
        unsigned char* tileFrmBuffer = artGetFrameData(tileFrm, 0, 0);
        tileFrmBuffer += tileWidth * (tileRect.top - y) + (tileRect.left - x);

        CacheEntry* eggFrmHandle;
        Art* eggFrm = artLock(gEgg->fid, &eggFrmHandle);
        if (eggFrm != nullptr) {
            int eggWidth = artGetWidth(eggFrm, 0, 0);
            int eggHeight = artGetHeight(eggFrm, 0, 0);

            int eggScreenX;
            int eggScreenY;
            tileToScreenXY(gEgg->tile, &eggScreenX, &eggScreenY, gEgg->elevation);

            eggScreenX += 16;
            eggScreenY += 8;

            eggScreenX += eggFrm->xOffsets[0];
            eggScreenY += eggFrm->yOffsets[0];

            eggScreenX += gEgg->x;
            eggScreenY += gEgg->y;

            Rect eggRect;
            eggRect.left = eggScreenX - eggWidth / 2;
            eggRect.top = eggScreenY - eggHeight + 1;
            eggRect.right = eggRect.left + eggWidth - 1;
            eggRect.bottom = eggScreenY;

            gEgg->sx = eggRect.left;
            gEgg->sy = eggRect.top;

            Rect intersectedRect;
            if (rectIntersection(&eggRect, &tileRect, &intersectedRect) == 0) {
                Rect rects[4];

                rects[0].left = tileRect.left;
                rects[0].top = tileRect.top;
                rects[0].right = tileRect.right;
                rects[0].bottom = intersectedRect.top - 1;

                rects[1].left = tileRect.left;
                rects[1].top = intersectedRect.top;
                rects[1].right = intersectedRect.left - 1;
                rects[1].bottom = intersectedRect.bottom;

                rects[2].left = intersectedRect.right + 1;
                rects[2].top = intersectedRect.top;
                rects[2].right = tileRect.right;
                rects[2].bottom = intersectedRect.bottom;

                rects[3].left = tileRect.left;
                rects[3].top = intersectedRect.bottom + 1;
                rects[3].right = tileRect.right;
                rects[3].bottom = tileRect.bottom;

                for (int i = 0; i < 4; i++) {
                    Rect* cr = &(rects[i]);
                    if (cr->left <= cr->right && cr->top <= cr->bottom) {
                        _dark_trans_buf_to_buf(tileFrmBuffer + tileWidth * (cr->top - tileRect.top) + (cr->left - tileRect.left),
                            cr->right - cr->left + 1,
                            cr->bottom - cr->top + 1,
                            tileWidth,
                            gTileWindowBuffer,
                            cr->left,
                            cr->top,
                            gTileWindowPitch,
                            light);
                    }
                }

                unsigned char* eggBuf = artGetFrameData(eggFrm, 0, 0);
                _intensity_mask_buf_to_buf(tileFrmBuffer + tileWidth * (intersectedRect.top - tileRect.top) + (intersectedRect.left - tileRect.left),
                    intersectedRect.right - intersectedRect.left + 1,
                    intersectedRect.bottom - intersectedRect.top + 1,
                    tileWidth,
                    gTileWindowBuffer + gTileWindowPitch * intersectedRect.top + intersectedRect.left,
                    gTileWindowPitch,
                    eggBuf + eggWidth * (intersectedRect.top - eggRect.top) + (intersectedRect.left - eggRect.left),
                    eggWidth,
                    light);
            } else {
                _dark_trans_buf_to_buf(tileFrmBuffer, tileRect.right - tileRect.left + 1, tileRect.bottom - tileRect.top + 1, tileWidth, gTileWindowBuffer, tileRect.left, tileRect.top, gTileWindowPitch, light);
            }

            artUnlock(eggFrmHandle);
        }
    }

    artUnlock(tileFrmHandle);
}

// 0x4B2944
void tileRenderFloorsInRect(Rect* rect, int elevation)
{
    int minY;
    int maxX;
    int maxY;
    int minX;
    int temp;

    squareTileScreenToCoord(rect->left, rect->top, elevation, &temp, &minY);
    squareTileScreenToCoord(rect->right, rect->top, elevation, &minX, &temp);
    squareTileScreenToCoord(rect->left, rect->bottom, elevation, &maxX, &temp);
    squareTileScreenToCoord(rect->right, rect->bottom, elevation, &temp, &maxY);

    if (minX < 0) {
        minX = 0;
    }

    if (minX >= gSquareGridWidth) {
        minX = gSquareGridWidth - 1;
    }

    if (minY < 0) {
        minY = 0;
    }

    if (minX >= gSquareGridHeight) {
        minY = gSquareGridHeight - 1;
    }

    lightGetAmbientIntensity();

    int baseSquareTile = gSquareGridWidth * minY;

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            int squareTile = baseSquareTile + x;
            int frmId = gTileSquares[elevation]->field_0[squareTile];
            if ((((frmId & 0xF000) >> 12) & 0x01) == 0) {
                int tileScreenX;
                int tileScreenY;
                squareTileToScreenXY(squareTile, &tileScreenX, &tileScreenY, elevation);
                int fid = buildFid(OBJ_TYPE_TILE, frmId & 0xFFF, 0, 0, 0);
                tileRenderFloor(fid, tileScreenX, tileScreenY, rect);
            }
        }
        baseSquareTile += gSquareGridWidth;
    }
}

// 0x4B2E98
void _grid_render(Rect* rect, int elevation)
{
    if (!gTileGridIsVisible) {
        return;
    }

    for (int y = rect->top - 12; y < rect->bottom + 12; y += 6) {
        for (int x = rect->left - 32; x < rect->right + 32; x += 16) {
            int tile = tileFromScreenXY(x, y, elevation);
            _draw_grid(tile, elevation, rect);
        }
    }
}

// 0x4B2F4C
static void _draw_grid(int tile, int elevation, Rect* rect)
{
    if (tile == -1) {
        return;
    }

    int x;
    int y;
    tileToScreenXY(tile, &x, &y, elevation);

    Rect r;
    r.left = x;
    r.top = y;
    r.right = x + 32 - 1;
    r.bottom = y + 16 - 1;

    if (rectIntersection(&r, rect, &r) == -1) {
        return;
    }

    if (_obj_blocking_at(nullptr, tile, elevation) != nullptr) {
        blitBufferToBufferTrans(_tile_grid_blocked + 32 * (r.top - y) + (r.left - x),
            r.right - r.left + 1,
            r.bottom - r.top + 1,
            32,
            gTileWindowBuffer + gTileWindowPitch * r.top + r.left,
            gTileWindowPitch);
        return;
    }

    if (_obj_occupied(tile, elevation)) {
        blitBufferToBufferTrans(_tile_grid_occupied + 32 * (r.top - y) + (r.left - x),
            r.right - r.left + 1,
            r.bottom - r.top + 1,
            32,
            gTileWindowBuffer + gTileWindowPitch * r.top + r.left,
            gTileWindowPitch);
        return;
    }

    _translucent_trans_buf_to_buf(_tile_grid_occupied + 32 * (r.top - y) + (r.left - x),
        r.right - r.left + 1,
        r.bottom - r.top + 1,
        32,
        gTileWindowBuffer + gTileWindowPitch * r.top + r.left,
        0,
        0,
        gTileWindowPitch,
        _wallBlendTable,
        _commonGrayTable);
}

// 0x4B30C4
static void tileRenderFloor(int fid, int x, int y, Rect* rect)
{
    if (artIsObjectTypeHidden(FID_TYPE(fid)) != 0) {
        return;
    }

    CacheEntry* cacheEntry;
    Art* art = artLock(fid, &cacheEntry);
    if (art == nullptr) {
        return;
    }

    int elev = gElevation;
    int left = rect->left;
    int top = rect->top;
    int width = rect->right - rect->left + 1;
    int height = rect->bottom - rect->top + 1;
    int frameWidth;
    int frameHeight;
    int tile;
    int v76;
    int v77;
    int v78;
    int v79;

    int savedX = x;
    int savedY = y;

    if (left < 0) {
        left = 0;
    }

    if (top < 0) {
        top = 0;
    }

    if (left + width > gTileWindowWidth) {
        width = gTileWindowWidth - left;
    }

    if (top + height > gTileWindowHeight) {
        height = gTileWindowHeight - top;
    }

    if (x >= gTileWindowWidth || x > rect->right || y >= gTileWindowHeight || y > rect->bottom) goto out;

    frameWidth = artGetWidth(art, 0, 0);
    frameHeight = artGetHeight(art, 0, 0);

    if (left < x) {
        v79 = 0;
        int v12 = left + width;
        v77 = frameWidth + x <= v12 ? frameWidth : v12 - x;
    } else {
        v79 = left - x;
        x = left;
        v77 = frameWidth - v79;
        if (v77 > width) {
            v77 = width;
        }
    }

    if (top < y) {
        int v14 = height + top;
        v78 = 0;
        v76 = frameHeight + y <= v14 ? frameHeight : v14 - y;
    } else {
        v78 = top - y;
        y = top;
        v76 = frameHeight - v78;
        if (v76 > height) {
            v76 = height;
        }
    }

    if (v77 <= 0 || v76 <= 0) goto out;

    tile = tileFromScreenXY(savedX, savedY + 13, gElevation);
    if (tile != -1) {
        int parity = tile & 1;
        int ambientIntensity = lightGetAmbientIntensity();
        for (int i = 0; i < 10; i++) {
            // NOTE: Calls `lightGetTileIntensity` twice.
            _verticies[i].intensity = std::max(lightGetTileIntensity(elev, tile + _verticies[i].offsets[parity]), ambientIntensity);
        }

        int v23 = 0;
        for (int i = 0; i < 9; i++) {
            if (_verticies[i + 1].intensity != _verticies[i].intensity) {
                break;
            }

            v23++;
        }

        if (v23 == 9) {
            unsigned char* buf = artGetFrameData(art, 0, 0);
            _dark_trans_buf_to_buf(buf + frameWidth * v78 + v79, v77, v76, frameWidth, gTileWindowBuffer, x, y, gTileWindowPitch, _verticies[0].intensity);
            goto out;
        }

        for (int i = 0; i < 5; i++) {
            RightsideUpTriangle* triangle = &(_rightside_up_triangles[i]);
            int v32 = _verticies[triangle->field_8].intensity;
            int v33 = _verticies[triangle->field_8].field_0;
            int v34 = _verticies[triangle->field_4].intensity - _verticies[triangle->field_0].intensity;
            // TODO: Probably wrong.
            int v35 = v34 / 32;
            int v36 = (_verticies[triangle->field_0].intensity - v32) / 13;
            int* v37 = &(_intensity_map[v33]);
            if (v35 != 0) {
                if (v36 != 0) {
                    for (int i = 0; i < 13; i++) {
                        int v41 = v32;
                        int v42 = _rightside_up_table[i].field_4;
                        v37 += _rightside_up_table[i].field_0;
                        for (int j = 0; j < v42; j++) {
                            *v37++ = v41;
                            v41 += v35;
                        }
                        v32 += v36;
                    }
                } else {
                    for (int i = 0; i < 13; i++) {
                        int v38 = v32;
                        int v39 = _rightside_up_table[i].field_4;
                        v37 += _rightside_up_table[i].field_0;
                        for (int j = 0; j < v39; j++) {
                            *v37++ = v38;
                            v38 += v35;
                        }
                    }
                }
            } else {
                if (v36 != 0) {
                    for (int i = 0; i < 13; i++) {
                        int v46 = _rightside_up_table[i].field_4;
                        v37 += _rightside_up_table[i].field_0;
                        for (int j = 0; j < v46; j++) {
                            *v37++ = v32;
                        }
                        v32 += v36;
                    }
                } else {
                    for (int i = 0; i < 13; i++) {
                        int v44 = _rightside_up_table[i].field_4;
                        v37 += _rightside_up_table[i].field_0;
                        for (int j = 0; j < v44; j++) {
                            *v37++ = v32;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < 5; i++) {
            UpsideDownTriangle* triangle = &(_upside_down_triangles[i]);
            int v50 = _verticies[triangle->field_0].intensity;
            int v51 = _verticies[triangle->field_0].field_0;
            int v52 = _verticies[triangle->field_8].intensity - v50;
            // TODO: Probably wrong.
            int v53 = v52 / 32;
            int v54 = (_verticies[triangle->field_4].intensity - v50) / 13;
            int* v55 = &(_intensity_map[v51]);
            if (v53 != 0) {
                if (v54 != 0) {
                    for (int i = 0; i < 13; i++) {
                        int v59 = v50;
                        int v60 = _upside_down_table[i].field_4;
                        v55 += _upside_down_table[i].field_0;
                        for (int j = 0; j < v60; j++) {
                            *v55++ = v59;
                            v59 += v53;
                        }
                        v50 += v54;
                    }
                } else {
                    for (int i = 0; i < 13; i++) {
                        int v56 = v50;
                        int v57 = _upside_down_table[i].field_4;
                        v55 += _upside_down_table[i].field_0;
                        for (int j = 0; j < v57; j++) {
                            *v55++ = v56;
                            v56 += v53;
                        }
                    }
                }
            } else {
                if (v54 != 0) {
                    for (int i = 0; i < 13; i++) {
                        int v64 = _upside_down_table[i].field_4;
                        v55 += _upside_down_table[i].field_0;
                        for (int j = 0; j < v64; j++) {
                            *v55++ = v50;
                        }
                        v50 += v54;
                    }
                } else {
                    for (int i = 0; i < 13; i++) {
                        int v62 = _upside_down_table[i].field_4;
                        v55 += _upside_down_table[i].field_0;
                        for (int j = 0; j < v62; j++) {
                            *v55++ = v50;
                        }
                    }
                }
            }
        }

        unsigned char* v66 = gTileWindowBuffer + gTileWindowPitch * y + x;
        unsigned char* v67 = artGetFrameData(art, 0, 0) + frameWidth * v78 + v79;
        int* v68 = &(_intensity_map[160 + 80 * v78]) + v79;
        int v86 = frameWidth - v77;
        int v85 = gTileWindowPitch - v77;
        int v87 = 80 - v77;

        while (--v76 != -1) {
            for (int kk = 0; kk < v77; kk++) {
                if (*v67 != 0) {
                    *v66 = intensityColorTable[*v67][*v68 >> 9];
                }
                v67++;
                v68++;
                v66++;
            }
            v66 += v85;
            v68 += v87;
            v67 += v86;
        }
    }

out:

    artUnlock(cacheEntry);
}

} // namespace fallout
