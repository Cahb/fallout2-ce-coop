#ifndef CLIENT_SAY_H
#define CLIENT_SAY_H

namespace fallout {

// ─── Player chat ("SAY") — a NON-BLOCKING viewer overlay ─────────────────────
//
// Press ENTER out of combat, type, ENTER sends / ESC cancels. The server echoes
// the line to everyone as floating text over the speaker's head plus a styled
// chat line in the message log.
//
// ►► DELIBERATELY NOT A MODAL. Per the viewer-modal design of record, a NEW
// screen copies DIALOG's overlay shape (a flag plus main-loop dispatch), never
// barter's blocking loop: an enter/tick/exit shape needs no kViewerModalMask
// entry and structurally cannot starve the wire, so no service ticker, no
// force-close plumbing, and no third window-lifecycle driver to fight. Closing
// it is just clearing a flag.
//
// The whole feature is client-local input: the only thing that leaves this
// machine is one `say <text>` verb. Nothing here touches the sim.

// Is the input box open? While true the main loop routes keys here and must not
// dispatch them as gameplay.
bool clientSayActive();

// Open the box (no-op if already open). Clears any previous draft.
void clientSayOpen();

// Close and DISCARD the draft. Idempotent — this is the force-close path used
// when combat starts, so it must be safe to call unconditionally.
void clientSayCancel();

// Feed one keycode from inputGetInput(). Returns true if the key was consumed
// (the caller must then not treat it as a gameplay key).
//
// ENTER sends a non-empty draft and closes; ESC cancels; BACKSPACE deletes; any
// printable ASCII appends up to the length cap. Everything else is swallowed
// while the box is open — typing "A" must not toggle combat.
bool clientSayHandleKey(int keyCode);

// Draw the box if open. Called once per frame from the viewer main loop, after
// the world has been drawn.
void clientSayRender();

} // namespace fallout

#endif /* CLIENT_SAY_H */
