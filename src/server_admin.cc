#include "server_admin.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "animation.h" // reg_anim_clear — cancel a stress critter's anims pre-destroy
#include "combat.h" // isInCombat / _combat_delete_critter
#include "command.h" // probeApplyAggro — `stress` forces the fight the way `aggro` does
#include "critter.h" // critterRevive / critterIsDead / critterGetName — the `revive` verb
#include "db.h"
#include "debug.h"
#include "game.h"
#include "game_dialog.h" // gameDialogServerNodeActive — never autosave mid-node
#include "map.h" // mapTransitionPending / mapGetLoadGeneration / gElevation
#include "map_defs.h" // HEX_GRID_SIZE / hexGridTileIsValid — random stress placement
#include "object.h" // objectCreateWithPid / objectSetLocation / objectDestroy / _obj_blocking_at
#include "object_delta.h" // objectDeltaForgetShadow — re-announce a hand-repaired object
#include "art.h" // artLock/artGetFrameCount — the trunk frame-range repair
#include "proto.h" // protoGetProto — validate a spawn pid before creating
#include "proto_instance.h" // _objPMAttemptPlacement (see why fixcar does NOT use it)
#include "tile.h" // tileDistanceBetween — bias stress spawns toward the players
#include "party_member.h" // partyMemberFindByPid — the `fixcar` trunk check
#include "proto_types.h" // PROTO_ID_CAR_TRUNK — the `fixcar` trunk check
#include "worldmap.h" // wmMapIsSaveable — some maps forbid saving outright;
                      // wmCar*/wmArea* — the `fixcar` verb
#include "game_movie.h" // MOVIE_COUNT / gameMoviePlay — the `movie` test verb
#include "msg_channel.h"
#include "platform_compat.h"
#include "presenter.h"
#include "queue.h"
#include "savegame.h"
#include "scripts.h" // gameTimeAddTicks / gameTimeGetTime — the `timeskip` verb
#include "inventory.h" // _invenWieldFunc / HAND_RIGHT — arm stress hostiles
#include "item.h" // itemAdd
#include "path.h" // _make_path — stress spawns must be REACHABLE, not merely unblocked
#include "server_players.h"
#include "rest.h" // restPerform / RestOutcome — the operator's rest verb
#include "sheet_intent.h" // sheetEdit* — the operator's side of the sheet edit intents
#include "perk.h" // perkOwedPickGet / PERK_CHOICE_PENDING_* — the `sheet` read-out
#include "skill.h" // skillGetValue / skillsGetTagged — the `sheet` read-out
#include "trait.h" // traitsGetSelected — the `sheet` read-out
#include "stat.h" // pcAddExperience / pcGetStat — the xp verb // playerActorAt / playerActorCount — the `revive` verb

namespace fallout {

// Vanilla's slot count. The on-disk layout is SAVEGAME\SLOTnn\SAVE.DAT for
// nn in 01..10, and the header carries a 30-char description — which is exactly
// the "numbered slot with an optional mnemonic label" the operator wants, so
// nothing new is invented here.
static constexpr int kSlotCount = 10;

// SLOT11.. — the AUTOSAVE ROTATION WINDOW (indices 10..10+kAutosaveKeep-1).
// Deliberately past everything the vanilla client's save/load screens can reach,
// so the periodic autosave can never clobber a real save. The OPERATOR still sees
// them: parseSlot and the `saves` listing run to kAdminSlotCount, so `load 13`
// restores an autosave (as does F2_SERVER_LOAD=13).
//
// ►►►► A WINDOW, NOT ONE SLOT — and this is a correctness fix, not a luxury.
// It was a single slot rewritten in place, and serverAutosaveTick fires on EVERY
// MAP CHANGE. So walking into a random encounter immediately overwrote the only
// checkpoint in existence with a mid-encounter save on a TRANSIENT map: hostiles
// already in perception range, and (until the worldmap-teardown fix) the only exit
// wedged the map. The save was perfectly valid — it was the POSITION that was
// unreloadable, and there was no older one left to go back to. One bad checkpoint
// could therefore eat a session. N checkpoints cannot.
//
// ROUND-ROBIN, not shift-down-the-chain: rotating by renaming SLOT11->SLOT12->...
// would need the directories moved on disk, and lsgPerformSaveGame mkdirs its slot
// but never CLEANS it, so any recycled directory must be emptied first anyway
// (a stale MAPS *.SAV left behind is a real corruption: mapLoadSaved reads the
// slot by map NAME, so it would happily load a map from a different save). Writing
// to the next slot in sequence after savegameEraseSlot() needs no renames at all.
// The cost is that the newest autosave is not always slot 11, which is exactly why
// the slot is named in the announcement and in the server log.
static constexpr int kAutosaveSlot = 10;
static constexpr int kAutosaveKeep = kSaveSlotSpace - kAutosaveSlot;
static constexpr int kAdminSlotCount = kSaveSlotSpace;

static ServerAdminRequest gPendingRequest;

// Skip leading blanks and return the rest, or nullptr when the line is spent.
static const char* skipBlanks(const char* p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p != '\0' ? p : nullptr;
}

// Parse "<verb> <rest>": copies the verb out, returns the remainder (trimmed) or
// nullptr when there is none.
static const char* splitVerb(const char* line, char* verbOut, size_t verbSize)
{
    const char* p = skipBlanks(line);
    if (p == nullptr) {
        verbOut[0] = '\0';
        return nullptr;
    }

    size_t n = 0;
    while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' && n + 1 < verbSize) {
        verbOut[n] = p[n];
        n++;
    }
    verbOut[n] = '\0';

    while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t') {
        n++; // an over-long verb: consume the tail so the remainder is right
    }
    return skipBlanks(p + n);
}

// Operator-facing slot numbers are 1-based (SLOT01 is "1") because that is what
// the directory names say; every API below wants 0-based. Returns -1 if `text`
// is not a slot number in range.
static int parseSlot(const char* text)
{
    if (text == nullptr) {
        return -1;
    }
    char* end = nullptr;
    long value = strtol(text, &end, 10);
    if (end == text || value < 1 || value > kAdminSlotCount) {
        return -1;
    }
    return static_cast<int>(value) - 1;
}

// Read one slot's header WITHOUT loading the world. Returns false for an empty,
// unreadable or corrupt slot; `dataOut` is only valid when it returns true.
static bool readSlotHeader(int slot, LoadSaveSlotData& dataOut)
{
    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s%.2d\\%s", "SAVEGAME", "SLOT", slot + 1, "SAVE.DAT");

    int fileSize;
    if (dbGetFileSize(path, &fileSize) != 0) {
        return false; // empty slot
    }

    File* stream = fileOpen(path, "rb");
    if (stream == nullptr) {
        return false;
    }

    // Mirrors _GetSlotList (loadsave.cc): the slot is threaded BOTH as the
    // ambient cursor and as the explicit argument, because the header reader
    // indexes its own table with the argument while the surrounding operation
    // reads the cursor.
    savegameSetSlot(slot);
    bool ok = lsgLoadHeaderInSlot(stream, slot) != -1;
    fileClose(stream);

    if (ok) {
        dataOut = *savegameSlotData(slot);
    }
    return ok;
}

// The one true save writer: refresh the write root, aim the slot cursor, stamp
// the description, blank the thumbnail, write. Shared by the operator's `save`
// verb and the autosave ticker so the two can never drift.
static bool adminWriteSave(int slot, const char* label)
{
    savegameRefreshPatchesPath();
    savegameSetSlot(slot);

    // The label IS the vanilla description field, so a save written here
    // shows up named in a stock client's load screen.
    LoadSaveSlotData* data = savegameSlotData(slot);
    memset(data->description, 0, sizeof(data->description));
    if (label != nullptr) {
        strncpy(data->description, label, sizeof(data->description) - 1);
    }

    // No screen, so no thumbnail to grab. Null leaves the preview block
    // written but blank, which is what keeps the file loadable by a client
    // that expects the block to be there.
    savegameSetPreviewBuffer(nullptr);

    return lsgPerformSaveGame() != -1;
}

// Which autosave slot to overwrite next: an EMPTY one if the window is not full
// yet, otherwise the OLDEST. Recomputed from the saves on disk every time rather
// than held in a cursor, which makes it inherently restart-safe — a fresh process
// cannot clobber the newest checkpoint just because its counter started at zero,
// which is precisely what a remembered cursor would do.
//
// ►► ORDERED BY THE HEADER'S gameTime (in-game clock), NOT by the real-world save
// time. Not a stylistic choice: `fileTime` is filled as `tm_hour + tm_min`
// (savegame.cc:539 — vanilla's own bug, inherited, left alone here because it is a
// save-header display field and changing it is a format decision, not this fix's
// business). 14:30 and 01:43 both come out 44, so it cannot order two saves from
// the same day. gameTime is written correctly and rises monotonically through a
// campaign, so it is the better recency signal anyway. A save from an ABANDONED
// timeline (operator loaded an older save and played on) sorts as newest and is
// therefore preserved longest — acceptable, arguably right: it is the one nothing
// else can reproduce.
static int autosavePickSlot()
{
    int oldest = kAutosaveSlot;
    bool haveOldest = false;
    unsigned int oldestGameTime = 0;

    for (int index = 0; index < kAutosaveKeep; index++) {
        int slot = kAutosaveSlot + index;
        LoadSaveSlotData data;
        if (!readSlotHeader(slot, data)) {
            return slot; // never recycle while the window still has room
        }
        if (!haveOldest || data.gameTime < oldestGameTime) {
            haveOldest = true;
            oldestGameTime = data.gameTime;
            oldest = slot;
        }
    }

    return oldest;
}

