#ifndef FALLOUT_PRESENTER_NETWORK_H_
#define FALLOUT_PRESENTER_NETWORK_H_

namespace fallout {

// The dedicated server's outbound wire (MP_PROTOCOL.md §2), STEP 2 "MAKE IT TALK".
//
// NetworkPresenter is the third Presenter subclass and the first real consumer of
// the widened state seam: ClientPresenter overrides only PRESENTATION (33 methods,
// zero state), NarratePresenter serializes a human-readable debug trace, and this
// one serializes the STATE + CONTROL events to the binary encoding a socket carries.
//
// PLACEMENT (deliberate, [[p5-server-plan]] seam taxonomy): the ENCODER lives in
// f2_core and the TRANSPORT does not. Encoding sim events is pure shared policy —
// and, decisively, f2_core is the only place the CLIENT-PROBE binary can reach, and
// the probe is the only binary with a golden oracle (the replay/narrate gates run
// build/fallout2-ce, not f2_server, whose sole oracle is "it didn't abort"). An
// encoder in f2_server would be unverifiable. A socket in f2_core would be wrong —
// that is genuinely per-side behavior and belongs in the f2_server shim TU
// (src/server_net.cc, STEP 3). ByteSink is the seam between the two.

// Where encoded bytes go. Implemented in-core by the file sink below (the gate's
// stream source); implemented over a socket by f2_server in STEP 3. Same one-way
// seam, swapped source — exactly the file-replayer-first plan (MP_PROTOCOL.md §7).
// Per-frame sidecar handed to writeFrame (presentation-pacing outbox, design §8.6).
// The socket sink keeps this beside each queued frame so it can schedule release
// (costMs), map an ack's entryId back to a frame (entryBase/eventCount), and later
// identify a dead map's frames (mapGeneration) — all WITHOUT re-parsing the header.
// The file/golden sink ignores it (default writeFrame just writes the bytes).
struct WireFrameMeta {
    unsigned int seq = 0;        // frame seq (== header[0..3])
    unsigned int entryBase = 0;  // total-order id of the frame's first event (wire v4)
    unsigned short eventCount = 0;
    unsigned int simTs = 0;      // sim clock at emission (== header[4..7])
    unsigned int costMs = 0;     // presentation time this frame costs (MAX over actor lanes)
    unsigned int mapGeneration = 0; // mapGetLoadGeneration() at emission
};

class ByteSink {
public:
    virtual ~ByteSink() = default;

    // Append size bytes. Must consume all of them.
    virtual void write(const void* data, unsigned int size) = 0;

    // Emit ONE complete frame (header + payload) with its metadata sidecar. The
    // default is exactly the pre-outbox behavior — write the two byte runs in order,
    // meta ignored — so the file/golden sink is byte-identical and needs no override.
    // The socket sink overrides this to enqueue per client and schedule release (§8.6).
    virtual void writeFrame(const unsigned char* header, unsigned int headerLen,
        const unsigned char* payload, unsigned int payloadLen, const WireFrameMeta& /*meta*/)
    {
        write(header, headerLen);
        if (payloadLen != 0) {
            write(payload, payloadLen);
        }
    }

    // Push buffered bytes toward their destination. Called once per frame.
    virtual void flush() {}

    // Should the encoder write the stream preamble into this sink at begin()?
    //
    // TRUE for a FILE sink (F2_NETSTREAM): the file is one stream with one
    // reader, so the preamble belongs at its head.
    //
    // FALSE for the SOCKET sink: from wire version 2 the preamble carries a
    // PER-CLIENT sessionId, so it cannot ride the shared broadcast buffer at
    // all — the transport writes it directly to each socket at accept instead
    // (server_net.cc). This also makes the two join paths (boot-time accept and
    // mid-stream join) identical, where before, one got the preamble from
    // begin() and the other had a hand-copied duplicate of those bytes.
    //
    // ►► Consequence for the netsocket gate: the F2_SERVER_NET_TEE log is now
    // purely the BROADCAST stream, so a client capture is exactly the tee plus
    // its own kWirePreambleLen-byte head. The gate skips that head before
    // comparing (tests/golden/run_golden_netsocket.sh).
    virtual bool wantsStreamPreamble() const { return true; }
};

// Install the NetworkPresenter writing the wire to `path` (a file — the STEP 2
// source for tools/replay.py). Wired to F2_NETSTREAM in serverInstall().
//
// Returns false (installing NOTHING) if the stream cannot be opened. THE CALLER MUST
// HANDLE THAT: leaving the incoming presenter installed under the server loop means
// the CLIENT presenter runs headless in the probe binary, which is exactly the
// interlock serverInstall's null-presenter default exists to prevent.
bool presenterInstallNetwork(const char* path);

// Install the NetworkPresenter writing to a caller-owned sink (STEP 3's socket).
// The sink must outlive the server loop; this never takes ownership.
// Returns false (installing nothing) if `sink` is null.
bool presenterInstallNetworkSink(ByteSink* sink);

// Flush and release the stream. Paired with either install above; called from
// serverRestore(). Safe if no network presenter was installed.
void presenterUninstallNetwork();

// STEP 3 seam ("MAKE IT ACCEPT"). Pre-register a caller-owned sink that
// serverInstall() prefers over the F2_NETSTREAM file path when installing the
// network presenter. This is how f2_server injects its SocketByteSink
// (src/server_net.cc) BEFORE the serve loop's join-baseline emission, so a
// connect-at-start client receives the baseline snapshot + the live stream —
// without core ever naming the concrete socket type (the sink is an abstract
// ByteSink*, and the wrong direction — core → f2_server — never occurs). Pass
// nullptr to clear. The sink must outlive the serve loop; never owned here.
void presenterSetServerSink(ByteSink* sink);

// Presentation duration (ms) stamped on the NEXT objectMoved emission's move.durMs
// slot; 0 = snap (the default for every mover that doesn't declare a pace). The
// stepped-walk engine (server_anim.cc) brackets each objectSetLocation with
// set(msPerTile) / set(0) — a pending-value seam in the presenterSetServerSink
// mold, chosen over widening objectMoved's signature through every caller that
// has no duration to declare. Only the NetworkPresenter reads it; the null/
// client/narrate presenters ignore it, so it is golden-inert.
void presenterSetNextMoveDurationMs(int ms);
int presenterNextMoveDurationMs();

// Whether the NEXT objectMoved emission is a RUN (true) or a WALK (false).
// Same pending-value seam as the duration above, stamped in the same bracket.
//
// ►► This exists because the viewer used to INFER it: `durMs <= 120 ? RUNNING :
// WALK`. durMs is wall-clock pacing, so that made the animation a function of
// server load — two critters at different speeds landed on opposite sides of one
// constant ("some guards run, some walk"), and an idle wanderer rendered with the
// run cycle reads as panic. The server knows the real answer; ship it, never
// re-derive it. The viewer keeps its artExists() fallback to WALK, which is a
// genuine art-availability rule (many critters have no run frames), not a guess.
void presenterSetNextMoveRun(bool run);
bool presenterNextMoveRun();

// The pending server sink, or nullptr. serverInstall() feeds this straight into
// presenterInstallNetworkSink() (null-safe: returns false, falling through to
// the F2_NETSTREAM/F2_NARRATE/null presenter chain).
ByteSink* presenterServerSink();

} // namespace fallout

#endif /* FALLOUT_PRESENTER_NETWORK_H_ */
