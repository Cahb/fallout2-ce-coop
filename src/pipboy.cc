#include "pipboy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "art.h"
#include "automap.h"
#include "client_net.h" // clientViewerActive — a viewer does not own the clock
#include "color.h"
#include "combat.h"
#include "config.h"
#include "critter.h"
#include "cycle.h"
#include "dbox.h"
#include "debug.h"
#include "delay.h"
#include "draw.h"
#include "game.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "platform_compat.h"
#include "queue.h"
#include "random.h"
#include "rest.h" // restPerform — the shared rest simulation
#include "scripts.h"
#include "server_loop.h"
#include "settings.h"
#include "sfall_config.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"
#include "word_wrap.h"
#include "worldmap.h"

namespace fallout {

#define PIPBOY_WINDOW_WIDTH (640)
#define PIPBOY_WINDOW_HEIGHT (480)

#define PIPBOY_WINDOW_DAY_X (20)
#define PIPBOY_WINDOW_DAY_Y (17)

#define PIPBOY_WINDOW_MONTH_X (46)
#define PIPBOY_WINDOW_MONTH_Y (18)

#define PIPBOY_WINDOW_YEAR_X (83)
#define PIPBOY_WINDOW_YEAR_Y (17)

#define PIPBOY_WINDOW_TIME_X (155)
#define PIPBOY_WINDOW_TIME_Y (17)

#define PIPBOY_HOLODISK_LINES_MAX (35)

#define PIPBOY_WINDOW_CONTENT_VIEW_X (254)
#define PIPBOY_WINDOW_CONTENT_VIEW_Y (46)
#define PIPBOY_WINDOW_CONTENT_VIEW_WIDTH (374)
#define PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT (410)

#define PIPBOY_IDLE_TIMEOUT (120000)
#define PIPBOY_RAND_MAX (32767)

#define PIPBOY_BOMB_COUNT (16)

typedef enum Holiday {
    HOLIDAY_NEW_YEAR,
    HOLIDAY_VALENTINES_DAY,
    HOLIDAY_FOOLS_DAY,
    HOLIDAY_SHIPPING_DAY,
    HOLIDAY_INDEPENDENCE_DAY,
    HOLIDAY_HALLOWEEN,
    HOLIDAY_THANKSGIVING_DAY,
    HOLIDAY_CRISTMAS,
    HOLIDAY_COUNT,
} Holiday;

// Options used to render Pipboy texts.
typedef enum PipboyTextOptions {
    // Specifies that text should be rendered in the center of the Pipboy
    // monitor.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_CENTER = 0x02,

    // Specifies that text should be rendered in the beginning of the right
    // column in two-column layout.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN = 0x04,

    // Specifies that text should be rendered in the center of the left column.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER = 0x10,

    // Specifies that text should be rendered in the center of the right
    // column.
    //
    // This option is mutually exclusive with other alignment options.
    PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER = 0x20,

    // Specifies that text should rendered with underline.
    PIPBOY_TEXT_STYLE_UNDERLINE = 0x08,

    // Specifies that text should rendered with strike-through line.
    PIPBOY_TEXT_STYLE_STRIKE_THROUGH = 0x40,

    // Specifies that text should be rendered with no (minimal) indentation.
    PIPBOY_TEXT_NO_INDENT = 0x80,
} PipboyTextOptions;

// Ledger H-42: the PipboyRestDuration enum moved to party_member.h with the
// rest-intent decoder.

typedef enum PipboyFrm {
    PIPBOY_FRM_LITTLE_RED_BUTTON_UP,
    PIPBOY_FRM_LITTLE_RED_BUTTON_DOWN,
    PIPBOY_FRM_NUMBERS,
    PIPBOY_FRM_BACKGROUND,
    PIPBOY_FRM_NOTE,
    PIPBOY_FRM_MONTHS,
    PIPBOY_FRM_NOTE_NUMBERS,
    PIPBOY_FRM_ALARM_DOWN,
    PIPBOY_FRM_ALARM_UP,
    PIPBOY_FRM_LOGO,
    PIPBOY_FRM_BOMB,
    PIPBOY_FRM_COUNT,
} PipboyFrm;

// Provides metadata information on quests.
//
// Loaded from `data/quest.txt`.
typedef struct QuestDescription {
    int location;
    int description;
    int gvar;
    int displayThreshold;
    int completedThreshold;
} QuestDescription;

// Provides metadata information on holodisks.
//
// Loaded from `data/holodisk.txt`.
typedef struct HolodiskDescription {
    int gvar;
    int name;
    int description;
} HolodiskDescription;

// ─── SERVER-AUTHORED HOLODISKS (see pipboy.h for the model + why it is cheap) ──

struct ServerHolodisk {
    std::string name;
    std::vector<std::string> lines;
};

// Whole set, replaced on each announcement so a rebaseline cannot duplicate it.
static std::vector<ServerHolodisk> gServerHolodisks;

void pipboyServerHolodiskClear()
{
    gServerHolodisks.clear();
}

void pipboyServerHolodiskAdd(const char* name, const char* const* lines, int lineCount)
{
    ServerHolodisk disk;
    disk.name = name != nullptr ? name : "";
    for (int i = 0; i < lineCount; i++) {
        disk.lines.push_back(lines[i] != nullptr ? lines[i] : "");
    }
    gServerHolodisks.push_back(std::move(disk));
}

int pipboyServerHolodiskCount()
{
    return (int)gServerHolodisks.size();
}


typedef struct HolidayDescription {
    short month;
    short day;
    short textId;
} HolidayDescription;

typedef struct STRUCT_664350 {
    char* name;
    short field_4;
    short field_6;
} STRUCT_664350;

typedef struct PipboyBomb {
    int x;
    int y;
    float field_8;
    float field_C;
    unsigned char field_10;
} PipboyBomb;

typedef void PipboyRenderProc(int a1);

static int pipboyWindowInit(int intent);
static void pipboyWindowFree();
static void _pip_init_();
static void pipboyDrawNumber(int value, int digits, int x, int y);
static void pipboyDrawDate();
static void pipboyDrawText(const char* text, int a2, int a3);
static void pipboyDrawBackButton(int a1);
static int _save_pipboy(File* stream);
static void pipboyWindowHandleStatus(int a1);
static void pipboyWindowRenderQuestLocationList(int a1);
static void pipboyRenderHolodiskText();
static int pipboyWindowRenderHolodiskList(int a1);
static int _qscmp(const void* a1, const void* a2);
static void pipboyWindowHandleAutomaps(int a1);
static int _PrintAMelevList(int a1);
static int _PrintAMList(int a1);
static void pipboyHandleVideoArchive(int a1);
static int pipboyRenderVideoArchive(int a1);
static void pipboyHandleAlarmClock(int eventCode);
static void pipboyWindowRenderRestOptions(int a1);
static void pipboyDrawHitPoints();
static void pipboyWindowCreateButtons(int a1, int a2, bool a3);
static void pipboyWindowDestroyButtons();
static bool pipboyRest(int hours, int minutes, int kind);
static int pipboyRenderScreensaver();
static int questInit();
static void questFree();
static int questDescriptionCompare(const void* a1, const void* a2);
static int holodiskInit();
static void holodiskFree();

// 0x496FC0
const Rect gPipboyWindowContentRect = {
    PIPBOY_WINDOW_CONTENT_VIEW_X,
    PIPBOY_WINDOW_CONTENT_VIEW_Y,
    PIPBOY_WINDOW_CONTENT_VIEW_X + PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
    PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
};

// 0x496FD0
const int gPipboyFrmIds[PIPBOY_FRM_COUNT] = {
    8,
    9,
    82,
    127,
    128,
    129,
    130,
    131,
    132,
    133,
    226,
};

// 0x51C128
QuestDescription* gQuestDescriptions = nullptr;

// 0x51C12C
int gQuestsCount = 0;

// 0x51C130
HolodiskDescription* gHolodiskDescriptions = nullptr;

// 0x51C134
int gHolodisksCount = 0;

// Number of rest options available.
//
// 0x51C138
int gPipboyRestOptionsCount = PIPBOY_REST_DURATION_COUNT;

// 0x51C13C
bool gPipboyWindowIsoWasEnabled = false;

// 0x51C140
const HolidayDescription gHolidayDescriptions[HOLIDAY_COUNT] = {
    { 1, 1, 100 },
    { 2, 14, 101 },
    { 4, 1, 102 },
    { 7, 4, 104 },
    { 10, 6, 103 },
    { 10, 31, 105 },
    { 11, 28, 106 },
    { 12, 25, 107 },
};

// 0x51C170
PipboyRenderProc* _PipFnctn[5] = {
    pipboyWindowHandleStatus,
    pipboyWindowHandleAutomaps,
    pipboyHandleVideoArchive,
    pipboyHandleAlarmClock,
    pipboyHandleAlarmClock,
};

// 0x664338
MessageListItem gPipboyMessageListItem;

// pipboy.msg
//
// 0x664348
MessageList gPipboyMessageList;

// Selection indices run [0, gHolodisksCount) for vanilla disks and continue past it
// for server disks, so ONE int keeps identifying the selected disk (`_holodisk`).
static bool holodiskIsServer(int disk)
{
    return disk >= gHolodisksCount;
}

static const ServerHolodisk* holodiskServerAt(int disk)
{
    int index = disk - gHolodisksCount;
    if (index < 0 || index >= (int)gServerHolodisks.size()) {
        return nullptr;
    }
    return &gServerHolodisks[index];
}

// ►► ONE LINE ACCESSOR FOR BOTH KINDS, which is the whole trick. Vanilla walks
// consecutive message ids and stops at a "**END-DISK**" entry; a server disk stops at
// the end of its array. Returning nullptr for "past the end" unifies them, so the
// render loops below need no idea which kind they are drawing.
//
// `line` is 0-based within the disk. The 500 bound is vanilla's own sanity limit.
static const char* holodiskLine(int disk, int line)
{
    if (line < 0) {
        return nullptr;
    }
    if (holodiskIsServer(disk)) {
        const ServerHolodisk* serverDisk = holodiskServerAt(disk);
        if (serverDisk == nullptr || line >= (int)serverDisk->lines.size()) {
            return nullptr;
        }
        return serverDisk->lines[line].c_str();
    }
    if (disk < 0 || disk >= gHolodisksCount || line >= 500) {
        return nullptr;
    }
    const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem,
        gHolodiskDescriptions[disk].description + line);
    if (text == nullptr || strcmp(text, "**END-DISK**") == 0) {
        return nullptr;
    }
    return text;
}

