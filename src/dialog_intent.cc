#include "dialog_intent.h"

#include <deque>

namespace fallout {

// See dialog_intent.h. A plain FIFO; the server _gdProcess branch drains it.
static std::deque<DialogIntent> gDialogIntents;

void dialogIntentPush(int kind, int arg)
{
    gDialogIntents.push_back(DialogIntent { kind, arg });
}

bool dialogIntentPeek(DialogIntent* out)
{
    if (gDialogIntents.empty()) {
        return false;
    }
    *out = gDialogIntents.front();
    return true;
}

void dialogIntentPop()
{
    if (!gDialogIntents.empty()) {
        gDialogIntents.pop_front();
    }
}

bool dialogIntentPending()
{
    return !gDialogIntents.empty();
}

void dialogIntentClear()
{
    gDialogIntents.clear();
}

} // namespace fallout
