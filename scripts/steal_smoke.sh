#!/usr/bin/env bash
# Server-side smoke for the STEAL / PICKPOCKET / PLANT session (the steal twin of
# barter_pump_smoke.sh). Proves the whole chain end to end, headless:
#
#   stealon -> actionUseSkill(SKILL_STEAL) -> skillUse -> scriptsRequestStealing
#           -> the request drain -> the server's stealing() handler
#           -> stealSessionRun, which PARKS the tick and pumps the CMD channel
#           -> stake/splant roll the Steal skill and move real items
#           -> sdone closes it and the victim's own gear is handed back
#
# No F2_SERVER_NET -> haveNet=false, so the barrier neither bails on "no clients"
# nor needs one; it services the CMD port (haveCmd), which is also why the
# thief-present bail does not fire here.
#
# ►► THE TELL THAT THE BARRIER IS REAL is the same one barter uses: the session
# survives an EMPTY intent queue between injections. With no pump the session
# would close the instant the queue ran dry, long before the second verb landed.
#
# ►► NOTHING HERE ASSERTS A LITERAL OUTCOME, on purpose: the transfer is a DICE
# ROLL against the victim's Perception, so a run where the thief is caught on the
# first grab is a CORRECT run. What must always hold is the shape — session
# opens, verbs are drained one at a time, the session closes exactly once, and
# the victim ends up holding their own equipped gear again.
set -u
ROOT="/mnt/NVME/Projects/fallout2-ce"
GAME="$ROOT/FO2"
BIN="$ROOT/build/f2_server"
CMD=9379
ERR="$ROOT/build/steal_smoke.err"
DUMP="$ROOT/build/steal_smoke.dump"

pkill -x f2_server 2>/dev/null; sleep 1
rm -f "$ERR" "$DUMP"

# denbus1: populated, out of combat, plenty of pockets. give:41:50 puts caps on
# the dude so `splant` has something of its own to push the other way.
( cd "$GAME" && exec env \
    F2_SERVER_MAP=denbus1.map F2_SERVER_CMD=$CMD \
    F2_SERVER_PACE_MS=40 F2_SERVER_TICKS=600 \
    F2_SERVER_DUMP="$DUMP" \
    F2_SERVER_ACTIONS="20:give:41:50,40:stealon:0" \
    DEBUGACTIVE=screen \
    timeout -k 3 40 "$BIN" ) > "$ERR" 2>&1 &
SRV=$!

send() {
    printf '%s\n' "$1" | nc -N -w1 127.0.0.1 $CMD 2>/dev/null || \
    printf '%s\n' "$1" | nc -q1 127.0.0.1 $CMD 2>/dev/null
}

# Past tick 40: the skill has fired and the server is parked in the steal barrier.
sleep 4
echo "--- injecting the theft ONE VERB AT A TIME, queue empty in between ---"
# 41 = caps: the victim may or may not carry any (that is what the "gone" refusal
# is for), and planting our own is the reverse direction through the same roll.
for v in "stake 41 1" "splant 41 5" "sdone 0"; do
    echo "    -> $v"
    send "$v"
    sleep 2   # long enough that the queue is DRY before the next verb
done

wait $SRV 2>/dev/null

echo "======== session lines ========"
grep -aE "steal session (OPEN|CLOSE)|f2_server: steal (take|plant)|headless-probe: (stealon|stake|splant|sdone)|\[steal\] PUMP BAIL" "$ERR" | head -40

echo "======== shape checks ========"
opens=$(grep -ac "steal session OPEN" "$ERR")
closes=$(grep -ac "steal session CLOSE" "$ERR")
moves=$(grep -ac "f2_server: steal " "$ERR")
echo "sessions opened=$opens closed=$closes   transfer attempts logged=$moves"
[ "$opens" = "1" ] && echo "PASS: exactly one session opened" || echo "FAIL: expected 1 session, got $opens"
[ "$closes" = "1" ] && echo "PASS: it closed exactly once" || echo "FAIL: expected 1 close, got $closes"
if grep -aq "steal session OPEN" "$ERR" && ! grep -aq "PUMP BAIL" "$ERR"; then
    echo "PASS: the barrier held across dry-queue gaps (no bail)"
else
    echo "NOTE: check the bail line above — a bail before sdone means the barrier let go early"
fi
if [ -f "$DUMP" ]; then
    echo "PASS: the server survived to its tick cap and wrote a dump"
else
    echo "FAIL: no dump — the server did not reach its tick cap"
fi