static const char* holodiskName(int disk)
{
    if (holodiskIsServer(disk)) {
        const ServerHolodisk* serverDisk = holodiskServerAt(disk);
        return serverDisk != nullptr ? serverDisk->name.c_str() : "";
    }
    if (disk < 0 || disk >= gHolodisksCount) {
        return "";
    }
    return getmsg(&gPipboyMessageList, &gPipboyMessageListItem, gHolodiskDescriptions[disk].name);
}

// 0x664350
STRUCT_664350 _sortlist[24];

// quests.msg
//
// 0x664410
MessageList gQuestsMessageList;

// 0x664418
int gPipboyQuestLocationsCount;

// 0x66441C
unsigned char* gPipboyWindowBuffer;

// 0x66444C
int gPipboyWindowHolodisksCount;

// 0x664450
int gPipboyMouseY;

// 0x664454
int gPipboyMouseX;

// 0x664458
unsigned int gPipboyLastEventTimestamp;

// Index of the last page when rendering holodisk content.
//
// 0x66445C
int gPipboyHolodiskLastPage;

// 0x664460
int _HotLines[22];

// 0x6644B8
int _button;

// 0x6644BC
int gPipboyPreviousMouseX;

// 0x6644C0
int gPipboyPreviousMouseY;

// 0x6644C4
int gPipboyWindow;

// 0x6644F4
int _holodisk;

// 0x6644F8
int gPipboyWindowButtonCount;

// 0x6644FC
int gPipboyWindowOldFont;

// 0x664500
bool _proc_bail_flag;

// 0x664504
int _amlst_mode;

// 0x664508
int gPipboyTab;

// 0x66450C
int _actcnt;

// 0x664510
int gPipboyWindowButtonStart;

// 0x664514
int gPipboyCurrentLine;

// 0x664518

// 0x66451C
int _amcty_indx;

// 0x664520
int _view_page;

// 0x664524
int gPipboyLinesCount;

// 0x664528
unsigned char _hot_back_line;

// 0x664529
unsigned char _holo_flag;

// 0x66452A
unsigned char _stat_flag;

static int gPipboyPrevTab;
static bool pipboy_available_at_game_start = false;

static FrmImage _pipboyFrmImages[PIPBOY_FRM_COUNT];

// 0x497004
int pipboyOpen(int intent)
{
    if (!wmMapPipboyActive() && !pipboy_available_at_game_start) {
        // You aren't wearing the pipboy!
        const char* text = getmsg(&gMiscMessageList, &gPipboyMessageListItem, 7000);
        showDialogBox(text, nullptr, 0, 192, 135, _colorTable[32328], nullptr, _colorTable[32328], 1);
        return 0;
    }

    ScopedGameMode gm(GameMode::kPipboy);

    intent = pipboyWindowInit(intent);
    if (intent == -1) {
        return -1;
    }

    mouseGetPositionInWindow(gPipboyWindow, &gPipboyPreviousMouseX, &gPipboyPreviousMouseY);
    gPipboyLastEventTimestamp = getTicks();

    while (true) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        if (intent == PIPBOY_OPEN_INTENT_REST) {
            keyCode = 504;
            intent = PIPBOY_OPEN_INTENT_UNSPECIFIED;
        }

        mouseGetPositionInWindow(gPipboyWindow, &gPipboyMouseX, &gPipboyMouseY);

        if (keyCode != -1 || gPipboyMouseX != gPipboyPreviousMouseX || gPipboyMouseY != gPipboyPreviousMouseY) {
            gPipboyLastEventTimestamp = getTicks();
            gPipboyPreviousMouseX = gPipboyMouseX;
            gPipboyPreviousMouseY = gPipboyMouseY;
        } else {
            if (getTicks() - gPipboyLastEventTimestamp > PIPBOY_IDLE_TIMEOUT) {
                pipboyRenderScreensaver();

                gPipboyLastEventTimestamp = getTicks();
                mouseGetPositionInWindow(gPipboyWindow, &gPipboyPreviousMouseX, &gPipboyPreviousMouseY);
            }
        }

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
            break;
        }

        // SFALL: Close with 'Z'.
        if (keyCode == 503 || keyCode == KEY_ESCAPE || keyCode == KEY_RETURN || keyCode == KEY_UPPERCASE_P || keyCode == KEY_LOWERCASE_P || keyCode == KEY_UPPERCASE_Z || keyCode == KEY_LOWERCASE_Z || _game_user_wants_to_quit != 0) {
            break;
        }

        if (keyCode == KEY_F12) {
            takeScreenshot();
        } else if (keyCode >= 500 && keyCode <= 504) {
            // CE: Save previous tab selected so that the underlying handlers
            // (alarm clock in particular) can fallback if something goes wrong.
            gPipboyPrevTab = gPipboyTab;

            gPipboyTab = keyCode - 500;
            _PipFnctn[gPipboyTab](1024);
        } else if (keyCode >= 505 && keyCode <= 527) {
            _PipFnctn[gPipboyTab](keyCode - 506);
        } else if (keyCode == 528) {
            _PipFnctn[gPipboyTab](1025);
        } else if (keyCode == KEY_PAGE_DOWN) {
            _PipFnctn[gPipboyTab](1026);
        } else if (keyCode == KEY_PAGE_UP) {
            _PipFnctn[gPipboyTab](1027);
        }

        if (_proc_bail_flag) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    pipboyWindowFree();

    return 0;
}

