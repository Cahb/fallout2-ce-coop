#!/usr/bin/env python3
# GENERIC control-verb probe: a raw wire client that claims a seat and then sends
# whatever control lines you name, so a verb can be smoke-tested headlessly.
#
# The specialised probes next door (control_probe.py, wire_combat_probe.py,
# resumable_probe.py) each parse the stream to assert a specific EVENT came back.
# This one asserts nothing about the stream on purpose: it exists for verbs whose
# effect is observed on the SERVER's stderr, which the gate scripts already treat as a
# contract. It keeps draining the socket throughout — a client that stops reading
# backpressures the server, which would change the very timing under test.
#
# Usage: verb_probe.py <host> <port> [--claim] [--wait N] <verb line> [<verb line>...]
#   --claim   send `claim` first and give the server a beat to bind the session
#             (default on; --no-claim to drive as an unbound spectator)
#   --wait N  seconds to keep draining after the last line (default 3)
#
# Exit 0 if every line was written and the connection stayed up; 1 otherwise.

import socket
import sys
import time


def main(argv):
    args = argv[1:]
    if len(args) < 3:
        sys.stderr.write("usage: verb_probe.py <host> <port> [--claim|--no-claim] "
                         "[--wait N] <verb line>...\n")
        return 2

    host = args[0]
    port = int(args[1])
    rest = args[2:]

    claim = True
    wait_after = 3.0
    lines = []
    i = 0
    while i < len(rest):
        if rest[i] == "--claim":
            claim = True
        elif rest[i] == "--no-claim":
            claim = False
        elif rest[i] == "--wait":
            i += 1
            wait_after = float(rest[i])
        else:
            lines.append(rest[i])
        i += 1

    if not lines:
        sys.stderr.write("verb_probe: no verb lines given\n")
        return 2

    try:
        sock = socket.create_connection((host, port), timeout=10.0)
    except OSError as exc:
        sys.stderr.write("verb_probe: connect failed: %s\n" % exc)
        return 1

    sock.settimeout(0.2)

    def drain(seconds):
        # Read and discard. A dead peer (recv returns b"") is a failure: the server
        # kicked us, which usually means a verb was malformed enough to drop the
        # session, and that is exactly what a gate wants to catch.
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                if sock.recv(65536) == b"":
                    return False
            except socket.timeout:
                pass
            except OSError:
                return False
        return True

    # Let the join baseline flow before saying anything.
    if not drain(2.0):
        sys.stderr.write("verb_probe: server closed during the baseline\n")
        return 1

    try:
        if claim:
            sock.sendall(b"claim\n")
            if not drain(1.5):
                sys.stderr.write("verb_probe: server closed after claim\n")
                return 1

        for line in lines:
            sock.sendall(line.encode("utf-8") + b"\n")
            print("verb_probe: sent %r" % line)
            # One beat between lines: several verbs in one packet are fine for the
            # server, but a gate reading stderr wants them in a knowable order.
            if not drain(0.8):
                sys.stderr.write("verb_probe: server closed after %r\n" % line)
                return 1

        if not drain(wait_after):
            sys.stderr.write("verb_probe: server closed while settling\n")
            return 1
    except OSError as exc:
        sys.stderr.write("verb_probe: send failed: %s\n" % exc)
        return 1
    finally:
        sock.close()

    print("VERB PROBE PASS — %d line(s) delivered" % len(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
