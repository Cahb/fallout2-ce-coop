#ifndef SHEET_INTENT_H
#define SHEET_INTENT_H

#include "obj_types.h"

namespace fallout {

// ─── CHARACTER-SHEET EDIT INTENTS (PLAYER_SHEET_DESIGN.md §9) ─────────────────
//
// A player's level-up spends: one skill point into one skill, one perk pick, and
// the two follow-up choices (Tag!/Mutate!) those perks demand. The client never
// mutates its own sheet — it sends an intent, the server rules on it and streams
// the authoritative row back (EVENT_PLAYER_SHEET), so a refusal is simply a row
// that did not change.
//
// ►► WHY THIS LAYER EXISTS AT ALL, rather than the verbs calling skillAdd/perkAdd
// directly:
//   1. THE SUBJECT. skillAdd/skillSub open with `if (obj != gDude) return -5`, and
//      read the unspent-point budget with no subject at all. Every entry point
//      here takes a ServerActorScope for the acting actor, which makes the guard
//      pass AND lands the budget read/write in that actor's row. That is the whole
//      fix; the leaves keep their vanilla shape.
//   2. ENTITLEMENT. perkAdd checks prerequisites but NOT whether the player is
//      owed a pick, so a caller that forgot would let anyone take every perk they
//      qualify for. perkOwedPickGet is checked here, once.
//   3. RANGE. The skill/perk index arrives from a client. An unchecked index is an
//      out-of-bounds read into gSkillDescriptions/gPerkDescriptions — on the one
//      shared simulation, from any session. Checked here, once, for every caller
//      including the admin console.
//   4. THE UNDO BOUNDARY. Vanilla's "-" button only walks back to the value the
//      screen opened with. That baseline is a per-session fact, and on a dedicated
//      server it has to live on the server: without it, a player could sell skill
//      points they were never awarded (down-spend a starting skill, pocket the
//      refund) — an unbounded point fountain.
//
// Every entry point is safe to call from any beat: they mutate sheet state only,
// never the world, so they need no combat/turn gating.

enum SheetEditResult {
    kSheetEditOk = 0,
    // The session has no actor to edit (spectator).
    kSheetEditNoActor,
    // Skill/perk/trait index outside the valid range.
    kSheetEditBadIndex,
    // Not enough unspent skill points for the next point in that skill.
    kSheetEditNoPoints,
    // The skill is at vanilla's 300%% ceiling.
    kSheetEditAtCap,
    // Nothing owes this player a perk pick.
    kSheetEditNoOwedPick,
    // Prerequisites (level, stats, skills) are not met.
    kSheetEditPrereq,
    // "-" below the value the character screen was opened with.
    kSheetEditAtBaseline,
    // "-" with no open screen session, so there is no baseline to walk back to.
    kSheetEditNoSession,
    // A follow-up choice (Tag!/Mutate!) is owed, or one arrived unowed.
    kSheetEditChoiceOwed,
    kSheetEditNoChoiceOwed,
    // Tag! aimed at a skill the actor already has tagged.
    kSheetEditAlreadyTagged,
    // The engine leaf refused for a reason not modelled above.
    kSheetEditRefused,
};

// One player's character screen opened / closed. The open snapshots the undo
// baseline (see #4 above); the close drops it. Both are idempotent, and a session
// that never sends the close (crash, disconnect) only leaves a stale baseline that
// the next open replaces.
void sheetEditSessionOpen(Object* actor);
void sheetEditSessionClose(Object* actor);

// Open one only if none is open, PRESERVING an existing baseline. For drivers that
// have no character screen to bracket the edits (the admin console): calling the
// plain open before each verb would re-snapshot the baseline every time, which
// silently makes every "-" look like it is already at the floor.
void sheetEditSessionEnsure(Object* actor);

// Spend ONE point in one skill / walk one point back to the baseline.
int sheetEditSkillUp(Object* actor, int skill);
int sheetEditSkillDown(Object* actor, int skill);

// Take one perk, consuming the owed pick. On success `pendingChoice` reports
// whether the perk owes a follow-up — PERK_CHOICE_PENDING_TAG (a 4th tagged skill)
// or PERK_CHOICE_PENDING_MUTATE (a trait swap) — to be answered by the two calls
// below. Until it is answered, another pick is refused (kSheetEditChoiceOwed).
int sheetEditPerkPick(Object* actor, int perk, int* pendingChoice);

// Answer an owed follow-up. Both are addressed by SKILL/TRAIT ID, never by the
// dialog's line number (the perk dialog sorts alphabetically, so line numbers
// depend on the client's render order and its message language).
//
// `skill == -1`, or both traits -1, means the player CANCELLED: the perk comes back
// off — vanilla's behaviour when the follow-up dialog is escaped — and the owed pick
// is handed back, which vanilla does not do.
int sheetEditTagPick(Object* actor, int skill);
int sheetEditMutatePick(Object* actor, int dropTrait, int gainTrait);

// A player-facing sentence for a result, for the refusal channel. Never null.
const char* sheetEditReason(int result);

} // namespace fallout

#endif /* SHEET_INTENT_H */
