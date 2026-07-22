#!/usr/bin/env python3
"""Event-stream file replayer / protocol-completeness validator (P5-C, slice 1).

The dedicated-server design (MP_PROTOCOL.md) streams authoritative world state as
a sequence of events. The NarratePresenter (F2_NARRATE) is already the first
serializer of that stream (event -> text line), so a narrate capture on disk IS a
serialized event log. This tool is the first CONSUMER of that log: it reads the
event stream back, reconstructs a world model by APPLYING each event, and checks
the reconstruction against the server's own authoritative state_dump.

That is the whole point of building the consumer as a file replayer FIRST (before
any socket): it proves the LOGICAL protocol is sufficient to reconstruct state,
decoupled from wire encoding and networking. When the socket lands (later P5-C
slice) the file source is swapped for a socket; the reconstruction logic is reused.

SCOPE OF THIS SLICE — POSITION / PRESENCE reconstruction.
The world is seeded FIRST from the join baseline `snapshot` (server_loop.cc
serverInstall): one line per object present at t=0, carrying pid/tile/elev/netid.
Objects that appear only later are still seeded lazily from the first event that
reveals a position: spawn (tile) / connect (tile,elev) / a move's fromTile+fromElev.
Subsequent move/connect/disconnect/destroy update the entry. We then compare the
reconstructed world position of every tracked object against the dump. The baseline
closes the old UNTRACKED gap: objects placed before the stream began (the initial
map loads before the narrate presenter installs) or loaded silently via _obj_load_obj
(no spawn -- the item<->world load finding) are now seeded at t=0, so UNTRACKED is 0
for a case that does not change maps (the single install-time baseline covers the
whole run; a mid-run map change would need a re-snapshot -- none of the gate cases do).
A reconstructed position the dump CONTRADICTS is a hard fail (exit 1) -- the stream
lost a state change.

KEYING: the world model and the dump index are keyed by NETID (the unique server id
minted per object under the server loop; a full deterministic re-walk assigns them on
map load -- MP_PROTOCOL.md §7 / cb54952). Netids are unique within a map, so the old
obj->id collision (~53%) no longer keys anything here; pid is still carried and used
to disambiguate a NO_SAVE transient (e.g. a gore critter) from dumped scenery so it
is classified not_in_dump rather than a false hole.

WHAT THIS IS / IS NOT. This is a reconstruction REGRESSION GATE + a semantic position
check, NOT a from-scratch completeness oracle. "REPLAY OK" alone is not proof of
completeness; the GATE (run_golden_replay.sh) adds the teeth by PINNING the per-case
PROFILE line (matched/moved/mismatched/not_in_dump). With the baseline seeding every
present object, a LOST event is caught more strongly than before:
  * a lost destroy/disconnect leaves an on-map object the dump no longer attests ->
    not_in_dump grows past the pin -> FAIL;
  * a lost move leaves a BASELINE object stuck at its t=0 tile != the dump's final
    tile -> mismatched grows -> FAIL (pre-baseline such an object was silently
    untracked and invisible; now it is always tracked).
`moved` (matches with real net displacement, seed_tile != final tile) is pinned as a
meaningful coverage floor, excluding trivial same-tile self-matches. Residual leniency:
an object whose position AND presence are wrong in a way that happens to still match
some dump entry with its (netid,pid) -- bounded to near-zero by the unique netid.

Usage: replay.py <stream_file> <state_dump_file>
The stream may be either a NARRATE text capture (F2_NARRATE) or the BINARY
netstream (F2_NETSTREAM, presenter_network.cc) -- dispatched on the magic. Both
feed the same reconstructor, so the validation below is encoding-agnostic.
Prints a PROFILE line the gate pins. Exit 0 if no reconstructed position
contradicts the dump; 1 on a contradiction / parse error.
"""

import re
import sys

# --- event stream (narrate) line grammar (presenter_narrate.cc) ---------------
RE_SPAWN = re.compile(r"^\[t=\d+\] spawn id=-?\d+ pid=0x([0-9A-Fa-f]+) .*tile=(-?\d+) netid=(-?\d+)$")
RE_MOVE = re.compile(r"^\[t=\d+\] move  id=-?\d+ .*?(-?\d+)->(-?\d+) \(elev (-?\d+)->(-?\d+)\) netid=(-?\d+)$")
RE_CONNECT = re.compile(r"^\[t=\d+\] connect id=-?\d+ pid=0x([0-9A-Fa-f]+) .*tile=(-?\d+) elev=(-?\d+) netid=(-?\d+)$")
RE_DISCONN = re.compile(r"^\[t=\d+\] disconn id=-?\d+ .*netid=(-?\d+)")
RE_DESTROY = re.compile(r"^\[t=\d+\] destroy id=-?\d+ .*netid=(-?\d+)")
# Join baseline (server_loop.cc serverInstall): every object present at t=0, before
# any event. Seeds the world so an object no event ever touches is still validated
# against the dump (and a lost event on a baseline object is caught as a mismatch /
# not_in_dump rather than silently untracked).
RE_SNAPSHOT = re.compile(r"^\[t=\d+\] snapshot id=-?\d+ pid=0x([0-9A-Fa-f]+) .*tile=(-?\d+) elev=(-?\d+) netid=(-?\d+)$")

