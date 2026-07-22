#ifndef FALLOUT_DIALOG_INTENT_H_
#define FALLOUT_DIALOG_INTENT_H_

namespace fallout {

// Dialog option-selection intent queue (the dialog analog of combat_intent).
// Under the server loop the blocking dialog loop `_gdProcess` is not driven by
// keyboard input — it drains these intents and calls `_gdProcessChoice(index)`
// directly (the same core path a numeric-key press drives). Data only, no
// SDL/window dependency, so this lives in f2_core.
//
// Scope v1: plain reply/option conversations. If a selected option's script
// proc tries to switch into a nested blocking UI (barter/party control), the
// server _gdProcess branch detects it (_dialogue_switch_mode != 0) and aborts
// the conversation rather than entering the unsupported nested loop.

enum DialogIntentKind {
    // Select the reply option at `arg` (0-based, must be <
    // gGameDialogOptionEntriesLength at the time it is drained).
    DIALOG_INTENT_SELECT,
    // End the conversation now (analog of pressing the [Done]/escape option).
    DIALOG_INTENT_END,
    // Open barter with the current speaker (analog of the on-screen Barter
    // button). Server-authoritative: the CRITTER_BARTER check and the "will not
    // barter" refusal are made in the drain, not on the viewer.
    DIALOG_INTENT_BARTER,
};

struct DialogIntent {
    int kind;
    int arg;
};

// Enqueue an intent (FIFO).
void dialogIntentPush(int kind, int arg);

// Peek the front intent without removing it; returns false if the queue is
// empty (out is untouched).
bool dialogIntentPeek(DialogIntent* out);

// Remove the front intent (no-op if empty).
void dialogIntentPop();

// True if any intent is queued.
bool dialogIntentPending();

// Drop all queued intents (called at serverRun start so runs are independent).
void dialogIntentClear();

} // namespace fallout

#endif /* FALLOUT_DIALOG_INTENT_H_ */
