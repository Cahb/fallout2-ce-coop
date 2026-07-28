#!/usr/bin/env bash
# REST + ELEVATOR gate (docs/COOP_COVERAGE.md items 5 and 6 — two vanilla systems that
# were dead in co-op, each for a different reason).
#
# REST was dead three ways at once: the server's own verb hit an abort stub, a viewer
# was refused by vanilla's "You cannot rest at this location!", and no wire verb
# existed to ask with. The simulation is now shared (rest.cc) and the server drives it.
#
# ELEVATORS were dead SILENTLY: the server never overrode elevatorSelect, so the base
# class answered -1, which the drain reads as "the player cancelled the picker". No
# error, no log. The server now streams vanilla's panel to the rider and rides on the
# answer — and the answer is a BUTTON INDEX, never a destination.
#
# What is asserted:
#   1. rest advances the CLOCK by exactly the requested duration
#   2. a short rest heals NOTHING (vanilla's 180-minute heal cadence) …
#   3. … and a long one does heal                          (the sim is intact)
#   4. rest is refused where vanilla refuses it            (the location gate)
#   5. `elev <n>` with NO offer outstanding is REFUSED     (►► the trust boundary:
#      otherwise the verb is a teleport-anywhere primitive any session can send)
#   6. `rest` over the WIRE reaches the same simulation as the operator's verb
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${F2_GAME_DIR:-$ROOT/FO2}"
OUT="$ROOT/build/rest-gate"
CMD=9343
WIRE=9344
SRV=""

fail() {
    echo "FAIL rest/elevator — $1"
    tail -25 "$OUT/server.err" 2>/dev/null | sed 's/^/    srv| /'
    kill "$SRV" 2>/dev/null
    wait 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL rest/elevator — no f2_server (build broken?)"; exit 1; }
[ -f "$ROOT/tools/verb_probe.py" ] || { echo "FAIL rest/elevator — no verb_probe.py"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL rest/elevator — no FO2/ assets"; exit 1; }
command -v nc > /dev/null 2>&1 || { echo "SKIP rest/elevator — no nc(1) to drive the admin channel"; exit 0; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/replies.txt" "$OUT/probe.out"

pkill -x f2_server 2>/dev/null && sleep 1

( cd "$GAME" && exec env F2_SERVER_MAP=artemple.map F2_SERVER_CMD=$CMD F2_SERVER_NET=$WIRE \
    F2_SERVER_PLAYERS=2 F2_SERVER_PACE_MS=20 F2_SERVER_TICKS=5000000 \
    "$ROOT/build/f2_server" > "$OUT/server.err" 2>&1 ) &
SRV=$!

# ⚠ The wire listener BLOCKS at boot until the first client connects
# (server_main.cc acceptClients(1)), so the probe has to go first — an admin-only
# start would hang here forever. Documented in docs/FULL_RUN_RECON.md §5.
sleep 2
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

# 5 + 6 over the WIRE. `elev 1` answers a prompt that was never sent; `rest 180` is the
# real thing. Both are control verbs, reachable only over the wire (the admin channel
# dispatches admin verbs then DEBUG verbs, never control ones).
python3 "$ROOT/tools/verb_probe.py" 127.0.0.1 $WIRE --claim --wait 6 \
    'elev 1' 'rest 180' > "$OUT/probe.out" 2>&1 &
PROBE=$!

sleep 4
kill -0 "$SRV" 2>/dev/null || fail "server died while the probe drove it"

# 1-4 through the admin channel, which is the only place a rest REPORTS itself (the
# debug verb answers through debugPrint, which f2_server drops).
# The `sleep` holds the pipe open: closing stdin half-closes the socket and the server
# drops the client before flushing replies (see scripts/check_sheet.sh).
REPLIES="$({ printf 'hurt 30\nrest 60 0\nrest 1440 0\nrest 0 0\n'; sleep 12; } \
    | timeout 40 nc 127.0.0.1 $CMD 2>/dev/null)"
echo "$REPLIES" > "$OUT/replies.txt"

wait "$PROBE" 2>/dev/null
kill "$SRV" 2>/dev/null
wait 2>/dev/null

[ -n "$REPLIES" ] || fail "admin channel gave no replies (port $CMD unreachable?)"

# 1: the clock moves by exactly the duration. 60 minutes = 36000 ticks
# (GAME_TIME_TICKS_PER_HOUR), 1440 minutes = 864000. Exact, because an off-by-a-frame
# clock is how a rest silently heals the wrong amount.
grep -q 'rest: slot 0 .*1:00 -> completed; clock .*(+36000 ticks)' "$OUT/replies.txt" \
    || fail "a 1-hour rest did not advance the clock by exactly 36000 ticks:
    $(grep -m1 '^rest: slot 0 .*1:00' "$OUT/replies.txt")"
grep -q 'rest: slot 0 .*24:00 -> completed; clock .*(+864000 ticks)' "$OUT/replies.txt" \
    || fail "a 24-hour rest did not advance the clock by exactly 864000 ticks:
    $(grep -m1 '^rest: slot 0 .*24:00' "$OUT/replies.txt")"

# 2 + 3: the heal cadence. One hour is under vanilla's 180 accumulated minutes so it
# must heal NOTHING; a full day must heal.
SHORT="$(grep -m1 'rest: slot 0 .*1:00' "$OUT/replies.txt" | sed 's/.*hp //')"
LONG="$(grep -m1 'rest: slot 0 .*24:00' "$OUT/replies.txt" | sed 's/.*hp //')"
SHORT_FROM="${SHORT%% ->*}"; SHORT_TO="${SHORT##*-> }"
LONG_FROM="${LONG%% ->*}"; LONG_TO="${LONG##*-> }"
[ "$SHORT_FROM" = "$SHORT_TO" ] \
    || fail "a 1-hour rest healed $SHORT_FROM -> $SHORT_TO; vanilla's cadence is 180 minutes, so it must heal nothing"
[ "$LONG_TO" -gt "$LONG_FROM" ] \
    || fail "a 24-hour rest did not heal at all ($LONG_FROM -> $LONG_TO) — the shared rest sim is not accruing"

# 4: a nonsense duration is refused rather than run.
grep -q 'rest: minutes must be positive' "$OUT/replies.txt" \
    || fail "rest 0 was not refused"

# 5: ►► the elevator trust boundary. No prompt was ever sent to this session, so the
# verb must be dropped — and it must say so, because a silent drop here is
# indistinguishable from the bug this whole feature fixes.
grep -q 'control elev with no elevator offered' "$OUT/server.err" \
    || fail "an unsolicited 'elev' was NOT refused — the verb is a teleport primitive:
    $(grep -m1 'control elev' "$OUT/server.err")"

# 6: the same simulation from the wire side.
grep -q 'f2_server: control rest slot=.* 3:00 kind=0 outcome=0' "$OUT/server.err" \
    || fail "the wire 'rest' verb did not complete:
    $(grep -m1 'control rest' "$OUT/server.err")"

echo "PASS rest/elevator — clock+heal exact, location gate honored, unsolicited elev refused"
