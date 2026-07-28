#!/usr/bin/env python3
"""Probe the SHIPPED Fallout 2 game data (DAT2 archives) from the command line.

This is the standing implementation of the recipe in the project memory
("game data probing"): every question of the form "what IS this thing the server
just logged?" should be one command here, not a fresh ad-hoc script.

    tools/f2data.py map 117                  # server logged "entered map=117" -> what map is that?
    tools/f2data.py map 117 --find Well      # is the well on it scripted (i.e. does it DO anything)?
    tools/f2data.py map desrt12              # same, by internal name
    tools/f2data.py proto scenery Well       # which proto(s) is "Well", and is it USE-able?
    tools/f2data.py script 313               # scripts.lst line 313 -> RndDsrt.int
    tools/f2data.py area 25                  # city.txt worldmap area block (entrances etc.)
    tools/f2data.py ls MAPS/DESRT            # what is in the archives
    tools/f2data.py cat data/maps.txt        # dump any archived file

►► TRAP THIS TOOL HANDLES FOR YOU: patch000.dat SHADOWS master.dat and the engine
searches it FIRST, so decoding the master's copy silently gives STALE data. Every
lookup here tries the patch archive before the master, and `--from` reports which
archive answered.

►► VALIDATION HABIT: check a decode against a number the engine itself printed
before believing it. `map` does this for free — the .map header carries its own
`index`, so a mismatch against the maps.txt section number is loud, not silent.
"""

import argparse
import os
import re
import struct
import sys
import zlib

# Archive search order = the engine's: patch first, then master, then the rest.
ARCHIVES = ["patch000.dat", "master.dat", "critter.dat", "f2_res.dat"]

# Proto type index -> (subdirectory, message file). PID_TYPE(pid) == the object type.
PROTO_TYPES = {
    0: ("items", "pro_item.msg"),
    1: ("critters", "pro_crit.msg"),
    2: ("scenery", "pro_scen.msg"),
    3: ("walls", "pro_wall.msg"),
    4: ("tiles", "pro_tile.msg"),
    5: ("misc", "pro_misc.msg"),
}
TYPE_NAMES = {0: "item", 1: "critter", 2: "scenery", 3: "wall", 4: "tile", 5: "misc"}


class Dat:
    """The DAT2 archives of one FO2 install, searched in engine order."""

    def __init__(self, root):
        self.root = root
        self.roots = []  # [(tree, blob, archive_name)]
        for name in ARCHIVES:
            path = os.path.join(root, name)
            if os.path.exists(path):
                self.roots.append(self._load(path) + (name,))
        if not self.roots:
            sys.exit("no DAT archives under %s (expected master.dat)" % root)

    @staticmethod
    def _load(path):
        blob = open(path, "rb").read()
        tree_size, _data_size = struct.unpack("<II", blob[-8:])
        p = len(blob) - 8 - tree_size
        (count,) = struct.unpack("<I", blob[p : p + 4])
        p += 4
        tree = {}
        for _ in range(count):
            (name_len,) = struct.unpack("<I", blob[p : p + 4])
            p += 4
            name = blob[p : p + name_len].decode("ascii", "replace")
            p += name_len
            kind = blob[p]
            p += 1
            real, packed, off = struct.unpack("<III", blob[p : p + 12])
            p += 12
            tree[norm(name)] = (kind, real, packed, off)
        return tree, blob

    def get(self, name):
        """Bytes of an archived file, or None. Returns the first (= winning) copy."""
        key = norm(name)
        for tree, blob, _archive in self.roots:
            entry = tree.get(key)
            if entry is None:
                continue
            kind, real, packed, off = entry
            data = blob[off : off + packed]
            return zlib.decompress(data) if kind == 1 else blob[off : off + real]
        return None

    def which(self, name):
        key = norm(name)
        for tree, _blob, archive in self.roots:
            if key in tree:
                return archive
        return None

    def names(self, pattern=""):
        pattern = norm(pattern)
        seen = []
        for tree, _blob, _archive in self.roots:
            seen.extend(k for k in tree if pattern in k)
        return sorted(set(seen))

    def text(self, name):
        raw = self.get(name)
        # cp1252, not utf-8: the shipped text uses the DOS/Windows codepage.
        return None if raw is None else raw.decode("cp1252", "replace")


def norm(name):
    return name.upper().replace("\\", "/")


def msg_strings(dat, msg_file):
    """{id: text} from a .msg file ({id}{}{text} records, one per line)."""
    text = dat.text("text/english/game/" + msg_file)
    if text is None:
        return {}
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s*\{(\d+)\}\{[^}]*\}\{(.*)\}", line)
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


