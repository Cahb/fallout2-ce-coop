#include "player_sheet.h"

#include <stdio.h> // TEMP [psht]: stderr trace of the sheet-delta emit
#include <stdlib.h> // getenv — sheet-delta temp-file path

#include "critter.h"
#include "db.h"
#include "debug.h"
#include "object.h"
#include "perk.h"
#include "presenter.h"
#include "proto.h"
#include "queue.h"
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
// TWO versions, both still readable:
//   'PSHT' (v1) — six rows: proto, perk ranks, PC stats, traits, tagged skills, name.
//   'PSH2' (v2) — appends the OWED FREE PERK PICK byte (perk.cc), the level-up
//                 entitlement a player spends through the sheet edit intents. A v1
//                 row simply leaves the flag at whatever the seed left, i.e. owing
//                 nothing, which is what an actor saved before the flag existed had.
// Writes v2; the reader accepts either, so co-op saves taken before this change
// still load.
static constexpr int kPlayerSheetBlockMagic = 0x50534854; // 'PSHT' (v1)
static constexpr int kPlayerSheetBlockMagicV2 = 0x50534832; // 'PSH2' (v2)

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
// Tail section carrying each extra's queued timed effects (drug/rad/poison/etc.).
// Appended AFTER the sheet block; a save written before this section existed (or a
// vanilla save) simply hits EOF there, which the loader treats as "no events" — so
// no appendix-magic bump is needed, same self-delimiting nicety as the appendix
// itself.
static constexpr int kPlayerActorEventsMagic = 0x50414556; // 'PAEV'

// Per-slot "sheet row changed this beat" bits, set by playerSheetMarkDirty and
// drained by playerSheetDeltaEmit. A runtime sheet mutation (drug, level-up,
// trait/perk change) sets the bit; the beat coalesces a whole burst into one
// row emit. Slot 0 (host) rides it too — a viewer joined after the host leveled
// needs the host's live sheet just as much.
static bool gPlayerSheetDirty[kMaxPlayerActors] = { false };

// The members, in the order stage 2 seeds them (protoPlayerActorSheetsSeed,
// perkPlayerActorSeedRanks, pcPlayerActorSeedStats, traitsPlayerActorSeed,
// skillsPlayerActorSeed, critterPlayerActorSeedNames), plus the v2 tail. Keeping
// the two orders identical is what makes "seeded but not shipped" a reviewable
// one-line diff instead of a hunt — add to both or the actor is a chimera.
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

    // v2 tail: the owed free perk pick. Last on purpose — appending keeps the
    // v1 prefix byte-identical, which is what lets the reader below take either.
    if (perkPlayerActorOwedPickRowWrite(stream, slot) == -1) {
        return -1;
    }

    return 0;
}

