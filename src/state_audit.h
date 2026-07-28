#ifndef FALLOUT_STATE_AUDIT_H_
#define FALLOUT_STATE_AUDIT_H_

#include <stdio.h>

#include <vector>

namespace fallout {

struct Object;

// ─── LIVE MIRROR DIVERGENCE AUDIT ───────────────────────────────────────────
//
// ►►►► WHY THIS EXISTS. The replication model is a shadow diff over a HAND-PICKED
// list of fields (object_delta.cc), and it is complete only for the fields on that
// list. Every bug of one whole class has been the same sentence — "that field was
// never streamed": the art FRAME wasn't, per-object LIGHT wasn't, GVARs weren't,
// and an object's PID wasn't (dig a grave and the viewer rendered the opened grave
// while still resolving the closed grave's proto). Each was found by a player
// clicking something and getting nothing, because nothing else could see it:
//
//   ►► the gates reconstruct TWO fields. tools/replay.py's reconstructed object is
//   `Obj(tile, elev)` — position and nothing else. 69 green gates could not see a
//   single one of those bugs, and never will.
//
// So this is the missing instrument, and it has one rule that makes it worth
// building: **it is derived from the Object struct, NOT from the delta's field
// list.** An oracle that audits exactly the fields the tap already streams is blind
// in precisely the places the tap is, which is the only place bugs live. When you
// add a field to Object, add it HERE; the audit will then report it as divergent
// until the tap learns to stream it, which is the correct order of events.
//
// Two consumers, one record:
//   * LIVE — the server emits an audit on demand (`audit` control verb, or every
//     F2_AUDIT_SECS) and each viewer diffs it against its own mirror, printing one
//     line per divergent field. "Nothing happened when I clicked" becomes a diff.
//   * HEADLESS — tools/replay.py reconstructs from the same stream and compares,
//     so a gate can FAIL on divergence with no client process at all.

// One object's full syncable state. Flat, POD, and byte-order-explicit on the wire
// (see presenter_network) so both consumers read the same thing.
struct StateAuditRecord {
    int netId;
    int pid;
    int tile;
    int elevation;
    int fid;
    int frame;
    int rotation;
    unsigned int flags;
    int lightDistance;
    int lightIntensity;
    // Critter-only; 0 for everything else (and never compared for non-critters).
    int hp;
    int radiation;
    int poison;
    int ap;
    int combatResults;
    // Top-level inventory: the count, plus a fingerprint that folds each item's
    // netId, pid, quantity and flags. netId is deliberately IN it — the delta's own
    // hash omits identity, so a mirror holding the right items under the wrong ids
    // (the armed-charge and thrown-rock class) hashes equal there and divergent here.
    int inventoryCount;
    unsigned int inventoryHash;
};

// Fill `out` from `obj`. False when the object is not syncable (no netId) — the
// caller skips it.
bool stateAuditCapture(Object* obj, StateAuditRecord* out);

// Every syncable object in the current world, ordered by netId.
void stateAuditWalk(std::vector<StateAuditRecord>* out);

// Diff `authoritative` (the server's, off the wire) against this process's OWN
// world. Writes one human-readable line per divergence to `report` and returns the
// number of divergences — 0 means the mirror is exact. Reports three kinds:
// a differing field, an object the server has that we do not (MISSING), and one we
// have that the server does not (EXTRA).
int stateAuditCompare(const std::vector<StateAuditRecord>& authoritative, FILE* report);

// SERVER: snapshot the world and ship it to every viewer (chunked). Call from the
// `audit` control verb or a periodic trigger. Cheap enough to run on demand; not
// something to put on the per-beat path.
void stateAuditEmit();

// Names of the fields, for reports/tools. Indices match the struct order.
const char* stateAuditFieldName(int index);

} // namespace fallout

#endif /* FALLOUT_STATE_AUDIT_H_ */
