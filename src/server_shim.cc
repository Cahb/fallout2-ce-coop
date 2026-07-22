// f2_server behavior shims (P5-C Step 1 slice 1d, [[p5-server-plan]]).
//
// COMPANION to server_stubs.cc, but the OPPOSITE kind of symbol. server_stubs.cc
// resolves f2_client symbols the core still reaches with LOUD ABORTS — the
// enumerated, monotonically-shrinking severance dashboard. THIS file holds the
// small set of symbols whose server behavior is GENUINELY DIVERGENT from the
// client: the headless server does not just fail to have a window/dialog/movie
// subsystem, it deliberately runs a different, reduced version. Those cannot be
// an abort (the core calls them every beat) and must not be a byte-identical
// extraction into f2_core (the behavior differs), so per the one-clean-seam rule
// ([[p5-server-plan]] SEAM STRATEGY) they live here as a LABELED per-side shim,
// never as a serverLoopActive() branch inside a durable core/client file.
//
// Signatures are lifted verbatim from the declaring f2_client headers so the
// compiler validates each shim against the real prototype and the linker
// confirms exactly one definition wins.
//
// Slice 1d shims (4):
//   * _process_bk        — the background pump. Client _process_bk (input.cc)
//     runs tickersExecute() THEN polls the mouse/buttons/keyboard for input
//     events. The server has no such input surface (its "input" is the command
//     FIFO / socket), so the shim is tickersExecute() ONLY. This is not a subset
//     of the client pump behind a flag — it is a different pump, hence a shim,
//     not a guard. tickersExecute() still drives every registered ticker,
//     including _doBkProcesses (script bk) and _object_animate (the
//     InstantAnimationScheduler drain), so the combat/interpreter spin-loops
//     that busy-wait `while (animationIsBusy(x)) _process_bk();`
//     (combat.cc:2589/2762, combat_ai.cc:3164/3210, interpreter_lib.cc:446) still
//     terminate once the animation family is de-stubbed — the reduced pump keeps
//     the drain route intact (audit banked in the plan).
//   * _updateWindows     — no-op. There are no windows to repaint headless.
//   * _gdialogActive      — false. The server pump never enters a modal g-dialog
//     session (a live player's dialog is the future DialogSession refactor, open
//     risk (c)); _doBkProcesses / _script_chk_critters gate critter/timed-event
//     processing on !_gdialogActive(), so "false" = "keep the sim running."
//   * gameMovieIsPlaying  — false. The server never plays movies (gameMoviePlay
//     is already headless-guarded, H-30); _doBkProcesses gates the same critter/
//     timed-event block on !gameMovieIsPlaying(), so "false" keeps it running.

#include "game_dialog.h"
#include "game_movie.h"
#include "input.h"
#include "window.h"

namespace fallout {

// The server background pump: drive the tickers, nothing else. No mouse/button/
// keyboard polling — the server's inbound surface is the command queue, not a
// device. See the header note for why this is a shim, not a guarded subset.
void _process_bk()
{
    tickersExecute();
}

// Headless: no window manager, nothing to repaint. _doBkProcesses calls this
// every beat; on the client it flushes dirty windows to the screen.
void _updateWindows()
{
}

// The server never plays movies (playback is headless-guarded upstream). False
// keeps the _doBkProcesses critter/timed-event block live.
bool gameMovieIsPlaying()
{
    return false;
}

} // namespace fallout
