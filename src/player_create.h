#ifndef FALLOUT_PLAYER_CREATE_H_
#define FALLOUT_PLAYER_CREATE_H_

#include "skill_defs.h" // NUM_TAGGED_SKILLS
#include "trait_defs.h" // TRAITS_MAX_SELECTED_COUNT

namespace fallout {

struct Object;

// SERVER-SIDE CHARACTER CREATION (ACCOUNT_IDENTITY_DESIGN.md stage 2).
//
// The spec a client supplies to say "this is who I am" instead of joining as a
// copy of the host. Deliberately DATA, not a screen: the creation UI (whenever it
// exists) only has to fill this in, and the headless/scripted path fills the same
// struct — so both go through one applier and one validation point.
//
// Owner ruling: creation is an EXPLICIT CLIENT INTENT and never a default. A
// login with no spec keeps today's behaviour (a clone of the host), which is what
// keeps the premade/golden path unchanged.
struct PlayerCreateSpec {
    // Base SPECIAL, in STAT_STRENGTH..STAT_LUCK order.
    int special[7];
    // Tagged skills (SKILL_*). Vanilla grants 3 at creation; the 4th tag comes
    // from the Tag! perk later, so unused entries are -1.
    int tagged[NUM_TAGGED_SKILLS];
    // Optional traits (TRAIT_*), -1 for none.
    int traits[TRAITS_MAX_SELECTED_COUNT];
};

// Fill `spec` with the vanilla starting defaults (all SPECIAL 5, no tags, no
// traits) so a partial client spec is still coherent.
void playerCreateSpecDefaults(PlayerCreateSpec* spec);

// Reject out-of-range SPECIAL / unknown skill or trait ids / duplicate tags or
// traits. Returns true when the spec is safe to apply.
//
// ⚠ This is the ONE validation point for wire-supplied character data. The values
// land in a proto row that the combat and skill machinery reads every beat, so an
// out-of-range stat is not a cosmetic problem.
bool playerCreateSpecValidate(const PlayerCreateSpec* spec);

// Apply `spec` to the player actor in `slot`, which must already exist and carry
// that slot's sheet pid.
//
// Resets the row FIRST (SPECIAL to defaults, invested skill points to zero, perks
// cleared, XP/level/karma to a new character's) — the spawn path seeds a slot from
// the HOST so the non-sheet parts of the row (fid, messageId, flags, AI packet)
// are right, and without the reset a created character would silently inherit the
// host's perks and skill investment.
//
// Finishes the way _proto_dude_init does: critterUpdateDerivedStats then heal to
// full, so HP/AP/carry weight match the new SPECIAL instead of the seeded one.
int playerCreateApply(int slot, const PlayerCreateSpec* spec);

} // namespace fallout

#endif /* FALLOUT_PLAYER_CREATE_H_ */