def proto_name(dat, pid):
    """(name, description) for a PID, from the type's pro_*.msg."""
    ptype, index = pid >> 24, pid & 0xFFFFFF
    if ptype not in PROTO_TYPES:
        return None, None
    strings = msg_strings(dat, PROTO_TYPES[ptype][1])
    # msg id == proto index * 100; +1 is the description line.
    return strings.get(index * 100), strings.get(index * 100 + 1)


def read_proto(dat, ptype, index):
    """Parse a .pro header. ►► The FILE CONTENTS ARE BIG-ENDIAN (the DAT tree is not).

    ►► THE FILE NUMBER IS THE PROTO INDEX ITSELF — no +1. This used to say "+1" and
    read `index + 1`, which is the .lst 1-based convention leaking somewhere it does
    not apply, and it made this tool answer CONFIDENTLY WRONG: the name comes from
    pro_*.msg at index*100 (0-based, correct) while every FIELD came from the NEXT
    proto, so `proto item 365` printed "Plant Spike" with the G.E.C.K.'s type and
    flags. A throwable weapon read as an unusable misc item.

    Verified against the data rather than assumed: a .pro self-identifies in its
    first big-endian int, whose low 24 bits are the index. proto/items/00000365.pro
    starts 0x0000016D (=365), critters/00000016.pro is 0x01000010 (=16),
    scenery/00000012.pro is 0x0200000C, misc/00000049.pro is 0x05000031. Holds for
    every proto type, so there is no type where the +1 was right. The engine agrees:
    proto.cc `_proto_load_pid` builds the path straight from the pid.
    """
    subdir = PROTO_TYPES[ptype][0]
    raw = dat.get("proto/%s/%08d.pro" % (subdir, index))
    if raw is None:
        return None
    n = len(raw) // 4
    v = struct.unpack(">%di" % n, raw[: n * 4])
    fields = {
        "pid": v[0] & 0xFFFFFFFF,
        "message_id": v[1],
        "fid": v[2] & 0xFFFFFFFF,
        "light_distance": v[3],
        "light_intensity": v[4],
        "flags": v[5] & 0xFFFFFFFF,
        "extended_flags": v[6] & 0xFFFFFFFF,
        "sid": v[7],
        "type": v[8],
        "rest": list(v[9:]),
    }
    # proto.cc _proto_action_can_use: extendedFlags & 0x0800 (plus "any container").
    fields["can_use"] = bool(fields["extended_flags"] & 0x0800)
    fields["can_use_on"] = bool(fields["extended_flags"] & 0x1000)
    return fields


# ---------------------------------------------------------------- maps.txt

def maps_txt_sections(dat):
    """{index: {key: value}} for every [Map N] block in data/maps.txt."""
    text = dat.text("data/maps.txt")
    if text is None:
        return {}
    out = {}
    current = None
    for line in text.splitlines():
        head = re.match(r"\s*\[\s*Map\s+(\d+)\s*\]", line, re.I)
        if head:
            current = {}
            out[int(head.group(1))] = current
            continue
        if current is None:
            continue
        kv = re.match(r"\s*([A-Za-z_0-9]+)\s*=\s*(.*?)\s*(?:;.*)?$", line)
        if kv:
            current.setdefault(kv.group(1).lower(), kv.group(2))
    return out


def resolve_map(dat, token):
    """'117' or 'desrt12' -> (index, section dict). The index is the number the
    server logs (gMapHeader.index / [wmsrv] entered map=N)."""
    sections = maps_txt_sections(dat)
    if re.fullmatch(r"\d+", token):
        index = int(token)
        return index, sections.get(index)
    want = token.lower().removesuffix(".map")
    for index, section in sections.items():
        if section.get("map_name", "").lower() == want:
            return index, section
    return None, None


# ---------------------------------------------------------------- .map file

MAP_HEADER = [
    "version", "_name", "entering_tile", "entering_elevation", "entering_rotation",
    "local_variables_count", "map_script_index", "flags", "darkness",
    "global_variables_count", "index", "ticks",
]

# object.cc objectRead field order, up to the fields that answer "is it live?".
OBJECT_FIELDS = [
    "id", "tile", "x", "y", "sx", "sy", "frame", "rotation", "fid", "flags",
    "elevation", "pid", "cid", "light_distance", "light_intensity", "outline",
    "sid", "script_index",
]
PID_OFFSET = OBJECT_FIELDS.index("pid") * 4


def read_map_header(raw):
    (version,) = struct.unpack(">I", raw[0:4])
    name = raw[4:20].split(b"\0")[0].decode("ascii", "replace")
    ints = struct.unpack(">10i", raw[20:60])
    out = {"version": version, "name": name}
    out.update(dict(zip(MAP_HEADER[2:], ints)))
    return out


