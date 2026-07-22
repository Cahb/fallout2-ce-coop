#ifndef LOAD_SAVE_GAME_H
#define LOAD_SAVE_GAME_H

namespace fallout {

typedef enum LoadSaveMode {
    // Special case - loading game from main menu.
    LOAD_SAVE_MODE_FROM_MAIN_MENU,

    // Normal (full-screen) save/load screen.
    LOAD_SAVE_MODE_NORMAL,

    // Quick load/save.
    LOAD_SAVE_MODE_QUICK,
} LoadSaveMode;

void _InitLoadSave();
int lsgSaveGame(int mode);
int lsgLoadGame(int mode);
bool _isLoadingGame();
int MapDirErase(const char* path, const char* extension);

// `_ResetLoadSave` and `lsgInit` now live in savegame.h (core).

} // namespace fallout

#endif /* LOAD_SAVE_GAME_H */
