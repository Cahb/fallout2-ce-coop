#include "script_request_handler.h"

namespace fallout {

// Base = null handler (all no-ops, elevatorSelect returns -1): a dedicated server
// drops UI-bound script requests deterministically. The client installs a
// forwarding subclass at boot (script_request_handler_client.cc), so the client
// (and the headless probe that boots through game_lifecycle.cc) keeps the legacy
// behavior byte-for-byte.
static ScriptRequestHandler gNullHandler;
static ScriptRequestHandler* gHandler = &gNullHandler;

ScriptRequestHandler* scriptRequestHandler()
{
    return gHandler;
}

void scriptRequestHandlerSet(ScriptRequestHandler* newHandler)
{
    gHandler = newHandler != nullptr ? newHandler : &gNullHandler;
}

} // namespace fallout
