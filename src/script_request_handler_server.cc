#include "script_request_handler_server.h"

#include <cstdio>
#include "critter.h"
#include "elevator.h" // elevatorResolveStartLevel — where the rider's gauge starts
#include "inventory.h" // stealSessionRun — the server-owned steal screen
#include "scripts.h" // scriptsRequestedElevatorRider — WHO pressed the button
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
// Every other request is intentionally left as the base no-op: looting and the
// endgame are dropped on the server exactly as the previous null handler dropped
// them (client-side modal presentation, not yet server-authoritative). Looting
// stays that way on purpose — it is viewer-local, driven by take/put verbs, and
// the sim keeps running behind it. STEALING cannot be, so it is answered here.
//
// ►► A DROPPED REQUEST IS SILENT, and that is the trap this seam sets. The base
// class answers "nothing happened" — elevatorSelect returned -1, which the drain
// reads as "the player cancelled the picker" — so for a long time every elevator in
// the game simply did nothing on a dedicated server: no error, no log, no crash, and
// no way to enter Sierra, Navarro, the Military Base, Vault City's vault, Vault 15,
// the Shi Temple or the Wanamingo Mine. When a vanilla feature "just doesn't
// happen" in co-op, look here first (docs/COOP_COVERAGE.md).
class ServerScriptRequestHandler : public ScriptRequestHandler {
public:
    void dialogDriveBegin() override
    {
        // TALK dialogs already hold a drive (dialogEnter above) when the script's
        // start_gdialog fires; only a script-initiated conversation needs one here.
        if (serverControlDialogDriveActive()) {
            return;
        }
        serverControlBeginDialogDrive();
        _scriptedDrive = true;
    }

    void dialogDriveEnd() override
    {
        if (_scriptedDrive) {
            _scriptedDrive = false;
            serverControlEndDialogDrive();
        }
    }

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

    // STEAL / PICKPOCKET / PLANT. The one request in this class that the server
    // must answer itself rather than delegate to a screen: every transfer is a
    // Steal roll against the victim, and a client cannot be trusted to roll its
    // own dice (nor could it — the skill, the perk and the difficulty ramp all
    // live here). So the session runs on the server and the viewers render it.
    // stealSessionRun blocks in its own block-and-pump barrier, exactly like a
    // trade; see its definition in inventory_ui.cc for why the world stops.
    void stealing(Object* thief, Object* target) override
    {
        stealSessionRun(thief, target);
    }

    // ELEVATORS. The server owns no screen, so it does not answer this request at
    // all: it asks the rider's client to show vanilla's panel and returns -1, the
    // same "cancelled" the base class returned. The ride happens LATER, when the
    // player's `elev <level>` verb arrives (server_control.cc), which resolves the
    // level against the server's own table and calls elevatorRideApply.
    //
    // ►► WHY NOT PARK THE SIM AND WAIT, like dialog and the encounter prompt do? A
    // block-and-pump driver is the heavier shape, and an elevator does not need it:
    // nothing in the script depends on the answer (vanilla's own cancel path returns
    // -1 and continues), so freezing every other player while one of them reads a
    // floor panel would buy nothing but a wedge risk. Answer immediately, ride on the
    // verb.
    int elevatorSelect(int elevator, int* map, int* elevation, int* tile) override
    {
        Object* rider = scriptsRequestedElevatorRider();
        int slot = rider != nullptr ? playerActorSlotOf(rider) : -1;
        if (slot < 0) {
            // Not a player (a script drove it) — nothing to ask, nothing to ride.
            return -1;
        }

        // The gauge's starting position is the rider's CURRENT floor, computed from
        // the map/elevation the drain handed us (including the Sierra / Military Base
        // remap math), so the panel opens showing where they are.
        int startLevel = elevatorResolveStartLevel(elevator, *map, *elevation);

        // Remember what was offered, to whom. `elev` is refused unless it answers a
        // prompt this actor was actually given — otherwise the verb would be a
        // teleport primitive addressable from any session.
        serverControlSetPendingElevator(slot, elevator);

        presenter()->elevatorPrompt(rider->netId, elevator, startLevel);
        fprintf(stderr, "f2_server: elevator %d offered to slot %d (start level %d)\n",
            elevator, slot, startLevel);

        return -1;
    }

private:
    bool _scriptedDrive = false; // drive begun by dialogDriveBegin, ended by dialogDriveEnd
};

static ServerScriptRequestHandler gServerScriptRequestHandler;

void scriptRequestHandlerInstallServer()
{
    scriptRequestHandlerSet(&gServerScriptRequestHandler);
}

} // namespace fallout
