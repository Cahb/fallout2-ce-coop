#include "combat_ui.h"

#include <limits.h>
#include <string.h>

#include "combat.h"

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat_defs.h"
#include "combat_intent.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "draw.h"
#include "game.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "message.h"
#include "object.h"
#include "perk.h"
#include "presenter.h"
#include "scripts.h"
#include "server_loop.h"
#include "settings.h"
#include "sfall_global_scripts.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "window_manager.h"

// Client-side combat HUD/display seam extracted from combat.cc (REWRITE_PLAN,
// TU split). Pure presentation/input chrome: the input/render pump, the attack
// description printer, the called-shot hit-location picker and its drawing
// helpers, and the critter-outline highlight toggles. Core combat.cc keeps the
// turn-resolution/AP/to-hit/damage/AI sim and owns all combat state; it calls
// these UI functions by name (public ones resolve via combat.h, the two
// de-static'd ones via combat_ui.h). The moved bodies are byte-identical; this
// move is mechanical and replay-identical.
//
// Note: unlike the one-way-outward render/UI seams elsewhere in the rewrite,
// combat's core<->UI edges are INWARD direct by-name calls in both directions:
// core sim invokes _combat_turn_run/_combat_display/_combat_outline_on/off and
// calledShotSelectHitLocation, while the moved input/outline pumps call back
// into core combatAttemptEnd()/_combat_update_critters_in_los(). That is fine
// for this same-binary TU split; the future headless/link-split will need
// presenter methods or callbacks for these -- the presenter does NOT yet cover
// combat presentation.

