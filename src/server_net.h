#ifndef FALLOUT_SERVER_NET_H_
#define FALLOUT_SERVER_NET_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "platform_net.h"
#include "presenter_network.h"

namespace fallout {

// f2_server socket transport (P5-C STEP 3 "MAKE IT ACCEPT", [[p5-server-plan]]).
//
// SocketByteSink is the ByteSink implementation that carries the binary wire
// (the "F2NS" stream built by the NetworkPresenter in f2_core's
// presenter_network.cc, STEP 2) over a TCP socket to connected viewer clients.
// presenter_network.h reserves this per-side transport for f2_server precisely
// because a socket in f2_core would be the wrong layer: the encoder is shared
// core policy, the transport is genuinely per-side behavior.
//
// Model: SINGLE-THREADED. server_main binds and listens, blocks until the first
// viewer connects (acceptClients), registers the sink (presenterSetServerSink),
// then streams beats. STEP 5 adds MID-STREAM JOIN: the serve loop polls
// acceptPending() each beat; a joiner gets the stream preamble immediately from
// the transport, and the loop answers with a rebaseline BROADCAST (fresh netId
// walk + blob + baseline to ALL clients — C.4: the walk resets the whole-stream
// netId domain, so a per-client blob cannot exist without netIds on the wire).
// Every client sees one identical byte stream after its preamble; there is no
// per-client framing (the client seeds its frame-seq from the first frame it
// receives). A separate accept thread + locked FIFO can be layered on later;
// the beat loop stays deterministic and lock-free.
//
// Outbound: write() broadcasts each frame to every live client and drops any
// client whose socket has died (EPIPE/ECONNRESET).
//
// Inbound (3b): pollInbound() is the non-blocking reader wired into the serve
// loop's intentsDrain slot — it recvs whatever bytes have arrived, splits them
// into newline-delimited command lines, and hands each line to a callback that
// reifies it as a Command and dispatches it (the P5-A FIFO choke point). The v1
// inbound framing is a plain text line "verb [arg] [arg2]\n" — deliberately the
// string-verb v0 the shared commandDispatch already speaks (a typed binary
// command frame replaces it once the protocol is frozen, REWRITE_PLAN 3.4).
class SocketByteSink : public ByteSink {
public:
    ~SocketByteSink() override;

    // False: the preamble is per-client (it carries the sessionId from wire
    // version 2) and is written at accept, not through the broadcast buffer.
    bool wantsStreamPreamble() const override;

    // Create the listening socket on `port` (INADDR_ANY). Idempotent-safe: an
    // already-open sink is torn down first. Returns false (and logs) on any
    // socket/bind/listen error, leaving the sink closed. Also installs the
    // process SIGPIPE ignore so a dead client cannot kill the server on write.
    bool listen(uint16_t port);

    // Block accepting connections until at least `minClients` are connected.
    // Sets TCP_NODELAY on each (the wire is latency-sensitive, not throughput).
    // Returns false if the sink is not listening or accept() fails hard.
    bool acceptClients(int minClients);

    // Non-blocking: accept any viewers that connected since the last call
    // (mid-stream join, STEP 5) and send each the stream preamble. Returns how
    // many joined — a nonzero return obligates the caller to broadcast a
    // rebaseline this beat (serverRequestRebaseline), or the joiner starves
    // with no world. Flips the listener non-blocking on first use.
    int acceptPending();

    // Non-blocking inbound drain (3b). recv from every client, accumulate into
    // its line buffer, and invoke `onLine` once per complete '\n'-terminated
    // command line (the trailing '\n' stripped; a bare '\r' before it too). A
    // client that has closed (recv==0) or errored is dropped. Never blocks: a
    // client with nothing pending is simply skipped this beat.
    //
    // The line is tagged with the emitting client's stable sessionId (STEP 6):
    // the control plane needs a per-client identity to gate who may drive the
    // authoritative actor (server_control.cc). Identity is passed by value — the
    // onLine contract (must not mutate the client set) is unchanged.
    void pollInbound(const std::function<void(int sessionId, const char* line)>& onLine);

    // True if a client with this sessionId is still connected. The control plane
    // calls this each drain to release a claim whose owner has disconnected
    // (server_control.cc); sessionIds are never reused, so a stale id reads false.
    bool hasSession(int sessionId) const;

    // ByteSink: enqueue `size` bytes to every connected client's outbound queue
    // (zero-cost meta) and pump. Fallback for any direct-bytes caller; the frame
    // path goes through writeFrame. A failed send drops only that client.
    void write(const void* data, unsigned int size) override;

    // ByteSink (outbox §8.6): enqueue ONE frame to every client's per-client queue
    // as a SHARED buffer (one copy, N pointers) + its WireFrameMeta sidecar, tee the
    // canonical bytes, then pump(). Increment 2 drains everything immediately (no
    // release scheduling yet), so this is behavior-identical to the old broadcast.
    void writeFrame(const unsigned char* header, unsigned int headerLen,
        const unsigned char* payload, unsigned int payloadLen, const WireFrameMeta& meta) override;

    // TCP has no application buffer to push here (writes go straight to the
    // kernel), so this is a no-op — the frame-boundary contract is satisfied by
    // write() itself. Kept for the ByteSink contract and future batching.
    void flush() override;

    // Live client count. The serve loop's keepServing predicate stops when this
    // hits zero (all viewers left).
    size_t clientCount() const { return _clients.size(); }

