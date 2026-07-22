#ifndef FALLOUT_SAVEGAME_H_
#define FALLOUT_SAVEGAME_H_

#include "db.h"

namespace fallout {

#define LOAD_SAVE_DESCRIPTION_LENGTH 30
#define LOAD_SAVE_HANDLER_COUNT 27

typedef struct LoadSaveSlotData {
    char signature[24];
    short versionMinor;
    short versionMajor;
    // TODO: The type is probably char, but it's read with the same function as
    // reading unsigned chars, which in turn probably result of collapsing
    // reading functions.
    unsigned char versionRelease;
    char characterName[32];
    char description[LOAD_SAVE_DESCRIPTION_LENGTH];
    short fileMonth;
    short fileDay;
    short fileYear;
    int fileTime;
    short gameMonth;
    short gameDay;
    short gameYear;
    unsigned int gameTime;
    short elevation;
    short map;
    char fileName[16];
} LoadSaveSlotData;

// The save/load driver: the handler tables and the .SAV file I/O, with no
// dependency on the slot-picker screen. `loadsave.cc` is the screen that drives
// it; a headless writer drives the same functions directly.

// The slot a save/load operation runs against. `_GameMap2Slot` and
// `_SlotMap2Game` are entries in the handler table, whose signature is fixed at
// `int(File*)`, so the slot cannot be a parameter — it is operation-scoped
// ambient state that the caller sets before starting.
void savegameSetSlot(int slot);
int savegameGetSlot();

// The preview thumbnail written into the save header. It is grabbed off the
// screen, so a headless writer has none; leave it null and the header block is
// written blank, which keeps the file loadable by a client.
void savegameSetPreviewBuffer(unsigned char* buffer);

// The header rows the slot-picker screen displays.
LoadSaveSlotData* savegameSlotData(int slot);

// Refreshes the cached master patches path from settings. Callers do this at
// the start of an operation because the setting can change between them.
void savegameRefreshPatchesPath();

int savegameGetErrorCode();
void savegameResetErrorCode();

int lsgPerformSaveGame();
int lsgLoadGameInSlot(int slot);
int lsgSaveHeaderInSlot(File* stream, int slot);
int lsgLoadHeaderInSlot(File* stream, int slot);
int savegameEraseSlot();

bool _isLoadingGame();
int MapDirErase(const char* relativePath, const char* extension);

// Savegame-scratch cleanup: erases the MAPS\*.SAV working copies (and, for
// `_ResetLoadSave`, the per-save proto overrides). Screen-free, so the headless
// sim reset runs them as well as the client's.
void lsgInit();
void _ResetLoadSave();

} // namespace fallout

#endif /* FALLOUT_SAVEGAME_H_ */
