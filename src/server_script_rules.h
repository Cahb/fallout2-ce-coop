#ifndef FALLOUT_SERVER_SCRIPT_RULES_H_
#define FALLOUT_SERVER_SCRIPT_RULES_H_

namespace fallout {

// Server-side co-op RULES expressed as taps on the interpreter's public opcode
// hook seam (interpreter.h). Policy lives HERE, in f2_server; the interpreter
// only provides the mechanism.
//
// Installed once at server boot. Nothing is registered on the client, the
// headless probe or any golden path — those never call this — so script
// behavior there is unchanged by construction.
void serverScriptRulesInstall();

} // namespace fallout

#endif /* FALLOUT_SERVER_SCRIPT_RULES_H_ */