// Periodic unattended save into the autosave window. Called every MAIN-PHASE beat
// (server_main's intent drain — never from a modal pump, which is what already
// rules out saving mid-dialog/barter/worldmap/movie structurally; the mode
// checks below are the belt to that suspenders). Cadence is WALL clock,
// deliberately: the sim clock parks for the entire life of a block-and-pump
// modal driver, so a sim-tick cadence silently starves ([[worldmap-streaming-
// track]]'s lesson). A map change latches an opportunistic save — the world is
// freshly settled right after a transition — and an interval save that comes
// due while unsafe simply fires on the first safe beat after.
void serverAutosaveTick()
{
    static const int intervalSecs = []() {
        const char* env = getenv("F2_AUTOSAVE_SECS");
        return env != nullptr ? atoi(env) : 300;
    }();
    if (intervalSecs <= 0) {
        return; // F2_AUTOSAVE_SECS=0 turns the feature off
    }

    static std::chrono::steady_clock::time_point lastSave = std::chrono::steady_clock::now();
    static int lastGeneration = -1;

    int generation = mapGetLoadGeneration();
    if (lastGeneration == -1) {
        lastGeneration = generation; // boot: the world just loaded, nothing owed
    }

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    bool intervalDue = now - lastSave >= std::chrono::seconds(intervalSecs);
    bool mapChanged = generation != lastGeneration;
    if (!intervalDue && !mapChanged) {
        return;
    }

    // Refuse while the world is unsettled; the trigger stays latched (lastSave/
    // lastGeneration only advance on a successful attempt below).
    if (isInCombat() || mapTransitionPending()
        || GameMode::isInGameMode(GameMode::kDialog | GameMode::kBarter | GameMode::kWorldmap)
        || gameDialogServerNodeActive() || gameMovieIsPlaying()) {
        return;
    }

    // NOTE: no wmMapIsSaveable() gate. That flag only means "this map's .SAV is not
    // PERSISTED across revisits" (random encounters / free-roam wilderness regenerate)
    // — lsgPerformSaveGame still succeeds on them (map.cc:1507 just erases the transient
    // .SAV; the savegame itself, incl. worldmap position + player state, is written), the
    // same as a vanilla manual save there. Gating on it wrongly skipped autosave whenever
    // the player entered a non-city/encounter map (owner-reported). Autosave fires on
    // every map entry now; on a transient map the checkpoint captures the persistent
    // state, which is the sensible reload point.
    int slot = autosavePickSlot();

    // ►► EMPTY THE TARGET BEFORE WRITING IT. lsgPerformSaveGame mkdirs its slot but
    // never cleans it, and a recycled slot still holds the PREVIOUS autosave's
    // MAPS *.SAV working copies. mapLoadSaved resolves those by map NAME out of the
    // slot, so a leftover from an unrelated map is not clutter — it is a map the
    // next load of this slot could actually pick up. savegameEraseSlot is vanilla's
    // own primitive for exactly this (SAVE.DAT + every *.SAV + AUTOMAP.DB.SAV).
    savegameRefreshPatchesPath();
    savegameSetSlot(slot);
    savegameEraseSlot();

    bool ok = adminWriteSave(slot, "autosave");
    fprintf(stderr, "f2_server: autosave -> slot %d (window %d-%d) %s%s\n",
        slot + 1, kAutosaveSlot + 1, kAutosaveSlot + kAutosaveKeep,
        ok ? "ok" : "FAILED", mapChanged ? " (map change)" : "");
    if (ok) {
        // Every player sees the checkpoint land — same system channel as the
        // join/leave announcements. ►► IT NAMES THE SLOT because the window
        // rotates: "auto-saved" alone would leave both the players and the
        // operator guessing which of N slots to reload after something goes
        // wrong, which is the moment the information is actually needed.
        char msg[96];
        snprintf(msg, sizeof(msg), "Game auto-saved to slot %d.", slot + 1);
        presenter()->consoleMessageStyled(0, kMsgChannelSystem, msg);
    }
    lastSave = now;
    lastGeneration = generation;
}

// ---- Stress-test spawning (`spawn` / `stress` / `despawnall`) ----
//
// Lives on the ADMIN channel by construction, not by runtime gating: the
// golden probe drives commandDispatch and never serverAdminLine, so these
// verbs cannot perturb a golden no matter what they do (the
// serverDedicatedActive-vs-serverLoopActive trap does not arise). Purpose:
// spawn waves of hostiles on a big map (denbus1/2) to hunt co-op desyncs
// under load. Ceilings to respect (stress recon 2026-07-22): object ids are
// process-monotonic and NEVER recycled (warns at 18000 — despawnall frees
// objects but not ids; a map reload is the id reset), and combat AI is O(n²)
// in combatants — 10-30 is the smooth range, 40-50 deliberately hurts.

// What `spawn`/`stress` created, so `despawnall` can destroy exactly that.
// id+pid ride along because a stale Object* does not go dead, it can resolve
// to a recycled allocation — destroy only what still proves to be ours.
struct StressSpawnRecord {
    Object* obj;
    int id;
    int pid;
};
static std::vector<StressSpawnRecord> gStressSpawned;

// "Raider" (critters.lst 238) — an armed humanoid hostile that exists as pure
// proto data, loadable on any map.
static constexpr int kStressDefaultPid = 0x010000EE;

// A random unblocked tile, biased toward the players: the first half of the
// attempts must land within 30 hexes of the host so the fight happens where
// the testers are; the back half accepts anywhere walkable. PRIVATE rng on
// purpose — drawing from the sim RNG would perturb sim state, and a caller-
// supplied seed makes a found desync reproducible.
// ►► CAN A PLAYER ACTUALLY WALK BETWEEN THIS TILE AND SOMEBODY? Uses the SAME test the
// combat AI uses to decide a destination is reachable (combat_ai.cc:1194:
// `_make_path(..., nullptr, 1) > 0`), so a tile accepted here is a tile the AI will be
// able to path out of. That is the whole point: a hostile that cannot reach anyone just
// stands there and the stress run measures nothing.
//
// Pathed FROM the player with the player as the pathing object, so the query runs against
// a REAL object's blocking/door rules. We cannot path with the hostile itself — it does
// not exist yet, and creating one per candidate would announce a SPAWN to every viewer and
// burn a process-lifetime object id per rejected tile ([[object-id-budget-long-session]]).
//
// Checks EVERY player actor, not just gDude: in co-op the party can be spread across the
// map, and "reachable by anyone" is the useful bar. Dead and off-elevation actors are
// skipped — pathing to a corpse or through a floor proves nothing.
static bool stressTileReachesAnyPlayer(int tile, int elevation)
{
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* actor = playerActorAt(slot);
        if (actor == nullptr || actor->elevation != elevation || critterIsDead(actor)) {
            continue;
        }
        if (_make_path(actor, actor->tile, tile, nullptr, 1) > 0) {
            return true;
        }
    }
    return false;
}

// Pick a tile to drop a hostile on. `rejected` counts candidates thrown out for being
// UNREACHABLE, so the caller can tell the operator "this map is tight" instead of leaving a
// short spawn count unexplained.
//
// ►► THE OLD VERSION SPAWNED OFF-MAP AND INSIDE SEALED ROOMS, which is what made stress
// runs "mostly useless or hit-or-miss" (owner). Two reasons, both fixed here:
//   1. hexGridTileIsValid only bounds the GRID, not the playable map, and after 400
//      attempts the old code dropped its distance gate entirely and took any grid tile —
//      so late spawns landed anywhere, including off the map edges.
//   2. "not blocking" is not "reachable": a free tile can sit behind a wall or in a closed
//      room, where the AI can never path to anyone.
// The reachability test subsumes BOTH: an off-map or sealed tile cannot path to a player.
static int stressRandomFreeTile(int elevation, std::mt19937& rng, int& rejected)
{
    // Cheap tests first, path LAST: A* is the only expensive step here, and the distance
    // gate keeps the searches short by construction.
    for (int attempt = 0; attempt < 800; attempt++) {
        int tile = static_cast<int>(rng() % HEX_GRID_SIZE);
        if (!hexGridTileIsValid(tile)) {
            continue;
        }
        if (_obj_blocking_at(nullptr, tile, elevation) != nullptr) {
            continue;
        }
        // Near SOME player. Unlike the old gate this is never relaxed — a hostile 200
        // tiles away is not stress, it is a spawn leak. Cheap: pure hex distance.
        bool nearAnyPlayer = false;
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* actor = playerActorAt(slot);
            if (actor != nullptr && actor->elevation == elevation
                && tileDistanceBetween(tile, actor->tile) <= 30) {
                nearAnyPlayer = true;
                break;
            }
        }
        if (!nearAnyPlayer) {
            continue;
        }
        if (!stressTileReachesAnyPlayer(tile, elevation)) {
            rejected++;
            continue;
        }
        return tile;
    }
    return -1;
}

// ─── STRESS LOADOUTS ────────────────────────────────────────────────────────
//
// ►► A SPAWNED CRITTER IS UNARMED. objectCreateWithPid builds it from its PROTO, and a
// critter proto carries no inventory — in a shipped map a raider's gun comes from the map
// file's instance data or from a script, neither of which a synthetic spawn has. So every
// `stress` hostile fought BARE-HANDED, which is why a stress run neither looked nor
// performed like a real fight (owner: "are they all unarmed now or something?").
//
// Hand each one a random weapon plus ammo for it. Ammo matters: a wielded gun with an empty
// magazine falls back to unarmed, so a table without ammunition would look armed and fight
// bare-handed — worse than obviously unarmed, because it hides the problem.
//
// Deliberately a MIXED table (melee, pistols, SMG, rifle, shotgun): variety is the point of
// a stress run — different AI attack modes, different anim sets, different sound paths, and
// projectile weapons exercise the throw/flight presentation the melee ones never touch.
// PIDs from docs/ITEM_GLOSSARY.md; ammoPid -1 = a melee weapon that needs none.
struct StressLoadout {
    int weaponPid;
    int ammoPid;
    int ammoCount;
};

