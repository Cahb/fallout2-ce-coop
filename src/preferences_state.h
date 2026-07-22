#ifndef FALLOUT_PREFERENCES_STATE_H_
#define FALLOUT_PREFERENCES_STATE_H_

#include "db.h"

namespace fallout {

// The preferences savegame block. The preferences SCREEN fills this from its
// working copies; the headless server fills it from `settings.preferences`.
// Both go through the one layout in preferences_state.cc so a save written by
// either side stays loadable by the other.
typedef struct PreferencesBlock {
    int gameDifficulty;
    int combatDifficulty;
    int violenceLevel;
    int targetHighlight;
    int combatLooks;
    int combatMessages;
    int combatTaunts;
    int languageFilter;
    int running;
    int subtitles;
    int itemHighlight;
    int combatSpeed;
    int playerSpeedup;
    float textBaseDelay;
    int masterVolume;
    int musicVolume;
    int soundEffectsVolume;
    int speechVolume;
    float brightness;
    float mouseSensitivity;
} PreferencesBlock;

int preferencesBlockWrite(File* stream, const PreferencesBlock& block);
int preferencesBlockRead(File* stream, PreferencesBlock& block);

} // namespace fallout

#endif /* FALLOUT_PREFERENCES_STATE_H_ */
