#include "preferences_state.h"

#include "db.h"
#include "debug.h"

namespace fallout {

// The preferences savegame block, split out of the client's preferences.cc on
// the game_movie_state.cc precedent: move the state, not the projector.
//
// Only the BYTE LAYOUT lives here, not the values. The preferences screen keeps
// its own working copies and the headless server reads `settings.preferences`,
// but a save written by either must be loadable by the other, so both fill this
// one struct and neither restates the field order. Two hand-maintained copies
// of a layout is how EVENT_PLAYER_ROSTER silently dropped a row.
//
// The block is 20 fields: 18 int32 then 2 float, with text_base_delay written
// as a float in the middle. Order is load-bearing — it is the on-disk format.

int preferencesBlockWrite(File* stream, const PreferencesBlock& block)
{
    if (fileWriteInt32(stream, block.gameDifficulty) == -1) goto err;
    if (fileWriteInt32(stream, block.combatDifficulty) == -1) goto err;
    if (fileWriteInt32(stream, block.violenceLevel) == -1) goto err;
    if (fileWriteInt32(stream, block.targetHighlight) == -1) goto err;
    if (fileWriteInt32(stream, block.combatLooks) == -1) goto err;
    if (fileWriteInt32(stream, block.combatMessages) == -1) goto err;
    if (fileWriteInt32(stream, block.combatTaunts) == -1) goto err;
    if (fileWriteInt32(stream, block.languageFilter) == -1) goto err;
    if (fileWriteInt32(stream, block.running) == -1) goto err;
    if (fileWriteInt32(stream, block.subtitles) == -1) goto err;
    if (fileWriteInt32(stream, block.itemHighlight) == -1) goto err;
    if (fileWriteInt32(stream, block.combatSpeed) == -1) goto err;
    if (fileWriteInt32(stream, block.playerSpeedup) == -1) goto err;
    if (fileWriteFloat(stream, block.textBaseDelay) == -1) goto err;
    if (fileWriteInt32(stream, block.masterVolume) == -1) goto err;
    if (fileWriteInt32(stream, block.musicVolume) == -1) goto err;
    if (fileWriteInt32(stream, block.soundEffectsVolume) == -1) goto err;
    if (fileWriteInt32(stream, block.speechVolume) == -1) goto err;
    if (fileWriteFloat(stream, block.brightness) == -1) goto err;
    if (fileWriteFloat(stream, block.mouseSensitivity) == -1) goto err;

    return 0;

err:

    debugPrint("\nOPTION MENU: Error save option data!\n");

    return -1;
}

int preferencesBlockRead(File* stream, PreferencesBlock& block)
{
    if (fileReadInt32(stream, &block.gameDifficulty) == -1) goto err;
    if (fileReadInt32(stream, &block.combatDifficulty) == -1) goto err;
    if (fileReadInt32(stream, &block.violenceLevel) == -1) goto err;
    if (fileReadInt32(stream, &block.targetHighlight) == -1) goto err;
    if (fileReadInt32(stream, &block.combatLooks) == -1) goto err;
    if (fileReadInt32(stream, &block.combatMessages) == -1) goto err;
    if (fileReadInt32(stream, &block.combatTaunts) == -1) goto err;
    if (fileReadInt32(stream, &block.languageFilter) == -1) goto err;
    if (fileReadInt32(stream, &block.running) == -1) goto err;
    if (fileReadInt32(stream, &block.subtitles) == -1) goto err;
    if (fileReadInt32(stream, &block.itemHighlight) == -1) goto err;
    if (fileReadInt32(stream, &block.combatSpeed) == -1) goto err;
    if (fileReadInt32(stream, &block.playerSpeedup) == -1) goto err;
    if (fileReadFloat(stream, &block.textBaseDelay) == -1) goto err;
    if (fileReadInt32(stream, &block.masterVolume) == -1) goto err;
    if (fileReadInt32(stream, &block.musicVolume) == -1) goto err;
    if (fileReadInt32(stream, &block.soundEffectsVolume) == -1) goto err;
    if (fileReadInt32(stream, &block.speechVolume) == -1) goto err;
    if (fileReadFloat(stream, &block.brightness) == -1) goto err;
    if (fileReadFloat(stream, &block.mouseSensitivity) == -1) goto err;

    return 0;

err:

    debugPrint("\nOPTION MENU: Error loading option data!, using defaults.\n");

    return -1;
}

} // namespace fallout
