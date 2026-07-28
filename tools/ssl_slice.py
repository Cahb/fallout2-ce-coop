#!/usr/bin/env python3
"""Slice the decompiled script corpus down to just the procedures that matter.

The whole corpus (decomp/ALL_SCRIPTS.ssl) is ~27 MB / ~7M tokens — too big to read in one
pass. But almost every question ("which scripts relocate the player?", "which open a
cutscene input lock?") only needs the ENCLOSING PROCEDURE of each match, which is 1-2
orders of magnitude smaller.

    tools/ssl_slice.py --pattern 'move_to\\s*\\(\\s*dude_obj' --out reloc.txt
    tools/ssl_slice.py --pattern 'game_ui_disable' --pattern 'game_ui_enable' --count

Output uses the same markers as ALL_SCRIPTS.ssl so a slice and the full corpus read alike:

    #### SCRIPT START: NAME ####
    ### proc <name> ###
    <procedure body>
    #### SCRIPT END: NAME ####

CAVEAT: this is textual, not dataflow. `move_to(v)` where `v := dude_obj` earlier will NOT
match. It finds direct uses; treat the result as a lower bound.
"""
import argparse
import os
import re
import sys

PROC_RE = re.compile(r"^procedure\s+(\w+)")


def procedures(lines):
    """Yield (name, start, end) for each `procedure X ... end` block (declarations skipped)."""
    cur = None
    for i, line in enumerate(lines):
        m = PROC_RE.match(line)
        if m and not line.rstrip().endswith(";"):
            cur = (m.group(1), i)
        elif cur is not None and line.startswith("end"):
            yield cur[0], cur[1], i
            cur = None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssl-dir", default="decomp/ssl", help="per-file corpus (default decomp/ssl)")
    ap.add_argument("--pattern", action="append", required=True, metavar="REGEX",
                    help="match a procedure if its body matches; repeatable (OR)")
    ap.add_argument("--out", help="write the slice here (default stdout)")
    ap.add_argument("--count", action="store_true", help="print the census only, no slice")
    args = ap.parse_args()

    if not os.path.isdir(args.ssl_dir):
        sys.exit("%s not found — run tools/decomp_scripts.sh first" % args.ssl_dir)

    pats = [re.compile(p) for p in args.pattern]
    chunks = []
    nscript = nproc = 0

    for f in sorted(os.listdir(args.ssl_dir)):
        if not f.endswith(".ssl"):
            continue
        lines = open(os.path.join(args.ssl_dir, f), errors="replace").read().splitlines()
        hits = []
        for name, a, b in procedures(lines):
            body = "\n".join(lines[a:b + 1])
            if any(p.search(body) for p in pats):
                hits.append((name, body))
        if not hits:
            continue
        nscript += 1
        nproc += len(hits)
        script = f[:-4]
        chunks.append("#### SCRIPT START: %s ####" % script)
        chunks.extend("### proc %s ###\n%s" % (n, b) for n, b in hits)
        chunks.append("#### SCRIPT END: %s ####\n" % script)

    text = "\n".join(chunks)
    sys.stderr.write("scripts=%d procs=%d size=%.2f MB est_tokens=%.0fk\n"
                     % (nscript, nproc, len(text) / 1048576, len(text) / 3.5 / 1000))
    if args.count:
        return
    if args.out:
        open(args.out, "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
