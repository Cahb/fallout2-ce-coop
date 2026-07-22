#include "client_net.h"
#include "msg_channel.h"
#include "game_dialog.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <functional>

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat.h"
#include "combat_ai.h"
#include "critter.h"
#include "cycle.h"
#include "debug.h"
#include "delay.h"
#include "dialog.h"
#include "dialog_intent.h"
#include "display_monitor.h"
#include "draw.h"
#include "game.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "lips.h"
#include "memory.h"
#include "mouse.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "presenter.h"
#include "proto.h"
#include "random.h"
#include "scripts.h"
#include "server_loop.h"
#include "sfall_config.h"
#include "skill.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "text_object.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

#define DIALOG_REVIEW_ENTRIES_CAPACITY 80

#define DIALOG_OPTION_ENTRIES_CAPACITY 30

#define GAME_DIALOG_WINDOW_WIDTH 640
#define GAME_DIALOG_WINDOW_HEIGHT 480

#define GAME_DIALOG_REPLY_WINDOW_X 135
#define GAME_DIALOG_REPLY_WINDOW_Y 225
#define GAME_DIALOG_REPLY_WINDOW_WIDTH 379
#define GAME_DIALOG_REPLY_WINDOW_HEIGHT 58

#define GAME_DIALOG_OPTIONS_WINDOW_X 127
#define GAME_DIALOG_OPTIONS_WINDOW_Y 335
#define GAME_DIALOG_OPTIONS_WINDOW_WIDTH 393
#define GAME_DIALOG_OPTIONS_WINDOW_HEIGHT 117

#define GAME_DIALOG_REVIEW_WINDOW_WIDTH 640
#define GAME_DIALOG_REVIEW_WINDOW_HEIGHT 480

typedef enum GameDialogReviewWindowButton {
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT,
} GameDialogReviewWindowButton;

typedef enum GameDialogReviewWindowButtonFrm {
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_NORMAL,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_PRESSED,
    GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT,
} GameDialogReviewWindowButtonFrm;

typedef enum GameDialogReaction {
    GAME_DIALOG_REACTION_GOOD = 49,
    GAME_DIALOG_REACTION_NEUTRAL = 50,
    GAME_DIALOG_REACTION_BAD = 51,
} GameDialogReaction;

typedef struct GameDialogReviewEntry {
    int replyMessageListId;
    int replyMessageId;
    // Can be NULL.
    char* replyText;
    int optionMessageListId;
    int optionMessageId;
    char* optionText;
} GameDialogReviewEntry;

typedef struct GameDialogOptionEntry {
    int messageListId;
    int messageId;
    int reaction;
    int proc;
    int btn;
    int top;
    char text[900];
    int bottom;
} GameDialogOptionEntry;

// Provides button configuration for party member combat control and
// customization interface.
typedef struct GameDialogButtonData {
    int x;
    int y;
    int upFrmId;
    int downFrmId;
    int disabledFrmId;
    CacheEntry* upFrmHandle;
    CacheEntry* downFrmHandle;
    CacheEntry* disabledFrmHandle;
    int keyCode;
    int value;
} GameDialogButtonData;

typedef struct STRUCT_5189E4 {
    int messageId;
    int value;
} STRUCT_5189E4;

typedef enum PartyMemberCustomizationOption {
    PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE,
    PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT,
} PartyMemberCustomizationOption;

// 0x444D10
static int _Dogs[3] = {
    0x1000088,
    0x1000156,
    0x1000180,
};

// 0x5186D4
static int _dialog_state_fix = 0;

// Live dialog block-and-pump seam (DIALOG_STREAMING_PLAN.md Stage 2). Installed by
// the dedicated server (server_main.cc). nullptr everywhere else (client, golden
// harness), where the barrier keeps its original headless behavior. See
// gameDialogSetServerPump.
static std::function<bool()> gDialogServerPump;

// > 0 while _gdProcess is running a conversation that can consume a dialog
// intent. See the long note at the increment site — this, NOT _gdialogActive(),
// is what the server's dsay/dend gate must ask.
static int gDialogServerNodeDepth = 0;

// 0x5186D8
static int gGameDialogOptionEntriesLength = 0;

// 0x5186DC
static int gGameDialogReviewEntriesLength = 0;

// 0x5186E0
static unsigned char* gGameDialogDisplayBuffer = nullptr;

// 0x5186E4
static int gGameDialogReplyWindow = -1;

// 0x5186E8
static int gGameDialogOptionsWindow = -1;

// 0x5186EC
static bool _gdialog_window_created = false;

// 0x5186F0
static int _boxesWereDisabled = 0;

// 0x5186F4
static int gGameDialogFidgetFid = 0;

// 0x5186F8
static CacheEntry* gGameDialogFidgetFrmHandle = nullptr;

// 0x5186FC
static Art* gGameDialogFidgetFrm = nullptr;

// 0x518700
static int gGameDialogBackground = 2;

// 0x518704
static int _lipsFID = 0;

// 0x518708
static CacheEntry* _lipsKey = nullptr;

// 0x51870C
static Art* _lipsFp = nullptr;

// 0x518710
static bool gGameDialogLipSyncStarted = false;

// 0x518714
static int _dialogue_state = 0;

// 0x518718
static int _dialogue_switch_mode = 0;

// 0x51871C
static int _gdialog_state = -1;

// 0x518720
static bool _gdDialogWentOff = false;

// 0x518724
static bool _gdDialogTurnMouseOff = false;

// 0x518728
static int _gdReenterLevel = 0;

// 0x51872C
static bool _gdReplyTooBig = false;

// 0x518730
static Object* _peon_table_obj = nullptr;

// 0x518734
static Object* _barterer_table_obj = nullptr;

// 0x518738
static Object* _barterer_temp_obj = nullptr;

// 0x51873C
static int gGameDialogBarterModifier = 0;

// dialogueBackWindow
// 0x518740
static int gGameDialogBackgroundWindow = -1;

// 0x518744
static int gGameDialogWindow = -1;

// 0x518748
static Rect _backgrndRects[8] = {
    { 126, 14, 152, 40 },
    { 488, 14, 514, 40 },
    { 126, 188, 152, 214 },
    { 488, 188, 514, 214 },
    { 152, 14, 488, 24 },
    { 152, 204, 488, 214 },
    { 126, 40, 136, 188 },
    { 504, 40, 514, 188 },
};

// 0x5187C8
static int _talk_need_to_center = 1;

// 0x5187CC
static bool _can_start_new_fidget = false;

// 0x5187D0
static int _gd_replyWin = -1;

// 0x5187D4
static int _gd_optionsWin = -1;

// 0x5187D8
static int gGameDialogOldMusicVolume = -1;

// 0x5187DC
static int gGameDialogOldCenterTile = -1;

// 0x5187E0
static int gGameDialogOldDudeTile = -1;

// 0x5187E4
static unsigned char* _light_BlendTable = nullptr;

// 0x5187E8
static unsigned char* _dark_BlendTable = nullptr;

// 0x5187EC
static int _dialogue_just_started = 0;

// 0x5187F0
static int _dialogue_seconds_since_last_input = 0;

// 0x518818
static const int gGameDialogReviewWindowButtonWidths[GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT] = {
    35,
    35,
    82,
};

// 0x518824
static const int gGameDialogReviewWindowButtonHeights[GAME_DIALOG_REVIEW_WINDOW_BUTTON_COUNT] = {
    35,
    37,
    46,
};

// 0x518830
static int gGameDialogReviewWindowButtonFrmIds[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT] = {
    89, // di_bgdn1.frm - dialog big down arrow
    90, // di_bgdn2.frm - dialog big down arrow
    87, // di_bgup1.frm - dialog big up arrow
    88, // di_bgup2.frm - dialog big up arrow
    91, // di_done1.frm - dialog big done button up
    92, // di_done2.frm - dialog big done button down
};

// 0x518848
Object* gGameDialogSpeaker = nullptr;

// 0x51884C
bool gGameDialogSpeakerIsPartyMember = false;

// 0x518850
int gGameDialogHeadFid = 0;

// The script-supplied reaction/fidget level for the live conversation (FIDGET_GOOD /
// NEUTRAL / BAD). Drives the talking head's idle animation. Set by
// _gdialogInitFromScript, streamed by dialogEmitNode.
int gGameDialogFidget = FIDGET_NEUTRAL;

// 0x518854
int gGameDialogSid = -1;

// Maps phoneme to talking head frame.
//
// 0x518858
static int _head_phoneme_lookup[PHONEME_COUNT] = {
    0,
    3,
    1,
    1,
    3,
    1,
    1,
    1,
    7,
    8,
    7,
    3,
    1,
    8,
    1,
    7,
    7,
    6,
    6,
    2,
    2,
    2,
    2,
    4,
    4,
    5,
    5,
    2,
    2,
    2,
    2,
    2,
    6,
    2,
    2,
    5,
    8,
    2,
    2,
    2,
    2,
    8,
};

// 0x518900
static int _phone_anim = 0;

// 0x518904
static int _loop_cnt = -1;

// 0x518908
static unsigned int _tocksWaiting = 10000;

// 0x51890C
static const char* _react_strs[3] = {
    "Said Good",
    "Said Neutral",
    "Said Bad",
};

// 0x518918
static int _dialogue_subwin_len = 0;

// 0x51891C
static GameDialogButtonData gGameDialogDispositionButtonsData[5] = {
    { 438, 37, 397, 395, 396, nullptr, nullptr, nullptr, 2098, 4 },
    { 438, 67, 394, 392, 393, nullptr, nullptr, nullptr, 2103, 3 },
    { 438, 96, 406, 404, 405, nullptr, nullptr, nullptr, 2102, 2 },
    { 438, 126, 400, 398, 399, nullptr, nullptr, nullptr, 2111, 1 },
    { 438, 156, 403, 401, 402, nullptr, nullptr, nullptr, 2099, 0 },
};

// 0x5189E4
static STRUCT_5189E4 _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT][6] = {
    {
        { 100, AREA_ATTACK_MODE_ALWAYS }, // Always!
        { 101, AREA_ATTACK_MODE_SOMETIMES }, // Sometimes, don't worry about hitting me
        { 102, AREA_ATTACK_MODE_BE_SURE }, // Be sure you won't hit me
        { 103, AREA_ATTACK_MODE_BE_CAREFUL }, // Be careful not to hit me
        { 104, AREA_ATTACK_MODE_BE_ABSOLUTELY_SURE }, // Be absolutely sure you won't hit me
        { -1, 0 },
    },
    {
        { 200, RUN_AWAY_MODE_COWARD - 1 }, // Abject coward
        { 201, RUN_AWAY_MODE_FINGER_HURTS - 1 }, // Your finger hurts
        { 202, RUN_AWAY_MODE_BLEEDING - 1 }, // You're bleeding a bit
        { 203, RUN_AWAY_MODE_NOT_FEELING_GOOD - 1 }, // Not feeling good
        { 204, RUN_AWAY_MODE_TOURNIQUET - 1 }, // You need a tourniquet
        { 205, RUN_AWAY_MODE_NEVER - 1 }, // Never!
    },
    {
        { 300, BEST_WEAPON_NO_PREF }, // None
        { 301, BEST_WEAPON_MELEE }, // Melee
        { 302, BEST_WEAPON_MELEE_OVER_RANGED }, // Melee then ranged
        { 303, BEST_WEAPON_RANGED_OVER_MELEE }, // Ranged then melee
        { 304, BEST_WEAPON_RANGED }, // Ranged
        { 305, BEST_WEAPON_UNARMED }, // Unarmed
    },
    {
        { 400, DISTANCE_STAY_CLOSE }, // Stay close to me
        { 401, DISTANCE_CHARGE }, // Charge!
        { 402, DISTANCE_SNIPE }, // Snipe the enemy
        { 403, DISTANCE_ON_YOUR_OWN }, // On your own
        { 404, DISTANCE_STAY }, // Say where you are
        { -1, 0 },
    },
    {
        { 500, ATTACK_WHO_WHOMEVER_ATTACKING_ME }, // Whomever is attacking me
        { 501, ATTACK_WHO_STRONGEST }, // The strongest
        { 502, ATTACK_WHO_WEAKEST }, // The weakest
        { 503, ATTACK_WHO_WHOMEVER }, // Whomever you want
        { 504, ATTACK_WHO_CLOSEST }, // Whoever is closest
        { -1, 0 },
    },
    {
        { 600, CHEM_USE_CLEAN }, // I'm clean
        { 601, CHEM_USE_STIMS_WHEN_HURT_LITTLE }, // Stimpacks when hurt a bit
        { 602, CHEM_USE_STIMS_WHEN_HURT_LOTS }, // Stimpacks when hurt a lot
        { 603, CHEM_USE_SOMETIMES }, // Any drug some of the time
        { 604, CHEM_USE_ANYTIME }, // Any drug any time
        { -1, 0 },
    },
};

// 0x518B04
static GameDialogButtonData _custom_button_info[PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT] = {
    { 95, 9, 410, 409, -1, nullptr, nullptr, nullptr, 0, 0 },
    { 96, 38, 416, 415, -1, nullptr, nullptr, nullptr, 1, 0 },
    { 96, 68, 418, 417, -1, nullptr, nullptr, nullptr, 2, 0 },
    { 96, 98, 414, 413, -1, nullptr, nullptr, nullptr, 3, 0 },
    { 96, 127, 408, 407, -1, nullptr, nullptr, nullptr, 4, 0 },
    { 96, 157, 412, 411, -1, nullptr, nullptr, nullptr, 5, 0 },
};

// 0x518BF4
static int _totalHotx = 0;

// 0x58EA80
static int _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT];

// custom.msg
//
// 0x58EA98
static MessageList gCustomMessageList;

// 0x58EAA0
static unsigned char _light_GrayTable[256];

// 0x58EBA0
static unsigned char _dark_GrayTable[256];

// 0x58ECA0
static unsigned char* _backgrndBufs[8];

// 0x58ECC0
static Rect _optionRect;

// 0x58ECD0
static Rect _replyRect;

// 0x58ECE0
static GameDialogReviewEntry gDialogReviewEntries[DIALOG_REVIEW_ENTRIES_CAPACITY];

// 0x58F460
static int _custom_buttons_start;

// 0x58F464
static int _control_buttons_start;

// 0x58F468
static int gGameDialogReviewWindowOldFont;

// 0x58F470
static int _gdialog_buttons[9];

// 0x58F4C8
static int _oldFont;

// 0x58F4CC
static unsigned int gGameDialogFidgetLastUpdateTimestamp;

// 0x58F4D0
static int gGameDialogFidgetReaction;

// 0x58F4D4
static Program* gDialogReplyProgram;

// 0x58F4D8
static int gDialogReplyMessageListId;

// 0x58F4DC
static int gDialogReplyMessageId;

// 0x58F4E0
static int dword_58F4E0;

// Audio filename pending for the current dialog node (set by _scr_get_msg_str_speech,
// consumed by dialogEmitNode). Empty string = no audio for this node.
char gDialogPendingAudio[16];

// NOTE: The is something odd about this variable. There are 2700 bytes, which
// is 3 x 900, but anywhere in the app only 900 characters is used. The length
// of text in [DialogOptionEntry] is definitely 900 bytes. There are two
// possible explanations:
// - it's an array of 3 elements
// - there are three separate elements, two of which are not used, therefore
// they are not referenced anywhere, but they take up their space.
//
// See `_gdProcessChoice` for more info how this unreferenced range plays
// important role.
//
// 0x58F4E4
static char gDialogReplyText[900];

// 0x58FF70
static GameDialogOptionEntry gDialogOptionEntries[DIALOG_OPTION_ENTRIES_CAPACITY];

// 0x596C30
static int _talkOldFont;

// 0x596C34
static unsigned int gGameDialogFidgetUpdateDelay;

// 0x596C38
static int gGameDialogFidgetFrmCurrentFrame;

static FrmImage _reviewBackgroundFrmImage;
static FrmImage _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT];
static FrmImage _reviewButtonNormalFrmImage;
static FrmImage _reviewButtonPressedFrmImage;
static FrmImage _redButtonNormalFrmImage;
static FrmImage _redButtonPressedFrmImage;
static FrmImage _lowerHighlightFrmImage;
static FrmImage _upperHighlightFrmImage;

static int _gdialogReset();
static void gameDialogEndLips();
static int gdHide();
static int gdUnhide();
static int gameDialogAddMessageOption(int a1, int a2, int a3);
static int gameDialogAddTextOption(int a1, const char* a2, int a3);
static int gameDialogReviewWindowInit(int* win);
static int gameDialogReviewWindowFree(int* win);
static int gameDialogShowReview();
static void gameDialogReviewButtonOnMouseUp(int btn, int keyCode);
static void gameDialogReviewWindowUpdate(int win, int origin);
static void dialogReviewEntriesClear();
static int gameDialogAddReviewMessage(int messageListId, int messageId);
static int gameDialogAddReviewText(const char* text);
static int gameDialogSetReviewOptionMessage(int messageListId, int messageId);
static int gameDialogSetReviewOptionText(const char* text);
static int _gdProcessInit();
static void _gdProcessCleanup();
static int _gdProcessExit();
static void gameDialogRenderCaps();
static int _gdProcess();
static int _gdProcessChoice(int a1);
static void gameDialogOptionOnMouseEnter(int a1);
static void gameDialogOptionOnMouseExit(int a1);
static void gameDialogRenderReply();
static void _gdProcessUpdate();
static int _gdCreateHeadWindow();
static void _gdDestroyHeadWindow();
static void _gdSetupFidget(int headFid, int reaction);
static void gameDialogWaitForFidgetToComplete();
static void _gdPlayTransition(int a1);
static void _reply_arrow_up(int btn, int a2);
static void _reply_arrow_down(int btn, int a2);
static void _reply_arrow_restore(int btn, int a2);
static void _demo_copy_title(int win);
static void _demo_copy_options(int win);
static void _gDialogRefreshOptionsRect(int win, Rect* drawRect);
static void gameDialogTicker();
// Animates scroll up or down of a given dialog sub-window.
// If scrolling up - only uses subWindowFrmData to gradually fill the window (must be pre-filled with bg window contents).
// If scroliing down - uses both subWindowFrmData and bgWindowFrmData to fill parts of window buffer.
static void _gdialog_scroll_subwin(int windowIdx, bool scrollUp, unsigned char* subWindowFrmData, unsigned char* windowBuf, unsigned char* bgWindowFrmData, int windowHeight, bool instantScrollUp = false);
static int _text_num_lines(const char* a1, int a2);
static int text_to_rect_wrapped(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color);
static int gameDialogDrawText(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color, int a7);
static int _gdialog_barter_create_win();
static void _gdialog_barter_destroy_win();
static void _gdialog_barter_cleanup_tables();
static int partyMemberControlWindowInit();
static void partyMemberControlWindowFree();
static void partyMemberControlWindowUpdate();
static void gameDialogCombatControlButtonOnMouseUp(int a1, int a2);
static int _gdPickAIUpdateMsg(Object* obj);
static int _gdCanBarter();
static void partyMemberControlWindowHandleEvents();
static int partyMemberCustomizationWindowInit();
static void partyMemberCustomizationWindowFree();
static void partyMemberCustomizationWindowHandleEvents();
static void partyMemberCustomizationWindowUpdate();
static void _gdCustomSelectRedraw(unsigned char* dest, int pitch, int type, int selectedIndex);
static int _gdCustomSelect(int a1);
static void _gdCustomUpdateSetting(int option, int value);
static void gameDialogBarterButtonUpMouseUp(int btn, int a2);
static int _gdialog_window_create();
static void _gdialog_window_destroy();
static int talk_to_create_background_window();
static int gameDialogWindowRenderBackground();
static int _talkToRefreshDialogWindowRect(Rect* rect);
static void gameDialogRenderHighlight(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int x, int y, int destPitch, unsigned char* a9, unsigned char* a10);
static void gameDialogRenderTalkingHead(Art* art, int frame);
static void gameDialogHighlightsInit();
static void gameDialogHighlightsExit();

static void gameDialogRedButtonsInit();
static void gameDialogRedButtonsExit();

static bool gGameDialogFix;
static bool gNumberOptions;

// gdialog_init
// 0x444D1C
int gameDialogInit()
{
    // SFALL: Prevents from using 0 to escape from dialogue at any time.
    gGameDialogFix = true;
    configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_GAME_DIALOG_FIX_KEY, &gGameDialogFix);

    // SFALL: Use numbers for replies (instead of default knobs).
    gNumberOptions = false;
    configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_NUMBERS_IS_DIALOG_KEY, &gNumberOptions);

    return 0;
}

// 0x444D20
int _gdialogReset()
{
    gameDialogEndLips();
    return 0;
}

// NOTE: Uncollapsed 0x444D20.
int gameDialogReset()
{
    return _gdialogReset();
}

// NOTE: Uncollapsed 0x444D20.
int gameDialogExit()
{
    return _gdialogReset();
}

// 0x444D2C
bool _gdialogActive()
{
    return _dialog_state_fix != 0;
}

