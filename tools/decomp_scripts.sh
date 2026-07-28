#!/usr/bin/env bash
# Decompile every shipped Fallout 2 .int script to readable .ssl source.
#
# Output (all gitignored, see decomp/README.md):
#   decomp/ssl/<NAME>.ssl     one file per script
#   decomp/ALL_SCRIPTS.ssl    all of them concatenated with #### SCRIPT START/END #### markers
#
# The .int bytecode lives inside the DAT archives, not on disk — tools/f2data.py pulls it out
# in engine search order (patch000.dat SHADOWS master.dat; getting that backwards silently
# yields stale scripts). Decompiler is falltergeist/int2ssl, cloned and built on first run.
#
# Usage:  tools/decomp_scripts.sh [--root FO2] [--force]
set -euo pipefail

ROOT="FO2"
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
OUT="$REPO/decomp"
INT2SSL="$OUT/.int2ssl/build/int2ssl"

if [ ! -x "$INT2SSL" ]; then
    echo "== building int2ssl (first run) =="
    rm -rf "$OUT/.int2ssl"
    mkdir -p "$OUT"
    git clone --depth 1 https://github.com/falltergeist/int2ssl.git "$OUT/.int2ssl"
    mkdir -p "$OUT/.int2ssl/build"
    ( cd "$OUT/.int2ssl/build" && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j"$(nproc)" >/dev/null )
fi

if [ -d "$OUT/ssl" ] && [ "$FORCE" -eq 0 ]; then
    echo "decomp/ssl already exists ($(ls "$OUT/ssl" | wc -l) files) — pass --force to rebuild"
    exit 0
fi

echo "== extracting .int from the DAT archives =="
rm -rf "$OUT/int" "$OUT/ssl"
mkdir -p "$OUT/int" "$OUT/ssl"
FO2_ROOT="$ROOT" OUT_DIR="$OUT/int" python3 - <<'PY'
import importlib.util, os
spec = importlib.util.spec_from_file_location("f2data", "tools/f2data.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
dat = m.Dat(os.environ["FO2_ROOT"])
out = os.environ["OUT_DIR"]
n = 0
for name in dat.names(".int"):
    raw = dat.get(name)          # first (= winning) copy, engine search order
    if not raw:
        continue
    open(os.path.join(out, name.split("/")[-1]), "wb").write(raw)
    n += 1
print("  extracted %d .int" % n)
PY

echo "== decompiling =="
( cd "$OUT/int" && for f in *.INT; do "$INT2SSL" "$f" >/dev/null 2>&1 || true; done )
( cd "$OUT/int" && for f in *.ssl; do [ -e "$f" ] || continue; mv "$f" "$OUT/ssl/${f%.INT.ssl}.ssl"; done )
rm -rf "$OUT/int"

echo "== concatenating =="
OUT_DIR="$OUT" python3 - <<'PY'
import os
out = os.environ["OUT_DIR"]
d = os.path.join(out, "ssl")
files = sorted(f for f in os.listdir(d) if f.endswith(".ssl"))
n = 0; empty = []
with open(os.path.join(out, "ALL_SCRIPTS.ssl"), "w") as w:
    for f in files:
        name = f[:-4]
        body = open(os.path.join(d, f), errors="replace").read()
        if not body.strip():
            empty.append(name); continue
        w.write("#### SCRIPT START: %s ####\n" % name)
        w.write(body if body.endswith("\n") else body + "\n")
        w.write("#### SCRIPT END: %s ####\n\n" % name)
        n += 1
size = os.path.getsize(os.path.join(out, "ALL_SCRIPTS.ssl")) / 1048576
print("  %d scripts -> ALL_SCRIPTS.ssl (%.1f MB)" % (n, size))
if empty:
    print("  int2ssl produced NOTHING for %d (known gap): %s" % (len(empty), ", ".join(empty)))
PY

echo "done. slice it with: tools/ssl_slice.py --pattern '<regex>' --out slice.txt"