// 0x497228
static int pipboyWindowInit(int intent)
{
    gPipboyWindowIsoWasEnabled = isoDisable();

    colorCycleDisable();
    gameMouseObjectsHide();
    indicatorBarHide();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    gPipboyRestOptionsCount = PIPBOY_REST_DURATION_COUNT_WITHOUT_PARTY;

    // Offering, not gating: with another player present the rest-until-everyone-is-
    // healed durations are the useful ones.
    if (partyGroupSize() > 1 && partyIsAnyoneCanBeHealedByRest()) {
        gPipboyRestOptionsCount = PIPBOY_REST_DURATION_COUNT;
    }

    gPipboyWindowOldFont = fontGetCurrent();
    fontSetCurrent(101);

    _proc_bail_flag = 0;
    restHealReset();
    gPipboyCurrentLine = 0;
    gPipboyWindowButtonCount = 0;
    gPipboyLinesCount = PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT / fontGetLineHeight() - 1;
    gPipboyWindowButtonStart = 0;
    _hot_back_line = 0;

    if (holodiskInit() == -1) {
        return -1;
    }

    if (!messageListInit(&gPipboyMessageList)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "pipboy.msg");

    if (!(messageListLoad(&gPipboyMessageList, path))) {
        return -1;
    }

    int index;
    for (index = 0; index < PIPBOY_FRM_COUNT; index++) {
        int fid = buildFid(OBJ_TYPE_INTERFACE, gPipboyFrmIds[index], 0, 0, 0);
        if (!_pipboyFrmImages[index].lock(fid)) {
            break;
        }
    }

    if (index != PIPBOY_FRM_COUNT) {
        debugPrint("\n** Error loading pipboy graphics! **\n");

        while (--index >= 0) {
            _pipboyFrmImages[index].unlock();
        }

        return -1;
    }

    int pipboyWindowX = (screenGetWidth() - PIPBOY_WINDOW_WIDTH) / 2;
    int pipboyWindowY = (screenGetHeight() - PIPBOY_WINDOW_HEIGHT) / 2;
    gPipboyWindow = windowCreate(pipboyWindowX, pipboyWindowY, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_HEIGHT, _colorTable[0], WINDOW_MODAL);
    if (gPipboyWindow == -1) {
        debugPrint("\n** Error opening pipboy window! **\n");
        for (int index = 0; index < PIPBOY_FRM_COUNT; index++) {
            _pipboyFrmImages[index].unlock();
        }
        return -1;
    }

    gPipboyWindowBuffer = windowGetBuffer(gPipboyWindow);
    memcpy(gPipboyWindowBuffer, _pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData(), PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_HEIGHT);

    pipboyDrawNumber(gameTimeGetHour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
    pipboyDrawDate();

    int alarmButton = buttonCreate(gPipboyWindow,
        124,
        13,
        _pipboyFrmImages[PIPBOY_FRM_ALARM_UP].getWidth(),
        _pipboyFrmImages[PIPBOY_FRM_ALARM_UP].getHeight(),
        -1,
        -1,
        -1,
        504,
        _pipboyFrmImages[PIPBOY_FRM_ALARM_UP].getData(),
        _pipboyFrmImages[PIPBOY_FRM_ALARM_DOWN].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (alarmButton != -1) {
        buttonSetCallbacks(alarmButton, _gsound_med_butt_press, _gsound_med_butt_release);
    }

    int y = 341;
    int eventCode = 500;
    for (int index = 0; index < 5; index += 1) {
        if (index != 1) {
            int btn = buttonCreate(gPipboyWindow,
                53,
                y,
                _pipboyFrmImages[PIPBOY_FRM_LITTLE_RED_BUTTON_UP].getWidth(),
                _pipboyFrmImages[PIPBOY_FRM_LITTLE_RED_BUTTON_UP].getHeight(),
                -1,
                -1,
                -1,
                eventCode,
                _pipboyFrmImages[PIPBOY_FRM_LITTLE_RED_BUTTON_UP].getData(),
                _pipboyFrmImages[PIPBOY_FRM_LITTLE_RED_BUTTON_DOWN].getData(),
                nullptr,
                BUTTON_FLAG_TRANSPARENT);
            if (btn != -1) {
                buttonSetCallbacks(btn, _gsound_red_butt_press, _gsound_red_butt_release);
            }

            eventCode += 1;
        }

        y += 27;
    }

    if (intent == PIPBOY_OPEN_INTENT_REST) {
        if (!_critter_can_obj_dude_rest()) {
            blitBufferToBufferTrans(
                _pipboyFrmImages[PIPBOY_FRM_LOGO].getData(),
                _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth(),
                _pipboyFrmImages[PIPBOY_FRM_LOGO].getHeight(),
                _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth(),
                gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 156 + 323,
                PIPBOY_WINDOW_WIDTH);

            int month;
            int day;
            int year;
            gameTimeGetDate(&month, &day, &year);

            int holiday = 0;
            for (; holiday < HOLIDAY_COUNT; holiday += 1) {
                const HolidayDescription* holidayDescription = &(gHolidayDescriptions[holiday]);
                if (holidayDescription->month == month && holidayDescription->day == day) {
                    break;
                }
            }

            if (holiday != HOLIDAY_COUNT) {
                const HolidayDescription* holidayDescription = &(gHolidayDescriptions[holiday]);
                const char* holidayName = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, holidayDescription->textId);
                char holidayNameCopy[256];
                strcpy(holidayNameCopy, holidayName);

                int len = fontGetStringWidth(holidayNameCopy);
                fontDrawText(gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * (_pipboyFrmImages[PIPBOY_FRM_LOGO].getHeight() + 174) + 6 + _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth() / 2 + 323 - len / 2,
                    holidayNameCopy,
                    350,
                    PIPBOY_WINDOW_WIDTH,
                    _colorTable[992]);
            }

            windowRefresh(gPipboyWindow);

            soundPlayFile("iisxxxx1");

            const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 215);
            showDialogBox(text, nullptr, 0, 192, 135, _colorTable[32328], nullptr, _colorTable[32328], DIALOG_BOX_LARGE);

            intent = PIPBOY_OPEN_INTENT_UNSPECIFIED;
        }
    } else {
        blitBufferToBufferTrans(
            _pipboyFrmImages[PIPBOY_FRM_LOGO].getData(),
            _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth(),
            _pipboyFrmImages[PIPBOY_FRM_LOGO].getHeight(),
            _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth(),
            gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 156 + 323,
            PIPBOY_WINDOW_WIDTH);

        int month;
        int day;
        int year;
        gameTimeGetDate(&month, &day, &year);

        int holiday;
        for (holiday = 0; holiday < HOLIDAY_COUNT; holiday += 1) {
            const HolidayDescription* holidayDescription = &(gHolidayDescriptions[holiday]);
            if (holidayDescription->month == month && holidayDescription->day == day) {
                break;
            }
        }

        if (holiday != HOLIDAY_COUNT) {
            const HolidayDescription* holidayDescription = &(gHolidayDescriptions[holiday]);
            const char* holidayName = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, holidayDescription->textId);
            char holidayNameCopy[256];
            strcpy(holidayNameCopy, holidayName);

            int length = fontGetStringWidth(holidayNameCopy);
            fontDrawText(gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * (_pipboyFrmImages[PIPBOY_FRM_LOGO].getHeight() + 174) + 6 + _pipboyFrmImages[PIPBOY_FRM_LOGO].getWidth() / 2 + 323 - length / 2,
                holidayNameCopy,
                350,
                PIPBOY_WINDOW_WIDTH,
                _colorTable[992]);
        }

        windowRefresh(gPipboyWindow);
    }

    if (questInit() == -1) {
        return -1;
    }

    soundPlayFile("pipon");
    windowRefresh(gPipboyWindow);

    return intent;
}

// 0x497828
static void pipboyWindowFree()
{
    if (settings.debug.show_script_messages) {
        debugPrint("\nScript <Map Update>");
    }

    scriptsExecMapUpdateProc();

    windowDestroy(gPipboyWindow);

    messageListFree(&gPipboyMessageList);

    // NOTE: Uninline.
    holodiskFree();

    for (int index = 0; index < PIPBOY_FRM_COUNT; index++) {
        _pipboyFrmImages[index].unlock();
    }

    pipboyWindowDestroyButtons();

    fontSetCurrent(gPipboyWindowOldFont);

    if (gPipboyWindowIsoWasEnabled) {
        isoEnable();
    }

    colorCycleEnable();
    indicatorBarShow();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    interfaceBarRefresh();

    // NOTE: Uninline.
    questFree();
}

// NOTE: Collapsed.
//
// 0x497918
static void _pip_init_()
{
    // SFALL: Make the pipboy available at the start of the game.
    // CE: The implementation is slightly different. SFALL has two values for
    // making the pipboy available at the start of the game. When the option is
    // set to (1), the `MOVIE_VSUIT` is automatically marked as viewed (the suit
    // grants the pipboy, see `wmMapPipboyActive`). Doing so exposes that movie
    // in the "Video Archives" section of the pipboy, which is likely an
    // undesired side effect. When the option is set to (2), the check is simply
    // bypassed. CE implements only the latter approach, as it does not have any
    // side effects.
    int value = 0;
    if (configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_PIPBOY_AVAILABLE_AT_GAMESTART, &value)) {
        pipboy_available_at_game_start = value == 1 || value == 2;
    }
}

// NOTE: Uncollapsed 0x497918.
//
// pip_init
void pipboyInit()
{
    _pip_init_();
}

// NOTE: Uncollapsed 0x497918.
void pipboyReset()
{
    _pip_init_();
}

// 0x49791C
static void pipboyDrawNumber(int value, int digits, int x, int y)
{
    int offset = PIPBOY_WINDOW_WIDTH * y + x + 9 * (digits - 1);

    for (int index = 0; index < digits; index++) {
        blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_NUMBERS].getData() + 9 * (value % 10), 9, 17, 360, gPipboyWindowBuffer + offset, PIPBOY_WINDOW_WIDTH);
        offset -= 9;
        value /= 10;
    }
}

// 0x4979B4
static void pipboyDrawDate()
{
    int day;
    int month;
    int year;

    gameTimeGetDate(&month, &day, &year);
    pipboyDrawNumber(day, 2, PIPBOY_WINDOW_DAY_X, PIPBOY_WINDOW_DAY_Y);

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_MONTHS].getData() + 435 * (month - 1), 29, 14, 29, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_MONTH_Y + PIPBOY_WINDOW_MONTH_X, PIPBOY_WINDOW_WIDTH);

    pipboyDrawNumber(year, 4, PIPBOY_WINDOW_YEAR_X, PIPBOY_WINDOW_YEAR_Y);
}

