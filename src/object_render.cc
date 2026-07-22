#include "object_render.h"

#include <string.h>

#include "art.h"
#include "color.h"
#include "draw.h"
#include "geometry.h"
#include "light.h"
#include "memory.h"
#include "object.h"
#include "proto.h"
#include "proto_instance.h"
#include "text_object.h"
#include "tile.h"

// Client-side render layer extracted from object.cc (Batch 7 / REWRITE_PLAN 1.4).
// Pure presentation: blits into the iso window buffer handed in by the client.
// object.cc (core sim) retains ownership of the shared state below and calls
// the render/blend table init/exit seam declared in object_render.h. This move
// is mechanical and replay-identical.

namespace fallout {

// Render-only file statics (moved wholesale from object.cc).
// 0x519620
static ObjectListNode** _renderTable = nullptr;
// Number of objects in _outlinedObjects.
//
// 0x519624
static int _outlineCount = 0;
// 0x519780
unsigned char* _wallBlendTable = nullptr;
// 0x519784
static unsigned char* _glassBlendTable = nullptr;
// 0x519788
static unsigned char* _steamBlendTable = nullptr;
// 0x51978C
static unsigned char* _energyBlendTable = nullptr;
// 0x519790
static unsigned char* _redBlendTable = nullptr;
// Likely outlined objects on the screen.
//
// 0x639C00
static Object* _outlinedObjects[100];
// 0x660EA0
static unsigned char _glassGrayTable[256];
// 0x660FA0
unsigned char _commonGrayTable[256];

static void objectDrawOutline(Object* object, Rect* rect);
static void _obj_render_object(Object* object, Rect* rect, int light);

// 0x489550
void _obj_render_pre_roof(Rect* rect, int elevation)
{
    if (!gObjectsInitialized) {
        return;
    }

    Rect updatedRect;
    if (rectIntersection(rect, &gObjectsWindowRect, &updatedRect) != 0) {
        return;
    }

    int ambientIntensity = lightGetAmbientIntensity();
    int minX = updatedRect.left - 320;
    int minY = updatedRect.top - 240;
    int maxX = updatedRect.right + 320;
    int maxY = updatedRect.bottom + 240;
    int upperLeftTile = tileFromScreenXY(minX, minY, elevation, true);
    int updateAreaHexWidth = (maxX - minX + 1) / 32;
    int updateAreaHexHeight = (maxY - minY + 1) / 12;
    int parity = gCenterTile & 1;

    _outlineCount = 0;

    int renderCount = 0;
    for (int i = 0; i < gObjectsUpdateAreaHexSize; i++) {
        int offsetIndex = _orderTable[parity][i];
        if (updateAreaHexHeight > _offsetDivTable[offsetIndex] && updateAreaHexWidth > _offsetModTable[offsetIndex]) {
            int tile = upperLeftTile + _offsetTable[parity][offsetIndex];
            ObjectListNode* objectListNode = hexGridTileIsValid(tile)
                ? gObjectListHeadByTile[tile]
                : nullptr;

            int lightIntensity;
            if (objectListNode != nullptr) {
                // NOTE: Calls `lightGetTileIntensity` twice.
                lightIntensity = std::max(ambientIntensity, lightGetTileIntensity(elevation, objectListNode->obj->tile));
            }

            while (objectListNode != nullptr) {
                if (elevation < objectListNode->obj->elevation) {
                    break;
                }

                if (elevation == objectListNode->obj->elevation) {
                    if ((objectListNode->obj->flags & OBJECT_FLAT) == 0) {
                        break;
                    }

                    if ((objectListNode->obj->flags & OBJECT_HIDDEN) == 0) {
                        _obj_render_object(objectListNode->obj, &updatedRect, lightIntensity);

                        if ((objectListNode->obj->outline & OUTLINE_TYPE_MASK) != 0) {
                            if ((objectListNode->obj->outline & OUTLINE_DISABLED) == 0 && _outlineCount < 100) {
                                _outlinedObjects[_outlineCount++] = objectListNode->obj;
                            }
                        }
                    }
                }

                objectListNode = objectListNode->next;
            }

            if (objectListNode != nullptr) {
                _renderTable[renderCount++] = objectListNode;
            }
        }
    }

    for (int i = 0; i < renderCount; i++) {
        int lightIntensity;

        ObjectListNode* objectListNode = _renderTable[i];
        if (objectListNode != nullptr) {
            // NOTE: Calls `lightGetTileIntensity` twice.
            lightIntensity = std::max(ambientIntensity, lightGetTileIntensity(elevation, objectListNode->obj->tile));
        }

        while (objectListNode != nullptr) {
            Object* object = objectListNode->obj;
            if (elevation < object->elevation) {
                break;
            }

            if (elevation == objectListNode->obj->elevation) {
                if ((objectListNode->obj->flags & OBJECT_HIDDEN) == 0) {
                    _obj_render_object(object, &updatedRect, lightIntensity);

                    if ((objectListNode->obj->outline & OUTLINE_TYPE_MASK) != 0) {
                        if ((objectListNode->obj->outline & OUTLINE_DISABLED) == 0 && _outlineCount < 100) {
                            _outlinedObjects[_outlineCount++] = objectListNode->obj;
                        }
                    }
                }
            }

            objectListNode = objectListNode->next;
        }
    }
}

// 0x4897EC
void _obj_render_post_roof(Rect* rect, int elevation)
{
    if (!gObjectsInitialized) {
        return;
    }

    Rect updatedRect;
    if (rectIntersection(rect, &gObjectsWindowRect, &updatedRect) != 0) {
        return;
    }

    for (int index = 0; index < _outlineCount; index++) {
        objectDrawOutline(_outlinedObjects[index], &updatedRect);
    }

    textObjectsRenderInRect(&updatedRect);

    ObjectListNode* objectListNode = gObjectListHead;
    while (objectListNode != nullptr) {
        Object* object = objectListNode->obj;
        if ((object->flags & OBJECT_HIDDEN) == 0) {
            _obj_render_object(object, &updatedRect, 0x10000);
        }
        objectListNode = objectListNode->next;
    }
}

// 0x48BDD8
void _translucent_trans_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, unsigned char* a9, unsigned char* a10)
{
    dest += destPitch * destY + destX;
    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            // TODO: Probably wrong.
            unsigned char v1 = a10[*src];
            unsigned char* v2 = a9 + (v1 << 8);
            unsigned char v3 = *dest;

            *dest = v2[v3];

            src++;
            dest++;
        }

        src += srcStep;
        dest += destStep;
    }
}

