#ifndef FALLOUT_WIRE_DEFS_H_
#define FALLOUT_WIRE_DEFS_H_

namespace fallout {

// Wire constants shared by the ENCODER (presenter_network.cc) and the TRANSPORT
// (server_net.cc), which both write the stream preamble and must never drift.
// They used to be two hand-kept copies with a comment begging them to match —
// this header is that comment made load-bearing.

// Stream preamble version.
//   1 = "F2NS" | u16 version              (6 bytes, pre-co-op)
//   2 = "F2NS" | u16 version | i32 sessionId  (10 bytes)
//
// Version 2 exists because the sessionId is the ONE thing that cannot ride the
// shared stream: there is a single encoder broadcasting one identical byte
// buffer to every client (server_net.cc SocketByteSink::write), so the accept
// preamble is the only per-client channel in the entire protocol. A viewer needs
// its own sessionId to find itself in EVENT_PLAYER_ROSTER and bind to its actor
// (MP_PROPOSAL.md Ch 5.5/5.6).
//
// No compat shim: the server and viewer ship together, so a version mismatch is
// a build mistake and should fail loudly rather than degrade.
//
// Version 3 adds per-item ammo to the OBJECT_DELTA_INVENTORY payload (putInventory):
// each item now carries its loaded-round count and ammo-type pid after the flags, so
// a weapon fired dry / reloaded replicates its real ammo LIVE instead of the client
// rebuilding it to the proto default (ammo previously rode only the join blob).
//
// Version 4 widens the per-frame header by a trailing u32 entryBase: the total-order
// id of the FIRST event in the frame (entryId of event e = entryBase + e). This is
// the presentation-pacing "delta seq-stamping" (PRESENTATION_PACING_DESIGN.md §8.1) —
// a monotonic id on EVERY entry, deltas included, that the per-client outbox (§4 P2)
// and the state-hash ack (§4 P2b) both key on. On the wire (not merely a client-local
// counter) so a MID-STREAM joiner agrees with the server on entry ids from its first
// frame. Frames stay dense; entryBase is dense over EVENTS across frames.
constexpr unsigned short kWireVersion = 4;

// "F2NS", as bytes, in stream order.
constexpr char kWireMagic[4] = { 'F', '2', 'N', 'S' };

// Byte length of a version-2 preamble: magic(4) + version(2) + sessionId(4).
constexpr int kWirePreambleLen = 10;

// The debug command port's sentinel session. Real sessions are minted from 1 and
// are never reused, so 0 is safe as "not a wire session" / "unbound".
constexpr int kNoSessionId = 0;

} // namespace fallout

#endif /* FALLOUT_WIRE_DEFS_H_ */