// 0x497A40
static void pipboyDrawText(const char* text, int flags, int color)
{
    if ((flags & PIPBOY_TEXT_STYLE_UNDERLINE) != 0) {
        color |= FONT_UNDERLINE;
    }

    int left = 8;
    if ((flags & PIPBOY_TEXT_NO_INDENT) != 0) {
        left -= 7;
    }

    int length = fontGetStringWidth(text);

    if ((flags & PIPBOY_TEXT_ALIGNMENT_CENTER) != 0) {
        left = (350 - length) / 2;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN) != 0) {
        left += 175;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER) != 0) {
        left += 86 - length + 16;
    } else if ((flags & PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER) != 0) {
        left += 260 - length;
    }

    fontDrawText(gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * (gPipboyCurrentLine * fontGetLineHeight() + PIPBOY_WINDOW_CONTENT_VIEW_Y) + PIPBOY_WINDOW_CONTENT_VIEW_X + left, text, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_WIDTH, color);

    if ((flags & PIPBOY_TEXT_STYLE_STRIKE_THROUGH) != 0) {
        int top = gPipboyCurrentLine * fontGetLineHeight() + 49;
        bufferDrawLine(gPipboyWindowBuffer, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_CONTENT_VIEW_X + left, top, PIPBOY_WINDOW_CONTENT_VIEW_X + left + length, top, color);
    }

    if (gPipboyCurrentLine < gPipboyLinesCount) {
        gPipboyCurrentLine += 1;
    }
}

// 0x497B64
static void pipboyDrawBackButton(int color)
{
    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = gPipboyLinesCount;
    }

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

    // BACK
    const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
    pipboyDrawText(text, PIPBOY_TEXT_ALIGNMENT_CENTER, color);
}

// NOTE: Collapsed.
//
// 0x497BD4
static int _save_pipboy(File* stream)
{
    return 0;
}

// NOTE: Uncollapsed 0x497BD4.
int pipboySave(File* stream)
{
    return _save_pipboy(stream);
}

// NOTE: Uncollapsed 0x497BD4.
int pipboyLoad(File* stream)
{
    return _save_pipboy(stream);
}

// 0x497BD8
static void pipboyWindowHandleStatus(int a1)
{
    if (a1 == 1024) {
        pipboyWindowDestroyButtons();
        blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);
        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = 0;
        }

        _holo_flag = 0;
        _holodisk = -1;
        gPipboyWindowHolodisksCount = 0;
        _view_page = 0;
        _stat_flag = 0;

        for (int index = 0; index < gHolodisksCount; index += 1) {
            HolodiskDescription* holodiskDescription = &(gHolodiskDescriptions[index]);
            if (gGameGlobalVars[holodiskDescription->gvar] != 0) {
                gPipboyWindowHolodisksCount += 1;
                break;
            }
        }

        pipboyWindowRenderQuestLocationList(-1);

        if (gPipboyQuestLocationsCount == 0) {
            const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 203);
            pipboyDrawText(text, 0, _colorTable[992]);
        }

        gPipboyWindowHolodisksCount = pipboyWindowRenderHolodiskList(-1);

        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        pipboyWindowCreateButtons(2, gPipboyQuestLocationsCount + gPipboyWindowHolodisksCount + 1, false);
        windowRefresh(gPipboyWindow);
        return;
    }

    if (_stat_flag == 0 && _holo_flag == 0) {
        if (gPipboyQuestLocationsCount != 0 && gPipboyMouseX < 429) {
            soundPlayFile("ib1p1xx1");
            blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                PIPBOY_WINDOW_WIDTH,
                gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_WIDTH);
            pipboyWindowRenderQuestLocationList(a1);
            pipboyWindowRenderHolodiskList(-1);
            windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
            inputPauseForTocks(200);
            _stat_flag = 1;
        } else {
            if (gPipboyWindowHolodisksCount != 0 && gPipboyWindowHolodisksCount >= a1 && gPipboyMouseX > 429) {
                soundPlayFile("ib1p1xx1");
                _holodisk = 0;

                // Walk the VISIBLE disks counting matches against the clicked row, then
                // keep walking into the server disks — one index space, vanilla first
                // (see holodiskIsServer). `_holodisk` ends as the absolute disk index.
                int index = 0;
                for (; index < gHolodisksCount; index += 1) {
                    HolodiskDescription* holodiskDescription = &(gHolodiskDescriptions[index]);
                    if (gGameGlobalVars[holodiskDescription->gvar] > 0) {
                        if (a1 - 1 == _holodisk) {
                            break;
                        }
                        _holodisk += 1;
                    }
                }
                if (index >= gHolodisksCount) {
                    // Not one of the vanilla disks — it is the (a1-1 - visibleVanilla)-th
                    // server disk. Every server disk is visible, so the remainder indexes
                    // them directly.
                    int serverIndex = (a1 - 1) - _holodisk;
                    if (serverIndex >= 0 && serverIndex < (int)gServerHolodisks.size()) {
                        index = gHolodisksCount + serverIndex;
                    }
                }
                _holodisk = index;

                blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                    PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                    PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                    PIPBOY_WINDOW_WIDTH,
                    gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                    PIPBOY_WINDOW_WIDTH);
                // CE: Fix highlighting holodisk (on par with quest highlighting
                // approach).
                pipboyWindowRenderHolodiskList(a1);
                pipboyWindowRenderQuestLocationList(-1);
                windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
                inputPauseForTocks(200);
                pipboyWindowDestroyButtons();
                pipboyRenderHolodiskText();
                pipboyWindowCreateButtons(0, 0, true);
                _holo_flag = 1;
            }
        }
    }

    if (_stat_flag == 0) {
        if (_holo_flag == 0 || a1 < 1025 || a1 > 1027) {
            return;
        }

        if ((gPipboyMouseX > 459 && a1 != 1027) || a1 == 1026) {
            if (gPipboyHolodiskLastPage <= _view_page) {
                if (a1 != 1026) {
                    soundPlayFile("ib1p1xx1");
                    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

                    if (gPipboyLinesCount >= 0) {
                        gPipboyCurrentLine = gPipboyLinesCount;
                    }

                    // Back
                    const char* text1 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
                    pipboyDrawText(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

                    if (gPipboyLinesCount >= 0) {
                        gPipboyCurrentLine = gPipboyLinesCount;
                    }

                    // Done
                    const char* text2 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 214);
                    pipboyDrawText(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, _colorTable[992]);

                    windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
                    inputPauseForTocks(200);
                    pipboyWindowHandleStatus(1024);
                }
            } else {
                soundPlayFile("ib1p1xx1");
                blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

                if (gPipboyLinesCount >= 0) {
                    gPipboyCurrentLine = gPipboyLinesCount;
                }

                // Back
                const char* text1 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
                pipboyDrawText(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

                if (gPipboyLinesCount >= 0) {
                    gPipboyCurrentLine = gPipboyLinesCount;
                }

                // More
                const char* text2 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 200);
                pipboyDrawText(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, _colorTable[992]);

                windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
                inputPauseForTocks(200);

                _view_page += 1;

                pipboyRenderHolodiskText();
            }
            return;
        }

        if (a1 == 1027) {
            soundPlayFile("ib1p1xx1");
            blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

            if (gPipboyLinesCount >= 0) {
                gPipboyCurrentLine = gPipboyLinesCount;
            }

            // Back
            const char* text1 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
            pipboyDrawText(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

            if (gPipboyLinesCount >= 0) {
                gPipboyCurrentLine = gPipboyLinesCount;
            }

            // More
            const char* text2 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 200);
            pipboyDrawText(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, _colorTable[992]);

            windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
            inputPauseForTocks(200);

            _view_page -= 1;

            if (_view_page < 0) {
                pipboyWindowHandleStatus(1024);
                return;
            }
        } else {
            if (gPipboyMouseX > 395) {
                return;
            }

            soundPlayFile("ib1p1xx1");
            blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * 436 + 254, 350, 20, PIPBOY_WINDOW_WIDTH, gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 436 + 254, PIPBOY_WINDOW_WIDTH);

            if (gPipboyLinesCount >= 0) {
                gPipboyCurrentLine = gPipboyLinesCount;
            }

            // Back
            const char* text1 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
            pipboyDrawText(text1, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

            if (gPipboyLinesCount >= 0) {
                gPipboyCurrentLine = gPipboyLinesCount;
            }

            // More
            const char* text2 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 200);
            pipboyDrawText(text2, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, _colorTable[992]);

            windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
            inputPauseForTocks(200);

            if (_view_page <= 0) {
                pipboyWindowHandleStatus(1024);
                return;
            }

            _view_page -= 1;
        }

        pipboyRenderHolodiskText();
        return;
    }

    if (a1 == 1025) {
        soundPlayFile("ib1p1xx1");
        pipboyDrawBackButton(_colorTable[32747]);
        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        inputPauseForTocks(200);
        pipboyWindowHandleStatus(1024);
    }

    if (a1 <= gPipboyQuestLocationsCount) {
        soundPlayFile("ib1p1xx1");

        int v13 = 0;
        int index = 0;
        for (; index < gQuestsCount; index++) {
            QuestDescription* questDescription = &(gQuestDescriptions[index]);
            if (questDescription->displayThreshold <= gGameGlobalVars[questDescription->gvar]) {
                if (v13 == a1 - 1) {
                    break;
                }

                v13 += 1;

                // Skip quests in the same location.
                //
                // FIXME: This code should be identical to the one in the
                // `pipboyWindowRenderQuestLocationList`. See buffer overread
                // bug involved.
                for (; index < gQuestsCount; index++) {
                    if (gQuestDescriptions[index].location != gQuestDescriptions[index + 1].location) {
                        break;
                    }
                }
            }
        }

        pipboyWindowDestroyButtons();
        blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);
        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = 0;
        }

        if (gPipboyLinesCount >= 1) {
            gPipboyCurrentLine = 1;
        }

        pipboyWindowCreateButtons(0, 0, true);

        QuestDescription* questDescription = &(gQuestDescriptions[index]);

        const char* text1 = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 210);
        const char* text2 = getmsg(&gMapMessageList, &gPipboyMessageListItem, questDescription->location);
        char formattedText[1024];
        snprintf(formattedText, sizeof(formattedText), "%s %s", text2, text1);
        pipboyDrawText(formattedText, PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

        if (gPipboyLinesCount >= 3) {
            gPipboyCurrentLine = 3;
        }

        int number = 1;
        for (; index < gQuestsCount; index++) {
            QuestDescription* questDescription = &(gQuestDescriptions[index]);
            if (gGameGlobalVars[questDescription->gvar] >= questDescription->displayThreshold) {
                const char* text = getmsg(&gQuestsMessageList, &gPipboyMessageListItem, questDescription->description);
                char formattedText[1024];
                snprintf(formattedText, sizeof(formattedText), "%d. %s", number, text);
                number += 1;

                short beginnings[WORD_WRAP_MAX_COUNT];
                short count;
                if (wordWrap(formattedText, 350, beginnings, &count) == 0) {
                    for (int line = 0; line < count - 1; line += 1) {
                        char* beginning = formattedText + beginnings[line];
                        char* ending = formattedText + beginnings[line + 1];
                        char c = *ending;
                        *ending = '\0';

                        int flags;
                        int color;
                        if (gGameGlobalVars[questDescription->gvar] < questDescription->completedThreshold) {
                            flags = 0;
                            color = _colorTable[992];
                        } else {
                            flags = PIPBOY_TEXT_STYLE_STRIKE_THROUGH;
                            color = _colorTable[8804];
                        }

                        pipboyDrawText(beginning, flags, color);

                        *ending = c;
                        gPipboyCurrentLine += 1;
                    }
                } else {
                    debugPrint("\n ** Word wrap error in pipboy! **\n");
                }
            }

            if (index != gQuestsCount - 1) {
                QuestDescription* nextQuestDescription = &(gQuestDescriptions[index + 1]);
                if (questDescription->location != nextQuestDescription->location) {
                    break;
                }
            }
        }

        pipboyDrawBackButton(_colorTable[992]);
        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        _stat_flag = 1;
    }
}

