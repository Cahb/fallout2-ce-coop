#include "game_movie.h"

#include <stdio.h>
#include <string.h>

#include "audio_engine.h" // audioEngineResume — a movie needs a running audio device even if unfocused
#include "color.h"
#include "cycle.h"
#include "debug.h"
#include "game.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "mouse.h"
#include "movie.h"
#include "movie_effect.h"
#include "palette.h"
#include "platform_compat.h"
#include "server_loop.h"
#include "settings.h"
#include "svga.h"
#include "text_font.h"
#include "touch.h"
#include "window_manager.h"

namespace fallout {

#define GAME_MOVIE_WINDOW_WIDTH 640
#define GAME_MOVIE_WINDOW_HEIGHT 480

static char* gameMovieBuildSubtitlesFilePath(char* movieFilePath);

// 0x50352A
static const float flt_50352A = 0.032258064f;

// 0x518DA0
static const char* gMovieFileNames[MOVIE_COUNT] = {
    "iplogo.mve",
    "intro.mve",
    "elder.mve",
    "vsuit.mve",
    "afailed.mve",
    "adestroy.mve",
    "car.mve",
    "cartucci.mve",
    "timeout.mve",
    "tanker.mve",
    "enclave.mve",
    "derrick.mve",
    "artimer1.mve",
    "artimer2.mve",
    "artimer3.mve",
    "artimer4.mve",
    "credits.mve",
};

// 0x518DE4
static const char* gMoviePaletteFilePaths[MOVIE_COUNT] = {
    nullptr,
    "art\\cuts\\introsub.pal",
    "art\\cuts\\eldersub.pal",
    nullptr,
    "art\\cuts\\artmrsub.pal",
    nullptr,
    nullptr,
    nullptr,
    "art\\cuts\\artmrsub.pal",
    nullptr,
    nullptr,
    nullptr,
    "art\\cuts\\artmrsub.pal",
    "art\\cuts\\artmrsub.pal",
    "art\\cuts\\artmrsub.pal",
    "art\\cuts\\artmrsub.pal",
    "art\\cuts\\crdtssub.pal",
};

// 0x518E28
static bool gGameMovieIsPlaying = false;

// 0x518E2C
static bool gGameMovieFaded = false;

// 0x596C89
static char gGameMovieSubtitlesFilePath[COMPAT_MAX_PATH];

// gmovie_init
// 0x44E5C0
int gameMoviesInit()
{
    int v1 = 0;
    if (backgroundSoundIsEnabled()) {
        v1 = backgroundSoundGetVolume();
    }

    movieSetVolume(v1);

    movieSetBuildSubtitleFilePathProc(gameMovieBuildSubtitlesFilePath);

    gameMoviesResetSeen();

    gGameMovieIsPlaying = false;
    gGameMovieFaded = false;

    return 0;
}

// 0x44E60C
void gameMoviesReset()
{
    gameMoviesResetSeen();

    gGameMovieIsPlaying = false;
    gGameMovieFaded = false;
}

// gmovie_play
// 0x44E690
int gameMoviePlay(int movie, int flags)
{
    // The viewer plays movies by a wire-supplied index (onMoviePlay) — a trust
    // boundary. Reject out-of-range before it reaches gMovieFileNames[movie] (OOB
    // read) or gameMovieMarkSeen (OOB write). SP passes MOVIE_* constants only.
    if (movie < 0 || movie >= MOVIE_COUNT) {
        return -1;
    }

    // Headless server: no playback pipeline (no frame/audio sync -> the
    // _movieRun / _MVE_sndSync loop spins forever). Preserve the only
    // sim-visible side effect -- gameMovieIsSeen is savegame state queried by
    // scripts (H-30) -- and skip the window/movie/palette machinery. Never
    // leaves gGameMovieIsPlaying set (script critter cadence gates on it).
    if (serverLoopActive()) {
        gameMovieMarkSeen(movie);
        return 0;
    }

    gGameMovieIsPlaying = true;

    // Guarantee a RUNNING audio device for the movie's whole duration, even on a
    // viewer that was ALREADY unfocused when the movie started. The MVE player is
    // slaved to the audio play cursor (audio_engine.cc), and a window that lost
    // focus BEFORE the movie began already paused its device (input.cc FOCUS_LOST,
    // gameMovieIsPlaying() was still false then) — so the callback never fires, the
    // cursor never advances, and the decoder stalls at frame 0 (a black window that
    // only "unsticks" if you focus it). Resume unconditionally here; audioEngineMixin
    // keeps the OUTPUT muted while unfocused, so this plays the cutscene through
    // silently rather than not at all. [[movie-playback-coop]]
    audioEngineResume();

    const char* movieFileName = gMovieFileNames[movie];
    debugPrint("\nPlaying movie: %s\n", movieFileName);

    const char* language = settings.system.language.c_str();
    char movieFilePath[COMPAT_MAX_PATH];
    int movieFileSize;
    bool movieFound = false;

    if (compat_stricmp(language, ENGLISH) != 0) {
        snprintf(movieFilePath, sizeof(movieFilePath), "art\\%s\\cuts\\%s", language, gMovieFileNames[movie]);
        movieFound = dbGetFileSize(movieFilePath, &movieFileSize) == 0;
    }

    if (!movieFound) {
        snprintf(movieFilePath, sizeof(movieFilePath), "art\\cuts\\%s", gMovieFileNames[movie]);
        movieFound = dbGetFileSize(movieFilePath, &movieFileSize) == 0;
    }

    if (!movieFound) {
        debugPrint("\ngmovie_play() - Error: Unable to open %s\n", gMovieFileNames[movie]);
        gGameMovieIsPlaying = false;
        return -1;
    }

    if ((flags & GAME_MOVIE_FADE_IN) != 0) {
        paletteFadeTo(gPaletteBlack);
        gGameMovieFaded = true;
    }

    // On a viewer running larger than the movie's 640x480, the movie window is
    // centered and its border is exposed. _zero_vid_mem blacks the raw surface, but
    // the GNW compositor can repaint stale window content (the tactical view) into
    // that border mid-playback, which the movie's OWN palette then renders as colour
    // noise (the "messed up background"). A full-screen black window BEHIND the movie
    // paints the border black through the compositor itself, so it survives every
    // repaint. Created before the movie window so it sits below it; destroyed just
    // before the post-movie windowRefreshAll. color 0 = filled with the black index.
    int movieBackdropWin = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), 0, 0);

    int gameMovieWindowX = (screenGetWidth() - GAME_MOVIE_WINDOW_WIDTH) / 2;
    int gameMovieWindowY = (screenGetHeight() - GAME_MOVIE_WINDOW_HEIGHT) / 2;
    int win = windowCreate(gameMovieWindowX,
        gameMovieWindowY,
        GAME_MOVIE_WINDOW_WIDTH,
        GAME_MOVIE_WINDOW_HEIGHT,
        0,
        WINDOW_MODAL);
    if (win == -1) {
        if (movieBackdropWin != -1) {
            windowDestroy(movieBackdropWin);
        }
        gGameMovieIsPlaying = false;
        return -1;
    }

    if ((flags & GAME_MOVIE_STOP_MUSIC) != 0) {
        backgroundSoundDelete();
    } else if ((flags & GAME_MOVIE_PAUSE_MUSIC) != 0) {
        backgroundSoundPause();
    }

    windowRefresh(win);

    bool subtitlesEnabled = settings.preferences.subtitles;
    int v1 = 4;
    if (subtitlesEnabled) {
        char* subtitlesFilePath = gameMovieBuildSubtitlesFilePath(movieFilePath);

        int subtitlesFileSize;
        if (dbGetFileSize(subtitlesFilePath, &subtitlesFileSize) == 0) {
            v1 = 12;
        } else {
            subtitlesEnabled = false;
        }
    }

    movieSetFlags(v1);

    int oldTextColor;
    int oldFont;
    if (subtitlesEnabled) {
        const char* subtitlesPaletteFilePath;
        if (gMoviePaletteFilePaths[movie] != nullptr) {
            subtitlesPaletteFilePath = gMoviePaletteFilePaths[movie];
        } else {
            subtitlesPaletteFilePath = "art\\cuts\\subtitle.pal";
        }

        colorPaletteLoad(subtitlesPaletteFilePath);

        oldTextColor = windowGetTextColor();
        windowSetTextColor(1.0, 1.0, 1.0);

        oldFont = fontGetCurrent();
        windowSetFont(101);
    }

    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        gameMouseSetCursor(MOUSE_CURSOR_NONE);
        mouseShowCursor();
    }

    while (mouseGetEvent() != 0) {
        _mouse_info();
    }

    mouseHideCursor();
    colorCycleDisable();

    movieEffectsLoad(movieFilePath);

    _zero_vid_mem();
    _movieRun(win, movieFilePath);

    int v11 = 0;
    int buttons;
    do {
        if (!_moviePlaying() || _game_user_wants_to_quit || inputGetInput() != -1) {
            break;
        }

        Gesture gesture;
        if (touch_get_gesture(&gesture) && gesture.state == kEnded) {
            break;
        }

        int x;
        int y;
        _mouse_get_raw_state(&x, &y, &buttons);

        v11 |= buttons;
    } while (((v11 & 1) == 0 && (v11 & 2) == 0) || (buttons & 1) != 0 || (buttons & 2) != 0);

    _movieStop();
    _moviefx_stop();
    _movieUpdate();
    paletteSetEntries(gPaletteBlack);

    gameMovieMarkSeen(movie);

    colorCycleEnable();

    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    if (!cursorWasHidden) {
        mouseShowCursor();
    }

    if (subtitlesEnabled) {
        colorPaletteLoad("color.pal");

        windowSetFont(oldFont);

        float r = (float)((Color2RGB(oldTextColor) & 0x7C00) >> 10) * flt_50352A;
        float g = (float)((Color2RGB(oldTextColor) & 0x3E0) >> 5) * flt_50352A;
        float b = (float)(Color2RGB(oldTextColor) & 0x1F) * flt_50352A;
        windowSetTextColor(r, g, b);
    }

    windowDestroy(win);
    if (movieBackdropWin != -1) {
        windowDestroy(movieBackdropWin);
    }

    // CE: Destroying a window redraws only content it was covering (centered
    // 640x480). This leads to everything outside this rect to remain black.
    windowRefreshAll(&_scr_size);

    if ((flags & GAME_MOVIE_PAUSE_MUSIC) != 0) {
        backgroundSoundResume();
    }

    if ((flags & GAME_MOVIE_FADE_OUT) != 0) {
        if (!subtitlesEnabled) {
            colorPaletteLoad("color.pal");
        }

        paletteFadeTo(_cmap);
        gGameMovieFaded = false;
    }

    gGameMovieIsPlaying = false;
    return 0;
}

// 0x44EAE4
void gameMovieFadeOut()
{
    if (gGameMovieFaded) {
        paletteFadeTo(_cmap);
        gGameMovieFaded = false;
    }
}

// 0x44EB14
bool gameMovieIsPlaying()
{
    return gGameMovieIsPlaying;
}

// 0x44EB1C
static char* gameMovieBuildSubtitlesFilePath(char* movieFilePath)
{
    char* path = movieFilePath;

    char* separator = strrchr(path, '\\');
    if (separator != nullptr) {
        path = separator + 1;
    }

    snprintf(gGameMovieSubtitlesFilePath, sizeof(gGameMovieSubtitlesFilePath), "text\\%s\\cuts\\%s", settings.system.language.c_str(), path);

    char* pch = strrchr(gGameMovieSubtitlesFilePath, '.');
    if (*pch != '\0') {
        *pch = '\0';
    }

    strcpy(gGameMovieSubtitlesFilePath + strlen(gGameMovieSubtitlesFilePath), ".SVE");

    return gGameMovieSubtitlesFilePath;
}

} // namespace fallout
