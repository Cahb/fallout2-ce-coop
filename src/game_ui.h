#ifndef FALLOUT_GAME_UI_H_
#define FALLOUT_GAME_UI_H_

namespace fallout {

// Client-side UI/input seam for game.cc (REWRITE_PLAN, TU split).
//
// The key dispatcher (gameHandleKey), the UI enable/disable gate (gameUi*),
// and the modal help/splash/quit/death screens that used to live in game.cc
// now live in game_ui.cc (the f2_client side). The public UI entry points
// (gameHandleKey/gameUiDisable/gameUiEnable/gameUiIsDisabled/
// showQuitConfirmationDialog/gameShowDeathDialog) stay declared in game.h
// since existing callers include that.
//
// This header declares the pieces the split newly exposes:
//  - showSplash(), the one core->UI callee: core gameInitWithOptions (which
//    stays in game.cc) calls it by name during startup. It was de-static'd for
//    this seam; its body lives in game_ui.cc.
//  - an extern view of gIsMapper, the mapper-mode flag core game.cc owns
//    (defines and writes in gameInitWithOptions) and the moved gameHandleKey
//    reads. This one-way exposure is the mechanical stand-in for the eventual
//    client-init hook.

// Splash screen shown by core gameInitWithOptions during startup. Body lives
// in game_ui.cc.
void showSplash();

// game.cc mapper-mode flag. The definition stays in game.cc (core owns and
// writes it); the UI layer only reads it.
extern bool gIsMapper;

} // namespace fallout

#endif /* FALLOUT_GAME_UI_H_ */
