#ifndef FALLOUT_COMBAT_UI_H_
#define FALLOUT_COMBAT_UI_H_

#include "message.h"
#include "obj_types.h"

namespace fallout {

// Client-side combat HUD/display seam for combat.cc (REWRITE_PLAN, TU split).
//
// The input/render pump, the attack-description printer, the called-shot
// hit-location picker and its chrome, and the critter-outline highlight toggles
// that used to live in combat.cc now live in combat_ui.cc (the f2_client side).
// The public UI entry points (_combat_turn_run, _combat_display,
// _combat_outline_on, _combat_outline_off) stay declared in combat.h since
// existing callers include that.
//
// This header declares the pieces the split newly exposes:
//  - the two functions that were de-static'd because core combat.cc invokes
//    them by name: _combat_input() (called by core _combat_turn) and
//    calledShotSelectHitLocation() (called by core _combat_attack_this). Their
//    bodies live in combat_ui.cc.
//  - two core sim functions the moved UI calls back into and which therefore
//    had to be de-static'd: combatAttemptEnd() (invoked by _combat_input) and
//    _combat_update_critters_in_los() (invoked by _combat_outline_on). Their
//    definitions stay in combat.cc (core owns the combat lists/state they
//    touch); the UI layer only calls them.
//  - four read-only externs onto combat state that core combat.cc owns and
//    writes; the moved UI functions only read them.
//
// Unlike the one-way-outward render/UI seams elsewhere in the rewrite, combat's
// core<->UI edges are INWARD direct by-name calls in both directions (core sim
// -> moved UI, and moved UI -> the two core callbacks above). That is fine for
// this same-binary TU split; the future headless/link-split will need presenter
// methods or callbacks for these -- the presenter does NOT yet cover combat
// presentation.

// Combat state owned and written by core combat.cc; the moved UI only reads it.
extern int _combat_turn_running;
extern MessageList gCombatMessageList;
extern int _list_total;
extern Object** _combat_list;

// De-static'd UI functions invoked by core combat.cc by name. Bodies live in
// combat_drain.cc (f2_core) — the serverLoopActive() headless branch of each is
// authoritative; they delegate the interactive tail to the combat*Client helpers
// below (combat_ui.cc, f2_client).
int _combat_input();
int calledShotSelectHitLocation(Object* critter, int* hitLocation, int hitMode);

// Client-interactive tails invoked by the core combat drivers in combat_drain.cc
// when !serverLoopActive(). Bodies live in combat_ui.cc (f2_client); f2_server
// stubs them (never reached — the server always drives the headless branch).
void combatTurnRunClient();
int combatInputClient();
int calledShotSelectHitLocationClient(Object* critter, int* hitLocation, int hitMode);

// Attack-description hit-location name lookup. Lives in combat_drain.cc (f2_core,
// SDL-free) alongside _combat_display; still called by the called-shot chrome in
// combat_ui.cc.
char* hitLocationGetName(Object* critter, int hitLocation);

// Core sim functions invoked by the moved UI. Definitions stay in combat.cc.
void combatAttemptEnd();
void _combat_update_critters_in_los(bool a1);

} // namespace fallout

#endif /* FALLOUT_COMBAT_UI_H_ */
