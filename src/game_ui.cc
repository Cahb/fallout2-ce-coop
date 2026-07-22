#include "game_ui.h"

#include <stdio.h>
#include <string.h>

#include "game.h"

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "automap.h"
#include "character_editor.h"
#include "color.h"
#include "combat.h"
#include "config.h"
#include "cycle.h"
#include "db.h"
#include "dbox.h"
#include "debug.h"
#include "display_monitor.h"
#include "draw.h"
#include "game_config.h"
#include "game_dialog.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "inventory.h"
#include "kb.h"
#include "loadsave.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "mouse.h"
#include "options.h"
#include "palette.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "preferences.h"
#include "scripts.h"
#include "settings.h"
#include "skill.h"
#include "skilldex.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "version.h"
#include "window_manager.h"

// Client-side UI/input seam extracted from game.cc (REWRITE_PLAN, TU split).
// Pure presentation/input: the top-level key dispatcher plus the modal
// help/splash/quit/death screens and the UI enable/disable gate. Core game.cc
// retains lifecycle orchestration and the global-var/state ownership; it calls
// showSplash() by name through the game_ui.h seam and shares the de-static'd
// gIsMapper flag it owns. The public UI entry points (gameHandleKey, gameUi*,
// showQuitConfirmationDialog, gameShowDeathDialog) stay declared in game.h
// since existing callers include that. This move is mechanical and
// replay-identical.

namespace fallout {

#define HELP_SCREEN_WIDTH (640)
#define HELP_SCREEN_HEIGHT (480)

#define SPLASH_WIDTH (640)
#define SPLASH_HEIGHT (480)
#define SPLASH_COUNT (10)

static void showHelp();

// 0x5020B8
static char _aDec11199816543[] = VERSION_BUILD_TIME;

// 0x5186B4
static bool gGameUiDisabled = false;

// 0x442D44
int gameHandleKey(int eventCode, bool isInCombatMode)
{
    // NOTE: Uninline.
    if (gameGetState() == GAME_STATE_5) {
        _gdialogSystemEnter();
    }

    if (eventCode == -1) {
        if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
            int wheelX;
            int wheelY;
            mouseGetWheel(&wheelX, &wheelY);

            int dx = 0;
            if (wheelX > 0) {
                dx = 1;
            } else if (wheelX < 0) {
                dx = -1;
            }

            int dy = 0;
            if (wheelY > 0) {
                dy = -1;
            } else if (wheelY < 0) {
                dy = 1;
            }

            mapScroll(dx, dy);
        }
        return 0;
    }

