#!/usr/bin/env bash
# RECORD-PURITY gate (PRESENTATION_RECORD_REPLAY_SPEC.md).
#
# The presentation recorder runs on the DEDICATED SERVER (f2_server, the server_anim.cc
# backend): to record an animation it runs the engine's animate branch it otherwise skips
# — creating transient objects, drawing cosmetic RNG, walking reg_anim — and claims to be
# sim-neutral via an RNG snapshot/restore + transient cleanup. This gate PROVES it.
#
# DIFFERENTIAL, not baseline: each case runs f2_server twice on a DETERMINISTIC map with a
# record-exercising scenario — once with F2_SERVER_PRES_RECORD off, once on — and requires:
#   (1) the state dump byte-identical off vs on  (saveable sim state + RNG), and
#   (2) the NO_SAVE object count identical off vs on  (transient-leak check — NO_SAVE
#       objects are skipped by the dump, so a leaked explosion cloud is invisible to (1)).
# Purity == identity, so there is NO checked-in baseline and NO reblessing; adding a record
# family just adds a CASE. A divergence means record mode perturbed the sim: an unrestored
# RNG draw, a double-applied state callback, or a leaked transient.
#
# Only the RECORDING backend (f2_server) is meaningful here — the client/golden binary
# (fallout2-ce) links animation.cc, where the recorder is inert by design (see
# pres_record.h presRecordSetBackendActive).
#
# Usage: tests/golden/run_record_purity.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME_DIR="${F2_GAME_DIR:-$ROOT/FO2}"
BIN="${F2_SERVER_BIN:-$ROOT/build/f2_server}"
SEED="${F2_SERVER_SEED:-1337}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; pkill -9 -x f2_server 2>/dev/null || true' EXIT

# name  map           ticks  actions (F2_SERVER_ACTIONS "tick:verb:arg,...")
# Deterministic maps only (run-to-run stable: kladwtwn/artemple/newr1/vault13/arcaves).
# Add a row per record family as it is migrated (door + explosion cover Table A today).
CASES=(
  "door|kladwtwn.map|600|300:usedoor:0"
  "explosion|kladwtwn.map|600|300:explode:200"
  "cdamage|kladwtwn.map|800|300:cdamage:100,600:cdamage:15"
  "wield|kladwtwn.map|600|300:give:7,360:wield:0:1"
  "melee|artemple.map|4000|500:cattack:60,505:aggro:3"
)

run() { # $1=dump path  $2=err path  $3=REC (non-empty => record on)
    (cd "$GAME_DIR" && env \
        F2_SERVER_MAP="$MAP" F2_SERVER_SEED="$SEED" F2_SERVER_TICKS="$TICKS" \
        F2_SERVER_ACTIONS="$ACTIONS" F2_SERVER_DUMP="$1" F2_SERVER_LEAKPROBE=1 \
        ${3:+F2_SERVER_PRES_RECORD=1} \
        timeout -k 5 120 "$BIN" > /dev/null 2>"$2") || { echo "  (binary exited non-zero)"; return 1; }
}

fail=0
for row in "${CASES[@]}"; do
    IFS='|' read -r name MAP TICKS ACTIONS <<< "$row"

    run "$TMP/$name.off" "$TMP/$name.off.err" ""    || { echo "FAIL record-purity $name (off run crashed)"; fail=1; continue; }
    run "$TMP/$name.on"  "$TMP/$name.on.err"  REC   || { echo "FAIL record-purity $name (on run crashed)"; fail=1; continue; }

    off_leak="$(grep -o 'no_save=[0-9]*' "$TMP/$name.off.err" | tail -1)"
    on_leak="$(grep -o 'no_save=[0-9]*' "$TMP/$name.on.err" | tail -1)"

    if diff -q "$TMP/$name.off" "$TMP/$name.on" > /dev/null 2>&1 && [ "$off_leak" = "$on_leak" ]; then
        echo "PASS record-purity $name (state + transients identical off/on; $off_leak)"
    else
        echo "FAIL record-purity $name — record mode perturbed the simulation:"
        diff "$TMP/$name.off" "$TMP/$name.on" | head -20 || true
        [ "$off_leak" != "$on_leak" ] && echo "  TRANSIENT LEAK: off $off_leak vs on $on_leak"
        fail=1
    fi
done

exit "$fail"