// 0x48BEFC
void _dark_trans_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, int intensity)
{
    unsigned char* sp = src;
    unsigned char* dp = dest + destPitch * destY + destX;

    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;
    int intensityIndex = intensity / 512;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            unsigned char color = *sp;
            if (color != 0) {
                if (color < 0xE5) {
                    color = intensityColorTable[color][intensityIndex];
                }

                *dp = color;
            }

            sp++;
            dp++;
        }

        sp += srcStep;
        dp += destStep;
    }
}

// 0x48BF88
void _dark_translucent_trans_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, int intensity, unsigned char* a10, unsigned char* a11)
{
    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;
    int intensityIndex = intensity / 512;

    dest += destPitch * destY + destX;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            unsigned char srcByte = *src;
            if (srcByte != 0) {
                unsigned char destByte = *dest;
                unsigned int index = a11[srcByte] << 8;
                index = a10[index + destByte];
                *dest = intensityColorTable[index][intensityIndex];
            }

            src++;
            dest++;
        }

        src += srcStep;
        dest += destStep;
    }
}

// 0x48C03C
void _intensity_mask_buf_to_buf(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destPitch, unsigned char* mask, int maskPitch, int intensity)
{
    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;
    int maskStep = maskPitch - srcWidth;
    int intensityIndex = intensity / 512;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            unsigned char color = *src;
            if (color != 0) {
                color = intensityColorTable[color][intensityIndex];
                if (*mask != 0) {
                    unsigned char v1 = intensityColorTable[*dest][128 - *mask];
                    unsigned char v2 = intensityColorTable[color][*mask];
                    color = colorMixAddTable[v2][v1];
                }
                *dest = color;
            }

            src++;
            dest++;
            mask++;
        }

        src += srcStep;
        dest += destStep;
        mask += maskStep;
    }
}

// 0x48CF8C
int _obj_render_table_init()
{
    if (_renderTable != nullptr) {
        return -1;
    }

    _renderTable = (ObjectListNode**)internal_malloc(sizeof(*_renderTable) * gObjectsUpdateAreaHexSize);
    if (_renderTable == nullptr) {
        return -1;
    }

    for (int index = 0; index < gObjectsUpdateAreaHexSize; index++) {
        _renderTable[index] = nullptr;
    }

    return 0;
}

