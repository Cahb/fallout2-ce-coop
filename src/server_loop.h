#ifndef FALLOUT_SERVER_LOOP_H_
#define FALLOUT_SERVER_LOOP_H_

#include <functional>

namespace fallout {

struct Object;

// Fixed-timestep headless simulation loop (SERVER_LOOP_DESIGN.md). This is the
// keystone that decouples the sim from animation/render/frame timing and
// dissolves the four timing couplings that defeated the instant-animation
// retrofit into the frame-driven probe loop (see REWRITE_PLAN Phase 2 status).
//
// Activation is INTERLOCKED and done as a coherent whole (the pieces are
// mutually load-bearing — each partial step breaks on its own):
//   * sim_clock feeds tickersExecute (input.cc) when serverLoopActive() — sim
//     time = tick count * kServerTickDelta, no longer getTicks() call-count;
//   * NullPresenter installed (the deadlocking HUD end-button loops become
//     no-ops), InstantAnimationScheduler installed (animations drain per pump);
//   * gameMoviePlay is guarded headless (marks seen, skips playback);
//   * combat_ui's _combat_input/_combat_turn_run take the server branch
//     (auto-end-turn, pump w/o render/throttle).
// serverRun() owns installing/restoring the presenter + scheduler + clock and
// the active flag; the caller supplies the per-tick intent drain.

// True while serverRun() is executing. Read by the interlocked seams
// (input.cc clock feed, combat_ui.cc combat driver, game_movie.cc guard) to
// select server behavior; false everywhere else (the client is unaffected).
bool serverLoopActive();

// True for the whole duration of a headless probe run — BOTH the legacy
// F2_PROBE_ACTIONS pump and the F2_SERVER_LOOP tick loop (serverLoopActive() is
// the narrower subset, true only inside serverRun()). Modal UI drivers that
// have no headless backing for their window/render/real-time pacing — e.g. the
// pipboy rest loop, which would draw to a never-created window — gate that
// pacing on this so the real modal loop's SIM runs on either probe path while
// the UI is skipped. Set once at probe entry (main.cc); always false in the
// real client, so the client is unaffected.
bool headlessProbeActive();
void headlessProbeSetActive(bool active);

// True only inside a REAL dedicated-server run (f2_server), i.e. the server loop
// minus the headless golden probe, which drives the same loop out of
// fallout2-ce's mainHeadlessProbe.
//
// ►► Use this — NOT serverLoopActive() — for rules that exist because a server
// HOSTS OTHER PLAYERS, as opposed to rules about how the sim is driven. The
// motivating case is the player-death survival rule (MP_PROPOSAL.md Ch 9): a
// dedicated server must never terminate because a player died, but the golden
// probe is not a dedicated server — it is a single-player sim harness whose
// pinned traces legitimately END when the dude dies, exactly as vanilla does.
// Gating that rule on serverLoopActive() suppressed the vanilla endgame in the
// probe too, which silently extended four combat goldens by thousands of ticks
// of post-death AI (measured: arvillag_restfight/gunfight/aimshot +
// klatoxcv_combat, the four whose dude reaches hp=0).
bool serverDedicatedActive();

// Run one server tick (SERVER_LOOP_DESIGN.md §1 ordered steps):
//   (1) intentsDrain(tick) — apply this tick's scheduled intents by calling
//       core entry points directly (no key/mouse events);
//   (2) simClockAdvance(kServerTickDelta);
//   (3) _process_bk() — tickers (scripts/timed-events, object animation w/ the
//       instant scheduler draining to completion, dude fidget);
//   (4) scriptsHandleRequests() — combat entry / map switch requests;
//   (5) mapHandleTransition().
// Reusing _process_bk (vs an explicit call list) preserves the exact intra-pump
// order the RNG stream depends on.
//
// advanceSim=false FREEZES the simulation for this beat: intentsDrain STILL runs
// (so a persistent server with no players can still accept a connection and log a
// player in — the thing that un-freezes it), but steps (2)-(5) and the frame emit
// are skipped, so the game clock, scripts, NPCs and the object-id budget do not
// advance while nobody is watching. Used by the keepalive dedicated server when the
// last client leaves; the default (true) is every existing caller unchanged.
void serverTick(int tick, const std::function<void(int)>& intentsDrain, bool advanceSim = true);

// Bridge f2_server-only control-plane state into f2_core: the resumable-combat
// turn barrier (combat.cc, core) must know whether a wire client currently holds
// the control claim, but server_control.cc is not linked into the client/probe.
// f2_server installs the query at boot; unset (client/probe/goldens) → no
// claimant, so the barrier never waits on a connection that cannot exist.
void serverSetClaimantQuery(bool (*query)());

// Install the player-initiated combat-start latch consumer (server_control.cc's
// serverControlConsumePendingCombatStart). Null on every non-server path. Called
// once by server_main alongside serverSetClaimantQuery.
void serverSetCombatStartConsumer(int (*consumer)());
bool serverClaimantConnected();

// Which session controls the player actor in `slot`, or 0 when unbound (and
// always 0 where no control plane is installed). Installed by f2_server;
// consumed by the roster emit and, from M3, by the per-actor combat turn
// barrier (MP_PROPOSAL.md Ch 6.1/8.3).
void serverSetSlotSessionQuery(int (*query)(int slot));
int serverSessionForSlot(int slot);

// Install the in-combat interaction executor (server_control.cc's
// serverControlRunCombatInteract). Same bridge idiom as the queries above, and
// needed for the same reason: the combat pump lives in f2_core (combat_drain.cc)
// while the interaction verbs, their target re-resolution and their outcome
// bodies live in the f2_server-only control plane. Null everywhere else, where
// a COMBAT_INTENT_INTERACT cannot exist in the first place — nothing but the
// wire produces one — so the runner reporting "did nothing" is the honest answer
// rather than a silently skipped action.
void serverSetCombatInteractRunner(bool (*runner)(Object* actor, int verb, int targetNetId, int arg));
bool serverRunCombatInteract(Object* actor, int verb, int targetNetId, int arg);

// Does `slot` currently hold an open in-combat inventory screen? Installed by
// f2_server (serverControlInventorySessionOpen); false everywhere else.
//
// The turn machine needs this because vanilla's answer to "the player is in
// their pack" is to BLOCK THE ENTIRE GAME LOOP, which a server with N players
// cannot do. What it can do is refuse to end that one actor's turn while the
// screen is up: spending your last AP to open the inventory would otherwise trip
// the out-of-AP auto-end and hand your turn away while you were still standing
// in the screen, with the fight moving on behind it. The idle deadline remains
// the cap, so an abandoned screen cannot stall the fight forever.
void serverSetSlotModalQuery(bool (*query)(int slot));
bool serverSlotInModal(int slot);

// Broadcast the player-actor roster. Called at every baseline tail; the control
// plane also calls it whenever a claim is granted or released so viewers can
// re-derive which actor is theirs without waiting for a rebaseline.
void serverEmitPlayerRoster();

// Resumable-combat idle-deadline pacing (COMBAT_CLIENT_DESIGN.md §6 S6). The AI turns
// that precede the player's turn are dispatched in a burst the client still has to
// ANIMATE before the human even sees their turn, so a flat idle deadline can time the
// player out before their turn is on screen. The emit path (server_anim move stamp +
// combat attackResult) accumulates an estimate of that AI-only presentation backlog in
// sim-ms; the player barrier TAKES (reads + clears) it at turn-begin and adds it on top
// of the flat human budget. Only non-player actions accumulate (the player's own moves/
// attacks are already on screen), so a take at the player's turn-begin is exactly the
// backlog since their last turn. No-op off the resumable path (nothing accumulates, the
// value is never read) → goldens byte-identical.
void serverAddPresentationCostMs(unsigned int ms);
unsigned int serverTakePresentationCostMs();

// STEP 5 mid-stream join: ask the loop to re-walk netIds and broadcast a fresh
// blob + baseline to ALL clients at the end of the current beat (C.4 — the walk
// resets the whole-stream netId domain, so everyone must resync, not just the
// joiner). Coalesces: N joins in one beat = one rebaseline.
void serverRequestRebaseline();

// Install the interlocked server-loop state, run `ticks` server ticks, then
// restore the previous presenter/scheduler. `intentsDrain` may be empty.
void serverRun(int ticks, const std::function<void(int)>& intentsDrain);

// Open-ended "serve" loop — the dedicated-server shape (P5-A, [[p5-server-plan]]).
// Unlike serverRun's fixed tick count, this drains beats until the caller's
// `keepServing(tick)` predicate returns false: the seam a real networked server
// fills with "until a shutdown command arrives / no clients remain". v1 has no
// I/O, so the caller supplies BOTH the per-beat command drain and the stop
// condition (e.g. "the game hasn't requested quit, and a safety tick cap hasn't
// been hit"). Installs/restores the SAME interlocked state as serverRun; the
// predicate is evaluated AFTER each beat's serverTick so at least one beat runs.
// simGate (optional): asked BEFORE each beat's serverTick — return false to FREEZE
// the sim that beat (intentsDrain still runs; see serverTick's advanceSim). The
// keepalive dedicated server passes "are there any clients", so an empty server
// idles frozen instead of ticking the world with nobody watching. Null = always
// advance (every pre-keepalive caller unchanged).
void serverServe(const std::function<void(int)>& intentsDrain,
    const std::function<bool(int)>& keepServing,
    const std::function<bool()>& simGate = nullptr);

} // namespace fallout

#endif /* FALLOUT_SERVER_LOOP_H_ */
