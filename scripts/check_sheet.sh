#!/usr/bin/env bash
# CHARACTER-SHEET EDIT INTENT gate (PLAYER_SHEET_DESIGN.md §9).
#
# Drives the sheet edit rulings through the admin channel against a TWO-SEAT server
# and asserts the answers. Every verb here runs the same sheetEdit* path a client's
# wire intent runs (server_control.cc dispatches to the identical calls), so this
# gates the ruling layer even though the character SCREEN can only be checked live.
#
# ►► WHY N>1 MATTERS HERE. This is the one thing per-actor sheets exist to get right
# and the one thing the golden suite structurally cannot see: the goldens run a single
# actor, where "the acting player's row" and "gDude's row" are the same row, so every
# subject bug is invisible. F2_SERVER_PLAYERS=2 makes them different rows, and the
# assertions below pin that a spend on slot 1 moves slot 1 and LEAVES SLOT 0 ALONE —
# slot 0 being gDudeProto itself, the one row a dispatch bug corrupts silently.
#
# What is asserted, in order:
#   1. an unfunded seat is REFUSED a skill point            (entitlement)
#   2. a funded seat spends one, and the value moves by 1   (the spend works)
#   3. slot 0's skill value and points are UNTOUCHED         (subject routing)
#   4. an out-of-range skill index is refused, not indexed   (untrusted input)
#   5. a perk with no owed pick is REFUSED                   (perk entitlement)
#   6. "-" walks the point back and refunds it               (the undo baseline)
#   7. "-" past the baseline is refused                      (no point fountain)
#   8. an XP grant to slot 1 owes SLOT 1 a perk and slot 0 NOTHING, and slot 0 is
#      then still refused a pick                             (per-actor entitlement)
#
# ►► #8 is the one that caught a real bug during development: the owed-pick resolver
# short-circuited on `subject == gDude`, which is not a test for "the host" — a
# ServerActorScope rebinds gDude to the ACTING actor, so every seat's pick resolved to
# slot 0's. It read correctly at N==1 and corrupted the host's character at N>1.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${F2_GAME_DIR:-$ROOT/FO2}"
OUT="$ROOT/build/sheet-gate"
CMD=9341
SRV=""

fail() {
    echo "FAIL sheet — $1"
    tail -25 "$OUT/server.err" 2>/dev/null | sed 's/^/    srv| /'
    kill "$SRV" 2>/dev/null
    wait 2>/dev/null
    exit 1
}

[ -x "$ROOT/build/f2_server" ] || { echo "FAIL sheet — no f2_server (build broken?)"; exit 1; }
[ -d "$GAME" ] || { echo "FAIL sheet — no FO2/ assets"; exit 1; }
command -v nc > /dev/null 2>&1 || { echo "SKIP sheet — no nc(1) to drive the admin channel"; exit 0; }

mkdir -p "$OUT"
rm -f "$OUT/server.err" "$OUT/replies.txt"

pkill -x f2_server 2>/dev/null && sleep 1

# artemple: the map every server gate uses (clean + walkable under f2_server). No
# wire port — this gate needs no viewer, and the sheet verbs are not world actions.
( cd "$GAME" && exec env F2_SERVER_MAP=artemple.map F2_SERVER_CMD=$CMD \
    F2_SERVER_PLAYERS=2 F2_SERVER_PACE_MS=20 F2_SERVER_TICKS=5000000 \
    "$ROOT/build/f2_server" > "$OUT/server.err" 2>&1 ) &
SRV=$!
sleep 3
kill -0 "$SRV" 2>/dev/null || fail "server died at boot"

# One connection, one line per verb; the admin channel replies in line order.
# SKILL 12 = SMALL_GUNS (skill_defs.h) — any skill would do; a named one keeps the
# expected values readable.
#
# ⚠ THE `sleep` IS LOAD-BEARING, do not "simplify" this to `echo … | nc`. Closing
# stdin half-closes the socket; the server sees recv()==0, drops the client, and only
# THEN dispatches the lines it already buffered — so the verbs run but every reply is
# written to a closed fd and lost. Holding the pipe open until the beats that answer
# have gone by is what makes the replies observable at all. (Operators scripting the
# admin port with `echo | nc` hit the same thing: the command takes effect, the
# confirmation vanishes.)
ask() {
    { printf '%s\n' "$@"; sleep 6; } | timeout 30 nc 127.0.0.1 $CMD 2>/dev/null
}

