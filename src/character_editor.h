#ifndef CHARACTER_EDITOR_H
#define CHARACTER_EDITOR_H

#include "db.h"

namespace fallout {

extern int gCharacterEditorRemainingCharacterPoints;

int characterEditorShow(bool isCreationMode);
// The co-op player's character sheet: characterEditorShow(0) whose spends EMIT
// WIRE INTENTS instead of mutating locally. See the definition.
int characterEditorShowViewOnly();
void characterEditorInit();
bool _isdoschar(int ch);
// Lives in character_editor_state.cc so the savegame driver in f2_core can reach
// it; the editor screen owns only the undo copy. The owed PERK moved to
// perkOwedPickGet/Set (per player — this one is per process).
extern int gCharacterEditorLastLevel;

int characterEditorSave(File* stream);
int characterEditorLoad(File* stream);
void characterEditorReset();

} // namespace fallout

#endif /* CHARACTER_EDITOR_H */
