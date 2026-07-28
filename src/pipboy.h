#ifndef PIPBOY_H
#define PIPBOY_H

#include "db.h"

namespace fallout {

typedef enum PipboyOpenIntent {
    PIPBOY_OPEN_INTENT_UNSPECIFIED = 0,
    PIPBOY_OPEN_INTENT_REST = 1,
} PipboyOpenIntent;

int pipboyOpen(int intent);
void pipboyInit();
void pipboyReset();
int pipboySave(File* stream);
int pipboyLoad(File* stream);

// ─── SERVER-AUTHORED HOLODISKS (co-op) ──────────────────────────────────────
//
// A vanilla holodisk is three numbers in data/holodisk.txt — a gvar, a name msgId and
// a BODY msgId — and its body is the CONSECUTIVE pipboy.msg entries from that id until
// one reads "**END-DISK**". So a disk is really just a name and an array of lines,
// which is what makes a server-authored one cheap: the server ships the strings and
// the viewer appends them to the list. No holodisk.txt row, no pipboy.msg entries, no
// gvar, and no savegame change — the server RE-ANNOUNCES its disks on every baseline,
// so they are server config rather than sim state and nothing has to persist them.
//
// They render after the vanilla disks and are ALWAYS visible (a vanilla disk is gated
// on its gvar; a server disk has none to gate on). Selection indices continue past
// gHolodisksCount, so `_holodisk >= gHolodisksCount` means "server disk number
// _holodisk - gHolodisksCount".
//
// Replace-whole-set semantics: clear, then add each. That keeps the wire event
// idempotent, so a rebaseline re-announcement cannot duplicate entries.
void pipboyServerHolodiskClear();
void pipboyServerHolodiskAdd(const char* name, const char* const* lines, int lineCount);
int pipboyServerHolodiskCount();

} // namespace fallout

#endif /* PIPBOY_H */
