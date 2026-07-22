#ifndef CLIENT_NET_H
#define CLIENT_NET_H

namespace fallout {

struct Object;

// STEP-4 joining client (CLIENT_JOIN_DESIGN.md §D — the inverse of the
// NetworkPresenter encoder). The decoder loads the join snapshot BLOB, then
// applies the live F2NS event stream, mutating the local sim into the server's
// reconstructed state. It runs INSIDE the client binary (headless probe or the
// real SDL viewer), so it has the full sim to mutate. The frame/event walker is
// source-agnostic: it consumes bytes from a file (S2) or a socket (S3) through
// the same IncrementalStream.

// S2 file-sourced consumer: reads a captured F2NS wire stream from `path`, loads
// the join snapshot, applies the live stream, and leaves the sim in the
// reconstructed state (ready for a state_dump). `blobTmpPath` is scratch used to
// reassemble the blob before mapLoad. Returns false on a framing error, a
// blob-guard mismatch, or a load failure.
bool clientApplyStreamFile(const char* path, const char* blobTmpPath);

// S3 live viewer connection (CLIENT_JOIN_DESIGN.md §A). A TCP ByteSource that
// connects to a running f2_server, receives the join snapshot + live wire, and
// feeds the SAME decoder incrementally. The SDL viewer loop (main.cc) holds one
// of these: it pump()s each frame and render()s. Socket transport is POSIX-only
// (mirrors server_net.cc); on Windows connect() fails and the viewer is
// unsupported (the game itself is unaffected).
// Set by mainClientViewer for the process lifetime. The §E design doc reserves
// this as the viewer's backstop flag; today it gates focus behavior — a viewer
// must keep pumping and rendering while UNFOCUSED (inputGetInput's
// _GNW95_lost_focus block would freeze the render AND stop consuming the
// socket, backpressuring the server; N windowed viewers side-by-side need all
// of them live at once).
void clientViewerSetActive(bool active);
bool clientViewerActive();

class ClientConnection {
public:
    ClientConnection();
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    // Open a blocking TCP connection to host:port (TCP_NODELAY, SIGPIPE ignored).
    // `blobTmpPath` is the scratch file the decoder reassembles the join blob into
    // (its bytes must outlive the connection). Returns false on any resolve/socket/
    // connect error, leaving the connection closed.
    bool connect(const char* host, int port, const char* blobTmpPath);

    // Non-blocking: recv whatever has arrived and apply every now-complete frame.
    // Returns false on a fatal framing error or a server disconnect (the caller
    // should tear down the viewer); true otherwise, including "no data yet".
    bool pump();

    // Send one upstream control line (STEP 6). A trailing '\n' is appended (the
    // server's v1 framing is newline-delimited text). Returns false if the write
    // failed — the connection is marked dead like pump() does, so the caller's
    // next pump() tears the viewer down. No-op-false when not connected.
    bool sendLine(const char* line);

    // True once the join snapshot blob has been fully received and loaded (the
    // world is present). The viewer blocks on this before entering its render loop.
    bool blobLoaded() const;

    // How many times a join blob has been applied (world loads). Increments on
    // every rebaseline broadcast (another viewer joined / map transition), not
    // just the first — the viewer re-applies its puppet levers when it changes,
    // because mapLoad re-registers the animation tickers each time.
    int loadCount() const;

    // Combat framing decoded from the wire (P3, presentation-only). The viewer
    // reads these to route a click (out-of-combat mv vs in-combat cmove) and to
    // cue the player on their turn. False until the first combatEnter; reset on
    // combatExit and map transitions.
    bool inCombat() const;
    bool myTurn() const;

    // Drive combat presentation once per render frame (viewer only): start the next
    // queued attack replay when the previous one is idle, and apply the deferred
    // end-of-combat chrome once the queue drains. No-op headless / when disconnected.
    void presentationTick();

    // Recompute vanilla combat outlines from the current turn + mouse mode (viewer
    // only, #8). Idempotent; the render loop calls it after a combat mouse-mode switch
    // (crosshair ⇄ move/view toggles the target highlight). Combat/turn/move-driven
    // recomputes happen internally on decode. No-op headless / when disconnected.
    void recomputeCombatOutlines();

    // True while the viewer still owes combat animation — a replay in flight, more
    // attacks queued, or the end-of-combat chrome deferred behind them. The render
    // loop shows the wait cursor and locks combat input while this holds.
    bool combatPresentationBusy() const;

    // A rebaseline arrived while a viewer modal screen was open and is buffered (onBlobEnd
    // deferred it so mapLoad can't free gDude under the modal's pointers). The main loop
    // applies it via applyDeferredBlob() once no modal is up. See clientViewerInstallServiceTicker.
    bool blobDeferred() const;
    void applyDeferredBlob();