    if (eventCode == -2) {
        int mouseState = mouseGetEvent();
        int mouseX;
        int mouseY;
        mouseGetPosition(&mouseX, &mouseY);

        if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
            if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_REPEAT) == 0) {
                if (mouseX == _scr_size.left || mouseX == _scr_size.right
                    || mouseY == _scr_size.top || mouseY == _scr_size.bottom) {
                    _gmouse_clicked_on_edge = true;
                } else {
                    _gmouse_clicked_on_edge = false;
                }
            }
        } else {
            if ((mouseState & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                _gmouse_clicked_on_edge = false;
            }
        }

        _gmouse_handle_event(mouseX, mouseY, mouseState);
        return 0;
    }

    if (_gmouse_is_scrolling()) {
        return 0;
    }

    switch (eventCode) {
    case -20:
        if (interfaceBarEnabled()) {
            _intface_use_item();
        }
        break;
    case -2:
        if (1) {
            int mouseEvent = mouseGetEvent();
            int mouseX;
            int mouseY;
            mouseGetPosition(&mouseX, &mouseY);

            if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0) {
                if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_REPEAT) == 0) {
                    if (mouseX == _scr_size.left || mouseX == _scr_size.right
                        || mouseY == _scr_size.top || mouseY == _scr_size.bottom) {
                        _gmouse_clicked_on_edge = true;
                    } else {
                        _gmouse_clicked_on_edge = false;
                    }
                }
            } else {
                if ((mouseEvent & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                    _gmouse_clicked_on_edge = false;
                }
            }

            _gmouse_handle_event(mouseX, mouseY, mouseEvent);
        }
        break;
    case KEY_CTRL_Q:
    case KEY_CTRL_X:
    case KEY_F10:
        soundPlayFile("ib1p1xx1");
        showQuitConfirmationDialog();
        break;
    case KEY_TAB:
        if (interfaceBarEnabled()
            && gPressedPhysicalKeys[SDL_SCANCODE_LALT] == 0
            && gPressedPhysicalKeys[SDL_SCANCODE_RALT] == 0) {
            soundPlayFile("ib1p1xx1");
            automapShow(true, false);
        }
        break;
    case KEY_CTRL_P:
        soundPlayFile("ib1p1xx1");
        showPause(false);
        break;
    case KEY_UPPERCASE_A:
    case KEY_LOWERCASE_A:
        if (interfaceBarEnabled()) {
            if (!isInCombatMode) {
                _combat(nullptr);
            }
        }
        break;
    case KEY_UPPERCASE_N:
    case KEY_LOWERCASE_N:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            interfaceCycleItemAction();
        }
        break;
    case KEY_UPPERCASE_M:
    case KEY_LOWERCASE_M:
        gameMouseCycleMode();
        break;
    case KEY_UPPERCASE_B:
    case KEY_LOWERCASE_B:
        // change active hand
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            interfaceBarSwapHands(true);
        }
        break;
    case KEY_UPPERCASE_C:
    case KEY_LOWERCASE_C:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            bool isoWasEnabled = isoDisable();
            characterEditorShow(false);
            if (isoWasEnabled) {
                isoEnable();
            }
        }
        break;
    case KEY_UPPERCASE_I:
    case KEY_LOWERCASE_I:
        // open inventory
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            inventoryOpen();
        }
        break;
    case KEY_ESCAPE:
    case KEY_UPPERCASE_O:
    case KEY_LOWERCASE_O:
        // options
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            showOptions();
        }
        break;
    case KEY_UPPERCASE_P:
    case KEY_LOWERCASE_P:
        // pipboy
        if (interfaceBarEnabled()) {
            if (isInCombatMode) {
                soundPlayFile("iisxxxx1");

                // Pipboy not available in combat!
                MessageListItem messageListItem;
                char title[128];
                strcpy(title, getmsg(&gMiscMessageList, &messageListItem, 7));
                showDialogBox(title, nullptr, 0, 192, 116, _colorTable[32328], nullptr, _colorTable[32328], 0);
            } else {
                soundPlayFile("ib1p1xx1");
                pipboyOpen(PIPBOY_OPEN_INTENT_UNSPECIFIED);
            }
        }
        break;
    case KEY_UPPERCASE_S:
    case KEY_LOWERCASE_S:
        // skilldex
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");

            int mode = -1;

            // NOTE: There is an `inc` for this value to build jump table which
            // is not needed.
            int rc = skilldexOpen();

            // Remap Skilldex result code to action.
            switch (rc) {
            case SKILLDEX_RC_ERROR:
                debugPrint("\n ** Error calling skilldex_select()! ** \n");
                break;
            case SKILLDEX_RC_SNEAK:
                _action_skill_use(SKILL_SNEAK);
                break;
            case SKILLDEX_RC_LOCKPICK:
                mode = GAME_MOUSE_MODE_USE_LOCKPICK;
                break;
            case SKILLDEX_RC_STEAL:
                mode = GAME_MOUSE_MODE_USE_STEAL;
                break;
            case SKILLDEX_RC_TRAPS:
                mode = GAME_MOUSE_MODE_USE_TRAPS;
                break;
            case SKILLDEX_RC_FIRST_AID:
                mode = GAME_MOUSE_MODE_USE_FIRST_AID;
                break;
            case SKILLDEX_RC_DOCTOR:
                mode = GAME_MOUSE_MODE_USE_DOCTOR;
                break;
            case SKILLDEX_RC_SCIENCE:
                mode = GAME_MOUSE_MODE_USE_SCIENCE;
                break;
            case SKILLDEX_RC_REPAIR:
                mode = GAME_MOUSE_MODE_USE_REPAIR;
                break;
            default:
                break;
            }

            if (mode != -1) {
                gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
                gameMouseSetMode(mode);
            }
        }
        break;
    case KEY_UPPERCASE_Z:
    case KEY_LOWERCASE_Z:
        if (interfaceBarEnabled()) {
            if (isInCombatMode) {
                soundPlayFile("iisxxxx1");

                // Pipboy not available in combat!
                MessageListItem messageListItem;
                char title[128];
                strcpy(title, getmsg(&gMiscMessageList, &messageListItem, 7));
                showDialogBox(title, nullptr, 0, 192, 116, _colorTable[32328], nullptr, _colorTable[32328], 0);
            } else {
                soundPlayFile("ib1p1xx1");
                pipboyOpen(PIPBOY_OPEN_INTENT_REST);
            }
        }
        break;
    case KEY_HOME:
        if (gDude->elevation != gElevation) {
            mapSetElevation(gDude->elevation);
        }

        if (gIsMapper) {
            tileSetCenter(gDude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
        } else {
            _tile_scroll_to(gDude->tile, 2);
        }

        break;
    case KEY_1:
    case KEY_EXCLAMATION:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            _action_skill_use(SKILL_SNEAK);
        }
        break;
    case KEY_2:
    case KEY_AT:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_LOCKPICK);
        }
        break;
    case KEY_3:
    case KEY_NUMBER_SIGN:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_STEAL);
        }
        break;
    case KEY_4:
    case KEY_DOLLAR:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_TRAPS);
        }
        break;
    case KEY_5:
    case KEY_PERCENT:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_FIRST_AID);
        }
        break;
    case KEY_6:
    case KEY_CARET:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_DOCTOR);
        }
        break;
    case KEY_7:
    case KEY_AMPERSAND:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_SCIENCE);
        }
        break;
    case KEY_8:
    case KEY_ASTERISK:
        if (interfaceBarEnabled()) {
            soundPlayFile("ib1p1xx1");
            gameMouseSetCursor(MOUSE_CURSOR_USE_CROSSHAIR);
            gameMouseSetMode(GAME_MOUSE_MODE_USE_REPAIR);
        }
        break;
    case KEY_MINUS:
    case KEY_UNDERSCORE:
        brightnessDecrease();
        break;
    case KEY_EQUAL:
    case KEY_PLUS:
        brightnessIncrease();
        break;
    case KEY_COMMA:
    case KEY_LESS:
        if (reg_anim_begin(ANIMATION_REQUEST_RESERVED) == 0) {
            animationRegisterRotateCounterClockwise(gDude);
            reg_anim_end();
        }
        break;
    case KEY_DOT:
    case KEY_GREATER:
        if (reg_anim_begin(ANIMATION_REQUEST_RESERVED) == 0) {
            animationRegisterRotateClockwise(gDude);
            reg_anim_end();
        }
        break;
    case KEY_SLASH:
    case KEY_QUESTION:
        if (1) {
            soundPlayFile("ib1p1xx1");

            int month;
            int day;
            int year;
            gameTimeGetDate(&month, &day, &year);

            MessageList messageList;
            if (messageListInit(&messageList)) {
                char path[COMPAT_MAX_PATH];
                snprintf(path, sizeof(path), "%s%s", asc_5186C8, "editor.msg");

                if (messageListLoad(&messageList, path)) {
                    MessageListItem messageListItem;
                    messageListItem.num = 500 + month - 1;
                    if (messageListGetItem(&messageList, &messageListItem)) {
                        char* time = gameTimeGetTimeString();

                        char date[128];
                        snprintf(date, sizeof(date), "%s: %d/%d %s", messageListItem.text, day, year, time);

                        displayMonitorAddMessage(date);
                    }
                }

                messageListFree(&messageList);
            }
        }
        break;
    case KEY_F1:
        soundPlayFile("ib1p1xx1");
        showHelp();
        break;
    case KEY_F2:
        gameSoundSetMasterVolume(gameSoundGetMasterVolume() - 2047);
        break;
    case KEY_F3:
        gameSoundSetMasterVolume(gameSoundGetMasterVolume() + 2047);
        break;
    case KEY_CTRL_S:
    case KEY_F4:
        soundPlayFile("ib1p1xx1");
        if (lsgSaveGame(1) == -1) {
            debugPrint("\n ** Error calling SaveGame()! **\n");
        }
        break;
    case KEY_CTRL_L:
    case KEY_F5:
        soundPlayFile("ib1p1xx1");
        if (lsgLoadGame(LOAD_SAVE_MODE_NORMAL) == -1) {
            debugPrint("\n ** Error calling LoadGame()! **\n");
        }
        break;
    case KEY_F6:
        if (1) {
            soundPlayFile("ib1p1xx1");

            int rc = lsgSaveGame(LOAD_SAVE_MODE_QUICK);
            if (rc == -1) {
                debugPrint("\n ** Error calling SaveGame()! **\n");
            } else if (rc == 1) {
                MessageListItem messageListItem;
                // Quick save game successfully saved.
                char* msg = getmsg(&gMiscMessageList, &messageListItem, 5);
                displayMonitorAddMessage(msg);
            }
        }
        break;
    case KEY_F7:
        if (1) {
            soundPlayFile("ib1p1xx1");

            int rc = lsgLoadGame(LOAD_SAVE_MODE_QUICK);
            if (rc == -1) {
                debugPrint("\n ** Error calling LoadGame()! **\n");
            } else if (rc == 1) {
                MessageListItem messageListItem;
                // Quick load game successfully loaded.
                char* msg = getmsg(&gMiscMessageList, &messageListItem, 4);
                displayMonitorAddMessage(msg);
            }
        }
        break;
    case KEY_CTRL_V:
        if (1) {
            soundPlayFile("ib1p1xx1");

            char version[VERSION_MAX];
            versionGetVersion(version, sizeof(version));
            displayMonitorAddMessage(version);
            displayMonitorAddMessage(_aDec11199816543);
        }
        break;
    case KEY_ARROW_LEFT:
        mapScroll(-1, 0);
        break;
    case KEY_ARROW_RIGHT:
        mapScroll(1, 0);
        break;
    case KEY_ARROW_UP:
        mapScroll(0, -1);
        break;
    case KEY_ARROW_DOWN:
        mapScroll(0, 1);
        break;
    }

    return 0;
}

