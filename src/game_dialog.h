#ifndef GAME_DIALOG_H
#define GAME_DIALOG_H

#include <cstddef>
#include <functional>

#include "interpreter.h"
#include "obj_types.h"

namespace fallout {

extern Object* gGameDialogSpeaker;
extern bool gGameDialogSpeakerIsPartyMember;
extern int gGameDialogHeadFid;
extern int gGameDialogSid;

int gameDialogInit();
int gameDialogReset();
int gameDialogExit();
bool _gdialogActive();
void gameDialogEnter(Object* speaker, int a2);
void _gdialogSystemEnter();
void gameDialogStartLips(const char* a1);
int gameDialogEnable();
int gameDialogDisable();
int _gdialogInitFromScript(int headFid, int reaction);
int _gdialogExitFromScript();
void gameDialogSetBackground(int a1);
void gameDialogRenderSupplementaryMessage(char* msg);
int _gdialogStart();
int _gdialogSayMessage();
int gameDialogAddMessageOptionWithProcIdentifier(int messageListId, int messageId, const char* a3, int reaction);
int gameDialogAddTextOptionWithProcIdentifier(int messageListId, const char* text, const char* a3, int reaction);
int gameDialogAddMessageOptionWithProc(int messageListId, int messageId, int proc, int reaction);
int gameDialogAddTextOptionWithProc(int messageListId, const char* text, int proc, int reaction);
bool gameDialogGetOptionText(int index, char* out, size_t size);

// Seed the reply/options statics from the wire (viewer dialog render, A3). The
// viewer never resolves through message lists — it receives raw text from the
// server. Clear first (gameDialogClearOptions), set the reply, then add each
// option via gameDialogAddTextOptionWithProc (messageListId=-4, proc=0).
void gameDialogSetReplyText(const char* text);
void gameDialogClearOptions();

// Render the current reply and options into the dialog sub-windows (call from the
// viewer after seeding wire text). No-op on the server / when dialog is closed.
void gameDialogRenderNode();

// Create the reply and options sub-windows + scroll buttons (the render half of
// _gdProcessInit). Call once after _gdialogInitFromScript on the viewer — the
// vanilla _gdProcess creates these at conversation entry; on the viewer we bypass
// _gdProcess so we must create them explicitly. No-op on the server.
int gameDialogInitNodeWindows();

// Destroy the reply and options sub-windows + option buttons (the teardown half of
// _gdProcessExit). Call once before _gdialogExitFromScript on the viewer. No-op
// on the server.
void gameDialogExitNodeWindows();

// Viewer-side barter window lifecycle (see the .cc note). The viewer bypasses
// _gdProcess, so the trade window's normal mode-switch construction never runs.
int gameDialogInitBarterWindows();
void gameDialogExitBarterWindows();

// Viewer-side NORMAL dialog control subwindow (the lower window carrying the
// Barter/Review buttons). The twin of the barter pair above: gGameDialogWindow is
// ONE handle with two mutually-exclusive flavors, and the viewer bypasses the
// _gdProcess mode machine that swaps between them. Entering barter must destroy the
// normal control first (else it leaks and its Barter button stays live over the
// trade screen); leaving barter must recreate it (else the handle is -1 and the
// next node render dereferences a dead window — a segfault). Init also restores
// _dialogue_state to the NORMAL value so the eventual teardown frees the right
// flavor. No-op on the server.
int gameDialogInitControlWindow();
void gameDialogExitControlWindow();

// The dialog window handle — inventory_ui needs it as the barter backing window.
int gameDialogGetWindow();

// True while the dialog session (background/head) window is up. The viewer's trade
// screen nests in it; opening barter without it faults. No-op-safe on the server.
bool gameDialogBackgroundActive();

// Mouse hover highlight on dialog options (call from the viewer's key handler
// for keyCode events in the 1200-1250 enter / 1300-1330 exit ranges that the
// window manager posts when the cursor enters/leaves an option button).
void gameDialogOptionHoverEnter(int index);
void gameDialogOptionHoverExit(int index);

// Install the dedicated server's dialog block-and-pump hook (DIALOG_STREAMING_PLAN.md
// Stage 2). While an actor is in conversation the server tick BLOCKS inside _gdProcess;
// this hook is called repeatedly there to service the control channel (so the owner's
// dsay/dend reach the intent queue) and returns false to BAIL the conversation
// (owner disconnect / combat / quit). nullptr (client, golden harness) → the barrier
// keeps its original headless behavior (empty queue ends the conversation at once).
void gameDialogSetServerPump(std::function<bool()> pump);

// True while _gdProcess is running a conversation that can consume a dialog
// intent — i.e. the ONLY correct predicate for "may this dsay/dend be accepted".
//
// ►► Do NOT use _gdialogActive() for that. It reports _dialog_state_fix, which
// only gameDialogEnter (the engine's TALK-on-an-NPC path) ever sets. A script
// that runs a conversation via gsay_start/gsay_end reaches _gdProcess through
// _gdialogGo() with that flag still 0, so the gate rejected input for a live,
// parked conversation and deadlocked the server (denbus2/Sheila's second
// conversation, after the teleport + game_time_advance).
bool gameDialogServerNodeActive();

extern char gDialogPendingAudio[16];

// Script-supplied reaction/fidget level for the live conversation (see game_dialog.cc).
extern int gGameDialogFidget;

int gameDialogSetMessageReply(Program* a1, int a2, int a3);
int gameDialogSetTextReply(Program* a1, int a2, const char* a3);
int _gdialogGo();
void _gdialogUpdatePartyStatus();
void _talk_to_critter_reacts(int a1);
void gameDialogSetBarterModifier(int modifier);
int gameDialogBarter(int modifier);
void _barter_end_to_talk_to();

} // namespace fallout

#endif /* GAME_DIALOG_H */
