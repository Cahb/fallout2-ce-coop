#ifndef FALLOUT_CLIENT_DIALOG_H_
#define FALLOUT_CLIENT_DIALOG_H_

namespace fallout {

// Called from the EVENT_DIALOG_NODE handler on the viewer. LATCHES the node payload
// (speaker/driver netIds, reaction, reply + option texts, audio, headFid) and marks
// the conversation live — it does NOT touch windows (see clientModalWindowsSync).
// driverNetId gates editability: the controlling viewer (its gDude->netId ==
// driverNetId) has editable options; other viewers are read-only spectators.
void clientDialogOnNode(int speakerNetId, int driverNetId, int reaction,
    const char* reply, const char* const* options, int optionCount,
    const char* audioFileName = nullptr, int headFid = -1);

// Called from the EVENT_DIALOG_END handler (and the local ESC bail). LATCHES the
// conversation as over — clientModalWindowsSync() tears the windows down.
void clientDialogOnEnd();

// ►► THE SINGLE OWNER OF ALL DIALOG WINDOW TRANSITIONS (viewer-modal design I2).
// A pure function of (dialogActive, barterActive): builds/destroys the dialog
// session windows and node subwindows to match the latched state. The ONLY code
// permitted to call gameDialog{Init,Exit}NodeWindows / _gdialog{Init,Exit}FromScript.
// Call once per frame from the main loop, and from the barter modal's entry.
// Idempotent.
void clientModalWindowsSync();

// Apply the latched node content into the node subwindows. Call after
// clientModalWindowsSync() each frame; no-op unless a node is pending and its
// windows are built.
void clientDialogRenderPendingNode();

// True while a conversation is live on this viewer (regardless of window state).
bool clientDialogActive();

// Handle a keypress while the dialog is open. Number keys 1-9 send "dsay <index>"
// upstream; ESC (or '0') sends "dend". Gated on ownership (only the driving viewer
// can send) and rate-limited (ignored while waiting for the server's reply).
// Returns true if the key was consumed (the caller should NOT process it further).
bool clientDialogHandleKey(int keyCode);

} // namespace fallout

#endif