    // Optionally mirror every outbound byte to a file (opened "wb"). This is the
    // server's own outbound-wire log, and doubles as the netsocket gate's
    // same-run reference: because the tee and the sockets receive the identical
    // write() buffer, comparing a client's socket capture against this file tests
    // TRANSPORT delivery alone — no dependence on run-to-run sim determinism
    // (which f2_server lacks on AI-heavy maps: wall-clock getTicks, no fake
    // clock). No-op / silently disabled if the path cannot be opened. Call before
    // serving; closed by closeAll()/dtor.
    void setTeeFile(const char* path);

    // Close every client socket and the listener. Safe to call repeatedly.
    void closeAll();

private:
    // One frame waiting in a client's outbound queue: a SHARED pointer to the encoded
    // bytes (one copy across all clients), its metadata sidecar, and how many bytes
    // have already gone out (partial-write resume; 0 in increment 2's blocking path).
    struct QueuedFrame {
        std::shared_ptr<const std::vector<unsigned char>> bytes;
        WireFrameMeta meta;
        size_t sent = 0;
        long long releaseAtMs = 0; // wall ms this frame may leave (0 = due now; §8.6)
    };

    // A connected client and its inbound line-assembly buffer (recv chunks do
    // not respect line boundaries, so partial lines carry across polls). Each
    // client carries a stable, monotonically-assigned sessionId (never reused)
    // so the control plane can bind authority to a specific connection.
    struct Client {
        NetSocket fd;
        std::string inbuf;
        int sessionId;
        std::deque<QueuedFrame> outq; // per-client outbound frame queue (outbox §8.6)
        // Release-schedule cursor (F2_SERVER_OUTBOX_PACE): the last frame's release time
        // and cost, so the next frame is held until lastReleaseAtMs + lastCostMs (§8.6).
        // Seeded to now at accept; max()-reanchored so an idle client accrues no credit.
        long long lastReleaseAtMs = 0;
        unsigned int lastCostMs = 0;
    };

    void dropClient(size_t index, const char* why);

    // Drain each client's outbound queue toward its socket, dropping a client whose
    // send fails. Increment 2: every queued frame is due, sent with the bounded
    // blocking writeAll (behavior-identical to the old broadcast). Increment 3 gates
    // on per-frame release times and flips sends non-blocking.
    void pump();

    NetSocket _listenFd = kNetInvalidSocket;
    // acceptPending flips the listener non-blocking on first use; tracked here
    // because Winsock has no fcntl(F_GETFL) to read the mode back.
    bool _listenNonBlocking = false;
    std::vector<Client> _clients;
    // Next sessionId to hand out. Starts at 1 so 0 is a usable "no session"
    // sentinel (the debug CommandListener dispatches with sessionId 0).
    int _nextSessionId = 1;
    FILE* _tee = nullptr;
};

// A dedicated inbound COMMAND channel (STEP-4 debug/admin — user-requested runtime
// injector). Unlike SocketByteSink it NEVER streams the wire OUT: it only accepts
// control clients (telnet/nc/scripts) and reads newline-delimited "verb arg arg2"
// command lines, which the serve loop dispatches through commandDispatch (the P5-A
// FIFO choke point — same vocabulary as the probe's F2_PROBE_ACTIONS). Because it
// carries no per-client baseline, it can accept connections at ANY time (the viewer
// wire is connect-at-start until mid-stream join lands, STEP 5). Its purpose is to
// poke a running server — `aggro`, `hurt`, etc. — and watch the effect propagate to
// the connected viewers live.
class CommandListener {
public:
    ~CommandListener();

    // Owns raw fds; forbid copies so a stray copy can't double-close on destruction.
    CommandListener(const CommandListener&) = delete;
    CommandListener& operator=(const CommandListener&) = delete;
    CommandListener() = default;

    // Create a NON-BLOCKING listening socket on `port` (INADDR_ANY). Returns false
    // (and logs) on any socket/bind/listen error. SO_REUSEADDR set.
    bool listen(uint16_t port);

    // Non-blocking: accept any newly-arrived control clients, recv whatever bytes
    // have come in, and invoke `onLine` once per complete '\n'-terminated command
    // line (trailing '\r'/'\n' stripped). A closed/errored/overlong client is
    // dropped. Never blocks. Contract mirrors SocketByteSink::pollInbound: `onLine`
    // dispatches a command and must not mutate the client set.
    void pollCommands(const std::function<void(const char* line)>& onLine);

    // Sink for writing a line back to the control client that sent the command.
    // The control channel is an ordinary full-duplex TCP socket, so a reply is
    // just a send on the same fd; a trailing '\n' is appended if absent.
    using ReplyFn = std::function<void(const char* text)>;

    // As above, but the callback also gets a reply sink bound to the sending
    // client — for admin verbs that ANSWER (a save listing, an error) instead of
    // only poking the sim. The reply is valid for the duration of the call only.
    void pollCommands(const std::function<void(const char* line, const ReplyFn& reply)>& onLine);

    // Text to send a control client the moment it connects, before it has typed
    // anything. The lobby greets with the slot listing: an operator who telnets in
    // otherwise meets silence and has to already know `help` exists.
    //
    // Invoked from the accept loop of either pollCommands overload, with a reply
    // sink bound to the new client only. Set it before the first poll; clearing it
    // (an empty function) is how the running server stops greeting.
    void setGreeting(std::function<void(const ReplyFn& reply)> greeting) { _greeting = std::move(greeting); }

    size_t clientCount() const { return _clients.size(); }

    // Close every control client and the listener. Safe to call repeatedly.
    void closeAll();

private:
    struct Client {
        NetSocket fd;
        std::string inbuf;
    };

    void dropClient(size_t index);

    NetSocket _listenFd = kNetInvalidSocket;
    std::vector<Client> _clients;
    std::function<void(const ReplyFn& reply)> _greeting;
};

} // namespace fallout

#endif /* FALLOUT_SERVER_NET_H_ */
