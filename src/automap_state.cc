#include "automap.h"

#include "db.h"

namespace fallout {

// The automap display flags, split out of the client's automap.cc rendering TU
// on the game_movie_state.cc precedent: move the state, not the projector.
//
// The flags round-trip through the savegame, so the driver in savegame.cc has
// to reach them. The map data itself already lives in AUTOMAP.DB and is written
// by automapSaveCurrent(); this is only the small header the save file carries.

// 0x41B884
int gAutomapFlags = 0;

// 0x41B87C
int automapLoad(File* stream)
{
    return fileReadInt32(stream, &gAutomapFlags);
}

// 0x41B898
int automapSave(File* stream)
{
    return fileWriteInt32(stream, gAutomapFlags);
}

} // namespace fallout
