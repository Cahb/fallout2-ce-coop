#include "player_sheet.h"

#include "critter.h"
#include "db.h"
#include "debug.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "server_accounts.h"
#include "server_players.h"
#include "skill.h"
#include "stat.h"
#include "trait.h"

namespace fallout {

// Sentinel ahead of the rows. The block sits at the tail of a stream whose
// earlier sections self-delimit, so a length/order bug upstream lands here as a
// silent misread rather than a short read — and the first thing a misread would
// scribble on is the host's live sheet (slot 0 is gDudeProto itself). Cheap
// enough to be worth it at once per join.
static constexpr int kPlayerSheetBlockMagic = 0x50534854; // 'PSHT'

// Sentinel ahead of the DISK appendix (extra actor bodies + sheets). Doubles as
// the "is there an appendix" probe: the loader reads this word at the tail and
// treats a short read (EOF) as "no appendix, vanilla-shaped save".
//
// TWO versions, both still readable (ACCOUNT_IDENTITY_DESIGN.md §2):
//   'PACT' (v1) — count + bodies + sheet block. Loads with NO account table, so
//                 its slots come back UNOWNED (claimable, name-attachable on the
//                 next login).
//   'PAC2' (v2) — inserts an account name/token table (slots [0, count+1),
//                 host included) between the count and the bodies.
// Writes 'PAC2' now; a fresh vanilla/stock build reads neither and stops at EOF,
// loading the save as a host-only single-player game (NO version bump).
static constexpr int kPlayerActorAppendixMagic = 0x50414354; // 'PACT' (v1)
static constexpr int kPlayerActorAppendixMagicV2 = 0x50414332; // 'PAC2' (v2)

// The members, in the order stage 2 seeds them (protoPlayerActorSheetsSeed,
// perkPlayerActorSeedRanks, pcPlayerActorSeedStats, traitsPlayerActorSeed,
// skillsPlayerActorSeed, critterPlayerActorSeedNames). Keeping the two orders
// identical is what makes "seeded but not shipped" a reviewable one-line diff
// instead of a hunt — add to both or the actor is a chimera.
static int playerSheetRowWrite(File* stream, int slot)
{
    if (protoPlayerActorRowWrite(stream, slot) == -1) {
        return -1;
    }

    if (perkPlayerActorRowWrite(stream, slot) == -1) {
        return -1;
    }

    if (pcPlayerActorRowWrite(stream, slot) == -1) {
        return -1;
    }

    if (traitsPlayerActorRowWrite(stream, slot) == -1) {
        return -1;
    }

    if (skillsPlayerActorTaggedRowWrite(stream, slot) == -1) {
        return -1;
    }

    if (critterPlayerActorNameRowWrite(stream, slot) == -1) {
        return -1;
    }

    return 0;
}

static int playerSheetRowRead(File* stream, int slot)
{
    if (protoPlayerActorRowRead(stream, slot) == -1) {
        return -1;
    }

    if (perkPlayerActorRowRead(stream, slot) == -1) {
        return -1;
    }

    if (pcPlayerActorRowRead(stream, slot) == -1) {
        return -1;
    }

    if (traitsPlayerActorRowRead(stream, slot) == -1) {
        return -1;
    }

    if (skillsPlayerActorTaggedRowRead(stream, slot) == -1) {
        return -1;
    }

    if (critterPlayerActorNameRowRead(stream, slot) == -1) {
        return -1;
    }

    return 0;
}

int playerSheetBlockWrite(File* stream, int firstSlot)
{
    int count = playerActorCount() - firstSlot;
    if (count <= 0) {
        // Nothing to say. Emitting a zero-row header instead would append eight
        // bytes to every single-player save and every N==1 blob, which is
        // exactly the byte-identity the degeneracy argument buys us.
        return 0;
    }

    if (fileWriteInt32(stream, kPlayerSheetBlockMagic) == -1) {
        return -1;
    }

    if (fileWriteInt32(stream, firstSlot) == -1) {
        return -1;
    }

    if (fileWriteInt32(stream, count) == -1) {
        return -1;
    }

    for (int slot = firstSlot; slot < firstSlot + count; slot++) {
        if (playerSheetRowWrite(stream, slot) == -1) {
            return -1;
        }
    }

    return 0;
}

int playerSheetBlockRead(File* stream)
{
    int magic;
    if (fileReadInt32(stream, &magic) == -1) {
        return -1;
    }

    if (magic != kPlayerSheetBlockMagic) {
        debugPrint("player_sheet: bad block magic 0x%08x\n", magic);
        return -1;
    }

    int firstSlot;
    if (fileReadInt32(stream, &firstSlot) == -1) {
        return -1;
    }

    int count;
    if (fileReadInt32(stream, &count) == -1) {
        return -1;
    }

    // Range-check BEFORE the first row is applied: a partial apply would leave
    // some actors on wire data and some on seeds, which is the chimera state the
    // whole block exists to prevent.
    if (firstSlot < 0 || count < 0 || firstSlot + count > kMaxPlayerActors) {
        debugPrint("player_sheet: block slots %d..%d out of range\n",
            firstSlot, firstSlot + count - 1);
        return -1;
    }

    for (int slot = firstSlot; slot < firstSlot + count; slot++) {
        if (playerSheetRowRead(stream, slot) == -1) {
            debugPrint("player_sheet: slot %d row read failed\n", slot);
            return -1;
        }
    }

    return 0;
}

int playerActorAppendixSave(File* stream)
{
    // Extras are slots [1, playerActorCount()). Nothing to append at N <= 1 —
    // and appending nothing (not even the magic) is what keeps single-player
    // saves byte-for-byte a vanilla save.
    int extras = playerActorCount() - 1;
    if (extras <= 0) {
        return 0;
    }

    if (fileWriteInt32(stream, kPlayerActorAppendixMagicV2) == -1) {
        return -1;
    }

    // The body count drives the load loop; the sheet block below carries its own
    // count and must agree (the loader cross-checks). Kept explicit rather than
    // re-deriving from playerActorCount() at load time, because the loader runs
    // before the registry is populated.
    if (fileWriteInt32(stream, extras) == -1) {
        return -1;
    }

    // Account name/token table for slots [0, playerActorCount()) — HOST INCLUDED
    // (slot 0), so account ownership of the host character persists too. Sits
    // between the count and the bodies (ACCOUNT_IDENTITY_DESIGN.md §2).
    if (accountTableWrite(stream, extras + 1) == -1) {
        return -1;
    }

    // Bodies + inventory, in slot order — the same order the loader registers
    // them, which is the order objectAssignAllNetIds numbers the registry in.
    // _obj_save_player_actor clears NO_SAVE around the write exactly as
    // _obj_save_dude does for the host.
    for (int slot = 1; slot <= extras; slot++) {
        if (_obj_save_player_actor(stream, playerActorAt(slot)) == -1) {
            return -1;
        }
    }

    // Sheets from slot 1 — slot 0 (the host) is already on disk via the legacy
    // critterSave / statsSave / perksSave / traitsSave / skillsSave handlers.
    return playerSheetBlockWrite(stream, 1);
}

int playerActorAppendixLoad(File* stream)
{
    int magic;
    if (fileReadInt32(stream, &magic) == -1) {
        // EOF at the tail: a vanilla-shaped save with no appendix. Not an error.
        return 0;
    }

    bool v2 = magic == kPlayerActorAppendixMagicV2;
    if (magic != kPlayerActorAppendixMagic && !v2) {
        debugPrint("player_sheet: bad appendix magic 0x%08x\n", magic);
        return -1;
    }

    int extras;
    if (fileReadInt32(stream, &extras) == -1) {
        return -1;
    }

    if (extras < 0 || extras >= kMaxPlayerActors) {
        debugPrint("player_sheet: appendix extra count %d out of range\n", extras);
        return -1;
    }

    // Account table: v2 carries one for slots [0, extras+1) (host included); v1
    // ('PACT') carries none, so its slots load UNOWNED. accountTableRead clears
    // first either way, so a load never inherits the previous game's ownership.
    if (v2) {
        if (accountTableRead(stream, extras + 1) == -1) {
            debugPrint("player_sheet: appendix account table read failed\n");
            return -1;
        }
    } else {
        accountClear();
    }

    // SEED the rows before any actor resolves its pid, and register slot 0 =
    // gDude, mirroring the wire path (client_net.cc). The seeds fill the parts a
    // player-actor row never varies (fid, messageId, flags, AI packet) from this
    // process's gDudeProto; the sheet block below overwrites the sheet proper.
    // gPlayerActorProtos is a fixed array, so this is value-init, not allocation.
    protoPlayerActorSheetsSeed();
    perkPlayerActorSeedRanks();
    pcPlayerActorSeedStats();
    traitsPlayerActorSeed();
    skillsPlayerActorSeed();
    critterPlayerActorSeedNames();

    // Idempotent + slot-0-guarded (server_players.cc): safe whether or not the
    // caller already registered the host.
    if (playerActorRegister(gDude) != 0) {
        debugPrint("player_sheet: host did not take registry slot 0\n");
        return -1;
    }

    for (int slot = 1; slot <= extras; slot++) {
        Object* actor = nullptr;
        if (_obj_load_player_actor(stream, &actor) == -1 || actor == nullptr) {
            debugPrint("player_sheet: appendix actor %d body load failed\n", slot);
            return -1;
        }

        // ⚠ _obj_load_player_actor bakes in the VIEWER's lifecycle rule and
        // STRIPS OBJECT_NO_REMOVE (object.cc): a client's blob-loaded extras must
        // die on the next rebaseline. On the SERVER the opposite is required — an
        // extra must carry NO_REMOVE so map teardown spares it and its registry
        // Object* stays valid across transitions ([[coop-character-identity]];
        // serverSpawnExtraActors sets the very same bit at spawn). Re-assert it so
        // a reloaded co-op actor matches the fresh-spawn lifecycle class exactly;
        // without this the first map change after a load frees a still-registered
        // actor and the netId walk faults on the dangling pointer.
        actor->flags |= OBJECT_NO_REMOVE;

        if (playerActorRegister(actor) != slot) {
            debugPrint("player_sheet: appendix actor %d registered out of slot\n", slot);
            return -1;
        }

        // A body saved while its player was away round-trips as OFF-MAP: tile -1
        // in the save → the loader's objectSetLocation(-1) failed closed → it
        // holds a floating list node, exactly the parked state. Mark it so
        // baselines/placement keep skipping it; the re-login reattach
        // (serverControlDrainPresence) is what brings it back.
        if (actor->tile == -1) {
            playerActorSetOnline(slot, false);
        }

        // ⚠ The sheet pid ENCODES the slot, and the pid rode along in the saved
        // body. If a body ever lands in a slot whose sheet pid it does not carry,
        // the slot-keyed sheet block below would apply this actor's row to the
        // WRONG actor — slot 0's row IS gDudeProto, so the worst case scribbles on
        // the live host. This can only happen if the save's slot order was
        // remapped (which the append-only contract forbids); catch it here rather
        // than discover it as silent host corruption (ACCOUNT_IDENTITY_DESIGN.md
        // §2 + trap 3).
        if (actor->pid != playerActorSheetPid(slot)) {
            debugPrint("player_sheet: appendix actor %d pid 0x%X != slot pid 0x%X\n",
                slot, actor->pid, playerActorSheetPid(slot));
            return -1;
        }
    }

    // Applied AFTER the registry is populated: the block is keyed by slot, and
    // slot 0's row is gDudeProto itself, so a misread here corrupts the live
    // host — fail loud, never half-apply.
    return playerSheetBlockRead(stream);
}

} // namespace fallout