# --- state dump grammar (state_dump.cc) ---------------------------------------
RE_DUMP_DUDE = re.compile(r"^dude id=-?\d+ tile=(-?\d+) elev=(-?\d+).*netid=(-?\d+)")
RE_DUMP_OBJ = re.compile(r"^obj id=-?\d+ pid=0x([0-9A-Fa-f]+) tile=(-?\d+) elev=(-?\d+).*netid=(-?\d+)")

DUDE_PID = 0x1000000  # gDude's pid (state_dump prints no pid on the dude line)

NOT_IN_WORLD = -1  # obj->tile == -1: in an inventory / limbo, not on a map tile.


class Obj:
    __slots__ = ("tile", "elev", "alive", "pid", "seed_tile")

    def __init__(self, tile, elev, pid=None):
        self.tile = tile
        self.elev = elev
        self.alive = True
        self.pid = pid  # from spawn/connect/destroy; None if only ever seen moving
        self.seed_tile = tile  # first-revealed tile; net displacement != this == a real move


def reconstruct(stream_path):
    """Apply the event stream; return {id: Obj} of the reconstructed world."""
    world = {}

    def seed(oid, tile, elev):
        # Track a not-yet-seen object; if already tracked, the caller updates it.
        if oid not in world:
            world[oid] = Obj(tile, elev)
        return world[oid]

    with open(stream_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = RE_SNAPSHOT.match(line)
            if m:
                pid, tile, elev, oid = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4))
                # Baseline ground truth (t=0). Precedes every event, so a later
                # spawn/move/connect on the same id just updates this entry.
                world[oid] = Obj(tile, elev, pid)
                continue
            m = RE_MOVE.match(line)
            if m:
                ft, tt, fe, te, oid = (int(x) for x in m.groups())
                o = seed(oid, ft, fe)  # first move's fromTile seeds position
                o.tile, o.elev, o.alive = tt, te, True
                continue
            m = RE_SPAWN.match(line)
            if m:
                pid, tile, oid = int(m.group(1), 16), int(m.group(2)), int(m.group(3))
                world[oid] = Obj(tile, 0, pid)  # spawn carries tile only; elev via later event
                continue
            m = RE_CONNECT.match(line)
            if m:
                pid, tile, elev, oid = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4))
                o = seed(oid, tile, elev)
                o.tile, o.elev, o.alive, o.pid = tile, elev, True, pid
                continue
            m = RE_DISCONN.match(line)
            if m:
                oid = int(m.group(1))
                o = world.get(oid)
                if o is not None:
                    o.tile = NOT_IN_WORLD  # left the world into an inventory/limbo
                continue
            m = RE_DESTROY.match(line)
            if m:
                oid = int(m.group(1))
                o = world.get(oid)
                if o is not None:
                    o.alive = False
                continue
    return world


# --- binary netstream front-end (presenter_network.cc, F2_NETSTREAM) ----------
# The SECOND serialization of the same logical event stream: NarratePresenter emits
# text, NetworkPresenter emits the real binary wire (MP_PROTOCOL.md §2). Both feed
# the SAME reconstructor + validator below -- only the front-end differs, which is
# the whole point of keeping reconstruction encoding-agnostic.
#
# Format (little-endian, packed):
#   stream: b"F2NS" | u16 version | i32 sessionId   (sessionId from version 2; 0 in a file log)
#   frame:  u32 seq | u32 simTs | u32 payloadLen | u16 eventCount | u32 entryBase (v4) | payload
#   event:  u8 type | u8 flags | u16 len | payload[len]
# Every event is length-prefixed, so unknown types are skipped whole. We only decode
# the 6 that carry position/presence; the rest (deltas, combat, presentation cues)
# are structurally skipped exactly as a forward-compatible client would.
NETSTREAM_MAGIC = b"F2NS"

E_SPAWN, E_MOVE, E_DESTROY, E_CONNECT, E_DISCONNECT = 1, 2, 3, 4, 5
E_SNAPSHOT_OBJECT, E_SNAPSHOT_BEGIN, E_SNAPSHOT_END = 8, 9, 10