    // In-combat inventory grant (Stage 4). In combat the inventory OPEN is the
    // priced act (4 AP, 2 with Quick Pockets), so the viewer asks with `invopen`
    // and the server answers with EVENT_INVENTORY_GRANT once it has checked the
    // turn and taken the AP. takeInventoryGrant() consumes that one-shot answer;
    // the main loop opens the screen at its no-modal-open point, because the
    // screen runs a blocking loop and must not be entered from inside pump().
    // setCombatModalOpen() brackets it so the service ticker's combat
    // force-ESC skips the one screen the server just charged us for.
    bool takeInventoryGrant();
    void setCombatModalOpen(bool open);
    bool combatModalOpen() const;

    bool connected() const;
    void close();

private:
    struct Impl;
    Impl* _impl;
};

// Register the viewer's live upstream connection so shared combat code can forward
// verbs through it. Set once by mainClientViewer after connect; passing nullptr is
// safe (a subsequent commit becomes a no-op).
void clientViewerSetConnection(ClientConnection* conn);

// Register the per-iteration service ticker that keeps the wire pumping inside viewer modal
// loops (inventory/skilldex/char/pipboy) and force-closes them on combat entry / rebaseline.
// Call once after clientViewerSetConnection. See viewerServiceTicker in client_net.cc.
void clientViewerInstallServiceTicker();

// Tear down the service ticker at viewer shutdown (unregister + drop the captured conn),
// so it can't fire against a destroyed connection. Pair with clientViewerInstallServiceTicker.
void clientViewerRemoveServiceTicker();

// Forward a fully-selected attack upstream as `cattack <netId> <hitMode> <hitLoc>`
// (COMBAT_CLIENT_DESIGN.md §3.b). Installed as combatSetViewerAttackHook's target by
// mainClientViewer; a no-op if no connection is registered or target is null. The
// server re-validates the mode/location, so this is a request, not a trusted action.
void clientViewerCommitAttack(Object* target, int hitMode, int hitLocation);

// Return (and clear) whether the last clientViewerCommitAttack actually sent a verb.
// The viewer arms its input-lock only when true, so a crosshair click that resolved
// to a bad-shot message / range failure / picker-cancel (no verb sent) does not
// freeze combat input for the round-trip timeout.
bool clientViewerTakeAttackCommitted();

// Dude inventory verbs (player-UI Slice 3b). The inventory screen reroutes its
// drag-drop / ctx-menu DROP resolution through these instead of mutating the local
// mirror — the server runs the authoritative _inven_wield/_inven_unwield/
// itemDropStack and streams the result back. hand: 0 = left, 1 = right, 2 = armor
// (invunwield only; invwield auto-detects armor and ignores the hand). No-ops when
// no connection is registered.
void clientViewerWield(Object* item, int hand);
void clientViewerUnwield(int hand);
void clientViewerDrop(Object* item);

// Tell the server this viewer is done with the movie it was shown (finished or
// skipped). The server's movie barrier releases on the FIRST ack it gets, so this
// ends the cutscene for the whole room by design (game_movie.h).
void clientViewerMovieAck();
void clientViewerUseItem(int pid);
void clientViewerUseItemOn(int targetNetId, int pid);
void clientViewerArmExplosive(int pid, int seconds);
void clientViewerLootTake(int containerNetId, int pid, int quantity);
void clientViewerLootPut(int containerNetId, int pid, int quantity);
void clientViewerLootTakeAll(int containerNetId);
void clientViewerSetLootTarget(int netId);
bool clientViewerConsumeLootTargetInvDirty();

// Dialog viewer verbs (A3, DIALOG_STREAMING_PLAN Stage 3). Send the option
// selection or dialog-end command upstream over the viewer's control connection.
// The server routes these through serverControlLine into the dialog intent queue,
// which the block-and-pump barrier drains. No-op when no connection is registered.
// spectatorGuard: if true (the default), only sends when the viewer owns the dialog
// (gDude->netId == driverNetId); false to bypass (ESC/dend always allowed).
void clientViewerDialogSay(int index);
void clientViewerDialogEnd();

// Worldmap viewer verbs: send the travel-direction intent upstream. The server
// routes these through serverControlLine into the worldmap intent queue, which the
// block-and-pump barrier drains. No-op when no connection is registered.
void clientViewerWmMove(int x, int y);
void clientViewerWmEnter();
void clientViewerWmEscape();

// True (once) if a live dude-inventory reconcile mutated the mirror while a screen was
// open; the open inventory loop polls this to repaint its list (it otherwise repaints
// only on user events, so an async drop/consume would linger visibly). Self-clearing.
bool clientViewerConsumeDudeInvDirty();

// Free the item Objects the reconcile UNLINKED while a modal was open but deferred
// destroying (an open inventory handler may hold a raw Object* across its inner pump).
// Call after the inventory screen has closed, and at ticker teardown.
void clientViewerFlushDeferredItemFrees();

// Send one barter verb upstream (boffer/btake/bunoffer/bcommit/bdone/bcancel).
// pid < 0 omits the arguments, for the arg-less verbs. Only the driver should
// call this -- the server refuses everyone else, so sending anyway would just
// earn a refusal message.
void clientViewerBarterVerb(const char* verb, int pid, int quantity);

} // namespace fallout

#endif /* CLIENT_NET_H */