// gdialogEnter
// 0x444D3C
void gameDialogEnter(Object* speaker, int a2)
{
    if (speaker == nullptr) {
        debugPrint("\nError: gdialogEnter: target was NULL!");
        return;
    }

    _gdDialogWentOff = false;

    if (isInCombat()) {
        return;
    }

    if (speaker->sid == -1) {
        return;
    }

    // Under the server loop the dude is not walked into talk range first (the
    // approach locomotion is dropped, same accepted class as the door/pickup/
    // skill-on-target decouples), so skip the LOS/range gate — the conversation
    // itself does not depend on the dude's exact tile.
    if (!serverLoopActive() && PID_TYPE(speaker->pid) != OBJ_TYPE_ITEM && SID_TYPE(speaker->sid) != SCRIPT_TYPE_SPATIAL) {
        MessageListItem messageListItem;

        int rc = _action_can_talk_to(gDude, speaker);
        if (rc == -1) {
            // You can't see there.
            messageListItem.num = 660;
            if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
                if (a2) {
                    displayMonitorAddMessage(messageListItem.text);
                } else {
                    debugPrint(messageListItem.text);
                }
            } else {
                debugPrint("\nError: gdialog: Can't find message!");
            }
            return;
        }

        if (rc == -2) {
            // Too far away.
            messageListItem.num = 661;
            if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
                if (a2) {
                    displayMonitorAddMessage(messageListItem.text);
                } else {
                    debugPrint(messageListItem.text);
                }
            } else {
                debugPrint("\nError: gdialog: Can't find message!");
            }
            return;
        }
    }

    gGameDialogOldCenterTile = gCenterTile;
    gGameDialogBarterModifier = 0;
    gGameDialogOldDudeTile = gDude->tile;
    isoDisable();

    _dialog_state_fix = 1;
    gGameDialogSpeaker = speaker;
    gGameDialogSpeakerIsPartyMember = objectIsPartyMember(speaker);

    _dialogue_just_started = 1;

    // CE: Obtain and keep SID in a separate variable. This is needed because in
    // rare circumstates the speaker can destroy itself. So after executing it's
    // script |speaker| can point to freed memory. Dereferencing such pointer
    // can lead to crash depending on the environment (confirmed on Android and
    // MSVC debug builds).
    int sid = speaker->sid;
    if (sid != -1) {
        scriptExecProc(speaker->sid, SCRIPT_PROC_TALK);
    }

    Script* script;
    if (scriptGetScript(sid, &script) == -1) {
        gameMouseObjectsShow();
        isoEnable();
        scriptsExecMapUpdateProc();
        _dialog_state_fix = 0;
        return;
    }

    if (script->scriptOverrides || _dialogue_state != 4) {
        _dialogue_just_started = 0;
        isoEnable();
        scriptsExecMapUpdateProc();
        _dialog_state_fix = 0;
        return;
    }

    gameDialogEndLips();

    if (_gdialog_state == 1) {
        // TODO: Not sure about these conditions.
        if (_dialogue_switch_mode == 2) {
            _gdialog_window_destroy();
        } else if (_dialogue_switch_mode == 8) {
            _gdialog_window_destroy();
        } else if (_dialogue_switch_mode == 11) {
            _gdialog_window_destroy();
        } else {
            if (_dialogue_switch_mode == _gdialog_state) {
                _gdialog_barter_destroy_win();
            } else if (_dialogue_state == _gdialog_state) {
                _gdialog_window_destroy();
            } else if (_dialogue_state == a2) {
                _gdialog_barter_destroy_win();
            }
        }
        _gdialogExitFromScript();
    }

    _gdialog_state = 0;
    _dialogue_state = 0;

    int tile = gDude->tile;
    if (gGameDialogOldDudeTile != tile) {
        gGameDialogOldCenterTile = tile;
    }

    if (_gdDialogWentOff) {
        _tile_scroll_to(gGameDialogOldCenterTile, 2);
    }

    isoEnable();
    scriptsExecMapUpdateProc();

    _dialog_state_fix = 0;
}

// 0x444FE4
void _gdialogSystemEnter()
{
    gameUpdateState();

    _gdDialogTurnMouseOff = true;

    soundContinueAll();
    gameDialogEnter(gGameDialogSpeaker, 0);
    soundContinueAll();

    if (gGameDialogOldDudeTile != gDude->tile) {
        gGameDialogOldCenterTile = gDude->tile;
    }

    if (_gdDialogWentOff) {
        _tile_scroll_to(gGameDialogOldCenterTile, 2);
    }

    gameRequestState(GAME_STATE_2);

    gameUpdateState();
}

// 0x445050
void gameDialogStartLips(const char* audioFileName)
{
    if (serverLoopActive()) {
        return;
    }

    if (audioFileName == nullptr) {
        debugPrint("\nGDialog: Bleep!");
        soundPlayFile("censor");
        return;
    }

    char name[16];
    if (artCopyFileName(OBJ_TYPE_HEAD, gGameDialogHeadFid & 0xFFF, name) == -1) {
        return;
    }

    if (lipsLoad(audioFileName, name) == -1) {
        return;
    }

    gGameDialogLipSyncStarted = true;

    lipsStart();

    debugPrint("Starting lipsynch speech");
}

// 0x4450C4
void gameDialogEndLips()
{
    if (gGameDialogLipSyncStarted) {
        debugPrint("Ending lipsynch system");
        gGameDialogLipSyncStarted = false;

        lipsFree();
    }
}

// 0x4450EC
int gameDialogEnable()
{
    tickersAdd(gameDialogTicker);
    return 0;
}

// 0x4450FC
int gameDialogDisable()
{
    tickersRemove(gameDialogTicker);
    return 0;
}

// 0x44510C
int _gdialogInitFromScript(int headFid, int reaction)
{
    // The conversation's AUTHORITATIVE head + mood, captured for the wire. Both come
    // from the script's start_gdialog(..., headId, ...) args (interpreter_extra.cc),
    // NOT from the critter proto — proto head_fid is unset for most critters, which
    // is why a viewer deriving the head from the proto renders NO head and falls back
    // to showing the world in the upper panel (owner-found on the Arroyo Elder, whose
    // script passes headId=3). Captured before the headless early-outs below so the
    // server records them even though it builds no window.
    gGameDialogFidget = reaction;

    if (_dialogue_state == 1) {
        return -1;
    }

    if (_gdialog_state == 1) {
        return 0;
    }

    // Headless server (A2): the dialog runs authoritatively but has NO window,
    // head, tickers or fidget — the _gdProcess barrier drives node transitions off
    // the intent queue, and the node's reply/option state is populated by the
    // gsay_* opcodes, not by this window choreography. Skip the entire renderer
    // setup (windows/highlights/red buttons/head/lips/mouse/music) and keep only
    // the _gdialog_state latch the gsay_start/gsay_end state machine checks.
    // Pairs with the matching guard in _gdialogExitFromScript. The window build
    // still runs normally on the viewer (serverLoopActive() is false there — the
    // A3 render path reuses this exact function).
    if (serverLoopActive()) {
        _gdialog_state = 1;
        _gdDialogWentOff = true;
        return 0;
    }

    animationStop();

    _boxesWereDisabled = indicatorBarHide();
    gGameDialogSpeakerIsPartyMember = objectIsPartyMember(gGameDialogSpeaker);
    _oldFont = fontGetCurrent();
    fontSetCurrent(101);
    dialogSetReplyWindow(135, 225, 379, 58, nullptr);
    dialogSetReplyColor(0.3f, 0.3f, 0.3f);
    dialogSetOptionWindow(127, 335, 393, 117, nullptr);
    dialogSetOptionColor(0.2f, 0.2f, 0.2f);
    dialogSetReplyTitle(nullptr);
    _dialogRegisterWinDrawCallbacks(_demo_copy_title, _demo_copy_options);
    gameDialogHighlightsInit();
    colorCycleDisable();
    if (_gdDialogTurnMouseOff) {
        _gmouse_disable(0);
    }
    gameMouseObjectsHide();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    textObjectsReset();

    if (PID_TYPE(gGameDialogSpeaker->pid) != OBJ_TYPE_ITEM) {
        _tile_scroll_to(gGameDialogSpeaker->tile, 2);
    }

    _talk_need_to_center = 1;

    // CE: Fix Barter button.
    gameDialogRedButtonsInit();

    _gdCreateHeadWindow();
    tickersAdd(gameDialogTicker);
    _gdSetupFidget(headFid, reaction);
    _gdialog_state = 1;
    _gmouse_disable_scrolling();

    if (headFid == -1) {
        // SFALL: Fix the music volume when entering the dialog.
        gGameDialogOldMusicVolume = _gsound_background_volume_get_set(gMusicVolume / 2);
    } else {
        gGameDialogOldMusicVolume = -1;
        backgroundSoundDelete();
    }

    _gdDialogWentOff = true;

    GameMode::enterGameMode(GameMode::kDialog);

    return 0;
}

// 0x445298
int _gdialogExitFromScript()
{
    if (_dialogue_switch_mode == 2 || _dialogue_switch_mode == 8 || _dialogue_switch_mode == 11) {
        return -1;
    }

    if (_gdialog_state == 0) {
        return 0;
    }

    // Headless server (A2): mirror the _gdialogInitFromScript guard — no window,
    // head, tickers or lips were ever created, so tear down only the state latch.
    if (serverLoopActive()) {
        _gdialog_state = 0;
        return 0;
    }

    gameDialogEndLips();
    dialogReviewEntriesClear();
    tickersRemove(gameDialogTicker);

    if (PID_TYPE(gGameDialogSpeaker->pid) != OBJ_TYPE_ITEM) {
        if (gGameDialogOldDudeTile != gDude->tile) {
            gGameDialogOldCenterTile = gDude->tile;
        }
        _tile_scroll_to(gGameDialogOldCenterTile, 2);
    }

    GameMode::exitGameMode(GameMode::kDialog);

    GameMode::enterGameMode(GameMode::kSpecial);
    _gdDestroyHeadWindow();
    GameMode::exitGameMode(GameMode::kSpecial);

    // CE: Fix Barter button.
    gameDialogRedButtonsExit();

    fontSetCurrent(_oldFont);

    if (gGameDialogFidgetFrm != nullptr) {
        artUnlock(gGameDialogFidgetFrmHandle);
        gGameDialogFidgetFrm = nullptr;
    }

    if (_lipsKey != nullptr) {
        if (artUnlock(_lipsKey) == -1) {
            debugPrint("Failure unlocking lips frame!\n");
        }
        _lipsKey = nullptr;
        _lipsFp = nullptr;
        _lipsFID = 0;
    }

    // NOTE: Uninline.
    gameDialogHighlightsExit();

    _gdialog_state = 0;
    _dialogue_state = 0;

    colorCycleEnable();

    if (!gameUiIsDisabled()) {
        _gmouse_enable_scrolling();
    }

    if (gGameDialogOldMusicVolume == -1) {
        backgroundSoundRestart(11);
    } else {
        backgroundSoundSetVolume(gGameDialogOldMusicVolume);
    }

    if (_boxesWereDisabled) {
        indicatorBarShow();
    }

    _boxesWereDisabled = 0;

    if (_gdDialogTurnMouseOff) {
        if (!gameUiIsDisabled()) {
            _gmouse_enable();
        }

        _gdDialogTurnMouseOff = 0;
    }

    if (!gameUiIsDisabled()) {
        gameMouseObjectsShow();
    }

    _gdDialogWentOff = true;

    return 0;
}

// 0x445438
void gameDialogSetBackground(int a1)
{
    if (a1 != -1) {
        gGameDialogBackground = a1;
    }
}

// Renders supplementary message in reply area of the dialog.
//
// 0x445448
void gameDialogRenderSupplementaryMessage(char* msg)
{
    if (_gd_replyWin == -1) {
        debugPrint("\nError: Reply window doesn't exist!");
        return;
    }

    _replyRect.left = 5;
    _replyRect.top = 10;
    _replyRect.right = 374;
    _replyRect.bottom = 58;
    _demo_copy_title(gGameDialogReplyWindow);

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogReplyWindow);
    int lineHeight = fontGetLineHeight();

    int a4 = 0;

    // NOTE: Uninline.
    text_to_rect_wrapped(windowBuffer,
        &_replyRect,
        msg,
        &a4,
        lineHeight,
        379,
        _colorTable[992] | 0x2000000);

    windowShow(_gd_replyWin);
    windowRefresh(gGameDialogReplyWindow);
}

// 0x4454FC
int _gdialogStart()
{
    gGameDialogReviewEntriesLength = 0;
    gGameDialogOptionEntriesLength = 0;
    return 0;
}

// 0x445510
int _gdialogSayMessage()
{
    mouseShowCursor();
    _gdialogGo();

    gGameDialogOptionEntriesLength = 0;
    gDialogReplyMessageListId = -1;

    return 0;
}

// NOTE: If you look at the scripts handlers, my best guess that their intention
// was to allow scripters to specify proc names instead of proc addresses. They
// dropped this idea, probably because they've updated their own compiler, or
// maybe there was not enough time to complete it. Any way, [procedure] is the
// identifier of the procedure in the script, but it is silently ignored.
//
// 0x445538
int gameDialogAddMessageOptionWithProcIdentifier(int messageListId, int messageId, const char* proc, int reaction)
{
    gDialogOptionEntries[gGameDialogOptionEntriesLength].proc = 0;

    return gameDialogAddMessageOption(messageListId, messageId, reaction);
}

// NOTE: If you look at the script handlers, my best guess that their intention
// was to allow scripters to specify proc names instead of proc addresses. They
// dropped this idea, probably because they've updated their own compiler, or
// maybe there was not enough time to complete it. Any way, [procedure] is the
// identifier of the procedure in the script, but it is silently ignored.
//
// 0x445578
int gameDialogAddTextOptionWithProcIdentifier(int messageListId, const char* text, const char* proc, int reaction)
{
    gDialogOptionEntries[gGameDialogOptionEntriesLength].proc = 0;

    return gameDialogAddTextOption(messageListId, text, reaction);
}

// 0x4455B8
int gameDialogAddMessageOptionWithProc(int messageListId, int messageId, int proc, int reaction)
{
    gDialogOptionEntries[gGameDialogOptionEntriesLength].proc = proc;

    return gameDialogAddMessageOption(messageListId, messageId, reaction);
}

// 0x4455FC
int gameDialogAddTextOptionWithProc(int messageListId, const char* text, int proc, int reaction)
{
    gDialogOptionEntries[gGameDialogOptionEntriesLength].proc = proc;

    return gameDialogAddTextOption(messageListId, text, reaction);
}

// 0x445640
int gameDialogSetMessageReply(Program* program, int messageListId, int messageId)
{
    gDialogPendingAudio[0] = '\0';
    gameDialogAddReviewMessage(messageListId, messageId);

    gDialogReplyProgram = program;
    gDialogReplyMessageListId = messageListId;
    gDialogReplyMessageId = messageId;
    dword_58F4E0 = 0;
    gDialogReplyText[0] = '\0';
    gGameDialogOptionEntriesLength = 0;

    return 0;
}

// 0x44567C
int gameDialogSetTextReply(Program* program, int messageListId, const char* text)
{
    gameDialogAddReviewText(text);

    gDialogReplyProgram = program;
    dword_58F4E0 = 0;
    gDialogReplyMessageListId = -4;
    gDialogReplyMessageId = -4;

    strcpy(gDialogReplyText, text);

    gGameDialogOptionEntriesLength = 0;

    return 0;
}

// 0x4456D8
int _gdialogGo()
{
    if (gDialogReplyMessageListId == -1) {
        return 0;
    }

    int rc = 0;

    if (gGameDialogOptionEntriesLength < 1) {
        gDialogOptionEntries[gGameDialogOptionEntriesLength].proc = 0;

        if (gameDialogAddMessageOption(-1, -1, 50) == -1) {
            programFatalError("Error setting option.");
            rc = -1;
        }
    }

    if (rc != -1) {
        rc = _gdProcess();
    }

    gGameDialogOptionEntriesLength = 0;

    return rc;
}

// 0x445764
void _gdialogUpdatePartyStatus()
{
    if (_dialogue_state != 1) {
        return;
    }

    bool isPartyMember = objectIsPartyMember(gGameDialogSpeaker);
    if (isPartyMember == gGameDialogSpeakerIsPartyMember) {
        return;
    }

    // NOTE: Uninline.
    gdHide();

    GameMode::enterGameMode(GameMode::kSpecial);

    _gdialog_window_destroy();

    gGameDialogSpeakerIsPartyMember = isPartyMember;

    GameMode::exitGameMode(GameMode::kSpecial);

    _gdialog_window_create();

    // NOTE: Uninline.
    gdUnhide();
}

// NOTE: Inlined.
//
// 0x4457EC
static int gdHide()
{
    if (_gd_replyWin != -1) {
        windowHide(_gd_replyWin);
    }

    if (_gd_optionsWin != -1) {
        windowHide(_gd_optionsWin);
    }

    return 0;
}

// NOTE: Inlined.
//
// 0x445818
static int gdUnhide()
{
    if (_gd_replyWin != -1) {
        windowShow(_gd_replyWin);
    }

    if (_gd_optionsWin != -1) {
        windowShow(_gd_optionsWin);
    }

    return 0;
}

// 0x44585C
int gameDialogAddMessageOption(int messageListId, int messageId, int reaction)
{
    if (gGameDialogOptionEntriesLength >= DIALOG_OPTION_ENTRIES_CAPACITY) {
        debugPrint("\nError: dialog: Ran out of options!");
        return -1;
    }

    GameDialogOptionEntry* optionEntry = &(gDialogOptionEntries[gGameDialogOptionEntriesLength]);
    optionEntry->messageListId = messageListId;
    optionEntry->messageId = messageId;
    optionEntry->reaction = reaction;
    optionEntry->btn = -1;
    optionEntry->text[0] = '\0';

    gGameDialogOptionEntriesLength++;

    return 0;
}

// 0x4458BC
int gameDialogAddTextOption(int messageListId, const char* text, int reaction)
{
    if (gGameDialogOptionEntriesLength >= DIALOG_OPTION_ENTRIES_CAPACITY) {
        debugPrint("\nError: dialog: Ran out of options!");
        return -1;
    }

    GameDialogOptionEntry* optionEntry = &(gDialogOptionEntries[gGameDialogOptionEntriesLength]);
    optionEntry->messageListId = -4;
    optionEntry->messageId = -4;
    optionEntry->reaction = reaction;
    optionEntry->btn = -1;

    // SFALL
    if (gNumberOptions) {
        snprintf(optionEntry->text, sizeof(optionEntry->text), "%d. %s", gGameDialogOptionEntriesLength + 1, text);
    } else {
        snprintf(optionEntry->text, sizeof(optionEntry->text), "%c %s", '\x95', text);
    }

    gGameDialogOptionEntriesLength++;

    return 0;
}

// 0x445938
int gameDialogReviewWindowInit(int* win)
{
    if (gGameDialogLipSyncStarted) {
        if (soundIsPlaying(gLipsData.sound)) {
            gameDialogEndLips();
        }
    }

    gGameDialogReviewWindowOldFont = fontGetCurrent();

    if (win == nullptr) {
        return -1;
    }

    int reviewWindowX = (screenGetWidth() - GAME_DIALOG_REVIEW_WINDOW_WIDTH) / 2;
    int reviewWindowY = (screenGetHeight() - GAME_DIALOG_REVIEW_WINDOW_HEIGHT) / 2;
    *win = windowCreate(reviewWindowX,
        reviewWindowY,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        GAME_DIALOG_REVIEW_WINDOW_HEIGHT,
        256,
        WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (*win == -1) {
        return -1;
    }

    FrmImage backgroundFrmImage;
    int fid = buildFid(OBJ_TYPE_INTERFACE, 102, 0, 0, 0);
    if (!backgroundFrmImage.lock(fid)) {
        windowDestroy(*win);
        *win = -1;
        return -1;
    }

    unsigned char* windowBuffer = windowGetBuffer(*win);
    blitBufferToBuffer(backgroundFrmImage.getData(),
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        GAME_DIALOG_REVIEW_WINDOW_HEIGHT,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH,
        windowBuffer,
        GAME_DIALOG_REVIEW_WINDOW_WIDTH);

    backgroundFrmImage.unlock();

    int index;
    for (index = 0; index < GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT; index++) {
        int fid = buildFid(OBJ_TYPE_INTERFACE, gGameDialogReviewWindowButtonFrmIds[index], 0, 0, 0);
        if (!_reviewFrmImages[index].lock(fid)) {
            break;
        }
    }

    if (index != GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT) {
        gameDialogReviewWindowFree(win);
        return -1;
    }

    int upBtn = buttonCreate(*win,
        475,
        152,
        gGameDialogReviewWindowButtonWidths[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP],
        gGameDialogReviewWindowButtonHeights[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_UP],
        -1,
        -1,
        -1,
        KEY_ARROW_UP,
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_NORMAL].getData(),
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_UP_PRESSED].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (upBtn == -1) {
        gameDialogReviewWindowFree(win);
        return -1;
    }

    buttonSetCallbacks(upBtn, _gsound_med_butt_press, _gsound_med_butt_release);

    int downBtn = buttonCreate(*win,
        475,
        191,
        gGameDialogReviewWindowButtonWidths[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN],
        gGameDialogReviewWindowButtonHeights[GAME_DIALOG_REVIEW_WINDOW_BUTTON_SCROLL_DOWN],
        -1,
        -1,
        -1,
        KEY_ARROW_DOWN,
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_NORMAL].getData(),
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_ARROW_DOWN_PRESSED].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (downBtn == -1) {
        gameDialogReviewWindowFree(win);
        return -1;
    }

    buttonSetCallbacks(downBtn, _gsound_med_butt_press, _gsound_med_butt_release);

    int doneBtn = buttonCreate(*win,
        499,
        398,
        gGameDialogReviewWindowButtonWidths[GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE],
        gGameDialogReviewWindowButtonHeights[GAME_DIALOG_REVIEW_WINDOW_BUTTON_DONE],
        -1,
        -1,
        -1,
        KEY_ESCAPE,
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_NORMAL].getData(),
        _reviewFrmImages[GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_DONE_PRESSED].getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (doneBtn == -1) {
        gameDialogReviewWindowFree(win);
        return -1;
    }

    buttonSetCallbacks(doneBtn, _gsound_red_butt_press, _gsound_red_butt_release);

    fontSetCurrent(101);

    windowRefresh(*win);

    tickersRemove(gameDialogTicker);

    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 102, 0, 0, 0);
    if (!_reviewBackgroundFrmImage.lock(backgroundFid)) {
        gameDialogReviewWindowFree(win);
        return -1;
    }

    return 0;
}

