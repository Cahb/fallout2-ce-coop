// f2_server — core-only dedicated-server entry point (P5-C, [[p5-server-plan]]).
//
// This binary links f2_core WITHOUT f2_client: it proves the simulation core
// can stand as the basis of a headless dedicated server, independent of the
// SDL presentation layer. It is a LINK-BOUNDARY MILESTONE — engine asset/SDL
// bring-up and the socket transport are not wired yet. Its job is to:
//   (1) validate, at build time, that f2_core's server codepath links with no
//       f2_client objects (every client symbol the core reaches is satisfied by
//       the abort/no-op stubs in src/server_stubs.cc), and
//   (2) host the forthcoming socket accept/framing + NetworkPresenter +
//       per-client sequenced buffer, and the binary join snapshot (via the SAVE
//       pipeline) that lands with a real connecting client.
//
// The set of stubs in server_stubs.cc IS the enumerated client-severance
// surface: replacing them with de-SDL'd implementations (timing, file I/O, the
// movement/animation path, the background pump) is the incremental road from
// this link milestone to a runnable standalone server.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "combat.h" // isInCombat — dialog pump bails when combat preempts
#include "command.h"
#include "debug.h" // TEMP DIAGNOSTIC (revert): _debug_register_env
#include "game.h"
#include "game_dialog.h" // gameDialogSetServerPump — A2 dialog block-and-pump seam
#include "inventory.h" // barterSetServerPump — the barter block-and-pump seam
#include "server_worldmap.h" // worldmapSetServerPump — worldmap block-and-pump seam
#include "object.h"
#include "pres_record.h"
#include "presenter_network.h"
#include "server_admin.h"
#include "server_anim.h"
#include "game_movie.h" // gameMovieSetServerPump — the movie barrier
#include "server_boot.h"
#include "server_control.h"
#include "server_loop.h"
#include "server_net.h"
#include "state_dump.h"

