#include "character_editor.h"

#include "db.h"
#include "perk.h"

namespace fallout {

// The level-up bookkeeping, split out of the client's character_editor.cc on
// the game_movie_state.cc precedent: move the state, not the projector.
//
// gCharacterEditorLastLevel is the level this PROCESS's editor last reconciled
// the award against. It stays a PC-global on purpose: since the award moved onto
// the XP funnel (stat.cc, per earner) it no longer decides what anyone is owed —
// it only keeps a screen from re-awarding what the funnel already granted, and a
// screen belongs to one process. The owed PERK, which IS per-player state, moved
// to perkOwedPickGet/Set.
//
// It round-trips through the savegame, so the driver in savegame.cc has to reach
// it. The editor screen keeps the *_Backup copies, which exist only to undo an
// abandoned edit.

// 0x518528
int gCharacterEditorLastLevel;

// 0x43C1B0
int characterEditorSave(File* stream)
{
    if (fileWriteInt32(stream, gCharacterEditorLastLevel) == -1)
        return -1;
    // Slot 0's owed pick — the byte's position and width are vanilla's, only its
    // backing store moved (perk.cc). Extras' flags ride the sheet row.
    if (fileWriteUInt8(stream, perkOwedPickGet(nullptr) ? 1 : 0) == -1)
        return -1;

    return 0;
}

// 0x43C1E0
int characterEditorLoad(File* stream)
{
    if (fileReadInt32(stream, &gCharacterEditorLastLevel) == -1)
        return -1;
    unsigned char hasFreePerk;
    if (fileReadUInt8(stream, &hasFreePerk) == -1)
        return -1;
    perkOwedPickSet(nullptr, hasFreePerk != 0);

    return 0;
}

} // namespace fallout
