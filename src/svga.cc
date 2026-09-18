#include "svga.h"

#include <limits.h>
#include <string.h>

#include <SDL.h>

#include "config.h"
#include "draw.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect _scr_size;

// 0x6ACA18
void (*_scr_blit)(unsigned char* src, int src_pitch, int a3, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) = _GNW95_ShowRect;

// 0x6ACA1C
void (*_zero_mem)() = nullptr;

SDL_Window* gSdlWindow = nullptr;

// True when the window was created windowed (not fullscreen) — the mouse is grabbed
// to the window in that case, and re-grabbed on focus gain (input.cc).
bool gSdlWindowedMode = false;
SDL_Surface* gSdlSurface = nullptr;
SDL_Renderer* gSdlRenderer = nullptr;
SDL_Texture* gSdlTexture = nullptr;
SDL_Surface* gSdlTextureSurface = nullptr;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// 0x4CAD08
int _init_mode_320_200()
{
    return _GNW95_init_mode_ex(320, 200, 8);
}

// 0x4CAD40
int _init_mode_320_400()
{
    return _GNW95_init_mode_ex(320, 400, 8);
}

// 0x4CAD5C
int _init_mode_640_480_16()
{
    return -1;
}

// 0x4CAD64
int _init_mode_640_480()
{
    return _init_vesa_mode(640, 480);
}

// 0x4CAD94
int _init_mode_640_400()
{
    return _init_vesa_mode(640, 400);
}

// 0x4CADA8
int _init_mode_800_600()
{
    return _init_vesa_mode(800, 600);
}

// 0x4CADBC
int _init_mode_1024_768()
{
    return _init_vesa_mode(1024, 768);
}

// 0x4CADD0
int _init_mode_1280_1024()
{
    return _init_vesa_mode(1280, 1024);
}

// 0x4CADF8
void _get_start_mode_()
{
}

// 0x4CADFC
void _zero_vid_mem()
{
    if (_zero_mem) {
        _zero_mem();
    }
}

// 0x4CAE1C
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    bool fullscreen = true;
    int scale = 1;

    Config resolutionConfig;
    if (configInit(&resolutionConfig)) {
        if (configRead(&resolutionConfig, "f2_res.ini", false)) {
            int screenWidth;
            if (configGetInt(&resolutionConfig, "MAIN", "SCR_WIDTH", &screenWidth)) {
                width = screenWidth;
            }

            int screenHeight;
            if (configGetInt(&resolutionConfig, "MAIN", "SCR_HEIGHT", &screenHeight)) {
                height = screenHeight;
            }

            bool windowed;
            if (configGetBool(&resolutionConfig, "MAIN", "WINDOWED", &windowed)) {
                fullscreen = !windowed;
            }

            int scaleValue;
            if (configGetInt(&resolutionConfig, "MAIN", "SCALE_2X", &scaleValue)) {
                scale = scaleValue + 1; // 0 = 1x, 1 = 2x
                // Only allow scaling if resulting game resolution is >= 640x480
                if ((width / scale) < 640 || (height / scale) < 480) {
                    scale = 1;
                } else {
                    width /= scale;
                    height /= scale;
                }
            }

            configGetBool(&resolutionConfig, "IFACE", "IFACE_BAR_MODE", &gInterfaceBarMode);
            configGetInt(&resolutionConfig, "IFACE", "IFACE_BAR_WIDTH", &gInterfaceBarWidth);
            configGetInt(&resolutionConfig, "IFACE", "IFACE_BAR_SIDE_ART", &gInterfaceSidePanelsImageId);
            configGetBool(&resolutionConfig, "IFACE", "IFACE_BAR_SIDES_ORI", &gInterfaceSidePanelsExtendFromScreenEdge);
        }
        configFree(&resolutionConfig);
    }

    // F2_WINDOWED=1/0 overrides f2_res.ini's WINDOWED without editing the user's
    // config — the N-viewer demo needs several windowed clients side by side
    // (scripts/viewer_live.sh VIEWERS=N). Unset = config/default behavior.
    const char* windowedOverride = getenv("F2_WINDOWED");
    if (windowedOverride != nullptr) {
        fullscreen = atoi(windowedOverride) == 0;
    }

    if (_GNW95_init_window(width, height, fullscreen, scale) == -1) {
        return -1;
    }

    if (directDrawInit(width, height, bpp) == -1) {
        return -1;
    }

    _scr_size.left = 0;
    _scr_size.top = 0;
    _scr_size.right = width - 1;
    _scr_size.bottom = height - 1;

    _mouse_blit_trans = nullptr;
    _scr_blit = _GNW95_ShowRect;
    _zero_mem = _GNW95_zero_vid_mem;
    _mouse_blit = _GNW95_ShowRect;

    return 0;
}

// 0x4CAECC
int _init_vesa_mode(int width, int height)
{
    return _GNW95_init_mode_ex(width, height, 8);
}

// 0x4CAEDC
/*int _GNW95_init_window(int width, int height, bool fullscreen, int scale)
{
    if (gSdlWindow == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

        if (fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        }

        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width * scale, height * scale, windowFlags);
        if (gSdlWindow == nullptr) {
            // Fall back for video drivers without OpenGL support (notably
            // SDL's "dummy" driver used for headless runs).
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
            windowFlags &= ~SDL_WINDOW_OPENGL;
            gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width * scale, height * scale, windowFlags);
            if (gSdlWindow == nullptr) {
                return -1;
            }
        }

        if (!createRenderer(width, height)) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }

        // Confine the mouse to the window in WINDOWED mode — the game positions the
        // cursor in absolute coords over a small window, so without a grab the
        // pointer slips off the edge and clicks land outside (fullscreen already
        // confines). SDL releases the confine while the window is unfocused, so
        // alt-tab still works; input.cc re-applies it on focus gain.
        gSdlWindowedMode = !fullscreen;
        SDL_SetWindowGrab(gSdlWindow, gSdlWindowedMode ? SDL_TRUE : SDL_FALSE);
    }

    return 0;
}*/

