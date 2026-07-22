#!/usr/bin/env bash
# Live eyeball test for the message-log CHANNEL STYLES (msg_channel.h) and the
# join greeting. Not a gate — there is no headless oracle for "is it legible",
# which is the whole question (AGENTS/visual-verification-protocol.md).
#
# Assumes a server + viewer are ALREADY UP via scripts/viewer_live.sh, and drives
# the command port. Run it, watch the log in the interface bar, answer the
# questions it prints at the end.
#
#   Terminal 1:  F2_SERVER_NAME="Cahb's Wasteland" scripts/viewer_live.sh
#   Terminal 2:  scripts/msg_style_demo.sh
set -u

PORT="${F2_SERVER_CMD:-9201}"
HOST="${F2_SERVER_HOST:-127.0.0.1}"

# One line, one connection: nc -q1 is flaky here (p5-server-plan), and a fresh
# connection per verb is the reliable shape. Slow enough to watch each land.
send() {
    printf '%s\n' "$1" | timeout 3 nc -q1 "$HOST" "$PORT" >/dev/null 2>&1
    sleep "${2:-1.2}"
}

if ! timeout 3 nc -z "$HOST" "$PORT" 2>/dev/null; then
    echo "no command port on $HOST:$PORT — start scripts/viewer_live.sh first" >&2
    exit 1
fi

echo "=== 1. every channel, back to back (the palette) ==="
send 'saydemo' 3

echo "=== 2. single stylized liners (each channel alone, in isolation) ==="
send 'say system Server restarting in 5 minutes.'
send 'say chat Narg: got any stimpaks?'
send 'say reward You gained a level.'
send 'say refusal You do not have enough action points.'
send 'say combat The radscorpion was hit for 12 points.'
send 'say default Vanilla line — this must look exactly like it always did.'

echo "=== 3. WRAPPING: a styled line longer than 80 columns ==="
echo "    (all of its wrapped rows must carry the SAME colour)"
send 'say reward You have earned an enormous and frankly implausible quantity of experience points for doing something only moderately heroic.' 2

echo "=== 4. INTERLEAVING: styles alternating, to check nothing bleeds ==="
for i in 1 2 3; do
    send "say combat combat line $i" 0.4
    send "say chat chat line $i" 0.4
    send "say system system line $i" 0.4
done
sleep 2

echo "=== 5. REAL combat narration on the default channel ==="
echo "    (unstyled engine text must be UNCHANGED — this is the regression check)"
send 'aggro 5' 6

cat <<'EOF'

────────────────────────────────────────────────────────────────────
Please answer:

 1. Are the six channels actually TELLABLE APART at a glance, in the
    real 80-column green-on-black monitor? (default green / combat amber /
    refusal grey / system pale-blue+shadow / chat white / reward
    yellow+underline)
 2. Does any colour come out MUDDY or near-black? The palette is 256
    entries and quantizes silently — that is the expected failure.
 3. Is `refusal` grey too dim to read, or correctly recessive?
 4. Does the FONT_UNDERLINE on `reward` and FONT_SHADOW on `system`
    render cleanly at font 101, or do they smear into the row below?
 5. Step 3: did every wrapped row of the long reward line share one colour?
 6. Step 5: does ordinary combat text look EXACTLY as it did before?
    (it must — default resolves to the same _colorTable[992])
 7. Any row drift / clipping at the bottom of the monitor rectangle?

Then, on a SECOND viewer connecting (co-op):
 8. Does the joiner get the greeting — server name, "You are <name>, slot N",
    "M/K players online", the Online: roster?
 9. Does the player ALREADY in the game see "<name> joined the game."?
10. Is the greeting readable, or does the server name wrap badly?
────────────────────────────────────────────────────────────────────
EOF