// [a1] is likely selected location, or -1 if nothing is selected
//
// 0x498734
static void pipboyWindowRenderQuestLocationList(int a1)
{
    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = 0;
    }

    int flags = gPipboyWindowHolodisksCount != 0 ? PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER : PIPBOY_TEXT_ALIGNMENT_CENTER;
    flags |= PIPBOY_TEXT_STYLE_UNDERLINE;

    // STATUS
    const char* statusText = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 202);
    pipboyDrawText(statusText, flags, _colorTable[992]);

    if (gPipboyLinesCount >= 2) {
        gPipboyCurrentLine = 2;
    }

    gPipboyQuestLocationsCount = 0;

    for (int index = 0; index < gQuestsCount; index += 1) {
        QuestDescription* quest = &(gQuestDescriptions[index]);
        if (quest->displayThreshold > gGameGlobalVars[quest->gvar]) {
            continue;
        }

        int color = (gPipboyCurrentLine - 1) / 2 == (a1 - 1) ? _colorTable[32747] : _colorTable[992];

        // Render location.
        const char* questLocation = getmsg(&gMapMessageList, &gPipboyMessageListItem, quest->location);
        pipboyDrawText(questLocation, 0, color);

        gPipboyCurrentLine += 1;
        gPipboyQuestLocationsCount += 1;

        // Skip quests in the same location.
        //
        // FIXME: There is a buffer overread bug at the end of the loop. It does
        // not manifest because dynamically allocated memory blocks have special
        // footer guard. Location field is the first in the struct and matches
        // size of the guard. So on the final iteration it compares location of
        // the last quest with this special guard (0xBEEFCAFE).
        for (; index < gQuestsCount; index++) {
            if (gQuestDescriptions[index].location != gQuestDescriptions[index + 1].location) {
                break;
            }
        }
    }
}

// 0x4988A0
static void pipboyRenderHolodiskText()
{
    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = 0;
    }

    // ►► LINE INDEX, not a message id. holodiskLine() serves the selected disk's lines
    // from EITHER pipboy.msg (vanilla: consecutive ids ending at "**END-DISK**") or a
    // streamed server disk, returning nullptr past the end — so these three walks no
    // longer care which kind is open. `line` replaces vanilla's holodiskTextId; the
    // arithmetic is otherwise unchanged.
    int line = 0;
    int linesCount = 0;

    gPipboyHolodiskLastPage = 0;

    for (line = 0; holodiskLine(_holodisk, line) != nullptr; line += 1) {
        linesCount += 1;
        if (linesCount >= PIPBOY_HOLODISK_LINES_MAX) {
            linesCount = 0;
            gPipboyHolodiskLastPage += 1;
        }
    }

    if (line >= 500) {
        debugPrint("\nPIPBOY: #1 Holodisk text end not found!\n");
    }

    line = 0;

    if (_view_page != 0) {
        int page = 0;
        int numberOfLines = 0;
        // Vanilla's loop condition here was `holodiskTextId < holodiskTextId + 500` — a
        // self-comparison that is always true, i.e. it relied entirely on the breaks. The
        // accessor's nullptr end bounds it properly; behaviour only differs for a
        // malformed disk that never terminates, which vanilla would have run off.
        for (; ; line += 1) {
            if (holodiskLine(_holodisk, line) == nullptr) {
                debugPrint("\nPIPBOY: Premature page end in holodisk page search!\n");
                break;
            }

            numberOfLines += 1;
            if (numberOfLines >= PIPBOY_HOLODISK_LINES_MAX) {
                page += 1;
                if (page >= _view_page) {
                    break;
                }

                numberOfLines = 0;
            }
        }

        line += 1;
    } else {
        pipboyDrawText(holodiskName(_holodisk), PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);
    }

    if (gPipboyHolodiskLastPage != 0) {
        // of
        const char* of = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 212);
        char formattedText[60]; // TODO: Size is probably wrong.
        snprintf(formattedText, sizeof(formattedText), "%d %s %d", _view_page + 1, of, gPipboyHolodiskLastPage + 1);

        int len = fontGetStringWidth(of);
        fontDrawText(gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * 47 + 616 + 604 - len, formattedText, 350, PIPBOY_WINDOW_WIDTH, _colorTable[992]);
    }

    if (gPipboyLinesCount >= 3) {
        gPipboyCurrentLine = 3;
    }

    for (int drawn = 0; drawn < PIPBOY_HOLODISK_LINES_MAX; drawn += 1) {
        const char* text = holodiskLine(_holodisk, line);
        if (text == nullptr) {
            break; // end of disk
        }

        if (strcmp(text, "**END-PAR**") == 0) {
            gPipboyCurrentLine += 1;
        } else {
            pipboyDrawText(text, PIPBOY_TEXT_NO_INDENT, _colorTable[992]);
        }

        line += 1;
    }

    int moreOrDoneTextId;
    if (gPipboyHolodiskLastPage <= _view_page) {
        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = gPipboyLinesCount;
        }

        const char* back = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
        pipboyDrawText(back, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = gPipboyLinesCount;
        }

        moreOrDoneTextId = 214;
    } else {
        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = gPipboyLinesCount;
        }

        const char* back = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 201);
        pipboyDrawText(back, PIPBOY_TEXT_ALIGNMENT_LEFT_COLUMN_CENTER, _colorTable[992]);

        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = gPipboyLinesCount;
        }

        moreOrDoneTextId = 200;
    }

    const char* moreOrDoneText = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, moreOrDoneTextId);
    pipboyDrawText(moreOrDoneText, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER, _colorTable[992]);
    windowRefresh(gPipboyWindow);
}

