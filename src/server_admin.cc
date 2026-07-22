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
#include "db.h"
#include "debug.h"
#include "game.h"
#include "game_dialog.h" // gameDialogServerNodeActive — never autosave mid-node
#include "map.h" // mapTransitionPending / mapGetLoadGeneration / gElevation
#include "map_defs.h" // HEX_GRID_SIZE / hexGridTileIsValid — random stress placement
#include "object.h" // objectCreateWithPid / objectSetLocation / objectDestroy / _obj_blocking_at
#include "proto.h" // protoGetProto — validate a spawn pid before creating
#include "tile.h" // tileDistanceBetween — bias stress spawns toward the players
#include "worldmap.h" // wmMapIsSaveable — some maps forbid saving outright
#include "game_movie.h" // MOVIE_COUNT / gameMoviePlay — the `movie` test verb
#include "msg_channel.h"
#include "platform_compat.h"
#include "presenter.h"
#include "queue.h"
#include "savegame.h"
#include "scripts.h" // gameTimeAddTicks / gameTimeGetTime — the `timeskip` verb

namespace fallout {

// Vanilla's slot count. The on-disk layout is SAVEGAME\SLOTnn\SAVE.DAT for
// nn in 01..10, and the header carries a 30-char description — which is exactly
// the "numbered slot with an optional mnemonic label" the operator wants, so
// nothing new is invented here.
static constexpr int kSlotCount = 10;

// SLOT11 — the AUTOSAVE slot (index 10). Deliberately past everything the
// vanilla client's save/load screens can reach, so the periodic autosave can
// never clobber a real save. The OPERATOR still sees it: parseSlot and the
// `saves` listing run to kAdminSlotCount, so `load 11` restores an autosave
// (as does F2_SERVER_LOAD=11).
static constexpr int kAutosaveSlot = 10;
static constexpr int kAdminSlotCount = 11;

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

// Periodic unattended save into kAutosaveSlot. Called every MAIN-PHASE beat
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

    // Some maps forbid saving outright (MAP_SAVED flag) — on those the periodic
    // trigger waits for the next map rather than spamming failures.
    if (!wmMapIsSaveable()) {
        lastGeneration = generation; // don't re-latch the map-change trigger here
        return;
    }

    bool ok = adminWriteSave(kAutosaveSlot, "autosave");
    fprintf(stderr, "f2_server: autosave -> slot %d %s%s\n", kAutosaveSlot + 1,
        ok ? "ok" : "FAILED", mapChanged ? " (map change)" : "");
    if (ok) {
        // Every player sees the checkpoint land — same system channel as the
        // join/leave announcements.
        presenter()->consoleMessageStyled(0, kMsgChannelSystem, "Game auto-saved.");
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
static int stressRandomFreeTile(int elevation, std::mt19937& rng)
{
    for (int attempt = 0; attempt < 800; attempt++) {
        int tile = static_cast<int>(rng() % HEX_GRID_SIZE);
        if (!hexGridTileIsValid(tile)) {
            continue;
        }
        if (attempt < 400 && gDude != nullptr
            && tileDistanceBetween(tile, gDude->tile) > 30) {
            continue;
        }
        if (_obj_blocking_at(nullptr, tile, elevation) != nullptr) {
            continue;
        }
        return tile;
    }
    return -1;
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
        for (int i = 0; i < count; i++) {
            int tile = wantTile != -1 && hexGridTileIsValid(wantTile)
                ? wantTile
                : stressRandomFreeTile(gElevation, rng);
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
        for (int i = 0; i < count; i++) {
            int tile = stressRandomFreeTile(gElevation, rng);
            if (tile == -1) {
                break;
            }
            if (stressSpawnOne(pid, tile, gElevation) != nullptr) {
                placed++;
            }
        }

        if (placed > 0) {
            probeApplyAggro(placed);
        }

        snprintf(msg, sizeof(msg), "stress: %d/%d hostiles up (pid 0x%X seed %u) — aggroed",
            placed, count, pid, seed);
        reply(msg);
        fprintf(stderr, "f2_server: admin stress placed=%d seed=%u\n", placed, seed);
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

        if (!adminWriteSave(slot, label)) {
            snprintf(msg, sizeof(msg), "save: FAILED writing slot %d (error %d)",
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
