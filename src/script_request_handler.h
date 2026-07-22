#ifndef FALLOUT_SCRIPT_REQUEST_HANDLER_H_
#define FALLOUT_SCRIPT_REQUEST_HANDLER_H_

#include "obj_types.h"

namespace fallout {

// Seam for the presentation/UI side of the deferred script-request drain
// (scripts.cc scriptsHandleRequests / _scripts_check_state_in_combat) and the
// worldmap-entry branch of mapHandleTransition (map.cc) — both on the serverTick
// spine (server_loop.cc).
//
// Those drains service requests a script queued during the beat: enter combat,
// switch map/elevation, open a dialog, play the endgame, loot/steal a container,
// enter the town/world map. Combat and the elevator POST-selection world
// mutations (rotation/placement/door flags/mapSetTransition) are pure sim and
// stay inline in the caller; only the handful of calls that reach the UI
// (worldmap screens, the elevator level picker, automap save, the modal dialog,
// the endgame slideshow/movie, the loot/steal inventory screens) route through
// this handler, so f2_core does not name those client symbols at link time.
//
// The base class is the null handler (all no-ops): what a dedicated server runs.
// A dropped request is DETERMINISTIC — elevatorSelect returns -1, so the caller
// skips the whole elevator placement/transition block exactly as if the player
// had cancelled the level picker; townMap/worldMap no-op and the caller clears
// the pending transition. ClientScriptRequestHandler
// (script_request_handler_client.cc) forwards each method to the legacy client
// call; the client installs it at boot (game_lifecycle.cc), alongside
// presenterInstallClient().
//
// Unlike Presenter, this seam is intentionally TWO-WAY: elevatorSelect fills the
// chosen map/elevation/tile the sim then acts on. That is why it is a distinct
// handler rather than another one-way Presenter method.
class ScriptRequestHandler {
public:
    virtual ~ScriptRequestHandler() = default;

    // SCRIPT_REQUEST_TOWN_MAP / SCRIPT_REQUEST_WORLD_MAP, and the map==-1/-2
    // branches of mapHandleTransition: enter the worldmap UI.
    virtual void townMap() {}
    virtual void worldMap() {}

    // SCRIPT_REQUEST_ELEVATOR: run the elevator level picker. Fills *map /
    // *elevation / *tile with the chosen destination and returns >= 0 on a
    // selection, or -1 if cancelled. Base returns -1 (no UI → no selection) so the
    // caller skips the whole placement/transition block.
    virtual int elevatorSelect(int elevator, int* map, int* elevation, int* tile) { return -1; }

    // Persist the current level's automap (paired with a committed elevator move).
    virtual void automapSave() {}

    // SCRIPT_REQUEST_DIALOG: open the modal conversation with `speaker`.
    virtual void dialogEnter(Object* speaker) {}

    // SCRIPT_REQUEST_ENDGAME: play the ending slideshow then movie.
    virtual void endgame() {}

    // SCRIPT_REQUEST_LOOTING / SCRIPT_REQUEST_STEALING: open the loot / steal
    // inventory screen.
    virtual void looting(Object* looter, Object* target) {}
    virtual void stealing(Object* thief, Object* target) {}
};

// Current handler; never null (defaults to the built-in null handler).
ScriptRequestHandler* scriptRequestHandler();

// Install a handler (nullptr restores the null handler).
void scriptRequestHandlerSet(ScriptRequestHandler* newHandler);

} // namespace fallout

#endif /* FALLOUT_SCRIPT_REQUEST_HANDLER_H_ */
