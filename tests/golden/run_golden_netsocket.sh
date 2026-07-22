#!/usr/bin/env bash
# Netsocket gate (P5-C STEP 3 "MAKE IT ACCEPT": the wire over a real TCP socket).
#
# STEP 3 adds only the TRANSPORT (src/server_net.cc SocketByteSink), not the wire
# CONTENT. So this gate does NOT re-validate reconstruction against a state dump —
# that is the netstream gate's job (run_golden_netstream.sh), via the client probe,
# which is the only binary with a state_dump oracle. f2_server has no such oracle
# ("did it abort?" is its STEP 1 oracle), so bolting one on would be a detour.
#
# ►► THE CLAIM HERE IS TRANSPORT FAITHFULNESS: the bytes a client captures off the
# socket equal the bytes the SAME serve run produced. SocketByteSink tees every
# outbound write() to a file (F2_SERVER_NET_TEE) AND to the client sockets from the
# identical buffer; capturing the socket and diffing against the tee therefore tests
# only DELIVERY — no dropped, reordered, or truncated frames. Wire CONTENT correctness
# is the netstream gate's job (client probe vs state_dump) and carries over unchanged.
#
# ►► WHY SAME-RUN, NOT TWO RUNS: an earlier two-run design (socket run vs a separate
# F2_NETSTREAM file run) conflated transport with sim determinism — and f2_server is
# NOT deterministic run-to-run on AI-heavy maps (it has no F2_FAKE_CLOCK, so wall-clock
# getTicks steers AI timing; denbus1 even reaches the still-stubbed _strmfe symbol on
# some runs and aborts). The tee removes that dependency: one run, produced-vs-delivered.
#
# Maps here must SERVE CLEANLY under f2_server (no _strmfe-class STEP-1 stub abort),
# else the run truncates and tee/socket can diverge at the tail. artemple + newr1 are
# verified clean + deterministic and span a wide stream size.
#
# ►► BYTE-IDENTITY (not prefix) is valid here because net_capture.py stays connected
# and reads to EOF: the server streams every beat up to the tick cap, THEN closes, so
# the client receives exactly what the tee logged. (If a client disconnected mid-run,
# the tee — the authoritative produced-wire log — would legitimately be a superset of
# that client's capture by the beats after it left; the gate deliberately does not
# exercise that, since it is testing delivery integrity, not disconnect behavior.)
#
# A case PASSES only when the capture succeeds AND cmp reports byte-identical.
#
# Usage: tests/golden/run_golden_netsocket.sh
# Env: F2_GAME_DIR (default <repo>/FO2), F2_SERVER_BIN (default <repo>/build/f2_server),
#      F2_NET_PORT_BASE (default 45700; each case uses BASE+index)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_SERVER_BIN:-$ROOT/build/f2_server}"
CAPTURE="$ROOT/tools/net_capture.py"
PORT_BASE="${F2_NET_PORT_BASE:-45700}"
RESULTS="$(mktemp -d)"
trap 'rm -rf "$RESULTS"' EXIT

fail=0
idx=0

# Must equal kWirePreambleLen (src/wire_defs.h): magic(4) + version(2) + sessionId(4).
PREAMBLE_LEN=10

run_case() {
    local name="$1" map="$2" ticks="$3"
    local port=$((PORT_BASE + idx)); idx=$((idx + 1))
    local tee_bin="$RESULTS/$name.tee.nets"    # produced (server-side wire log)
    local sock_bin="$RESULTS/$name.sock.nets"  # delivered (client-side capture)

    # ONE run: stream over TCP (SocketByteSink) while teeing the identical bytes to
    # a file. The server blocks on accept() until the capture client connects, then
    # streams and closes at the tick cap.
    (cd "$GAME_DIR" && env F2_SERVER_MAP="$map" F2_SERVER_TICKS="$ticks" \
        F2_SERVER_NET="$port" F2_SERVER_NET_TEE="$tee_bin" \
        timeout -k 5 180 "$BIN" >/dev/null 2>&1) &
    local srv_pid=$!
    python3 "$CAPTURE" 127.0.0.1 "$port" "$sock_bin" 15 >/dev/null 2>&1
    local cap_rc=$?
    wait "$srv_pid" 2>/dev/null

    local tsz ssz
    tsz=$(wc -c < "$tee_bin" 2>/dev/null || echo 0)
    ssz=$(wc -c < "$sock_bin" 2>/dev/null || echo 0)
    # ►► The capture leads with a PER-CLIENT accept preamble that the tee does not
    # and must not contain. From wire version 2 the preamble carries the client's
    # own sessionId ("F2NS" | u16 version | i32 sessionId, wire_defs.h
    # kWirePreambleLen), so it is written straight to that one socket rather than
    # through the broadcast buffer the tee mirrors. The tee is therefore exactly
    # the BROADCAST stream, and a capture is that stream plus its own 10-byte head
    # — so skip the head on the delivered side and compare the rest byte for byte.
    # (Before v2 the preamble was broadcast, so both files held one copy and a
    # plain cmp lined up. The claim under test is unchanged: delivered == produced.)
    if [ "$cap_rc" = 0 ] && [ "$tsz" -gt 0 ] && [ "$ssz" -ge "$PREAMBLE_LEN" ] \
        && cmp -s -i "${PREAMBLE_LEN}:0" "$sock_bin" "$tee_bin"; then
        echo "PASS $name — delivered==produced, $tsz broadcast bytes (+${PREAMBLE_LEN}B preamble)"
    else
        echo "FAIL $name — socket transport diverged (cap_rc=$cap_rc, produced=$tsz delivered=$ssz):"
        cmp -i "${PREAMBLE_LEN}:0" "$sock_bin" "$tee_bin" 2>&1 | sed 's/^/    /' | head
        fail=1
    fi
}

# Two clean-serving, deterministic maps spanning a wide stream size exercise the
# transport across many frames. Transport faithfulness is map-agnostic (it carries
# bytes), so this need not enumerate every serving map. Maps that trip the _strmfe
# STEP-1 stub (arvillag, denbus1, klamath) are deliberately absent — their abort is
# unrelated to the socket.
#        name     map           ticks
run_case artemple artemple.map  400
run_case newr1    newr1.map     500

[ "$fail" = 0 ] || exit 1
exit 0