namespace fallout {

#define CALLED_SHOT_WINDOW_Y (20)
#define CALLED_SHOT_WINDOW_WIDTH (504)
#define CALLED_SHOT_WINDOW_HEIGHT (309)

static void _print_tohit(unsigned char* dest, int dest_pitch, int a3);
static void _draw_loc_off(int a1, int a2);
static void _draw_loc_on_(int a1, int a2);
static void _draw_loc_(int eventCode, int color);

// 0x51802C
static const int _call_ty[4] = {
    122,
    188,
    251,
    316,
};

// 0x51803C
static const int _hit_loc_left[4] = {
    HIT_LOCATION_HEAD,
    HIT_LOCATION_EYES,
    HIT_LOCATION_RIGHT_ARM,
    HIT_LOCATION_RIGHT_LEG,
};

// 0x51804C
static const int _hit_loc_right[4] = {
    HIT_LOCATION_TORSO,
    HIT_LOCATION_GROIN,
    HIT_LOCATION_LEFT_ARM,
    HIT_LOCATION_LEFT_LEG,
};

// 0x56D370
static Object* gCalledShotCritter;

// 0x56D374
static int gCalledShotWindow;

// Client-interactive frame pump for _combat_turn_run (combat_drain.cc): drains
// in-flight combat animations at the fps limit with renderPresent. The headless
// server drains synchronously in combat_drain.cc and never calls this.
//
// 0x4227DC
void combatTurnRunClient()
{
    while (_combat_turn_running > 0) {
        sharedFpsLimiter.mark();

        _process_bk();

        renderPresent();
        sharedFpsLimiter.throttle();
    }
}

// Client-interactive keyboard/mouse turn loop for _combat_input (combat_drain.cc).
// The headless server drives the dude's turn from the intent queue in
// combat_drain.cc and never calls this; the enclosing ScopedGameMode(kPlayerTurn)
// is established by the core _combat_input before this is invoked.
int combatInputClient()
{
    while ((gCombatState & COMBAT_STATE_0x02) != 0) {
        sharedFpsLimiter.mark();

        if (combatPlayerTurnShouldBreak()) {
            break;
        }

        int keyCode = inputGetInput();

        // SFALL: CombatLoopHook.
        sfall_gl_scr_process_main();

        if (_action_explode_running()) {
            // NOTE: Uninline.
            _combat_turn_run();
        }

        if (combatPlayerTurnOutOfAp()) {
            break;
        }

        if (keyCode == KEY_SPACE) {
            break;
        }

        if (keyCode == KEY_RETURN) {
            combatAttemptEnd();
        } else {
            _scripts_check_state_in_combat();
            gameHandleKey(keyCode, true);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    return combatPlayerTurnResolve();
}

// Render two digits.
//
// 0x42603C
static void _print_tohit(unsigned char* dest, int destPitch, int accuracy)
{
    FrmImage numbersFrmImage;
    int numbersFid = buildFid(OBJ_TYPE_INTERFACE, 82, 0, 0, 0);
    if (!numbersFrmImage.lock(numbersFid)) {
        return;
    }

    if (accuracy >= 0) {
        blitBufferToBuffer(numbersFrmImage.getData() + 9 * (accuracy % 10), 9, 17, 360, dest + 9, destPitch);
        blitBufferToBuffer(numbersFrmImage.getData() + 9 * (accuracy / 10), 9, 17, 360, dest, destPitch);
    } else {
        blitBufferToBuffer(numbersFrmImage.getData() + 108, 6, 17, 360, dest + 9, destPitch);
        blitBufferToBuffer(numbersFrmImage.getData() + 108, 6, 17, 360, dest, destPitch);
    }
}

static void _draw_loc_off(int a1, int a2)
{
    _draw_loc_(a2, _colorTable[992]);
}

// 0x4261C0
static void _draw_loc_on_(int a1, int a2)
{
    _draw_loc_(a2, _colorTable[31744]);
}

// 0x4261CC
static void _draw_loc_(int eventCode, int color)
{
    color |= 0x3000000;

    if (eventCode >= 4) {
        char* name = hitLocationGetName(gCalledShotCritter, _hit_loc_right[eventCode - 4]);
        int width = fontGetStringWidth(name);
        windowDrawText(gCalledShotWindow, name, 0, 431 - width, _call_ty[eventCode - 4] - 86, color);
    } else {
        char* name = hitLocationGetName(gCalledShotCritter, _hit_loc_left[eventCode]);
        windowDrawText(gCalledShotWindow, name, 0, 74, _call_ty[eventCode] - 86, color);
    }
}

// Client-interactive called-shot hit-location modal picker, invoked by the core
// dispatcher calledShotSelectHitLocation (combat_drain.cc) when !serverLoopActive().
// The headless server passes an explicit hit location to _combat_attack and never
// reaches this.
//
// 0x426218
int calledShotSelectHitLocationClient(Object* critter, int* hitLocation, int hitMode)
{
    *hitLocation = HIT_LOCATION_TORSO;

    if (critter == nullptr) {
        return 0;
    }

    if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER) {
        return 0;
    }

    gCalledShotCritter = critter;

    // The default value is 68, which centers called shot window given it's
    // width (68 - 504 - 68).
    int calledShotWindowX = (screenGetWidth() - CALLED_SHOT_WINDOW_WIDTH) / 2;
    // Center vertically for HRP, otherwise maintain original location (20).
    int calledShotWindowY = screenGetHeight() != 480
        ? (screenGetHeight() - INTERFACE_BAR_HEIGHT - 1 - CALLED_SHOT_WINDOW_HEIGHT) / 2
        : CALLED_SHOT_WINDOW_Y;
    gCalledShotWindow = windowCreate(calledShotWindowX,
        calledShotWindowY,
        CALLED_SHOT_WINDOW_WIDTH,
        CALLED_SHOT_WINDOW_HEIGHT,
        _colorTable[0],
        WINDOW_MODAL);
    if (gCalledShotWindow == -1) {
        return -1;
    }

    unsigned char* windowBuffer = windowGetBuffer(gCalledShotWindow);

    FrmImage backgroundFrm;
    int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 118, 0, 0, 0);
    if (!backgroundFrm.lock(backgroundFid)) {
        windowDestroy(gCalledShotWindow);
        return -1;
    }

    blitBufferToBuffer(backgroundFrm.getData(),
        CALLED_SHOT_WINDOW_WIDTH,
        CALLED_SHOT_WINDOW_HEIGHT,
        CALLED_SHOT_WINDOW_WIDTH,
        windowBuffer,
        CALLED_SHOT_WINDOW_WIDTH);

    FrmImage critterFrm;
    int critterFid = buildFid(OBJ_TYPE_CRITTER, critter->fid & 0xFFF, ANIM_CALLED_SHOT_PIC, 0, 0);
    if (critterFrm.lock(critterFid)) {
        blitBufferToBuffer(critterFrm.getData(),
            170,
            225,
            170,
            windowBuffer + CALLED_SHOT_WINDOW_WIDTH * 31 + 168,
            CALLED_SHOT_WINDOW_WIDTH);
    }

    FrmImage cancelButtonNormalFrmImage;
    int cancelButtonNormalFid = buildFid(OBJ_TYPE_INTERFACE, 8, 0, 0, 0);
    if (!cancelButtonNormalFrmImage.lock(cancelButtonNormalFid)) {
        windowDestroy(gCalledShotWindow);
        return -1;
    }

    FrmImage cancelButtonPressedFrmImage;
    int cancelButtonPressedFid = buildFid(OBJ_TYPE_INTERFACE, 9, 0, 0, 0);
    if (!cancelButtonPressedFrmImage.lock(cancelButtonPressedFid)) {
        windowDestroy(gCalledShotWindow);
        return -1;
    }

    // Cancel button
    int cancelBtn = buttonCreate(gCalledShotWindow,
        210,
        268,
        15,
        16,
        -1,
        -1,
        -1,
        KEY_ESCAPE,
        cancelButtonNormalFrmImage.getData(),
        cancelButtonPressedFrmImage.getData(),
        nullptr,
        BUTTON_FLAG_TRANSPARENT);
    if (cancelBtn != -1) {
        buttonSetCallbacks(cancelBtn, _gsound_red_butt_press, _gsound_red_butt_release);
    }

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    for (int index = 0; index < 4; index++) {
        int probability;
        int btn;

        probability = _determine_to_hit(gDude, critter, _hit_loc_left[index], hitMode);
        _print_tohit(windowBuffer + CALLED_SHOT_WINDOW_WIDTH * (_call_ty[index] - 86) + 33, CALLED_SHOT_WINDOW_WIDTH, probability);

        btn = buttonCreate(gCalledShotWindow, 33, _call_ty[index] - 90, 128, 20, index, index, -1, index, nullptr, nullptr, nullptr, 0);
        buttonSetMouseCallbacks(btn, _draw_loc_on_, _draw_loc_off, nullptr, nullptr);
        _draw_loc_(index, _colorTable[992]);

        probability = _determine_to_hit(gDude, critter, _hit_loc_right[index], hitMode);
        _print_tohit(windowBuffer + CALLED_SHOT_WINDOW_WIDTH * (_call_ty[index] - 86) + 453, CALLED_SHOT_WINDOW_WIDTH, probability);

        btn = buttonCreate(gCalledShotWindow, 341, _call_ty[index] - 90, 128, 20, index + 4, index + 4, -1, index + 4, nullptr, nullptr, nullptr, 0);
        buttonSetMouseCallbacks(btn, _draw_loc_on_, _draw_loc_off, nullptr, nullptr);
        _draw_loc_(index + 4, _colorTable[992]);
    }

    windowRefresh(gCalledShotWindow);

    bool gameUiWasDisabled = gameUiIsDisabled();
    if (gameUiWasDisabled) {
        gameUiEnable();
    }

    _gmouse_disable(0);
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);

    int eventCode;
    while (true) {
        sharedFpsLimiter.mark();

        eventCode = inputGetInput();

        if (eventCode == KEY_ESCAPE) {
            break;
        }

        if (eventCode >= 0 && eventCode < HIT_LOCATION_COUNT) {
            break;
        }

        if (_game_user_wants_to_quit != 0) {
            break;
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    _gmouse_enable();

    if (gameUiWasDisabled) {
        gameUiDisable(0);
    }

    fontSetCurrent(oldFont);

    windowDestroy(gCalledShotWindow);

    if (eventCode == KEY_ESCAPE) {
        return -1;
    }

    *hitLocation = eventCode < 4 ? _hit_loc_left[eventCode] : _hit_loc_right[eventCode - 4];

    soundPlayFile("icsxxxx1");

    return 0;
}

} // namespace fallout
