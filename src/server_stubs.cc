// f2_server client-severance stubs (P5-C, [[p5-server-plan]]).
//
// f2_server links f2_core WITHOUT f2_client. Every f2_client symbol that the
// core's server codepath still reaches is satisfied here by an abort/no-op
// stub or a placeholder definition. THIS FILE IS THE ENUMERATED SEVERANCE
// SURFACE: each entry is a client dependency the simulation core has not yet
// been decoupled from. Making the server actually RUN standalone means
// replacing these, roughly in this order of leverage:
//   * INFRASTRUCTURE the server genuinely needs — the movement/animation path
//     (reg_anim_*/animationRegister*/_object_animate), still living in the
//     SDL-coupled f2_client animation.cc. Timing, debug output, and file/string
//     I/O were already extracted to f2_core (timing.cc, debug.cc,
//     platform_compat.cc); the background pump (_process_bk) and the three
//     _doBkProcesses hooks (_updateWindows/_gdialogActive/gameMovieIsPlaying) are
//     now server behavior SHIMS in server_shim.cc, not aborts.
//   * PRESENTATION the server must never call — window/art/sound/dialog/mouse/
//     movie/interface/render. These abort() so a mis-routed call is caught
//     loudly rather than silently corrupting state.
//
// Function signatures are lifted verbatim from the declaring f2_client headers
// (included below), so the compiler validates every stub against the real
// prototype and the linker confirms the set is complete. Regenerate with
// tools in the P5-C session if the core's client-symbol surface changes.
//
// 232 function stubs + 11 data placeholders
// + 2 global-namespace sfall_kb helpers. De-stubbed so far in P5-C Step 1:
//   1a timing/ticker infra (getTicks/_get_bk_time/tickers*) -> f2_core/timing.cc
//      with a client-registered clock backend;
//   1b debug output (debugPrint + _debug_register_*) -> f2_core/debug.cc, the
//      SDL_LogMessageV fallback now vfprintf(stderr);
//   1c file/string I/O (17 compat_* fns + getFileSize) -> f2_core/
//      platform_compat.cc, the 6 SDL string wrappers swapped for libc
//      (strcasecmp/strncasecmp/strdup) and hand-rolled ASCII strupr/strlwr/itoa;
//   1d pump + _doBkProcesses neutralize (4 fns: _process_bk/_updateWindows/
//      _gdialogActive/gameMovieIsPlaying) -> SHIMMED (not extracted) in
//      server_shim.cc: reduced tickers-only pump + no-op/false headless bodies;
//   H2 animation backend (30 fns) -> server_anim.cc synchronous applier;
//   H4 art subsystem (art.cc whole -> f2_core; artRender split to art_render.cc):
//      14 art* fns + _art_fid_valid + buildFid + FrmImage + 2 data placeholders;
//   H1 boot/reset/teardown (gameInitWithOptions/gameReset/gameExit ->
//      game_lifecycle.cc in f2_client): 47 window/palette/font/mouse/movie/dialog/
//      pipboy/char-editor/automap/loadsave/screenshot fns + gModernFontManager.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "animation.h"
#include "pres_record.h"
#include "art.h"
#include "automap.h"
#include "character_editor.h"
#include "character_selector.h"
#include "color.h"
#include "combat.h"
#include "combat_ui.h"
#include "cycle.h"
#include "dbox.h"
#include "debug.h"
#include "dialog.h"
#include "draw.h"
#include "elevator.h"
#include "endgame.h"
#include "font_manager.h"
#include "game.h"
#include "game_dialog.h"
// A0 (DIALOG_STREAMING_PLAN): game_dialog.cc is now linked into f2_server;
// these headers declare the renderer symbols its deps resolve to on the
// "dialog-UI" stub block near the bottom of this file.
#include "delay.h"
#include "display_monitor.h"
#include "fps_limiter.h"
#include "lips.h"
#include "game_mouse.h"
#include "game_movie.h"
#include "presenter.h" // presenter()->moviePlay — the server projects, viewers play
#include "game_sound.h"
#include "game_ui.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "client_barter.h"
#include "client_dialog.h"
#include "client_net.h"
#include "inventory.h"
#include "kb.h"
#include "loadsave.h"
#include "map.h"
#include "map_render.h"
#include "mouse.h"
#include "mouse_manager.h"
#include "movie.h"
#include "movie_effect.h"
#include "obj_types.h"
#include "object_render.h"
#include "options.h"
#include "palette.h"
#include "pcx.h"
#include "pipboy.h"
#include "preferences.h"
#include "preferences_state.h"
#include "settings.h"
#include "platform_compat.h"
#include "presenter_client.h"
#include "sfall_kb_helpers.h"
#include "sound.h"
#include "svga.h"
#include "text_font.h"
#include "text_object.h"
#include "tile.h"
#include "tile_render.h"
#include "window.h"
#include "window_manager.h"
#include "window_manager_private.h"
#include "worldmap.h"
#include "worldmap_ui.h"