def find_objects_by_pid(raw, pid):
    """Every object record carrying `pid`, decoded from the record start.

    Deliberately a SCAN, not a full object-list walk: the record is
    variable-length (per-type tail + inventory), and a scan answers the question
    that actually comes up — "this thing is on the map, is it scripted?" — with no
    format risk. sid/script_index == -1 on both the instance AND the proto means
    the object is scenery-only: it may still offer USE, and nothing will happen.
    """
    needle = struct.pack(">I", pid)
    found = []
    at = raw.find(needle)
    while at != -1:
        start = at - PID_OFFSET
        if start >= 0 and start + len(OBJECT_FIELDS) * 4 <= len(raw):
            values = struct.unpack(">%di" % len(OBJECT_FIELDS), raw[start : start + len(OBJECT_FIELDS) * 4])
            record = dict(zip(OBJECT_FIELDS, values))
            if (record["pid"] & 0xFFFFFFFF) == pid:
                record["pid"] &= 0xFFFFFFFF
                record["fid"] &= 0xFFFFFFFF
                record["flags"] &= 0xFFFFFFFF
                found.append(record)
        at = raw.find(needle, at + 1)
    return found


def pids_matching_name(dat, needle, ptypes=(0, 1, 2, 3, 5)):
    """PIDs whose proto NAME matches `needle` (case-insensitive, exact word)."""
    out = []
    for ptype in ptypes:
        strings = msg_strings(dat, PROTO_TYPES[ptype][1])
        for msg_id, value in strings.items():
            if msg_id % 100 != 0:
                continue
            if value.strip().lower() == needle.strip().lower():
                out.append((ptype << 24) | (msg_id // 100))
    return sorted(out)


# ---------------------------------------------------------------- commands

def cmd_ls(dat, args):
    for name in dat.names(args.pattern):
        print("%-40s [%s]" % (name, dat.which(name)))


def cmd_cat(dat, args):
    raw = dat.get(args.path)
    if raw is None:
        sys.exit("not in any archive: %s" % args.path)
    if args.out:
        open(args.out, "wb").write(raw)
        print("wrote %d bytes to %s (from %s)" % (len(raw), args.out, dat.which(args.path)))
    else:
        sys.stdout.write(raw.decode("cp1252", "replace"))


def cmd_map(dat, args):
    index, section = resolve_map(dat, args.map)
    if section is None:
        sys.exit("no maps.txt entry for %r" % args.map)
    map_name = section.get("map_name", "?")
    print("map index %d = %s.map — %s" % (index, map_name, section.get("lookup_name", "?")))
    for key in ("music", "saved", "dead_bodies_age", "can_rest_here"):
        if key in section:
            print("  %-16s %s" % (key, section[key]))
    if section.get("saved", "").lower().startswith("no"):
        print("  ►► saved=No: this map is REBUILT on every visit — nothing left here persists.")

    raw = dat.get("maps/%s.map" % map_name)
    if raw is None:
        sys.exit("maps/%s.map not in any archive" % map_name)
    header = read_map_header(raw)
    print("\n%s.map (%d bytes, from %s)" % (map_name, len(raw), dat.which("maps/%s.map" % map_name)))
    for key in MAP_HEADER:
        if key.startswith("_"):
            continue
        print("  %-24s %s" % (key, header.get(key)))
    # Engine-printed oracle: the header's own index must match the section number.
    if header.get("index") != index:
        print("  ⚠ header index %s != maps.txt [Map %d] — decode is off" % (header.get("index"), index))
    script_index = header.get("map_script_index", -1)
    if script_index >= 0:
        print("  map script            %s" % (script_line(dat, script_index) or "?"))

    for needle in args.find or []:
        pids = [int(needle, 16)] if re.fullmatch(r"(0x)?[0-9a-fA-F]{6,8}", needle) and needle.lower().startswith("0x") \
            else pids_matching_name(dat, needle)
        if not pids:
            print("\nno proto named %r" % needle)
            continue
        for pid in pids:
            name, description = proto_name(dat, pid)
            records = find_objects_by_pid(raw, pid)
            print("\npid 0x%08X (%s idx %d) %r — %d instance(s) on this map"
                  % (pid, TYPE_NAMES.get(pid >> 24, "?"), pid & 0xFFFFFF, name, len(records)))
            if description:
                print("  %s" % description)
            proto = read_proto(dat, pid >> 24, pid & 0xFFFFFF)
            if proto:
                print("  proto: sid=%d can_use=%s can_use_on=%s ext_flags=0x%08X"
                      % (proto["sid"], proto["can_use"], proto["can_use_on"], proto["extended_flags"]))
            for record in records:
                live = record["sid"] != -1 or record["script_index"] != -1
                print("  obj id=%d tile=%d elev=%d sid=%d script_index=%d -> %s"
                      % (record["id"], record["tile"], record["elevation"], record["sid"],
                         record["script_index"], "SCRIPTED" if live else "inert (no script)"))
                if not live and proto and proto["sid"] == -1 and proto["can_use"]:
                    print("     ►► offers USE in the action menu but has NO script, proto or instance:")
                    print("        vanilla walks over, plays the gesture, and nothing happens. Dressing.")


def cmd_proto(dat, args):
    ptype = {v: k for k, v in TYPE_NAMES.items()}.get(args.type)
    if ptype is None:
        sys.exit("type must be one of %s" % ", ".join(TYPE_NAMES.values()))
    if re.fullmatch(r"\d+", args.which):
        pids = [(ptype << 24) | int(args.which)]
    else:
        pids = pids_matching_name(dat, args.which, (ptype,))
    if not pids:
        sys.exit("no %s proto named %r" % (args.type, args.which))
    for pid in pids:
        name, description = proto_name(dat, pid)
        proto = read_proto(dat, ptype, pid & 0xFFFFFF)
        print("pid 0x%08X (%s idx %d) %r" % (pid, args.type, pid & 0xFFFFFF, name))
        if description:
            print("  %s" % description)
        if proto:
            print("  sid=%d type=%d flags=0x%08X ext=0x%08X can_use=%s can_use_on=%s"
                  % (proto["sid"], proto["type"], proto["flags"], proto["extended_flags"],
                     proto["can_use"], proto["can_use_on"]))


def script_line(dat, index):
    text = dat.text("scripts/scripts.lst")
    if text is None:
        return None
    lines = text.splitlines()
    # ►► scripts.lst is 1-BASED against the script index stored in a .map (same
    # convention as items.lst vs item PIDs). Verified against an obvious oracle:
    # desrt12 (a DESERT encounter tile) has map_script_index 313, and 1-based line
    # 313 is RndDsrt.int — "Random Encounter Desert Map Script". Reading it 0-based
    # returns RndCoast.int, which is off by exactly one and plausible enough to
    # believe by mistake.
    return lines[index - 1].strip() if 1 <= index <= len(lines) else None


def cmd_script(dat, args):
    line = script_line(dat, args.index)
    print("scripts.lst[%d] = %s" % (args.index, line or "?"))


def cmd_area(dat, args):
    text = dat.text("data/city.txt")
    if text is None:
        sys.exit("data/city.txt not in any archive")
    # wmAreaInit reads "Area %02d" upward and STOPS at the first block with no
    # townmap_art_idx — so areas are a dynamic, append-only list (realloc per
    # entry, wmMaxAreaNum++). That is what makes a CUSTOM worldmap point data-only.
    blocks = re.findall(r"(?ms)^\s*\[\s*Area\s+(\d+)\s*\](.*?)(?=^\s*\[|\Z)", text)
    if args.index is None:
        print("city.txt defines %d areas (Area 00..%s)" % (len(blocks), blocks[-1][0]))
        for number, body in blocks:
            name = re.search(r"(?m)^\s*area_name\s*=\s*(.*)$", body)
            print("  Area %s  %s" % (number, name.group(1).strip() if name else "?"))
        return
    for number, body in blocks:
        if int(number) == args.index:
            print("[Area %s]%s" % (number, body.rstrip()))
            return
    sys.exit("no [Area %02d] in city.txt" % args.index)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default="FO2", help="install directory holding the .dat archives (default FO2)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ls", help="list archived files matching a substring")
    p.add_argument("pattern", nargs="?", default="")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("cat", help="dump an archived file (text to stdout, --out for binary)")
    p.add_argument("path")
    p.add_argument("--out")
    p.set_defaults(func=cmd_cat)

    p = sub.add_parser("map", help="decode a map by INDEX (what the server logs) or internal name")
    p.add_argument("map")
    p.add_argument("--find", action="append", metavar="NAME|0xPID",
                   help="also report instances of this proto on the map, and whether they are scripted")
    p.set_defaults(func=cmd_map)

    p = sub.add_parser("proto", help="look up a proto by name or index, with its USE flags")
    p.add_argument("type", choices=sorted(TYPE_NAMES.values()))
    p.add_argument("which")
    p.set_defaults(func=cmd_proto)

    p = sub.add_parser("script", help="resolve a script index against scripts.lst")
    p.add_argument("index", type=int)
    p.set_defaults(func=cmd_script)

    p = sub.add_parser("area", help="worldmap areas from city.txt (list, or one block)")
    p.add_argument("index", nargs="?", type=int)
    p.set_defaults(func=cmd_area)

    args = parser.parse_args()
    args.func(Dat(args.root), args)


if __name__ == "__main__":
    main()