// 0x445C18
int gameDialogReviewWindowFree(int* win)
{
    tickersAdd(gameDialogTicker);

    for (int index = 0; index < GAME_DIALOG_REVIEW_WINDOW_BUTTON_FRM_COUNT; index++) {
        _reviewFrmImages[index].unlock();
    }

    _reviewBackgroundFrmImage.unlock();

    fontSetCurrent(gGameDialogReviewWindowOldFont);

    if (win == nullptr) {
        return -1;
    }

    windowDestroy(*win);
    *win = -1;

    return 0;
}

// 0x445CA0
int gameDialogShowReview()
{
    ScopedGameMode gm(GameMode::kDialogReview);

    int win;

    if (gameDialogReviewWindowInit(&win) == -1) {
        debugPrint("\nError initializing review window!");
        return -1;
    }

    // probably current top line or something like this, which is used to scroll
    int v1 = 0;
    gameDialogReviewWindowUpdate(win, v1);

    while (true) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode == 17 || keyCode == 24 || keyCode == 324) {
            showQuitConfirmationDialog();
        }

        if (_game_user_wants_to_quit != 0 || keyCode == KEY_ESCAPE) {
            break;
        }

        // likely scrolling
        if (keyCode == 328) {
            v1 -= 1;
            if (v1 >= 0) {
                gameDialogReviewWindowUpdate(win, v1);
            } else {
                v1 = 0;
            }
        } else if (keyCode == 336) {
            v1 += 1;
            if (v1 <= gGameDialogReviewEntriesLength - 1) {
                gameDialogReviewWindowUpdate(win, v1);
            } else {
                v1 = gGameDialogReviewEntriesLength - 1;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (gameDialogReviewWindowFree(&win) == -1) {
        return -1;
    }

    return 0;
}

// NOTE: Uncollapsed 0x445CA0 with different signature.
void gameDialogReviewButtonOnMouseUp(int btn, int keyCode)
{
    gameDialogShowReview();
}

// 0x445D44
void gameDialogReviewWindowUpdate(int win, int origin)
{
    Rect entriesRect;
    entriesRect.left = 113;
    entriesRect.top = 76;
    entriesRect.right = 422;
    entriesRect.bottom = 418;

    int v20 = fontGetLineHeight() + 2;
    unsigned char* windowBuffer = windowGetBuffer(win);
    if (windowBuffer == nullptr) {
        debugPrint("\nError: gdialog: review: can't find buffer!");
        return;
    }

    int width = GAME_DIALOG_WINDOW_WIDTH;
    blitBufferToBuffer(
        _reviewBackgroundFrmImage.getData() + width * entriesRect.top + entriesRect.left,
        width,
        entriesRect.bottom - entriesRect.top + 15,
        width,
        windowBuffer + width * entriesRect.top + entriesRect.left,
        width);

    int y = 76;
    for (int index = origin; index < gGameDialogReviewEntriesLength; index++) {
        GameDialogReviewEntry* dialogReviewEntry = &(gDialogReviewEntries[index]);

        char name[60];
        snprintf(name, sizeof(name), "%s:", objectGetName(gGameDialogSpeaker));
        windowDrawText(win, name, 180, 88, y, _colorTable[992] | 0x2000000);
        entriesRect.top += v20;

        char* replyText;
        if (dialogReviewEntry->replyMessageListId <= -3) {
            replyText = dialogReviewEntry->replyText;
        } else {
            replyText = _scr_get_msg_str(dialogReviewEntry->replyMessageListId, dialogReviewEntry->replyMessageId);
        }

        if (replyText == nullptr) {
            showMesageBox("\nGDialog::Error Grabbing text message!");
            exit(1);
        }

        // NOTE: Uninline.
        y = text_to_rect_wrapped(windowBuffer + 113,
            &entriesRect,
            replyText,
            nullptr,
            fontGetLineHeight(),
            640,
            _colorTable[768] | 0x2000000);

        // SFALL: Cosmetic fix to the dialog review interface to prevent the
        // player name from being displayed at the bottom of the window when the
        // text is longer than one screen.
        if (y >= 407) {
            break;
        }

        if (dialogReviewEntry->optionMessageListId != -3) {
            snprintf(name, sizeof(name), "%s:", objectGetName(gDude));
            windowDrawText(win, name, 180, 88, y, _colorTable[21140] | 0x2000000);
            entriesRect.top += v20;

            char* optionText;
            if (dialogReviewEntry->optionMessageListId <= -3) {
                optionText = dialogReviewEntry->optionText;
            } else {
                optionText = _scr_get_msg_str(dialogReviewEntry->optionMessageListId, dialogReviewEntry->optionMessageId);
            }

            if (optionText == nullptr) {
                showMesageBox("\nGDialog::Error Grabbing text message!");
                exit(1);
            }

            // NOTE: Uninline.
            y = text_to_rect_wrapped(windowBuffer + 113,
                &entriesRect,
                optionText,
                nullptr,
                fontGetLineHeight(),
                640,
                _colorTable[15855] | 0x2000000);
        }

        if (y >= 407) {
            break;
        }
    }

    entriesRect.left = 88;
    entriesRect.top = 76;
    entriesRect.bottom += 14;
    entriesRect.right = 434;
    windowRefreshRect(win, &entriesRect);
}

// 0x445FDC
void dialogReviewEntriesClear()
{
    for (int index = 0; index < gGameDialogReviewEntriesLength; index++) {
        GameDialogReviewEntry* entry = &(gDialogReviewEntries[index]);
        entry->replyMessageListId = 0;
        entry->replyMessageId = 0;

        if (entry->replyText != nullptr) {
            internal_free(entry->replyText);
            entry->replyText = nullptr;
        }

        entry->optionMessageListId = 0;
        entry->optionMessageId = 0;
    }
}

// 0x446040
int gameDialogAddReviewMessage(int messageListId, int messageId)
{
    if (gGameDialogReviewEntriesLength >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debugPrint("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(gDialogReviewEntries[gGameDialogReviewEntriesLength]);
    entry->replyMessageListId = messageListId;
    entry->replyMessageId = messageId;

    // NOTE: I'm not sure why there are two consequtive assignments.
    entry->optionMessageListId = -1;
    entry->optionMessageId = -1;

    entry->optionMessageListId = -3;
    entry->optionMessageId = -3;

    gGameDialogReviewEntriesLength++;

    return 0;
}

// 0x4460B4
int gameDialogAddReviewText(const char* string)
{
    if (gGameDialogReviewEntriesLength >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debugPrint("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(gDialogReviewEntries[gGameDialogReviewEntriesLength]);
    entry->replyMessageListId = -4;
    entry->replyMessageId = -4;

    if (entry->replyText != nullptr) {
        internal_free(entry->replyText);
        entry->replyText = nullptr;
    }

    entry->replyText = (char*)internal_malloc(strlen(string) + 1);
    strcpy(entry->replyText, string);

    entry->optionMessageListId = -3;
    entry->optionMessageId = -3;
    entry->optionText = nullptr;

    gGameDialogReviewEntriesLength++;

    return 0;
}

// 0x4461A4
int gameDialogSetReviewOptionMessage(int messageListId, int messageId)
{
    if (gGameDialogReviewEntriesLength >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debugPrint("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(gDialogReviewEntries[gGameDialogReviewEntriesLength - 1]);
    entry->optionMessageListId = messageListId;
    entry->optionMessageId = messageId;
    entry->optionText = nullptr;

    return 0;
}

// 0x4461F0
int gameDialogSetReviewOptionText(const char* string)
{
    if (gGameDialogReviewEntriesLength >= DIALOG_REVIEW_ENTRIES_CAPACITY) {
        debugPrint("\nError: Ran out of review slots!");
        return -1;
    }

    GameDialogReviewEntry* entry = &(gDialogReviewEntries[gGameDialogReviewEntriesLength - 1]);
    entry->optionMessageListId = -4;
    entry->optionMessageId = -4;

    entry->optionText = (char*)internal_malloc(strlen(string) + 1);
    strcpy(entry->optionText, string);

    return 0;
}

// Creates dialog interface.
//
// 0x446288
int _gdProcessInit()
{
    // Headless: create no reply/options windows or scroll buttons. The server
    // _gdProcess branch reads gDialogOptionEntries directly and never renders,
    // so the -1 windows are never touched.
    if (serverLoopActive()) {
        return 0;
    }

    int upBtn;
    int downBtn;
    int optionsWindowX;
    int optionsWindowY;

    int replyWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2 + GAME_DIALOG_REPLY_WINDOW_X;
    int replyWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_REPLY_WINDOW_Y;
    gGameDialogReplyWindow = windowCreate(replyWindowX,
        replyWindowY,
        GAME_DIALOG_REPLY_WINDOW_WIDTH,
        GAME_DIALOG_REPLY_WINDOW_HEIGHT,
        256,
        WINDOW_MOVE_ON_TOP);
    if (gGameDialogReplyWindow == -1) {
        goto err;
    }

    // Top part of the reply window - scroll up.
    upBtn = buttonCreate(gGameDialogReplyWindow, 1, 1, 377, 28, -1, -1, KEY_ARROW_UP, -1, nullptr, nullptr, nullptr, 32);
    if (upBtn == -1) {
        goto err_1;
    }

    buttonSetCallbacks(upBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    buttonSetMouseCallbacks(upBtn, _reply_arrow_up, _reply_arrow_restore, nullptr, nullptr);

    // Bottom part of the reply window - scroll down.
    downBtn = buttonCreate(gGameDialogReplyWindow, 1, 29, 377, 28, -1, -1, KEY_ARROW_DOWN, -1, nullptr, nullptr, nullptr, 32);
    if (downBtn == -1) {
        goto err_1;
    }

    buttonSetCallbacks(downBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    buttonSetMouseCallbacks(downBtn, _reply_arrow_down, _reply_arrow_restore, nullptr, nullptr);

    optionsWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2 + GAME_DIALOG_OPTIONS_WINDOW_X;
    optionsWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_OPTIONS_WINDOW_Y;
    gGameDialogOptionsWindow = windowCreate(optionsWindowX, optionsWindowY, GAME_DIALOG_OPTIONS_WINDOW_WIDTH, GAME_DIALOG_OPTIONS_WINDOW_HEIGHT, 256, WINDOW_MOVE_ON_TOP);
    if (gGameDialogOptionsWindow == -1) {
        goto err_2;
    }

    // CE: Move red buttons init to `_gdialogInitFromScript`.

    _talkOldFont = fontGetCurrent();
    fontSetCurrent(101);

    return 0;

err_2:

    windowDestroy(gGameDialogOptionsWindow);
    gGameDialogOptionsWindow = -1;

err_1:

    windowDestroy(gGameDialogReplyWindow);
    gGameDialogReplyWindow = -1;

err:

    return -1;
}

// RELASE: Rename/comment.
// free dialog option buttons
// 0x446454
void _gdProcessCleanup()
{
    for (int index = 0; index < gGameDialogOptionEntriesLength; index++) {
        GameDialogOptionEntry* optionEntry = &(gDialogOptionEntries[index]);

        if (optionEntry->btn != -1) {
            buttonDestroy(optionEntry->btn);
            optionEntry->btn = -1;
        }
    }
}

// RELASE: Rename/comment.
// free dialog interface
// 0x446498
int _gdProcessExit()
{
    // Headless server (A2): mirror the serverLoopActive() guard in _gdProcessInit —
    // no reply/options windows or option buttons were ever created, so there is
    // nothing to clean up or destroy. Skipping avoids the windowDestroy abort.
    if (serverLoopActive()) {
        return 0;
    }

    _gdProcessCleanup();

    // CE: Move red buttons exit to `_gdialogExitFromScript`.

    windowDestroy(gGameDialogReplyWindow);
    gGameDialogReplyWindow = -1;

    windowDestroy(gGameDialogOptionsWindow);
    gGameDialogOptionsWindow = -1;

    fontSetCurrent(_talkOldFont);

    return 0;
}

// 0x446504
void gameDialogRenderCaps()
{
    Rect rect;
    rect.left = 5;
    rect.right = 70;
    rect.top = 36;
    rect.bottom = fontGetLineHeight() + 36;

    _talkToRefreshDialogWindowRect(&rect);

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    int caps = itemGetTotalCaps(gDude);
    char text[20];
    snprintf(text, sizeof(text), "$%d", caps);

    int width = fontGetStringWidth(text);
    if (width > 60) {
        width = 60;
    }

    windowDrawText(gGameDialogWindow, text, width, 38 - width / 2, 36, _colorTable[992] | 0x7000000);

    fontSetCurrent(oldFont);
}

void gameDialogSetServerPump(std::function<bool()> pump)
{
    gDialogServerPump = std::move(pump);
}

bool gameDialogServerNodeActive()
{
    return gDialogServerNodeDepth > 0;
}

// Ship the CURRENT dialog node (reply + resolved option texts) to viewers over the
// presentation seam. Called at the top of each live server-barrier iteration, after
// the node's reply/options have been populated (_gdProcessUpdate at entry, then
// _gdProcessChoice after each selection). Options resolve through the SAME
// gameDialogGetOptionText the renderer uses (Stage 0), so server and viewer can never
// drift. `driver` is the talking actor (gDude in v1; the barrier stays actor-agnostic
// otherwise). `reaction` carries the script's real fidget level and `headFid` the
// script's real head — the viewer cannot derive either (proto head_fid is unset). Pure reads + local buffers → no sim mutation, no RNG.
static void dialogEmitNode()
{
    static char optionText[DIALOG_OPTION_ENTRIES_CAPACITY][900];
    const char* optionPtrs[DIALOG_OPTION_ENTRIES_CAPACITY];

    int count = gGameDialogOptionEntriesLength;
    if (count < 0) count = 0;
    if (count > DIALOG_OPTION_ENTRIES_CAPACITY) count = DIALOG_OPTION_ENTRIES_CAPACITY;

    for (int i = 0; i < count; i++) {
        optionText[i][0] = '\0';
        gameDialogGetOptionText(i, optionText[i], sizeof(optionText[i]));
        optionPtrs[i] = optionText[i];
    }

    presenter()->dialogNode(gGameDialogSpeaker, gDude, gGameDialogFidget,
        gDialogReplyText, optionPtrs, count,
        gDialogPendingAudio[0] != '\0' ? gDialogPendingAudio : nullptr,
        gGameDialogHeadFid);
}

// 0x4465C0
int _gdProcess()
{
    if (_gdReenterLevel == 0) {
        if (_gdProcessInit() == -1) {
            return -1;
        }
    }

    _gdReenterLevel += 1;

    // ►► THE dsay/dend GATE'S GROUND TRUTH. Raised for exactly as long as this
    // conversation can consume a dialog intent.
    //
    // It exists because _gdialogActive() (i.e. _dialog_state_fix) is the WRONG
    // question and answering it caused a hard deadlock. That flag is set only by
    // gameDialogEnter — the engine's "player used TALK on an NPC" path. But a
    // script can run a whole conversation through gsay_start/gsay_end, which
    // reaches _gdProcess via _gdialogGo() with gameDialogEnter never involved and
    // _dialog_state_fix still 0. denbus2/Sheila's paid path does exactly that for
    // its SECOND conversation (the one after the teleport + game_time_advance).
    //
    // Vanilla never noticed: _gdProcess reads the keyboard directly. Our server
    // gate did notice — it rejected every dsay/dend as "no active dialog" while
    // this very function sat parked in the barrier waiting for one. The client
    // showed a node whose buttons did nothing and the world stayed frozen.
    //
    // Depth-counted, not a bool: conversations nest (_gdReenterLevel).
    gDialogServerNodeDepth++;

    // Conversation lifecycle bracket. Pairs with the "[dialog] LEAVE" line at the
    // teardown below, so a log shows exactly which conversation a stranded node
    // belonged to and whether the server ever left it — the "server is parked in a
    // dialog the client is not showing" limbo is invisible without this.
    if (gDialogServerPump != nullptr) {
        fprintf(stderr, "f2_server: [dialog] ENTER _gdProcess reenter=%d inCombat=%d\n",
            _gdReenterLevel, isInCombat() ? 1 : 0);
    }

    _gdProcessUpdate();

    int v18 = 0;
    if (dword_58F4E0 != 0) {
        v18 = 1;
        _gdReplyTooBig = 1;
    }

    unsigned int tick = getTicks();
    int pageCount = 0;
    int pageIndex = 0;
    int pageOffsets[10];
    pageOffsets[0] = 0;
    for (;;) {
        if (serverLoopActive()) {
            // Headless: no keyboard/render/fps loop. Drain the dialog intent
            // queue, calling _gdProcessChoice directly — the same core path a
            // numeric-key press drives at line ~1992 below. The initial node's
            // options are already registered (gDialogOptionEntries, populated by
            // the gsay_option opcodes before gsay_end reached _gdProcess).
            if (getenv("F2_DIALOG_TRACE") != nullptr) {
                debugPrint("\n[dtrace] node: %d options; reply=\"%.80s\"\n",
                    gGameDialogOptionEntriesLength, gDialogReplyText);
                for (int oi = 0; oi < gGameDialogOptionEntriesLength; oi++) {
                    debugPrint("[dtrace]   opt %d: \"%.70s\"\n", oi, gDialogOptionEntries[oi].text);
                }
            }
            // LIVE (dedicated server, pump installed): ship the current node to the
            // viewers so the OWNER renders it editable and spectators read-only.
            // Inert on the golden/headless path (pump == nullptr).
            if (gDialogServerPump != nullptr) {
                dialogEmitNode();
            }

            DialogIntent intent;
            bool haveIntent = dialogIntentPeek(&intent);
            if (gDialogServerPump != nullptr) {
                // Block-and-pump: park the server tick here for the whole node and
                // service the control channel until the owner's dsay/dend lands, or
                // the pump signals a bail (owner disconnect / combat start / quit).
                // MP_PROTOCOL §1: a dialog choice is an accepted input barrier, and
                // vanilla freezes the world during dialog anyway.
                while (!haveIntent) {
                    if (!gDialogServerPump()) {
                        break;
                    }
                    haveIntent = dialogIntentPeek(&intent);
                }
            }
            if (!haveIntent) {
                // Empty queue with no live pump = the original headless behavior (a
                // well-formed golden fixture pre-queues one intent per node plus a
                // terminating choice); OR a live bail. Either way, end the
                // conversation — dialogEnd is emitted in the reenter==0 teardown.
                //
                // ►► LIVE, this is the bug path: dialogEmitNode() above already
                // shipped this node to the viewer, which is now rendering options
                // the server will never read. The pump logs WHY it bailed.
                if (gDialogServerPump != nullptr) {
                    fprintf(stderr, "f2_server: [dialog] ABANDONED a node the client "
                                    "is already showing (%d options, reenter=%d)\n",
                        gGameDialogOptionEntriesLength, _gdReenterLevel);
                }
                break;
            }
            dialogIntentPop();

            if (intent.kind == DIALOG_INTENT_END) {
                break;
            }

            int choiceResult = 0;
            if (intent.kind == DIALOG_INTENT_BARTER) {
                // The routed Barter button (server_control dbarter). WHETHER this
                // speaker barters is ours to decide, exactly as the interactive
                // button handler does it (gameDialogBarterButtonUpMouseUp): a
                // CRITTER_BARTER critter opens the trade; anyone else gets the
                // "will not barter" refusal and the conversation continues. We set
                // mode 2 directly rather than calling the button handler, which
                // also does gdHide()/lip teardown that has no headless meaning.
                bool willBarter = false;
                if (PID_TYPE(gGameDialogSpeaker->pid) == OBJ_TYPE_CRITTER) {
                    Proto* sproto = nullptr;
                    if (protoGetProto(gGameDialogSpeaker->pid, &sproto) != -1
                        && (sproto->critter.data.flags & CRITTER_BARTER) != 0) {
                        willBarter = true;
                    }
                }
                if (willBarter) {
                    _dialogue_switch_mode = 2;
                    _dialogue_state = 4;
                    // falls into the mode==2 barter block below
                } else {
                    // proto msg 903 "This person will not barter with you." Answer
                    // the driver only — it is a refusal, nothing happened for anyone
                    // else. gDude is the driver here (the pump's ServerActorScope).
                    MessageListItem m;
                    m.num = gGameDialogSpeakerIsPartyMember ? 913 : 903;
                    if (messageListGetItem(&gProtoMessageList, &m)) {
                        presenter()->consoleMessageStyled(
                            gDude != nullptr ? gDude->netId : 0, kMsgChannelRefusal, m.text);
                    }
                    continue; // stay in the conversation
                }
            } else {
                // DIALOG_INTENT_SELECT: bounds-check against the current node's
                // option count (mirrors the `v11 < gGameDialogOptionEntriesLength`
                // guard on the keyboard path).
                if (intent.arg < 0 || intent.arg >= gGameDialogOptionEntriesLength) {
                    debugPrint("\nserver dialog: option %d out of range [0,%d) - ending\n",
                        intent.arg, gGameDialogOptionEntriesLength);
                    break;
                }

                choiceResult = _gdProcessChoice(intent.arg);
            }

            // A choice's proc can switch into a nested blocking mode: barter
            // (2), party control (8/9), or party customization (11/12). Barter
            // is driven headless; the party modes are UI-button-only and remain
            // unsupported here. This MUST be tested before the choiceResult == -1
            // end check below: the barter option's proc registers no new dialog
            // options (barter replaces them), so _gdProcessChoice returns -1 even
            // though barter must still run — breaking first would skip it.
            if (_dialogue_switch_mode == 2) {
                // Headless barter. The interactive path runs this across two
                // steps: gameDialogTicker case 2 (mode 2->3: enterGameMode +
                // _gdialog_barter_create_win) then the mode==3 branch below
                // (exitGameMode + inventoryOpenTrade + cleanup + destroy). We
                // BYPASS the ticker (a client-tick callback that would build the
                // SDL barter window) and drive create -> trade -> destroy inline.
                // The create/destroy win fns are serverLoopActive-guarded to
                // allocate/free ONLY the three hidden table objects (no SDL), so
                // the enterGameMode/exitGameMode(kSpecial) pair — a no-op round
                // trip here — is elided.
                _gdialog_barter_create_win();
                inventoryOpenTrade(gGameDialogWindow, gGameDialogSpeaker, _peon_table_obj, _barterer_table_obj, gGameDialogBarterModifier);
                _gdialog_barter_cleanup_tables();
                _gdialog_barter_destroy_win();

                // inventoryOpenTrade's DONE path already ran _barter_end_to_talk_to
                // (which closed the dialog programs and set mode=1, state=1). A
                // CANCEL/empty-queue path leaves mode==2 and state==4. Normalize
                // BOTH — mirroring the interactive mode==3 branch, which always
                // ends barter at state==1 (see the `if (v5 == 4)` block). mode=0
                // so the subsequent _gdialogExitFromScript teardown does NOT
                // short-circuit (it returns early for modes 2/8/11); state=1 so a
                // leftover state==4 can't misroute _gdDestroyHeadWindow /
                // gameDialogEnter teardown into the barter-recovery path.
                _dialogue_switch_mode = 0;
                _dialogue_state = 1;

                // ►► CONTINUE, NOT BREAK. Leaving a trade RETURNS YOU TO THE
                // CONVERSATION -- that is what the 'T' button means, and what
                // _barter_end_to_talk_to is named after. The interactive path says
                // so explicitly: its barter branch ends in `continue` (:2189), and
                // this one used to end in `break`, hanging up on the merchant the
                // moment a trade closed. Owner-reported: "trade -> back should pop
                // the same dialog as the initial one".
                //
                // Looping also re-emits the node (dialogEmitNode runs at the top of
                // each iteration), which is what repopulates every viewer's option
                // list -- without it the viewer holds an empty dialog window that
                // no node will ever fill.
                continue;
            }

            if (_dialogue_switch_mode != 0) {
                debugPrint("\nserver dialog: option requested nested mode %d (party) - aborting\n",
                    _dialogue_switch_mode);
                // Reset the pending switch mode BEFORE breaking: otherwise the
                // subsequent end_dialogue -> _gdialogExitFromScript short-circuits
                // (it returns early when mode is 2/8/11), leaving gameDialogTicker
                // registered — which would then try to build a party SDL window
                // on the next server tick. Clearing it lets the normal teardown
                // (tickersRemove + GameMode pop) run.
                _dialogue_switch_mode = 0;
                break;
            }

            if (choiceResult == -1) {
                // The option's proc registered no new options and switched into
                // no nested mode: conversation ended (same semantics as the
                // keyboard path's break at ~1992).
                break;
            }

            continue;
        }

        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();

        convertMouseWheelToArrowKey(&keyCode);

        if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
            showQuitConfirmationDialog();
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        if (keyCode == KEY_CTRL_B && !_mouse_click_in(135, 225, 514, 283)) {
            if (gameMouseGetCursor() != MOUSE_CURSOR_ARROW) {
                gameMouseSetCursor(MOUSE_CURSOR_ARROW);
            }
        } else {
            if (_dialogue_switch_mode == 3) {
                _dialogue_state = 4;

                GameMode::exitGameMode(GameMode::kSpecial);

                inventoryOpenTrade(gGameDialogWindow, gGameDialogSpeaker, _peon_table_obj, _barterer_table_obj, gGameDialogBarterModifier);
                _gdialog_barter_cleanup_tables();

                int v5 = _dialogue_state;
                _gdialog_barter_destroy_win();
                _dialogue_state = v5;

                if (v5 == 4) {
                    _dialogue_switch_mode = 1;
                    _dialogue_state = 1;
                }
                continue;
            } else if (_dialogue_switch_mode == 9) {
                _dialogue_state = 10;
                partyMemberControlWindowHandleEvents();
                partyMemberControlWindowFree();
                continue;
            } else if (_dialogue_switch_mode == 12) {
                _dialogue_state = 13;
                partyMemberCustomizationWindowHandleEvents();
                partyMemberCustomizationWindowFree();
                continue;
            }

            if (keyCode == KEY_LOWERCASE_B) {
                gameDialogBarterButtonUpMouseUp(-1, -1);
            }
        }

        if (_gdReplyTooBig) {
            unsigned int v6 = _get_bk_time();
            if (v18) {
                if (getTicksBetween(v6, tick) >= 10000 || keyCode == KEY_SPACE) {
                    pageCount++;
                    pageIndex++;
                    pageOffsets[pageCount] = dword_58F4E0;
                    gameDialogRenderReply();
                    tick = v6;
                    if (!dword_58F4E0) {
                        v18 = 0;
                    }
                }
            }

            if (keyCode == KEY_ARROW_UP) {
                if (pageIndex > 0) {
                    pageIndex--;
                    dword_58F4E0 = pageOffsets[pageIndex];
                    v18 = 0;
                    gameDialogRenderReply();
                }
            } else if (keyCode == KEY_ARROW_DOWN) {
                if (pageIndex < pageCount) {
                    pageIndex++;
                    dword_58F4E0 = pageOffsets[pageIndex];
                    v18 = 0;
                    gameDialogRenderReply();
                } else {
                    if (dword_58F4E0 != 0) {
                        tick = v6;
                        pageIndex++;
                        pageCount++;
                        pageOffsets[pageCount] = dword_58F4E0;
                        v18 = 0;
                        gameDialogRenderReply();
                    }
                }
            }
        }

        if (keyCode != -1) {
            if (keyCode >= 1200 && keyCode <= 1250) {
                gameDialogOptionOnMouseEnter(keyCode - 1200);
            } else if (keyCode >= 1300 && keyCode <= 1330) {
                gameDialogOptionOnMouseExit(keyCode - 1300);
            } else if (keyCode >= 48 && keyCode <= 57) {
                // SFALL: Prevents from using 0 to escape from dialogue at any time.
                if (keyCode == KEY_0 && gGameDialogFix) {
                    continue;
                }

                int v11 = keyCode - 49;
                if (v11 < gGameDialogOptionEntriesLength) {
                    pageCount = 0;
                    pageIndex = 0;
                    pageOffsets[0] = 0;
                    _gdReplyTooBig = 0;

                    if (_gdProcessChoice(v11) == -1) {
                        break;
                    }

                    tick = getTicks();

                    if (dword_58F4E0) {
                        v18 = 1;
                        _gdReplyTooBig = 1;
                    } else {
                        v18 = 0;
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    _gdReenterLevel -= 1;

    if (gDialogServerNodeDepth > 0) {
        gDialogServerNodeDepth--;
    }

    if (gDialogServerPump != nullptr) {
        fprintf(stderr, "f2_server: [dialog] LEAVE _gdProcess reenter=%d inCombat=%d\n",
            _gdReenterLevel, isInCombat() ? 1 : 0);
    }

    if (_gdReenterLevel == 0) {
        // Headless: a conversation may end (natural -1, explicit END, or nested-
        // mode abort) with unconsumed intents still queued. Drop them so they
        // can't bleed into a later dialog in the same server run (the queue is
        // otherwise cleared only once per serverRun).
        if (serverLoopActive()) {
            dialogIntentClear();
            // Tell every viewer the conversation is over (tear the window down; a
            // spectator can't dismiss early). Only when a live pump drove it — inert
            // on the golden/headless path.
            if (gDialogServerPump != nullptr) {
                presenter()->dialogEnd(gDude);
            }
        }
        if (_gdProcessExit() == -1) {
            return -1;
        }
    }

    return 0;
}

// 0x4468DC
int _gdProcessChoice(int a1)
{
    // FIXME: There is a buffer underread bug when `a1` is -1 (pressing 0 on the
    // keyboard, see `_gdProcess`). When it happens the game looks into unused
    // continuation of `gDialogReplyText` (within 0x58F868-0x58FF70 range) which
    // is initialized to 0 according to C spec. I was not able to replicate the
    // same behaviour by extending gDialogReplyText to 2700 bytes or introduce
    // new 1800 bytes buffer in between, at least not in debug builds. In order
    // to preserve original behaviour this dummy dialog option entry is used.
    GameDialogOptionEntry dummy;
    memset(&dummy, 0, sizeof(dummy));

    mouseHideCursor();
    _gdProcessCleanup();

    GameDialogOptionEntry* dialogOptionEntry = a1 != -1 ? &(gDialogOptionEntries[a1]) : &dummy;
    if (dialogOptionEntry->messageListId == -4) {
        gameDialogSetReviewOptionText(dialogOptionEntry->text);
    } else {
        gameDialogSetReviewOptionMessage(dialogOptionEntry->messageListId, dialogOptionEntry->messageId);
    }

    _can_start_new_fidget = false;

    gameDialogEndLips();

    int v1 = GAME_DIALOG_REACTION_NEUTRAL;
    switch (dialogOptionEntry->reaction) {
    case GAME_DIALOG_REACTION_GOOD:
        v1 = -1;
        break;
    case GAME_DIALOG_REACTION_NEUTRAL:
        v1 = 0;
        break;
    case GAME_DIALOG_REACTION_BAD:
        v1 = 1;
        break;
    default:
        // See 0x446907 in ecx but this branch should be unreachable. Due to the
        // bug described above, this code is reachable.
        v1 = GAME_DIALOG_REACTION_NEUTRAL;
        debugPrint("\nError: dialog: Empathy Perk: invalid reaction!");
        break;
    }

    // Headless: skip the render/hover-highlight of the -1 reply/options
    // windows (gameDialogOptionOnMouseEnter would hit the invalid options
    // window). The reaction update below still runs (its fidget/transition
    // calls self-neutralize on the -1 head window).
    if (!serverLoopActive()) {
        _demo_copy_title(gGameDialogReplyWindow);
        _demo_copy_options(gGameDialogOptionsWindow);
        windowRefresh(gGameDialogReplyWindow);
        windowRefresh(gGameDialogOptionsWindow);

        gameDialogOptionOnMouseEnter(a1);
    }
    _talk_to_critter_reacts(v1);

    gGameDialogOptionEntriesLength = 0;

    if (_gdReenterLevel < 2) {
        if (dialogOptionEntry->proc != 0) {
            _executeProcedure(gDialogReplyProgram, dialogOptionEntry->proc);
        }
    }

    mouseShowCursor();

    if (gGameDialogOptionEntriesLength == 0) {
        return -1;
    }

    _gdProcessUpdate();

    return 0;
}

// 0x446A18
void gameDialogOptionOnMouseEnter(int index)
{
    // FIXME: See explanation in `_gdProcessChoice`.
    GameDialogOptionEntry dummy;
    memset(&dummy, 0, sizeof(dummy));

    GameDialogOptionEntry* dialogOptionEntry = index != -1 ? &(gDialogOptionEntries[index]) : &dummy;
    if (dialogOptionEntry->btn == 0) {
        return;
    }

    _optionRect.left = 0;
    _optionRect.top = dialogOptionEntry->top;
    _optionRect.right = 391;
    _optionRect.bottom = dialogOptionEntry->bottom;
    _gDialogRefreshOptionsRect(gGameDialogOptionsWindow, &_optionRect);

    _optionRect.left = 5;
    _optionRect.right = 388;

    int color = _colorTable[32747] | 0x2000000;
    if (perkHasRank(gDude, PERK_EMPATHY)) {
        color = _colorTable[32747] | 0x2000000;
        switch (dialogOptionEntry->reaction) {
        case GAME_DIALOG_REACTION_GOOD:
            color = _colorTable[31775] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_NEUTRAL:
            break;
        case GAME_DIALOG_REACTION_BAD:
            color = _colorTable[32074] | 0x2000000;
            break;
        default:
            debugPrint("\nError: dialog: Empathy Perk: invalid reaction!");
            break;
        }
    }

    // NOTE: Uninline.
    text_to_rect_wrapped(windowGetBuffer(gGameDialogOptionsWindow),
        &_optionRect,
        dialogOptionEntry->text,
        nullptr,
        fontGetLineHeight(),
        393,
        color);

    _optionRect.left = 0;
    _optionRect.right = 391;
    _optionRect.top = dialogOptionEntry->top;
    windowRefreshRect(gGameDialogOptionsWindow, &_optionRect);
}

// 0x446B5C
void gameDialogOptionOnMouseExit(int index)
{
    GameDialogOptionEntry* dialogOptionEntry = &(gDialogOptionEntries[index]);

    _optionRect.left = 0;
    _optionRect.top = dialogOptionEntry->top;
    _optionRect.right = 391;
    _optionRect.bottom = dialogOptionEntry->bottom;
    _gDialogRefreshOptionsRect(gGameDialogOptionsWindow, &_optionRect);

    int color = _colorTable[992] | 0x2000000;
    if (perkGetRank(gDude, PERK_EMPATHY) != 0) {
        color = _colorTable[32747] | 0x2000000;
        switch (dialogOptionEntry->reaction) {
        case GAME_DIALOG_REACTION_GOOD:
            color = _colorTable[31] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_NEUTRAL:
            color = _colorTable[992] | 0x2000000;
            break;
        case GAME_DIALOG_REACTION_BAD:
            color = _colorTable[31744] | 0x2000000;
            break;
        default:
            debugPrint("\nError: dialog: Empathy Perk: invalid reaction!");
            break;
        }
    }

    _optionRect.left = 5;
    _optionRect.right = 388;

    // NOTE: Uninline.
    text_to_rect_wrapped(windowGetBuffer(gGameDialogOptionsWindow),
        &_optionRect,
        dialogOptionEntry->text,
        nullptr,
        fontGetLineHeight(),
        393,
        color);

    _optionRect.right = 391;
    _optionRect.top = dialogOptionEntry->top;
    _optionRect.left = 0;
    windowRefreshRect(gGameDialogOptionsWindow, &_optionRect);
}

// 0x446C94
void gameDialogRenderReply()
{
    _replyRect.left = 5;
    _replyRect.top = 10;
    _replyRect.right = 374;
    _replyRect.bottom = 58;

    // NOTE: There is an unused if condition.
    perkGetRank(gDude, PERK_EMPATHY);

    _demo_copy_title(gGameDialogReplyWindow);

    // NOTE: Uninline.
    text_to_rect_wrapped(windowGetBuffer(gGameDialogReplyWindow),
        &_replyRect,
        gDialogReplyText,
        &dword_58F4E0,
        fontGetLineHeight(),
        379,
        _colorTable[992] | 0x2000000);
    windowRefresh(gGameDialogReplyWindow);
}

// Resolve option `index`'s final display text (including the SFALL number/bullet
// prefix, exactly as the render loop bakes it) into `out`. Shared by the render
// loop and the dialog-streaming emitter so the two can never drift on how an
// option reads. Returns false when a synthetic continuation/done message can't
// be found (the caller aborts the node); a genuinely corrupt script message
// (messageListId >= 0 resolving to null) is fatal, mirroring the render path.
//
// For script text options (messageListId == -4) the text was already baked at
// add time (gameDialogAddTextOption): if `out` aliases the entry's own buffer
// (the render loop passes &entry->text) it is left untouched; otherwise it is
// copied out for the emitter.
bool gameDialogGetOptionText(int index, char* out, size_t size)
{
    GameDialogOptionEntry* dialogOptionEntry = &(gDialogOptionEntries[index]);

    if (dialogOptionEntry->messageListId >= 0) {
        char* text = _scr_get_msg_str_speech(dialogOptionEntry->messageListId, dialogOptionEntry->messageId, 0);
        if (text == nullptr) {
            showMesageBox("\nGDialog::Error Grabbing text message!");
            exit(1);
        }

        // SFALL
        if (gNumberOptions) {
            snprintf(out, size, "%d. %s", index + 1, text);
        } else {
            snprintf(out, size, "%c %s", '\x95', text);
        }
    } else if (dialogOptionEntry->messageListId == -1) {
        MessageListItem messageListItem;
        if (index == 0) {
            // Go on
            messageListItem.num = 655;
            if (critterGetStat(gDude, STAT_INTELLIGENCE) < 4) {
                if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
                    // SFALL
                    if (gNumberOptions) {
                        snprintf(out, size, "%d. %s", index + 1, messageListItem.text);
                    } else {
                        snprintf(out, size, "%s", messageListItem.text);
                    }
                } else {
                    debugPrint("\nError...can't find message!");
                    return false;
                }
            }
        } else {
            // TODO: Why only space?
            // SFALL
            if (gNumberOptions) {
                snprintf(out, size, "%d. %s", index + 1, " ");
            } else {
                strcpy(out, " ");
            }
        }
    } else if (dialogOptionEntry->messageListId == -2) {
        // [Done]
        MessageListItem messageListItem;
        messageListItem.num = 650;
        if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
            // SFALL
            if (gNumberOptions) {
                snprintf(out, size, "%d. %s", index + 1, messageListItem.text);
            } else {
                snprintf(out, size, "%c %s", '\x95', messageListItem.text);
            }
        } else {
            debugPrint("\nError...can't find message!");
            return false;
        }
    } else if (out != dialogOptionEntry->text) {
        // Script text option (messageListId == -4): already baked. Copy for a
        // non-aliasing caller (the emitter); the render loop passes its own
        // buffer and needs no copy.
        snprintf(out, size, "%s", dialogOptionEntry->text);
    }

    return true;
}

void gameDialogSetReplyText(const char* text)
{
    snprintf(gDialogReplyText, sizeof(gDialogReplyText), "%s", text != nullptr ? text : "");
}

void gameDialogClearOptions()
{
    gGameDialogOptionEntriesLength = 0;
}

void gameDialogRenderNode()
{
    if (serverLoopActive() || gGameDialogReplyWindow == -1 || _gdialog_state != 1) {
        return;
    }
    _gdProcessUpdate();
}

int gameDialogInitNodeWindows()
{
    return _gdProcessInit();
}

void gameDialogExitNodeWindows()
{
    _gdProcessExit();
}

// Viewer-side barter window lifecycle, the barter twin of the two above and for
// the same reason: on the viewer we bypass _gdProcess entirely (it would execute
// scripts locally), so the mode switch that normally builds and tears down the
// trade window never runs. The server tells us a trade opened; these put the
// vanilla window on screen so there is something to render the mirrored tables
// into. Also exposes the backing window handle, which inventory_ui needs as
// `_barter_back_win` and which is otherwise a file-static here.
int gameDialogInitBarterWindows()
{
    return _gdialog_barter_create_win();
}

void gameDialogExitBarterWindows()
{
    _gdialog_barter_destroy_win();
}

// The normal-flavor twin of the barter pair above. gGameDialogWindow is a single
// handle: _gdialog_barter_create_win overwrites it and _gdialog_barter_destroy_win
// sets it to -1, so on the viewer (which has no _gdProcess mode machine to swap the
// flavors) the barter boundary must destroy/recreate the normal control explicitly.
int gameDialogInitControlWindow()
{
    int rc = _gdialog_window_create();
    // _gdialog_window_create (unlike _gdCreateHeadWindow) does not set the flavor
    // latch. Restore it to NORMAL so _gdDestroyHeadWindow at conversation end frees
    // this window via _gdialog_window_destroy, not _gdialog_barter_destroy_win.
    _dialogue_state = 1;
    return rc;
}

void gameDialogExitControlWindow()
{
    _gdialog_window_destroy();
}

int gameDialogGetWindow()
{
    return gGameDialogWindow;
}

// True when the dialog SESSION (background/head) window is up. The viewer's trade
// screen is always nested in a live conversation — it reuses this window as its
// backing and _gdialog_barter_create_win blits from its buffer — so opening barter
// with this window gone (a viewer that dropped its dialog out of sync) dereferences
// a dead handle. inventoryOpenTradeViewer checks this and refuses rather than fault.
bool gameDialogBackgroundActive()
{
    return gGameDialogBackgroundWindow != -1;
}

void gameDialogOptionHoverEnter(int index)
{
    if (!serverLoopActive()) {
        gameDialogOptionOnMouseEnter(index);
    }
}

void gameDialogOptionHoverExit(int index)
{
    if (!serverLoopActive()) {
        gameDialogOptionOnMouseExit(index);
    }
}

// 0x446D30
void _gdProcessUpdate()
{
    _replyRect.left = 5;
    _replyRect.top = 10;
    _replyRect.right = 374;
    _replyRect.bottom = 58;

    _optionRect.left = 5;
    _optionRect.top = 5;
    _optionRect.right = 388;
    _optionRect.bottom = 112;

    if (!serverLoopActive()) {
        _demo_copy_title(gGameDialogReplyWindow);
        _demo_copy_options(gGameDialogOptionsWindow);
    }

    if (gDialogReplyMessageListId > 0) {
        char* s = _scr_get_msg_str_speech(gDialogReplyMessageListId, gDialogReplyMessageId, 1);
        if (s == nullptr) {
            showMesageBox("\n'GDialog::Error Grabbing text message!");
            exit(1);
        }

        strncpy(gDialogReplyText, s, sizeof(gDialogReplyText) - 1);
        *(gDialogReplyText + sizeof(gDialogReplyText) - 1) = '\0';
    }

    // Headless: the reply text is now in gDialogReplyText (available to a
    // narrating presenter); skip all rendering, which would dereference the
    // -1 reply/options window buffers.
    if (serverLoopActive()) {
        return;
    }

    gameDialogRenderReply();

    int color = _colorTable[992] | 0x2000000;

    bool hasEmpathy = perkGetRank(gDude, PERK_EMPATHY) != 0;

    int width = _optionRect.right - _optionRect.left - 4;

    int v21 = 0;

    for (int index = 0; index < gGameDialogOptionEntriesLength; index++) {
        GameDialogOptionEntry* dialogOptionEntry = &(gDialogOptionEntries[index]);

        if (hasEmpathy) {
            switch (dialogOptionEntry->reaction) {
            case GAME_DIALOG_REACTION_GOOD:
                color = _colorTable[31] | 0x2000000;
                break;
            case GAME_DIALOG_REACTION_NEUTRAL:
                color = _colorTable[992] | 0x2000000;
                break;
            case GAME_DIALOG_REACTION_BAD:
                color = _colorTable[31744] | 0x2000000;
                break;
            default:
                debugPrint("\nError: dialog: Empathy Perk: invalid reaction!");
                break;
            }
        }

        if (!gameDialogGetOptionText(index, dialogOptionEntry->text, sizeof(dialogOptionEntry->text))) {
            return;
        }

        int estimate = _text_num_lines(dialogOptionEntry->text, _optionRect.right - _optionRect.left) * fontGetLineHeight() + _optionRect.top + 2;
        if (estimate < _optionRect.bottom) {
            int y = _optionRect.top;

            dialogOptionEntry->bottom = estimate;
            dialogOptionEntry->top = y;

            if (index == 0) {
                y = 0;
            }

            // NOTE: Uninline.
            text_to_rect_wrapped(windowGetBuffer(gGameDialogOptionsWindow),
                &_optionRect,
                dialogOptionEntry->text,
                nullptr,
                fontGetLineHeight(),
                393,
                color);

            dialogOptionEntry->bottom = _optionRect.top;
            _optionRect.top += 2;

            dialogOptionEntry->btn = buttonCreate(gGameDialogOptionsWindow, 2, y, width, _optionRect.top - y - 4, 1200 + index, 1300 + index, -1, 49 + index, nullptr, nullptr, nullptr, 0);
            if (dialogOptionEntry->btn != -1) {
                buttonSetCallbacks(dialogOptionEntry->btn, _gsound_red_butt_press, _gsound_red_butt_release);
            } else {
                debugPrint("\nError: Can't create button!");
            }
        } else {
            if (!v21) {
                v21 = 1;
            } else {
                debugPrint("Error: couldn't make button because it went below the window.\n");
            }
        }
    }

    gameDialogRenderCaps();
    windowRefresh(gGameDialogReplyWindow);
    windowRefresh(gGameDialogOptionsWindow);
}

// 0x44715C
int _gdCreateHeadWindow()
{
    _dialogue_state = 1;

    // Headless: keep the dialog-active state but create no SDL window/buffers.
    // Leaving gGameDialogWindow == -1 turns the fidget/transition pumps and the
    // ticker's render tail into free no-ops (all guarded on window/frm state).
    if (serverLoopActive()) {
        return 0;
    }

    int windowWidth = GAME_DIALOG_WINDOW_WIDTH;

    // NOTE: Uninline.
    talk_to_create_background_window();
    gameDialogWindowRenderBackground();

    unsigned char* buf = windowGetBuffer(gGameDialogBackgroundWindow);

    for (int index = 0; index < 8; index++) {
        soundContinueAll();

        Rect* rect = &(_backgrndRects[index]);
        int width = rect->right - rect->left;
        int height = rect->bottom - rect->top;
        _backgrndBufs[index] = (unsigned char*)internal_malloc(width * height);
        if (_backgrndBufs[index] == nullptr) {
            return -1;
        }

        unsigned char* src = buf;
        src += windowWidth * rect->top + rect->left;

        blitBufferToBuffer(src, width, height, windowWidth, _backgrndBufs[index], width);
    }

    _gdialog_window_create();

    gGameDialogDisplayBuffer = windowGetBuffer(gGameDialogBackgroundWindow) + windowWidth * 14 + 126;

    // TODO: jnz at 0x447275 without cmp or test, not sure what that means.
    if (false) {
        _gdDestroyHeadWindow();
        return -1;
    }

    return 0;
}

// 0x447294
void _gdDestroyHeadWindow()
{
    if (gGameDialogWindow != -1) {
        gGameDialogDisplayBuffer = nullptr;
    }

    if (_dialogue_state == 1) {
        _gdialog_window_destroy();
    } else if (_dialogue_state == 4) {
        _gdialog_barter_destroy_win();
    }

    if (gGameDialogBackgroundWindow != -1) {
        windowDestroy(gGameDialogBackgroundWindow);
        gGameDialogBackgroundWindow = -1;
    }

    for (int index = 0; index < 8; index++) {
        internal_free(_backgrndBufs[index]);
    }
}

// 0x447300
void _gdSetupFidget(int headFrmId, int reaction)
{
    gGameDialogFidgetFrmCurrentFrame = 0;

    // Headless: the server never animates the talking head. Take the no-head
    // path so the fidget-art lock and its randomBetween(1,100) fidget-variant
    // RNG draw are skipped — otherwise a talking-head NPC would perturb the
    // global RNG at dialog init (and the client's ongoing ticker fidget RNG,
    // which the server drops, is itself wall-clock-nondeterministic). This
    // keeps server dialog RNG-isolated for every NPC, not just head-fid -1 ones.
    if (serverLoopActive()) {
        headFrmId = -1;
    }

    if (headFrmId == -1) {
        gGameDialogFidgetFid = -1;
        gGameDialogFidgetFrm = nullptr;
        gGameDialogFidgetFrmHandle = INVALID_CACHE_ENTRY;
        gGameDialogFidgetReaction = -1;
        gGameDialogFidgetUpdateDelay = 0;
        gGameDialogFidgetLastUpdateTimestamp = 0;
        gameDialogRenderTalkingHead(nullptr, 0);
        _lipsFID = 0;
        _lipsKey = nullptr;
        _lipsFp = nullptr;
        return;
    }

    int anim = HEAD_ANIMATION_NEUTRAL_PHONEMES;
    switch (reaction) {
    case FIDGET_GOOD:
        anim = HEAD_ANIMATION_GOOD_PHONEMES;
        break;
    case FIDGET_BAD:
        anim = HEAD_ANIMATION_BAD_PHONEMES;
        break;
    }

    if (_lipsFID != 0) {
        if (anim != _phone_anim) {
            if (artUnlock(_lipsKey) == -1) {
                debugPrint("failure unlocking lips frame!\n");
            }
            _lipsKey = nullptr;
            _lipsFp = nullptr;
            _lipsFID = 0;
        }
    }

    if (_lipsFID == 0) {
        _phone_anim = anim;
        _lipsFID = buildFid(OBJ_TYPE_HEAD, headFrmId, anim, 0, 0);
        _lipsFp = artLock(_lipsFID, &_lipsKey);
        if (_lipsFp == nullptr) {
            debugPrint("failure!\n");

            char stats[200];
            cachePrintStats(&gArtCache, stats, sizeof(stats));
            debugPrint("%s", stats);
        }
    }

    int fid = buildFid(OBJ_TYPE_HEAD, headFrmId, reaction, 0, 0);
    int fidgetCount = artGetFidgetCount(fid);
    if (fidgetCount == -1) {
        debugPrint("\tError - No available fidgets for given frame id\n");
        return;
    }

    int chance = randomBetween(1, 100) + _dialogue_seconds_since_last_input / 2;

    int fidget = fidgetCount;
    switch (fidgetCount) {
    case 1:
        fidget = 1;
        break;
    case 2:
        if (chance < 68) {
            fidget = 1;
        } else {
            fidget = 2;
        }
        break;
    case 3:
        _dialogue_seconds_since_last_input = 0;
        if (chance < 52) {
            fidget = 1;
        } else if (chance < 77) {
            fidget = 2;
        } else {
            fidget = 3;
        }
        break;
    }

    debugPrint("Choosing fidget %d out of %d\n", fidget, fidgetCount);

    if (gGameDialogFidgetFrm != nullptr) {
        if (artUnlock(gGameDialogFidgetFrmHandle) == -1) {
            debugPrint("failure!\n");
        }
    }

    gGameDialogFidgetFid = buildFid(OBJ_TYPE_HEAD, headFrmId, reaction, fidget, 0);
    gGameDialogFidgetFrmCurrentFrame = 0;
    gGameDialogFidgetFrm = artLock(gGameDialogFidgetFid, &gGameDialogFidgetFrmHandle);
    if (gGameDialogFidgetFrm == nullptr) {
        debugPrint("failure!\n");

        char stats[200];
        cachePrintStats(&gArtCache, stats, sizeof(stats));
        debugPrint("%s", stats);
    }

    gGameDialogFidgetLastUpdateTimestamp = 0;
    gGameDialogFidgetReaction = reaction;
    gGameDialogFidgetUpdateDelay = 1000 / artGetFramesPerSecond(gGameDialogFidgetFrm);
}

// 0x447598
void gameDialogWaitForFidgetToComplete()
{
    if (gGameDialogFidgetFrm == nullptr) {
        return;
    }

    if (gGameDialogWindow == -1) {
        return;
    }

    debugPrint("Waiting for fidget to complete...\n");

    while (artGetFrameCount(gGameDialogFidgetFrm) > gGameDialogFidgetFrmCurrentFrame) {
        sharedFpsLimiter.mark();

        if (getTicksSince(gGameDialogFidgetLastUpdateTimestamp) >= gGameDialogFidgetUpdateDelay) {
            gameDialogRenderTalkingHead(gGameDialogFidgetFrm, gGameDialogFidgetFrmCurrentFrame);
            gGameDialogFidgetLastUpdateTimestamp = getTicks();
            gGameDialogFidgetFrmCurrentFrame++;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    gGameDialogFidgetFrmCurrentFrame = 0;
}

// 0x447614
void _gdPlayTransition(int anim)
{
    if (gGameDialogFidgetFrm == nullptr) {
        return;
    }

    if (gGameDialogWindow == -1) {
        return;
    }

    mouseHideCursor();

    debugPrint("Starting transition...\n");

    gameDialogWaitForFidgetToComplete();

    if (gGameDialogFidgetFrm != nullptr) {
        if (artUnlock(gGameDialogFidgetFrmHandle) == -1) {
            debugPrint("\tError unlocking fidget in transition func...");
        }
        gGameDialogFidgetFrm = nullptr;
    }

    CacheEntry* headFrmHandle;
    int headFid = buildFid(OBJ_TYPE_HEAD, gGameDialogHeadFid, anim, 0, 0);
    Art* headFrm = artLock(headFid, &headFrmHandle);
    if (headFrm == nullptr) {
        debugPrint("\tError locking transition...\n");
    }

    unsigned int delay = 1000 / artGetFramesPerSecond(headFrm);

    int frame = 0;
    unsigned int time = 0;
    while (frame < artGetFrameCount(headFrm)) {
        sharedFpsLimiter.mark();

        if (getTicksSince(time) >= delay) {
            gameDialogRenderTalkingHead(headFrm, frame);
            time = getTicks();
            frame++;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    if (artUnlock(headFrmHandle) == -1) {
        debugPrint("\tError unlocking transition...\n");
    }

    debugPrint("Finished transition...\n");
    mouseShowCursor();
}

// 0x447724
void _reply_arrow_up(int btn, int keyCode)
{
    if (_gdReplyTooBig) {
        gameMouseSetCursor(MOUSE_CURSOR_SMALL_ARROW_UP);
    }
}

// 0x447738
void _reply_arrow_down(int btn, int keyCode)
{
    if (_gdReplyTooBig) {
        gameMouseSetCursor(MOUSE_CURSOR_SMALL_ARROW_DOWN);
    }
}

// 0x44774C
void _reply_arrow_restore(int btn, int keyCode)
{
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
}

// demo_copy_title
// 0x447758
void _demo_copy_title(int win)
{
    _gd_replyWin = win;

    if (win == -1) {
        debugPrint("\nError: demo_copy_title: win invalid!");
        return;
    }

    int width = windowGetWidth(win);
    if (width < 1) {
        debugPrint("\nError: demo_copy_title: width invalid!");
        return;
    }

    int height = windowGetHeight(win);
    if (height < 1) {
        debugPrint("\nError: demo_copy_title: length invalid!");
        return;
    }

    if (gGameDialogBackgroundWindow == -1) {
        debugPrint("\nError: demo_copy_title: dialogueBackWindow wasn't created!");
        return;
    }

    unsigned char* src = windowGetBuffer(gGameDialogBackgroundWindow);
    if (src == nullptr) {
        debugPrint("\nError: demo_copy_title: couldn't get buffer!");
        return;
    }

    unsigned char* dest = windowGetBuffer(win);

    blitBufferToBuffer(src + 640 * 225 + 135, width, height, 640, dest, width);
}

// demo_copy_options
// 0x447818
void _demo_copy_options(int win)
{
    _gd_optionsWin = win;

    if (win == -1) {
        debugPrint("\nError: demo_copy_options: win invalid!");
        return;
    }

    int width = windowGetWidth(win);
    if (width < 1) {
        debugPrint("\nError: demo_copy_options: width invalid!");
        return;
    }

    int height = windowGetHeight(win);
    if (height < 1) {
        debugPrint("\nError: demo_copy_options: length invalid!");
        return;
    }

    if (gGameDialogBackgroundWindow == -1) {
        debugPrint("\nError: demo_copy_options: dialogueBackWindow wasn't created!");
        return;
    }

    Rect windowRect;
    windowGetRect(gGameDialogWindow, &windowRect);
    windowRect.left -= (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    windowRect.top -= (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;

    unsigned char* src = windowGetBuffer(gGameDialogWindow);
    if (src == nullptr) {
        debugPrint("\nError: demo_copy_options: couldn't get buffer!");
        return;
    }

    unsigned char* dest = windowGetBuffer(win);
    blitBufferToBuffer(src + 640 * (335 - windowRect.top) + 127, width, height, 640, dest, width);
}

// gDialogRefreshOptionsRect
// 0x447914
void _gDialogRefreshOptionsRect(int win, Rect* drawRect)
{
    if (drawRect == nullptr) {
        debugPrint("\nError: gDialogRefreshOptionsRect: drawRect NULL!");
        return;
    }

    if (win == -1) {
        debugPrint("\nError: gDialogRefreshOptionsRect: win invalid!");
        return;
    }

    if (gGameDialogBackgroundWindow == -1) {
        debugPrint("\nError: gDialogRefreshOptionsRect: dialogueBackWindow wasn't created!");
        return;
    }

    Rect windowRect;
    windowGetRect(gGameDialogWindow, &windowRect);
    windowRect.left -= (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    windowRect.top -= (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;

    unsigned char* src = windowGetBuffer(gGameDialogWindow);
    if (src == nullptr) {
        debugPrint("\nError: gDialogRefreshOptionsRect: couldn't get buffer!");
        return;
    }

    if (drawRect->top >= drawRect->bottom) {
        debugPrint("\nError: gDialogRefreshOptionsRect: Invalid Rect (too many options)!");
        return;
    }

    if (drawRect->left >= drawRect->right) {
        debugPrint("\nError: gDialogRefreshOptionsRect: Invalid Rect (too many options)!");
        return;
    }

    int destWidth = windowGetWidth(win);
    unsigned char* dest = windowGetBuffer(win);

    blitBufferToBuffer(
        src + (640 * (335 - windowRect.top) + 127) + (640 * drawRect->top + drawRect->left),
        drawRect->right - drawRect->left,
        drawRect->bottom - drawRect->top,
        640,
        dest + destWidth * drawRect->top,
        destWidth);
}

// 0x447A58
void gameDialogTicker()
{
    // ►► THE MODE MACHINE IS INERT ON A CONNECTED VIEWER, AND THAT IS THE ROOT
    // FIX FOR A WHOLE CLASS OF DEFECTS. On a wire viewer the conversation belongs
    // to the SERVER; this switch is a third, uninvited owner of the same windows
    // that client_dialog and the barter screen manage from stream state, and it
    // acts with no knowledge of either.
    //
    // Concretely, this is the ghost trade screen: the dialog window builds a live
    // Barter button, clicking it sets _dialogue_switch_mode = 2, case 2 below
    // destroys the dialog window and builds a real but EMPTY local barter window,
    // enters kSpecial and never leaves it, then hands off to mode 3 — whose
    // handler lives inside _gdProcess, which never runs on a viewer. So the
    // machine strands itself halfway and the server never hears a thing.
    //
    // Gated on clientViewerActive(), NOT on which binary this is: local
    // single-player runs the same code in the same executable and must keep
    // vanilla behavior exactly. The fidget/lips work below the switch KEEPS
    // running — the viewer needs it — so this returns from the switch only.
    //
    // Loud rather than silent: reaching here with a nonzero mode means something
    // still drives the machine, which is the bug, not the symptom.
    if (clientViewerActive()) {
        if (_dialogue_switch_mode != 0) {
            fprintf(stderr, "game_dialog: viewer reached the mode machine (mode=%d) — "
                            "something local is driving dialog state; forcing 0\n",
                _dialogue_switch_mode);
            _dialogue_switch_mode = 0;
        }
    } else
    switch (_dialogue_switch_mode) {
    case 2:
        _loop_cnt = -1;
        _dialogue_switch_mode = 3;

        GameMode::enterGameMode(GameMode::kSpecial);

        _gdialog_window_destroy();
        _gdialog_barter_create_win();
        break;
    case 1:
        _loop_cnt = -1;
        _dialogue_switch_mode = 0;
        _gdialog_barter_destroy_win();
        _gdialog_window_create();

        // NOTE: Uninline.
        gdUnhide();

        if (_gd_optionsWin != -1) {
            // SFALL: Fix for the player's money not being displayed in the
            // dialog window after leaving the barter/combat control interface.
            gameDialogRenderCaps();
        }

        break;
    case 8:
        _loop_cnt = -1;
        _dialogue_switch_mode = 9;
        _gdialog_window_destroy();
        partyMemberControlWindowInit();
        break;
    case 11:
        _loop_cnt = -1;
        _dialogue_switch_mode = 12;
        _gdialog_window_destroy();
        partyMemberCustomizationWindowInit();
        break;
    }

    if (gGameDialogFidgetFrm == nullptr) {
        return;
    }

    if (gGameDialogLipSyncStarted) {
        lipsTicker();

        if (gLipsPhonemeChanged) {
            gameDialogRenderTalkingHead(_lipsFp, _head_phoneme_lookup[gLipsCurrentPhoneme]);
            gLipsPhonemeChanged = false;
        }

        if (!soundIsPlaying(gLipsData.sound)) {
            gameDialogEndLips();
            gameDialogRenderTalkingHead(_lipsFp, 0);
            _can_start_new_fidget = true;
            _dialogue_seconds_since_last_input = 3;
            gGameDialogFidgetFrmCurrentFrame = 0;
        }
        return;
    }

    if (_can_start_new_fidget) {
        if (getTicksSince(gGameDialogFidgetLastUpdateTimestamp) >= _tocksWaiting) {
            _can_start_new_fidget = false;
            _dialogue_seconds_since_last_input += _tocksWaiting / 1000;
            _tocksWaiting = 1000 * (randomBetween(0, 3) + 4);
            _gdSetupFidget(gGameDialogFidgetFid & 0xFFF, (gGameDialogFidgetFid & 0xFF0000) >> 16);
        }
        return;
    }

    if (getTicksSince(gGameDialogFidgetLastUpdateTimestamp) >= gGameDialogFidgetUpdateDelay) {
        if (artGetFrameCount(gGameDialogFidgetFrm) <= gGameDialogFidgetFrmCurrentFrame) {
            gameDialogRenderTalkingHead(gGameDialogFidgetFrm, 0);
            _can_start_new_fidget = true;
        } else {
            gameDialogRenderTalkingHead(gGameDialogFidgetFrm, gGameDialogFidgetFrmCurrentFrame);
            gGameDialogFidgetLastUpdateTimestamp = getTicks();
            gGameDialogFidgetFrmCurrentFrame += 1;
        }
    }
}

// FIXME: Due to the bug in `_gdProcessChoice` this function can receive invalid
// reaction value (50 instead of expected -1, 0, 1). It's handled gracefully by
// the game.
//
// 0x447CA0
void _talk_to_critter_reacts(int a1)
{
    int v1 = a1 + 1;

    debugPrint("Dialogue Reaction: ");
    if (v1 < 3) {
        debugPrint("%s\n", _react_strs[v1]);
    }

    int v3 = a1 + 50;
    _dialogue_seconds_since_last_input = 0;

    switch (v3) {
    case GAME_DIALOG_REACTION_GOOD:
        switch (gGameDialogFidgetReaction) {
        case FIDGET_GOOD:
            _gdPlayTransition(HEAD_ANIMATION_VERY_GOOD_REACTION);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_GOOD);
            break;
        case FIDGET_NEUTRAL:
            _gdPlayTransition(HEAD_ANIMATION_NEUTRAL_TO_GOOD);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_GOOD);
            break;
        case FIDGET_BAD:
            _gdPlayTransition(HEAD_ANIMATION_BAD_TO_NEUTRAL);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_NEUTRAL);
            break;
        }
        break;
    case GAME_DIALOG_REACTION_NEUTRAL:
        break;
    case GAME_DIALOG_REACTION_BAD:
        switch (gGameDialogFidgetReaction) {
        case FIDGET_GOOD:
            _gdPlayTransition(HEAD_ANIMATION_GOOD_TO_NEUTRAL);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_NEUTRAL);
            break;
        case FIDGET_NEUTRAL:
            _gdPlayTransition(HEAD_ANIMATION_NEUTRAL_TO_BAD);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_BAD);
            break;
        case FIDGET_BAD:
            _gdPlayTransition(HEAD_ANIMATION_VERY_BAD_REACTION);
            _gdSetupFidget(gGameDialogHeadFid, FIDGET_BAD);
            break;
        }
        break;
    }
}

// 0x447D98
void _gdialog_scroll_subwin(int windowIdx, bool scrollUp, unsigned char* windowFrmData, unsigned char* windowBuf, unsigned char* bgWindowFrmData, int windowHeight, bool instantScrollUp)
{
    constexpr int stripHeight = 10;
    int height = windowHeight;
    unsigned char* dest = windowBuf;
    Rect rect;

    if (scrollUp) {
        rect.left = 0;
        rect.right = GAME_DIALOG_WINDOW_WIDTH - 1;
        rect.bottom = windowHeight - 1;

        int strips = windowHeight / stripHeight;
        if (instantScrollUp) {
            rect.top = stripHeight;
            strips = 0;
        } else {
            rect.top = strips * stripHeight;
            height = windowHeight % stripHeight;
            dest += GAME_DIALOG_WINDOW_WIDTH * rect.top;
        }

        for (; strips >= 0; strips--) {
            sharedFpsLimiter.mark();

            soundContinueAll();
            blitBufferToBuffer(windowFrmData,
                GAME_DIALOG_WINDOW_WIDTH,
                height,
                GAME_DIALOG_WINDOW_WIDTH,
                dest,
                GAME_DIALOG_WINDOW_WIDTH);
            rect.top -= stripHeight;
            windowRefreshRect(windowIdx, &rect);
            height += stripHeight;
            dest -= stripHeight * (GAME_DIALOG_WINDOW_WIDTH);

            delay_ms(33);

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    } else {
        rect.right = GAME_DIALOG_WINDOW_WIDTH - 1;
        rect.bottom = windowHeight - 1;
        rect.left = 0;
        rect.top = 0;

        for (int strips = windowHeight / stripHeight; strips > 0; strips--) {
            sharedFpsLimiter.mark();

            soundContinueAll();

            blitBufferToBuffer(bgWindowFrmData,
                GAME_DIALOG_WINDOW_WIDTH,
                stripHeight,
                GAME_DIALOG_WINDOW_WIDTH,
                dest,
                GAME_DIALOG_WINDOW_WIDTH);

            dest += stripHeight * (GAME_DIALOG_WINDOW_WIDTH);
            height -= stripHeight;
            bgWindowFrmData += stripHeight * (GAME_DIALOG_WINDOW_WIDTH);

            blitBufferToBuffer(windowFrmData,
                GAME_DIALOG_WINDOW_WIDTH,
                height,
                GAME_DIALOG_WINDOW_WIDTH,
                dest,
                GAME_DIALOG_WINDOW_WIDTH);

            windowRefreshRect(windowIdx, &rect);

            rect.top += stripHeight;

            delay_ms(33);

            renderPresent();
            sharedFpsLimiter.throttle();
        }
    }
}

// 0x447F64
int _text_num_lines(const char* a1, int a2)
{
    int width = fontGetStringWidth(a1);

    int v1 = 0;
    while (width > 0) {
        width -= a2;
        v1++;
    }

    return v1;
}

// NOTE: Inlined.
//
// 0x447F80
static int text_to_rect_wrapped(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color)
{
    return gameDialogDrawText(buffer, rect, string, a4, height, pitch, color, 1);
}

// display_msg
// 0x447FA0
int gameDialogDrawText(unsigned char* buffer, Rect* rect, char* string, int* a4, int height, int pitch, int color, int a7)
{
    char* start;
    if (a4 != nullptr) {
        start = string + *a4;
    } else {
        start = string;
    }

    int maxWidth = rect->right - rect->left;
    char* end = nullptr;
    while (start != nullptr && *start != '\0') {
        if (fontGetStringWidth(start) > maxWidth) {
            end = start + 1;
            while (*end != '\0' && *end != ' ') {
                end++;
            }

            if (*end != '\0') {
                char* lookahead = end + 1;
                while (lookahead != nullptr) {
                    while (*lookahead != '\0' && *lookahead != ' ') {
                        lookahead++;
                    }

                    if (*lookahead == '\0') {
                        lookahead = nullptr;
                    } else {
                        *lookahead = '\0';
                        if (fontGetStringWidth(start) >= maxWidth) {
                            *lookahead = ' ';
                            lookahead = nullptr;
                        } else {
                            end = lookahead;
                            *lookahead = ' ';
                            lookahead++;
                        }
                    }
                }

                if (*end == ' ') {
                    *end = '\0';
                }
            } else {
                if (rect->bottom - fontGetLineHeight() < rect->top) {
                    return rect->top;
                }

                if (a7 != 1 || start == string) {
                    fontDrawText(buffer + pitch * rect->top + 10, start, maxWidth, pitch, color);
                } else {
                    fontDrawText(buffer + pitch * rect->top, start, maxWidth, pitch, color);
                }

                if (a4 != nullptr) {
                    *a4 += static_cast<int>(strlen(start)) + 1;
                }

                rect->top += height;
                return rect->top;
            }
        }

        if (fontGetStringWidth(start) > maxWidth) {
            debugPrint("\nError: display_msg: word too long!");
            break;
        }

        if (a7 != 0) {
            if (rect->bottom - fontGetLineHeight() < rect->top) {
                if (end != nullptr && *end == '\0') {
                    *end = ' ';
                }
                return rect->top;
            }

            unsigned char* dest;
            if (a7 != 1 || start == string) {
                dest = buffer + 10;
            } else {
                dest = buffer;
            }
            fontDrawText(dest + pitch * rect->top, start, maxWidth, pitch, color);
        }

        if (a4 != nullptr && end != nullptr) {
            *a4 += static_cast<int>(strlen(start)) + 1;
        }

        rect->top += height;

        if (end != nullptr) {
            start = end + 1;
            if (*end == '\0') {
                *end = ' ';
            }
            end = nullptr;
        } else {
            start = nullptr;
        }
    }

    if (a4 != nullptr) {
        *a4 = 0;
    }

    return rect->top;
}

// 0x448214
void gameDialogSetBarterModifier(int modifier)
{
    gGameDialogBarterModifier = modifier;
}

// gdialog_barter
// 0x44821C
int gameDialogBarter(int modifier)
{
    if (!_dialog_state_fix) {
        return -1;
    }

    gGameDialogBarterModifier = modifier;
    gameDialogBarterButtonUpMouseUp(-1, -1);
    _dialogue_state = 4;
    _dialogue_switch_mode = 2;

    return 0;
}

// 0x448268
void _barter_end_to_talk_to()
{
    _dialogQuit();
    _dialogClose();
    _updatePrograms();
    _updateWindows();
    _dialogue_state = 1;
    _dialogue_switch_mode = 1;
}

// 0x448290
int _gdialog_barter_create_win()
{
    _dialogue_state = 4;

    // Headless: allocate ONLY the three hidden barter table objects that
    // inventoryOpenTrade needs (the player offer table, the barterer table, and
    // the barterer temp object). Skip the barter.frm window, its two buttons,
    // and all blitting — there is no SDL window here (gGameDialogWindow stays
    // -1, exactly as _gdCreateHeadWindow leaves it). Object creation order and
    // flags are identical to the SDL path below so the sim state matches.
    if (serverLoopActive()) {
        if (objectCreateWithFidPid(&_peon_table_obj, -1, -1) == -1) {
            return -1;
        }
        _peon_table_obj->flags |= OBJECT_HIDDEN;

        if (objectCreateWithFidPid(&_barterer_table_obj, -1, -1) == -1) {
            objectDestroy(_peon_table_obj, nullptr);
            _peon_table_obj = nullptr;
            return -1;
        }
        _barterer_table_obj->flags |= OBJECT_HIDDEN;

        if (objectCreateWithFidPid(&_barterer_temp_obj, gGameDialogSpeaker->fid, -1) == -1) {
            objectDestroy(_barterer_table_obj, nullptr);
            objectDestroy(_peon_table_obj, nullptr);
            _barterer_table_obj = nullptr;
            _peon_table_obj = nullptr;
            return -1;
        }
        _barterer_temp_obj->flags |= OBJECT_HIDDEN | OBJECT_NO_SAVE;
        _barterer_temp_obj->sid = -1;
        return 0;
    }

    int frmId;
    if (gGameDialogSpeakerIsPartyMember) {
        // trade.frm - party member barter/trade interface
        frmId = 420;
    } else {
        // barter.frm - barter window
        frmId = 111;
    }

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, frmId, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    unsigned char* backgroundData = backgroundFrmImage.getData();
    if (backgroundData == nullptr) {
        return -1;
    }

    _dialogue_subwin_len = backgroundFrmImage.getHeight();

    int barterWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int barterWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len;
    gGameDialogWindow = windowCreate(barterWindowX,
        barterWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        _dialogue_subwin_len,
        256,
        WINDOW_DONT_MOVE_TOP);
    if (gGameDialogWindow == -1) {
        return -1;
    }

    int width = GAME_DIALOG_WINDOW_WIDTH;

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    unsigned char* backgroundWindowBuffer = windowGetBuffer(gGameDialogBackgroundWindow);
    blitBufferToBuffer(backgroundWindowBuffer + width * (480 - _dialogue_subwin_len), width, _dialogue_subwin_len, width, windowBuffer, width);

    _gdialog_scroll_subwin(gGameDialogWindow, true, backgroundData, windowBuffer, nullptr, _dialogue_subwin_len);

    backgroundFrmImage.unlock();

    // TRADE
    _gdialog_buttons[0] = buttonCreate(gGameDialogWindow, 41, 163, 14, 14, -1, -1, -1, KEY_LOWERCASE_M, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[0] != -1) {
        buttonSetCallbacks(_gdialog_buttons[0], _gsound_med_butt_press, _gsound_med_butt_release);

        // TALK
        _gdialog_buttons[1] = buttonCreate(gGameDialogWindow, 584, 162, 14, 14, -1, -1, -1, KEY_LOWERCASE_T, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
        if (_gdialog_buttons[1] != -1) {
            buttonSetCallbacks(_gdialog_buttons[1], _gsound_med_butt_press, _gsound_med_butt_release);

            if (objectCreateWithFidPid(&_peon_table_obj, -1, -1) != -1) {
                _peon_table_obj->flags |= OBJECT_HIDDEN;

                if (objectCreateWithFidPid(&_barterer_table_obj, -1, -1) != -1) {
                    _barterer_table_obj->flags |= OBJECT_HIDDEN;

                    if (objectCreateWithFidPid(&_barterer_temp_obj, gGameDialogSpeaker->fid, -1) != -1) {
                        _barterer_temp_obj->flags |= OBJECT_HIDDEN | OBJECT_NO_SAVE;
                        _barterer_temp_obj->sid = -1;
                        return 0;
                    }

                    objectDestroy(_barterer_table_obj, nullptr);
                }

                objectDestroy(_peon_table_obj, nullptr);
            }

            buttonDestroy(_gdialog_buttons[1]);
            _gdialog_buttons[1] = -1;
        }

        buttonDestroy(_gdialog_buttons[0]);
        _gdialog_buttons[0] = -1;
    }

    windowDestroy(gGameDialogWindow);
    gGameDialogWindow = -1;

    return -1;
}

// 0x44854C
void _gdialog_barter_destroy_win()
{
    // Headless mirror of the SDL teardown's object destruction (there is no
    // window or buttons to destroy). gGameDialogWindow stayed -1 the whole
    // time, so the normal guard below would early-return and LEAK the three
    // table objects. Null the statics so the second teardown call from
    // _gdDestroyHeadWindow (_dialogue_state == 4 branch) is a no-op instead of
    // a double free.
    if (serverLoopActive()) {
        objectDestroy(_barterer_temp_obj, nullptr);
        objectDestroy(_barterer_table_obj, nullptr);
        objectDestroy(_peon_table_obj, nullptr);
        _barterer_temp_obj = nullptr;
        _barterer_table_obj = nullptr;
        _peon_table_obj = nullptr;
        // The SDL teardown ends with this too (below). It is a real sim
        // mutation — a weapon-carrying merchant tops up its right-hand weapon
        // from loose ammo (and may destroy the emptied ammo object) — so it
        // must run headless as well. Safe here: animate=0, and the reload sfx
        // only fires for party members, which self-neutralizes on the null
        // presenter. (Inert for denbus1's Tubby, who holds no weapon.)
        aiAttemptWeaponReload(gGameDialogSpeaker, 0);
        return;
    }

    if (gGameDialogWindow == -1) {
        return;
    }

    objectDestroy(_barterer_temp_obj, nullptr);
    objectDestroy(_barterer_table_obj, nullptr);
    objectDestroy(_peon_table_obj, nullptr);

    for (int index = 0; index < 9; index++) {
        buttonDestroy(_gdialog_buttons[index]);
        _gdialog_buttons[index] = -1;
    }

    unsigned char* backgroundWindowBuffer = windowGetBuffer(gGameDialogBackgroundWindow);
    backgroundWindowBuffer += (GAME_DIALOG_WINDOW_WIDTH) * (480 - _dialogue_subwin_len);

    int frmId;
    if (gGameDialogSpeakerIsPartyMember) {
        // trade.frm - party member barter/trade interface
        frmId = 420;
    } else {
        // barter.frm - barter window
        frmId = 111;
    }

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, frmId, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
        _gdialog_scroll_subwin(gGameDialogWindow, false, backgroundFrmImage.getData(), windowBuffer, backgroundWindowBuffer, _dialogue_subwin_len);
    }

    windowDestroy(gGameDialogWindow);
    gGameDialogWindow = -1;

    aiAttemptWeaponReload(gGameDialogSpeaker, 0);
}

// 0x448660
void _gdialog_barter_cleanup_tables()
{
    Inventory* inventory;
    int length;

    inventory = &(_peon_table_obj->data.inventory);
    length = inventory->length;
    for (int index = 0; index < length; index++) {
        Object* item = inventory->items->item;
        int quantity = itemGetQuantity(_peon_table_obj, item);
        itemMoveForce(_peon_table_obj, gDude, item, quantity);
    }

    inventory = &(_barterer_table_obj->data.inventory);
    length = inventory->length;
    for (int index = 0; index < length; index++) {
        Object* item = inventory->items->item;
        int quantity = itemGetQuantity(_barterer_table_obj, item);
        itemMoveForce(_barterer_table_obj, gGameDialogSpeaker, item, quantity);
    }

    if (_barterer_temp_obj != nullptr) {
        inventory = &(_barterer_temp_obj->data.inventory);
        length = inventory->length;
        for (int index = 0; index < length; index++) {
            Object* item = inventory->items->item;
            int quantity = itemGetQuantity(_barterer_temp_obj, item);
            itemMoveForce(_barterer_temp_obj, gGameDialogSpeaker, item, quantity);
        }
    }
}

// 0x448740
int partyMemberControlWindowInit()
{
    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 390, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    unsigned char* backgroundData = backgroundFrmImage.getData();
    if (backgroundData == nullptr) {
        partyMemberControlWindowFree();
        return -1;
    }

    _dialogue_subwin_len = backgroundFrmImage.getHeight();
    int controlWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int controlWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len;
    gGameDialogWindow = windowCreate(controlWindowX,
        controlWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        _dialogue_subwin_len,
        256,
        WINDOW_DONT_MOVE_TOP);
    if (gGameDialogWindow == -1) {
        partyMemberControlWindowFree();
        return -1;
    }

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    unsigned char* src = windowGetBuffer(gGameDialogBackgroundWindow);
    blitBufferToBuffer(src + (GAME_DIALOG_WINDOW_WIDTH) * (GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len), GAME_DIALOG_WINDOW_WIDTH, _dialogue_subwin_len, GAME_DIALOG_WINDOW_WIDTH, windowBuffer, GAME_DIALOG_WINDOW_WIDTH);
    _gdialog_scroll_subwin(gGameDialogWindow, true, backgroundData, windowBuffer, nullptr, _dialogue_subwin_len);
    backgroundFrmImage.unlock();

    // TALK
    _gdialog_buttons[0] = buttonCreate(gGameDialogWindow, 593, 41, 14, 14, -1, -1, -1, KEY_ESCAPE, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[0] == -1) {
        partyMemberControlWindowFree();
        return -1;
    }
    buttonSetCallbacks(_gdialog_buttons[0], _gsound_med_butt_press, _gsound_med_butt_release);

    // TRADE
    _gdialog_buttons[1] = buttonCreate(gGameDialogWindow, 593, 97, 14, 14, -1, -1, -1, KEY_LOWERCASE_D, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[1] == -1) {
        partyMemberControlWindowFree();
        return -1;
    }
    buttonSetCallbacks(_gdialog_buttons[1], _gsound_med_butt_press, _gsound_med_butt_release);

    // USE BEST WEAPON
    _gdialog_buttons[2] = buttonCreate(gGameDialogWindow, 236, 15, 14, 14, -1, -1, -1, KEY_LOWERCASE_W, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[2] == -1) {
        partyMemberControlWindowFree();
        return -1;
    }
    buttonSetCallbacks(_gdialog_buttons[1], _gsound_med_butt_press, _gsound_med_butt_release);

    // USE BEST ARMOR
    _gdialog_buttons[3] = buttonCreate(gGameDialogWindow, 235, 46, 14, 14, -1, -1, -1, KEY_LOWERCASE_A, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[3] == -1) {
        partyMemberControlWindowFree();
        return -1;
    }
    buttonSetCallbacks(_gdialog_buttons[2], _gsound_med_butt_press, _gsound_med_butt_release);

    _control_buttons_start = 4;

    int v21 = 3;

    for (int index = 0; index < 5; index++) {
        GameDialogButtonData* buttonData = &(gGameDialogDispositionButtonsData[index]);
        int fid;

        fid = buildFid(OBJ_TYPE_INTERFACE, buttonData->upFrmId, 0, 0, 0);
        Art* upButtonFrm = artLock(fid, &(buttonData->upFrmHandle));
        if (upButtonFrm == nullptr) {
            partyMemberControlWindowFree();
            return -1;
        }

        int width = artGetWidth(upButtonFrm, 0, 0);
        int height = artGetHeight(upButtonFrm, 0, 0);
        unsigned char* upButtonFrmData = artGetFrameData(upButtonFrm, 0, 0);

        fid = buildFid(OBJ_TYPE_INTERFACE, buttonData->downFrmId, 0, 0, 0);
        Art* downButtonFrm = artLock(fid, &(buttonData->downFrmHandle));
        if (downButtonFrm == nullptr) {
            partyMemberControlWindowFree();
            return -1;
        }

        unsigned char* downButtonFrmData = artGetFrameData(downButtonFrm, 0, 0);

        fid = buildFid(OBJ_TYPE_INTERFACE, buttonData->disabledFrmId, 0, 0, 0);
        Art* disabledButtonFrm = artLock(fid, &(buttonData->disabledFrmHandle));
        if (disabledButtonFrm == nullptr) {
            partyMemberControlWindowFree();
            return -1;
        }

        unsigned char* disabledButtonFrmData = artGetFrameData(disabledButtonFrm, 0, 0);

        v21++;

        _gdialog_buttons[v21] = buttonCreate(gGameDialogWindow,
            buttonData->x,
            buttonData->y,
            width,
            height,
            -1,
            -1,
            buttonData->keyCode,
            -1,
            upButtonFrmData,
            downButtonFrmData,
            nullptr,
            BUTTON_FLAG_TRANSPARENT | BUTTON_FLAG_0x04 | BUTTON_FLAG_0x01);
        if (_gdialog_buttons[v21] == -1) {
            partyMemberControlWindowFree();
            return -1;
        }

        _win_register_button_disable(_gdialog_buttons[v21], disabledButtonFrmData, disabledButtonFrmData, disabledButtonFrmData);
        buttonSetCallbacks(_gdialog_buttons[v21], _gsound_med_butt_press, _gsound_med_butt_release);

        if (!partyMemberSupportsDisposition(gGameDialogSpeaker, buttonData->value)) {
            buttonDisable(_gdialog_buttons[v21]);
        }
    }

    _win_group_radio_buttons(5, &(_gdialog_buttons[_control_buttons_start]));

    int disposition = aiGetDisposition(gGameDialogSpeaker);
    _win_set_button_rest_state(_gdialog_buttons[_control_buttons_start + 4 - disposition], 1, 0);

    partyMemberControlWindowUpdate();

    _dialogue_state = 10;

    windowRefresh(gGameDialogWindow);

    return 0;
}

// 0x448C10
void partyMemberControlWindowFree()
{
    if (gGameDialogWindow == -1) {
        return;
    }

    for (int index = 0; index < 9; index++) {
        buttonDestroy(_gdialog_buttons[index]);
        _gdialog_buttons[index] = -1;
    }

    for (int index = 0; index < 5; index++) {
        GameDialogButtonData* buttonData = &(gGameDialogDispositionButtonsData[index]);

        if (buttonData->upFrmHandle) {
            artUnlock(buttonData->upFrmHandle);
            buttonData->upFrmHandle = nullptr;
        }

        if (buttonData->downFrmHandle) {
            artUnlock(buttonData->downFrmHandle);
            buttonData->downFrmHandle = nullptr;
        }

        if (buttonData->disabledFrmHandle) {
            artUnlock(buttonData->disabledFrmHandle);
            buttonData->disabledFrmHandle = nullptr;
        }
    }

    // control.frm - party member control interface
    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 390, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        _gdialog_scroll_subwin(gGameDialogWindow, false, backgroundFrmImage.getData(), windowGetBuffer(gGameDialogWindow), windowGetBuffer(gGameDialogBackgroundWindow) + (GAME_DIALOG_WINDOW_WIDTH) * (480 - _dialogue_subwin_len), _dialogue_subwin_len);
    }

    windowDestroy(gGameDialogWindow);
    gGameDialogWindow = -1;
}

// 0x448D30
void partyMemberControlWindowUpdate()
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    int windowWidth = windowGetWidth(gGameDialogWindow);

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 390, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        int width = backgroundFrmImage.getWidth();
        unsigned char* buffer = backgroundFrmImage.getData();

        // Clear "Weapon Used:".
        blitBufferToBuffer(buffer + width * 20 + 112, 110, fontGetLineHeight(), width, windowBuffer + windowWidth * 20 + 112, windowWidth);

        // Clear "Armor Used:".
        blitBufferToBuffer(buffer + width * 49 + 112, 110, fontGetLineHeight(), width, windowBuffer + windowWidth * 49 + 112, windowWidth);

        // Clear character preview.
        blitBufferToBuffer(buffer + width * 84 + 8, 70, 98, width, windowBuffer + windowWidth * 84 + 8, windowWidth);

        // Clear ?
        blitBufferToBuffer(buffer + width * 80 + 232, 132, 106, width, windowBuffer + windowWidth * 80 + 232, windowWidth);

        backgroundFrmImage.unlock();
    }

    MessageListItem messageListItem;
    char* text;
    char formattedText[256];

    // Render item in right hand.
    Object* item2 = critterGetItem2(gGameDialogSpeaker);
    text = item2 != nullptr ? itemGetName(item2) : getmsg(&gProtoMessageList, &messageListItem, 10);
    snprintf(formattedText, sizeof(formattedText), "%s", text);
    fontDrawText(windowBuffer + windowWidth * 20 + 112, formattedText, 110, windowWidth, _colorTable[992]);

    // Render armor.
    Object* armor = critterGetArmor(gGameDialogSpeaker);
    text = armor != nullptr ? itemGetName(armor) : getmsg(&gProtoMessageList, &messageListItem, 10);
    snprintf(formattedText, sizeof(formattedText), "%s", text);
    fontDrawText(windowBuffer + windowWidth * 49 + 112, formattedText, 110, windowWidth, _colorTable[992]);

    // Render preview.
    CacheEntry* previewHandle;
    int previewFid = buildFid(FID_TYPE(gGameDialogSpeaker->fid), gGameDialogSpeaker->fid & 0xFFF, ANIM_STAND, (gGameDialogSpeaker->fid & 0xF000) >> 12, ROTATION_SW);
    Art* preview = artLock(previewFid, &previewHandle);
    if (preview != nullptr) {
        int width = artGetWidth(preview, 0, ROTATION_SW);
        int height = artGetHeight(preview, 0, ROTATION_SW);
        unsigned char* buffer = artGetFrameData(preview, 0, ROTATION_SW);
        blitBufferToBufferTrans(buffer, width, height, width, windowBuffer + windowWidth * (132 - height / 2) + 39 - width / 2, windowWidth);
        artUnlock(previewHandle);
    }

    // Render hit points.
    int maximumHitPoints = critterGetStat(gGameDialogSpeaker, STAT_MAXIMUM_HIT_POINTS);
    int hitPoints = critterGetStat(gGameDialogSpeaker, STAT_CURRENT_HIT_POINTS);
    snprintf(formattedText, sizeof(formattedText), "%d/%d", hitPoints, maximumHitPoints);
    fontDrawText(windowBuffer + windowWidth * 96 + 240, formattedText, 115, windowWidth, _colorTable[992]);

    // Render best skill.
    int bestSkill = partyMemberGetBestSkill(gGameDialogSpeaker);
    text = skillGetName(bestSkill);
    snprintf(formattedText, sizeof(formattedText), "%s", text);
    fontDrawText(windowBuffer + windowWidth * 113 + 240, formattedText, 115, windowWidth, _colorTable[992]);

    // Render weight summary.
    int inventoryWeight = objectGetInventoryWeight(gGameDialogSpeaker);
    int carryWeight = critterGetStat(gGameDialogSpeaker, STAT_CARRY_WEIGHT);
    snprintf(formattedText, sizeof(formattedText), "%d/%d ", inventoryWeight, carryWeight);
    fontDrawText(windowBuffer + windowWidth * 131 + 240, formattedText, 115, windowWidth, critterIsEncumbered(gGameDialogSpeaker) ? _colorTable[31744] : _colorTable[992]);

    // Render melee damage.
    int meleeDamage = critterGetStat(gGameDialogSpeaker, STAT_MELEE_DAMAGE);
    snprintf(formattedText, sizeof(formattedText), "%d", meleeDamage);
    fontDrawText(windowBuffer + windowWidth * 148 + 240, formattedText, 115, windowWidth, _colorTable[992]);

    int actionPoints;
    if (isInCombat()) {
        actionPoints = gGameDialogSpeaker->data.critter.combat.ap;
    } else {
        actionPoints = critterGetStat(gGameDialogSpeaker, STAT_MAXIMUM_ACTION_POINTS);
    }
    int maximumActionPoints = critterGetStat(gGameDialogSpeaker, STAT_MAXIMUM_ACTION_POINTS);
    snprintf(formattedText, sizeof(formattedText), "%d/%d ", actionPoints, maximumActionPoints);
    fontDrawText(windowBuffer + windowWidth * 167 + 240, formattedText, 115, windowWidth, _colorTable[992]);

    fontSetCurrent(oldFont);
    windowRefresh(gGameDialogWindow);
}

// 0x44928C
void gameDialogCombatControlButtonOnMouseUp(int btn, int keyCode)
{
    _dialogue_switch_mode = 8;
    _dialogue_state = 10;

    // NOTE: Uninline.
    gdHide();
}

// 0x4492D0
int _gdPickAIUpdateMsg(Object* critter)
{
    int pids[3];
    memcpy(pids, _Dogs, sizeof(pids));

    for (int index = 0; index < 3; index++) {
        if (critter->pid == pids[index]) {
            return 677 + randomBetween(0, 1);
        }
    }

    return 670 + randomBetween(0, 4);
}

// 0x449330
int _gdCanBarter()
{
    if (PID_TYPE(gGameDialogSpeaker->pid) != OBJ_TYPE_CRITTER) {
        return 1;
    }

    Proto* proto;
    if (protoGetProto(gGameDialogSpeaker->pid, &proto) == -1) {
        return 1;
    }

    if (proto->critter.data.flags & CRITTER_BARTER) {
        return 1;
    }

    MessageListItem messageListItem;

    // This person will not barter with you.
    messageListItem.num = 903;
    if (gGameDialogSpeakerIsPartyMember) {
        // This critter can't carry anything.
        messageListItem.num = 913;
    }

    if (!messageListGetItem(&gProtoMessageList, &messageListItem)) {
        debugPrint("\nError: gdialog: Can't find message!");
        return 0;
    }

    gameDialogRenderSupplementaryMessage(messageListItem.text);

    return 0;
}

// 0x4493B8
void partyMemberControlWindowHandleEvents()
{
    MessageListItem messageListItem;

    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode != -1) {
            if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
                showQuitConfirmationDialog();
            }

            if (_game_user_wants_to_quit != 0) {
                break;
            }

            if (keyCode == KEY_LOWERCASE_W) {
                _inven_unwield(gGameDialogSpeaker, 1);

                Object* weapon = _ai_search_inven_weap(gGameDialogSpeaker, 0, nullptr);
                if (weapon != nullptr) {
                    _inven_wield(gGameDialogSpeaker, weapon, HAND_RIGHT);
                    aiAttemptWeaponReload(gGameDialogSpeaker, 0);

                    int num = _gdPickAIUpdateMsg(gGameDialogSpeaker);
                    char* msg = getmsg(&gProtoMessageList, &messageListItem, num);
                    gameDialogRenderSupplementaryMessage(msg);
                    partyMemberControlWindowUpdate();
                }
            } else if (keyCode == 2098) {
                aiSetDisposition(gGameDialogSpeaker, 4);
            } else if (keyCode == 2099) {
                aiSetDisposition(gGameDialogSpeaker, 0);
                _dialogue_state = 13;
                _dialogue_switch_mode = 11;
                done = true;
            } else if (keyCode == 2102) {
                aiSetDisposition(gGameDialogSpeaker, 2);
            } else if (keyCode == 2103) {
                aiSetDisposition(gGameDialogSpeaker, 3);
            } else if (keyCode == 2111) {
                aiSetDisposition(gGameDialogSpeaker, 1);
            } else if (keyCode == KEY_ESCAPE) {
                _dialogue_switch_mode = 1;
                _dialogue_state = 1;
                return;
            } else if (keyCode == KEY_LOWERCASE_A) {
                if (gGameDialogSpeaker->pid != 0x10000A1) {
                    Object* armor = _ai_search_inven_armor(gGameDialogSpeaker);
                    if (armor != nullptr) {
                        _inven_wield(gGameDialogSpeaker, armor, 0);
                    }
                }

                int num = _gdPickAIUpdateMsg(gGameDialogSpeaker);
                char* msg = getmsg(&gProtoMessageList, &messageListItem, num);
                gameDialogRenderSupplementaryMessage(msg);
                partyMemberControlWindowUpdate();
            } else if (keyCode == KEY_LOWERCASE_D) {
                if (_gdCanBarter()) {
                    _dialogue_switch_mode = 2;
                    _dialogue_state = 4;
                    return;
                }
            } else if (keyCode == -2) {
                // CE: Minor improvement - handle on mouse up (just like other
                // buttons). Also fixed active button area (in original code
                // it's slightly smaller than the button itself).
                if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                    if (mouseHitTestInWindow(gGameDialogWindow, 438, 156, 438 + 109, 156 + 28)) {
                        aiSetDisposition(gGameDialogSpeaker, 0);
                        _dialogue_state = 13;
                        _dialogue_switch_mode = 11;
                        done = true;
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }
}

// 0x4496A0
int partyMemberCustomizationWindowInit()
{
    if (!messageListInit(&gCustomMessageList)) {
        return -1;
    }

    if (!messageListLoad(&gCustomMessageList, "game\\custom.msg")) {
        return -1;
    }

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 391, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    unsigned char* backgroundFrmData = backgroundFrmImage.getData();
    if (backgroundFrmData == nullptr) {
        // FIXME: Leaking background.
        partyMemberCustomizationWindowFree();
        return -1;
    }

    _dialogue_subwin_len = backgroundFrmImage.getHeight();

    int customizationWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int customizationWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len;
    gGameDialogWindow = windowCreate(customizationWindowX,
        customizationWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        _dialogue_subwin_len,
        256,
        WINDOW_DONT_MOVE_TOP);
    if (gGameDialogWindow == -1) {
        partyMemberCustomizationWindowFree();
        return -1;
    }

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    unsigned char* parentWindowBuffer = windowGetBuffer(gGameDialogBackgroundWindow);
    blitBufferToBuffer(parentWindowBuffer + (GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len) * GAME_DIALOG_WINDOW_WIDTH,
        GAME_DIALOG_WINDOW_WIDTH,
        _dialogue_subwin_len,
        GAME_DIALOG_WINDOW_WIDTH,
        windowBuffer,
        GAME_DIALOG_WINDOW_WIDTH);

    _gdialog_scroll_subwin(gGameDialogWindow, true, backgroundFrmData, windowBuffer, nullptr, _dialogue_subwin_len);
    backgroundFrmImage.unlock();

    _gdialog_buttons[0] = buttonCreate(gGameDialogWindow, 593, 101, 14, 14, -1, -1, -1, 13, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (_gdialog_buttons[0] == -1) {
        partyMemberCustomizationWindowFree();
        return -1;
    }

    buttonSetCallbacks(_gdialog_buttons[0], _gsound_med_butt_press, _gsound_med_butt_release);

    int optionButton = 0;
    _custom_buttons_start = 1;

    for (int index = 0; index < PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT; index++) {
        GameDialogButtonData* buttonData = &(_custom_button_info[index]);

        int upButtonFid = buildFid(OBJ_TYPE_INTERFACE, buttonData->upFrmId, 0, 0, 0);
        Art* upButtonFrm = artLock(upButtonFid, &(buttonData->upFrmHandle));
        if (upButtonFrm == nullptr) {
            partyMemberCustomizationWindowFree();
            return -1;
        }

        int width = artGetWidth(upButtonFrm, 0, 0);
        int height = artGetHeight(upButtonFrm, 0, 0);
        unsigned char* upButtonFrmData = artGetFrameData(upButtonFrm, 0, 0);

        int downButtonFid = buildFid(OBJ_TYPE_INTERFACE, buttonData->downFrmId, 0, 0, 0);
        Art* downButtonFrm = artLock(downButtonFid, &(buttonData->downFrmHandle));
        if (downButtonFrm == nullptr) {
            partyMemberCustomizationWindowFree();
            return -1;
        }

        unsigned char* downButtonFrmData = artGetFrameData(downButtonFrm, 0, 0);

        optionButton++;
        _gdialog_buttons[optionButton] = buttonCreate(gGameDialogWindow,
            buttonData->x,
            buttonData->y,
            width,
            height,
            -1,
            -1,
            -1,
            buttonData->keyCode,
            upButtonFrmData,
            downButtonFrmData,
            nullptr,
            BUTTON_FLAG_TRANSPARENT);
        if (_gdialog_buttons[optionButton] == -1) {
            partyMemberCustomizationWindowFree();
            return -1;
        }

        buttonSetCallbacks(_gdialog_buttons[index], _gsound_med_butt_press, _gsound_med_butt_release);
    }

    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE] = aiGetAreaAttackMode(gGameDialogSpeaker);
    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE] = aiGetRunAwayMode(gGameDialogSpeaker);
    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON] = aiGetBestWeapon(gGameDialogSpeaker);
    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE] = aiGetDistance(gGameDialogSpeaker);
    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO] = aiGetAttackWho(gGameDialogSpeaker);
    _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE] = aiGetChemUse(gGameDialogSpeaker);

    _dialogue_state = 13;

    partyMemberCustomizationWindowUpdate();

    return 0;
}

// 0x449A10
void partyMemberCustomizationWindowFree()
{
    if (gGameDialogWindow == -1) {
        return;
    }

    for (int index = 0; index < 9; index++) {
        buttonDestroy(_gdialog_buttons[index]);
        _gdialog_buttons[index] = -1;
    }

    for (int index = 0; index < PARTY_MEMBER_CUSTOMIZATION_OPTION_COUNT; index++) {
        GameDialogButtonData* buttonData = &(_custom_button_info[index]);

        if (buttonData->upFrmHandle != nullptr) {
            artUnlock(buttonData->upFrmHandle);
            buttonData->upFrmHandle = nullptr;
        }

        if (buttonData->downFrmHandle != nullptr) {
            artUnlock(buttonData->downFrmHandle);
            buttonData->downFrmHandle = nullptr;
        }

        if (buttonData->disabledFrmHandle != nullptr) {
            artUnlock(buttonData->disabledFrmHandle);
            buttonData->disabledFrmHandle = nullptr;
        }
    }

    FrmImage backgroundFrmImage;
    // custom.frm - party member control interface
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 391, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        _gdialog_scroll_subwin(gGameDialogWindow, false, backgroundFrmImage.getData(), windowGetBuffer(gGameDialogWindow), windowGetBuffer(gGameDialogBackgroundWindow) + (GAME_DIALOG_WINDOW_WIDTH) * (480 - _dialogue_subwin_len), _dialogue_subwin_len);
    }

    windowDestroy(gGameDialogWindow);
    gGameDialogWindow = -1;

    messageListFree(&gCustomMessageList);
}

// 0x449B3C
void partyMemberCustomizationWindowHandleEvents()
{
    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        unsigned int keyCode = inputGetInput();
        if (keyCode != -1) {
            if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
                showQuitConfirmationDialog();
            }

            if (_game_user_wants_to_quit != 0) {
                break;
            }

            if (keyCode <= 5) {
                _gdCustomSelect(keyCode);
                partyMemberCustomizationWindowUpdate();
            } else if (keyCode == KEY_RETURN || keyCode == KEY_ESCAPE) {
                done = true;
                _dialogue_switch_mode = 8;
                _dialogue_state = 10;
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }
}

// 0x449BB4
void partyMemberCustomizationWindowUpdate()
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    int windowWidth = windowGetWidth(gGameDialogWindow);

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 391, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return;
    }

    int backgroundWidth = backgroundFrmImage.getWidth();
    int backgroundHeight = backgroundFrmImage.getHeight();
    unsigned char* backgroundData = backgroundFrmImage.getData();
    blitBufferToBuffer(backgroundData, backgroundWidth, backgroundHeight, backgroundWidth, windowBuffer, GAME_DIALOG_WINDOW_WIDTH);

    backgroundFrmImage.unlock();

    MessageListItem messageListItem;
    int num;
    char* msg;

    // BURST
    if (_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE] == -1) {
        // Not Applicable
        num = 99;
    } else {
        debugPrint("\nburst: %d", _custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE]);
        num = _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE]].messageId;
    }

    msg = getmsg(&gCustomMessageList, &messageListItem, num);
    fontDrawText(windowBuffer + windowWidth * 20 + 232, msg, 248, windowWidth, _colorTable[992]);

    // RUN AWAY
    msg = getmsg(&gCustomMessageList, &messageListItem, _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE]].messageId);
    fontDrawText(windowBuffer + windowWidth * 48 + 232, msg, 248, windowWidth, _colorTable[992]);

    // WEAPON PREF
    msg = getmsg(&gCustomMessageList, &messageListItem, _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON]].messageId);
    fontDrawText(windowBuffer + windowWidth * 78 + 232, msg, 248, windowWidth, _colorTable[992]);

    // DISTANCE
    msg = getmsg(&gCustomMessageList, &messageListItem, _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE]].messageId);
    fontDrawText(windowBuffer + windowWidth * 108 + 232, msg, 248, windowWidth, _colorTable[992]);

    // ATTACK WHO
    msg = getmsg(&gCustomMessageList, &messageListItem, _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO]].messageId);
    fontDrawText(windowBuffer + windowWidth * 137 + 232, msg, 248, windowWidth, _colorTable[992]);

    // CHEM USE
    msg = getmsg(&gCustomMessageList, &messageListItem, _custom_settings[PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE][_custom_current_selected[PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE]].messageId);
    fontDrawText(windowBuffer + windowWidth * 166 + 232, msg, 248, windowWidth, _colorTable[992]);

    windowRefresh(gGameDialogWindow);
    fontSetCurrent(oldFont);
}