// NOTE: Inlined.
//
// 0x48D000
void _obj_render_table_exit()
{
    if (_renderTable != nullptr) {
        internal_free(_renderTable);
        _renderTable = nullptr;
    }
}

// 0x48D1E4
void _obj_blend_table_init()
{
    for (int index = 0; index < 256; index++) {
        int r = (Color2RGB(index) & 0x7C00) >> 10;
        int g = (Color2RGB(index) & 0x3E0) >> 5;
        int b = Color2RGB(index) & 0x1F;
        _glassGrayTable[index] = ((r + 5 * g + 4 * b) / 10) >> 2;
        _commonGrayTable[index] = ((b + 3 * r + 6 * g) / 10) >> 2;
    }

    _glassGrayTable[0] = 0;
    _commonGrayTable[0] = 0;

    _wallBlendTable = _getColorBlendTable(_colorTable[25439]);
    _glassBlendTable = _getColorBlendTable(_colorTable[10239]);
    _steamBlendTable = _getColorBlendTable(_colorTable[32767]);
    _energyBlendTable = _getColorBlendTable(_colorTable[30689]);
    _redBlendTable = _getColorBlendTable(_colorTable[31744]);
}

// NOTE: Inlined.
//
// 0x48D2E8
void _obj_blend_table_exit()
{
    _freeColorBlendTable(_colorTable[25439]);
    _freeColorBlendTable(_colorTable[10239]);
    _freeColorBlendTable(_colorTable[32767]);
    _freeColorBlendTable(_colorTable[30689]);
    _freeColorBlendTable(_colorTable[31744]);
}

