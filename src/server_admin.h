#ifndef FALLOUT_SERVER_ADMIN_H_
#define FALLOUT_SERVER_ADMIN_H_

#include <functional>
#include <string>

namespace fallout {

// ADMIN verbs for the dedicated server's control channel (F2_SERVER_CMD) —
// save, load, slot listing, world selection. Distinct from the debug verbs in
// command.cc for two concrete reasons, not as a matter of taste:
//
//   * They ANSWER. `saves` prints a listing to the operator, so they need the
//     reply sink CommandListener hands the line callback. commandDispatch has
//     no reply path — it pokes the sim and logs to the server's stderr.
//   * Their arguments are not integers. `Command` is {name, arg, arg2} ints, so
//     `save 2 before the boss fight` cannot ride it. Widening Command for one
//     caller would disturb the shared probe/server dispatch contract.
//
// Server-side only (f2_server, not f2_core): the client has screens for all of
// this and no control channel to serve.

// One deferred world-lifecycle request. `load`/`new` cannot act inline — they
// have to run between serve loops, not from inside a poll callback nested in
// one — so the verb records the intent and main() acts on it.
struct ServerAdminRequest {
    enum Kind {
        kNone,
        kLoadSlot, // restore SAVEGAME\SLOTnn and serve it
        kNewWorld, // boot a fresh world on `map`
        kQuit,
    };

    Kind kind = kNone;
    int slot = -1; // 0-based, as savegameSetSlot wants it
    std::string map;
};

// Handle one control-channel line. Returns true if it was an admin verb and has
// been answered through `reply`; false means "not mine" and the caller should
// fall through to the debug commandDispatch chain.
//
// `worldLoaded` tells the handler which verbs are legal right now: `save` needs
// a world, `load`/`new` are lobby-only (see the note on serverAdminTakeRequest).
bool serverAdminLine(const char* line,
    const std::function<void(const char* text)>& reply,
    bool worldLoaded);

// Collect a pending world-lifecycle request, clearing it. Returns false if none.
//
// ⚠ Only the LOBBY consumes these today. Swapping the world under a running
// serve loop would re-mint every netId beneath connected viewers and free
// objects the presenter still references, so `load` is refused while a world is
// up rather than half-supported.
bool serverAdminTakeRequest(ServerAdminRequest& out);

// Render the slot listing (the body of `saves`) to the reply sink. Exposed so
// the lobby can greet a connecting operator with it unprompted.
void serverAdminWriteSlotListing(const std::function<void(const char* text)>& reply);

// Periodic unattended save into the dedicated autosave slot (SLOT11).
// F2_AUTOSAVE_SECS sets the cadence (default 300; 0 disables); an extra save
// fires on the first safe beat after each map change. Call once per MAIN-PHASE
// beat only — never from a modal pump. Refuses (and stays latched) while the
// world is unsettled: combat, a pending transition, dialog/barter/worldmap/
// movie, or an unsaveable map.
void serverAutosaveTick();

} // namespace fallout

#endif /* FALLOUT_SERVER_ADMIN_H_ */
