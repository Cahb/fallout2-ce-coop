#include "script_request_handler_server.h"

#include <cstdio>
#include "critter.h"
#include "msg_channel.h"
#include "object.h"
#include "presenter.h"
#include "game_dialog.h"
#include "script_request_handler.h"
#include "server_control.h" // dialog DRIVE ownership — who started this conversation
#include "server_players.h" // ServerActorScope / playerActorAt
#include "server_worldmap.h"

namespace fallout {

// The dedicated server's ScriptRequestHandler. Mirrors ClientScriptRequestHandler
// for dialog ONLY: SCRIPT_REQUEST_DIALOG runs the authoritative conversation on
// the server (game_dialog.cc executes the choice procs that mutate gvars/lvars;
// the viewer only renders a node + returns the picked index — Stage A2/A3).
//
// Every other request is intentionally left as the base no-op: looting, stealing,
// endgame, town/world map and elevator selection are dropped on the server exactly
// as the previous null handler dropped them (client-side modal presentation, not
// yet server-authoritative). This keeps A1 a pure additive unblock of the dialog
// path with no behavior change to any other request.
class ServerScriptRequestHandler : public ScriptRequestHandler {
public:
    void dialogEnter(Object* speaker) override
    {
        // ►► THE ONE PLACE A SERVER-SIDE CONVERSATION IS ENTERED, and gameDialogEnter
        // BLOCKS here for its whole life (the barrier pumps inside it). So a single
        // scope covers the entire conversation — every node proc, every `dude_obj`
        // read, and BARTER, which anchors `_inven_dude = gDude` when the trade screen
        // opens (inventory_ui.cc). That anchor is why barter was host-only in
        // practice, and why scoping here is what unlocks it for an extra rather than
        // barter needing a pass of its own.
        //
        // server_players.h names this exact nest ("the dialog barrier holds a
        // conversation-long scope while its pump services other sessions' verbs"),
        // so the destructor's restore-the-PREVIOUS-context discipline is already
        // written for it: an inner verb scope must not re-anchor the rest of the
        // conversation onto the host on its way out.
        //
        // A null driver (slot -1: nobody asked, i.e. the NPC opened the conversation
        // itself) leaves the scope DISENGAGED, so gDude keeps whatever the enclosing
        // context set. That is today's behavior verbatim, which is what keeps every
        // golden and the single-actor case byte-identical.
        int driverSlot = serverControlBeginDialogDrive();

        // Tell the OTHER players why the world just stopped. A conversation parks
        // the tick in the dialog barrier exactly like a trade does, so from every
        // other seat the game freezes with no explanation -- and an unexplained
        // freeze reads as a crash. Same treatment, same reason, as the trade line.
        //
        // Emitted BEFORE gameDialogEnter: consoleMessageStyled only buffers, and
        // the flush comes from the first dialogEmitNode inside the barrier. After
        // that call we are already blocked.
        Object* driver = driverSlot >= 0 ? playerActorAt(driverSlot) : nullptr;
        if (driver != nullptr) {
            for (int slot = 0; slot < playerActorCount(); slot++) {
                Object* other = playerActorAt(slot);
                if (other == nullptr || other == driver) {
                    continue;
                }
                char line[256];
                snprintf(line, sizeof(line), "%s is talking to %s. Please wait.",
                    critterGetName(driver), objectGetName(speaker));
                presenter()->consoleMessageStyled(other->netId, kMsgChannelSystem, line);
            }
        }
        {
            ServerActorScope scope(driverSlot >= 0 ? playerActorAt(driverSlot) : nullptr);
            gameDialogEnter(speaker, 0);
        }
        serverControlEndDialogDrive();
    }

    void worldMap() override
    {
        worldmapServerDriver();
    }
};

static ServerScriptRequestHandler gServerScriptRequestHandler;

void scriptRequestHandlerInstallServer()
{
    scriptRequestHandlerSet(&gServerScriptRequestHandler);
}

} // namespace fallout
