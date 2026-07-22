#ifndef FALLOUT_SCRIPT_REQUEST_HANDLER_SERVER_H_
#define FALLOUT_SCRIPT_REQUEST_HANDLER_SERVER_H_

namespace fallout {

// Install the dedicated-server ScriptRequestHandler (f2_server only). Overrides
// SCRIPT_REQUEST_DIALOG -> gameDialogEnter and SCRIPT_REQUEST_WORLD_MAP ->
// worldmapServerDriver so the authoritative worldmap travel engine runs
// server-side; other requests stay the base no-op.
void scriptRequestHandlerInstallServer();

} // namespace fallout

#endif /* FALLOUT_SCRIPT_REQUEST_HANDLER_SERVER_H_ */