// 0x449E64
void _gdCustomSelectRedraw(unsigned char* dest, int pitch, int type, int selectedIndex)
{
    MessageListItem messageListItem;

    fontSetCurrent(101);

    for (int index = 0; index < 6; index++) {
        STRUCT_5189E4* ptr = &(_custom_settings[type][index]);
        if (ptr->messageId != -1) {
            bool enabled = false;
            switch (type) {
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE:
                enabled = partyMemberSupportsAreaAttackMode(gGameDialogSpeaker, ptr->value);
                break;
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE:
                enabled = partyMemberSupportsRunAwayMode(gGameDialogSpeaker, ptr->value);
                break;
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON:
                enabled = partyMemberSupportsBestWeapon(gGameDialogSpeaker, ptr->value);
                break;
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE:
                enabled = partyMemberSupportsDistance(gGameDialogSpeaker, ptr->value);
                break;
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO:
                enabled = partyMemberSupportsAttackWho(gGameDialogSpeaker, ptr->value);
                break;
            case PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE:
                enabled = partyMemberSupportsChemUse(gGameDialogSpeaker, ptr->value);
                break;
            }

            int color;
            if (enabled) {
                if (index == selectedIndex) {
                    color = _colorTable[32747];
                } else {
                    color = _colorTable[992];
                }
            } else {
                color = _colorTable[15855];
            }

            const char* msg = getmsg(&gCustomMessageList, &messageListItem, ptr->messageId);
            fontDrawText(dest + pitch * (fontGetLineHeight() * index + 42) + 42, msg, pitch - 84, pitch, color);
        }
    }
}

