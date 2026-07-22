#ifndef FALLOUT_COMMAND_H_
#define FALLOUT_COMMAND_H_

namespace fallout {

// Unified inbound command for the headless/server driver (REWRITE_PLAN 3.4;
// [[p5-server-plan]] P5-A). A Command is one verb the sim should apply — the
// single validation/dispatch choke point that later becomes the network
// command handler verbatim. Today the only producer is the probe's
// tick-indexed F2_PROBE_ACTIONS queue (main.cc); commandDispatch() is the
// former applyProbeActions strcmp chain, moved out unchanged.
//
// Data only (no combat/animation/UI dependency in the struct itself), so the
// type mirrors combat_intent.h / dialog_intent.h and lives in f2_core.
struct Command {
    // Scheduled sim tick this command fires on (the probe's tick-indexed queue
    // v0). Read by verbs that record a dump checkpoint / log the tick. A real
    // network reader will ignore this (commands arrive live).
    int tick;

    // Verb name. v0 wire vocabulary is a string (matches ProbeAction.name so
    // the dispatch stays byte-identical); a future typed `kind` enum replaces
    // it once the protocol is frozen (REWRITE_PLAN 3.4).
    char name[16];

    // Primary argument (skill id, proto pid, tile, amount, packed world coords,
    // rest minutes, ...). Interpretation is per-verb.
    int arg;

    // Optional secondary argument (barter/give item quantity); -1 = unspecified
    // (a full stack). Mirrors ProbeAction.arg2.
    int arg2;
};

// Apply one command by calling core entry points directly (no key/mouse
// events). Shared by the legacy pump loop and the server loop, where it IS the
// intent-drain step (SERVER_LOOP_DESIGN.md §4). Verbs that touch client-only
// subsystems (rest via pipboy, walk via the animation registrar) resolve at
// exe-link time — f2_core and f2_client are both OBJECT libraries in one exe;
// severing that (a core-only f2_server) is a later P5-C step.
void commandDispatch(const Command& command);

// Probe combat fixture: make the `aggroCount` critters nearest the dude hostile
// and request combat (turn order / AI / to-hit / damage / death without any
// UI-coordinate scripting). Invoked by the `aggro` command and by the probe's
// startup F2_PROBE_AGGRO env var. Lives here so the `aggro` verb body stays
// self-contained in commandDispatch.
void probeApplyAggro(int aggroCount);

} // namespace fallout

#endif /* FALLOUT_COMMAND_H_ */