static int playerSheetRowRead(File* stream, int slot, bool withOwedPick)
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

    if (withOwedPick) {
        if (perkPlayerActorOwedPickRowRead(stream, slot) == -1) {
            return -1;
        }
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

    if (fileWriteInt32(stream, kPlayerSheetBlockMagicV2) == -1) {
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

    bool v2 = magic == kPlayerSheetBlockMagicV2;
    if (magic != kPlayerSheetBlockMagic && !v2) {
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
        if (playerSheetRowRead(stream, slot, v2) == -1) {
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
    if (playerSheetBlockWrite(stream, 1) == -1) {
        return -1;
    }

    // Extras' queued timed effects. The appendix OWNS these (queueSave is filtered
    // to skip slot >= 1 owners) because the vanilla queueLoad rebinds owners by id
    // inside the save handler loop, before this tail step reconstructs the extras —
    // so an extra-owned event would otherwise orphan to owner==nullptr on load
    // (permanent stat penalty, or a null-critter crash when it fires).
    if (fileWriteInt32(stream, kPlayerActorEventsMagic) == -1) {
        return -1;
    }

    for (int slot = 1; slot <= extras; slot++) {
        if (queueSaveEventsForOwner(stream, playerActorAt(slot)) == -1) {
            return -1;
        }
    }

    return 0;
}

int playerActorAppendixLoad(File* stream)
{
    // ►►►► DO NOT USE fileReadInt32 TO DETECT EOF HERE, AND THAT IS NOT A STYLE
    // PREFERENCE — IT COST EVERY SOLO SAVE ON DISK. fileReadInt32 tests
    // `xfileRead(...) == -1` (db.cc), but xfileRead returns a size_t COUNT OF
    // ELEMENTS READ: 0 at end of file, never -1, and being unsigned it cannot ever
    // equal -1. So fileReadInt32 CANNOT FAIL AT EOF. It returns 0 (success) with
    // `value` left as uninitialised stack garbage, byte-swapped into the out-param.
    //
    // Which turned the guard below inside out. A single-player-shaped save writes NO
    // appendix at all (playerActorAppendixSave returns early at extras <= 0, on
    // purpose, to keep a solo save byte-for-byte a vanilla save) — so the intended
    // "clean EOF => no appendix => not an error" path was UNREACHABLE, the magic read
    // as garbage (0 in practice), and the loader rejected the whole file as corrupt.
    // Every save made while playing alone was unloadable: `boot failed for slot N`,
    // with nothing wrong with the save. 5 of the owner's 8 slots, including the
    // autosave. The three that loaded were the ones with a real appendix.
    //
    // A COUNTED read is the only honest EOF test on this API. Byte order matches
    // fileReadInt32's big-endian swap, because that is how the magic was written.
    unsigned char magicBytes[4];
    if (fileRead(magicBytes, 1, sizeof(magicBytes), stream) != sizeof(magicBytes)) {
        // Fewer than 4 bytes left: a vanilla-shaped save with no appendix. Not an error.
        return 0;
    }
    int magic = (magicBytes[0] << 24) | (magicBytes[1] << 16) | (magicBytes[2] << 8) | magicBytes[3];

    bool v2 = magic == kPlayerActorAppendixMagicV2;
    if (magic != kPlayerActorAppendixMagic && !v2) {
        // Reached only with 4 real bytes in hand that are not either magic, i.e. an
        // actually malformed tail — which is worth refusing, unlike EOF.
        debugPrint("player_sheet: bad appendix magic 0x%08x\n", magic);
        fprintf(stderr, "player_sheet: bad appendix magic 0x%08x — refusing the save\n", magic);
        return -1;
    }

    int extras;
    if (fileReadInt32(stream, &extras) == -1) {
        fprintf(stderr, "player_sheet: appendix truncated before the extra count\n");
        return -1;
    }

    if (extras < 0 || extras >= kMaxPlayerActors) {
        debugPrint("player_sheet: appendix extra count %d out of range\n", extras);
        fprintf(stderr, "player_sheet: appendix extra count %d out of range (max %d)\n",
            extras, kMaxPlayerActors - 1);
        return -1;
    }

    // Account table: v2 carries one for slots [0, extras+1) (host included); v1
    // ('PACT') carries none, so its slots load UNOWNED. accountTableRead clears
    // first either way, so a load never inherits the previous game's ownership.
    if (v2) {
        if (accountTableRead(stream, extras + 1) == -1) {
            debugPrint("player_sheet: appendix account table read failed\n");
            fprintf(stderr, "player_sheet: appendix ACCOUNT TABLE read failed (%d slots)\n", extras + 1);
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
            fprintf(stderr, "player_sheet: appendix ACTOR %d BODY load failed\n", slot);
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
            fprintf(stderr, "player_sheet: appendix actor %d pid 0x%X != slot pid 0x%X\n",
                slot, actor->pid, playerActorSheetPid(slot));
            return -1;
        }
    }

    // Applied AFTER the registry is populated: the block is keyed by slot, and
    // slot 0's row is gDudeProto itself, so a misread here corrupts the live
    // host — fail loud, never half-apply.
    if (playerSheetBlockRead(stream) == -1) {
        return -1;
    }

    // Re-queue each extra's timed effects now that the bodies exist and are
    // registered (queueAddEvent binds to the fresh Object*). EOF here = a save
    // written before this section existed → nothing to re-queue, not an error.
    //
    // ►► COUNTED READ, for the same reason as the appendix magic above: fileReadInt32
    // cannot fail at EOF, so this "not an error" path was unreachable too and an older
    // co-op save — one with real extras, written before this section existed — was
    // rejected outright on the garbage magic. Both optional-tail probes in this file
    // had the same hole; this was the second one.
    unsigned char eventsMagicBytes[4];
    if (fileRead(eventsMagicBytes, 1, sizeof(eventsMagicBytes), stream) != sizeof(eventsMagicBytes)) {
        return 0;
    }
    int eventsMagic = (eventsMagicBytes[0] << 24) | (eventsMagicBytes[1] << 16)
        | (eventsMagicBytes[2] << 8) | eventsMagicBytes[3];

    if (eventsMagic != kPlayerActorEventsMagic) {
        debugPrint("player_sheet: bad appendix events magic 0x%08x\n", eventsMagic);
        fprintf(stderr, "player_sheet: bad appendix EVENTS magic 0x%08x — refusing the save\n", eventsMagic);
        return -1;
    }

    for (int slot = 1; slot <= extras; slot++) {
        if (queueLoadEventsForOwner(stream, playerActorAt(slot)) == -1) {
            debugPrint("player_sheet: slot %d event reload failed\n", slot);
            return -1;
        }
    }

    return 0;
}

// A one-slot sheet block, byte-compatible with playerSheetBlockRead (magic +
// firstSlot=slot + count=1 + the row). Used by the live delta channel.
static int playerSheetBlockWriteOne(File* stream, int slot)
{
    if (fileWriteInt32(stream, kPlayerSheetBlockMagicV2) == -1) {
        return -1;
    }
    if (fileWriteInt32(stream, slot) == -1) {
        return -1;
    }
    if (fileWriteInt32(stream, 1) == -1) {
        return -1;
    }
    return playerSheetRowWrite(stream, slot);
}

void playerSheetMarkDirty(Object* critter)
{
    if (critter == nullptr) {
        return;
    }
    int slot = playerActorSlotOf(critter);
    if (slot >= 0 && slot < kMaxPlayerActors) {
        gPlayerSheetDirty[slot] = true;
    }
}

void playerSheetDeltaEmit()
{
    // Network only. On any other presenter (single-player, golden probe) clear the
    // bits and emit nothing, so the stat setters' mark calls are free there.
    bool wanted = presenter()->wantsSheetDeltas();

    int count = playerActorCount();
    if (count > kMaxPlayerActors) {
        count = kMaxPlayerActors;
    }

    for (int slot = 0; slot < count; slot++) {
        if (!gPlayerSheetDirty[slot]) {
            continue;
        }
        gPlayerSheetDirty[slot] = false;

        if (!wanted) {
            continue;
        }

        // Serialize the one-slot block into RAM. This used to go through a scratch file
        // because XFILE_TYPE_MEMORY was read-only; it is not any more (db.h
        // fileOpenMemoryWrite), which matters more here than for the join blob — a sheet
        // delta fires whenever a stat changes, so the old path touched /tmp repeatedly
        // during ordinary play.
        File* out = fileOpenMemoryWrite();
        if (out == nullptr) {
            continue;
        }
        int rc = playerSheetBlockWriteOne(out, slot);
        int len = (int)fileTell(out);
        if (rc == -1 || len <= 0) {
            fileClose(out);
            continue;
        }

        const unsigned char* written = fileMemoryData(out);
        if (written == nullptr || fileMemorySize(out) < len) {
            fileClose(out);
            continue;
        }
        std::vector<unsigned char> buf(written, written + len);
        int got = len;
        fileClose(out);
        if (got != len) {
            continue;
        }

        // TEMP DIAGNOSTIC [psht]: "the perk/skill only shows up if I RECONNECT" has
        // exactly three possible cuts and this is the first of them — the row was
        // marked dirty and shipped. Its ABSENCE after an applied `perkpick rc=0` means
        // nothing marked the row (a mutation site missing playerSheetMarkDirty); its
        // presence moves the hunt to the client's matching [psht] line.
        fprintf(stderr, "[psht] emit slot=%d len=%d\n", slot, len);
        presenter()->playerSheetDelta(slot, buf.data(), len);
    }
}

} // namespace fallout
