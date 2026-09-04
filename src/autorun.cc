#include "autorun.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
// 0x530010
static HANDLE gInterplayGenericAutorunMutex;
#endif

namespace fallout {

// 0x4139C0
bool autorunMutexCreate()
{
#ifdef _WIN32
    // Headless probes (golden gates, CI) must run alongside a real game instance
    // and alongside each other. The single-instance lock is a desktop
    // convenience, not a correctness guard, so it does not apply to them.
    if (getenv("F2_HEADLESS_PROBE") != nullptr) {
        return true;
    }

    gInterplayGenericAutorunMutex = CreateMutexA(nullptr, FALSE, "InterplayGenericAutorunMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(gInterplayGenericAutorunMutex);
        return false;
    }
#endif

    return true;
}

// 0x413A00
void autorunMutexClose()
{
#ifdef _WIN32
    if (gInterplayGenericAutorunMutex != nullptr) {
        CloseHandle(gInterplayGenericAutorunMutex);
    }
#endif
}

} // namespace fallout
