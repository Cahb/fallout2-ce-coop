#include "client_say.h"

#include <cstdio>
#include <cstring>

#include "client_net.h"
#include "color.h"
#include "interface.h"
#include "kb.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"

namespace fallout {

// The log's own font and the chat channel's own colour, so what you type looks like
// what everyone is about to read (msg_channel.h kMsgChannelChat = white).
#define SAY_FONT (101) // == DISPLAY_MONITOR_FONT
static constexpr int kSayColorIndex = (31 << 10) | (31 << 5) | 31; // white
static constexpr int kSayPromptColorIndex = (0 << 10) | (31 << 5) | 0; // vanilla green

// Cap the draft well inside the server's 256-byte control-line buffer, with room
// for the "say " prefix and the terminator. Chat does not need more.
static constexpr int kMaxSayLength = 120;

static constexpr int kSayBoxHeight = 20;
static constexpr int kSayTextInsetX = 6;
static constexpr int kSayTextInsetY = 5;

static bool gSayActive = false;
static int gSayWindow = -1;
static char gSayBuffer[kMaxSayLength + 1];
static int gSayLength = 0;

bool clientSayActive()
{
    return gSayActive;
}

static void sayDestroyWindow()
{
    if (gSayWindow != -1) {
        windowDestroy(gSayWindow);
        gSayWindow = -1;
    }
}

void clientSayOpen()
{
    if (gSayActive) {
        return;
    }
    gSayActive = true;
    gSayBuffer[0] = '\0';
    gSayLength = 0;
}

void clientSayCancel()
{
    // Idempotent on purpose: this is the force-close path (combat entry), called
    // without asking whether the box is even open.
    gSayActive = false;
    gSayBuffer[0] = '\0';
    gSayLength = 0;
    sayDestroyWindow();
}

bool clientSayHandleKey(int keyCode)
{
    if (!gSayActive) {
        return false;
    }

    switch (keyCode) {
    case KEY_RETURN:
        if (gSayLength > 0) {
            clientViewerSay(gSayBuffer);
        }
        clientSayCancel();
        return true;
    case KEY_ESCAPE:
        clientSayCancel();
        return true;
    case KEY_BACKSPACE:
        if (gSayLength > 0) {
            gSayBuffer[--gSayLength] = '\0';
        }
        return true;
    default:
        break;
    }

    // Printable ASCII only. The engine's keycodes ARE the ASCII values for this
    // range (kb.h), so no translation is needed. Anything else — function keys,
    // arrows, the -1 idle frame — is swallowed rather than falling through to the
    // gameplay dispatch below us: while the box is open, typing "a" must not
    // toggle combat and "b" must not swap weapon hands.
    if (keyCode >= 32 && keyCode <= 126) {
        if (gSayLength < kMaxSayLength) {
            gSayBuffer[gSayLength++] = (char)keyCode;
            gSayBuffer[gSayLength] = '\0';
        }
        return true;
    }

    return true;
}

void clientSayRender()
{
    if (!gSayActive) {
        sayDestroyWindow(); // covers a cancel that came from anywhere but our own key path
        return;
    }

    // Sit directly above the interface bar, spanning the screen like the message
    // log it feeds. Placed from the live screen size so the hi-res modes are fine.
    int width = screenGetWidth();
    int x = 0;
    int y = screenGetHeight() - INTERFACE_BAR_HEIGHT - kSayBoxHeight;
    if (y < 0) {
        y = 0;
    }

    if (gSayWindow == -1) {
        gSayWindow = windowCreate(x, y, width, kSayBoxHeight, _colorTable[0],
            WINDOW_MOVE_ON_TOP);
        if (gSayWindow == -1) {
            // No window to type into — do not strand the player in a state that
            // eats every keypress.
            clientSayCancel();
            return;
        }
    }

    windowFill(gSayWindow, 0, 0, width, kSayBoxHeight, _colorTable[0]);

    int oldFont = fontGetCurrent();
    fontSetCurrent(SAY_FONT);

    windowDrawText(gSayWindow, "Say:", 0, kSayTextInsetX, kSayTextInsetY,
        _colorTable[kSayPromptColorIndex]);
    int promptWidth = fontGetStringWidth("Say: ");

    // A blinking caret would need a clock; a static one is honest and cheap.
    char line[kMaxSayLength + 2];
    snprintf(line, sizeof(line), "%s_", gSayBuffer);
    windowDrawText(gSayWindow, line, 0, kSayTextInsetX + promptWidth, kSayTextInsetY,
        _colorTable[kSayColorIndex]);

    fontSetCurrent(oldFont);
    windowRefresh(gSayWindow);
}

} // namespace fallout