// game_ui_disable
// 0x443BFC
void gameUiDisable(int a1)
{
    if (!gGameUiDisabled) {
        gameMouseObjectsHide();
        _gmouse_disable(a1);
        keyboardDisable();
        interfaceBarDisable();
        gGameUiDisabled = true;
    }
}

// game_ui_enable
// 0x443C30
void gameUiEnable()
{
    if (gGameUiDisabled) {
        interfaceBarEnable();
        keyboardEnable();
        keyboardReset();
        _gmouse_enable();
        gameMouseObjectsShow();
        gGameUiDisabled = false;
    }
}

// game_ui_is_disabled
// 0x443C60
bool gameUiIsDisabled()
{
    return gGameUiDisabled;
}

// 0x443F74
static void showHelp()
{
    ScopedGameMode gm(GameMode::kHelp);

    bool isoWasEnabled = isoDisable();
    gameMouseObjectsHide();

    gameMouseSetCursor(MOUSE_CURSOR_NONE);

    bool colorCycleWasEnabled = colorCycleEnabled();
    colorCycleDisable();

    // CE: Help screen uses separate color palette which is incompatible with
    // colors in other windows. Setup overlay to hide everything.
    int overlay = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), 0, WINDOW_HIDDEN | WINDOW_MOVE_ON_TOP);

    int helpWindowX = (screenGetWidth() - HELP_SCREEN_WIDTH) / 2;
    int helpWindowY = (screenGetHeight() - HELP_SCREEN_HEIGHT) / 2;
    int win = windowCreate(helpWindowX, helpWindowY, HELP_SCREEN_WIDTH, HELP_SCREEN_HEIGHT, 0, WINDOW_HIDDEN | WINDOW_MOVE_ON_TOP);
    if (win != -1) {
        unsigned char* windowBuffer = windowGetBuffer(win);
        if (windowBuffer != nullptr) {
            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 297, 0, 0, 0);
            if (backgroundFrmImage.lock(backgroundFid)) {
                paletteSetEntries(gPaletteBlack);
                blitBufferToBuffer(backgroundFrmImage.getData(), HELP_SCREEN_WIDTH, HELP_SCREEN_HEIGHT, HELP_SCREEN_WIDTH, windowBuffer, HELP_SCREEN_WIDTH);

                colorPaletteLoad("art\\intrface\\helpscrn.pal");
                paletteSetEntries(_cmap);

                // CE: Fill overlay with darkest color in the palette. It might
                // not be completely black, but at least it's uniform.
                bufferFill(windowGetBuffer(overlay),
                    screenGetWidth(),
                    screenGetHeight(),
                    screenGetWidth(),
                    intensityColorTable[_colorTable[0]][0]);

                windowShow(overlay);
                windowShow(win);

                while (inputGetInput() == -1 && _game_user_wants_to_quit == 0) {
                    sharedFpsLimiter.mark();
                    renderPresent();
                    sharedFpsLimiter.throttle();
                }

                while (mouseGetEvent() != 0) {
                    sharedFpsLimiter.mark();

                    inputGetInput();

                    renderPresent();
                    sharedFpsLimiter.throttle();
                }

                paletteSetEntries(gPaletteBlack);
            }
        }

        windowDestroy(overlay);
        windowDestroy(win);
        colorPaletteLoad("color.pal");
        paletteSetEntries(_cmap);
    }

    if (colorCycleWasEnabled) {
        colorCycleEnable();
    }

    gameMouseObjectsShow();

    if (isoWasEnabled) {
        isoEnable();
    }
}

