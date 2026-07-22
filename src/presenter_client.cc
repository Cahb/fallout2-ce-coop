#include "presenter_client.h"

#include "animation.h"
#include "color.h"
#include "display_monitor.h"
#include "game.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "map.h"
#include "palette.h"
#include "presenter.h"
#include "text_object.h"
#include "tile.h"
#include "window_manager.h"

namespace fallout {

// Reproduces legacy presentation behavior 1:1 (see presenter.h). This file
// belongs to the client side of the split; it is the only presenter that
// touches UI subsystems.
class ClientPresenter : public Presenter {
public:
    void consoleMessage(const char* text) override
    {
        displayMonitorAddMessage(const_cast<char*>(text));
    }

    void worldInvalidate() override
    {
        tileWindowRefresh();
    }

    void worldInvalidateRect(const Rect* rect, int elevation) override
    {
        tileWindowRefreshRect(const_cast<Rect*>(rect), elevation);
    }

    void worldClear() override
    {
        windowFill(gIsoWindow, 0, 0, windowGetWidth(gIsoWindow), windowGetHeight(gIsoWindow), _colorTable[0]);
        windowRefresh(gIsoWindow);
    }

    void ambientSoundLoad(const char* name, int a2, int a3, int a4) override
    {
        backgroundSoundLoad(name, a2, a3, a4);
    }

    void hudBarShow() override
    {
        interfaceBarShow();
    }

    void movieFadeOut() override
    {
        gameMovieFadeOut();
    }

    void mouseResetBouncingCursor() override
    {
        gameMouseResetBouncingCursorFid();
    }

    void worldEnable() override
    {
        textObjectsEnable();
        if (!gameUiIsDisabled()) {
            _gmouse_enable();
        }
        tickersAdd(_object_animate);
        tickersAdd(_dude_fidget);
    }

    void worldDisable() override
    {
        tickersRemove(_dude_fidget);
        tickersRemove(_object_animate);
        _gmouse_disable(0);
        textObjectsDisable();
    }

    void floatText(Object* owner, const char* text, int font, int color, int outlineColor) override
    {
        Rect rect;
        if (textObjectAdd(owner, const_cast<char*>(text), font, color, outlineColor, &rect) == 0) {
            tileWindowRefreshRect(&rect, owner->elevation);
        }
    }

    void hudHitPoints(bool animate) override
    {
        interfaceRenderHitPoints(animate);
    }

    void hudArmorClass(bool animate) override
    {
        interfaceRenderArmorClass(animate);
    }

    void hudActionPoints(int actionPointsLeft, int bonusActionPoints) override
    {
        interfaceRenderActionPoints(actionPointsLeft, bonusActionPoints);
    }

    void hudItems(bool animated, int leftItemAction, int rightItemAction) override
    {
        interfaceUpdateItems(animated, leftItemAction, rightItemAction);
    }

    void hudIndicatorBar() override
    {
        indicatorBarRefresh();
    }

    void sfxPlay(const char* name) override
    {
        soundPlayFile(name);
    }

    void sfxPlayAt(const char* name, Object* source) override
    {
        int volume = _gsound_compute_relative_volume(source);
        _gsound_play_sfx_file_volume(name, volume);
    }

    void screenFadeOut() override
    {
        paletteFadeTo(gPaletteBlack);
    }

    void screenFadeIn() override
    {
        paletteFadeTo(_cmap);
    }

    void hudEndButtonsShow(bool animated) override
    {
        interfaceBarEndButtonsShow(animated);
    }

    void hudEndButtonsHide(bool animated) override
    {
        interfaceBarEndButtonsHide(animated);
    }

    void hudEndButtonsGreen() override
    {
        interfaceBarEndButtonsRenderGreenLights();
    }

    void hudEndButtonsRed() override
    {
        interfaceBarEndButtonsRenderRedLights();
    }

    void cursorSet(int cursor) override
    {
        gameMouseSetCursor(cursor);
    }

    void cursorModeSet(int mode) override
    {
        gameMouseSetMode(mode);
    }

    void cursorRefresh() override
    {
        _gmouse_3d_refresh();
    }

    void mouseObjectsShow() override
    {
        gameMouseObjectsShow();
    }

    void mouseObjectsHide() override
    {
        gameMouseObjectsHide();
    }

    void scrollEnable() override
    {
        _gmouse_enable_scrolling();
    }

    void scrollDisable() override
    {
        _gmouse_disable_scrolling();
    }

    int musicPlayLevel(const char* fileName, int fadeIn) override
    {
        return _gsound_background_play_level_music(fileName, fadeIn);
    }

    void musicStop() override
    {
        backgroundSoundDelete();
    }

    void errorBox(const char* text) override
    {
        showMesageBox(text);
    }
};

static ClientPresenter gClientPresenter;

void presenterInstallClient()
{
    presenterSet(&gClientPresenter);
}

} // namespace fallout