// 0x48EABC
static void objectDrawOutline(Object* object, Rect* rect)
{
    CacheEntry* cacheEntry;
    Art* art = artLock(object->fid, &cacheEntry);
    if (art == nullptr) {
        return;
    }

    int frameWidth = 0;
    int frameHeight = 0;
    artGetSize(art, object->frame, object->rotation, &frameWidth, &frameHeight);

    Rect v49;
    v49.left = 0;
    v49.top = 0;
    v49.right = frameWidth - 1;

    // FIXME: I'm not sure why it ignores frameHeight and makes separate call
    // to obtain height.
    int v8 = artGetHeight(art, object->frame, object->rotation);
    v49.bottom = v8 - 1;

    Rect objectRect;
    if (object->tile == -1) {
        objectRect.left = object->sx;
        objectRect.top = object->sy;
        objectRect.right = object->sx + frameWidth - 1;
        objectRect.bottom = object->sy + frameHeight - 1;
    } else {
        int x;
        int y;
        tileToScreenXY(object->tile, &x, &y, object->elevation);
        x += 16;
        y += 8;

        x += art->xOffsets[object->rotation];
        y += art->yOffsets[object->rotation];

        x += object->x;
        y += object->y;

        objectRect.left = x - frameWidth / 2;
        objectRect.top = y - (frameHeight - 1);
        objectRect.right = objectRect.left + frameWidth - 1;
        objectRect.bottom = y;

        object->sx = objectRect.left;
        object->sy = objectRect.top;
    }

    Rect v32;
    rectCopy(&v32, rect);

    v32.left--;
    v32.top--;
    v32.right++;
    v32.bottom++;

    rectIntersection(&v32, &gObjectsWindowRect, &v32);

    if (rectIntersection(&objectRect, &v32, &objectRect) == 0) {
        v49.left += objectRect.left - object->sx;
        v49.top += objectRect.top - object->sy;
        v49.right = v49.left + (objectRect.right - objectRect.left);
        v49.bottom = v49.top + (objectRect.bottom - objectRect.top);

        unsigned char* src = artGetFrameData(art, object->frame, object->rotation);

        unsigned char* dest = gObjectsWindowBuffer + gObjectsWindowPitch * object->sy + object->sx;
        int destStep = gObjectsWindowPitch - frameWidth;

        unsigned char color;
        unsigned char* v47 = nullptr;
        unsigned char* v48 = nullptr;
        int v53 = object->outline & OUTLINE_PALETTED;
        int outlineType = object->outline & OUTLINE_TYPE_MASK;
        int v43;
        int v44;

        switch (outlineType) {
        case OUTLINE_TYPE_HOSTILE:
            color = 243;
            v53 = 0;
            v43 = 5;
            v44 = frameHeight / 5;
            break;
        case OUTLINE_TYPE_2:
            color = _colorTable[31744];
            v44 = 0;
            if (v53 != 0) {
                v47 = _commonGrayTable;
                v48 = _redBlendTable;
            }
            break;
        case OUTLINE_TYPE_4:
            color = _colorTable[15855];
            v44 = 0;
            if (v53 != 0) {
                v47 = _commonGrayTable;
                v48 = _wallBlendTable;
            }
            break;
        case OUTLINE_TYPE_FRIENDLY:
            v43 = 4;
            v44 = frameHeight / 4;
            color = 229;
            v53 = 0;
            break;
        case OUTLINE_TYPE_ITEM:
            v44 = 0;
            color = _colorTable[30632];
            if (v53 != 0) {
                v47 = _commonGrayTable;
                v48 = _redBlendTable;
            }
            break;
        case OUTLINE_TYPE_32:
            color = 61;
            v53 = 0;
            v43 = 1;
            v44 = frameHeight;
            break;
        default:
            color = _colorTable[31775];
            v53 = 0;
            v44 = 0;
            break;
        }

        unsigned char v54 = color;
        unsigned char* dest14 = dest;
        unsigned char* src15 = src;
        for (int y = 0; y < frameHeight; y++) {
            bool cycle = true;
            if (v44 != 0) {
                if (y % v44 == 0) {
                    v54++;
                }

                if (v54 > v43 + color - 1) {
                    v54 = color;
                }
            }

            int v22 = dest14 - gObjectsWindowBuffer;
            for (int x = 0; x < frameWidth; x++) {
                v22 = dest14 - gObjectsWindowBuffer;
                if (*src15 != 0 && cycle) {
                    if (x >= v49.left && x <= v49.right && y >= v49.top && y <= v49.bottom && v22 > 0 && v22 % gObjectsWindowPitch != 0) {
                        unsigned char v20;
                        if (v53 != 0) {
                            v20 = v48[(v47[v54] << 8) + *(dest14 - 1)];
                        } else {
                            v20 = v54;
                        }
                        *(dest14 - 1) = v20;
                    }
                    cycle = false;
                } else if (*src15 == 0 && !cycle) {
                    if (x >= v49.left && x <= v49.right && y >= v49.top && y <= v49.bottom) {
                        int v21;
                        if (v53 != 0) {
                            v21 = v48[(v47[v54] << 8) + *dest14];
                        } else {
                            v21 = v54;
                        }
                        *dest14 = v21 & 0xFF;
                    }
                    cycle = true;
                }
                dest14++;
                src15++;
            }

            if (*(src15 - 1) != 0) {
                if (v22 < gObjectsWindowBufferSize) {
                    int v23 = frameWidth - 1;
                    if (v23 >= v49.left && v23 <= v49.right && y >= v49.top && y <= v49.bottom) {
                        if (v53 != 0) {
                            *dest14 = v48[(v47[v54] << 8) + *dest14];
                        } else {
                            *dest14 = v54;
                        }
                    }
                }
            }

            dest14 += destStep;
        }

        for (int x = 0; x < frameWidth; x++) {
            bool cycle = true;
            unsigned char v28 = color;
            unsigned char* dest27 = dest + x;
            unsigned char* src27 = src + x;
            for (int y = 0; y < frameHeight; y++) {
                if (v44 != 0) {
                    if (y % v44 == 0) {
                        v28++;
                    }

                    if (v28 > color + v43 - 1) {
                        v28 = color;
                    }
                }

                if (*src27 != 0 && cycle) {
                    if (x >= v49.left && x <= v49.right && y >= v49.top && y <= v49.bottom) {
                        unsigned char* v29 = dest27 - gObjectsWindowPitch;
                        if (v29 >= gObjectsWindowBuffer) {
                            if (v53) {
                                *v29 = v48[(v47[v28] << 8) + *v29];
                            } else {
                                *v29 = v28;
                            }
                        }
                    }
                    cycle = false;
                } else if (*src27 == 0 && !cycle) {
                    if (x >= v49.left && x <= v49.right && y >= v49.top && y <= v49.bottom) {
                        if (v53) {
                            *dest27 = v48[(v47[v28] << 8) + *dest27];
                        } else {
                            *dest27 = v28;
                        }
                    }
                    cycle = true;
                }

                dest27 += gObjectsWindowPitch;
                src27 += frameWidth;
            }

            if (src27[-frameWidth] != 0) {
                if (dest27 - gObjectsWindowBuffer < gObjectsWindowBufferSize) {
                    int y = frameHeight - 1;
                    if (x >= v49.left && x <= v49.right && y >= v49.top && y <= v49.bottom) {
                        if (v53) {
                            *dest27 = v48[(v47[v28] << 8) + *dest27];
                        } else {
                            *dest27 = v28;
                        }
                    }
                }
            }
        }
    }

    artUnlock(cacheEntry);
}

