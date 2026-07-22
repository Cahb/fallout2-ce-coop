#ifndef FALLOUT_SERVER_ACCOUNTS_H_
#define FALLOUT_SERVER_ACCOUNTS_H_

#include "db.h" // File (typedef of XFile) — matches player_sheet.h

namespace fallout {

// The ACCOUNT table (ACCOUNT_IDENTITY_DESIGN.md §1). The NEW layer on top of the
// slot->Object* registry (server_players) and the sessionId->slot bindings
// (server_control): it maps a durable ACCOUNT NAME to a registry SLOT.
//
// WHY the slot cannot be ephemeral: the sheet pid ENCODES the slot
// (playerActorSheetPid = 0x1FFFF00 + slot) and that pid is baked into every
// serialized extra body. So a name owns a slot FOR THE LIFE OF THE SAVE LINEAGE —
// membership is APPEND-ONLY: a slot, once a name's, is never reordered, recycled
// or compacted (ACCOUNT_IDENTITY_DESIGN.md §1 + trap 3). The name is the stable
// key a returning player re-binds by; the slot index is an internal handle.
//
// Account name vs CHARACTER name are separate roles: the character name is the
// per-slot critterNameRow / gDudeName shown in-world; the account name lives ONLY
// here and answers "who is the human" (and carries the ownership token). They may
// hold the same string.
//
// Data only (no SDL / no combat) so it can live in f2_core, where player_sheet.cc
// serializes it and savegame.cc drives it — the same precedent as server_players.

constexpr int kAccountNameMaxLength = 32;
constexpr int kAccountTokenMaxLength = 64;

// Registry slot owning `name` (case-insensitively — a `bob`/`Bob` typo must not
// fork a character and burn a slot, trap 8), or -1 if no slot holds that name.
int accountSlotForName(const char* name);

// Record `name` (+ optional `token`, may be null/empty) as the owner of `slot`.
// Truncates to the fixed widths. Used when a login attaches a name to a slot —
// either a fresh allocation or (re)attaching to a slot that was unowned (a legacy
// 'PACT' save's slots load unowned).
void accountAssign(int slot, const char* name, const char* token);

// The account name owning `slot`, or "" when unowned/out-of-range. Never null.
const char* accountNameForSlot(int slot);

// True iff `slot` carries a non-empty account name.
bool accountSlotOwned(int slot);

// True iff `slot`'s stored token equals `token`. COMPARISON ONLY — the caller
// decides whether to enforce it (F2_REQUIRE_TOKEN); a slot with no stored token
// matches anything so the default-OFF, first-claimer-wins policy needs no branch
// here (ACCOUNT_IDENTITY_DESIGN.md §4).
bool accountTokenMatches(int slot, const char* token);

// Reset every row to unowned. Call wherever the registry is (re)established — a
// world switch or a load must not inherit the previous game's ownership.
void accountClear();

// Serialize rows for slots [0, count) as the disk-save appendix's name/token
// table (ACCOUNT_IDENTITY_DESIGN.md §2). Slot 0 is included so host ownership
// persists. Fixed-width and self-delimiting, like the sheet-block name row.
int accountTableWrite(File* stream, int count);
int accountTableRead(File* stream, int count);

} // namespace fallout

#endif /* FALLOUT_SERVER_ACCOUNTS_H_ */
