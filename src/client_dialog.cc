#include "client_dialog.h"

#include <cstdio>
#include <string>
#include <vector>

#include "art.h"
#include "client_barter.h"
#include "client_net.h"
#include "game_dialog.h"
#include "kb.h"
#include "object.h"
#include "obj_types.h"
#include "proto.h"
#include "proto_types.h"

namespace fallout {

// ►► WINDOW OWNERSHIP (viewer-modal design I2/I6). The wire decoder LATCHES state
// only — it never creates or destroys a window. All four gdialog init/exit calls
// (session + node subwindows) are owned by clientModalWindowsSync(), a pure
// function of (dialogActive, barterActive) called once per frame from the main
// loop. This is the single fix for the barter<->dialog window whack-a-mole: the
// three lifecycle drivers (decode, the barter modal, vanilla's mode machine) used
// to fight because decode touched windows at wire-decode time — which also runs
// INSIDE the barter modal's service ticker, so a dialog node/end arriving mid-trade
// tore dialog windows down under the open trade screen.
//
// Two window LEVELS, reconciled independently:
//  - session (background + head, _gdialogInitFromScript): up iff a conversation is
//    live. STAYS UP during barter — the trade screen reuses gameDialogGetWindow()
//    as its backing window, exactly as vanilla nests barter inside the dialog.
//  - node subwindows (reply/options, gameDialogInitNodeWindows): up iff a
//    conversation is live AND no trade is open. Barter REPLACES the option list.
static bool gClientDialogActive = false;      // server says a conversation is live
static bool gClientDialogSessionBuilt = false; // background/head windows are up
static bool gClientDialogNodeBuilt = false;    // reply/option subwindows are up
static bool gClientDialogOptionsHidden = false; // the options list is hidden for a trade
static bool gClientDialogNodePending = false;  // node content needs (re)applying
static bool gClientDialogLipsPending = false;  // a FRESH node wants lipsync played
static int gClientDialogDriverNetId = 0;
static bool gClientDialogWaitingForResponse = false;

// Latched node payload. Owned copies (not the transient wire buffers) so the render
// step can run a frame or more after decode, and so a node can be re-applied into
// freshly rebuilt windows when a trade closes.
static int gPendingSpeakerNetId = 0;
static int gPendingReaction = 0;
static int gPendingHeadFid = -1;
static std::string gPendingReply;
static std::vector<std::string> gPendingOptions;
// Parallel to gPendingOptions: how the listener takes each line. Feeds the vanilla
// renderer's Empathy branch, which colours the list green/grey/red.
static std::vector<int> gPendingOptionReactions;
static std::string gPendingAudio;

// EVENT_DIALOG_NODE handler. LATCHES the node — no window calls (I6). The first
// node marks the conversation live; clientModalWindowsSync() builds the windows on
// the next frame and clientDialogRenderPendingNode() applies this content into them.
void clientDialogOnNode(int speakerNetId, int driverNetId, int reaction,
    const char* reply, const char* const* options, int optionCount,
    const char* audioFileName, int headFid, const int* optionReactions)
{
    gPendingSpeakerNetId = speakerNetId;
    gPendingReaction = reaction;
    gPendingHeadFid = headFid;
    gPendingReply = reply != nullptr ? reply : "";
    gPendingOptions.clear();
    gPendingOptionReactions.clear();
    if (optionCount > 0 && options != nullptr) {
        for (int i = 0; i < optionCount; i++) {
            gPendingOptions.emplace_back(options[i] != nullptr ? options[i] : "");
            gPendingOptionReactions.push_back(
                optionReactions != nullptr ? optionReactions[i] : GAME_DIALOG_REACTION_NEUTRAL);
        }
    }
    gPendingAudio = audioFileName != nullptr ? audioFileName : "";

    gClientDialogDriverNetId = driverNetId;
    gClientDialogActive = true;
    gClientDialogNodePending = true;
    gClientDialogLipsPending = true; // a fresh node from the wire speaks; a rebuild does not
    gClientDialogWaitingForResponse = false;
}

// EVENT_DIALOG_END handler (and the local ESC bail). LATCHES the conversation as
// over — no window calls (I6). clientModalWindowsSync() tears the windows down on
// the next reconcile.
void clientDialogOnEnd()
{
    gClientDialogActive = false;
    gClientDialogNodePending = false;
    gClientDialogLipsPending = false;
    gClientDialogDriverNetId = 0;
    gClientDialogWaitingForResponse = false;
}

// ►► THE SINGLE WINDOW OWNER (I2). Pure function of (dialogActive, barterActive):
// reconciles what windows ARE up against what SHOULD be, and is the ONLY code
// permitted to call the four gdialog init/exit functions. Called once per frame
// from the main loop, and from the barter modal's entry so node subwindows drop
// before the trade window goes up. Idempotent — safe to call every frame.
void clientModalWindowsSync()
{
    bool sessionWanted = gClientDialogActive;
    // ►► THE NODE WINDOWS SURVIVE A TRADE, as they do in vanilla (_gdProcessExit
    // runs at the END of the conversation, never at the barter boundary). Only the
    // OPTIONS list conflicts with the trade window, and it is hidden below rather
    // than destroyed. Tearing the pair down for a trade is what made an item
    // description unrenderable during barter — the handler's target window was
    // gone, which is the fault that got it routed to a no-op sink.
    bool nodeWanted = gClientDialogActive;

    // Build order: session (background/head) before node subwindows, because the
    // node windows draw into the session window's frame.
    if (sessionWanted && !gClientDialogSessionBuilt) {
        Object* speaker = objectFindByNetId(gPendingSpeakerNetId);
        if (speaker == nullptr) {
            // Speaker netId not in the map yet (a node can arrive a frame before the
            // object baseline settles). Leave everything latched and retry next
            // frame rather than dropping the conversation.
            return;
        }
        // headFid + reaction come off the WIRE (the script's own start_gdialog args,
        // captured server-side). Assign the GLOBALS, not just the init args:
        // _gdialogInitFromScript never stores headFid, and gameDialogStartLips /
        // _gdSetupFidget read gGameDialogHeadFid to locate the head art for lipsync
        // and the idle fidget. See client_dialog history for the Arroyo Elder repro.
        gGameDialogSpeaker = speaker;
        gGameDialogHeadFid = gPendingHeadFid;
        gGameDialogFidget = gPendingReaction;
        _gdialogInitFromScript(gPendingHeadFid, gPendingReaction);
        gClientDialogSessionBuilt = true;
    }

    if (nodeWanted && !gClientDialogNodeBuilt) {
        // _gdialogInitFromScript builds head/background but NOT the reply/option
        // subwindows — those come from _gdProcessInit, which the viewer bypasses.
        gameDialogInitNodeWindows();
        gClientDialogNodeBuilt = true;
        gClientDialogNodePending = true; // re-apply the current node into the fresh windows
    } else if (!nodeWanted && gClientDialogNodeBuilt) {
        gameDialogExitNodeWindows();
        gClientDialogNodeBuilt = false;
        gClientDialogOptionsHidden = false;
    }

    // The options list is where the trade window goes, and it is WINDOW_MOVE_ON_TOP,
    // so it must be HIDDEN for a trade rather than merely covered — that leftover
    // list drawing over the barter background was the reported "dialog leftovers
    // blended". Toggled on CHANGE only: windowShow/windowHide every frame would
    // dirty the region for nothing.
    if (gClientDialogNodeBuilt) {
        bool wantHidden = clientBarterActive();
        if (wantHidden != gClientDialogOptionsHidden) {
            gameDialogSetOptionsWindowVisible(!wantHidden);
            gClientDialogOptionsHidden = wantHidden;
        }
    }

    // Teardown order: node subwindows come down (above) before the session window.
    if (!sessionWanted && gClientDialogSessionBuilt) {
        _gdialogExitFromScript();
        gClientDialogSessionBuilt = false;
        gClientDialogNodePending = false;
        gClientDialogLipsPending = false;
    }
}

// Apply the latched node content into the node subwindows. Runs after
// clientModalWindowsSync() each frame so the windows are guaranteed built. Lips
// play only for a FRESH node from the wire, never for a node re-applied because a
// trade closed and the subwindows were rebuilt (no re-speak on barter return).
void clientDialogRenderPendingNode()
{
    if (!gClientDialogNodePending || !gClientDialogNodeBuilt) {
        return;
    }

    gameDialogClearOptions();
    gameDialogSetReplyText(gPendingReply.c_str());
    for (size_t i = 0; i < gPendingOptions.size(); i++) {
        // ►► THE REACTION IS THE AUTHORITY'S, NOT A CONSTANT. This used to pass a
        // hardcoded 50 (NEUTRAL) for every option, which meant the Empathy perk — whose
        // whole visible effect is colouring this list — did nothing on a viewer. The
        // renderer's Empathy branch (game_dialog.cc) was always there, reading exactly
        // this field; it was being fed a lie.
        int optionReaction = i < gPendingOptionReactions.size()
            ? gPendingOptionReactions[i]
            : GAME_DIALOG_REACTION_NEUTRAL;
        gameDialogAddTextOptionWithProc(-1, gPendingOptions[i].c_str(), 0, optionReaction);
    }

    // Trigger the vanilla render pipeline now that reply/option globals are seeded.
    // On the viewer we bypass _gdProcess (it would execute scripts locally), so we
    // call the pure-render half directly.
    gameDialogRenderNode();

    if (gClientDialogLipsPending && !gPendingAudio.empty()) {
        gameDialogStartLips(gPendingAudio.c_str());
    }
    gClientDialogLipsPending = false;
    gClientDialogNodePending = false;
}

bool clientDialogActive()
{
    return gClientDialogActive;
}

bool clientDialogHandleKey(int keyCode)
{
    if (!gClientDialogActive) {
        return false;
    }

    // Only the owning viewer (gDude->netId == driverNetId) can drive the
    // conversation; spectators are read-only. See DIALOG_STREAMING_PLAN "CO-OP
    // DIALOG refinement" — gate on actor-ownership, never hardcode "viewer 0".
    bool isOwner = (gDude != nullptr && gDude->netId == gClientDialogDriverNetId);

    // ►► ESCAPE BAIL — DRIVER ONLY.
    //
    // For the DRIVER this is an always-live safety valve, deliberately ahead of the
    // waiting-for-response latch below. While the dialog window is open main.cc skips
    // the entire gameplay input block AND its own ESC-quits-the-viewer branch (both
    // gated on clientDialogActive()), so if the latch is set and the server's next
    // dialog event never arrives, every key returns false and the only way out is
    // killing the process (a real observed softlock: denbus2/Sheila's paid path). Tear
    // down LOCALLY rather than waiting for the echo; if a node arrives later the window
    // just reopens. The sync call takes the windows down THIS frame so no gameplay
    // input leaks under a live dialog window.
    //
    // For a SPECTATOR, ESC does NOTHING (consume it). A spectator's window lifetime is
    // SERVER-owned: it must close only on EVENT_DIALOG_END, together with everyone
    // else's. Tearing it down locally desyncs them from a conversation the server is
    // still driving — and the next event that assumes a live dialog window (a resumed
    // node, or EVENT_BARTER_BEGIN building the trade screen on the dialog background)
    // then dereferences a window that is gone: an observed segfault. If the driver
    // drops and the conversation genuinely wedges, that is the server-side host-transfer
    // fix (dialog-driver-ownership), not a local escape hatch that crashes.
    if (keyCode == KEY_ESCAPE) {
        if (isOwner) {
            clientViewerDialogEnd();
            clientDialogOnEnd();
            clientModalWindowsSync();
        }
        return true;
    }

    if (gClientDialogWaitingForResponse) {
        return false;
    }

    if (!isOwner) {
        return true; // consume the key but do nothing
    }

    // 'T' key: return from barter sub-screen to dialog (the barter window's
    // "Talk" button posts KEY_LOWERCASE_T, but _gdProcess mode-3 branch —
    // which calls inventoryOpenTrade — is skipped on the viewer).
    if (keyCode == KEY_LOWERCASE_T) {
        _barter_end_to_talk_to();
        // gameDialogTicker processes _dialogue_switch_mode=1 next tick:
        // destroys barter window, recreates dialog, unhides reply/options.
        return true;
    }

    // Mouse hover highlight: window manager posts 1200+N on enter, 1300+N exit.
    if (keyCode >= 1200 && keyCode <= 1250) {
        gameDialogOptionHoverEnter(keyCode - 1200);
        return true;
    }
    if (keyCode >= 1300 && keyCode <= 1330) {
        gameDialogOptionHoverExit(keyCode - 1300);
        return true;
    }

    // ESCAPE is handled above, unconditionally. '0' is the ordinary "end the
    // conversation" reply and stays on the normal latched path.
    if (keyCode == '0') {
        clientViewerDialogEnd();
        gClientDialogWaitingForResponse = true;
        return true;
    }

    // OPTION SCROLLING (the panel is fixed-size and the renderer drops what does not fit).
    // Both the page keys and the arrows/wheel, since a viewer does not run the reply's own
    // paging state and so has nothing to compete with.
    if (keyCode == KEY_PAGE_DOWN || keyCode == KEY_ARROW_DOWN) {
        gameDialogScrollOptions(1);
        return true;
    }
    if (keyCode == KEY_PAGE_UP || keyCode == KEY_ARROW_UP) {
        gameDialogScrollOptions(-1);
        return true;
    }

    // ►► THE WHOLE OPTION RANGE, not just '1'-'9'. Vanilla's option buttons post
    // `49 + index`, so with ten or more options — which crowded co-op dialogs reach, and
    // which is exactly when scrolling matters — clicking the tenth did nothing at all on a
    // viewer: the keycode fell outside the digit range and was dropped. Bounded by the
    // real option count so a stray key cannot index past the list. (Past 16 options the
    // codes reach the letter keys; that overlap is vanilla's own numbering scheme, and 'T'
    // — barter — is claimed above this.)
    int optionCount = (int)gPendingOptions.size();
    if (optionCount > 0 && keyCode >= 49 && keyCode < 49 + optionCount) {
        int index = keyCode - 49;
        clientViewerDialogSay(index);
        gClientDialogWaitingForResponse = true;
        return true;
    }

    return true;
}

} // namespace fallout
