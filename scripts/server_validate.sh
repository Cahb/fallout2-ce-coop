#!/usr/bin/env bash
# Validate the fixed-timestep server loop against the legacy goldens.
# For each of the 14 cases: run under F2_SERVER_LOOP twice (determinism), then
# emit a legacy-vs-server semantic summary. Input-replay SPACE traces are
# dropped (auto-end-turn); the walk trace is replaced by a walkto intent.
#
#   game_time / rng_state / queue_next_time / ambient-critter wander legitimately
#   SHIFT (fixed-step clock + instant animation). Invariants that must MATCH:
#   map/elevation, dude alive-or-dead, engaged-critter count, deterministic 2x.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
G="$ROOT/tests/golden"
# Dumps go to a gitignored scratch dir; the canonical server goldens are owned
# by tests/golden/run_golden_server.sh (BLESS=1). This tool is for the
# field-level legacy-vs-server semantic review before a re-bless.
OUT="${OUT:-$ROOT/build/server-validate}"
mkdir -p "$OUT"

# name  map  seed  ticks  aggro  server_actions
CASES=(
  "artemple_idle|artemple.map|1337|2000||"
  "arvillag_idle|arvillag.map|42|3000||"
  "arvillag_walk|arvillag.map|42|3000||300:walkto:20527"
  "klatoxcv_combat|klatoxcv.map|42|4000|3|"
  "arvillag_actions|arvillag.map|42|2500||300:xp:4500,600:rad:150,900:poison:45,1200:drug:40,1500:levelup:1"
  "arvillag_skills|arvillag.map|42|2500||300:hurt:25,600:useskill:6,900:useskill:7,1200:useskill:8"
  "kladwtwn_door|kladwtwn.map|42|2200||300:usedoor:0,900:usedoor:0"
  "arvillag_invmenu|arvillag.map|42|2500||300:give:41,320:give:41,340:give:41,400:drop:41,450:give:41,470:drop:41,500:give:206,550:drop:206,600:give:8,650:unload:8,680:hurt:20,700:give:40,720:usedrug:40,800:give:79,850:useitem:79,900:give:53,920:give:53,940:give:53,1000:drop:53,1100:reload:8,1150:give:46,1200:give:53,1250:stow:53,1300:lootall:0,1350:stealall:0"
  "arvillag_wmtravel|arvillag.map|42|1200||300:hurt:30,400:wmtravel:2800230"
  "arvillag_perks|arvillag.map|42|1500||300:xp:80000,400:give:53,450:usedrug:53,500:perk:18,600:perk:28,700:perk:51,750:tag4:12,800:perk:52,850:mutate:2"
  "arvillag_rest|arvillag.map|42|1200||300:hurt:30,400:rest:90,500:restopt:8,600:restopt:12"
  "arvillag_restfight|arvillag.map|42|4000||250:mark:0,300:hurt:10,350:mark:0,400:rest:360,450:mark:0,500:aggro:3,3000:mark:0"
  "arvillag_chartxn|arvillag.map|42|1500||300:charsnap:0,400:hurt:25,500:levelup:0,600:charroll:0"
  "denbus1_loot|denbus1.map|42|1500||400:lootall:0,800:lootall:0"
)

dudeState() { # prints "tile=.. hp=.. (ALIVE|DEAD)"
  local f="$1" line hp
  line="$(grep -m1 '^dude ' "$f")"
  hp="$(sed -E 's/.* hp=(-?[0-9]+).*/\1/' <<<"$line")"
  local status=ALIVE; [ "$hp" -le 0 ] && status=DEAD
  echo "$(sed -E 's/^dude id=[0-9]+ //' <<<"$line") [$status]"
}
field() { grep -m1 "^$2 " "$1" | awk '{print $2}'; }
engaged() { grep -c 'who_hit_me=1[0-9]\{4\}' "$1"; }

printf '%-20s %-4s %-6s %-9s %-9s %s\n' CASE RC DETERM MAP/ELV DUDE NOTES
for row in "${CASES[@]}"; do
  IFS='|' read -r name map seed ticks aggro actions <<<"$row"
  a="$OUT/$name.a.txt"; b="$OUT/$name.b.txt"
  scripts/server_run_case.sh "$map" "$seed" "$ticks" "$a" "$aggro" "$actions"; rc1=$?
  scripts/server_run_case.sh "$map" "$seed" "$ticks" "$b" "$aggro" "$actions"; rc2=$?
  determ=NO; diff -q "$a" "$b" >/dev/null 2>&1 && determ=YES

  gold="$G/$name.golden.txt"
  mapL=$(field "$gold" map); elvL=$(field "$gold" elevation)
  mapS=$(field "$a" map);   elvS=$(field "$a" elevation)
  mapelv="ok"; { [ "$mapL" != "$mapS" ] || [ "$elvL" != "$elvS" ]; } && mapelv="DIFF!"
  gtL=$(field "$gold" game_time); gtS=$(field "$a" game_time)
  engL=$(engaged "$gold"); engS=$(engaged "$a")
  dudeL="$(dudeState "$gold")"; dudeS="$(dudeState "$a")"
  # dude alive/dead invariant
  stL="${dudeL##*[}"; stS="${dudeS##*[}"
  dstatus="ok"; [ "$stL" != "$stS" ] && dstatus="DUDE-STATUS-DIFF!"
  eng="eng $engL/$engS"; [ "$engL" != "$engS" ] && eng="ENG-DIFF $engL/$engS"

  printf '%-20s %d/%d  %-6s %-9s %-9s gt %s->%s | %s | %s\n' \
    "$name" "$rc1" "$rc2" "$determ" "$mapelv" "$dstatus" "$gtL" "$gtS" "$eng" "L:$stL S:$stS"
done
echo
echo "Server dumps written to $OUT/*.a.txt (scratch). Canonical server goldens"
echo "live in tests/golden/server/ (BLESS=1 tests/golden/run_golden_server.sh)."