// 0x498C40
static int pipboyWindowRenderHolodiskList(int a1)
{
    if (gPipboyLinesCount >= 2) {
        gPipboyCurrentLine = 2;
    }

    int knownHolodisksCount = 0;
    for (int index = 0; index < gHolodisksCount; index++) {
        HolodiskDescription* holodisk = &(gHolodiskDescriptions[index]);
        if (gGameGlobalVars[holodisk->gvar] != 0) {
            int color;
            // CE: Fix highlighting holodisk (on par with quest highlighting
            // approach).
            if ((gPipboyCurrentLine - 1) / 2 == a1 - 1) {
                color = _colorTable[32747];
            } else {
                color = _colorTable[992];
            }

            const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, holodisk->name);
            pipboyDrawText(text, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN, color);

            gPipboyCurrentLine++;
            knownHolodisksCount++;
        }
    }

    // Server-authored disks, after the vanilla ones and ALWAYS shown: a vanilla disk is
    // gated on its gvar ("have you found it"), and a server disk has none — the server
    // announcing it IS the grant. Highlight arithmetic matches the loop above exactly, so
    // hover works the same on both kinds.
    for (int index = 0; index < (int)gServerHolodisks.size(); index++) {
        int color = ((gPipboyCurrentLine - 1) / 2 == a1 - 1) ? _colorTable[32747] : _colorTable[992];
        pipboyDrawText(gServerHolodisks[index].name.c_str(), PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN, color);
        gPipboyCurrentLine++;
        knownHolodisksCount++;
    }

    if (knownHolodisksCount != 0) {
        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = 0;
        }

        const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 211); // DATA
        pipboyDrawText(text, PIPBOY_TEXT_ALIGNMENT_RIGHT_COLUMN_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);
    }

    return knownHolodisksCount;
}

// 0x498D34
static int _qscmp(const void* a1, const void* a2)
{
    STRUCT_664350* v1 = (STRUCT_664350*)a1;
    STRUCT_664350* v2 = (STRUCT_664350*)a2;

    return strcmp(v1->name, v2->name);
}

// 0x498D40
static void pipboyWindowHandleAutomaps(int a1)
{
    if (a1 == 1024) {
        pipboyWindowDestroyButtons();
        blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);

        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = 0;
        }

        const char* title = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 205);
        pipboyDrawText(title, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

        _actcnt = _PrintAMList(-1);

        pipboyWindowCreateButtons(2, _actcnt, 0);

        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        _amlst_mode = 0;
        return;
    }

    if (_amlst_mode != 0) {
        if (a1 == 1025 || a1 <= -1) {
            pipboyWindowHandleAutomaps(1024);
            soundPlayFile("ib1p1xx1");
        }

        if (a1 >= 1 && a1 <= _actcnt + 3) {
            soundPlayFile("ib1p1xx1");
            _PrintAMelevList(a1);
            automapRenderInPipboyWindow(gPipboyWindow, _sortlist[a1 - 1].field_6, _sortlist[a1 - 1].field_4);
            windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        }

        return;
    }

    if (a1 > 0 && a1 <= _actcnt) {
        soundPlayFile("ib1p1xx1");
        pipboyWindowDestroyButtons();
        _PrintAMList(a1);
        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        _amcty_indx = _sortlist[a1 - 1].field_4;
        _actcnt = _PrintAMelevList(1);
        pipboyWindowCreateButtons(0, _actcnt + 2, 1);
        automapRenderInPipboyWindow(gPipboyWindow, _sortlist[0].field_6, _sortlist[0].field_4);
        windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
        _amlst_mode = 1;
    }
}

// 0x498F30
static int _PrintAMelevList(int a1)
{
    AutomapHeader* automapHeader;
    if (automapGetHeader(&automapHeader) == -1) {
        return -1;
    }

    int v4 = 0;
    for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
        if (automapHeader->offsets[_amcty_indx][elevation] > 0) {
            _sortlist[v4].name = mapGetName(_amcty_indx, elevation);
            _sortlist[v4].field_4 = elevation;
            _sortlist[v4].field_6 = _amcty_indx;
            v4++;
        }
    }

    int mapCount = wmMapMaxCount();
    for (int map = 0; map < mapCount; map++) {
        if (map == _amcty_indx) {
            continue;
        }

        if (_get_map_idx_same(_amcty_indx, map) == -1) {
            continue;
        }

        for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            if (automapHeader->offsets[map][elevation] > 0) {
                _sortlist[v4].name = mapGetName(map, elevation);
                _sortlist[v4].field_4 = elevation;
                _sortlist[v4].field_6 = map;
                v4++;
            }
        }
    }

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = 0;
    }

    const char* msg = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 205);
    pipboyDrawText(msg, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

    if (gPipboyLinesCount >= 2) {
        gPipboyCurrentLine = 2;
    }

    const char* name = _map_get_description_idx_(_amcty_indx);
    pipboyDrawText(name, PIPBOY_TEXT_ALIGNMENT_CENTER, _colorTable[992]);

    if (gPipboyLinesCount >= 4) {
        gPipboyCurrentLine = 4;
    }

    int selectedPipboyLine = (a1 - 1) * 2;

    for (int index = 0; index < v4; index++) {
        int color;
        if (gPipboyCurrentLine - 4 == selectedPipboyLine) {
            color = _colorTable[32747];
        } else {
            color = _colorTable[992];
        }

        pipboyDrawText(_sortlist[index].name, 0, color);
        gPipboyCurrentLine++;
    }

    pipboyDrawBackButton(_colorTable[992]);

    return v4;
}

// 0x499150
static int _PrintAMList(int a1)
{
    AutomapHeader* automapHeader;
    if (automapGetHeader(&automapHeader) == -1) {
        return -1;
    }

    int count = 0;
    int index = 0;

    int mapCount = wmMapMaxCount();
    for (int map = 0; map < mapCount; map++) {
        int elevation;
        for (elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            if (automapHeader->offsets[map][elevation] > 0) {
                if (_automapDisplayMap(map) == 0) {
                    break;
                }
            }
        }

        if (elevation < ELEVATION_COUNT) {
            int v7;
            if (count != 0) {
                v7 = 0;
                for (int index = 0; index < count; index++) {
                    if (_is_map_idx_same(map, _sortlist[index].field_4)) {
                        break;
                    }

                    v7++;
                }
            } else {
                v7 = 0;
            }

            if (v7 == count) {
                _sortlist[count].name = mapGetCityName(map);
                _sortlist[count].field_4 = map;
                count++;
            }
        }
    }

    if (count != 0) {
        if (count > 1) {
            qsort(_sortlist, count, sizeof(*_sortlist), _qscmp);
        }

        blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
            PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
            PIPBOY_WINDOW_WIDTH,
            gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
            PIPBOY_WINDOW_WIDTH);

        if (gPipboyLinesCount >= 0) {
            gPipboyCurrentLine = 0;
        }

        const char* msg = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 205);
        pipboyDrawText(msg, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

        if (gPipboyLinesCount >= 2) {
            gPipboyCurrentLine = 2;
        }

        for (int index = 0; index < count; index++) {
            int color;
            if (gPipboyCurrentLine - 1 == a1) {
                color = _colorTable[32747];
            } else {
                color = _colorTable[992];
            }

            pipboyDrawText(_sortlist[index].name, 0, color);
            gPipboyCurrentLine++;
        }
    }

    return count;
}

// 0x49932C
static void pipboyHandleVideoArchive(int a1)
{
    if (a1 == 1024) {
        pipboyWindowDestroyButtons();
        _view_page = pipboyRenderVideoArchive(-1);
        pipboyWindowCreateButtons(2, _view_page, false);
    } else if (a1 >= 0 && a1 <= _view_page) {
        soundPlayFile("ib1p1xx1");

        pipboyRenderVideoArchive(a1);

        int movie;
        for (movie = 2; movie < 16; movie++) {
            if (gameMovieIsSeen(movie)) {
                a1--;
                if (a1 <= 0) {
                    break;
                }
            }
        }

        if (movie <= MOVIE_COUNT) {
            gameMoviePlay(movie, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
        } else {
            debugPrint("\n ** Selected movie not found in list! **\n");
        }

        fontSetCurrent(101);

        gPipboyLastEventTimestamp = getTicks();
        pipboyRenderVideoArchive(-1);
    }
}

// 0x4993DC
static int pipboyRenderVideoArchive(int a1)
{
    const char* text;
    int i;
    int v12;
    int msg_num;
    int v5;
    int v8;
    int v9;

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = 0;
    }

    // VIDEO ARCHIVES
    text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 206);
    pipboyDrawText(text, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

    if (gPipboyLinesCount >= 2) {
        gPipboyCurrentLine = 2;
    }

    v5 = 0;
    v12 = a1 - 1;

    // 502 - Elder Speech
    // ...
    // 516 - Credits
    msg_num = 502;

    for (i = 2; i < 16; i++) {
        if (gameMovieIsSeen(i)) {
            v8 = v5++;
            if (v8 == v12) {
                v9 = _colorTable[32747];
            } else {
                v9 = _colorTable[992];
            }

            text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, msg_num);
            pipboyDrawText(text, 0, v9);

            gPipboyCurrentLine++;
        }

        msg_num++;
    }

    windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);

    return v5;
}

