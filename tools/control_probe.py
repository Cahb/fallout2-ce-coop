#!/usr/bin/env python3
# STEP 6 control-plane probe: a raw TCP wire client that drives the f2_server
# CONTROL plane and verifies the authoritative move came back on the wire.
#
# It connects to F2_SERVER_NET, consumes the F2NS stream (preamble + join baseline),
# reads the dude's tile from the first SNAPSHOT_OBJECT (gDude is netId 1, emitted
# first by serverEmitBaseline), then sends the upstream control lines:
#     claim
#     mv <dudeTile+8> 0
# on the SAME socket, and asserts that a MOVE event for the dude's netId appears in
# the stream afterward (the server executed the intent authoritatively). +8 tiles is
# the known-walkable churn distance the mid-join gate already relies on for artemple.
#
# Exit 0 (prints "CONTROL PROBE PASS ...") iff such a MOVE is observed; 1 otherwise.
#
# Usage: control_probe.py <host> <port> [timeout_seconds]

import socket
import struct
import sys
import time

# Event type tags — MUST match presenter_network.cc / client_net.cc.
E_MOVE = 2
E_SNAPSHOT_OBJECT = 8

# Destination = dude tile + one full hex-row south (gHexGridWidth = 200). Verified
# reachable across open ground on artemple's entrance — a raw +N east/west offset
# can land in a wall (e.g. +8 is blocked and pathfinds to nothing).
MOVE_OFFSET = 200


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: control_probe.py <host> <port> [timeout_s]\n")
        return 2
    host = argv[1]
    port = int(argv[2])
    timeout = float(argv[3]) if len(argv) > 3 else 12.0

    sock = socket.create_connection((host, port), timeout=timeout)

    buf = bytearray()
    pos = 0
    magic_done = False

    dude_net_id = None
    dude_tile = None
    sent = False

    # Absolute wall-clock deadline. The server streams a frame every beat, so a
    # per-recv timeout would never fire (recv always returns promptly) and the loop
    # could spin forever if the move never appears — bound it here instead.
    deadline = time.monotonic() + timeout

    def recv_more():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise socket.timeout("deadline reached")
        sock.settimeout(remaining)
        chunk = sock.recv(65536)
        if not chunk:
            raise EOFError("server closed the connection")
        buf.extend(chunk)

    try:
        while True:
            if time.monotonic() >= deadline:
                raise socket.timeout("deadline reached")
            # Consume the one-time stream preamble:
            # "F2NS" | u16 version | i32 sessionId (10 bytes from wire version 2).
            if not magic_done:
                while len(buf) - pos < 10:
                    recv_more()
                if bytes(buf[pos:pos + 4]) != b"F2NS":
                    sys.stderr.write("control_probe: bad stream magic\n")
                    return 1
                pos += 10
                magic_done = True

            # One frame: u32 seq, u32 simTs, u32 payloadLen, u16 eventCount,
            # u32 entryBase (wire v4), payload = 18-byte header. entryBase unused here.
            while len(buf) - pos < 18:
                recv_more()
            seq, _sim, payload_len, event_count = struct.unpack_from("<IIIH", buf, pos)
            while len(buf) - pos < 18 + payload_len:
                recv_more()
            payload = bytes(buf[pos + 18:pos + 18 + payload_len])
            pos += 18 + payload_len

            # Reclaim consumed prefix so the buffer stays bounded.
            if pos > (1 << 20):
                del buf[:pos]
                pos = 0

            ep = 0
            for _ in range(event_count):
                etype, _flags, elen = struct.unpack_from("<BBH", payload, ep)
                ep += 4
                body = payload[ep:ep + elen]
                ep += elen

                if etype == E_SNAPSHOT_OBJECT and dude_tile is None:
                    # First SNAPSHOT_OBJECT is gDude (serverEmitBaseline emits it
                    # before the tile-walk). body: netId, pid, tile, elev, fid, flags.
                    net_id, _pid, tile = struct.unpack_from("<iii", body, 0)
                    dude_net_id = net_id
                    dude_tile = tile
                    sys.stderr.write(
                        "control_probe: dude netId=%d tile=%d\n" % (net_id, tile))

                elif etype == E_MOVE and sent:
                    # body: netId, fromTile, toTile, fromElev, toElev, durMs.
                    net_id, from_tile, to_tile = struct.unpack_from("<iii", body, 0)
                    if net_id == dude_net_id:
                        print("CONTROL PROBE PASS — dude netId=%d MOVE %d->%d "
                              "(claim+mv honored authoritatively)"
                              % (net_id, from_tile, to_tile))
                        return 0

            # Once the dude's tile is known, send the control intents (once).
            if dude_tile is not None and not sent:
                target = dude_tile + MOVE_OFFSET
                sock.sendall(b"claim\n")
                sock.sendall(("mv %d 0\n" % target).encode("ascii"))
                sys.stderr.write("control_probe: sent claim + mv %d 0\n" % target)
                sent = True
    except (EOFError, socket.timeout) as exc:
        sys.stderr.write("control_probe: %s (sent=%s, dude_tile=%s)\n"
                         % (exc, sent, dude_tile))
        return 1
    finally:
        try:
            sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main(sys.argv))
