#ifndef FALLOUT_PLAYER_SHEET_H_
#define FALLOUT_PLAYER_SHEET_H_

#include "db.h"

namespace fallout {

// Serialization for per-actor CHARACTER SHEETS (PLAYER_SHEET_DESIGN.md §5).
//
// A "sheet" is the four members stage 2 seeds together — skills + base/bonus
// SPECIAL (the proto row), perk ranks, XP/level/karma/unspent SP, and traits.
// They travel as ONE unit for the same reason they are seeded as one: an actor
// carrying three of the four is a chimera. The level a player stands at drives
// the next level-up's HP award, so shipping skills without level is not
// cosmetic.
//
// ONE format, TWO consumers. The join blob and the disk save are the same
// stream — the blob IS the save pipeline (mapSaveToStream), so the row shape is
// written once here and adopted by both rather than diverging into two
// hand-counted layouts (the defect class that ate EVENT_PLAYER_ROSTER).
//
// What is NOT in a sheet, deliberately: the player's NAME and the TAGGED SKILLS
// are still PC-globals with no subject (§8). They join the block when per-actor
// character creation lands, which is also what makes them differ per actor.

// Write sheets for slots [firstSlot, playerActorCount()).
//
// firstSlot = 0 for the join blob: the viewer has no other source for the HOST's
// sheet, and seeding it from the viewer's own gDudeProto (what it did before
// this block existed) shows a stale character to anyone who joins after the host
// has leveled. firstSlot = 1 for the disk save, where slot 0 is already carried
// by the legacy statsSave / traitsSave / perksSave / critterSave handlers.
//
// Writes NOTHING when the range is empty, so a caller that must stay
// byte-identical at N==1 can call it unconditionally.
int playerSheetBlockWrite(File* stream, int firstSlot);

// Apply a block written by playerSheetBlockWrite.
//
// ⚠ SEED THE ROWS FIRST. Only the sheet itself is on the wire; fid, messageId,
// flags and the AI packet come from protoPlayerActorSheetsSeed, and a row that
// never got them renders a nameless fid-0 body.
//
// Fails loud (no partial apply) on a bad magic or an out-of-range slot/count.
// That guard is load-bearing rather than defensive: slot 0's proto row IS
// gDudeProto — not a copy — so a mis-positioned stream would write garbage
// directly into the host's live character (PLAYER_SHEET_DESIGN.md §7 risk 1).
int playerSheetBlockRead(File* stream);

// The DISK-save co-op appendix: the extra player actors' BODIES + inventory,
// then their sheets. It rides the tail of SAVE.DAT, after the 27 vanilla
// handlers, for the same reason the join blob's appendix rides the tail of the
// map stream — the sections above self-delimit, so a reader that predates this
// (stock/vanilla) stops early and ignores it, and the file stays loadable by
// vanilla as a host-only single-player game (owner ruling 2026-07-21: NO version
// bump — vanilla-loadable is a deliberate nicety, at the cost that re-saving in
// vanilla drops the tail).
//
// Extras carry OBJECT_NO_SAVE (the flag that keeps their registry Object* stable
// across map teardown — [[coop-character-identity]]), so objectSaveAll skips
// them and the host's dude blob / sheet handlers only cover slot 0. Without this
// appendix a reload re-clones every extra from the host — two of everything, and
// player 2 becomes player 1 (savegame-core-split "EXTRAS ARE NOT PERSISTED").
//
// Writes NOTHING when there are no extras (playerActorCount() <= 1), so every
// single-player save — and every golden — stays byte-identical to vanilla and
// no magic ever reaches the stream. Detection on load is therefore "read the
// magic; EOF means a vanilla-shaped save with no appendix".
int playerActorAppendixSave(File* stream);

// Reconstruct the extras written by playerActorAppendixSave, in registry slot
// order, and apply the sheet block. Call at the tail of a load, AFTER the map is
// present (extras are placed on it) and the host + slot-0 sheet are restored.
//
// Returns 0 when there is no appendix (EOF — a vanilla/single-player save) as
// well as on a successful reconstruction; -1 only on a genuinely malformed tail.
// SEEDS the sheet rows and registers slot 0 = gDude itself, mirroring the proven
// wire path (client_net.cc), so the caller need not pre-populate the registry.
int playerActorAppendixLoad(File* stream);

} // namespace fallout

#endif /* FALLOUT_PLAYER_SHEET_H_ */