// 0x499518
static void pipboyHandleAlarmClock(int eventCode)
{
    if (eventCode == 1024) {
        // ►► A VIEWER MAY NOT REST LOCALLY — the rest sim advances gameTime and heals
        // everyone, and the server owns both. But it may now ASK: the options menu opens
        // as normal and picking one sends `restopt`, which the server runs
        // authoritatively (see the option handler below). It used to be routed into
        // vanilla's own "You cannot rest at this location!" refusal, which was honest but
        // meant co-op simply had no way to pass time or heal on purpose.
        //
        // The can-rest gate still applies locally so an impossible location is answered
        // instantly, exactly as in single-player; the server re-checks it anyway, because
        // a client's answer to "may I" is never the authority's.
        if (_critter_can_obj_dude_rest()) {
            pipboyWindowDestroyButtons();
            pipboyWindowRenderRestOptions(0);
            pipboyWindowCreateButtons(5, gPipboyRestOptionsCount, false);
        } else {
            soundPlayFile("iisxxxx1");

            // You cannot rest at this location!
            const char* text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 215);
            showDialogBox(text, nullptr, 0, 192, 135, _colorTable[32328], nullptr, _colorTable[32328], DIALOG_BOX_LARGE);

            // CE: Restore previous tab to make sure clicks are processed by
            // appropriate handler (not the alarm clock).
            gPipboyTab = gPipboyPrevTab;
        }
    } else if (eventCode >= 4 && eventCode <= 17) {
        soundPlayFile("ib1p1xx1");

        pipboyWindowRenderRestOptions(eventCode - 3);

        int duration = eventCode - 4;
        int minutes = 0;
        int hours = 0;
        int kind = 0;

        // Ledger H-42: rest-intent decode (the rest-duration tables and the
        // until-hour wall-clock math) extracted to core; the pipboy keeps
        // the until-midnight midnight-render quirk.
        restOptionDecode(duration, &hours, &minutes, &kind);

        if (clientViewerActive()) {
            // CO-OP: the pick is an intent. The server holds the clock, re-checks the
            // location, rests the world and heals every player; the new game time and
            // hit points arrive on the ordinary delta channels. Nothing local moves —
            // an optimistic clock would be fought by the next update.
            clientViewerRest(duration);
        } else if (duration == PIPBOY_REST_DURATION_UNTIL_MIDNIGHT) {
            if (pipboyRest(hours, minutes, kind) == 0) {
                pipboyDrawNumber(0, 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
            }
            windowRefresh(gPipboyWindow);
        } else {
            pipboyRest(hours, minutes, kind);
        }

        soundPlayFile("ib2lu1x1");

        pipboyWindowRenderRestOptions(0);
    }
}

// 0x4996B4
static void pipboyWindowRenderRestOptions(int a1)
{
    const char* text;

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    if (gPipboyLinesCount >= 0) {
        gPipboyCurrentLine = 0;
    }

    // ALARM CLOCK
    text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 300);
    pipboyDrawText(text, PIPBOY_TEXT_ALIGNMENT_CENTER | PIPBOY_TEXT_STYLE_UNDERLINE, _colorTable[992]);

    if (gPipboyLinesCount >= 5) {
        gPipboyCurrentLine = 5;
    }

    pipboyDrawHitPoints();

    // NOTE: I don't know if this +1 was a result of compiler optimization or it
    // was written like this in the first place.
    for (int option = 1; option < gPipboyRestOptionsCount + 1; option++) {
        // 302 - Rest for ten minutes
        // ...
        // 315 - Rest until party is healed
        text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 302 + option - 1);
        int color = option == a1 ? _colorTable[32747] : _colorTable[992];

        pipboyDrawText(text, 0, color);

        gPipboyCurrentLine++;
    }

    windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
}

// 0x4997B8
static void pipboyDrawHitPoints()
{
    int max_hp;
    int cur_hp;
    char* text;
    char msg[64];
    int len;

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + 66 * PIPBOY_WINDOW_WIDTH + 254,
        350,
        10,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + 66 * PIPBOY_WINDOW_WIDTH + 254,
        PIPBOY_WINDOW_WIDTH);

    max_hp = critterGetStat(gDude, STAT_MAXIMUM_HIT_POINTS);
    cur_hp = critterGetHitPoints(gDude);
    text = getmsg(&gPipboyMessageList, &gPipboyMessageListItem, 301); // Hit Points
    snprintf(msg, sizeof(msg), "%s %d/%d", text, cur_hp, max_hp);
    len = fontGetStringWidth(msg);
    fontDrawText(gPipboyWindowBuffer + 66 * PIPBOY_WINDOW_WIDTH + 254 + (350 - len) / 2, msg, PIPBOY_WINDOW_WIDTH, PIPBOY_WINDOW_WIDTH, _colorTable[992]);
}

// 0x4998C0
static void pipboyWindowCreateButtons(int start, int count, bool a3)
{
    fontSetCurrent(101);

    int height = fontGetLineHeight();

    gPipboyWindowButtonStart = start;
    gPipboyWindowButtonCount = count;

    if (count != 0) {
        int y = start * height + PIPBOY_WINDOW_CONTENT_VIEW_Y;
        int eventCode = start + 505;
        for (int index = start; index < 22; index++) {
            if (gPipboyWindowButtonCount + gPipboyWindowButtonStart <= index) {
                break;
            }

            _HotLines[index] = buttonCreate(gPipboyWindow, 254, y, 350, height, -1, -1, -1, eventCode, nullptr, nullptr, nullptr, BUTTON_FLAG_TRANSPARENT);
            y += height * 2;
            eventCode += 1;
        }
    }

    if (a3) {
        _button = buttonCreate(gPipboyWindow, 254, height * gPipboyLinesCount + PIPBOY_WINDOW_CONTENT_VIEW_Y, 350, height, -1, -1, -1, 528, nullptr, nullptr, nullptr, BUTTON_FLAG_TRANSPARENT);
    }
}

// 0x4999C0
static void pipboyWindowDestroyButtons()
{
    if (gPipboyWindowButtonCount != 0) {
        // NOTE: There is a buffer overread bug. In original binary it leads to
        // reading continuation (from 0x6644B8 onwards), which finally destroys
        // button in `gPipboyWindow` (id #3), which corresponds to Skilldex
        // button. Other buttons can be destroyed depending on the last mouse
        // position. I was not able to replicate this exact behaviour with MSVC.
        // So here is a small fix, which is an exception to "Do not fix vanilla
        // bugs" strategy.
        int end = gPipboyWindowButtonStart + gPipboyWindowButtonCount;
        if (end > 22) {
            end = 22;
        }

        for (int index = gPipboyWindowButtonStart; index < end; index++) {
            buttonDestroy(_HotLines[index]);
        }
    }

    if (_hot_back_line) {
        buttonDestroy(_button);
    }

    gPipboyWindowButtonCount = 0;
    _hot_back_line = 0;
}

