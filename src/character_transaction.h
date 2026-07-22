#ifndef FALLOUT_CHARACTER_TRANSACTION_H_
#define FALLOUT_CHARACTER_TRANSACTION_H_

#include "proto_types.h"

namespace fallout {

// Ledger H-49: the character editor is a TRANSACTION over committed dude
// state — opening it takes a snapshot (begin), Cancel rolls the snapshot
// back, Done commits by discarding it. This module owns the begin/rollback
// of the transaction's committed sim state so the headless server can
// snapshot and roll back dude state without the editor UI.
//
// Only committed sim fields with no editor-dialog coupling live here: the
// dude's critter proto data (base SPECIAL/skills), current hit points, name
// and unspent skill points. The perk-rank backup and tagged-skill/trait
// backups stay editor statics (they double as the perk/skill/trait dialogs'
// session working copies), and the perk rollback (_pop_perks) plus the saved
// level/free-perk bookkeeping stay in character_editor.cc for now.
struct CharacterSnapshot {
    CritterProtoData dudeData;
    int hitPoints;
    char name[32];
    int unspentSkillPoints;
};

// Begin. Split to match the exact statement order of the counterpart
// characterEditorSavePlayer, which interleaves these captures with the
// editor-only backups that keep their original positions.
void characterSnapshotTake(CharacterSnapshot* snapshot);
void characterSnapshotTakeSkillPoints(CharacterSnapshot* snapshot);

// Rollback. Split into three sequential steps so the exact statement order
// of characterEditorRestorePlayer is preserved: its sim restores are
// interleaved with editor-only lines (the perk rollback, the level/free-perk
// assigns, the tagged-skill/trait restore and the session-copy rebuild) that
// must keep their original positions.
void characterSnapshotRestore(const CharacterSnapshot* snapshot);
void characterSnapshotRestoreSkillPoints(const CharacterSnapshot* snapshot);
void characterSnapshotRestoreHitPoints(const CharacterSnapshot* snapshot);

} // namespace fallout

#endif /* FALLOUT_CHARACTER_TRANSACTION_H_ */