// 0x449FC0
int _gdCustomSelect(int a1)
{
    int oldFont = fontGetCurrent();

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 419, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    int backgroundFrmWidth = backgroundFrmImage.getWidth();
    int backgroundFrmHeight = backgroundFrmImage.getHeight();

    int selectWindowX = (screenGetWidth() - backgroundFrmWidth) / 2;
    int selectWindowY = (screenGetHeight() - backgroundFrmHeight) / 2;
    int win = windowCreate(selectWindowX, selectWindowY, backgroundFrmWidth, backgroundFrmHeight, 256, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return -1;
    }

    unsigned char* windowBuffer = windowGetBuffer(win);
    blitBufferToBuffer(backgroundFrmImage.getData(),
        backgroundFrmWidth,
        backgroundFrmHeight,
        backgroundFrmWidth,
        windowBuffer,
        backgroundFrmWidth);

    backgroundFrmImage.unlock();

    int btn1 = buttonCreate(win, 70, 164, 14, 14, -1, -1, -1, KEY_RETURN, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (btn1 == -1) {
        windowDestroy(win);
        return -1;
    }

    int btn2 = buttonCreate(win, 176, 163, 14, 14, -1, -1, -1, KEY_ESCAPE, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
    if (btn2 == -1) {
        windowDestroy(win);
        return -1;
    }

    fontSetCurrent(103);

    MessageListItem messageListItem;
    const char* msg;

    msg = getmsg(&gCustomMessageList, &messageListItem, a1);
    fontDrawText(windowBuffer + backgroundFrmWidth * 15 + 40, msg, backgroundFrmWidth, backgroundFrmWidth, _colorTable[18979]);

    msg = getmsg(&gCustomMessageList, &messageListItem, 10);
    fontDrawText(windowBuffer + backgroundFrmWidth * 163 + 88, msg, backgroundFrmWidth, backgroundFrmWidth, _colorTable[18979]);

    msg = getmsg(&gCustomMessageList, &messageListItem, 11);
    fontDrawText(windowBuffer + backgroundFrmWidth * 162 + 193, msg, backgroundFrmWidth, backgroundFrmWidth, _colorTable[18979]);

    int value = _custom_current_selected[a1];
    _gdCustomSelectRedraw(windowBuffer, backgroundFrmWidth, a1, value);
    windowRefresh(win);

    int minX = selectWindowX + 42;
    int minY = selectWindowY + 42;
    int maxX = selectWindowX + backgroundFrmWidth - 42;
    int maxY = selectWindowY + backgroundFrmHeight - 42;

    bool done = false;
    unsigned int v53 = 0;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode != -1) {
            if (keyCode == KEY_CTRL_Q || keyCode == KEY_CTRL_X || keyCode == KEY_F10) {
                showQuitConfirmationDialog();
            }

            if (_game_user_wants_to_quit != 0) {
                break;
            }

            if (keyCode == KEY_RETURN) {
                STRUCT_5189E4* ptr = &(_custom_settings[a1][value]);
                _custom_current_selected[a1] = value;
                _gdCustomUpdateSetting(a1, ptr->value);
                done = true;
            } else if (keyCode == KEY_ESCAPE) {
                done = true;
            } else if (keyCode == -2) {
                if ((mouseGetEvent() & MOUSE_EVENT_LEFT_BUTTON_UP) != 0) {
                    // No need to use mouseHitTestInWindow as these values are already
                    // in screen coordinates.
                    if (_mouse_click_in(minX, minY, maxX, maxY)) {
                        int mouseX;
                        int mouseY;
                        mouseGetPosition(&mouseX, &mouseY);

                        int lineHeight = fontGetLineHeight();
                        int newValue = (mouseY - minY) / lineHeight;
                        if (newValue < 6) {
                            unsigned int timestamp = getTicks();
                            if (newValue == value) {
                                if (getTicksBetween(timestamp, v53) < 250) {
                                    _custom_current_selected[a1] = newValue;
                                    _gdCustomUpdateSetting(a1, newValue);
                                    done = true;
                                }
                            } else {
                                STRUCT_5189E4* ptr = &(_custom_settings[a1][newValue]);
                                if (ptr->messageId != -1) {
                                    bool enabled = false;
                                    switch (a1) {
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE:
                                        enabled = partyMemberSupportsAreaAttackMode(gGameDialogSpeaker, ptr->value);
                                        break;
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE:
                                        enabled = partyMemberSupportsRunAwayMode(gGameDialogSpeaker, ptr->value);
                                        break;
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON:
                                        enabled = partyMemberSupportsBestWeapon(gGameDialogSpeaker, ptr->value);
                                        break;
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE:
                                        enabled = partyMemberSupportsDistance(gGameDialogSpeaker, ptr->value);
                                        break;
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO:
                                        enabled = partyMemberSupportsAttackWho(gGameDialogSpeaker, ptr->value);
                                        break;
                                    case PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE:
                                        enabled = partyMemberSupportsChemUse(gGameDialogSpeaker, ptr->value);
                                        break;
                                    }

                                    if (enabled) {
                                        value = newValue;
                                        _gdCustomSelectRedraw(windowBuffer, backgroundFrmWidth, a1, newValue);
                                        windowRefresh(win);
                                    }
                                }
                            }
                            v53 = timestamp;
                        }
                    }
                }
            }
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    windowDestroy(win);
    fontSetCurrent(oldFont);
    return 0;
}

// 0x44A4E0
void _gdCustomUpdateSetting(int option, int value)
{
    switch (option) {
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_AREA_ATTACK_MODE:
        aiSetAreaAttackMode(gGameDialogSpeaker, value);
        break;
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_RUN_AWAY_MODE:
        aiSetRunAwayMode(gGameDialogSpeaker, value);
        break;
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_BEST_WEAPON:
        aiSetBestWeapon(gGameDialogSpeaker, value);
        break;
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_DISTANCE:
        aiSetDistance(gGameDialogSpeaker, value);
        break;
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_ATTACK_WHO:
        aiSetAttackWho(gGameDialogSpeaker, value);
        break;
    case PARTY_MEMBER_CUSTOMIZATION_OPTION_CHEM_USE:
        aiSetChemUse(gGameDialogSpeaker, value);
        break;
    }
}

// 0x44A52C
void gameDialogBarterButtonUpMouseUp(int btn, int keyCode)
{
    // ►► ON A CONNECTED VIEWER THIS BUTTON MUST NOT TOUCH LOCAL STATE. Neutering
    // the ticker was not enough: this CALLBACK runs first, and it both flips
    // _dialogue_switch_mode = 2 and calls gdHide(), tearing down the dialog
    // window -- after which the next server dialog node re-renders into a hidden
    // window and segfaults (_gdProcessUpdate -> _talkToRefreshDialogWindowRect ->
    // srcCopy on a freed buffer). The conversation belongs to the server, so the
    // button is a REQUEST like any other input: send `dbarter` and let the
    // authoritative _gdProcess open the trade (which streams EVENT_BARTER_BEGIN
    // back). Spectators are refused server-side; only the driver's request lands.
    // The CRITTER_BARTER "will not barter" check is the server's to make now.
    if (clientViewerActive()) {
        clientViewerBarterVerb("dbarter", -1, 0);
        return;
    }

    if (PID_TYPE(gGameDialogSpeaker->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    Script* script;
    if (scriptGetScript(gGameDialogSpeaker->sid, &script) == -1) {
        return;
    }

    Proto* proto;
    protoGetProto(gGameDialogSpeaker->pid, &proto);
    if (proto->critter.data.flags & CRITTER_BARTER) {
        if (gGameDialogLipSyncStarted) {
            if (soundIsPlaying(gLipsData.sound)) {
                gameDialogEndLips();
            }
        }

        _dialogue_switch_mode = 2;
        _dialogue_state = 4;

        // NOTE: Uninline.
        gdHide();
    } else {
        MessageListItem messageListItem;
        // This person will not barter with you.
        messageListItem.num = 903;
        if (gGameDialogSpeakerIsPartyMember) {
            // This critter can't carry anything.
            messageListItem.num = 913;
        }

        if (messageListGetItem(&gProtoMessageList, &messageListItem)) {
            gameDialogRenderSupplementaryMessage(messageListItem.text);
        } else {
            debugPrint("\nError: gdialog: Can't find message!");
        }
    }
}

// 0x44A62C
int _gdialog_window_create()
{
    const int screenWidth = GAME_DIALOG_WINDOW_WIDTH;

    if (_gdialog_window_created) {
        return -1;
    }

    for (int index = 0; index < 9; index++) {
        _gdialog_buttons[index] = -1;
    }

    FrmImage backgroundFrmImage;
    // 389 - di_talkp.frm - dialog screen subwindow (party members)
    // 99 - di_talk.frm - dialog screen subwindow (NPC's)
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, gGameDialogSpeakerIsPartyMember ? 389 : 99, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    unsigned char* backgroundFrmData = backgroundFrmImage.getData();
    if (backgroundFrmData != nullptr) {
        _dialogue_subwin_len = backgroundFrmImage.getHeight();

        int dialogSubwindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
        int dialogSubwindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2 + GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len;
        gGameDialogWindow = windowCreate(dialogSubwindowX, dialogSubwindowY, screenWidth, _dialogue_subwin_len, 256, WINDOW_DONT_MOVE_TOP);
        if (gGameDialogWindow != -1) {

            unsigned char* windowBuf = windowGetBuffer(gGameDialogWindow);
            unsigned char* bgWindowBuf = windowGetBuffer(gGameDialogBackgroundWindow);
            // TODO: Not sure about offsets.
            blitBufferToBuffer(bgWindowBuf + screenWidth * (GAME_DIALOG_WINDOW_HEIGHT - _dialogue_subwin_len), screenWidth, _dialogue_subwin_len, screenWidth, windowBuf, screenWidth);

            if (_dialogue_just_started) {
                windowRefresh(gGameDialogBackgroundWindow);
                _gdialog_scroll_subwin(gGameDialogWindow, true, backgroundFrmData, windowBuf, nullptr, _dialogue_subwin_len, true);
                _dialogue_just_started = 0;
            } else {
                _gdialog_scroll_subwin(gGameDialogWindow, true, backgroundFrmData, windowBuf, nullptr, _dialogue_subwin_len, false);
            }

            // BARTER/TRADE
            _gdialog_buttons[0] = buttonCreate(gGameDialogWindow, 593, 41, 14, 14, -1, -1, -1, -1, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
            if (_gdialog_buttons[0] != -1) {
                buttonSetMouseCallbacks(_gdialog_buttons[0], nullptr, nullptr, nullptr, gameDialogBarterButtonUpMouseUp);
                buttonSetCallbacks(_gdialog_buttons[0], _gsound_med_butt_press, _gsound_med_butt_release);

                // di_rest1.frm - dialog rest button up
                int upFid = buildFid(OBJ_TYPE_INTERFACE, 97, 0, 0, 0);
                if (_reviewButtonNormalFrmImage.lock(upFid)) {
                    // di_rest2.frm - dialog rest button down
                    int downFid = buildFid(OBJ_TYPE_INTERFACE, 98, 0, 0, 0);
                    if (_reviewButtonPressedFrmImage.lock(downFid)) {
                        // REVIEW
                        _gdialog_buttons[1] = buttonCreate(gGameDialogWindow, 13, 154, 51, 29, -1, -1, -1, -1, _reviewButtonNormalFrmImage.getData(), _reviewButtonPressedFrmImage.getData(), nullptr, 0);
                        if (_gdialog_buttons[1] != -1) {
                            buttonSetMouseCallbacks(_gdialog_buttons[1], nullptr, nullptr, nullptr, gameDialogReviewButtonOnMouseUp);
                            buttonSetCallbacks(_gdialog_buttons[1], _gsound_red_butt_press, _gsound_red_butt_release);

                            if (!gGameDialogSpeakerIsPartyMember) {
                                _gdialog_window_created = true;
                                return 0;
                            }

                            // COMBAT CONTROL
                            _gdialog_buttons[2] = buttonCreate(gGameDialogWindow, 593, 116, 14, 14, -1, -1, -1, -1, _redButtonNormalFrmImage.getData(), _redButtonPressedFrmImage.getData(), nullptr, BUTTON_FLAG_TRANSPARENT);
                            if (_gdialog_buttons[2] != -1) {
                                buttonSetMouseCallbacks(_gdialog_buttons[2], nullptr, nullptr, nullptr, gameDialogCombatControlButtonOnMouseUp);
                                buttonSetCallbacks(_gdialog_buttons[2], _gsound_med_butt_press, _gsound_med_butt_release);

                                _gdialog_window_created = true;
                                return 0;
                            }

                            buttonDestroy(_gdialog_buttons[1]);
                            _gdialog_buttons[1] = -1;
                        }

                        _reviewButtonPressedFrmImage.unlock();
                    }

                    _reviewButtonNormalFrmImage.unlock();
                }

                buttonDestroy(_gdialog_buttons[0]);
                _gdialog_buttons[0] = -1;
            }

            windowDestroy(gGameDialogWindow);
            gGameDialogWindow = -1;
        }
    }

    return -1;
}

// 0x44A9D8
void _gdialog_window_destroy()
{
    if (gGameDialogWindow == -1) {
        return;
    }

    for (int index = 0; index < 9; index++) {
        buttonDestroy(_gdialog_buttons[index]);
        _gdialog_buttons[index] = -1;
    }

    _reviewButtonNormalFrmImage.unlock();
    _reviewButtonPressedFrmImage.unlock();

    int offset = (GAME_DIALOG_WINDOW_WIDTH) * (480 - _dialogue_subwin_len);
    unsigned char* backgroundWindowBuffer = windowGetBuffer(gGameDialogBackgroundWindow) + offset;

    int frmId;
    if (gGameDialogSpeakerIsPartyMember) {
        // di_talkp.frm - dialog screen subwindow (party members)
        frmId = 389;
    } else {
        // di_talk.frm - dialog screen subwindow (NPC's)
        frmId = 99;
    }

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, frmId, 0, 0, 0);
    if (backgroundFrmImage.lock(backgroundFid)) {
        unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
        _gdialog_scroll_subwin(gGameDialogWindow, false, backgroundFrmImage.getData(), windowBuffer, backgroundWindowBuffer, _dialogue_subwin_len);
        windowDestroy(gGameDialogWindow);
        _gdialog_window_created = 0;
        gGameDialogWindow = -1;
    }
}

// NOTE: Inlined.
//
// 0x44AAD8
static int talk_to_create_background_window()
{
    int backgroundWindowX = (screenGetWidth() - GAME_DIALOG_WINDOW_WIDTH) / 2;
    int backgroundWindowY = (screenGetHeight() - GAME_DIALOG_WINDOW_HEIGHT) / 2;
    gGameDialogBackgroundWindow = windowCreate(backgroundWindowX,
        backgroundWindowY,
        GAME_DIALOG_WINDOW_WIDTH,
        GAME_DIALOG_WINDOW_HEIGHT,
        256,
        WINDOW_DONT_MOVE_TOP | WINDOW_MODAL);

    if (gGameDialogBackgroundWindow != -1) {
        return 0;
    }

    return -1;
}

// 0x44AB18
int gameDialogWindowRenderBackground()
{
    FrmImage backgroundFrmImage;
    // alltlk.frm - dialog screen background
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 103, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    int windowWidth = GAME_DIALOG_WINDOW_WIDTH;
    unsigned char* windowBuffer = windowGetBuffer(gGameDialogBackgroundWindow);
    blitBufferToBuffer(backgroundFrmImage.getData(), windowWidth, 480, windowWidth, windowBuffer, windowWidth);

    if (!_dialogue_just_started) {
        windowRefresh(gGameDialogBackgroundWindow);
    }

    return 0;
}

// 0x44ABA8
int _talkToRefreshDialogWindowRect(Rect* rect)
{
    int frmId;
    if (gGameDialogSpeakerIsPartyMember) {
        // di_talkp.frm - dialog screen subwindow (party members)
        frmId = 389;
    } else {
        // di_talk.frm - dialog screen subwindow (NPC's)
        frmId = 99;
    }

    FrmImage backgroundFrmImage;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, frmId, 0, 0, 0);
    if (!backgroundFrmImage.lock(backgroundFid)) {
        return -1;
    }

    int offset = 640 * rect->top + rect->left;

    unsigned char* windowBuffer = windowGetBuffer(gGameDialogWindow);
    blitBufferToBuffer(backgroundFrmImage.getData() + offset,
        rect->right - rect->left,
        rect->bottom - rect->top,
        GAME_DIALOG_WINDOW_WIDTH,
        windowBuffer + offset,
        GAME_DIALOG_WINDOW_WIDTH);

    windowRefreshRect(gGameDialogWindow, rect);

    return 0;
}

// 0x44AC68
void gameDialogRenderHighlight(unsigned char* src, int srcWidth, int srcHeight, int srcPitch, unsigned char* dest, int destX, int destY, int destPitch, unsigned char* a9, unsigned char* a10)
{
    int srcStep = srcPitch - srcWidth;
    int destStep = destPitch - srcWidth;

    dest += destPitch * destY + destX;

    for (int y = 0; y < srcHeight; y++) {
        for (int x = 0; x < srcWidth; x++) {
            unsigned char v1 = *src++;
            if (v1 != 0) {
                v1 = (256 - v1) >> 4;
            }

            unsigned char v15 = *dest;
            *dest++ = a9[256 * v1 + v15];
        }
        src += srcStep;
        dest += destStep;
    }
}

// 0x44ACFC
void gameDialogRenderTalkingHead(Art* headFrm, int frame)
{
    if (gGameDialogWindow == -1) {
        return;
    }

    if (headFrm != nullptr) {
        if (frame == 0) {
            _totalHotx = 0;
        }

        FrmImage backgroundFrmImage;
        int backgroundFid = buildFid(OBJ_TYPE_BACKGROUND, gGameDialogBackground, 0, 0, 0);
        if (!backgroundFrmImage.lock(backgroundFid)) {
            debugPrint("\tError locking background in display...\n");
        }

        unsigned char* backgroundFrmData = backgroundFrmImage.getData();
        if (backgroundFrmData != nullptr) {
            blitBufferToBuffer(backgroundFrmData, 388, 200, 388, gGameDialogDisplayBuffer, GAME_DIALOG_WINDOW_WIDTH);
        } else {
            debugPrint("\tError getting background data in display...\n");
        }

        int width = artGetWidth(headFrm, frame, 0);
        int height = artGetHeight(headFrm, frame, 0);
        unsigned char* data = artGetFrameData(headFrm, frame, 0);

        int a3;
        int v8;
        artGetRotationOffsets(headFrm, 0, &a3, &v8);

        int a4;
        int a5;
        artGetFrameOffsets(headFrm, frame, 0, &a4, &a5);

        _totalHotx += a4;
        a3 += _totalHotx;

        if (data != nullptr) {
            int destWidth = GAME_DIALOG_WINDOW_WIDTH;
            int destOffset = destWidth * (200 - height) + a3 + (388 - width) / 2;
            if (destOffset + width * v8 > 0) {
                destOffset += width * v8;
            }

            blitBufferToBufferTrans(
                data,
                width,
                height,
                width,
                gGameDialogDisplayBuffer + destOffset,
                destWidth);
        } else {
            debugPrint("\tError getting head data in display...\n");
        }
    } else {
        if (_talk_need_to_center == 1) {
            _talk_need_to_center = 0;
            tileWindowRefresh();
        }

        unsigned char* src = windowGetBuffer(gIsoWindow);

        // Usually rendering functions use `screenGetWidth`/`screenGetHeight` to
        // determine rendering position. However in this case `windowGetHeight`
        // is a must because isometric window's height can either include
        // interface bar or not. Offset is updated accordingly (332 -> 232, the
        // missing 100 is interface bar height, which is already accounted for
        // when we're using `windowGetHeight`). `windowGetWidth` is used for
        // consistency.
        blitBufferToBuffer(
            src + ((windowGetHeight(gIsoWindow) - 232) / 2) * windowGetWidth(gIsoWindow) + (windowGetWidth(gIsoWindow) - 388) / 2,
            388,
            200,
            windowGetWidth(gIsoWindow),
            gGameDialogDisplayBuffer,
            GAME_DIALOG_WINDOW_WIDTH);
    }

    Rect v27;
    v27.left = 126;
    v27.top = 14;
    v27.right = 514;
    v27.bottom = 214;

    unsigned char* dest = windowGetBuffer(gGameDialogBackgroundWindow);

    gameDialogRenderHighlight(_upperHighlightFrmImage.getData(),
        _upperHighlightFrmImage.getWidth(),
        _upperHighlightFrmImage.getHeight(),
        _upperHighlightFrmImage.getWidth(),
        dest,
        426,
        15,
        GAME_DIALOG_WINDOW_WIDTH,
        _light_BlendTable,
        _light_GrayTable);

    gameDialogRenderHighlight(_lowerHighlightFrmImage.getData(),
        _lowerHighlightFrmImage.getWidth(),
        _lowerHighlightFrmImage.getHeight(),
        _lowerHighlightFrmImage.getWidth(),
        dest,
        129,
        214 - _lowerHighlightFrmImage.getHeight() - 2,
        GAME_DIALOG_WINDOW_WIDTH,
        _dark_BlendTable,
        _dark_GrayTable);

    for (int index = 0; index < 8; ++index) {
        Rect* rect = &(_backgrndRects[index]);
        int width = rect->right - rect->left;

        blitBufferToBufferTrans(_backgrndBufs[index],
            width,
            rect->bottom - rect->top,
            width,
            dest + GAME_DIALOG_WINDOW_WIDTH * rect->top + rect->left,
            GAME_DIALOG_WINDOW_WIDTH);
    }

    windowRefreshRect(gGameDialogBackgroundWindow, &v27);
}

// 0x44B080
void gameDialogHighlightsInit()
{
    for (int color = 0; color < 256; color++) {
        int r = (Color2RGB(color) & 0x7C00) >> 10;
        int g = (Color2RGB(color) & 0x3E0) >> 5;
        int b = Color2RGB(color) & 0x1F;
        _light_GrayTable[color] = ((r + 2 * g + 2 * b) / 10) >> 2;
        _dark_GrayTable[color] = ((r + g + b) / 10) >> 2;
    }

    _light_GrayTable[0] = 0;
    _dark_GrayTable[0] = 0;

    _light_BlendTable = _getColorBlendTable(_colorTable[17969]);
    _dark_BlendTable = _getColorBlendTable(_colorTable[22187]);

    // hilight1.frm - dialogue upper hilight
    int upperHighlightFid = buildFid(OBJ_TYPE_INTERFACE, 115, 0, 0, 0);
    _upperHighlightFrmImage.lock(upperHighlightFid);

    // hilight2.frm - dialogue lower hilight
    int lowerHighlightFid = buildFid(OBJ_TYPE_INTERFACE, 116, 0, 0, 0);
    _lowerHighlightFrmImage.lock(lowerHighlightFid);
}

// NOTE: Inlined.
//
// 0x44B1D4
static void gameDialogHighlightsExit()
{
    _freeColorBlendTable(_colorTable[17969]);
    _freeColorBlendTable(_colorTable[22187]);

    _upperHighlightFrmImage.unlock();
    _lowerHighlightFrmImage.unlock();
}

static void gameDialogRedButtonsInit()
{
    // di_rdbt2.frm - dialog red button up
    int normalFid = buildFid(OBJ_TYPE_INTERFACE, 96, 0, 0, 0);
    if (!_redButtonNormalFrmImage.lock(normalFid)) {
        gameDialogRedButtonsExit();
    }

    // di_rdbt1.frm - dialog red button down
    int pressedFid = buildFid(OBJ_TYPE_INTERFACE, 95, 0, 0, 0);
    if (!_redButtonPressedFrmImage.lock(pressedFid)) {
        gameDialogRedButtonsExit();
    }
}

static void gameDialogRedButtonsExit()
{
    _redButtonNormalFrmImage.unlock();
    _redButtonPressedFrmImage.unlock();
}

} // namespace fallout