// 0x499A24
// One rest FRAME's presentation: the clock, the date, the hit points, escape, and
// the 50 ms of real time that makes a rest feel like one. The simulation half is
// restPerform (rest.cc, f2_core) — the dedicated server drives the same loop with no
// frame proc at all.
static bool pipboyRestFrame()
{
    sharedFpsLimiter.mark();

    unsigned int start = getTicks();

    bool abort = inputGetInput() == KEY_ESCAPE || _game_user_wants_to_quit != 0;

    pipboyDrawNumber(gameTimeGetHour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
    pipboyDrawDate();
    pipboyDrawHitPoints();
    windowRefresh(gPipboyWindow);

    if (!abort) {
        delay_ms(50 - (getTicks() - start));
    }

    renderPresent();
    sharedFpsLimiter.throttle();

    return abort;
}

static bool pipboyRest(int hours, int minutes, int duration)
{
    // Belt and braces for a SIM-DIVERGENCE hazard: whatever reaches this leaf, a viewer
    // must never run the rest sim locally (it advances gameTime and heals everyone). The
    // alarm-clock tab refuses above and the co-op path asks the SERVER with the `rest`
    // verb instead; this makes any other route inert too.
    if (clientViewerActive()) {
        debugPrint("pipboy: rest refused - a viewer does not own the clock\n");
        return false;
    }

    // The headless probe has no window to draw into, so it runs the loop bare — the
    // same way the server does. The gate stays the BROAD headlessProbeActive() so the
    // legacy probe pump, which never opens the pipboy window, also skips the UI.
    const bool headless = headlessProbeActive();

    if (!headless) {
        gameMouseSetCursor(MOUSE_CURSOR_WAIT_WATCH);
    }

    RestOutcome outcome = restPerform(hours, minutes, duration, headless ? nullptr : pipboyRestFrame);

    // A queued event wants the screen gone; escape does not.
    if (outcome == kRestInterrupted) {
        _proc_bail_flag = 1;
    }

    if (!headless) {
        pipboyDrawNumber(gameTimeGetHour(), 4, PIPBOY_WINDOW_TIME_X, PIPBOY_WINDOW_TIME_Y);
        pipboyDrawDate();
        windowRefresh(gPipboyWindow);

        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    }

    return outcome != kRestCompleted;
}

// 0x49A0C8
static int pipboyRenderScreensaver()
{
    PipboyBomb bombs[PIPBOY_BOMB_COUNT];

    mouseGetPositionInWindow(gPipboyWindow, &gPipboyPreviousMouseX, &gPipboyPreviousMouseY);

    for (int index = 0; index < PIPBOY_BOMB_COUNT; index += 1) {
        bombs[index].field_10 = 0;
    }

    _gmouse_disable(0);

    unsigned char* buf = (unsigned char*)internal_malloc(412 * 374);
    if (buf == nullptr) {
        return -1;
    }

    blitBufferToBuffer(gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        buf,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH);

    blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    int v31 = 50;
    while (true) {
        sharedFpsLimiter.mark();

        unsigned int time = getTicks();

        mouseGetPositionInWindow(gPipboyWindow, &gPipboyMouseX, &gPipboyMouseY);
        if (inputGetInput() != -1 || gPipboyPreviousMouseX != gPipboyMouseX || gPipboyPreviousMouseY != gPipboyMouseY) {
            break;
        }

        double random = randomBetween(0, PIPBOY_RAND_MAX);

        // TODO: Figure out what this constant means. Probably somehow related
        // to PIPBOY_RAND_MAX.
        if (random < 3047.3311) {
            int index = 0;
            for (; index < PIPBOY_BOMB_COUNT; index += 1) {
                if (bombs[index].field_10 == 0) {
                    break;
                }
            }

            if (index < PIPBOY_BOMB_COUNT) {
                PipboyBomb* bomb = &(bombs[index]);
                int v27 = (350 - _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth() / 4) + (406 - _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() / 4);
                int v5 = (int)((double)randomBetween(0, PIPBOY_RAND_MAX) / (double)PIPBOY_RAND_MAX * (double)v27);
                int v6 = _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() / 4;
                if (PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT - v6 >= v5) {
                    bomb->x = 602;
                    bomb->y = v5 + 48;
                } else {
                    bomb->x = v5 - (PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT - v6) + PIPBOY_WINDOW_CONTENT_VIEW_X + _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth() / 4;
                    bomb->y = PIPBOY_WINDOW_CONTENT_VIEW_Y - _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() + 2;
                }

                bomb->field_10 = 1;
                bomb->field_8 = (float)((double)randomBetween(0, PIPBOY_RAND_MAX) * (2.75 / PIPBOY_RAND_MAX) + 0.15);
                bomb->field_C = 0;
            }
        }

        if (v31 == 0) {
            blitBufferToBuffer(_pipboyFrmImages[PIPBOY_FRM_BACKGROUND].getData() + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
                PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
                PIPBOY_WINDOW_WIDTH,
                gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
                PIPBOY_WINDOW_WIDTH);
        }

        for (int index = 0; index < PIPBOY_BOMB_COUNT; index++) {
            PipboyBomb* bomb = &(bombs[index]);
            if (bomb->field_10 != 1) {
                continue;
            }

            int srcWidth = _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth();
            int srcHeight = _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight();
            int destX = bomb->x;
            int destY = bomb->y;
            int srcY = 0;
            int srcX = 0;

            if (destX >= PIPBOY_WINDOW_CONTENT_VIEW_X) {
                if (destX + _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth() >= 604) {
                    srcWidth = 604 - destX;
                    if (srcWidth < 1) {
                        bomb->field_10 = 0;
                    }
                }
            } else {
                srcX = PIPBOY_WINDOW_CONTENT_VIEW_X - destX;
                if (srcX >= _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth()) {
                    bomb->field_10 = 0;
                }
                destX = PIPBOY_WINDOW_CONTENT_VIEW_X;
                srcWidth = _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth() - srcX;
            }

            if (destY >= PIPBOY_WINDOW_CONTENT_VIEW_Y) {
                if (destY + _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() >= 452) {
                    srcHeight = 452 - destY;
                    if (srcHeight < 1) {
                        bomb->field_10 = 0;
                    }
                }
            } else {
                if (destY + _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() < PIPBOY_WINDOW_CONTENT_VIEW_Y) {
                    bomb->field_10 = 0;
                }

                srcY = PIPBOY_WINDOW_CONTENT_VIEW_Y - destY;
                srcHeight = _pipboyFrmImages[PIPBOY_FRM_BOMB].getHeight() - srcY;
                destY = PIPBOY_WINDOW_CONTENT_VIEW_Y;
            }

            if (bomb->field_10 == 1 && v31 == 0) {
                blitBufferToBufferTrans(
                    _pipboyFrmImages[PIPBOY_FRM_BOMB].getData() + _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth() * srcY + srcX,
                    srcWidth,
                    srcHeight,
                    _pipboyFrmImages[PIPBOY_FRM_BOMB].getWidth(),
                    gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * destY + destX,
                    PIPBOY_WINDOW_WIDTH);
            }

            bomb->field_C += bomb->field_8;
            if (bomb->field_C >= 1.0) {
                bomb->x = (int)((float)bomb->x - bomb->field_C);
                bomb->y = (int)((float)bomb->y + bomb->field_C);
                bomb->field_C = 0.0;
            }
        }

        if (v31 != 0) {
            v31 -= 1;
        } else {
            windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
            delay_ms(50 - (getTicks() - time));
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    blitBufferToBuffer(buf,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        PIPBOY_WINDOW_CONTENT_VIEW_HEIGHT,
        PIPBOY_WINDOW_CONTENT_VIEW_WIDTH,
        gPipboyWindowBuffer + PIPBOY_WINDOW_WIDTH * PIPBOY_WINDOW_CONTENT_VIEW_Y + PIPBOY_WINDOW_CONTENT_VIEW_X,
        PIPBOY_WINDOW_WIDTH);

    internal_free(buf);

    windowRefreshRect(gPipboyWindow, &gPipboyWindowContentRect);
    _gmouse_enable();

    return 0;
}

// 0x49A5D4
static int questInit()
{
    if (gQuestDescriptions != nullptr) {
        internal_free(gQuestDescriptions);
        gQuestDescriptions = nullptr;
    }

    gQuestsCount = 0;

    messageListFree(&gQuestsMessageList);

    if (!messageListInit(&gQuestsMessageList)) {
        return -1;
    }

    if (!messageListLoad(&gQuestsMessageList, "game\\quests.msg")) {
        return -1;
    }

    File* stream = fileOpen("data\\quests.txt", "rt");
    if (stream == nullptr) {
        return -1;
    }

    char string[256];
    while (fileReadString(string, 256, stream)) {
        const char* delim = " \t,";
        char* tok;
        QuestDescription entry;

        char* pch = string;
        while (isspace(*pch)) {
            pch++;
        }

        if (*pch == '#') {
            continue;
        }

        tok = strtok(pch, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.location = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.description = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.gvar = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.displayThreshold = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.completedThreshold = atoi(tok);

        QuestDescription* entries = (QuestDescription*)internal_realloc(gQuestDescriptions, sizeof(*gQuestDescriptions) * (gQuestsCount + 1));
        if (entries == nullptr) {
            goto err;
        }

        memcpy(&(entries[gQuestsCount]), &entry, sizeof(entry));

        gQuestDescriptions = entries;
        gQuestsCount++;
    }

    qsort(gQuestDescriptions, gQuestsCount, sizeof(*gQuestDescriptions), questDescriptionCompare);

    fileClose(stream);

    return 0;

err:

    fileClose(stream);

    return -1;
}

// 0x49A7E4
static void questFree()
{
    if (gQuestDescriptions != nullptr) {
        internal_free(gQuestDescriptions);
        gQuestDescriptions = nullptr;
    }

    gQuestsCount = 0;

    messageListFree(&gQuestsMessageList);
}

// 0x49A818
static int questDescriptionCompare(const void* a1, const void* a2)
{
    QuestDescription* v1 = (QuestDescription*)a1;
    QuestDescription* v2 = (QuestDescription*)a2;
    return v1->location - v2->location;
}

// 0x49A824
static int holodiskInit()
{
    if (gHolodiskDescriptions != nullptr) {
        internal_free(gHolodiskDescriptions);
        gHolodiskDescriptions = nullptr;
    }

    gHolodisksCount = 0;

    File* stream = fileOpen("data\\holodisk.txt", "rt");
    if (stream == nullptr) {
        return -1;
    }

    char str[256];
    while (fileReadString(str, sizeof(str), stream)) {
        const char* delim = " \t,";
        char* tok;
        HolodiskDescription entry;

        char* ch = str;
        while (isspace(*ch)) {
            ch++;
        }

        if (*ch == '#') {
            continue;
        }

        tok = strtok(ch, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.gvar = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.name = atoi(tok);

        tok = strtok(nullptr, delim);
        if (tok == nullptr) {
            continue;
        }

        entry.description = atoi(tok);

        HolodiskDescription* entries = (HolodiskDescription*)internal_realloc(gHolodiskDescriptions, sizeof(*gHolodiskDescriptions) * (gHolodisksCount + 1));
        if (entries == nullptr) {
            goto err;
        }

        memcpy(&(entries[gHolodisksCount]), &entry, sizeof(*gHolodiskDescriptions));

        gHolodiskDescriptions = entries;
        gHolodisksCount++;
    }

    fileClose(stream);

    return 0;

err:

    fileClose(stream);

    return -1;
}

// 0x49A968
static void holodiskFree()
{
    if (gHolodiskDescriptions != nullptr) {
        internal_free(gHolodiskDescriptions);
        gHolodiskDescriptions = nullptr;
    }

    gHolodisksCount = 0;
}

} // namespace fallout