// 0x4CAEDC new with resolution and borderless
int _GNW95_init_window(int width, int height, bool fullscreen, int scale)
{
    if (gSdlWindow == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest"); // Nearest-neighbor pixel-perfect scaling

        // --- Enhanced coop_res.ini with window borderless ---
        int game_w = 640;
        int game_h = 480;
        int win_w = 1600;
        int win_h = 900;
        int use_borderless = 1; // 1 = Enable borderless window, 0 = Classic windowed mode with frame

        FILE* ini = fopen("coop_res.ini", "r");
        if (ini) {
            // Read all 5 display parameters from the custom INI configuration
            fscanf(ini, "game_w=%d\ngame_h=%d\nwin_w=%d\nwin_h=%d\nborderless=%d",
                &game_w, &game_h, &win_w, &win_h, &use_borderless);
            fclose(ini);
        } else {
            ini = fopen("coop_res.ini", "w");
            if (ini) {
                // Generate default configuration if coop_res.ini is missing
                fprintf(ini, "game_w=640\ngame_h=480\nwin_w=1600\nwin_h=900\nborderless=1");
                fclose(ini);
            }
        }

        // Apply background scaling flags and disable focus pause for borderless mode
        if (!fullscreen && use_borderless == 1) {
            SDL_SetHint("SDL_ALLOW_BACKGROUND_EVENTS", "1"); // Process network and logic in background
            SDL_SetHint("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0"); // Prevent window from freezing or minimizing

            // Completely disable window state events to bypass focus-loss background sleep
            SDL_EventState(SDL_WINDOWEVENT, SDL_DISABLE);
        }
        
        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
        if (fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        } else {
            if (use_borderless == 1) {
                windowFlags |= SDL_WINDOW_BORDERLESS;
            }
        }

        // Create the physical window using custom viewport metrics from the INI file
        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, windowFlags);
        if (gSdlWindow == nullptr) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
            windowFlags &= ~SDL_WINDOW_OPENGL;
            gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, windowFlags);
            if (gSdlWindow == nullptr) {
                return -1;
            }
        }

        // Initialize the accelerated hardware renderer inside the custom physical window
        gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
        if (gSdlRenderer == nullptr) {
            return -1;
        }

        // Force SDL2 scaling subsystem to stretch game resolution up to physical window size
        SDL_RenderSetLogicalSize(gSdlRenderer, game_w, game_h);

        // Allocate texture buffers bound to original game dimensions to prevent viewport clipping
        gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, game_w, game_h);
        Uint32 format;
        SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr);
        gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, game_w, game_h, SDL_BITSPERPIXEL(format), format);

        gSdlWindowedMode = !fullscreen;
        SDL_SetWindowGrab(gSdlWindow, gSdlWindowedMode ? SDL_TRUE : SDL_FALSE);
    }

    return 0;
}

// 0x4CAF9C
int directDrawInit(int width, int height, int bpp)
{
    if (gSdlSurface != nullptr) {
        unsigned char* palette = directDrawGetPalette();
        directDrawFree();

        if (directDrawInit(width, height, bpp) == -1) {
            return -1;
        }

        directDrawSetPalette(palette);

        return 0;
    }

    gSdlSurface = SDL_CreateRGBSurface(0, width, height, bpp, 0, 0, 0, 0);

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    return 0;
}

// 0x4CB1B0
void directDrawFree()
{
    if (gSdlSurface != nullptr) {
        SDL_FreeSurface(gSdlSurface);
        gSdlSurface = nullptr;
    }
}

// 0x4CB310
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }
}

// 0x4CB568
void directDrawSetPalette(unsigned char* palette)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }
}

// 0x4CB68C
unsigned char* directDrawGetPalette()
{
    // 0x6ACA24
    static unsigned char palette[768];

    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color* colors = gSdlSurface->format->palette->colors;

        for (int index = 0; index < 256; index++) {
            SDL_Color* color = &(colors[index]);
            palette[index * 3] = color->r >> 2;
            palette[index * 3 + 1] = color->g >> 2;
            palette[index * 3 + 2] = color->b >> 2;
        }
    }

    return palette;
}

// 0x4CB850
void _GNW95_ShowRect(unsigned char* src, int srcPitch, int a3, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    blitBufferToBuffer(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);
}

// Clears drawing surface.
//
// 0x4CBBC8
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    unsigned char* surface = (unsigned char*)gSdlSurface->pixels;
    for (int y = 0; y < gSdlSurface->h; y++) {
        memset(surface, 0, gSdlSurface->w);
        surface += gSdlSurface->pitch;
    }

    SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&_scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&_scr_size);
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    if (!gInterfaceBarMode) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == nullptr) {
        return false;
    }

    return true;
}

static void destroyRenderer()
{
    if (gSdlTextureSurface != nullptr) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = nullptr;
    }

    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = nullptr;
    }

    if (gSdlRenderer != nullptr) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = nullptr;
    }
}

void handleWindowSizeChanged()
{
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
}

void renderPresent()
{
    SDL_UpdateTexture(gSdlTexture, nullptr, gSdlTextureSurface->pixels, gSdlTextureSurface->pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    SDL_RenderPresent(gSdlRenderer);
}

} // namespace fallout
