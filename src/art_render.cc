#include "art.h"

#include "draw.h"

// artRender is the ONLY presentation-coupled function of the art subsystem: it
// blits a frame into a caller buffer via draw.cc (SDL-backed). It is split out
// of the otherwise SDL-free art.cc so that art.cc can live in f2_core; this TU
// stays in f2_client. The server never renders, so it needs no stub here (the
// core never references artRender).

namespace fallout {

// 0x418FFC
void artRender(int fid, unsigned char* dest, int width, int height, int pitch)
{
    // NOTE: Original code is different. For unknown reason it directly calls
    // many art functions, for example instead of [artLock] it calls lower level
    // [cacheLock], instead of [artGetWidth] is calls [artGetFrame], then get
    // width from frame's struct field. I don't know if this was intentional or
    // not. I've replaced these calls with higher level functions where
    // appropriate.

    CacheEntry* handle;
    Art* frm = artLock(fid, &handle);
    if (frm == nullptr) {
        return;
    }

    unsigned char* frameData = artGetFrameData(frm, 0, 0);
    int frameWidth = artGetWidth(frm, 0, 0);
    int frameHeight = artGetHeight(frm, 0, 0);

    int remainingWidth = width - frameWidth;
    int remainingHeight = height - frameHeight;
    if (remainingWidth < 0 || remainingHeight < 0) {
        if (height * frameWidth >= width * frameHeight) {
            blitBufferToBufferStretchTrans(frameData,
                frameWidth,
                frameHeight,
                frameWidth,
                dest + pitch * ((height - width * frameHeight / frameWidth) / 2),
                width,
                width * frameHeight / frameWidth,
                pitch);
        } else {
            blitBufferToBufferStretchTrans(frameData,
                frameWidth,
                frameHeight,
                frameWidth,
                dest + (width - height * frameWidth / frameHeight) / 2,
                height * frameWidth / frameHeight,
                height,
                pitch);
        }
    } else {
        blitBufferToBufferTrans(frameData,
            frameWidth,
            frameHeight,
            frameWidth,
            dest + pitch * (remainingHeight / 2) + remainingWidth / 2,
            pitch);
    }

    artUnlock(handle);
}

} // namespace fallout