static const StressLoadout kStressLoadouts[] = {
    { 8, 29, 24 }, // 10mm Pistol + 10mm JHP
    { 9, 30, 30 }, // 10mm SMG + 10mm AP
    { 10, 34, 20 }, // Hunting Rifle + .223 FMJ
    { 23, 35, 30 }, // Assault Rifle + 5mm JHP
    { 94, 95, 12 }, // Shotgun + 12 ga. shells
    { 18, 30, 18 }, // Desert Eagle .44 + 10mm AP
    { 13, 14, 4 }, // Rocket Launcher + Explosive Rockets (AoE — brutal, which is the point)
    { 7, -1, 0 }, // Spear — also THROWN, so it exercises the flight/pickup path
    { 20, -1, 0 }, // Crowbar
    { 236, -1, 0 }, // Combat Knife
    { 6, -1, 0 }, // Sledgehammer
};

// Arm one spawned hostile from the table. Failures are non-fatal and traced, never fatal to
// the spawn: an unarmed hostile is still a hostile, and losing the whole stress run because
// one proto is missing would be worse.
static void stressArmOne(Object* critter, std::mt19937& rng)
{
    // ►► ONLY OFFER WEAPONS THIS BODY CAN ACTUALLY HOLD. Not every critter frame set has
    // animations for every weapon class — a body handed one it cannot animate does NOT equip
    // it and fights UNARMED (inventory.cc's artExists gate), which is the exact bug this
    // whole function exists to fix, and it would come back as a silent one-in-N.
    //
    // So: walk the table from a random offset and take the first entry whose art this critter
    // supports. weaponArtSupportedForCritter asks from the PID, so nothing is created for a
    // rejected guess — no object churn and no burned object ids. This is also what makes it
    // safe to keep rockets and throwables in the table: a body that cannot hold a launcher
    // simply never draws one.
    const int loadoutCount = (int)(sizeof(kStressLoadouts) / sizeof(kStressLoadouts[0]));
    const StressLoadout* chosen = nullptr;
    int offset = (int)(rng() % (unsigned)loadoutCount);
    for (int i = 0; i < loadoutCount; i++) {
        const StressLoadout& candidate = kStressLoadouts[(offset + i) % loadoutCount];
        if (weaponArtSupportedForCritter(critter, candidate.weaponPid)) {
            chosen = &candidate;
            break;
        }
    }
    if (chosen == nullptr) {
        fprintf(stderr, "f2_server: stress net=%d (pid 0x%X) supports NO table weapon's art —"
                        " left unarmed\n",
            critter->netId, critter->pid);
        return;
    }
    const StressLoadout& loadout = *chosen;

    Object* weapon = nullptr;
    if (objectCreateWithPid(&weapon, loadout.weaponPid) == -1 || weapon == nullptr) {
        fprintf(stderr, "f2_server: stress could not create weapon pid 0x%X\n", loadout.weaponPid);
        return;
    }
    // Off the world list before it goes into an inventory — single membership is a teardown
    // double-free invariant, the same rule the item paths elsewhere obey.
    _obj_disconnect(weapon, nullptr);
    if (itemAdd(critter, weapon, 1) == -1) {
        objectDestroy(weapon, nullptr);
        return;
    }

    if (loadout.ammoPid >= 0 && loadout.ammoCount > 0) {
        Object* ammo = nullptr;
        if (objectCreateWithPid(&ammo, loadout.ammoPid) == 0 && ammo != nullptr) {
            _obj_disconnect(ammo, nullptr);
            if (itemAdd(critter, ammo, loadout.ammoCount) == -1) {
                objectDestroy(ammo, nullptr);
            }
        }
    }

    // Right hand, no draw animation: these are being placed, not acting, and there is no
    // reg_anim context here to run a take-out through.
    int rc = _invenWieldFunc(critter, weapon, HAND_RIGHT, false);
    // Say what each hostile got. A stress run is a diagnostic, so "which of them had the
    // launcher" is exactly the question you ask afterwards — and it is the only way to tell
    // OUR loadouts apart from the weapons the map's own critters already carry.
    if (rc == 0) {
        fprintf(stderr, "f2_server: stress armed net=%d with pid=0x%X (ammo 0x%X x%d)\n",
            critter->netId, loadout.weaponPid, loadout.ammoPid, loadout.ammoCount);
    }
    if (rc != 0) {
        fprintf(stderr, "f2_server: stress wield pid 0x%X on net=%d FAILED rc=%d"
                        " (it will fight unarmed)\n",
            loadout.weaponPid, critter->netId, rc);
    }
}

// objectCreateWithPid + objectSetLocation IS the whole recipe (mirrors the
// script engine's op_create_object_sid): the proto hands the critter its HP,
// AP, team, AI packet and default script, and under the serve loop the create
// path assigns a netId and announces the SPAWN to every viewer by itself.
static Object* stressSpawnOne(int pid, int tile, int elevation)
{
    Object* obj = nullptr;
    if (objectCreateWithPid(&obj, pid) == -1 || obj == nullptr) {
        return nullptr;
    }

    Rect rect;
    if (objectSetLocation(obj, tile, elevation, &rect) == -1) {
        objectDestroy(obj, nullptr);
        return nullptr;
    }
    presenter()->worldInvalidateRect(&rect, elevation);

    gStressSpawned.push_back(StressSpawnRecord { obj, obj->id, obj->pid });
    return obj;
}

// True iff this record's pointer still names the object it was recorded for:
// present in the world walk AND carrying the same id+pid.
static bool stressRecordAlive(const StressSpawnRecord& record)
{
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        if (obj == record.obj) {
            return obj->id == record.id && obj->pid == record.pid;
        }
    }
    return false;
}

void serverAdminWriteSlotListing(const std::function<void(const char* text)>& reply)
{
    char line[256];
    int occupied = 0;

    reply("slot  label                           character         game date     map");
    reply("----  ------------------------------  ----------------  ------------  ----");

    for (int slot = 0; slot < kAdminSlotCount; slot++) {
        LoadSaveSlotData data;
        if (!readSlotHeader(slot, data)) {
            continue;
        }
        occupied++;

        // The header's char arrays are fixed-width and not guaranteed to be
        // terminated; bound every print with an explicit precision.
        snprintf(line, sizeof(line), "%4d  %-30.30s  %-16.16s  %02d-%02d-%04d   %d",
            slot + 1,
            data.description,
            data.characterName,
            data.gameDay, data.gameMonth, data.gameYear,
            data.map);
        reply(line);
    }

    if (occupied == 0) {
        reply("(no saves)");
    }
}

// Channel names for the operator, indexed by wire value (msg_channel.h). Kept in
// lockstep with the enum by the static_assert below rather than by vigilance.
static const char* const channelNames[] = {
    "default",
    "combat",
    "refusal",
    "system",
    "chat",
    "reward",
};
static_assert(sizeof(channelNames) / sizeof(channelNames[0]) == kMsgChannelCount,
    "channelNames must name every MessageChannel");