int main(int argc, char** argv)
{
    using namespace fallout;

    // F2_SERVER_MAP=<map.map>: boot the headless simulation core, load the map,
    // and run the open-ended serve loop to completion (STEP 1 "MAKE IT RUN",
    // [[p5-server-plan]]). This is the reactive de-stub driver: any f2_client
    // symbol the running core still reaches trips serverStubAbort loudly, marking
    // the next capability to sever. The serve loop runs UNLIMITED by default (a
    // real server never closes); set a POSITIVE F2_SERVER_TICKS to cap a headless
    // run that must terminate (see the F2_SERVER_TICKS block below).
    // TEMP DIAGNOSTIC (revert): f2_server never runs gameInitWithOptions, so
    // gDebugPrintProc stays null and every debugPrint (probe verbs, [dtrace])
    // is silently dropped. Honor DEBUGACTIVE here so headless probes can talk.
    _debug_register_env();

    fprintf(stderr, "f2_server: platform %s\n", serverPlatformName());

    const char* mapName = getenv("F2_SERVER_MAP");

    // F2_SERVER_LOAD=<1-10>: restore a save slot instead of booting a fresh map.
    // Together with F2_SERVER_MAP this is the UNATTENDED start — either env var
    // means "you already know what to run, skip the lobby" — so an existing
    // headless invocation keeps its exact behaviour.
    const char* loadSlotEnv = getenv("F2_SERVER_LOAD");
    int loadSlot = loadSlotEnv != nullptr ? atoi(loadSlotEnv) - 1 : -1;

    // ►► BOTH SET = RESTORE WINS, loudly. The two are alternatives, not a pair, and
    // the map used to win silently — which cost a live debugging session: every
    // launcher sets F2_SERVER_MAP (scripts/viewer_live.sh defaults it), so adding
    // F2_SERVER_LOAD booted a FRESH map and the reload looked like a save bug —
    // full HP, no inventory, no progression. An explicit slot is the stronger
    // statement of intent than a map that may just be a default, so it wins, and
    // the discarded one is named rather than dropped.
    if (mapName != nullptr && loadSlot >= 0) {
        fprintf(stderr, "f2_server: F2_SERVER_LOAD=%d and F2_SERVER_MAP='%s' both set"
                        " — restoring the save slot, IGNORING the map\n",
            loadSlot + 1, mapName);
        mapName = nullptr;
    }

    // F2_SERVER_CMD=<port>: a dedicated inbound command channel (telnet/nc) for
    // injecting commands into the running server at runtime — poke the sim and
    // watch the effect stream to the connected viewers (e.g. `aggro 5` to start
    // combat in view). Independent of the viewer wire, so it accepts control
    // clients at any time. Unset → no control channel.
    //
    // Brought up BEFORE the world: it is what an operator picks the world WITH
    // in the lobby, and holding one listener across the lobby→running transition
    // is what lets them stay connected through it.
    const char* cmdPort = getenv("F2_SERVER_CMD");
    CommandListener cmdListener;
    bool haveCmd = false;
    if (cmdPort != nullptr) {
        uint16_t port = static_cast<uint16_t>(atoi(cmdPort));
        if (cmdListener.listen(port)) {
            fprintf(stderr, "f2_server: command channel on :%u (telnet/nc, one \"verb arg arg2\" per line)\n", port);
            haveCmd = true;
        } else {
            fprintf(stderr, "f2_server: command channel disabled (listen failed)\n");
        }
    }

    bool subsystemsUp = false;
    std::string lobbyMap;

    // ---- LOBBY ----
    // Neither env var picked a world, so bring the subsystems up (the file DB
    // has to be mounted before a slot listing is readable) and serve the control
    // channel until an operator runs `load`/`new`. No world exists yet: the sim
    // is not ticking and nothing is being presented.
    if (mapName == nullptr && loadSlot < 0) {
        if (!haveCmd) {
            fprintf(stderr,
                "f2_server: nothing to run. Set F2_SERVER_MAP=<map.map> or F2_SERVER_LOAD=<1-10>\n"
                "for an unattended start, or F2_SERVER_CMD=<port> to pick a world over telnet.\n");
            return 1;
        }

        if (serverBootSubsystems(argc, argv) != 0) {
            fprintf(stderr, "f2_server: subsystem init failed\n");
            return 1;
        }
        subsystemsUp = true;

        fprintf(stderr, "f2_server: lobby — no world loaded, waiting for `load`/`new` on the command channel\n");

        // Greet each operator with the slot listing as they connect. Set only for
        // the lobby: once a world is up there is nothing to pick, and dumping ten
        // slots at every debug-console connection would be noise.
        cmdListener.setGreeting([](const CommandListener::ReplyFn& reply) {
            reply("f2_server — lobby. No world loaded.");
            serverAdminWriteSlotListing(reply);
            reply("`load <1-10>` to restore one, `new <map.map>` for a fresh world, `help` for the rest.");
        });

        ServerAdminRequest request;
        bool picked = false;
        while (!picked) {
            cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) {
                if (!serverAdminLine(line, reply, false)) {
                    // Debug verbs need a world to act on; answering is better
                    // than dispatching them into an empty sim.
                    reply("no world loaded — try `saves`, `load <n>`, `new <map.map>`, or `help`");
                }
            });

            if (serverAdminTakeRequest(request)) {
                switch (request.kind) {
                case ServerAdminRequest::kQuit:
                    fprintf(stderr, "f2_server: lobby — shutdown requested\n");
                    cmdListener.closeAll();
                    return 0;
                case ServerAdminRequest::kLoadSlot:
                    loadSlot = request.slot;
                    picked = true;
                    break;
                case ServerAdminRequest::kNewWorld:
                    lobbyMap = request.map;
                    mapName = lobbyMap.c_str();
                    picked = true;
                    break;
                default:
                    break;
                }
            }

            if (!picked) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20Hz — an idle lobby must not spin a core
            }
        }

        cmdListener.setGreeting(nullptr); // the world is chosen; stop offering the picker
    }

    if (mapName != nullptr || loadSlot >= 0) {
        // What this run is serving, for the log lines at either end.
        char worldLabel[128];
        if (mapName != nullptr) {
            snprintf(worldLabel, sizeof(worldLabel), "map '%s'", mapName);
        } else {
            snprintf(worldLabel, sizeof(worldLabel), "slot %d", loadSlot + 1);
        }

        if (!subsystemsUp && serverBootSubsystems(argc, argv) != 0) {
            fprintf(stderr, "f2_server: subsystem init failed\n");
            return 1;
        }

        // serverBootFinish(true) means "spawn the F2_SERVER_PLAYERS party if the
        // registry is empty". On a NEW world that spawns slots 1..N. On a co-op
        // LOAD it must NOT: lsgLoadGameInSlot already reconstructed the extras
        // from the save's tail appendix (playerActorAppendixLoad — bodies,
        // inventory AND each extra's own sheet, PLAYER_SHEET_DESIGN.md §5), so the
        // registry is already populated and serverBootFinish's own
        // playerActorCount() > 1 guard skips the re-spawn. A reloaded co-op world
        // therefore gives everyone their OWN body, inventory and progression back,
        // not the host's. A legacy single-player save carries no appendix, so only
        // slot 0 exists and the party spawns as on a fresh boot.
        const bool loadingSave = mapName == nullptr;
        int bootRc = loadingSave ? serverBootLoadSlot(loadSlot) : serverBootNewWorld(mapName);
        if (bootRc != 0 || serverBootFinish(true) != 0) {
            fprintf(stderr, "f2_server: boot failed for %s\n", worldLabel);
            return 1;
        }
        fprintf(stderr, "f2_server: %s loaded, ready to serve\n", worldLabel);

        // Bridge the control-plane claim state into f2_core so the resumable-combat
        // turn barrier (combat.cc) can wait for a live wire driver.
        serverSetClaimantQuery(serverControlHasClaimant);
        serverSetCombatInteractRunner(serverControlRunCombatInteract);
        serverSetSlotModalQuery(serverControlInventorySessionOpen);

        // Bridge the per-slot binding so f2_core can answer "is anyone driving
        // THIS actor" — the roster emit needs it now, and from M3 so does the
        // per-actor combat turn barrier (an unbound actor's turn must auto-end
        // instead of waiting out the idle budget).
        serverSetSlotSessionQuery(serverControlSessionForSlot);

        // Bridge the player-initiated combat-start latch (cstart verb) into f2_core so
        // the server loop's idle tick can enter combat on the claimant's request.
        serverSetCombatStartConsumer(serverControlConsumePendingCombatStart);

        // F2_SERVER_TICKS: safety cap on the number of beats. ►► DEFAULT IS NOW
        // UNLIMITED — a real dedicated server that never closes is the common case,
        // so unset (or 0, or negative) = serve until a client disconnects, a
        // terminal quit (dude death / endgame / operator `quit`), NEVER a tick cap.
        // A POSITIVE value is a safety cap for headless runs that must terminate
        // (the golden/record-purity harnesses set one explicitly; those are
        // unaffected). NOTE the beat counter is a 32-bit int: at PACE_MS=100 it
        // wraps after ~6.8 years, at full speed in ~12 days — a paced live server
        // is fine, and the nearer wall is the never-recycled object-id budget
        // ([[object-id-budget-long-session]]), not this.
        const char* ticksValue = getenv("F2_SERVER_TICKS");
        int ticks = ticksValue != nullptr ? atoi(ticksValue) : 0;
        const bool unlimitedTicks = ticks <= 0;

        // F2_SERVER_KEEPALIVE: a persistent dedicated server does NOT shut down when
        // the last client disconnects — it keeps idling (paced) and accepts
        // reconnects, so players can come and go. DEFAULT = on whenever there is a
        // command channel (F2_SERVER_CMD), because an operator console IS the mark of
        // a managed, long-lived server; a bare demo/probe/golden run (no CMD) keeps
        // the old "serve while watched, exit when the last viewer leaves" behaviour.
        // Explicit F2_SERVER_KEEPALIVE=1/0 overrides either way.
        bool keepAlive = haveCmd;
        if (const char* kaEnv = getenv("F2_SERVER_KEEPALIVE")) {
            keepAlive = !(kaEnv[0] == '0' || kaEnv[0] == '\0');
        }

        // F2_SERVER_ACTIONS="tick:verb:arg[:arg2],...": inject scripted debug commands
        // at fixed ticks WITHOUT a live socket — the headless analog of the client
        // probe's F2_PROBE_ACTIONS. Drives the record-purity harness (a deterministic
        // explode/usedoor whose record-ON vs record-OFF state dump must match).
        std::vector<std::pair<int, std::string>> scriptedActions;
        if (const char* actionsEnv = getenv("F2_SERVER_ACTIONS")) {
            std::string spec(actionsEnv);
            size_t pos = 0;
            while (pos < spec.size()) {
                size_t comma = spec.find(',', pos);
                std::string entry = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t colon = entry.find(':');
                if (colon != std::string::npos) {
                    int atTick = atoi(entry.substr(0, colon).c_str());
                    std::string cmd = entry.substr(colon + 1);
                    for (char& c : cmd) { // "verb:arg:arg2" -> "verb arg arg2" for dispatchLine
                        if (c == ':') c = ' ';
                    }
                    scriptedActions.emplace_back(atTick, cmd);
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }

        // F2_SERVER_PACE_MS=<ms>: sleep this long between beats. The core serve loop
        // is logical-time-only and never sleeps (a real client self-paces off the
        // wire timestamps); this is purely a DEBUG/DEMO knob so a live SDL viewer
        // sees motion at a watchable rate instead of the server blasting ~2000
        // beats/sec. 100 ≈ real time: one beat advances the sim clock by
        // kServerTickDelta=100ms, so 100ms wall per beat runs sim time 1:1 (33
        // runs it 3x — empirically "super fast" walking on a live viewer). Unset =
        // full speed (the default; goldens and the tick-cap sweeps are unaffected).
        const char* paceValue = getenv("F2_SERVER_PACE_MS");
        int paceMs = paceValue != nullptr ? atoi(paceValue) : 0;

        // F2_SERVER_NET=<port>: stream the binary wire over TCP (STEP 3 "MAKE IT
        // ACCEPT"). Connect-at-start: bind, block until a viewer connects, then
        // register the socket sink so serverServe's serverInstall installs the
        // NetworkPresenter over it (preferred over F2_NETSTREAM) and emits the
        // join baseline to the already-connected client. Unset → the map runs to
        // the tick cap with no transport (the STEP 1 "did it abort?" oracle).
        const char* netPort = getenv("F2_SERVER_NET");
        SocketByteSink netSink;
        bool haveNet = false;
        if (netPort != nullptr) {
            uint16_t port = static_cast<uint16_t>(atoi(netPort));
            if (!netSink.listen(port)) {
                serverShutdown();
                return 1;
            }
            fprintf(stderr, "f2_server: listening on :%u — waiting for a client...\n", port);
            if (!netSink.acceptClients(1)) {
                netSink.closeAll();
                serverShutdown();
                return 1;
            }
            // F2_SERVER_NET_TEE=<path>: also log the outbound wire to a file (the
            // netsocket gate's same-run reference).
            const char* teePath = getenv("F2_SERVER_NET_TEE");
            if (teePath != nullptr) {
                netSink.setTeeFile(teePath);
            }
            presenterSetServerSink(&netSink);
            haveNet = true;
        }

        // Line->Command dispatch for the DEBUG command channel (F2_SERVER_CMD),
        // which is unrestricted (telnet/nc admin). The viewer WIRE's inbound does
        // NOT come here: it is gated by the control plane (serverControlLine), the
        // trust boundary that restricts wire clients to claim/mv (STEP 6).
        //
        // Admin verbs (save/saves/load/new/status/help) are tried FIRST and, when
        // they match, consume the line — they answer the operator through the
        // reply sink instead of poking the sim. Anything else falls through to the
        // debug verb chain unchanged.
        auto dispatchLine = [](int tick, const char* line) {
            Command command;
            command.tick = tick;
            command.arg = 0;
            command.arg2 = -1;
            command.name[0] = '\0';
            if (sscanf(line, "%15s %d %d", command.name, &command.arg, &command.arg2) >= 1
                && command.name[0] != '\0') {
                fprintf(stderr, "f2_server: command '%s' arg=%d arg2=%d @tick %d\n",
                    command.name, command.arg, command.arg2, tick);
                commandDispatch(command);
            }
        };

        // The control channel's entry point: admin verbs get first refusal and
        // answer the operator directly; everything else is a debug verb.
        auto dispatchControlLine = [&dispatchLine](int tick, const char* line, const CommandListener::ReplyFn& reply) {
            if (serverAdminLine(line, reply, true)) {
                return;
            }
            dispatchLine(tick, line);
        };

        // A2 (dialog streaming, DIALOG_STREAMING_PLAN): install the block-and-pump
        // seam so a live dialog node parks the server tick and services the control
        // channel until the owner's dsay/dend lands (game_dialog.cc _gdProcess).
        //
        // ENV-GATED OFF by default (F2_DIALOG_STREAM). With no pump installed,
        // gDialogServerPump stays null and _gdProcess keeps its byte-identical
        // headless behavior (empty intent queue → end the conversation). The env
        // gate keeps every CI gate byte-identical AND is the live-test switch —
        // A2-ACTIVATE fills in the REAL body but leaves it flag-guarded until the
        // A3 viewer + a review pass promote it to always-on.
        //
        // NOTE (owner-directed): this activation body has NOT had the mandatory
        // adversarial review yet — it is a first cut, acceptable to land behind the
        // flag. Known follow-ups for the review pass: (a) non-dialog control verbs
        // (mv/use/…) are still serviced mid-dialog and would act on the frozen
        // world — the reviewed body should gate serverControlLine to dsay/dend while
        // a dialog is active; (b) hazard 5 — gGameDialogSpeaker can dangle across a
        // rebaseline, so we deliberately skip acceptPending() here (joins wait).
        if (getenv("F2_DIALOG_STREAM") != nullptr) {
            fprintf(stderr, "f2_server: F2_DIALOG_STREAM set — dialog block-and-pump ACTIVE (unreviewed first cut)\n");
            gameDialogSetServerPump([&]() -> bool {
                // Always-bail conditions — never wedge the tick on a vanished viewer,
                // and let combat/quit preempt an open conversation. TERMINAL quit
                // only — value 1 is the combat-break signal (gameTerminalQuitRequested),
                // and the isInCombat() bail below already covers that case.
                // ►► BAIL REASONS ARE LOGGED. A bail abandons the conversation the
                // client is ALREADY displaying (game_dialog.cc emits the node before
                // this loop), so "which condition fired" is the whole diagnosis for
                // the "buttons do nothing / dsay ignored (no active dialog)" class.
                // Unconditional: these fire at most once per conversation.
                if (gameTerminalQuitRequested()) {
                    fprintf(stderr, "f2_server: [dialog] PUMP BAIL — terminal quit requested\n");
                    return false;
                }
                if (isInCombat()) {
                    fprintf(stderr, "f2_server: [dialog] PUMP BAIL — combat started mid-dialog\n");
                    return false;
                }
                if (haveNet && netSink.clientCount() == 0) {
                    fprintf(stderr, "f2_server: [dialog] PUMP BAIL — no wire clients left\n");
                    return false;
                }

                if (haveNet) {
                    // Service the claimant's control lines so their dsay/dend lands
                    // in the dialog intent queue (serverControlLine, gated). We do
                    // NOT acceptPending() new joiners mid-dialog: a rebaseline
                    // re-mints netIds and can free gGameDialogSpeaker (hazard 5) —
                    // joins wait for the conversation to end. beginDrain first so a
                    // claimant that just dropped releases its claim (→ bail below).
                    serverControlBeginDrain([&](int sid) { return netSink.hasSession(sid); });
                    netSink.pollInbound([&](int sessionId, const char* line) {
                        serverControlLine(sessionId, line);
                    });
                }
                if (haveCmd) {
                    cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) { dispatchControlLine(0, line, reply); });
                }

                // No one left who can pick an option (owner disconnected and no debug
                // channel) → bail rather than spin the node forever.
                //
                // ►► "ANY claimant" WAS THE WRONG QUESTION and it wedged the server.
                // Only ONE player may answer a conversation, so a second connected
                // player kept this condition satisfied while nobody who could answer
                // was left — the barrier then spun forever. serverControlDialogDriver
                // Present() asks the question that actually matters, and answers true
                // when no conversation is driven, so this stays a NARROWING.
                if (!((haveNet && serverControlHasClaimant()
                          && serverControlDialogDriverPresent())
                        || haveCmd)) {
                    fprintf(stderr, "f2_server: [dialog] PUMP BAIL — nobody can answer "
                                    "(claimant gone, no cmd channel)\n");
                    return false;
                }

                // Yield so the barrier doesn't busy-spin while the owner reads the
                // node; PACE if set, else a small fixed nap.
                std::this_thread::sleep_for(std::chrono::milliseconds(paceMs > 0 ? paceMs : 10));
                return true;
            });

            // BARTER runs INSIDE a conversation but OUTSIDE the dialog barrier:
            // picking a trade option leaves _gdProcess and calls inventoryOpenTrade,
            // which blocks in its own loop. So the trade needs its own pump, gated
            // by the same env as the conversation that can reach it.
            //
            // Bail conditions are the dialog barrier's, for the same reasons, minus
            // the combat one — combat cannot start while the world is frozen in a
            // trade, and there is no barter analog of "a script started a fight".
            barterSetServerPump([&]() -> bool {
                if (gameTerminalQuitRequested()) {
                    fprintf(stderr, "f2_server: [barter] PUMP BAIL — terminal quit requested\n");
                    return false;
                }
                if (haveNet && netSink.clientCount() == 0) {
                    fprintf(stderr, "f2_server: [barter] PUMP BAIL — no wire clients left\n");
                    return false;
                }

                if (haveNet) {
                    serverControlBeginDrain([&](int sid) { return netSink.hasSession(sid); });
                    netSink.pollInbound([&](int sessionId, const char* line) {
                        serverControlLine(sessionId, line);
                    });
                }
                if (haveCmd) {
                    cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) { dispatchControlLine(0, line, reply); });
                }

                // The trade's DRIVER is the conversation's driver, and only they can
                // move an item or press Offer. "Anyone is connected" would wedge the
                // server the moment that one player dropped — the softlock the dialog
                // barrier already had.
                if (!((haveNet && serverControlHasClaimant()
                          && serverControlDialogDriverPresent())
                        || haveCmd)) {
                    fprintf(stderr, "f2_server: [barter] PUMP BAIL — nobody can trade "
                                    "(driver gone, no cmd channel)\n");
                    return false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(paceMs > 0 ? paceMs : 10));
                return true;
            });
        }

        // MOVIE BARRIER (game_movie.h). Unlike the two pumps around it this needs no
        // env gate, because its first bail condition makes it inert everywhere a gate
        // runs: with no WIRE CLIENT attached there is nobody to watch a movie and
        // nobody to ack one, so the barrier returns immediately and gameMoviePlay
        // keeps its old mark-seen-and-continue behaviour byte for byte. That covers
        // the headless goldens AND the cmd-port-only harnesses — which an
        // `if (haveNet && count == 0)` test would NOT have covered, and which would
        // then have hung forever on the first script-triggered movie.
        gameMovieSetServerPump([&]() -> bool {
            if (gameTerminalQuitRequested()) {
                return false;
            }
            if (!haveNet || netSink.clientCount() == 0) {
                return false;
            }

            // Service inbound so `movdone` can land. No acceptPending(): a rebaseline
            // mid-movie would re-mint netIds under a viewer that is not looking at the
            // world anyway — joiners wait for the cutscene, same ruling as dialog.
            serverControlBeginDrain([&](int sid) { return netSink.hasSession(sid); });
            netSink.pollInbound([&](int sessionId, const char* line) {
                serverControlLine(sessionId, line);
            });
            if (haveCmd) {
                cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) { dispatchControlLine(0, line, reply); });
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(paceMs > 0 ? paceMs : 10));
            return true;
        });

        if (getenv("F2_WORLDMAP_STREAM") != nullptr) {
            fprintf(stderr, "f2_server: F2_WORLDMAP_STREAM set — worldmap block-and-pump ACTIVE\n");
            worldmapSetServerPump([&]() -> bool {
                if (gameTerminalQuitRequested()) {
                    fprintf(stderr, "f2_server: wm-pump bail: terminal quit\n");
                    return false;
                }
                if (isInCombat()) {
                    fprintf(stderr, "f2_server: wm-pump bail: in combat\n");
                    return false;
                }
                if (haveNet && netSink.clientCount() == 0) {
                    fprintf(stderr, "f2_server: wm-pump bail: no clients (haveNet=%d count=%d)\n", haveNet, netSink.clientCount());
                    return false;
                }
                if (haveNet) {
                    serverControlBeginDrain([&](int sid) { return netSink.hasSession(sid); });
                    netSink.pollInbound([&](int sessionId, const char* line) {
                        serverControlLine(sessionId, line);
                    });
                }
                if (haveCmd) {
                    cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) { dispatchControlLine(0, line, reply); });
                }
                if (!((haveNet && serverControlHasClaimant()) || haveCmd)) {
                    fprintf(stderr, "f2_server: wm-pump bail: no claimant (haveNet=%d hasClaimant=%d haveCmd=%d)\n",
                        haveNet, serverControlHasClaimant() ? 1 : 0, haveCmd ? 1 : 0);
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(paceMs > 0 ? paceMs : 10));
                return true;
            });
        }

        serverServe(
            // intentsDrain: each beat, pull whatever command lines have arrived
            // from clients and dispatch them through the shared handler (P5-A
            // FIFO choke point). Non-blocking — a beat with no pending input
            // just resolves the sim. No-op without networking.
            [&](int tick) {
                // Snapshot the presentation recorder's created-this-beat watermark
                // BEFORE any command/action this beat creates a transient (explosion
                // cloud, synthetic attacker) — resolveRef uses it to ship those as
                // OBJ_CREATE. Cheap, unconditional (no-op unless recording).
                presRecordBeatBegin();
                // v1 inbound = "verb [arg] [arg2]" text; a typed binary frame
                // replaces it when the protocol is frozen (REWRITE_PLAN 3.4). Trust
                // boundary is v1-permissive (dev server): commandDispatch is the
                // single validation point a hardened reader will gate.
                if (haveNet) {
                    // Mid-stream joiners (STEP 5): accept, preamble them, and owe
                    // them a rebaseline broadcast at this beat's tail. Before the
                    // inbound drain so a joiner's very first beat is complete.
                    if (netSink.acceptPending() > 0) {
                        serverRequestRebaseline();
                    }
                    // STEP 6: wire-client lines are the CONTROL plane (claim/mv),
                    // gated per-session — not the unrestricted debug vocabulary.
                    // beginDrain first: release a claim whose owner disconnected
                    // (checked against the sink's live sessions) and reset the
                    // per-beat flood counters, before this beat's lines land.
                    serverControlBeginDrain([&](int sid) { return netSink.hasSession(sid); });
                    netSink.pollInbound([&](int sessionId, const char* line) {
                        serverControlLine(sessionId, line);
                    });
                }
                if (haveCmd) {
                    cmdListener.pollCommands([&](const char* line, const CommandListener::ReplyFn& reply) { dispatchControlLine(tick, line, reply); });
                }
                // Spawn actors for `login`s by names the server has never seen, and
                // deliver greetings owed from an earlier beat
                // (ACCOUNT_IDENTITY_DESIGN.md §3). ►► THIS CALL SITE IS THE BARRIER
                // GUARD: the dialog/movie/barter pumps service inbound lines too, but
                // deliberately do NOT call this — a spawn requests a rebaseline, which
                // re-mints every netId, and doing that under a barrier holding raw
                // object pointers is exactly the hazard those pumps skip
                // acceptPending() for. Placed after the inbound drain so a login that
                // arrived this beat is served this beat.
                serverControlDrainPendingLogins();
                // Body presence follows bindings (despawn on leave / reattach on
                // return) — same main-phase-only discipline as the login drain.
                serverControlDrainPresence();
                serverAutosaveTick();
                // Scripted actions (F2_SERVER_ACTIONS): fire any due this beat, through
                // the same command dispatch as the live debug channel.
                for (const auto& action : scriptedActions) {
                    if (action.first == tick) {
                        dispatchLine(tick, action.second.c_str());
                    }
                }
                // Advance the env-gated stepped-walk engine (F2_SERVER_SMOOTH_WALK,
                // server_anim.cc) one tile per walk per beat, AFTER the command
                // drain so a just-dispatched `walkto` takes its first step this
                // beat and its objectMoved lands in this beat's frame. No-op with
                // no walk in flight (always, unless the env gate is set).
                serverAnimAdvanceWalks();
                // STEP 6+: fire a claimant's walk-then-act interaction the beat its
                // approach arrives (INTERACTION_UX_DESIGN.md §2.3). AFTER the walk
                // advance so arrival + outcome share this beat, and the outcome's
                // events land wire-ordered after the final MOVE hop. No-op unless an
                // interaction is pending.
                if (haveNet) {
                    serverControlAdvancePending();
                }
            },
            [&](int tick) {
                // `quit`/`shutdown` on the command channel: the admin verb queues a
                // kQuit request; the LOBBY consumes it, but the serve loop must too —
                // otherwise a keepalive server (which no longer stops on the last
                // client leaving) has NO way to be shut down cleanly. load/new are
                // refused while a world runs, so kQuit is the only request reachable
                // here. Checked first so it wins even on a frozen/empty beat.
                ServerAdminRequest adminReq;
                if (serverAdminTakeRequest(adminReq) && adminReq.kind == ServerAdminRequest::kQuit) {
                    fprintf(stderr, "f2_server: shutdown requested on the command channel\n");
                    return false;
                }
                const bool empty = haveNet && netSink.clientCount() == 0;
                // DEBUG/DEMO pacing (see F2_SERVER_PACE_MS) — throttle the beat rate
                // so a live viewer can keep up and motion is watchable. When a
                // keepalive server sits EMPTY (frozen sim, see simGate below) always
                // nap even with no PACE set, so idling-for-reconnects doesn't peg a
                // core spinning the accept poll.
                if (paceMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(paceMs));
                } else if (empty && keepAlive) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                // TERMINAL quit only (dude died / endgame / load). Testing `!= 0`
                // here shut the whole server down whenever a script called
                // op_terminate_combat — that sets _game_user_wants_to_quit = 1, the
                // engine's in-band combat-break signal, which combatTeardown clears.
                // ACTemVil (Temple of Trials challenger) does exactly that at half
                // HP, so winning the scripted fistfight killed the server.
                if (gameTerminalQuitRequested()) {
                    return false;
                }
                // Last viewer left: a DEMO/probe server (no keepalive) has nothing
                // left to serve, so it stops. A KEEPALIVE dedicated server does NOT —
                // it keeps idling (frozen, via simGate) and accepting reconnects, so
                // players can come and go. Stop it with `quit` on the command channel.
                if (empty && !keepAlive) {
                    return false;
                }
                return unlimitedTicks || tick + 1 < ticks;
            },
            // simGate: FREEZE the sim only when a keepalive server sits with no
            // clients — "no player == freeze time". A CMD-only server (no wire)
            // and every non-keepalive run keep advancing exactly as before.
            [&]() -> bool {
                return !(keepAlive && haveNet && netSink.clientCount() == 0);
            });

        // Clear the dialog pump before the netSink (which a real pump body will
        // capture by reference) is destroyed at scope exit — no dangling capture
        // survives in the core dialog machinery. Harmless for today's captureless
        // placeholder; correct for the reviewed body that follows.
        gameDialogSetServerPump(nullptr);
        worldmapSetServerPump(nullptr);

        // Clear the registration before the sink is destroyed at scope exit, so
        // no dangling pointer survives in the core presenter machinery.
        if (haveNet) {
            presenterSetServerSink(nullptr);
            netSink.closeAll();
        }

        // F2_SERVER_LEAKPROBE=1: emit the live object count split by NO_SAVE. NO_SAVE
        // transients (explosion clouds, projectiles) are SKIPPED by the state dump, so
        // a record-mode transient leak is invisible to the differential dump gate; the
        // record-purity harness diffs this count off-vs-on to cover that blind spot.
        if (getenv("F2_SERVER_LEAKPROBE") != nullptr) {
            int total = 0, noSave = 0;
            for (Object* o = objectFindFirst(); o != nullptr; o = objectFindNext()) {
                total++;
                if ((o->flags & OBJECT_NO_SAVE) != 0) noSave++;
            }
            fprintf(stderr, "f2_server: [leakprobe] total=%d no_save=%d\n", total, noSave);
        }

        // F2_SERVER_DUMP=<path>: write the sim state dump after the run (same format
        // stateDumpWrite produces for the client probe). The record-purity harness
        // diffs two such dumps (F2_SERVER_PRES_RECORD off vs on) — they must be
        // byte-identical, proving record mode mutates zero simulation state.
        if (const char* dumpPath = getenv("F2_SERVER_DUMP")) {
            stateDumpWrite(dumpPath);
            fprintf(stderr, "f2_server: state dumped to '%s'\n", dumpPath);
        }

        serverShutdown();
        if (unlimitedTicks) {
            fprintf(stderr, "f2_server: served %s to completion (unlimited ticks).\n", worldLabel);
        } else {
            fprintf(stderr, "f2_server: served %s to completion (%d-tick cap).\n", worldLabel, ticks);
        }
        return 0;
    }

    fprintf(stderr,
        "f2_server: no F2_SERVER_MAP set — link-boundary milestone only.\n"
        "This binary links f2_core WITHOUT f2_client. Set F2_SERVER_MAP=<map.map>\n"
        "to boot+serve headlessly; see src/server_stubs.cc for the client symbol\n"
        "severance surface still being de-SDL'd.\n");

    // Reference the real inbound (command dispatch) and loop (serve/run/tick)
    // seams so the linker pulls the genuine server codepath into this binary's
    // closure — that is what makes the undefined-symbol set the TRUE severance
    // surface rather than an empty one.
    volatile std::uintptr_t sink = 0;
    sink ^= reinterpret_cast<std::uintptr_t>(&serverServe);
    sink ^= reinterpret_cast<std::uintptr_t>(&serverRun);
    sink ^= reinterpret_cast<std::uintptr_t>(&serverTick);
    sink ^= reinterpret_cast<std::uintptr_t>(&commandDispatch);
    (void)sink;

    return 0;
}
