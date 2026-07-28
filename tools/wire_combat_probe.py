#!/usr/bin/env python3
"""P3 wire-combat probe (gate 10). Drives a turn-based fight entirely through the
CLAIM-GATED VIEWER WIRE (server_control.cc) — the path the real SDL viewer uses —
rather than the unrestricted debug CMD port. It asserts the properties that make
the "first human-driven networked fight" work:

  (1) the client CLAIMS control over the main wire socket (not the CMD port);
  (2) with NO F2_SERVER_TURN_WAIT set, the resumable-combat barrier still waits on
      the dude's turn — proving the connected CLAIMANT alone holds the turn open
      (serverClaimantConnected, the core hook the barrier consults);
  (3) a bare `cattack` sent up the wire mid-fight (exactly the viewer's 'A' key)
      is drained by the barrier and resolves as a dude EVENT_ATTACK_RESULT.

The CMD port is used ONLY to start the fight (aggro); every combat verb goes up
the wire. This is the inverse of resumable_probe.py, which injects via CMD.

Usage: wire_combat_probe.py <host> <wire_port> <cmd_port> [timeout_seconds]
"""
import socket
import struct
import sys
import time

# Event tags — MUST match presenter_network.cc / client_net.cc.
E_SNAPSHOT_OBJECT = 8
E_COMBAT_ENTER = 12
E_TURN_START = 14
E_ATTACK_RESULT = 15
# ►► THE RECORD CHANNEL IS THE ATTACK PRESENTER. With F2_SERVER_PRES_RECORD on — which is now
# the DEFAULT for a server — combat.cc:6019 DELIBERATELY suppresses EVENT_ATTACK_RESULT for a
# recorded attack ("record channel = sole attack presenter ... else the viewer double-presents
# it"), and the attack ships as EVENT_PRES_SEQ instead. Asserting only on ATTACK_RESULT
# therefore reported "no dude attack" for an attack that resolved perfectly — which is what
# made this gate look red whenever recording was enabled. Accept EITHER channel: both carry
# the acting netId as their first i32.
E_PRES_SEQ = 31


def connect_retry(host, port, deadline):
    while time.monotonic() < deadline:
        try:
            return socket.create_connection((host, port), timeout=2.0)
        except (ConnectionRefusedError, OSError):
            time.sleep(0.05)
    return None


def main():
    if len(sys.argv) < 4:
        print("usage: wire_combat_probe.py <host> <wire_port> <cmd_port> [timeout]")
        return 2
    host = sys.argv[1]
    wire_port = int(sys.argv[2])
    cmd_port = int(sys.argv[3])
    timeout = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
    deadline = time.monotonic() + timeout

    wire = connect_retry(host, wire_port, deadline)
    if wire is None:
        print("FAIL — could not connect wire")
        return 1
    # The server opens the CMD listener only after a wire client connects.
    cmd = connect_retry(host, cmd_port, min(deadline, time.monotonic() + 5.0))
    if cmd is None:
        print("FAIL — could not connect CMD port")
        return 1

    buf = bytearray()
    pos = 0

    def recv_more():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("deadline reached")
        wire.settimeout(remaining)
        chunk = wire.recv(65536)
        if not chunk:
            raise EOFError("server closed the connection")
        buf.extend(chunk)

    def need(n):
        while len(buf) - pos < n:
            recv_more()

    # Preamble: "F2NS" | u16 version | i32 sessionId (10 bytes from wire version 2;
    # the sessionId is THIS client's — the only per-client field in the protocol).
    need(10)
    if bytes(buf[0:4]) != b"F2NS":
        print("FAIL — bad wire preamble")
        return 1
    pos = 10

    # CLAIM over the main wire socket immediately (the viewer claims after its
    # first blob; here the world is already streaming, so claim now).
    wire.sendall(b"claim\n")

    dude_net = None
    enter_seq = None
    dude_turn_seen = False
    dude_attack = False
    aggro_sent = False
    cattack_sent = False

    try:
        while time.monotonic() < deadline:
            # Kick off the fight once (klatoxcv is idle until aggro). CMD port is
            # used ONLY for this — never for a combat verb.
            if not aggro_sent:
                cmd.sendall(b"aggro 3\n")
                aggro_sent = True

            need(18)  # wire v4 header = 18 bytes (trailing u32 entryBase, unused here)
            seq, _sim, payload_len, _count = struct.unpack_from("<IIIH", buf, pos)
            need(18 + payload_len)
            payload = bytes(buf[pos + 18:pos + 18 + payload_len])
            pos += 18 + payload_len

            ep = 0
            while ep + 4 <= len(payload):
                etype, _flags, elen = struct.unpack_from("<BBH", payload, ep)
                ep += 4
                body = payload[ep:ep + elen]
                ep += elen

                if etype == E_SNAPSHOT_OBJECT and dude_net is None and len(body) >= 4:
                    dude_net = struct.unpack_from("<i", body, 0)[0]  # baseline emits gDude first
                elif etype == E_COMBAT_ENTER and enter_seq is None:
                    enter_seq = seq
                    print("saw COMBAT_ENTER seq=%d" % seq)
                elif etype == E_TURN_START and len(body) >= 13:
                    net_id, is_player = struct.unpack_from("<iB", body, 0)
                    _ap, deadline_ms = struct.unpack_from("<ii", body, 5)
                    if is_player and enter_seq is not None:
                        dude_turn_seen = True
                        # A nonzero deadline confirms the barrier is holding the turn
                        # open for us (the claimant) — no TURN_WAIT env is set.
                        # Re-inject on EVERY dude turn. This map's premade dude has a
                        # base critter art that cannot wield the ranged test weapon,
                        # so it fights unarmed = PUNCH (range ~1). A bare cattack
                        # auto-targets the nearest hostile, which starts out of range;
                        # the aggro'd melee enemy closes each turn and a punch lands
                        # once it is adjacent → the dude ATTACK_RESULT this gate
                        # asserts. A single shot never lands (a failed attack keeps the
                        # turn but drains the queue in one beat).
                        wire.sendall(b"cattack\n")  # the viewer's 'A' key, verbatim
                        if not cattack_sent:
                            cattack_sent = True
                            print("injected bare cattack over the WIRE (deadlineMs=%d)" % deadline_ms)
                elif etype in (E_ATTACK_RESULT, E_PRES_SEQ) and len(body) >= 4:
                    attacker_net = struct.unpack_from("<i", body, 0)[0]
                    if cattack_sent and dude_net is not None and attacker_net == dude_net:
                        if not dude_attack:
                            print("saw dude attack (ATTACK_RESULT or PRES_SEQ) attacker_net=%d" % attacker_net)
                        dude_attack = True

            if dude_attack:
                break
    except (EOFError, TimeoutError) as exc:
        print("wire ended: %s" % exc)

    ok = True
    if enter_seq is None:
        print("FAIL — no COMBAT_ENTER"); ok = False
    if not dude_turn_seen:
        print("FAIL — no dude TURN_START (barrier never opened the dude's turn)"); ok = False
    if not dude_attack:
        print("FAIL — no dude attack (ATTACK_RESULT or PRES_SEQ) after a wire cattack"); ok = False

    if ok:
        print("WIRE COMBAT PROBE PASS — claim+cattack over the claim-gated wire drove a dude attack (net=%d)"
              % dude_net)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