class _Reader:
    """Bounds-checked little-endian cursor. Raises on truncation."""

    def __init__(self, buf):
        self.buf, self.pos = buf, 0

    def take(self, n):
        if self.pos + n > len(self.buf):
            raise ValueError(f"netstream truncated at offset {self.pos} (wanted {n})")
        out = self.buf[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return int.from_bytes(self.take(2), "little", signed=False)

    def u32(self):
        return int.from_bytes(self.take(4), "little", signed=False)

    def i32(self):
        return int.from_bytes(self.take(4), "little", signed=True)


def reconstruct_binary(stream_path):
    """Apply the binary netstream; return {netid: Obj} -- same shape as reconstruct()."""
    with open(stream_path, "rb") as f:
        data = f.read()

    r = _Reader(data)
    if r.take(4) != NETSTREAM_MAGIC:
        raise ValueError("not a netstream (bad magic)")
    version = r.u16()
    if version not in (1, 2, 3, 4):
        raise ValueError(f"unsupported netstream version {version}")
    # v2 appends an i32 sessionId: over a socket it is the receiving client's own
    # id (the one per-client field in the protocol); in a FILE log it is 0. Either
    # way it is 4 bytes to step over before the first frame.
    # v3 only widens the OBJECT_DELTA_INVENTORY per-item payload (adds ammoQuantity +
    # ammoTypePid); this reconstructor skips OBJECT_DELTA whole via its event length,
    # so the layout change is transparent here — no per-item parse to update.
    if version >= 2:
        r.take(4)

    world = {}

    def seed(oid, tile, elev):
        if oid not in world:
            world[oid] = Obj(tile, elev)
        return world[oid]

    expected_seq = 0
    while r.pos < len(data):
        seq = r.u32()
        r.u32()  # simTs -- pacing only; state reconstruction is time-independent
        payload_len = r.u32()
        event_count = r.u16()
        if version >= 4:
            r.u32()  # entryBase -- total-order id of the frame's first event; pacing/
                     # ack only, state reconstruction does not key on it (§8.1)
        payload = r.take(payload_len)

        # Gap detection (MP_PROTOCOL.md §1): seq is per-FRAME and dense over frames
        # actually emitted, so any jump means a lost frame => the consumer would
        # request a snapshot + resync. In a FILE replay a gap is a hard bug.
        if seq != expected_seq:
            raise ValueError(f"netstream frame gap: expected seq {expected_seq}, got {seq}")
        expected_seq = seq + 1

        er = _Reader(payload)
        for _ in range(event_count):
            etype = er.u8()
            er.u8()  # flags (STATE vs PRESENTATION) -- not needed to reconstruct
            elen = er.u16()
            body = _Reader(er.take(elen))

            if etype == E_SNAPSHOT_BEGIN:
                # "Drop your world; authoritative full state follows." A baseline
                # REPLACES, it does not merge -- and after a map change it MUST, because
                # the transition recycled every netid (objectAssignAllNetIds resets the
                # counter to 1), so a surviving old-map entry would collide with a new
                # object under the same netid. Merging instead of replacing leaves ~99
                # stale phantoms on an arvillag->denbus1 crossing (measured).
                world.clear()
            elif etype == E_SNAPSHOT_OBJECT:
                oid, pid, tile, elev = body.i32(), body.i32(), body.i32(), body.i32()
                world[oid] = Obj(tile, elev, pid)
            elif etype == E_MOVE:
                oid, ft, tt, fe, te = body.i32(), body.i32(), body.i32(), body.i32(), body.i32()
                o = seed(oid, ft, fe)
                o.tile, o.elev, o.alive = tt, te, True
            elif etype == E_SPAWN:
                oid, pid, tile, elev = body.i32(), body.i32(), body.i32(), body.i32()
                world[oid] = Obj(tile, elev, pid)
            elif etype == E_CONNECT:
                oid, pid, tile, elev = body.i32(), body.i32(), body.i32(), body.i32()
                o = seed(oid, tile, elev)
                o.tile, o.elev, o.alive, o.pid = tile, elev, True, pid
            elif etype == E_DISCONNECT:
                oid = body.i32()
                o = world.get(oid)
                if o is not None:
                    o.tile = NOT_IN_WORLD
            elif etype == E_DESTROY:
                oid = body.i32()
                o = world.get(oid)
                if o is not None:
                    o.alive = False
            # else: unknown/irrelevant type -- skipped whole via elen.

        if er.pos != len(payload):
            raise ValueError(f"frame seq {seq}: {len(payload) - er.pos} trailing bytes")
    return world


def load_stream(stream_path):
    """Dispatch on the magic: binary netstream or narrate text."""
    with open(stream_path, "rb") as f:
        if f.read(4) == NETSTREAM_MAGIC:
            return reconstruct_binary(stream_path)
    return reconstruct(stream_path)


def parse_dump(dump_path):
    """Return (positions, lines): {id: set((tile,elev))} and total obj-line count."""
    positions, lines = {}, 0
    with open(dump_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_DUMP_DUDE.match(line)
            if m:
                tile, elev, oid = (int(x) for x in m.groups())
                pid = DUDE_PID
            else:
                m = RE_DUMP_OBJ.match(line)
                if not m:
                    continue
                pid, tile, elev, oid = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), int(m.group(4))
            positions.setdefault(oid, set()).add((pid, tile, elev))
            lines += 1
    return positions, lines


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    world = load_stream(argv[1])
    positions, dump_lines = parse_dump(argv[2])

    # An object the stream can validate = one it tracked that ended up ON A MAP
    # TILE and alive (items that went to inventory / destroyed objects are not
    # world objects in the dump, by design).
    tracked = {
        oid: (o.tile, o.elev)
        for oid, o in world.items()
        if o.alive and o.tile != NOT_IN_WORLD
    }

    # Validate by (id, pid) set membership: a reconstructed object's position must
    # equal one of the dump entries carrying its id AND (when the stream revealed a
    # pid via spawn/connect) its pid. pid disambiguates the heavy id collision: a
    # NO_SAVE transient (e.g. a gore critter) that merely shares an id with dumped
    # scenery has no dump entry with its pid -> classified not_in_dump, not a hole.
    # Objects seen only moving carry no pid -> fall back to id-set membership (they
    # are the pre-placed savable movers like the dude, which the dump does list).
    matches, mismatches, not_in_dump = [], [], []
    moved = 0  # matched objects with a real net displacement (see Finding 5 below)
    for oid in sorted(tracked):
        pos = tracked[oid]
        pid = world[oid].pid
        entries = positions.get(oid)  # set of (pid, tile, elev)
        if not entries:
            not_in_dump.append(oid)  # id absent entirely (NO_SAVE mover)
            continue
        if pid is not None:
            cand = {(t, e) for (pp, t, e) in entries if pp == pid}
            if not cand:
                not_in_dump.append(oid)  # this object (by pid) is not dumped
                continue
        else:
            cand = {(t, e) for (pp, t, e) in entries}
        if pos in cand:
            matches.append(oid)
            if world[oid].tile != world[oid].seed_tile:
                moved += 1  # a MEANINGFUL match: the stream reconstructed a real move
        else:
            mismatches.append((oid, pos, sorted(cand)))

    untracked = sorted(oid for oid in positions if oid not in world)

    print(f"dump object lines:            {dump_lines}")
    print(f"dump distinct ids:            {len(positions)}")
    print(f"  => id-collision rate:       {dump_lines - len(positions)}/{dump_lines}"
          f"  (P5-C: unique server id is MANDATORY)")
    print(f"reconstructed on-map objects: {len(tracked)}")
    print(f"validated position:           {len(matches)} matched ({moved} with real displacement),"
          f" {len(mismatches)} mismatched")
    print(f"not-in-dump (NO_SAVE / lost?):{len(not_in_dump)} {not_in_dump if not_in_dump else ''}")
    print(f"UNTRACKED (not in baseline):  {len(untracked)} (in dump but no snapshot/event -> 0 once the baseline covers the map)")
    if mismatches:
        print(f"POSITION MISMATCH ({len(mismatches)}) -- a real protocol hole:")
        for oid, got, want in mismatches:
            print(f"  id={oid} reconstructed tile/elev={got} not in dump set {want}")
    # Canonical profile line: the gate pins this per case so ANY reconstruction
    # drift trips it. A lost destroy/disconnect grows not_in_dump (an on-map object
    # the dump no longer attests); a lost move flips matched->mismatched or drops
    # `moved`. See the gate + the docstring on why this is a REGRESSION lock, not a
    # from-scratch completeness oracle (bounded by the missing unique id / snapshot).
    print(f"PROFILE matched={len(matches)} moved={moved} mismatched={len(mismatches)}"
          f" not_in_dump={len(not_in_dump)}")

    # replay.py's own exit code covers the SEMANTIC check (a reconstructed position
    # the dump contradicts). The count-regression check lives in the gate, which
    # compares the PROFILE line against a pinned expected value per case.
    ok = not mismatches and matches
    print("REPLAY OK" if ok else "REPLAY FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
