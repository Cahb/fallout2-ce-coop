#include "server_accounts.h"

#include <string.h>

#include "db.h"
#include "debug.h"
#include "platform_compat.h" // compat_stricmp — case-insensitive name match
#include "server_players.h" // kMaxPlayerActors

namespace fallout {

// One row per registry slot, indexed by slot DIRECTLY (slot 0 = the host's
// account, unlike the character-name array which aliases the host global — the
// host's ACCOUNT is a distinct concept that must persist). Static zero-init, so a
// fresh process starts fully unowned and every non-server path (client viewer,
// headless probe, goldens) reads empty names and takes no new branch.
struct AccountRow {
    char name[kAccountNameMaxLength];
    char token[kAccountTokenMaxLength];
};

static AccountRow gAccounts[kMaxPlayerActors];

int accountSlotForName(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }

    for (int slot = 0; slot < kMaxPlayerActors; slot++) {
        if (gAccounts[slot].name[0] != '\0'
            && compat_stricmp(gAccounts[slot].name, name) == 0) {
            return slot;
        }
    }

    return -1;
}

void accountAssign(int slot, const char* name, const char* token)
{
    if (slot < 0 || slot >= kMaxPlayerActors || name == nullptr) {
        return;
    }

    strncpy(gAccounts[slot].name, name, kAccountNameMaxLength - 1);
    gAccounts[slot].name[kAccountNameMaxLength - 1] = '\0';

    // A null/absent token clears the row's token (first login without one =
    // unprotected slot, matches everything until a token is supplied).
    if (token != nullptr) {
        strncpy(gAccounts[slot].token, token, kAccountTokenMaxLength - 1);
        gAccounts[slot].token[kAccountTokenMaxLength - 1] = '\0';
    } else {
        gAccounts[slot].token[0] = '\0';
    }
}

const char* accountNameForSlot(int slot)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return "";
    }

    return gAccounts[slot].name;
}

bool accountSlotOwned(int slot)
{
    return slot >= 0 && slot < kMaxPlayerActors && gAccounts[slot].name[0] != '\0';
}

bool accountTokenMatches(int slot, const char* token)
{
    if (slot < 0 || slot >= kMaxPlayerActors) {
        return false;
    }

    // No stored token → matches anything (default-OFF, first-claimer-wins).
    if (gAccounts[slot].token[0] == '\0') {
        return true;
    }

    return token != nullptr && strcmp(gAccounts[slot].token, token) == 0;
}

void accountClear()
{
    memset(gAccounts, 0, sizeof(gAccounts));
}

// Fixed-width rows so the table is self-delimiting inside the appendix, the same
// discipline as critterPlayerActorNameRowWrite. Slots [0, count).
int accountTableWrite(File* stream, int count)
{
    if (count < 0 || count > kMaxPlayerActors) {
        return -1;
    }

    for (int slot = 0; slot < count; slot++) {
        if (fileWrite(gAccounts[slot].name, kAccountNameMaxLength, 1, stream) != 1) {
            return -1;
        }

        if (fileWrite(gAccounts[slot].token, kAccountTokenMaxLength, 1, stream) != 1) {
            return -1;
        }
    }

    return 0;
}

int accountTableRead(File* stream, int count)
{
    if (count < 0 || count > kMaxPlayerActors) {
        debugPrint("server_accounts: table count %d out of range\n", count);
        return -1;
    }

    // Clear first: any slot the table does not cover (and a v1 'PACT' save, which
    // supplies no table at all) must land unowned rather than inherit a prior
    // game's ownership.
    accountClear();

    for (int slot = 0; slot < count; slot++) {
        if (fileRead(gAccounts[slot].name, kAccountNameMaxLength, 1, stream) != 1) {
            return -1;
        }

        if (fileRead(gAccounts[slot].token, kAccountTokenMaxLength, 1, stream) != 1) {
            return -1;
        }

        // Untrusted disk input handed to name lookups / message formatting.
        gAccounts[slot].name[kAccountNameMaxLength - 1] = '\0';
        gAccounts[slot].token[kAccountTokenMaxLength - 1] = '\0';
    }

    return 0;
}

} // namespace fallout