// 0x4440B8
int showQuitConfirmationDialog()
{
    bool isoWasEnabled = isoDisable();

    bool gameMouseWasVisible;
    if (isoWasEnabled) {
        gameMouseWasVisible = gameMouseObjectsIsVisible();
    } else {
        gameMouseWasVisible = false;
    }

    if (gameMouseWasVisible) {
        gameMouseObjectsHide();
    }

    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    int oldCursor = gameMouseGetCursor();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    int rc;

    // Are you sure you want to quit?
    MessageListItem messageListItem;
    messageListItem.num = 0;
    if (messageListGetItem(&gMiscMessageList, &messageListItem)) {
        rc = showDialogBox(messageListItem.text, nullptr, 0, 169, 117, _colorTable[32328], nullptr, _colorTable[32328], DIALOG_BOX_YES_NO);
        if (rc != 0) {
            _game_user_wants_to_quit = 2;
        }
    } else {
        rc = -1;
    }

    gameMouseSetCursor(oldCursor);

    if (cursorWasHidden) {
        mouseHideCursor();
    }

    if (gameMouseWasVisible) {
        gameMouseObjectsShow();
    }

    if (isoWasEnabled) {
        isoEnable();
    }

    return rc;
}

// 0x444384
void showSplash()
{
    int splash = settings.system.splash;

    char path[64];
    const char* language = settings.system.language.c_str();
    if (compat_stricmp(language, ENGLISH) != 0) {
        snprintf(path, sizeof(path), "art\\%s\\splash\\", language);
    } else {
        snprintf(path, sizeof(path), "art\\splash\\");
    }

    File* stream;
    for (int index = 0; index < SPLASH_COUNT; index++) {
        char filePath[64];
        snprintf(filePath, sizeof(filePath), "%ssplash%d.rix", path, splash);
        stream = fileOpen(filePath, "rb");
        if (stream != nullptr) {
            break;
        }

        splash++;

        if (splash >= SPLASH_COUNT) {
            splash = 0;
        }
    }

    if (stream == nullptr) {
        return;
    }

    unsigned char* palette = reinterpret_cast<unsigned char*>(internal_malloc(768));
    if (palette == nullptr) {
        fileClose(stream);
        return;
    }

    int version;
    fileReadInt32(stream, &version);
    if (memcmp(&version, "RIX3", 4) != 0) {
        fileClose(stream);
        return;
    }

    short width;
    fileRead(&width, sizeof(width), 1, stream);

    short height;
    fileRead(&height, sizeof(height), 1, stream);

    unsigned char* data = reinterpret_cast<unsigned char*>(internal_malloc(width * height));
    if (data == nullptr) {
        internal_free(palette);
        fileClose(stream);
        return;
    }

    paletteSetEntries(gPaletteBlack);
    fileSeek(stream, 10, SEEK_SET);
    fileRead(palette, 1, 768, stream);
    fileRead(data, 1, width * height, stream);
    fileClose(stream);

    int size = 0;

    // TODO: Move to settings.
    Config config;
    if (configInit(&config)) {
        if (configRead(&config, "f2_res.ini", false)) {
            configGetInt(&config, "STATIC_SCREENS", "SPLASH_SCRN_SIZE", &size);
        }

        configFree(&config);
    }

    int screenWidth = screenGetWidth();
    int screenHeight = screenGetHeight();

    if (size != 0 || screenWidth < width || screenHeight < height) {
        int scaledWidth;
        int scaledHeight;

        if (size == 2) {
            scaledWidth = screenWidth;
            scaledHeight = screenHeight;
        } else {
            if (screenHeight * width >= screenWidth * height) {
                scaledWidth = screenWidth;
                scaledHeight = screenWidth * height / width;
            } else {
                scaledWidth = screenHeight * width / height;
                scaledHeight = screenHeight;
            }
        }

        unsigned char* scaled = reinterpret_cast<unsigned char*>(internal_malloc(scaledWidth * scaledHeight));
        if (scaled != nullptr) {
            blitBufferToBufferStretch(data, width, height, width, scaled, scaledWidth, scaledHeight, scaledWidth);

            int x = screenWidth > scaledWidth ? (screenWidth - scaledWidth) / 2 : 0;
            int y = screenHeight > scaledHeight ? (screenHeight - scaledHeight) / 2 : 0;
            _scr_blit(scaled, scaledWidth, scaledHeight, 0, 0, scaledWidth, scaledHeight, x, y);
            paletteFadeTo(palette);

            internal_free(scaled);
        }
    } else {
        int x = (screenWidth - width) / 2;
        int y = (screenHeight - height) / 2;
        _scr_blit(data, width, height, 0, 0, width, height, x, y);
        paletteFadeTo(palette);
    }

    internal_free(data);
    internal_free(palette);

    settings.system.splash = splash + 1;
}

int gameShowDeathDialog(const char* message)
{
    bool isoWasEnabled = isoDisable();

    bool gameMouseWasVisible;
    if (isoWasEnabled) {
        gameMouseWasVisible = gameMouseObjectsIsVisible();
    } else {
        gameMouseWasVisible = false;
    }

    if (gameMouseWasVisible) {
        gameMouseObjectsHide();
    }

    bool cursorWasHidden = cursorIsHidden();
    if (cursorWasHidden) {
        mouseShowCursor();
    }

    int oldCursor = gameMouseGetCursor();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    int oldUserWantsToQuit = _game_user_wants_to_quit;
    _game_user_wants_to_quit = 0;

    int rc = showDialogBox(message, nullptr, 0, 169, 117, _colorTable[32328], nullptr, _colorTable[32328], DIALOG_BOX_LARGE);

    _game_user_wants_to_quit = oldUserWantsToQuit;

    gameMouseSetCursor(oldCursor);

    if (cursorWasHidden) {
        mouseHideCursor();
    }

    if (gameMouseWasVisible) {
        gameMouseObjectsShow();
    }

    if (isoWasEnabled) {
        isoEnable();
    }

    return rc;
}

} // namespace fallout