// Channel by name, or -1. Names only — a bare number would let an operator emit a
// channel no viewer has a style for, which renders as default and looks like a bug.
static int parseChannel(const char* name)
{
    for (int i = 0; i < kMsgChannelCount; i++) {
        if (compat_stricmp(name, channelNames[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void writeChannelListing(const std::function<void(const char* text)>& reply)
{
    char line[256];
    int used = snprintf(line, sizeof(line), "channels:");
    for (int i = 0; i < kMsgChannelCount && used < (int)sizeof(line) - 1; i++) {
        used += snprintf(line + used, sizeof(line) - used, " %s", channelNames[i]);
    }
    reply(line);
}

static void writeHelp(const std::function<void(const char* text)>& reply, bool worldLoaded)
{
    reply("admin verbs:");
    reply("  saves                 list save slots");
    reply("  save <1-10> [label]   save the running world into a slot");
    reply("  load <1-10>           restore a slot          (lobby only)");
    reply("  new <map.map>         boot a fresh world      (lobby only)");
    reply("  status                what is running right now");
    reply("  say <chan> <text>     push a styled line to every message log");
    reply("  saydemo               one line per channel (style eyeball test)");
    reply("  movie <0-16>          project a movie to every viewer (4 = VSUIT)");
    reply("  timeskip <minutes>    advance the game clock like a script does");
    reply("  spawn <pid> [n] [tile]  place n critters of pid (default 1, random tile)");
    reply("  stress <n> [pid] [seed] spawn n hostiles near the players and aggro them");
    reply("  despawnall            destroy everything spawn/stress created");
    reply("  revive <slot>         revive a dead player at 1 HP (no-op if not dead)");
    reply("  xp <slot> <amount>    award experience to one seat (levels come with it)");
    reply("  sheet [slot]          level/xp/unspent points/owed perk/tags/traits per seat");
    reply("  rest <minutes> [slot] pass time for EVERYONE and heal every player");
    reply("  sp <slot> <points>    set a seat's UNSPENT skill points (the level-up currency)");
    reply("  skillup <slot> <skil> spend ONE point in one skill, exactly as a client asks");
    reply("  skilldown <slot> <sk> take that point back (only to where the sheet opened)");
    reply("  perkpick <slot> <prk> take one perk (needs an owed pick)");
    reply("  encnext            arm ONE detected random encounter on the next travel check");
    reply("  crithit <slot>        arm a critical HIT on that seat's next attack (one shot)");
    reply("  critfail <slot> [0-4] arm a critical FAILURE (0=mildest 4=weapon explodes; one shot)");
    reply("  fixcar                warp the car trunk back onto the car (realtime)");
    reply("  fixcar list | park [n]  list areas | re-park the car (writes saved state)");
    reply("  help                  this");
    writeChannelListing(reply);
    reply(worldLoaded
            ? "world: LOADED — debug verbs (walk/aggro/...) are dispatched too"
            : "world: LOBBY — no world yet; load or new to start one");
}

bool serverAdminLine(const char* line,
    const std::function<void(const char* text)>& reply,
    bool worldLoaded)
{
    if (line == nullptr) {
        return false;
    }

    char verb[32];
    const char* rest = splitVerb(line, verb, sizeof(verb));
    if (verb[0] == '\0') {
        return false;
    }

    char msg[512];

    if (strcmp(verb, "help") == 0 || strcmp(verb, "?") == 0) {
        writeHelp(reply, worldLoaded);
        return true;
    }

    if (strcmp(verb, "status") == 0) {
        reply(worldLoaded ? "world: LOADED" : "world: LOBBY (nothing loaded)");
        return true;
    }

    if (strcmp(verb, "saves") == 0) {
        serverAdminWriteSlotListing(reply);
        return true;
    }

    if (strcmp(verb, "say") == 0) {
        // `say <channel> <text>` — push one styled line into every viewer's message
        // log. An admin verb rather than a debug one for the reason the whole file
        // exists: `Command` is int-only and this takes a sentence.
        //
        // Broadcast (netId 0) deliberately: an operator announcement is for the
        // room, and there is no session→actor mapping at this layer anyway.
        if (!worldLoaded) {
            reply("say: no world loaded — nobody is listening");
            return true;
        }
        if (rest == nullptr) {
            reply("usage: say <channel> <text>");
            writeChannelListing(reply);
            return true;
        }

        char channelName[32];
        const char* text = splitVerb(rest, channelName, sizeof(channelName));
        int channel = parseChannel(channelName);
        if (channel < 0) {
            snprintf(msg, sizeof(msg), "say: unknown channel '%s'", channelName);
            reply(msg);
            writeChannelListing(reply);
            return true;
        }
        if (text == nullptr) {
            reply("usage: say <channel> <text>");
            return true;
        }

        presenter()->consoleMessageStyled(0, channel, text);
        snprintf(msg, sizeof(msg), "say: sent on %s", channelName);
        reply(msg);
        return true;
    }

    if (strcmp(verb, "movie") == 0) {
        // `movie <0-16>` — project a movie to every viewer and park the tick in the
        // barrier until one of them acks (game_movie.h). The point of the verb is
        // that the real triggers are buried in scripts a play-through away
        // (MOVIE_VSUIT wants the whole Temple), so the sync behaviour is otherwise
        // untestable without an hour of play.
        //
        // ⚠ This bypasses the gameMovieIsSeen gate that the SCRIPT paths apply, so
        // it will happily replay something already seen. That is the useful
        // behaviour for a test verb and the wrong one for anything else.
        if (!worldLoaded) {
            reply("movie: no world loaded");
            return true;
        }
        if (rest == nullptr) {
            reply("usage: movie <0-16>   (4 = VSUIT, the post-Temple one)");
            return true;
        }

        int movie = atoi(rest);
        if (movie < 0 || movie >= MOVIE_COUNT) {
            snprintf(msg, sizeof(msg), "movie: %d out of range (0..%d)", movie, MOVIE_COUNT - 1);
            reply(msg);
            return true;
        }

        snprintf(msg, sizeof(msg), "movie: playing %d — the tick is parked until a viewer acks", movie);
        reply(msg);
        gameMoviePlay(movie, GAME_MOVIE_FADE_IN | GAME_MOVIE_FADE_OUT | GAME_MOVIE_PAUSE_MUSIC);
        reply("movie: barrier released");
        return true;
    }

    if (strcmp(verb, "timeskip") == 0) {
        // `timeskip <minutes>` — do exactly what the game_time_advance opcode does
        // (add ticks, then drain the event queue so every NPC catches up in this
        // one beat). Same reason the `movie` verb exists: the real trigger is
        // buried behind an hour of play (Sheila's session in denbus2 wants $350
        // and a dialog walk), so the presentation behaviour is otherwise
        // untestable. This is the demo path for the time-skip move coalescing
        // documented in presenter.h.
        if (!worldLoaded) {
            reply("timeskip: no world loaded");
            return true;
        }
        if (rest == nullptr) {
            reply("usage: timeskip <minutes>   (50 = the denbus2/Sheila skip)");
            return true;
        }

        int minutes = atoi(rest);
        if (minutes <= 0 || minutes > 60 * 24 * 30) {
            reply("timeskip: minutes out of range (1..43200)");
            return true;
        }

        // GAME_TIME_TICKS_PER_MINUTE is 600; opGameTimeAdvance's own day loop is
        // not reproduced here because the queue drain is what matters and one
        // drain covers the whole span for presentation purposes.
        snprintf(msg, sizeof(msg), "timeskip: advancing %d minute(s) — the world catches up in one beat", minutes);
        reply(msg);

        presenterTimeSkipBegin();
        gameTimeAddTicks(600 * minutes);
        queueProcessEvents();
        presenterTimeSkipEnd();

        snprintf(msg, sizeof(msg), "timeskip: done, gametime=%u", gameTimeGetTime());
        reply(msg);
        return true;
    }

    if (strcmp(verb, "fixcar") == 0) {
        // ─── `fixcar` — put the Highwayman and its trunk back together ───────────
        //
        //   fixcar                REPORT, then warp the trunk onto the car. Realtime.
        //   fixcar list           list area indices (report only)
        //   fixcar park [areaIdx] change where the car is PARKED (touches saved state)
        //
        // ►► THE DEFAULT NEVER TOUCHES PARKING STATE. The first cut had it backwards:
        // bare `fixcar` re-parked the car and cleared GVAR_CAR_PLACED_TILE, which
        // MUTATES a save whose car was fine and can make a working car vanish (the
        // placement scripts own that gvar; clearing it under them is not a read-only
        // act). Re-parking is now opt-in via `fixcar park`, and the default does the
        // thing that was actually wanted: move the trunk to the car, right now, with no
        // map re-entry, no city transition, no script involvement.
        if (!worldLoaded) {
            reply("fixcar: no world loaded");
            return true;
        }

        bool doList = rest != nullptr && strncmp(rest, "list", 4) == 0;
        bool doPark = rest != nullptr && strncmp(rest, "park", 4) == 0;

        int carArea = wmCarCurrentArea();
        char carAreaName[80];
        carAreaName[0] = '\0';
        if (carArea >= 0 && carArea < wmAreaCount()) {
            wmGetAreaIdxName(carArea, carAreaName);
        }
        bool gotCar = gameGetGlobalVar(GVAR_PLAYER_GOT_CAR) != 0;
        snprintf(msg, sizeof(msg), "fixcar: have-car=%s, parked-area=%d (%s), gas=%d, placed-tile=%d",
            gotCar ? "yes" : "NO", carArea,
            carAreaName[0] != '\0' ? carAreaName : "none/invalid", wmCarGasAmount(),
            gameGetGlobalVar(GVAR_CAR_PLACED_TILE));
        reply(msg);

        int partyArea = -1;
        char partyAreaName[80];
        partyAreaName[0] = '\0';
        if (wmGetPartyCurArea(&partyArea) != -1 && partyArea >= 0 && partyArea < wmAreaCount()) {
            wmGetAreaIdxName(partyArea, partyAreaName);
        }
        snprintf(msg, sizeof(msg), "fixcar: you are on map '%s' (idx %d), area %d (%s)",
            gMapHeader.name, mapGetCurrentMap(), partyArea,
            partyAreaName[0] != '\0' ? partyAreaName : "not over a known area");
        reply(msg);

        if (doList) {
            reply("fixcar: areas (K = known to the party, <== parked area):");
            for (int areaIdx = 0; areaIdx < wmAreaCount(); areaIdx++) {
                char name[80];
                name[0] = '\0';
                wmGetAreaIdxName(areaIdx, name);
                snprintf(msg, sizeof(msg), "  %3d %s %-28s%s", areaIdx,
                    wmAreaIsKnown(areaIdx) ? "K" : " ", name,
                    areaIdx == carArea ? " <==" : "");
                reply(msg);
            }
            return true;
        }

        // ─── find the car and the trunk on THIS map ──────────────────────────────
        // ►► Walk the object list directly instead of objectListCreate, for two
        // reasons. (1) objectListCreate SKIPS OBJECT_HIDDEN objects (object.cc:2693),
        // and a hidden car/trunk is exactly the state we are hunting — using it made
        // the report lie. (2) it returns 0 WITHOUT assigning its out-param
        // (object.cc:2714), so the obvious call-and-free shape frees an uninitialised
        // pointer on any empty elevation. That is what coredumped the first cut.
        Object* car = nullptr;
        for (int elev = 0; elev < ELEVATION_COUNT && car == nullptr; elev++) {
            for (Object* obj = objectFindFirstAtElevation(elev); obj != nullptr;
                 obj = objectFindNextAtElevation()) {
                if (obj->pid == PROTO_ID_CAR) {
                    car = obj;
                    break;
                }
            }
        }
        Object* trunk = partyMemberFindByPid(PROTO_ID_CAR_TRUNK);

        if (car != nullptr) {
            snprintf(msg, sizeof(msg), "fixcar: car object at tile=%d elev=%d (%s) netId=%d fid=0x%x frame=%d",
                car->tile, car->elevation,
                (car->flags & OBJECT_HIDDEN) != 0 ? "HIDDEN" : "visible",
                car->netId, car->fid, car->frame);
        } else {
            snprintf(msg, sizeof(msg), "fixcar: no car object on this map at all");
        }
        reply(msg);

        if (trunk != nullptr) {
            // ►► netId is the load-bearing field for the owner's symptom. The car is
            // replicated (a viewer interacts with it by netId), so if the TRUNK has no
            // netId it exists only on the server and no client can ever see or click it —
            // which looks exactly like "the car has no trunk" no matter where it sits.
            // fid/frame are here for the other candidate: a frame index past the art's
            // frame count renders NOTHING even when the object is present and visible.
            snprintf(msg, sizeof(msg), "fixcar: trunk id=%d netId=%d at tile=%d elev=%d (%s) "
                                       "fid=0x%x frame=%d flags=0x%x — contents intact",
                trunk->id, trunk->netId, trunk->tile, trunk->elevation,
                (trunk->flags & OBJECT_HIDDEN) != 0 ? "HIDDEN" : "visible",
                trunk->fid, trunk->frame, trunk->flags);
            reply(msg);
            if (trunk->netId == 0) {
                reply("fixcar: ►► THE TRUNK HAS NO netId — it is server-only, so NO CLIENT can "
                      "see or click it. That is the bug, and moving it will not help.");
            }
        } else if (gotCar) {
            reply("fixcar: ⚠ NO TRUNK in the party roster — already dropped by a previous "
                  "save/load, and its contents are not in this save to recover. Only an older "
                  "save has them.");
        } else {
            reply("fixcar: no trunk, and have-car is NO — this game simply has no car yet.");
        }

        // ─── `fixcar park [areaIdx]` — the only path that writes parking state ───
        if (doPark) {
            if (!gotCar) {
                reply("fixcar: have-car is NO — refusing to park a car you do not own");
                return true;
            }
            const char* arg = skipBlanks(rest + 4);
            int target;
            if (arg != nullptr) {
                target = atoi(arg);
            } else if (partyArea >= 0) {
                target = partyArea;
            } else {
                reply("fixcar: not over a known area — `fixcar park <areaIdx>` (see `fixcar list`)");
                return true;
            }
            if (target < 0 || target >= wmAreaCount()) {
                snprintf(msg, sizeof(msg), "fixcar: area %d out of range (0..%d)", target, wmAreaCount() - 1);
                reply(msg);
                return true;
            }
            wmCarSetCurrentArea(target);
            wmCarClearPlacedTile();
            char targetName[80];
            targetName[0] = '\0';
            wmGetAreaIdxName(target, targetName);
            snprintf(msg, sizeof(msg), "fixcar: parked at area %d (%s), placed-tile cleared. "
                                       "⚠ the town script re-places the car on next map entry",
                target, targetName[0] != '\0' ? targetName : "unnamed");
            reply(msg);
            return true;
        }

        // ─── DEFAULT: reunite them, in place, right now ──────────────────────────
        if (trunk == nullptr) {
            reply("fixcar: nothing to reunite — the trunk object is gone.");
            return true;
        }

        // A hidden car is itself a "my car vanished" cause, and it is recoverable: the
        // object is still here, just not drawn. Reveal it before placing the trunk.
        if (car != nullptr && (car->flags & OBJECT_HIDDEN) != 0) {
            objectShow(car, nullptr);
            reply("fixcar: the car object was HIDDEN — revealed it");
        }

        int destTile = car != nullptr ? car->tile : (gDude != nullptr ? gDude->tile : -1);
        int destElev = car != nullptr ? car->elevation : (gDude != nullptr ? gDude->elevation : 0);
        if (destTile == -1) {
            reply("fixcar: no car on this map and no dude to place it next to — nowhere to put it");
            return true;
        }

        if ((trunk->flags & OBJECT_HIDDEN) != 0) {
            objectShow(trunk, nullptr);
        }

        // ►► THE ACTUAL REPAIR for an already-damaged save. cartrunk.frm has ONE frame,
        // and vanilla's _obj_use_container sets frame=1 on open without checking — an
        // out-of-range frame renders NOTHING, so the trunk goes permanently invisible and
        // the save carries it forever. The engine fix stops it happening again; this puts
        // an already-broken trunk back on a frame its art actually has.
        int trunkFrameCount = 1;
        CacheEntry* trunkArtHandle;
        Art* trunkArt = artLock(trunk->fid, &trunkArtHandle);
        if (trunkArt != nullptr) {
            trunkFrameCount = artGetFrameCount(trunkArt);
            artUnlock(trunkArtHandle);
        }
        if (trunk->frame >= trunkFrameCount) {
            snprintf(msg, sizeof(msg), "fixcar: ►► trunk frame=%d but its art has only %d frame(s) "
                                       "— THAT is why it renders nothing. Reset to 0.",
                trunk->frame, trunkFrameCount);
            reply(msg);
            trunk->frame = 0;
        }

        // ►► DO NOT use _objPMAttemptPlacement here. When the requested tile fails
        // wmEvalTileNumForPlacement it throws the request away and spirals out from
        // gDude->tile (proto_instance.cc:2404) — so asking it for "the car's tile" put the
        // trunk next to the PLAYER, several rows from the car. Walk the car's own
        // neighbours instead, nearest ring first, and settle on the car's tile itself if
        // every neighbour is blocked (an item sharing the car's tile is still clickable).
        int placed = destTile;
        if (car != nullptr) {
            bool found = false;
            for (int ring = 1; ring <= 2 && !found; ring++) {
                for (int dir = 0; dir < ROTATION_COUNT; dir++) {
                    int candidate = tileGetTileInDirection(car->tile, dir, ring);
                    if (candidate >= 0 && wmEvalTileNumForPlacement(candidate)) {
                        placed = candidate;
                        found = true;
                        break;
                    }
                }
            }
        }
        objectSetLocation(trunk, placed, destElev, nullptr);

        // ►► RE-ANNOUNCE THE WHOLE OBJECT. The delta scan is a diff, so a hand repair
        // reaches only clients that already hold the object AND whose shadow disagrees.
        // Owner-observed: after the frame repair the trunk stayed invisible until they
        // rejoined THREE times — sessions 3/4 saw it (their join blob carried frame=0)
        // while sessions 1/2, connected throughout, never got the change. Forcing a full
        // delta converges an already-connected client without a rejoin.
        objectDeltaForgetShadow(trunk);
        if (car != nullptr) {
            objectDeltaForgetShadow(car);
        }

        snprintf(msg, sizeof(msg), "fixcar: trunk warped to tile=%d elev=%d (%s) — no map "
                                   "re-entry needed, it is there now",
            trunk->tile, trunk->elevation,
            car != nullptr ? "beside the car" : "beside you; no car object on this map");
        reply(msg);
        return true;
    }


    if (strcmp(verb, "revive") == 0) {
        // `revive <slot>` — bring a dead PLAYER actor back at 1 HP (owner spec
        // 2026-07-23). Players only: the arg is a registry slot, so this can never
        // target a random NPC corpse. A no-op with a clear reply when the slot is
        // empty or the player is already alive ("does nothing if not dead").
        if (!worldLoaded) {
            reply("revive: no world loaded");
            return true;
        }
        if (rest == nullptr) {
            reply("usage: revive <slot>   (0 = host, 1.. = the extras)");
            return true;
        }

        int slot = atoi(rest);
        if (slot < 0 || slot >= playerActorCount()) {
            snprintf(msg, sizeof(msg), "revive: slot %d out of range (0..%d)", slot, playerActorCount() - 1);
            reply(msg);
            return true;
        }

        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            snprintf(msg, sizeof(msg), "revive: slot %d is empty", slot);
            reply(msg);
            return true;
        }
        if (!critterIsDead(actor)) {
            snprintf(msg, sizeof(msg), "revive: slot %d (%s) is not dead — nothing to do", slot, critterGetName(actor));
            reply(msg);
            return true;
        }

        critterRevive(actor);
        snprintf(msg, sizeof(msg), "revive: slot %d (%s) back at 1 HP", slot, critterGetName(actor));
        reply(msg);
        char line[128];
        snprintf(line, sizeof(line), "%s has been revived.", critterGetName(actor));
        presenter()->consoleMessageStyled(0, kMsgChannelSystem, line);
        return true;
    }

    if (strcmp(verb, "gvar") == 0) {
        // `gvar <index> [value]` — read or set one global script variable. The
        // repair tool for a world whose quest flags went wrong (indices are the
        // GVAR_* numbers in data/vault13.gam; quests.txt maps quests to them).
        if (!worldLoaded) {
            reply("gvar: no world loaded");
            return true;
        }
        int index = -1;
        int value = 0;
        int n = rest != nullptr ? sscanf(rest, "%d %d", &index, &value) : 0;
        if (n < 1 || index < 0 || index >= gGameGlobalVarsLength) {
            snprintf(msg, sizeof(msg), "usage: gvar <index 0-%d> [value]", gGameGlobalVarsLength - 1);
            reply(msg);
            return true;
        }
        if (n >= 2) {
            gameSetGlobalVar(index, value);
            fprintf(stderr, "f2_server: admin gvar %d set to %d\n", index, value);
        }
        snprintf(msg, sizeof(msg), "gvar %d = %d", index, gameGetGlobalVar(index));
        reply(msg);
        return true;
    }
    if (strcmp(verb, "xp") == 0) {
        // `xp <slot> <amount>` — award experience to ONE seat. Slot-addressed for the same
        // reason revive is: the arg is a registry slot, so this can never land on a random
        // NPC, and "which player" is unambiguous at N>1.
        //
        // ►► LEVELS COME FOR FREE, and correctly. pcAddExperience runs vanilla's own
        // award LOOP (stat.cc: `while (level < PC_LEVEL_MAX)` with a
        // pcGetExperienceForNextLevel check), so one grant that crosses several thresholds
        // levels several times, each awarding HP (endurance/2+2, +Lifegiver) and healing the
        // difference. Nothing here reimplements any of that — the verb only chooses the
        // earner.
        //
        // Per-actor by construction: pcAddExperience takes a SUBJECT and indexes that
        // actor's own pc-stat row, so an extra's XP lands in the extra's row (and the new
        // row is streamed, so their client stops showing seeded XP).
        if (!worldLoaded) {
            reply("xp: no world loaded");
            return true;
        }
        char slotText[32];
        const char* amountText = splitVerb(rest != nullptr ? rest : "", slotText, sizeof(slotText));
        if (slotText[0] == '\0' || amountText == nullptr || amountText[0] == '\0') {
            reply("usage: xp <slot> <amount>   (0 = host, 1.. = the extras; amount may be negative)");
            return true;
        }

        int slot = atoi(slotText);
        if (slot < 0 || slot >= playerActorCount()) {
            snprintf(msg, sizeof(msg), "xp: slot %d out of range (0..%d)", slot, playerActorCount() - 1);
            reply(msg);
            return true;
        }
        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            snprintf(msg, sizeof(msg), "xp: slot %d is empty", slot);
            reply(msg);
            return true;
        }

        int amount = (int)strtol(amountText, nullptr, 0);
        if (amount == 0) {
            reply("xp: amount 0 — nothing to award");
            return true;
        }

        // Report the LEVEL either side of the award, because the levels are the interesting
        // part of the answer and pcAddExperience only hands back the XP delta.
        int levelBefore = pcGetStat(PC_STAT_LEVEL, actor);
        int gained = 0;
        pcAddExperience(amount, &gained, actor);
        int levelAfter = pcGetStat(PC_STAT_LEVEL, actor);
        int xpNow = pcGetStat(PC_STAT_EXPERIENCE, actor);

        if (levelAfter != levelBefore) {
            snprintf(msg, sizeof(msg),
                "xp: slot %d (%s) +%d xp (now %d) — LEVEL %d -> %d",
                slot, critterGetName(actor), gained, xpNow, levelBefore, levelAfter);
        } else {
            snprintf(msg, sizeof(msg),
                "xp: slot %d (%s) +%d xp (now %d) — still level %d, next at %d",
                slot, critterGetName(actor), gained, xpNow, levelAfter,
                pcGetExperienceForNextLevel(actor));
        }
        reply(msg);
        fprintf(stderr, "f2_server: admin xp slot=%d amount=%d gained=%d level=%d->%d\n",
            slot, amount, gained, levelBefore, levelAfter);
        return true;
    }

    if (strcmp(verb, "encnext") == 0) {
        // TEST HOOK: arm the next worldmap travel check to roll an encounter AND to
        // detect it, so the accept/decline prompt is reachable on demand instead of
        // behind two dice (the frequency roll, then Outdoorsman). One-shot — the check
        // it arms consumes it — so it cannot leave the sim biased if you forget it.
        // Travel afterwards: the ordinary movement and rate-limit guards still apply,
        // it does not fire on a party standing still.
        worldmapForceNextEncounter();
        reply("encnext: next travel encounter check will fire AND be detected (one shot)");
        fprintf(stderr, "f2_server: admin encnext armed\n");
        return true;
    }

    if (strcmp(verb, "crithit") == 0 || strcmp(verb, "critfail") == 0) {
        // TEST HOOK: arm ONE seat's next attack roll to be a critical hit / critical
        // failure, so critical EFFECTS (crippled limbs, blindness, knockdown, dropped or
        // exploded weapon) are reachable on demand instead of behind two dice — the
        // accuracy roll, then STAT_CRITICAL_CHANCE. Twin of `encnext`, and forced at the
        // ROLL for the same reason: the crit tables, the effect flags and the narration
        // all run untouched, so a repro exercises the real path and not a staged result.
        // One-shot — the seat's next attack consumes it.
        bool wantHit = strcmp(verb, "crithit") == 0;
        if (!worldLoaded) {
            snprintf(msg, sizeof(msg), "%s: no world loaded", verb);
            reply(msg);
            return true;
        }
        if (rest == nullptr) {
            snprintf(msg, sizeof(msg), "usage: %s <slot>%s   (0 = host, 1.. = the extras)",
                verb, wantHit ? "" : " [0-4]");
            reply(msg);
            return true;
        }

        int slot = atoi(rest);
        if (slot < 0 || slot >= playerActorCount()) {
            snprintf(msg, sizeof(msg), "%s: slot %d out of range (0..%d)", verb, slot,
                playerActorCount() - 1);
            reply(msg);
            return true;
        }
        if (playerActorAt(slot) == nullptr) {
            snprintf(msg, sizeof(msg), "%s: slot %d is empty", verb, slot);
            reply(msg);
            return true;
        }

        // Optional second arg for critfail: pin the severity column, 0 (mildest) .. 4 (worst,
        // the weapon-explosion row). Absent = leave it to the dice. Meaningless for crithit.
        int severity = -1;
        if (!wantHit) {
            const char* second = strchr(rest, ' ');
            if (second != nullptr) {
                severity = atoi(second + 1);
                if (severity < 0 || severity > 4) {
                    snprintf(msg, sizeof(msg),
                        "critfail: severity %d out of range (0 = mildest .. 4 = worst/explosion)",
                        severity);
                    reply(msg);
                    return true;
                }
            }
        }

        serverActorSetForceCrit(slot, wantHit ? kForceCritHit : kForceCritFail, severity);
        if (wantHit) {
            snprintf(msg, sizeof(msg), "crithit: slot %d's next attack rolls a critical HIT (one shot)", slot);
        } else if (severity >= 0) {
            snprintf(msg, sizeof(msg),
                "critfail: slot %d's next attack rolls a critical FAILURE, severity %d (one shot)",
                slot, severity);
        } else {
            snprintf(msg, sizeof(msg),
                "critfail: slot %d's next attack rolls a critical FAILURE, severity by luck (one shot)",
                slot);
        }
        reply(msg);
        fprintf(stderr, "f2_server: admin %s armed slot=%d severity=%d\n", verb, slot, severity);
        return true;
    }

    if (strcmp(verb, "skillup") == 0 || strcmp(verb, "skilldown") == 0
        || strcmp(verb, "perkpick") == 0 || strcmp(verb, "sp") == 0) {
        // The character-sheet edit intents, from the OPERATOR's side — the same three
        // rulings a client's verb goes through (server_control.cc), addressed by slot
        // instead of by session. This is how the sheet path is exercised without two
        // machines and a level-up: `sp 1 20` funds slot 1, `skillup 1 12` spends a
        // point in it, `perkpick 1 …` takes a perk once one is owed.
        //
        // `sp` is the ONLY one of the three that is a cheat rather than a player
        // action: it hands out the level-up currency directly. skillup/perkpick are
        // exactly what the player's own client sends, refusals included — an operator
        // cannot spend points a seat has not earned, which is the point of testing
        // through them.
        if (!worldLoaded) {
            snprintf(msg, sizeof(msg), "%s: no world loaded", verb);
            reply(msg);
            return true;
        }

        char slotText[32];
        const char* argText = splitVerb(rest != nullptr ? rest : "", slotText, sizeof(slotText));
        if (slotText[0] == '\0' || argText == nullptr || argText[0] == '\0') {
            if (strcmp(verb, "sp") == 0) {
                reply("usage: sp <slot> <points>   (set that seat's UNSPENT skill points; 0 = host)");
            } else if (strcmp(verb, "skillup") == 0 || strcmp(verb, "skilldown") == 0) {
                snprintf(msg, sizeof(msg),
                    "usage: %s <slot> <skillId>   (0..17, ONE point; down only back to where the sheet opened)",
                    verb);
                reply(msg);
            } else {
                reply("usage: perkpick <slot> <perkId>   (requires an owed pick — `sheet <slot>` lists state)");
            }
            return true;
        }

        int slot = atoi(slotText);
        if (slot < 0 || slot >= playerActorCount()) {
            snprintf(msg, sizeof(msg), "%s: slot %d out of range (0..%d)",
                verb, slot, playerActorCount() - 1);
            reply(msg);
            return true;
        }
        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            snprintf(msg, sizeof(msg), "%s: slot %d is empty", verb, slot);
            reply(msg);
            return true;
        }

        int value = (int)strtol(argText, nullptr, 0);

        if (strcmp(verb, "sp") == 0) {
            // Subject-addressed: pcSetStat range-checks against the stat's own
            // min/max and streams the row, so a bad number is refused, not clamped
            // silently.
            if (pcSetStat(PC_STAT_UNSPENT_SKILL_POINTS, value, actor) != 0) {
                snprintf(msg, sizeof(msg), "sp: %d rejected (out of range for unspent skill points)", value);
                reply(msg);
                return true;
            }
            snprintf(msg, sizeof(msg), "sp: slot %d (%s) unspent skill points = %d",
                slot, critterGetName(actor), pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS, actor));
            reply(msg);
            fprintf(stderr, "f2_server: admin sp slot=%d points=%d\n", slot, value);
            return true;
        }

        // An operator driving a seat has no character screen to bracket the edits, so
        // make sure a session exists — otherwise `skilldown` would have no baseline to
        // measure against. ENSURE, not open: re-snapshotting before every verb would
        // move the floor up to whatever was just spent, and every refund would be
        // refused as "already at the baseline".
        sheetEditSessionEnsure(actor);

        if (strcmp(verb, "skillup") == 0 || strcmp(verb, "skilldown") == 0) {
            bool up = strcmp(verb, "skillup") == 0;
            int rc = up ? sheetEditSkillUp(actor, value) : sheetEditSkillDown(actor, value);
            snprintf(msg, sizeof(msg), "%s: slot %d (%s) skill %d -> %s (value %d, %d points left)",
                verb, slot, critterGetName(actor), value, sheetEditReason(rc),
                skillIsValid(value) ? skillGetValue(actor, value) : -1,
                pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS, actor));
            reply(msg);
            fprintf(stderr, "f2_server: admin %s slot=%d skill=%d rc=%d\n", verb, slot, value, rc);
            return true;
        }

        int pendingChoice = PERK_CHOICE_PENDING_NONE;
        int rc = sheetEditPerkPick(actor, value, &pendingChoice);
        const char* pendingText = pendingChoice == PERK_CHOICE_PENDING_TAG
            ? " — owes a 4th TAG skill (tagpick)"
            : pendingChoice == PERK_CHOICE_PENDING_MUTATE ? " — owes a trait swap (mutpick)" : "";
        snprintf(msg, sizeof(msg), "perkpick: slot %d (%s) perk %d -> %s%s",
            slot, critterGetName(actor), value, sheetEditReason(rc), pendingText);
        reply(msg);
        fprintf(stderr, "f2_server: admin perkpick slot=%d perk=%d rc=%d pending=%d\n",
            slot, value, rc, pendingChoice);
        return true;
    }

    if (strcmp(verb, "rest") == 0) {
        // `rest <minutes> [slot]` — the operator's side of the rest verb, and the only
        // way to OBSERVE a rest on a dedicated server: the debug `rest` verb reports
        // through debugPrint, which f2_server drops on the floor.
        //
        // Slot-addressed only for the scope (whose ServerActorScope the sim runs
        // under); the effect is the whole world's, because there is one clock. The
        // reply reports the clock and the resting actor's HP either side, which is what
        // makes "did anything actually happen" answerable.
        if (!worldLoaded) {
            reply("rest: no world loaded");
            return true;
        }
        char minutesText[32];
        const char* slotText = splitVerb(rest != nullptr ? rest : "", minutesText, sizeof(minutesText));
        if (minutesText[0] == '\0') {
            reply("usage: rest <minutes> [slot]   (advances the clock for EVERYONE and heals every player)");
            return true;
        }

        int minutes = atoi(minutesText);
        int slot = slotText != nullptr && slotText[0] != '\0' ? atoi(slotText) : 0;
        if (slot < 0 || slot >= playerActorCount()) {
            snprintf(msg, sizeof(msg), "rest: slot %d out of range (0..%d)", slot, playerActorCount() - 1);
            reply(msg);
            return true;
        }
        Object* actor = playerActorAt(slot);
        if (actor == nullptr) {
            snprintf(msg, sizeof(msg), "rest: slot %d is empty", slot);
            reply(msg);
            return true;
        }
        if (minutes <= 0) {
            reply("rest: minutes must be positive");
            return true;
        }
        if (isInCombat()) {
            reply("rest: not in combat");
            return true;
        }
        if (!_critter_can_obj_dude_rest()) {
            reply("rest: you cannot rest at this location (vanilla's own gate)");
            return true;
        }

        unsigned int before = gameTimeGetTime();
        int hpBefore = critterGetHitPoints(actor);
        restHealReset();

        RestOutcome outcome;
        {
            ServerActorScope restScope(actor);
            outcome = restPerform(minutes / 60, minutes % 60, 0, nullptr);
        }

        snprintf(msg, sizeof(msg),
            "rest: slot %d (%s) %d:%02d -> %s; clock %u -> %u (+%u ticks), hp %d -> %d",
            slot, critterGetName(actor), minutes / 60, minutes % 60,
            outcome == kRestCompleted ? "completed"
                : outcome == kRestInterrupted ? "INTERRUPTED by a queued event" : "aborted",
            before, gameTimeGetTime(), gameTimeGetTime() - before,
            hpBefore, critterGetHitPoints(actor));
        reply(msg);
        fprintf(stderr, "f2_server: admin rest slot=%d minutes=%d outcome=%d clock=%u->%u\n",
            slot, minutes, (int)outcome, before, gameTimeGetTime());
        return true;
    }

    if (strcmp(verb, "sheet") == 0) {
        // `sheet [slot]` — what a seat is holding: level, XP, unspent points, an owed
        // perk, tags, traits. The read-side companion to the verbs above, and the
        // fastest way to answer "did that spend land in the RIGHT row" at N>1, which
        // is the failure mode per-actor sheets exist to prevent.
        if (!worldLoaded) {
            reply("sheet: no world loaded");
            return true;
        }
        int only = rest != nullptr && rest[0] != '\0' ? atoi(rest) : -1;
        for (int slot = 0; slot < playerActorCount(); slot++) {
            if (only >= 0 && slot != only) {
                continue;
            }
            Object* actor = playerActorAt(slot);
            if (actor == nullptr) {
                continue;
            }
            int tagged[NUM_TAGGED_SKILLS];
            skillsGetTagged(tagged, NUM_TAGGED_SKILLS, actor);
            int trait1;
            int trait2;
            traitsGetSelected(&trait1, &trait2, actor);
            snprintf(msg, sizeof(msg),
                "sheet %d (%s): level %d, xp %d, unspent %d, owed perk %s (x%d), tags %d/%d/%d/%d, traits %d/%d",
                slot, critterGetName(actor),
                pcGetStat(PC_STAT_LEVEL, actor), pcGetStat(PC_STAT_EXPERIENCE, actor),
                pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS, actor),
                // ►► THE YES/no WORD IS A GATE CONTRACT (scripts/check_sheet.sh greps
                // "owed perk YES" / "owed perk no"). The count is a multi-level award
                // can owe several — so it is APPENDED after the greppable phrase, never
                // substituted for it.
                perkOwedPickGet(actor) ? "YES" : "no", perkOwedPickCount(actor),
                tagged[0], tagged[1], tagged[2], tagged[3], trait1, trait2);
            reply(msg);
        }
        return true;
    }

    if (strcmp(verb, "party") == 0) {
        // `party [skillId]` — what the PARTY-WIDE READERS answer right now, per seat.
        //
        // These four questions ("how good is the party at X", "how many of us are
        // there") are the ones that silently ignored extra players: they walk
        // gPartyMembers, which players are deliberately not in (party_member.cc has the
        // why). They set shop prices, encounter avoidance, encounter SIZE and the
        // "leave your friends outside" script gates — all things that read as bad luck
        // rather than as a bug, and none of which any single-actor golden can see.
        //
        // Defaults to BARTER because that is the one with a number a player can feel.
        if (!worldLoaded) {
            reply("party: no world loaded");
            return true;
        }
        int skill = rest != nullptr && rest[0] != '\0' ? atoi(rest) : SKILL_BARTER;
        if (!skillIsValid(skill)) {
            reply("party: that is not a skill id");
            return true;
        }
        // BOTH numbers, because the difference is load-bearing: script-count is what a
        // script may refuse you on (companions only, deliberately — players must never
        // be counted into a "leave your friends outside" gate), group-size is how many
        // of us are really here.
        snprintf(msg, sizeof(msg),
            "party script-count %d, group-size %d (players %d, skill %d group-best %d)",
            _getPartyMemberCount(), partyGroupSize(), playerActorCount(), skill,
            partyGetBestSkillValue(skill));
        reply(msg);
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* actor = playerActorAt(slot);
            if (actor == nullptr) {
                continue;
            }
            Object* best = partyMemberGetBestInSkill(skill, actor);
            snprintf(msg, sizeof(msg),
                "party slot %d (%s): own %d, solo-best %d, performer %s, online %s",
                slot, critterGetName(actor),
                skillGetValue(actor, skill),
                partyGetBestSkillValueFor(skill, actor),
                best != nullptr ? critterGetName(best) : "-",
                playerActorOnline(slot) ? "yes" : "no");
            reply(msg);
        }
        return true;
    }

    if (strcmp(verb, "saydemo") == 0) {
        // One line per channel, in wire order — the eyeball test for the style
        // table. Named separately from `say` so a typo in a channel name can never
        // silently flood the log with six lines.
        if (!worldLoaded) {
            reply("saydemo: no world loaded — nobody is listening");
            return true;
        }
        for (int channel = 0; channel < kMsgChannelCount; channel++) {
            snprintf(msg, sizeof(msg), "%s: the quick brown brahmin jumps over the lazy deathclaw",
                channelNames[channel]);
            presenter()->consoleMessageStyled(0, channel, msg);
        }
        reply("saydemo: sent one line per channel");
        return true;
    }

    if (strcmp(verb, "spawn") == 0) {
        // `spawn <pid> [n] [tile]` — place n critters of pid; tile -1/absent =
        // random near the players. pid takes 0x-hex or decimal (strtol base 0).
        if (!worldLoaded) {
            reply("spawn: no world loaded");
            return true;
        }

        char pidText[32];
        char nText[32];
        char tileText[32];
        const char* args = splitVerb(rest != nullptr ? rest : "", pidText, sizeof(pidText));
        args = splitVerb(args != nullptr ? args : "", nText, sizeof(nText));
        splitVerb(args != nullptr ? args : "", tileText, sizeof(tileText));

        int pid = static_cast<int>(strtol(pidText, nullptr, 0));
        int count = nText[0] != '\0' ? atoi(nText) : 1;
        int wantTile = tileText[0] != '\0' ? atoi(tileText) : -1;

        Proto* proto;
        if (PID_TYPE(pid) != OBJ_TYPE_CRITTER || protoGetProto(pid, &proto) == -1) {
            reply("spawn: want a valid critter pid (0x01000000 + critters.lst index)");
            return true;
        }
        if (count < 1 || count > 100) {
            reply("spawn: n out of range (1-100)");
            return true;
        }

        std::mt19937 rng(std::random_device {}());
        int placed = 0;
        int unreachable = 0; // only counted on the random path; an explicit tile is honored
        for (int i = 0; i < count; i++) {
            int tile = wantTile != -1 && hexGridTileIsValid(wantTile)
                ? wantTile
                : stressRandomFreeTile(gElevation, rng, unreachable);
            if (tile == -1) {
                break;
            }
            if (stressSpawnOne(pid, tile, gElevation) != nullptr) {
                placed++;
            }
        }

        snprintf(msg, sizeof(msg), "spawn: placed %d/%d of pid 0x%X (%zu tracked)",
            placed, count, pid, gStressSpawned.size());
        reply(msg);
        fprintf(stderr, "f2_server: admin spawn pid=0x%X placed=%d\n", pid, placed);
        return true;
    }

    if (strcmp(verb, "stress") == 0) {
        // `stress <n> [pid] [seed]` — n hostiles at seeded-random tiles near the
        // players, then force the fight through the same path as the `aggro`
        // debug verb. Reuse the seed it prints to replay a found desync.
        if (!worldLoaded) {
            reply("stress: no world loaded");
            return true;
        }

        char nText[32];
        char pidText[32];
        char seedText[32];
        const char* args = splitVerb(rest != nullptr ? rest : "", nText, sizeof(nText));
        args = splitVerb(args != nullptr ? args : "", pidText, sizeof(pidText));
        splitVerb(args != nullptr ? args : "", seedText, sizeof(seedText));

        int count = atoi(nText);
        if (count < 1 || count > 100) {
            reply("stress: want a count 1-100 (10-30 plays smooth; combat AI is O(n^2))");
            return true;
        }
        int pid = pidText[0] != '\0' ? static_cast<int>(strtol(pidText, nullptr, 0)) : kStressDefaultPid;
        unsigned seed = seedText[0] != '\0'
            ? static_cast<unsigned>(strtoul(seedText, nullptr, 0))
            : std::random_device {}();

        Proto* proto;
        if (PID_TYPE(pid) != OBJ_TYPE_CRITTER || protoGetProto(pid, &proto) == -1) {
            reply("stress: bad pid");
            return true;
        }

        std::mt19937 rng(seed);
        int placed = 0;
        int unreachable = 0; // candidates rejected because nothing could path to them
        for (int i = 0; i < count; i++) {
            int tile = stressRandomFreeTile(gElevation, rng, unreachable);
            if (tile == -1) {
                break;
            }
            Object* hostile = stressSpawnOne(pid, tile, gElevation);
            if (hostile != nullptr) {
                stressArmOne(hostile, rng);
                placed++;
            }
        }

        if (placed > 0) {
            probeApplyAggro(placed);
        }

        // Report the REJECTS. A short spawn count with no explanation reads as a broken
        // verb; "18 unreachable" says the map is tight around the party, which is the truth
        // and is actionable (move somewhere more open, or ask for fewer).
        if (unreachable > 0) {
            snprintf(msg, sizeof(msg),
                "stress: %d/%d hostiles up (pid 0x%X seed %u) — aggroed; %d candidate tiles"
                " rejected as UNREACHABLE (no player could path to them)",
                placed, count, pid, seed, unreachable);
        } else {
            snprintf(msg, sizeof(msg), "stress: %d/%d hostiles up (pid 0x%X seed %u) — aggroed",
                placed, count, pid, seed);
        }
        reply(msg);
        fprintf(stderr, "f2_server: admin stress placed=%d unreachable=%d seed=%u\n",
            placed, unreachable, seed);
        return true;
    }

    if (strcmp(verb, "despawnall") == 0) {
        // Destroy exactly what spawn/stress created. Mirrors the script
        // engine's own destroy recipe for critters (opDestroyObject):
        // _combat_delete_critter FIRST — objectDestroy does not touch the
        // combat roster, and a rostered dangling pointer is a crash — then
        // clear anims, then destroy. Ids are NOT reclaimed (map reload is the
        // id reset); records whose object no longer proves to be ours are
        // skipped, never guessed at.
        if (!worldLoaded) {
            reply("despawnall: no world loaded");
            return true;
        }

        int destroyed = 0;
        int skipped = 0;
        for (const StressSpawnRecord& record : gStressSpawned) {
            if (!stressRecordAlive(record)) {
                skipped++;
                continue;
            }
            if (PID_TYPE(record.obj->pid) == OBJ_TYPE_CRITTER) {
                _combat_delete_critter(record.obj);
            }
            reg_anim_clear(record.obj);
            objectDestroy(record.obj, nullptr);
            destroyed++;
        }
        gStressSpawned.clear();

        snprintf(msg, sizeof(msg), "despawnall: destroyed %d, skipped %d (already gone)",
            destroyed, skipped);
        reply(msg);
        fprintf(stderr, "f2_server: admin despawnall destroyed=%d skipped=%d\n", destroyed, skipped);
        return true;
    }

    if (strcmp(verb, "save") == 0) {
        if (!worldLoaded) {
            reply("save: no world loaded — nothing to save");
            return true;
        }

        char slotText[32];
        const char* label = splitVerb(rest != nullptr ? rest : "", slotText, sizeof(slotText));
        int slot = parseSlot(slotText);
        if (slot < 0) {
            snprintf(msg, sizeof(msg), "save: want a slot number 1-%d", kAdminSlotCount);
            reply(msg);
            return true;
        }

        // Same refusal window as the autosave: the save routine fails part-way
        // (and reports "error 0": none of its exits set an error code) when a
        // fight, a streamed dialog node, worldmap travel, a map change or a
        // movie is in flight. Say so instead of failing.
        if (isInCombat() || mapTransitionPending()
            || GameMode::isInGameMode(GameMode::kDialog | GameMode::kBarter | GameMode::kWorldmap)
            || gameDialogServerNodeActive() || gameMovieIsPlaying()) {
            reply("save: not now (combat, dialogue, worldmap travel or a map change is in progress) - try again in a moment");
            return true;
        }
        if (!adminWriteSave(slot, label)) {
            snprintf(msg, sizeof(msg), "save: FAILED writing slot %d (error %d) - see the LOADSAVE line above for the step that failed",
                slot + 1, savegameGetErrorCode());
            reply(msg);
            fprintf(stderr, "f2_server: admin save slot %d FAILED (error %d)\n",
                slot + 1, savegameGetErrorCode());
            return true;
        }

        snprintf(msg, sizeof(msg), "save: wrote slot %d%s%s", slot + 1,
            label != nullptr ? " — " : "", label != nullptr ? label : "");
        reply(msg);
        fprintf(stderr, "f2_server: admin save slot %d ok\n", slot + 1);
        // The operator answered over the control socket above; the PLAYERS get
        // the same courtesy the autosave gives (serverAutosaveTick).
        snprintf(msg, sizeof(msg), "Game saved to slot %d.", slot + 1);
        presenter()->consoleMessageStyled(0, kMsgChannelSystem, msg);
        return true;
    }

    if (strcmp(verb, "load") == 0) {
        int slot = parseSlot(rest);
        if (slot < 0) {
            snprintf(msg, sizeof(msg), "load: want a slot number 1-%d", kAdminSlotCount);
            reply(msg);
            return true;
        }

        LoadSaveSlotData data;
        if (!readSlotHeader(slot, data)) {
            snprintf(msg, sizeof(msg), "load: slot %d is empty or unreadable", slot + 1);
            reply(msg);
            return true;
        }

        if (worldLoaded) {
            // Refused rather than half-supported: swapping the world under a
            // live serve loop re-mints every netId beneath the connected viewers
            // and frees objects the presenter still holds refs to.
            reply("load: a world is already running — restart the server to load another");
            return true;
        }

        gPendingRequest = ServerAdminRequest {};
        gPendingRequest.kind = ServerAdminRequest::kLoadSlot;
        gPendingRequest.slot = slot;

        snprintf(msg, sizeof(msg), "load: slot %d '%.30s' — starting...", slot + 1, data.description);
        reply(msg);
        return true;
    }

    if (strcmp(verb, "new") == 0) {
        if (rest == nullptr) {
            reply("new: want a map name, e.g. 'new artemple.map'");
            return true;
        }
        if (worldLoaded) {
            reply("new: a world is already running — restart the server to start another");
            return true;
        }

        gPendingRequest = ServerAdminRequest {};
        gPendingRequest.kind = ServerAdminRequest::kNewWorld;
        gPendingRequest.map = rest;

        snprintf(msg, sizeof(msg), "new: booting '%s'...", rest);
        reply(msg);
        return true;
    }

    if (strcmp(verb, "quit") == 0 || strcmp(verb, "shutdown") == 0) {
        gPendingRequest = ServerAdminRequest {};
        gPendingRequest.kind = ServerAdminRequest::kQuit;
        reply("shutting down");
        return true;
    }

    return false; // not an admin verb — let the debug dispatch have it
}

bool serverAdminTakeRequest(ServerAdminRequest& out)
{
    if (gPendingRequest.kind == ServerAdminRequest::kNone) {
        return false;
    }
    out = gPendingRequest;
    gPendingRequest = ServerAdminRequest {};
    return true;
}

} // namespace fallout