# `xp 1 6000` crosses level 3, the free-perk cadence, for SLOT 1 ONLY — the award now
# happens in the XP funnel, on the level, for the actor that earned it.
REPLIES="$(ask \
    'sheet 0' \
    'sheet 1' \
    'sp 1 0' \
    'skillup 1 12' \
    'sp 1 20' \
    'skillup 1 12' \
    'skillup 1 999' \
    'perkpick 1 18' \
    'skilldown 1 12' \
    'skilldown 1 12' \
    'xp 1 6000' \
    'sheet 1' \
    'perkpick 0 18' \
    'sheet 0')"
echo "$REPLIES" > "$OUT/replies.txt"

kill "$SRV" 2>/dev/null
wait 2>/dev/null

[ -n "$REPLIES" ] || fail "admin channel gave no replies (port $CMD unreachable?)"

# Slot 0's row, first and last reading, must be identical: nothing done to slot 1
# may touch the host. Compared as whole lines, so a stray point or a moved tag
# fails too.
SLOT0_BEFORE="$(grep -m1 '^sheet 0' "$OUT/replies.txt")"
SLOT0_AFTER="$(grep '^sheet 0' "$OUT/replies.txt" | tail -1)"
[ -n "$SLOT0_BEFORE" ] || fail "no 'sheet 0' read-out (verb missing?)"
[ "$SLOT0_BEFORE" = "$SLOT0_AFTER" ] || fail "slot 0's sheet CHANGED while editing slot 1:
    before| $SLOT0_BEFORE
    after | $SLOT0_AFTER"

# 1 + 5: refusals for an unfunded seat and an unowed perk.
grep -q 'skillup: slot 1 .*Not enough skill points' "$OUT/replies.txt" \
    || fail "an unfunded seat was NOT refused a skill point"
grep -q 'perkpick: slot 1 .*no perk to pick' "$OUT/replies.txt" \
    || fail "a perk with no owed pick was NOT refused"

# 4: an out-of-range index must be refused by name, never indexed.
grep -q 'skillup: slot 1 .*skill 999 -> That is not something you can pick' "$OUT/replies.txt" \
    || fail "skill index 999 was not refused as out of range"

# 2 + 6 + 7: the funded spend, the refund, and the baseline floor. The funded
# skillup leaves 19 of 20 points (the first point in a skill costs 1), the refund
# puts it back to 20, and the second "-" is refused because the baseline is reached.
grep -q 'skillup: slot 1 .*skill 12 -> Done\..*19 points left' "$OUT/replies.txt" \
    || fail "the funded spend did not land (expected 19 of 20 points left)"
grep -q 'skilldown: slot 1 .*skill 12 -> Done\..*20 points left' "$OUT/replies.txt" \
    || fail "the refund did not land"
grep -q "skilldown: slot 1 .*skill 12 -> You can't lower it any further" "$OUT/replies.txt" \
    || fail "a '-' past the session baseline was NOT refused (point fountain)"

# 8: the level-up entitlement is PER SEAT. Slot 1 crossed the perk cadence, so slot 1
# owes a pick; slot 0 earned nothing and must both READ as owing nothing and be
# REFUSED one. (The whole-line slot-0 comparison above already pins that its level and
# XP did not move either.)
grep '^sheet 1' "$OUT/replies.txt" | tail -1 | grep -q 'owed perk YES' \
    || fail "slot 1 crossed the free-perk level but is not owed a pick:
    $(grep '^sheet 1' "$OUT/replies.txt" | tail -1)"
grep '^sheet 0' "$OUT/replies.txt" | tail -1 | grep -q 'owed perk no' \
    || fail "slot 1's level-up left an owed perk on SLOT 0 (the host's own character):
    $(grep '^sheet 0' "$OUT/replies.txt" | tail -1)"
grep -q 'perkpick: slot 0 .*no perk to pick' "$OUT/replies.txt" \
    || fail "slot 0 was allowed a perk pick it never earned"

# The greppable server-side trace the whole path emits (gate-scripts contract).
grep -q 'f2_server: admin skillup slot=1' "$OUT/server.err" \
    || fail "server never logged the admin skillup"

echo "PASS sheet — 2 seats: spend rulings + per-seat level-up entitlement, slot 0 untouched"
