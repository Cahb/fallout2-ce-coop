#ifndef CHARACTER_EDITOR_H
#define CHARACTER_EDITOR_H

#include "db.h"

namespace fallout {

extern int gCharacterEditorRemainingCharacterPoints;

int characterEditorShow(bool isCreationMode);
// The co-op viewer's character sheet: characterEditorShow(0) with every edit
// rolled back on the way out. See the definition for why.
int characterEditorShowViewOnly();
void characterEditorInit();
bool _isdoschar(int ch);
// Live in character_editor_state.cc so the savegame driver in f2_core can
// reach them; the editor screen owns only the undo copies.
extern int gCharacterEditorLastLevel;
extern unsigned char gCharacterEditorHasFreePerk;

int characterEditorSave(File* stream);
int characterEditorLoad(File* stream);
void characterEditorReset();

} // namespace fallout

#endif /* CHARACTER_EDITOR_H */
