#!/usr/bin/env python3
"""Capture a f2_server binary wire stream over TCP to a file.

P5-C STEP 3 "MAKE IT ACCEPT" test client. f2_server (F2_SERVER_NET=<port>) binds
and blocks until a viewer connects, then streams the "F2NS" wire (the same bytes
FileByteSink writes to F2_NETSTREAM) until the serve loop ends and closes the
socket. This connects, reads to EOF, and writes the raw bytes out — the socket
counterpart of the F2_NETSTREAM file, so a byte-diff proves the socket transport
carries the wire intact (tests/golden/run_golden_netsocket.sh).

Usage: net_capture.py <host> <port> <out_file> [connect_timeout_s]

Retries the connect (the server may not be listening yet when we start), then
reads until the server closes. Exit 0 on a clean capture, nonzero otherwise.
"""

import socket
import sys
import time


def main() -> int:
    if len(sys.argv) < 4:
        sys.stderr.write("usage: net_capture.py <host> <port> <out_file> [connect_timeout_s]\n")
        return 2

    host = sys.argv[1]
    port = int(sys.argv[2])
    out_path = sys.argv[3]
    connect_timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 10.0

    # The server races us to listen(); retry the connect until it is up.
    deadline = time.monotonic() + connect_timeout
    sock = None
    while sock is None:
        try:
            sock = socket.create_connection((host, port), timeout=2.0)
        except (ConnectionRefusedError, OSError):
            if time.monotonic() >= deadline:
                sys.stderr.write(f"net_capture: could not connect to {host}:{port} within {connect_timeout}s\n")
                return 1
            time.sleep(0.05)

    total = 0
    with open(out_path, "wb") as out:
        sock.settimeout(60.0)
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                sys.stderr.write("net_capture: recv timed out (server hung?)\n")
                sock.close()
                return 1
            if not chunk:
                break  # server closed — clean EOF
            out.write(chunk)
            total += len(chunk)
    sock.close()

    sys.stderr.write(f"net_capture: captured {total} bytes to {out_path}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
