#include "script_request_handler_client.h"

#include "automap.h"
#include "elevator.h"
#include "endgame.h"
#include "game_dialog.h"
#include "inventory.h"
#include "script_request_handler.h"
#include "worldmap.h"

namespace fallout {

// Forwards each script-request seam method to the legacy client call. This file
// is the only script-request handler that touches UI subsystems; it belongs to
// the client side of the split (mirrors presenter_client.cc).
class ClientScriptRequestHandler : public ScriptRequestHandler {
public:
    void townMap() override
    {
        wmTownMap();
    }

    void worldMap() override
    {
        wmWorldMap();
    }

    int elevatorSelect(int elevator, int* map, int* elevation, int* tile) override
    {
        return elevatorSelectLevel(elevator, map, elevation, tile);
    }

    void automapSave() override
    {
        automapSaveCurrent();
    }

    void dialogEnter(Object* speaker) override
    {
        gameDialogEnter(speaker, 0);
    }

    void endgame() override
    {
        endgamePlaySlideshow();
        endgamePlayMovie();
    }

    void looting(Object* looter, Object* target) override
    {
        inventoryOpenLooting(looter, target);
    }

    void stealing(Object* thief, Object* target) override
    {
        inventoryOpenStealing(thief, target);
    }
};

static ClientScriptRequestHandler gClientScriptRequestHandler;

void scriptRequestHandlerInstallClient()
{
    scriptRequestHandlerSet(&gClientScriptRequestHandler);
}

} // namespace fallout
