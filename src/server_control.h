#ifndef FALLOUT_SERVER_CONTROL_H_
#define FALLOUT_SERVER_CONTROL_H_

#include <functional>

namespace fallout {

struct Object;

// f2_server control plane — the FIRST CONTROLLABLE CLIENT (P5-C STEP 6,
// [[p5-server-plan]]). Wire clients (the SDL viewer) send newline text intents
// upstream on the MAIN socket; the server executes them authoritatively over the
// live simulation. Unlike the debug CommandListener (F2_SERVER_CMD, unrestricted),
// the viewer wire is RESTRICTED to this small verb set and gated by a per-client
// claim: exactly one connection may drive the authoritative actor at a time.
//
// This is deliberately NOT commandDispatch (command.cc): that dispatcher is shared
// with the F2_PROBE_ACTIONS golden harness and speaks the full unrestricted debug
// vocabulary. Threading session identity + a trust boundary into it would couple a
// golden fixture to the network trust model. The control plane is its own gate.
//
// v1 wire framing is plain text "verb [arg] [arg2]\n" (the same v0 the debug port
// speaks); a typed binary command frame + per-client acks are the banked upgrade
// once the protocol is frozen (MP_PROTOCOL.md C2S, REWRITE_PLAN 3.4).
//
// Verbs:
//   claim              grant control to the sending session (if unclaimed)
//   mv <tile> <run>    move the authoritative actor to an absolute tile; run!=0
//                      runs. Honored only from the current claimant, out of combat.
//   cattack [target]   (in combat) queue an attack intent; target = Object id, or
//                      absent/-1 = nearest hostile. Claimant-only.
//   cmove <tile>       (in combat) queue a combat move-to-tile intent. Claimant-only.
//   cendturn           (in combat) queue an end-turn intent. Claimant-only.
//   cendcombat         (in combat) queue an attempt-to-end-combat intent (vanilla
//                      RETURN; refuses if hostiles remain). Claimant-only.
//   cstart             (OUT of combat) request entering combat — the wire equivalent
//                      of vanilla's 'A' toggle (_combat(nullptr)). Claimant-only; the
//                      server loop honors it on its idle tick (serverControlConsume-
//                      PendingCombatStart).
//   use/usedoor <net>  use a scenery object (door/lever/ladder/stairs) at wire
//                      netId — walk adjacent, then run the real engine action
//                      (INTERACTION_UX_DESIGN.md). usedoor is the slice-1 alias
//                      kept for door-only clients. Claimant-only.
//   get <net>          pick up a ground item at netId (walk-then-get).
//   look <net>         examine any object at netId — streams its description as
//                      console text. No approach, no AP, legal in combat.
//   push <net>         shove a pushable critter at netId. No approach.
//   rot                rotate the actor one step clockwise. No target.
//   skill <net> <sk>   use skilldex skill <sk> on netId (walk-then-skill). <sk> is
//                      allow-listed to the eight skilldex skills. In combat vanilla
//                      refuses seven of them with proto msg 902 and toggles Sneak.
//   invopen/invclose   (IN combat) request / release the inventory screen. The open
//                      is the priced act — 4 AP, 2 with Quick Pockets, charged once
//                      and nothing for the actions inside. Granted by
//                      EVENT_INVENTORY_GRANT addressed to the asking actor. Out of
//                      combat the viewer opens locally and never sends these.
//
// STAGE 4 — IN-COMBAT INTERACTION. The acting verbs above are no longer dropped in
// combat. There they are QUEUED as COMBAT_INTENT_INTERACT stamped with the issuer's
// registry slot and executed by the combat pump on that actor's OWN turn, charging
// the vanilla AP (3 for use/loot, 2 for use-item-on, 4 to open the inventory —
// see combat_ap.h for the single switch that disables all of it). The approach walk
// is AP-limited and resolves within the turn rather than spanning beats, so in
// combat there is no walk-then-act latch. TALK stays dropped in combat: mid-fight
// conversation has no vanilla behavior to be faithful to, and dialog is host-only.
//   talk <net>         (OUT of combat) start dialog with a critter at netId (walk-
//                      then-talk). Verb exists; the viewer menu does not wire it until
//                      dialog-options streaming (INTERACTION_UX_DESIGN.md §5).
//   cancel             (OUT of combat) abort the pending walk-then-act intent and
//                      stop the in-flight approach where it stands. Claimant-only.
//
// The combat verbs push dude combat intents (combat_intent.h); the resumable-combat
// player barrier drains them on the dude's turn. Out of combat they are dropped
// (the click path uses mv); one queued during an AI turn waits for the dude's turn.

// Begin an inbound drain: release the claim if its owner has disconnected
// (queried via `liveSession`) and reset the per-beat per-session flood counters.
// MUST run each beat before serverControlLine, so a client that dropped last beat
// frees the claim for a new claimant this beat, and the disconnect check happens
// in the drain (not inside serverControlLine) — pollInbound's onLine must not
// touch the client set.
void serverControlBeginDrain(const std::function<bool(int sessionId)>& liveSession);

// Dispatch one inbound line from wire client `sessionId`. The ONLY entry point for
// viewer-wire lines (server_main routes netSink.pollInbound here). Unknown or
// disallowed verbs are ignored with a one-line debug log. A cheap flood guard
// drops lines past a per-session-per-beat cap.
void serverControlLine(int sessionId, const char* line);

// Spawn the player actors queued by `login` for names the server has never seen
// (ACCOUNT_IDENTITY_DESIGN.md §3), and deliver greetings owed from an earlier
// beat.
//
// ►► MUST be called ONLY from the serve loop's MAIN-phase intent drain — never
// from the dialog / movie / barter pumps, even though those also service inbound
// lines. Spawning requests a rebaseline, which re-mints every netId; doing that
// under a barrier holding raw pointers (gGameDialogSpeaker) is the hazard the
// latch exists to avoid. Defers itself while combat is running. No-op when
// nothing is queued.
void serverControlDrainPendingLogins();

// Reconcile body presence with bindings, same call-site discipline as the login
// drain (main phase ONLY): parks a queued leaver's body off-map, reattaches a
// returner's. Defers itself during combat / a latched map transition.
void serverControlDrainPresence();

// Advance the pending out-of-combat interaction (INTERACTION_UX_DESIGN.md §2.3):
// if the claimant's walk-then-act intent has reached its target, fire the real
// engine outcome; if the approach finished/failed short, drop it with a "cannot
// get there" console message. MUST be called once per beat by the f2_server drive
// loop AFTER serverAnimAdvanceWalks(), so an approach that completes this beat fires
// its action this same beat (outcome events wire-ordered after the final MOVE hop).
// No-op when no interaction is pending.
void serverControlAdvancePending();

// True while a wire client holds the control claim. Bridged into f2_core via
// serverSetClaimantQuery so the resumable-combat turn barrier can wait for a
// live driver without linking this f2_server-only TU into the client.
// True iff ANY session holds an actor binding. Kept under its original name
// because it is installed into f2_core as the coarse "someone is driving"
// query; M3 refines the combat barrier to the per-slot form below.
bool serverControlHasClaimant();

// -- Per-session actor binding (MP_PROPOSAL.md Ch 6.1) ------------------------
// Identity chain: sessionId -> registry SLOT -> actor Object*. The slot is the
// durable link (netIds are re-minted every rebaseline; sessionIds die with their
// socket), which is why bindings are stored per slot and the roster event
// announces slot -> netId after every baseline.

// Registry slot this session drives, or -1 (a spectator).
int serverControlSlotForSession(int sessionId);

// The actor this session drives, or nullptr. THE lookup the verb layer uses.
Object* serverControlActorForSession(int sessionId);

// Session bound to this slot, or 0 (kNoSessionId) if unbound.
int serverControlSessionForSlot(int slot);

// True iff any slot is bound.
bool serverControlAnyBound();

// This server binary's own OS ("Linux"/"Windows"/"macOS"), compile-time.
const char* serverPlatformName();

// Drop a session's pending walk-then-act latch (its binding was released).
void serverControlDropPendingFor(int sessionId);

// -- Dialog DRIVE ownership (owner ruling 2026-07-21) -------------------------
// THE ACTOR WHO STARTS A CONVERSATION DRIVES IT. This reverses the dialog half of
// the host-only-screens ruling (@c3243a1); the WORLDMAP stays host-only, because
// travel relocates everyone whereas a conversation only commits its own outcome.
//
// It also repairs an asymmetry rather than inventing one: `talk` never had a host
// gate while `dsay`/`dend` did, so an extra could open a conversation and then not
// answer it — the rule was being enforced at the wrong end.
//
// Identity is the registry SLOT, not the session or the netId, for the usual
// reason: a conversation can outlive neither, but the slot is durable.

// Latch the slot whose TALK asked for the conversation that is about to open.
// Consumed by the dialog entry point in the same tick (scriptsHandleRequests
// drains the request the tick it is made), so this is a handoff, not state.
void serverControlSetPendingDialogRequester(int slot);

// Take the latched requester and make it the live conversation's DRIVER, or -1
// when nobody asked — an NPC-initiated conversation has no initiator, so it stays
// host-driven exactly as it is today. Returns the driver slot.
int serverControlBeginDialogDrive();

// The conversation is over; nobody drives.
void serverControlEndDialogDrive();

// True iff a conversation is live AND its driver's session is still bound.
//
// ►► THIS IS THE BARRIER'S ANTI-WEDGE PREDICATE, and it fixes a softlock that
// PREDATES the ruling above. The dialog pump bailed only when NOBODY was bound,
// while drive was host-only — so a host who dropped mid-conversation left the
// server spinning in the barrier forever: a claimant still existed, so no bail,
// but only the host could answer, so no answer. "Someone is connected" was never
// the right question; "the one player who can answer is still here" is.
//
// Answers TRUE when no conversation is live, so the caller's other conditions
// decide — this narrows the bail, it does not widen it.
bool serverControlDialogDriverPresent();

// -- In-combat interaction (Stage 4) ------------------------------------------
// Execute one dequeued COMBAT_INTENT_INTERACT for `actor`, on `actor`'s own turn.
// Called BACK from the combat pump (combat_drain.cc combatServerPumpIntents),
// which holds the ServerActorScope that makes `actor` the acting dude — hence a
// callback rather than the pump reaching into the interaction internals: the verb
// codes, the target re-resolution and the per-verb outcome bodies all live here,
// beside the out-of-combat path that shares them, and `verb`/`arg` stay opaque
// ints on the way through the queue.
//
// In combat the approach is AP-LIMITED and resolves within this call (vanilla
// passes the actor's remaining AP to animationRegisterMoveToObject —
// actions.cc:1488), so unlike the out-of-combat path there is no latch spanning
// beats: the walk either reaches the target and the outcome fires, or it stops
// short and the intent is spent. Returns true if the outcome fired.
bool serverControlRunCombatInteract(Object* actor, int verb, int targetNetId, int arg);

// True while `slot` holds an OPEN in-combat inventory screen — it paid the entry
// fee (item.cc inventoryApCostApply) on its own turn and has not closed it yet.
// The inventory verbs consult this instead of rejecting outright in combat: the
// fee buys the session, and everything done inside it is free.
bool serverControlInventorySessionOpen(int slot);

// Forget every open inventory session. Called when combat ends: out of combat
// the screen is free and ungated, so a session carried across the boundary would
// be a stale entry that could only ever be wrong on the NEXT fight.
void serverControlClearInventorySessions();

// Consume a pending player-initiated combat-start request (cstart), returning the
// requesting player's registry SLOT exactly once per request (-1 = nothing pending)
// and clearing it. The server loop polls this on its idle tick and opens the fight
// with that actor as the initiator — the wire equivalent of the local 'A' toggle.
// Kept out of the combat-intent queue on purpose: that queue is drained only inside
// a player's turn, which does not exist yet before combat.
int serverControlConsumePendingCombatStart();

} // namespace fallout

#endif /* FALLOUT_SERVER_CONTROL_H_ */
