#ifndef FALLOUT_SCRIPT_REQUEST_HANDLER_CLIENT_H_
#define FALLOUT_SCRIPT_REQUEST_HANDLER_CLIENT_H_

namespace fallout {

// Install the client (UI-backed) script-request handler. Called once at boot
// from game_lifecycle.cc, next to presenterInstallClient().
void scriptRequestHandlerInstallClient();

} // namespace fallout

#endif /* FALLOUT_SCRIPT_REQUEST_HANDLER_CLIENT_H_ */