// 0x48F1B0
static void _obj_render_object(Object* object, Rect* rect, int light)
{
    int type = FID_TYPE(object->fid);
    if (artIsObjectTypeHidden(type)) {
        return;
    }

    CacheEntry* cacheEntry;
    Art* art = artLock(object->fid, &cacheEntry);
    if (art == nullptr) {
        return;
    }

    int frameWidth = artGetWidth(art, object->frame, object->rotation);
    int frameHeight = artGetHeight(art, object->frame, object->rotation);

    Rect objectRect;
    if (object->tile == -1) {
        objectRect.left = object->sx;
        objectRect.top = object->sy;
        objectRect.right = object->sx + frameWidth - 1;
        objectRect.bottom = object->sy + frameHeight - 1;
    } else {
        int objectScreenX;
        int objectScreenY;
        tileToScreenXY(object->tile, &objectScreenX, &objectScreenY, object->elevation);
        objectScreenX += 16;
        objectScreenY += 8;

        objectScreenX += art->xOffsets[object->rotation];
        objectScreenY += art->yOffsets[object->rotation];

        objectScreenX += object->x;
        objectScreenY += object->y;

        objectRect.left = objectScreenX - frameWidth / 2;
        objectRect.top = objectScreenY - (frameHeight - 1);
        objectRect.right = objectRect.left + frameWidth - 1;
        objectRect.bottom = objectScreenY;

        object->sx = objectRect.left;
        object->sy = objectRect.top;
    }

    if (rectIntersection(&objectRect, rect, &objectRect) != 0) {
        artUnlock(cacheEntry);
        return;
    }

    unsigned char* src = artGetFrameData(art, object->frame, object->rotation);
    unsigned char* src2 = src;
    int v50 = objectRect.left - object->sx;
    int v49 = objectRect.top - object->sy;
    src += frameWidth * v49 + v50;
    int objectWidth = objectRect.right - objectRect.left + 1;
    int objectHeight = objectRect.bottom - objectRect.top + 1;

    if (type == 6) {
        blitBufferToBufferTrans(src,
            objectWidth,
            objectHeight,
            frameWidth,
            gObjectsWindowBuffer + gObjectsWindowPitch * objectRect.top + objectRect.left,
            gObjectsWindowPitch);
        artUnlock(cacheEntry);
        return;
    }

    if (type == 2 || type == 3) {
        if ((gDude->flags & OBJECT_HIDDEN) == 0 && (object->flags & OBJECT_FLAG_0xFC000) == 0) {
            Proto* proto;
            protoGetProto(object->pid, &proto);

            bool v17;
            int extendedFlags = proto->critter.extendedFlags;
            if ((extendedFlags & 0x8000000) != 0 || (extendedFlags & 0x80000000) != 0) {
                // TODO: Probably wrong.
                v17 = tileIsInFrontOf(object->tile, gDude->tile);
                if (!v17
                    || !tileIsToRightOf(object->tile, gDude->tile)
                    || (object->flags & OBJECT_WALL_TRANS_END) == 0) {
                    // nothing
                } else {
                    v17 = false;
                }
            } else if ((extendedFlags & 0x10000000) != 0) {
                // NOTE: Uses bitwise OR, so both functions are evaluated.
                v17 = tileIsInFrontOf(object->tile, gDude->tile)
                    || tileIsToRightOf(gDude->tile, object->tile);
            } else if ((extendedFlags & 0x20000000) != 0) {
                v17 = tileIsInFrontOf(object->tile, gDude->tile)
                    && tileIsToRightOf(gDude->tile, object->tile);
            } else {
                v17 = tileIsToRightOf(gDude->tile, object->tile);
                if (v17
                    && tileIsInFrontOf(gDude->tile, object->tile)
                    && (object->flags & OBJECT_WALL_TRANS_END) != 0) {
                    v17 = 0;
                }
            }

            if (v17) {
                CacheEntry* eggHandle;
                Art* egg = artLock(gEgg->fid, &eggHandle);
                if (egg == nullptr) {
                    return;
                }

                int eggWidth;
                int eggHeight;
                artGetSize(egg, 0, 0, &eggWidth, &eggHeight);

                int eggScreenX;
                int eggScreenY;
                tileToScreenXY(gEgg->tile, &eggScreenX, &eggScreenY, gEgg->elevation);
                eggScreenX += 16;
                eggScreenY += 8;

                eggScreenX += egg->xOffsets[0];
                eggScreenY += egg->yOffsets[0];

                eggScreenX += gEgg->x;
                eggScreenY += gEgg->y;

                Rect eggRect;
                eggRect.left = eggScreenX - eggWidth / 2;
                eggRect.top = eggScreenY - (eggHeight - 1);
                eggRect.right = eggRect.left + eggWidth - 1;
                eggRect.bottom = eggScreenY;

                gEgg->sx = eggRect.left;
                gEgg->sy = eggRect.top;

                Rect updatedEggRect;
                if (rectIntersection(&eggRect, &objectRect, &updatedEggRect) == 0) {
                    Rect rects[4];

                    rects[0].left = objectRect.left;
                    rects[0].top = objectRect.top;
                    rects[0].right = objectRect.right;
                    rects[0].bottom = updatedEggRect.top - 1;

                    rects[1].left = objectRect.left;
                    rects[1].top = updatedEggRect.top;
                    rects[1].right = updatedEggRect.left - 1;
                    rects[1].bottom = updatedEggRect.bottom;

                    rects[2].left = updatedEggRect.right + 1;
                    rects[2].top = updatedEggRect.top;
                    rects[2].right = objectRect.right;
                    rects[2].bottom = updatedEggRect.bottom;

                    rects[3].left = objectRect.left;
                    rects[3].top = updatedEggRect.bottom + 1;
                    rects[3].right = objectRect.right;
                    rects[3].bottom = objectRect.bottom;

                    for (int i = 0; i < 4; i++) {
                        Rect* v21 = &(rects[i]);
                        if (v21->left <= v21->right && v21->top <= v21->bottom) {
                            unsigned char* sp = src + frameWidth * (v21->top - objectRect.top) + (v21->left - objectRect.left);
                            _dark_trans_buf_to_buf(sp, v21->right - v21->left + 1, v21->bottom - v21->top + 1, frameWidth, gObjectsWindowBuffer, v21->left, v21->top, gObjectsWindowPitch, light);
                        }
                    }

                    unsigned char* mask = artGetFrameData(egg, 0, 0);
                    _intensity_mask_buf_to_buf(
                        src + frameWidth * (updatedEggRect.top - objectRect.top) + (updatedEggRect.left - objectRect.left),
                        updatedEggRect.right - updatedEggRect.left + 1,
                        updatedEggRect.bottom - updatedEggRect.top + 1,
                        frameWidth,
                        gObjectsWindowBuffer + gObjectsWindowPitch * updatedEggRect.top + updatedEggRect.left,
                        gObjectsWindowPitch,
                        mask + eggWidth * (updatedEggRect.top - eggRect.top) + (updatedEggRect.left - eggRect.left),
                        eggWidth,
                        light);
                    artUnlock(eggHandle);
                    artUnlock(cacheEntry);
                    return;
                }

                artUnlock(eggHandle);
            }
        }
    }

    switch (object->flags & OBJECT_FLAG_0xFC000) {
    case OBJECT_TRANS_RED:
        _dark_translucent_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, light, _redBlendTable, _commonGrayTable);
        break;
    case OBJECT_TRANS_WALL:
        _dark_translucent_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, 0x10000, _wallBlendTable, _commonGrayTable);
        break;
    case OBJECT_TRANS_GLASS:
        _dark_translucent_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, light, _glassBlendTable, _glassGrayTable);
        break;
    case OBJECT_TRANS_STEAM:
        _dark_translucent_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, light, _steamBlendTable, _commonGrayTable);
        break;
    case OBJECT_TRANS_ENERGY:
        _dark_translucent_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, light, _energyBlendTable, _commonGrayTable);
        break;
    default:
        _dark_trans_buf_to_buf(src, objectWidth, objectHeight, frameWidth, gObjectsWindowBuffer, objectRect.left, objectRect.top, gObjectsWindowPitch, light);
        break;
    }

    artUnlock(cacheEntry);
}

} // namespace fallout