namespace fallout {

// Loud failure for any presentation/infrastructure symbol the server should
// not (yet) reach. [[noreturn]] so non-void stubs need no return statement.
[[noreturn]] static void serverStubAbort(const char* symbol)
{
    fprintf(stderr, "f2_server: FATAL — client symbol '%s' called on the core-only "
                    "server. This capability is not severed/reimplemented yet.\n",
        symbol);
    abort();
}

// ---- data placeholders (globals the core names; never meaningfully read on
// the aborting server — storage only, so the link resolves) ----
unsigned char _cmap[768];
unsigned char _colorTable[32768];
FontManagerGetStringWidthProc* fontGetStringWidth;
int gCharacterEditorRemainingCharacterPoints;
Object* gGameMouseBouncingCursor;
Object* gGameMouseHexCursor;
int gIsoWindow;
Rect _scr_size;

// ---- free-function stubs (auto-generated from the headers above) ----
char* _colorError() { serverStubAbort("_colorError"); }
int combatInputClient() { serverStubAbort("combatInputClient"); }
void combatTurnRunClient() { serverStubAbort("combatTurnRunClient"); }
int _createWindow(const char* windowName, int x, int y, int width, int height, int a6, int flags) { serverStubAbort("_createWindow"); }
bool _deleteWindow(const char* windowName) { serverStubAbort("_deleteWindow"); }
// Benign: the two halves of the generic dialog WIDGET's teardown (dialog.cc),
// reached headless through _barter_end_to_talk_to — the 'T'/Talk button that
// leaves a trade. Both are safe as no-ops because the widget itself never runs
// here: _dialogQuit's whole effect is on _inDialog/_exitDialog, statics of a
// loop no headless path enters, and it returns 0 unconditionally (opSayQuit
// treats non-zero as fatal, so 0 is the required answer, not a convenient one).
// _dialogClose frees four buffers the widget allocates; headless they were never
// allocated, so freeing nothing is the correct whole operation.
void _dialogClose() { }
int _dialogGetDialogDepth() { serverStubAbort("_dialogGetDialogDepth"); }
int _dialogGetExitPoint() { serverStubAbort("_dialogGetExitPoint"); }
int _dialogGetMediaFlag() { serverStubAbort("_dialogGetMediaFlag"); }
int _dialogGotoReply(const char* a1) { serverStubAbort("_dialogGotoReply"); }
int _dialogOption(const char* a1, const char* a2) { serverStubAbort("_dialogOption"); }
int _dialogOptionProc(const char* a1, int a2) { serverStubAbort("_dialogOptionProc"); }
int _dialogQuit() { return 0; } // see the _dialogClose note above
int _dialogReply(const char* a1, const char* a2) { serverStubAbort("_dialogReply"); }
int _dialogRestart() { serverStubAbort("_dialogRestart"); }
int _dialogSetOptionFlags(int flags) { serverStubAbort("_dialogSetOptionFlags"); }
int _dialogSetScrollDown(int a1, int a2, char* a3, char* a4, char* a5, char* a6, int a7) { serverStubAbort("_dialogSetScrollDown"); }
int _dialogSetScrollUp(int a1, int a2, char* a3, char* a4, char* a5, char* a6, int a7) { serverStubAbort("_dialogSetScrollUp"); }
int _dialogStart(Program* a1) { serverStubAbort("_dialogStart"); }
int _dialogToggleMediaFlag(int a1) { serverStubAbort("_dialogToggleMediaFlag"); }
void _displayFile(char* fileName) { serverStubAbort("_displayFile"); }
void _displayFileRaw(char* fileName) { serverStubAbort("_displayFileRaw"); }
// Benign: the dude's idle "fidget" animation ticker (registered by combat.cc:2732
// at combat-end and by ClientPresenter). Pure ambient presentation with no sim
// authority; the real body draws RNG only to pick a cosmetic idle frame, which a
// headless server must NOT consume. No-op. Reached when the dude SURVIVES a fight
// (combat re-enables fidget) — a path the combat goldens never hit (their dude dies).
void _dude_fidget() { /* no idle animation headless */ }
// _dude_stand / _dude_standup relocated to critter.cc (f2_core) — no longer stubs.
void _fillBuf3x3(unsigned char* src, int srcWidth, int srcHeight, unsigned char* dest, int destWidth, int destHeight) { serverStubAbort("_fillBuf3x3"); }
void _freeColorBlendTable(int a1) { (void)a1; /* Benign: paired with the null-returning _getColorBlendTable; render-only. */ }
// Benign: color blend (translucency) tables are render-only. Callers store the
// result solely for blitting (e.g. worldmap circleBlendTable) and null-check
// before use/free, so nullptr — "no blend table headless" — is safe.
unsigned char* _getColorBlendTable(int ch) { (void)ch; return nullptr; }
unsigned char* _getSystemPalette() { serverStubAbort("_getSystemPalette"); }
void _gmouse_disable(int a1) { (void)a1; /* Benign: mouse cursor gating; no mouse headless. */ }
// Benign: clears the outline on the item under the mouse cursor
// (gGameMouseHighlightedItem). That highlight is client-only state and is
// always null headless, so the real body is a no-op on the server anyway.
void _gmouse_remove_item_outline(Object* object) { (void)object; }
// Benign: background level music is pure presentation (the real body just calls
// backgroundSoundLoad). No audio headless → no-op. Return 0 (success), NOT -1:
// worldmap.cc:4342 branches on == -1 as a load failure, and the map-enter path
// treats non-zero as an error. Reached nondeterministically on AI-heavy maps
// (arvillag/denbus1) via the level-music load.
int _gsound_background_play_level_music(const char* a1, int a2) { (void)a1; (void)a2; return 0; }
// Benign: no sfx queue headless — report success (mapLoad treats -1 as an error
// that fails the load).
int _gsound_sfx_q_start() { return 0; }
int _intface_update_ammo_lights() { return 0; /* Benign: no HUD ammo lights headless. */ }
// _inven_set_timer / inven_get_current_target_obj: REAL now, from inventory_ui.cc
// (linked for barter). They were stubs only while that file was absent.
void _obj_blend_table_exit() { serverStubAbort("_obj_blend_table_exit"); }
void _obj_blend_table_init() { /* Benign: translucency blend tables are render-only (unused headless). */ }
void _obj_render_table_exit() { serverStubAbort("_obj_render_table_exit"); }
int _obj_render_table_init() { return 0; /* Benign: per-hex render list table is blit-only (unused headless). */ }
int _popWindow() { serverStubAbort("_popWindow"); }
int _pushWindow(const char* windowName) { serverStubAbort("_pushWindow"); }
// Reachable ONLY inside a presentation record section (the animate branch the
// server otherwise skips). Records the priority op; still aborts if reached on any
// other path (that would be a genuine unsevered client reference).
int _register_priority(int a1)
{
    if (presRecordActive()) {
        presRecordPriority(a1);
        return 0;
    }
    serverStubAbort("_register_priority");
}
bool _selectWindowID(int index) { serverStubAbort("_selectWindowID"); }
void _setSystemPalette(unsigned char* palette) { serverStubAbort("_setSystemPalette"); }
int _soundType(Sound* sound, int type) { serverStubAbort("_soundType"); }
int _win_debug(char* string) { serverStubAbort("_win_debug"); }
int _win_list_select(const char* title, char** fileList, int fileListLength, ListSelectionHandler* callback, int x, int y, int color) { serverStubAbort("_win_list_select"); }
bool _windowActivateRegion(const char* regionName, int a2) { serverStubAbort("_windowActivateRegion"); }
bool _windowAddButton(const char* buttonName, int x, int y, int width, int height, int flags) { serverStubAbort("_windowAddButton"); }
bool _windowAddButtonGfx(const char* buttonName, char* pressedFileName, char* normalFileName, char* hoverFileName) { serverStubAbort("_windowAddButtonGfx"); }
bool _windowAddButtonProc(const char* buttonName, Program* program, int mouseEnterProc, int mouseExitProc, int mouseDownProc, int mouseUpProc) { serverStubAbort("_windowAddButtonProc"); }
bool _windowAddButtonText(const char* buttonName, const char* text) { serverStubAbort("_windowAddButtonText"); }
// Benign: registers a per-window key/input callback (here intLibDoInput for the
// script key_pressed opcodes). The server polls no window input, so the handler
// never fires and dropping the registration is a no-op.
void _windowAddInputFunc(WindowInputHandler* handler) { (void)handler; }
bool _windowAddRegionName(const char* regionName) { serverStubAbort("_windowAddRegionName"); }
bool _windowAddRegionPoint(int x, int y, bool a3) { serverStubAbort("_windowAddRegionPoint"); }
bool _windowAddRegionProc(const char* regionName, Program* program, int a3, int a4, int a5, int a6) { serverStubAbort("_windowAddRegionProc"); }
bool _windowAddRegionRightProc(const char* regionName, Program* program, int a3, int a4) { serverStubAbort("_windowAddRegionRightProc"); }
bool _windowCheckRegionExists(const char* regionName) { serverStubAbort("_windowCheckRegionExists"); }
bool _windowDeleteButton(const char* buttonName) { serverStubAbort("_windowDeleteButton"); }
bool _windowDeleteRegion(const char* regionName) { serverStubAbort("_windowDeleteRegion"); }
bool _windowDisplay(char* fileName, int x, int y, int width, int height) { serverStubAbort("_windowDisplay"); }
bool _windowDraw() { serverStubAbort("_windowDraw"); }
void _windowEndRegion() { serverStubAbort("_windowEndRegion"); }
bool _windowFill(float r, float g, float b) { serverStubAbort("_windowFill"); }
bool _windowFillRect(int x, int y, int width, int height, float r, float g, float b) { serverStubAbort("_windowFillRect"); }
bool _windowFormatMessage(char* string, int x, int y, int width, int height, int textAlignment) { serverStubAbort("_windowFormatMessage"); }
unsigned char* _windowGetBuffer() { serverStubAbort("_windowGetBuffer"); }
int _windowGetXres() { serverStubAbort("_windowGetXres"); }
int _windowGetYres() { serverStubAbort("_windowGetYres"); }
bool _windowGotoXY(int x, int y) { serverStubAbort("_windowGotoXY"); }
int _windowHeight() { serverStubAbort("_windowHeight"); }
int _windowMoviePlaying() { serverStubAbort("_windowMoviePlaying"); }
int _windowOutput(char* string) { serverStubAbort("_windowOutput"); }
bool _windowPlayMovie(char* filePath) { serverStubAbort("_windowPlayMovie"); }
bool _windowPlayMovieRect(char* filePath, int a2, int a3, int a4, int a5) { serverStubAbort("_windowPlayMovieRect"); }
bool _windowPrintRect(char* string, int a2, int textAlignment) { serverStubAbort("_windowPrintRect"); }
bool _windowRefreshRegions() { serverStubAbort("_windowRefreshRegions"); }
bool _windowSetButtonFlag(const char* buttonName, int value) { serverStubAbort("_windowSetButtonFlag"); }
bool _windowSetMovieFlags(int flags) { serverStubAbort("_windowSetMovieFlags"); }
bool _windowSetRegionFlag(const char* regionName, int value) { serverStubAbort("_windowSetRegionFlag"); }
bool _windowShow() { serverStubAbort("_windowShow"); }
bool _windowShowNamed(const char* name) { serverStubAbort("_windowShowNamed"); }
bool _windowStartRegion(int initialCapacity) { serverStubAbort("_windowStartRegion"); }
void _windowStopMovie() { serverStubAbort("_windowStopMovie"); }
int _windowWidth() { serverStubAbort("_windowWidth"); }
int ambientSoundEffectEventProcess(Object* a1, void* a2) { serverStubAbort("ambientSoundEffectEventProcess"); }
// Benign no-ops: the automap is pure CLIENT presentation (the explored-map overlay).
// automapSaveCurrent persists the current map's automap into the save buffer on a MAP
// TRANSITION (map.cc:1361, result ignored); automapSetDisplayMap marks a map revealed on
// the automap when reached via the worldmap (worldmap.cc:2243). The server never renders
// or reads the automap, so both are no-ops — WITHOUT them a mid-run map transition aborts
// the core-only server (the "load Temple and enter it" crash, 2026-07-18). automapShow
// stays a LOUD abort: it opens the modal automap screen, which the server must never
// reach (the viewer runs that locally) — hitting it would be a real bug, not this class.
int automapSaveCurrent() { return 0; }
void automapSetDisplayMap(int map, bool available) { (void)map; (void)available; }
void automapShow(bool isInGame, bool isUsingScanner) { serverStubAbort("automapShow"); }
// Benign no-op: bufferDrawLine only ever writes color indices into display
// buffers (here, the tileInit hex-grid overlay bitmaps _tile_grid*). The server
// never blits and no logic reads these back, so drawing them is pure waste.
void bufferDrawLine(unsigned char* buf, int pitch, int left, int top, int right, int bottom, int color) { (void)buf; (void)pitch; (void)left; (void)top; (void)right; (void)bottom; (void)color; }
// Benign real body: a pure in-memory rectangle fill (verbatim from draw.cc, no
// SDL). The server only ever fills render buffers it never blits, but keeping
// the true behavior is correct whether or not a buffer is later read.
void bufferFill(unsigned char* buf, int width, int height, int pitch, int a5)
{
    for (int y = 0; y < height; y++) {
        memset(buf, a5, width);
        buf += pitch;
    }
}
int calledShotSelectHitLocationClient(Object* critter, int* hitLocation, int hitMode) { serverStubAbort("calledShotSelectHitLocationClient"); }
void characterEditorReset() { /* Benign: char-editor UI temp state; no editor headless. */ }
// Benign, and exactly faithful: the client body is wholly guarded by
// gColorCycleInitialized, which only colorCycleInit sets and no headless run
// calls — so even the real implementation would do nothing here. Palette
// animation besides. Reached through isoReset, on the load path's gameResetSim.
void colorCycleReset() { }
bool colorPaletteLoad(const char* path) { serverStubAbort("colorPaletteLoad"); }
bool cursorIsHidden() { serverStubAbort("cursorIsHidden"); }
void dialogInit() { /* Benign: empty in dialog.cc (client body is a no-op too). */ }
int dialogSetBorder(int a1, int a2) { serverStubAbort("dialogSetBorder"); }
int dialogSetOptionColor(float a1, float a2, float a3) { (void)a1; (void)a2; (void)a3; /* Benign (A0 flip): dialog reply/option window chrome; the headless _gdialogInitFromScript/ExitFromScript path calls these, no rendering server-side. */ return 0; }
int dialogSetOptionSpacing(int value) { serverStubAbort("dialogSetOptionSpacing"); }
int dialogSetOptionWindow(int a1, int a2, int a3, int a4, char* a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; /* Benign (A0 flip): dialog reply/option window chrome; the headless _gdialogInitFromScript/ExitFromScript path calls these, no rendering server-side. */ return 0; }
int dialogSetReplyColor(float a1, float a2, float a3) { (void)a1; (void)a2; (void)a3; /* Benign (A0 flip): dialog reply/option window chrome; the headless _gdialogInitFromScript/ExitFromScript path calls these, no rendering server-side. */ return 0; }
int dialogSetReplyTitle(const char* a1) { (void)a1; /* Benign (A0 flip): dialog reply/option window chrome; the headless _gdialogInitFromScript/ExitFromScript path calls these, no rendering server-side. */ return 0; }
int dialogSetReplyWindow(int a1, int a2, int a3, int a4, char* a5) { (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; /* Benign (A0 flip): dialog reply/option window chrome; the headless _gdialogInitFromScript/ExitFromScript path calls these, no rendering server-side. */ return 0; }
void endgamePlayMovie() { serverStubAbort("endgamePlayMovie"); }
// Benign: selects which death-ending narration/slideshow to show — presentation
// the server hands to clients, and it picks from gEndgameDeathEndings, which is
// empty here anyway (endgameDeathEndingInit is dropped from serverBoot), so the
// real body would early-return "No endgame death info!" regardless. The
// sim-visible half of the dude's death (_game_user_wants_to_quit = 2) is set by
// the core caller (critter.cc), not here.
void endgameSetupDeathEnding(int reason) { (void)reason; }
// Benign: no hardware cursor headless. Any non-WAIT_PLANET value is fine (the sim
// only compares it and round-trips it through the no-op presenter cursorSet).
int gameMouseGetCursor() { return MOUSE_CURSOR_NONE; }
int gameMouseGetMode() { serverStubAbort("gameMouseGetMode"); }
// Benign: nothing is shown headless.
bool gameMouseObjectsIsVisible() { return false; }
// Benign: _map_init registers this as a ticker (fires every _process_bk beat).
// The real body early-returns while !gGameMouseInitialized, which is always the
// case headless (gameMouseInit is never called), so a no-op matches exactly.
void gameMouseRefresh() { }
void gameMouseRefreshImmediately() { /* Benign: fileRead's load-progress cursor spin; no mouse headless. */ }
int gameMouseReset() { serverStubAbort("gameMouseReset"); }
void gameMouseSetMode(int a1) { serverStubAbort("gameMouseSetMode"); }
// Benign: movies never play headless, so none are "seen" on a fresh boot — which
// is the correct answer for the one sim-affecting caller (_proto_dude_update_gender
// gates the dude's native-look FID on MOVIE_VSUIT). NOTE: gGameMovieFlags is saved
// state; when the save/join-snapshot pipeline lands (P5 step 4), a loaded game's
// real seen-flags must drive this instead of a flat false.
// Benign: movie PLAYBACK is presentation the server has no pipeline for, but the
// seen flag is sim/save state — so mirror game_movie.cc's serverLoopActive()
// branch exactly (mark seen, report success). gameMovieIsSeen is no longer a
// stub at all: game_movie_state.cc owns the ledger in f2_core, so the server
// answers it truthfully. A future NetworkPresenter emits the movie to clients.
int gameMoviePlay(int movie, int flags)
{
    // No longer a stub in the "lie and move on" sense. Three real steps:
    //
    //  1. mark seen — SIM state, and the only part that was ever load-bearing
    //     (scripts branch on it; the vault-suit content gates on it). Marked BEFORE
    //     the barrier so a bail cannot leave the ledger disagreeing with the story.
    //  2. project it to the viewers, who own the playback pipeline;
    //  3. park the tick until somebody reports back (game_movie.h — first ack
    //     releases the room), so the world does not advance under a cutscene.
    //
    // ⚠ The barrier does NOT advance the sim clock, deliberately: vanilla freezes
    // the world for a movie, and a beat that ticked here would make a cutscene a
    // free action for every time-keyed cadence. That is the opposite of the
    // worldmap-driver trap (where a parked clock was a BUG because travel is
    // supposed to consume time) — the difference is whether the modal is meant to
    // cost game time. A movie is not.
    gameMovieMarkSeen(movie);

    // ►► ENV-GATED, DEFAULT OFF (F2_MOVIES=1 to project). Unset, this function is
    // byte-for-byte the old mark-seen-and-continue stub, which is the behaviour
    // every gate and every demo before this feature relied on.
    //
    // The gate exists because the projection has a failure mode with no runtime
    // escape: a viewer that shows a black screen instead of a movie leaves the
    // server parked in the barrier, and `movdone` is a CONTROL-plane verb, so the
    // operator's command channel cannot release it. A feature whose failure needs a
    // server restart must be switchable off without one.
    if (getenv("F2_MOVIES") == nullptr) {
        return 0;
    }

    presenter()->moviePlay(movie, flags);
    gameMovieServerBarrier();
    // The barrier just freed on the FIRST ack (game_movie.h — first skip ends it for
    // the room). Tell every OTHER viewer to stop its local playback NOW; without this
    // the skipper's world advances while the others keep rolling the cutscene until
    // each escapes it themselves ([[movie-playback-coop]]). A viewer with no movie
    // running ignores it.
    presenter()->movieStop();
    return 0;
}
int gameShowDeathDialog(const char* message) { serverStubAbort("gameShowDeathDialog"); }
// Benign: a dedicated server has no interactive UI to gate — it is never
// "UI-disabled" (matches gameUiIsDisabled()==false above), so both are no-ops.
void gameUiDisable(int a1) { }
void gameUiEnable() { }
// Benign: the headless server has no blocking UI, so the game UI is never disabled.
bool gameUiIsDisabled() { return false; }
Object* gmouse_get_outlined_object() { serverStubAbort("gmouse_get_outlined_object"); }
// Benign, and faithful for the same reason as interfaceReset: the whole body is
// guarded on gInterfaceBarWindow != -1 (there is no bar window headless), and
// the one unguarded call, indicatorBarRefresh, is itself an established no-op
// stub below. HUD repaint only — it reads sim state, never writes it.
void interfaceBarRefresh() { }
// Benign, but a BANKED GAP (sibling of interfaceGetCurrentHitMode below): the
// active hand is really per-actor SIM state — scripts branch on it, item use and
// hit-mode selection read it, and interface.cc writes it to the savegame — but
// it lives in gInterfaceCurrentHand inside the HUD TU. HAND_LEFT is exactly the
// value the client initializes and resets it to, so it matches the server's
// effective state for a dude who never swapped hands. Must be relocated to
// f2_core when hand-swapping joins the inbound protocol.
int interfaceGetCurrentHand() { return HAND_LEFT; }
// Benign: no interface bar headless. Return -1 WITHOUT writing the out-params —
// combat.cc:3467 relies on exactly this (pre-inits `aiming=false`, treats -1 as
// "no called-shot"). Was a LATENT server crash (abort would fire on the combat path).
int interfaceGetCurrentHitMode(int* hitMode, bool* aiming) { return -1; }
// Benign: no item-action buttons headless — report none.
int interfaceGetItemActions(int* leftItemAction, int* rightItemAction)
{
    if (leftItemAction != nullptr) *leftItemAction = 0;
    if (rightItemAction != nullptr) *rightItemAction = 0;
    return 0;
}
// Benign HERE, but read the caveat. The body is HUD teardown (interface bar,
// display monitor, indicator bar) which headless has none of — EXCEPT its last
// line, `gInterfaceCurrentHand = 0`, which is sim-relevant: it is the active
// weapon hand that interfaceGetCurrentHitMode reads. The server keeps no
// interface state at all (interfaceLoad above reads the block and discards it),
// so there is nothing here to zero, and this is consistent with that standing
// decision rather than a new gap. ⚠ When the server DOES grow a current-hand,
// this stub has to zero it — see the interfaceGetCurrentHitMode note in the
// cut-list. Reached through isoReset, on the load path's gameResetSim.
void interfaceReset() { }
bool interface_get_current_attack_mode(int* hit_mode) { serverStubAbort("interface_get_current_attack_mode"); }
void isoWindowRefreshRectGame(Rect* rect) { serverStubAbort("isoWindowRefreshRectGame"); }
void isoWindowRefreshRectMapper(Rect* rect) { serverStubAbort("isoWindowRefreshRectMapper"); }
void keyboardReset() { /* Benign: no keyboard headless. */ }
void mouseGetPosition(int* out_x, int* out_y) { serverStubAbort("mouseGetPosition"); }
void mouseHideCursor() { /* Benign: no hardware cursor headless (reached via _gdProcessChoice on the server dialog path). */ }
bool mouseManagerSetMouseShape(char* fileName, int a2, int a3) { serverStubAbort("mouseManagerSetMouseShape"); }
void mouseShowCursor() { /* Benign: no hardware cursor headless (paired with mouseHideCursor on the server dialog path). */ }
int mouse_get_last_buttons() { serverStubAbort("mouse_get_last_buttons"); }
unsigned char* pcxRead(const char* path, int* widthPtr, int* heightPtr, unsigned char* palette) { serverStubAbort("pcxRead"); }
bool pipboyRestHeadless(int hours, int minutes, int kind) { serverStubAbort("pipboyRestHeadless"); }
void renderPresent() { serverStubAbort("renderPresent"); }
int screenGetHeight() { serverStubAbort("screenGetHeight"); }
int screenGetWidth() { serverStubAbort("screenGetWidth"); }
int showDialogBox(const char* title, const char** body, int bodyLength, int x, int y, int titleColor, const char* a8, int bodyColor, int flags) { serverStubAbort("showDialogBox"); }
bool showMesageBox(const char* str) { serverStubAbort("showMesageBox"); }
Sound* soundAllocate(int type, int soundFlags) { serverStubAbort("soundAllocate"); }
void soundContinueAll() { /* Benign: no audio pump headless. */ }
int soundDelete(Sound* sound) { serverStubAbort("soundDelete"); }
bool soundIsPlaying(Sound* sound) { serverStubAbort("soundIsPlaying"); }
int soundLoad(Sound* sound, char* filePath) { serverStubAbort("soundLoad"); }
int soundPause(Sound* sound) { serverStubAbort("soundPause"); }
int soundPlay(Sound* sound) { serverStubAbort("soundPlay"); }
int soundResume(Sound* sound) { serverStubAbort("soundResume"); }
int soundSetCallback(Sound* sound, SoundCallback* callback, void* userData) { serverStubAbort("soundSetCallback"); }
int soundSetChannels(Sound* sound, int channels) { serverStubAbort("soundSetChannels"); }
int soundSetLooping(Sound* sound, int loops) { serverStubAbort("soundSetLooping"); }
int soundStop(Sound* sound) { serverStubAbort("soundStop"); }
int sub_430FD4(const char* a1, const char* a2, int timeout) { serverStubAbort("sub_430FD4"); }
int sub_431088(int a1) { serverStubAbort("sub_431088"); }
int sub_4B7AC4(const char* windowName, int x, int y, int width, int height) { serverStubAbort("sub_4B7AC4"); }
int sub_4B7E7C(const char* windowName, int x, int y, int width, int height) { serverStubAbort("sub_4B7E7C"); }
void textObjectsFree() { serverStubAbort("textObjectsFree"); }
int textObjectsGetCount() { return 0; /* Benign: no floating text headless. */ }
int textObjectsInit(unsigned char* windowBuffer, int width, int height) { return 0; /* Benign: floating text is presenter-driven; skip the render buffer AND the client-side textObjectsTicker registration. */ }
void textObjectsRemoveByOwner(Object* object) { serverStubAbort("textObjectsRemoveByOwner"); }
// Benign: floating text objects are presentation. This MIRRORS the real body
// rather than guessing — textObjectsInit is dropped from serverBoot, so
// gTextObjectsInitialized is false and the real function returns -1 without
// touching anything. Callers (map.cc/object.cc) ignore the result.
int textObjectsReset() { return -1; }
void tileRefreshGame(Rect* rect, int elevation) { serverStubAbort("tileRefreshGame"); }
void tileRefreshMapper(Rect* rect, int elevation) { serverStubAbort("tileRefreshMapper"); }
void tileWindowRefresh() { /* Benign: full-window tile repaint (e.g. tileSetCenter tail); no blit headless. */ }
// Benign: the dirty-rect screen repaint tail of the relocated _dude_stand (and
// other tile mutators). The server never blits, so invalidating a screen rect
// is a no-op; the sim state those mutators set has already been applied.
void tileWindowRefreshRect(Rect* rect, int elevation) { (void)rect; (void)elevation; }
void windowHide(int win) { (void)win; /* Benign: no GNW windows headless; nothing to hide (e.g. _map_exit teardown). */ }
int windowSetFont(int a1) { serverStubAbort("windowSetFont"); }
int windowSetHighlightColor(float a1, float a2, float a3) { serverStubAbort("windowSetHighlightColor"); }
int windowSetTextColor(float a1, float a2, float a3) { serverStubAbort("windowSetTextColor"); }
int windowSetTextFlags(int a1) { serverStubAbort("windowSetTextFlags"); }
void windowShow(int win) { (void)win; /* Benign: no GNW windows headless; nothing to show. */ }
void wmBlinkRndEncounterIcon(bool special) { serverStubAbort("wmBlinkRndEncounterIcon"); }
bool wmCursorIsVisible() { serverStubAbort("wmCursorIsVisible"); }
void wmFadeOut() { serverStubAbort("wmFadeOut"); }
// Benign: sets the worldmap VIEWPORT scroll offsets (wmWorldOffsetX/Y, statics
// of worldmap_ui.cc) and repaints. Pure camera — the party's actual world
// position is wmGenData.worldPosX/Y, which the sim owns and this only reads.
// Reached from wmWorldMap_load, i.e. on every load.
int wmInterfaceCenterOnParty() { return 0; }
void wmInterfaceDialSyncTime(bool shouldRefreshWindow) { serverStubAbort("wmInterfaceDialSyncTime"); }
void wmInterfaceRefreshDate(bool shouldRefreshWindow) { serverStubAbort("wmInterfaceRefreshDate"); }
int wmInterfaceScrollPixel(int stepX, int stepY, int dx, int dy, bool* success, bool shouldRefresh) { serverStubAbort("wmInterfaceScrollPixel"); }

// ============================================================================
// dialog-UI stubs (A0, DIALOG_STREAMING_PLAN) — retires WHOLESALE with the
// game_dialog core/client split (Stage B). game_dialog.cc is linked into
// f2_server for its authoritative choice procs; these are the RENDERER +
// input + audio deps it drags in. Two classes:
//   * benign no-ops — the live headless _gdialogInitFromScript/ExitFromScript/
//     _gdProcess path calls them each dialog; they touch only chrome.
//   * loud aborts — pure render/input; the headless sim must NEVER reach them.
// Data symbols get inert definitions (the render/lips paths that read them are
// themselves unreachable headless).
// ============================================================================

// ---- data (inert) ----
FpsLimiter sharedFpsLimiter;
FpsLimiter::FpsLimiter(unsigned int fps) : _fps(fps), _ticks(0) {}
void FpsLimiter::mark() { serverStubAbort("FpsLimiter::mark"); }
void FpsLimiter::throttle() const { serverStubAbort("FpsLimiter::throttle"); }
FontManagerDrawTextProc* fontDrawText = nullptr;
FontManagerGetLineHeightProc* fontGetLineHeight = nullptr;
int gMusicVolume = 0;
unsigned char gLipsCurrentPhoneme = 0;
bool gLipsPhonemeChanged = false;
LipsData gLipsData;

// ---- benign no-ops (reached by the headless dialog init/exit/process path) ----
int fontGetCurrent() { return 0; }
void fontSetCurrent(int font) { (void)font; }
void _dialogRegisterWinDrawCallbacks(DialogFunc1* a1, DialogFunc2* a2) { (void)a1; (void)a2; }
void colorCycleDisable() { }
void colorCycleEnable() { }
bool indicatorBarShow() { return false; }
bool indicatorBarHide() { return false; }
void gameMouseObjectsShow() { }
void gameMouseObjectsHide() { }
int gameMouseSetCursor(int cursor) { (void)cursor; return 0; }
void _gmouse_enable() { }
void _gmouse_enable_scrolling() { }
void _gmouse_disable_scrolling() { }
int _gsound_background_volume_get_set(int a1) { (void)a1; return 0; }
void backgroundSoundDelete() { }
void backgroundSoundRestart(int value) { (void)value; }
void backgroundSoundSetVolume(int value) { (void)value; }
int soundPlayFile(const char* name) { (void)name; return -1; }

// ---- loud aborts (pure render/input — unreachable headless) ----
void blitBufferToBuffer(unsigned char* src, int width, int height, int srcPitch, unsigned char* dest, int destPitch) { serverStubAbort("blitBufferToBuffer"); }
void blitBufferToBufferTrans(unsigned char* src, int width, int height, int srcPitch, unsigned char* dest, int destPitch) { serverStubAbort("blitBufferToBufferTrans"); }
int buttonCreate(int win, int x, int y, int width, int height, int mouseEnterEventCode, int mouseExitEventCode, int mouseDownEventCode, int mouseUpEventCode, unsigned char* up, unsigned char* dn, unsigned char* hover, int flags) { serverStubAbort("buttonCreate"); }
int buttonDestroy(int btn) { serverStubAbort("buttonDestroy"); }
int buttonDisable(int btn) { serverStubAbort("buttonDisable"); }
int buttonSetCallbacks(int btn, ButtonCallback* pressSoundFunc, ButtonCallback* releaseSoundFunc) { serverStubAbort("buttonSetCallbacks"); }
int buttonSetMouseCallbacks(int btn, ButtonCallback* mouseEnterProc, ButtonCallback* mouseExitProc, ButtonCallback* mouseDownProc, ButtonCallback* mouseUpProc) { serverStubAbort("buttonSetMouseCallbacks"); }
int Color2RGB(Color c) { serverStubAbort("Color2RGB"); }
void convertMouseWheelToArrowKey(int* keyCodePtr) { serverStubAbort("convertMouseWheelToArrowKey"); }
void displayMonitorAddMessage(char* string) { serverStubAbort("displayMonitorAddMessage"); }
int inputGetInput() { serverStubAbort("inputGetInput"); }
int lipsFree() { serverStubAbort("lipsFree"); }
int lipsLoad(const char* audioFileName, const char* headFileName) { serverStubAbort("lipsLoad"); }
int lipsStart() { serverStubAbort("lipsStart"); }
void lipsTicker() { serverStubAbort("lipsTicker"); }
bool _mouse_click_in(int left, int top, int right, int bottom) { serverStubAbort("_mouse_click_in"); }
int mouseGetEvent() { serverStubAbort("mouseGetEvent"); }
bool mouseHitTestInWindow(int win, int left, int top, int right, int bottom) { serverStubAbort("mouseHitTestInWindow"); }
int showQuitConfirmationDialog() { serverStubAbort("showQuitConfirmationDialog"); }
int windowCreate(int x, int y, int width, int height, int color, int flags) { serverStubAbort("windowCreate"); }
void windowDestroy(int win) { serverStubAbort("windowDestroy"); }
void windowDrawText(int win, const char* str, int a3, int x, int y, int a6) { serverStubAbort("windowDrawText"); }
unsigned char* windowGetBuffer(int win) { serverStubAbort("windowGetBuffer"); }
int windowGetWidth(int win) { serverStubAbort("windowGetWidth"); }
int windowGetHeight(int win) { serverStubAbort("windowGetHeight"); }
int windowGetRect(int win, Rect* rect) { serverStubAbort("windowGetRect"); }
void windowRefresh(int win) { serverStubAbort("windowRefresh"); }
void windowRefreshRect(int win, const Rect* rect) { serverStubAbort("windowRefreshRect"); }
int _win_register_button_disable(int btn, unsigned char* up, unsigned char* down, unsigned char* hover) { serverStubAbort("_win_register_button_disable"); }
int _win_set_button_rest_state(int btn, bool checked, int flags) { serverStubAbort("_win_set_button_rest_state"); }
int _win_group_radio_buttons(int buttonCount, int* btns) { serverStubAbort("_win_group_radio_buttons"); }
void _gsound_red_butt_press(int btn, int keyCode) { serverStubAbort("_gsound_red_butt_press"); }
void _gsound_red_butt_release(int btn, int keyCode) { serverStubAbort("_gsound_red_butt_release"); }
void _gsound_med_butt_press(int btn, int keyCode) { serverStubAbort("_gsound_med_butt_press"); }
void _gsound_med_butt_release(int btn, int keyCode) { serverStubAbort("_gsound_med_butt_release"); }

// ---- savegame handlers whose state is genuinely screen-owned ----
// The save FORMAT is shared in both directions (a server-written save must load
// in a stock client and vice versa), so these write their blocks rather than
// skipping them. Skipping would shorten the file and every later handler would
// read at the wrong offset.

// The preferences block goes through the one layout in preferences_state.cc.
// The server has no preferences SCREEN, but `settings.preferences` is real and
// the sim reads it (combat.cc and combat_ai.cc branch on combat_difficulty), so
// the values are taken from and applied to settings rather than defaulted.
int preferencesSave(File* stream)
{
    PreferencesBlock block;
    block.gameDifficulty = settings.preferences.game_difficulty;
    block.combatDifficulty = settings.preferences.combat_difficulty;
    block.violenceLevel = settings.preferences.violence_level;
    block.targetHighlight = settings.preferences.target_highlight;
    block.combatLooks = settings.preferences.combat_looks;
    block.combatMessages = settings.preferences.combat_messages;
    block.combatTaunts = settings.preferences.combat_taunts;
    block.languageFilter = settings.preferences.language_filter;
    block.running = settings.preferences.running;
    block.subtitles = settings.preferences.subtitles;
    block.itemHighlight = settings.preferences.item_highlight;
    block.combatSpeed = settings.preferences.combat_speed;
    block.playerSpeedup = settings.preferences.player_speedup;
    block.textBaseDelay = (float)settings.preferences.text_base_delay;
    block.masterVolume = settings.sound.master_volume;
    block.musicVolume = settings.sound.music_volume;
    block.soundEffectsVolume = settings.sound.sndfx_volume;
    block.speechVolume = settings.sound.speech_volume;
    block.brightness = (float)settings.preferences.brightness;
    block.mouseSensitivity = (float)settings.preferences.mouse_sensitivity;

    return preferencesBlockWrite(stream, block);
}

int preferencesLoad(File* stream)
{
    PreferencesBlock block;
    if (preferencesBlockRead(stream, block) == -1) {
        return -1;
    }

    settings.preferences.game_difficulty = block.gameDifficulty;
    settings.preferences.combat_difficulty = block.combatDifficulty;
    settings.preferences.violence_level = block.violenceLevel;
    settings.preferences.target_highlight = block.targetHighlight;
    settings.preferences.combat_looks = block.combatLooks;
    settings.preferences.combat_messages = block.combatMessages;
    settings.preferences.combat_taunts = block.combatTaunts;
    settings.preferences.language_filter = block.languageFilter;
    settings.preferences.running = block.running;
    settings.preferences.subtitles = block.subtitles;
    settings.preferences.item_highlight = block.itemHighlight;
    settings.preferences.combat_speed = block.combatSpeed;
    settings.preferences.player_speedup = block.playerSpeedup;
    settings.preferences.text_base_delay = block.textBaseDelay;
    settings.sound.master_volume = block.masterVolume;
    settings.sound.music_volume = block.musicVolume;
    settings.sound.sndfx_volume = block.soundEffectsVolume;
    settings.sound.speech_volume = block.speechVolume;
    settings.preferences.brightness = block.brightness;
    settings.preferences.mouse_sensitivity = block.mouseSensitivity;

    return 0;
}

// The interface bar block: 4 fields, all of them pure HUD chrome (bar enabled,
// bar hidden, active hand, end-turn buttons shown). Note the CLIENT's
// interfaceSave returns -1 when the bar window is absent, which headless would
// be every time — that would fail the whole save, so the server writes the
// values a freshly-initialized bar would have instead.
//
// ⚠ gInterfaceCurrentHand is the BANKED GAP noted at interfaceGetCurrentHand
// above: the active hand is really per-actor sim state stranded in the HUD TU.
// HAND_LEFT here matches what the client initializes it to; when hand-swapping
// joins the inbound protocol this must read the actor's real hand instead.
int interfaceSave(File* stream)
{
    if (fileWriteBool(stream, true) == -1) return -1;
    if (fileWriteBool(stream, false) == -1) return -1;
    if (fileWriteInt32(stream, HAND_LEFT) == -1) return -1;
    if (fileWriteBool(stream, false) == -1) return -1;

    return 0;
}

int interfaceLoad(File* stream)
{
    bool interfaceBarEnabled;
    bool interfaceBarHidden;
    int interfaceCurrentHand;
    bool interfaceBarEndButtonsIsVisible;

    if (fileReadBool(stream, &interfaceBarEnabled) == -1) return -1;
    if (fileReadBool(stream, &interfaceBarHidden) == -1) return -1;
    if (fileReadInt32(stream, &interfaceCurrentHand) == -1) return -1;
    if (fileReadBool(stream, &interfaceBarEndButtonsIsVisible) == -1) return -1;

    return 0;
}

// The pipboy block is empty in the client too (_save_pipboy writes nothing), so
// matching it means writing nothing. Kept explicit rather than aliased so a
// future pipboy block does not silently diverge between the two.
int pipboySave(File* stream) { (void)stream; return 0; }
int pipboyLoad(File* stream) { (void)stream; return 0; }

// The server's gameReset IS the sim reset — there is no chrome half to run.
// `gameResetSim` (game.cc) holds every module reset a load needs; the client's
// gameReset is that plus palette/sound/movies/mouse and the screen modules,
// none of which exist here. Not a stub: this is the whole of the operation on
// this side, which is why it is a plain forward rather than a serverStubAbort.
void gameReset() { gameResetSim(); }

// Benign: no audio device headless, so there is no background track to pause.
void backgroundSoundPause() { }
void backgroundSoundResume() { }
// Benign: no HUD indicator bar headless.
int indicatorBarRefresh() { return 0; }
void interfaceBarEndButtonsHide(bool animated) { (void)animated; }

// ============================================================================
// inventory-UI stubs — the barter half of the same story as the dialog-UI block
// above, and it retires the same way (the inventory_ui core/client split).
//
// inventory_ui.cc is linked into f2_server for ONE thing: inventoryOpenTrade's
// serverLoopActive branch, which is authoritative SIM (itemMoveForce /
// barterAttemptTransaction / itemMoveAll). Before it was linked, a dialog option
// that opened a trade reached the aborting stub and killed the server outright,
// because game_dialog.cc — which makes that call — WAS linked.
//
// Everything below is the renderer/input/viewer tail that file drags in. All of
// it is loud: unlike the dialog block there are no benign entries, because the
// headless barter branch skips _setup_inventory and every display_* call by
// construction. If one of these ever fires, the serverLoopActive branch has been
// broken open — which is exactly what we want to hear about, loudly.
//
// The clientViewer* family is the VIEWER's wire reroute (the inventory screen
// sends verbs instead of mutating its mirror). On the server those are nonsense
// by definition: the server IS the authority, so there is nobody to send to.
// ============================================================================
void artRender(int fid, unsigned char* dest, int width, int height, int pitch) { serverStubAbort("artRender"); }
int buttonEnable(int btn) { serverStubAbort("buttonEnable"); }
void clientViewerArmExplosive(int pid, int seconds) { serverStubAbort("clientViewerArmExplosive"); }
bool clientViewerConsumeDudeInvDirty() { serverStubAbort("clientViewerConsumeDudeInvDirty"); }
bool clientViewerConsumeLootTargetInvDirty() { serverStubAbort("clientViewerConsumeLootTargetInvDirty"); }
void clientViewerDrop(Object* item) { serverStubAbort("clientViewerDrop"); }
void clientViewerLootPut(int containerNetId, int pid, int quantity) { serverStubAbort("clientViewerLootPut"); }
void clientViewerLootTake(int containerNetId, int pid, int quantity) { serverStubAbort("clientViewerLootTake"); }
void clientViewerLootTakeAll(int containerNetId) { serverStubAbort("clientViewerLootTakeAll"); }
void clientViewerSetLootTarget(int netId) { serverStubAbort("clientViewerSetLootTarget"); }
void clientViewerUnwield(int hand) { serverStubAbort("clientViewerUnwield"); }
void clientViewerUseItem(int pid) { serverStubAbort("clientViewerUseItem"); }
void clientViewerUseItemOn(int targetNetId, int pid) { serverStubAbort("clientViewerUseItemOn"); }
void clientViewerWield(Object* item, int hand) { serverStubAbort("clientViewerWield"); }
int gameMouseHighlightActionMenuItemAtIndex(int menuItemIndex) { serverStubAbort("gameMouseHighlightActionMenuItemAtIndex"); }
int gameMouseRenderActionMenuItems(int x, int y, const int* menuItems, int menuItemsCount, int width, int height) { serverStubAbort("gameMouseRenderActionMenuItems"); }
int gameMouseRenderPrimaryAction(int x, int y, int menuItem, int width, int height) { serverStubAbort("gameMouseRenderPrimaryAction"); }
int _gmouse_3d_pick_frame_hot(int* a1, int* a2) { serverStubAbort("_gmouse_3d_pick_frame_hot"); }
void inputPauseForTocks(unsigned int ms) { serverStubAbort("inputPauseForTocks"); }
void interfaceRenderArmorClass(bool animate) { serverStubAbort("interfaceRenderArmorClass"); }
int interfaceUpdateItems(bool animated, int leftItemAction, int rightItemAction) { serverStubAbort("interfaceUpdateItems"); }
void mouseGetPositionInWindow(int win, int* x, int* y) { serverStubAbort("mouseGetPositionInWindow"); }
void mouseGetWheel(int* x, int* y) { serverStubAbort("mouseGetWheel"); }
int mouseSetFrame(unsigned char* a1, int width, int height, int pitch, int a5, int a6, char a7) { serverStubAbort("mouseSetFrame"); }
void _mouse_set_position(int x, int y) { serverStubAbort("_mouse_set_position"); }

// The viewer trade window's own deps. inventoryOpenTradeViewer is the one
// function in inventory_ui.cc the server must NEVER call -- it renders what a
// trade looks like, and the server is the thing being looked at. Loud, so a
// mis-route says so rather than quietly half-working.
bool clientBarterActive() { serverStubAbort("clientBarterActive"); }
bool clientDialogActive() { serverStubAbort("clientDialogActive"); }
Object* clientBarterDriver() { serverStubAbort("clientBarterDriver"); }
Object* clientBarterDriverInv() { serverStubAbort("clientBarterDriverInv"); }
Object* clientBarterMerchantInv() { serverStubAbort("clientBarterMerchantInv"); }
bool clientBarterIsDriver() { serverStubAbort("clientBarterIsDriver"); }
bool clientBarterConsumeDirty() { serverStubAbort("clientBarterConsumeDirty"); }
int clientBarterConsumeResult() { serverStubAbort("clientBarterConsumeResult"); }
void clientBarterApplyPending() { serverStubAbort("clientBarterApplyPending"); }
int clientBarterOfferValue() { serverStubAbort("clientBarterOfferValue"); }
int clientBarterAskingValue() { serverStubAbort("clientBarterAskingValue"); }
void clientViewerBarterVerb(const char* verb, int pid, int quantity) { serverStubAbort("clientViewerBarterVerb"); }
void clientModalWindowsSync() { serverStubAbort("clientModalWindowsSync"); }
void clientDialogRenderPendingNode() { serverStubAbort("clientDialogRenderPendingNode"); }

} // namespace fallout

// ---- global-namespace symbols ----
// delay_ms: dialog ticker throttle (game_dialog gameDialogTicker); render-only, unreachable headless.
void delay_ms(int ms) { (void)ms; fallout::serverStubAbort("delay_ms"); }
// ---- sfall keyboard helpers live in the GLOBAL namespace ----
bool sfall_kb_is_key_pressed(int key) { fallout::serverStubAbort("sfall_kb_is_key_pressed"); }
void sfall_kb_press_key(int key) { fallout::serverStubAbort("sfall_kb_press_key"); }
