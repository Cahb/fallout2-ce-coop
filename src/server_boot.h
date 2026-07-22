#ifndef FALLOUT_SERVER_BOOT_H_
#define FALLOUT_SERVER_BOOT_H_

namespace fallout {

// Headless boot orchestrator for the core-only dedicated server (P5-C,
// [[p5-server-plan]] Step 1 "MAKE IT RUN"). This is the server-side counterpart
// to the client's gameInitWithOptions()/_main_load_new() (both relocated to the
// f2_client game_lifecycle.cc by H1): it runs the SIM-CORE subset of the boot
// sequence — the subsystem inits that load and own simulation state — and skips
// every presentation bring-up (window/palette/font/mouse/movie/iso/pipboy/
// character-editor/load-save/dialog-UI/automap). It never names an SDL symbol,
// so f2_server links and runs without f2_client.
//
// Order mirrors gameInitWithOptions() for the calls it keeps, so any ordering
// dependency between the sim inits is preserved. The presenter/scheduler/
// clock interlock is NOT installed here — serverServe()/serverRun() own that
// via serverInstall() (server_loop.cc).
//
// Returns 0 on success, non-zero if a required init or the map load failed.
// `mapName` is a map file name (e.g. "artemple.map"), loaded via mapLoadByName.
// argc/argv are forwarded to the config inits (sfallConfigInit/settingsInit),
// which parse them for overrides and read argv[0] — pass the process's real ones.
int serverBoot(const char* mapName, int argc, char** argv);

// serverBoot() split into its three phases, so the LOBBY can sit between them:
// the control channel has to be answering `saves` before a world exists, and
// the subsystem inits (file DB in particular) must already be up for a slot
// listing to be readable. serverBoot() is exactly these three in sequence and
// keeps its behaviour; call it when the world is known up front.
//
//   1. serverBootSubsystems  — memory/config/DB/message/sim inits. No world.
//   2a. serverBootNewWorld   — premade dude + map load  (the "new game" path)
//   2b. serverBootLoadSlot   — restore everything from SAVEGAME\SLOTnn instead
//   3. serverBootFinish      — actor registry, request handler, script rules,
//                              scriptsEnable. Required after EITHER 2a or 2b.
//
// ⚠ serverBootLoadSlot is N==1 ONLY. The player-actor registry is process
// lifetime state that no save carries yet, so a restored world comes back with
// the host actor alone; extras must rejoin. This is the known "disk save is not
// N-actor yet" gap, not a defect in the load path.
int serverBootSubsystems(int argc, char** argv);
int serverBootNewWorld(const char* mapName);
int serverBootLoadSlot(int slot);
int serverBootFinish(bool spawnExtras);

// Spawn ONE extra player actor beside the host into `slot` and register it;
// returns the slot, or -1. THE single spawn path, shared by boot and by
// spawn-at-login so a dynamically joined player is indistinguishable from a
// pre-spawned one (ACCOUNT_IDENTITY_DESIGN.md §3).
//
// ⚠ Seed the slot's SHEET first (playerActorSeedSheetFromHost) — this places a
// body, it does not fill a character sheet.
int serverSpawnPlayerActor(int slot);

// Seed ONE slot's whole sheet from the host: proto row, perks, PC stats, traits,
// tagged skills, name. All six or the actor is a chimera (PLAYER_SHEET_DESIGN.md
// stage 2) — and per-slot, never the bulk seeders, which would reset every
// already-connected player to the host's sheet (ACCOUNT_IDENTITY_DESIGN.md trap 1).
void playerActorSeedSheetFromHost(int slot);

// Tear down what serverBoot() brought up (the core-subsystem exits, in reverse
// dependency order). Mirrors the sim-core subset of the client's gameExit().
void serverShutdown();

} // namespace fallout

#endif /* FALLOUT_SERVER_BOOT_H_ */
