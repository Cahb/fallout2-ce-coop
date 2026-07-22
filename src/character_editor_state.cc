#include "character_editor.h"

#include "db.h"

namespace fallout {

// The level-up bookkeeping, split out of the client's character_editor.cc on
// the game_movie_state.cc precedent: move the state, not the projector.
//
// Both fields round-trip through the savegame, so the driver in savegame.cc has
// to reach them. They are also genuinely per-character rather than per-screen:
// gCharacterEditorLastLevel is the level the perk award was last reconciled
// against, and gCharacterEditorHasFreePerk is an owed pick. The editor screen
// keeps the *_Backup copies, which exist only to undo an abandoned edit.
//
// NOTE: these are still PC-globals. Per-actor sheets will have to move them
// onto the sheet rather than leave one owed perk shared by every player.

// 0x518528
int gCharacterEditorLastLevel;

// 0x51852C
unsigned char gCharacterEditorHasFreePerk;

// 0x43C1B0
int characterEditorSave(File* stream)
{
    if (fileWriteInt32(stream, gCharacterEditorLastLevel) == -1)
        return -1;
    if (fileWriteUInt8(stream, gCharacterEditorHasFreePerk) == -1)
        return -1;

    return 0;
}

// 0x43C1E0
int characterEditorLoad(File* stream)
{
    if (fileReadInt32(stream, &gCharacterEditorLastLevel) == -1)
        return -1;
    if (fileReadUInt8(stream, &gCharacterEditorHasFreePerk) == -1)
        return -1;

    return 0;
}

} // namespace fallout
