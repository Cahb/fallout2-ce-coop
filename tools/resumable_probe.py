#!/usr/bin/env python3
"""Resumable-combat wire probe (P5-C / P2 gate).

Drives the f2_server resumable-combat session over the live wire + debug CMD
port and asserts the four properties that are IMPOSSIBLE before the session
machine exists (a fight draining inside one scriptsHandleRequests call cannot
span beats, carry a turn deadline, or accept a mid-fight command):

  (1) EVENT_COMBAT_ENTER and EVENT_COMBAT_EXIT arrive in frames with DIFFERENT
      seq numbers -> the fight spanned beats.
  (2) a dude EVENT_TURN_START carries a nonzero deadlineMs -> the session is the
      producer of the reserved idle-timer slot.
  (3) a `cattack` injected via the CMD port AFTER combatEnter was observed (i.e.
      genuinely mid-fight) is followed by an EVENT_ATTACK_RESULT whose attacker
      is the dude.
  (4) the fight terminates (combatExit) and the server exits/closes cleanly.

Usage: resumable_probe.py <host> <wire_port> <cmd_port> [timeout_seconds]
"""
import socket
import struct
import sys
import time

# Event tags — MUST match presenter_network.cc / client_net.cc.
E_SNAPSHOT_OBJECT = 8
E_COMBAT_ENTER = 12
E_COMBAT_EXIT = 13
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
        print("usage: resumable_probe.py <host> <wire_port> <cmd_port> [timeout]")
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
    # The server opens the CMD listener only AFTER the wire client connects, so
    # retry the CMD connect for a moment.
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

    dude_net = None
    enter_seq = None
    exit_seq = None
    dude_deadline = 0
    dude_attack = False

    aggro_sent = False
    cattack_sent = False

    try:
        while time.monotonic() < deadline:
            # Kick off combat once (server is idle on klatoxcv until aggro).
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
                    # Baseline emits gDude first.
                    dude_net = struct.unpack_from("<i", body, 0)[0]
                elif etype == E_COMBAT_ENTER and enter_seq is None:
                    enter_seq = seq
                    print("saw COMBAT_ENTER seq=%d" % seq)
                elif etype == E_TURN_START and len(body) >= 13:
                    net_id, is_player = struct.unpack_from("<iB", body, 0)
                    _ap, deadline_ms = struct.unpack_from("<ii", body, 5)
                    if is_player and deadline_ms > 0:
                        if dude_deadline == 0:
                            dude_deadline = deadline_ms
                            print("saw dude TURN_START deadlineMs=%d" % deadline_ms)
                        # Genuinely mid-fight: inject a punch on EVERY dude turn.
                        # This map's premade dude has a base critter art that
                        # cannot wield the ranged test weapon (artExists → wield
                        # fails), so it fights unarmed = PUNCH (range ~1). A whole
                        # attack queue drains in the first beat (a failed shot keeps
                        # the turn), so one-shot injection never lands. The aggro'd
                        # enemy is melee and closes each turn; re-injecting a punch
                        # every dude turn lands one once the enemy is adjacent →
                        # the EVENT_ATTACK_RESULT this gate asserts.
                        if enter_seq is not None:
                            cmd.sendall(b"cattack 3\n")
                            if not cattack_sent:
                                cattack_sent = True
                                print("injected cattack mid-fight")
                elif etype in (E_ATTACK_RESULT, E_PRES_SEQ) and len(body) >= 4:
                    attacker_net = struct.unpack_from("<i", body, 0)[0]
                    if cattack_sent and dude_net is not None and attacker_net == dude_net:
                        if not dude_attack:
                            print("saw dude attack (ATTACK_RESULT or PRES_SEQ) attacker_net=%d" % attacker_net)
                        dude_attack = True
                elif etype == E_COMBAT_EXIT and enter_seq is not None and exit_seq is None:
                    exit_seq = seq
                    print("saw COMBAT_EXIT seq=%d" % seq)

            if (enter_seq is not None and exit_seq is not None
                    and dude_deadline > 0 and dude_attack):
                break
    except (EOFError, TimeoutError) as exc:
        print("wire ended: %s" % exc)

    ok = True
    if enter_seq is None:
        print("FAIL — no COMBAT_ENTER"); ok = False
    if exit_seq is None:
        print("FAIL — no COMBAT_EXIT (fight did not terminate)"); ok = False
    if enter_seq is not None and exit_seq is not None and enter_seq == exit_seq:
        print("FAIL — enter_seq == exit_seq (fight did not span beats)"); ok = False
    if dude_deadline <= 0:
        print("FAIL — no dude TURN_START with nonzero deadlineMs"); ok = False
    if not dude_attack:
        print("FAIL — no dude attack (ATTACK_RESULT or PRES_SEQ) after mid-fight cattack"); ok = False

    if ok:
        print("RESUMABLE PROBE PASS — enter_seq=%d exit_seq=%d deadlineMs=%d dude_attack=1"
              % (enter_seq, exit_seq, dude_deadline))
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
