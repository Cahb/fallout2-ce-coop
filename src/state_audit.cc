#include "state_audit.h"

#include <algorithm>
#include <unordered_map>

#include "object.h"
#include "presenter.h"
#include "proto_types.h"

namespace fallout {

// Same avalanche the delta tracker uses, for the same reason: a per-item hash must
// be non-linear in its inputs before being summed, or a sum-preserving mutation
// (k units moved between two stacks in one beat) cancels to the same fingerprint.
static unsigned int auditHashMix(unsigned int h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Order-independent fingerprint of an owner's top-level inventory, folding each
// item's IDENTITY as well as its kind. See the header for why netId is in here.
static unsigned int auditInventoryHash(Object* obj)
{
    const Inventory* inventory = &(obj->data.inventory);
    unsigned int hash = (unsigned int)inventory->length;
    for (int i = 0; i < inventory->length; i++) {
        const InventoryItem* entry = &(inventory->items[i]);
        const Object* item = entry->item;
        if (item == nullptr) {
            continue;
        }
        unsigned int itemHash = (unsigned int)item->netId * 2654435761u
            + (unsigned int)item->pid * 2246822519u
            + (unsigned int)entry->quantity * 3266489917u
            + item->flags * 40503u;
        hash += auditHashMix(itemHash);
    }
    return hash;
}

static bool auditIsCritter(Object* obj)
{
    return PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER;
}

bool stateAuditCapture(Object* obj, StateAuditRecord* out)
{
    if (obj == nullptr || out == nullptr || obj->netId == 0) {
        return false;
    }
    *out = StateAuditRecord {};
    out->netId = obj->netId;
    out->pid = obj->pid;
    out->tile = obj->tile;
    out->elevation = obj->elevation;
    out->fid = obj->fid;
    out->frame = obj->frame;
    out->rotation = obj->rotation;
    out->flags = obj->flags;
    out->lightDistance = obj->lightDistance;
    out->lightIntensity = obj->lightIntensity;
    if (auditIsCritter(obj)) {
        out->hp = obj->data.critter.hp;
        out->radiation = obj->data.critter.radiation;
        out->poison = obj->data.critter.poison;
        out->ap = obj->data.critter.combat.ap;
        out->combatResults = obj->data.critter.combat.results;
    }
    out->inventoryCount = obj->data.inventory.length;
    out->inventoryHash = auditInventoryHash(obj);
    return true;
}

void stateAuditWalk(std::vector<StateAuditRecord>* out)
{
    if (out == nullptr) {
        return;
    }
    out->clear();
    StateAuditRecord record;
    for (Object* obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        if (stateAuditCapture(obj, &record)) {
            out->push_back(record);
        }
    }
    // The dude can be absent from the bucket walk (same carve-out the netId seeding
    // makes), and it is the single object a divergence matters most on.
    if (gDude != nullptr && stateAuditCapture(gDude, &record)) {
        bool seen = false;
        for (const StateAuditRecord& r : *out) {
            if (r.netId == record.netId) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out->push_back(record);
        }
    }
    std::sort(out->begin(), out->end(),
        [](const StateAuditRecord& a, const StateAuditRecord& b) { return a.netId < b.netId; });
}

static const char* kFieldNames[] = {
    "netId", "pid", "tile", "elevation", "fid", "frame", "rotation", "flags",
    "lightDistance", "lightIntensity", "hp", "radiation", "poison", "ap",
    "combatResults", "inventoryCount", "inventoryHash",
};

const char* stateAuditFieldName(int index)
{
    if (index < 0 || index >= (int)(sizeof(kFieldNames) / sizeof(kFieldNames[0]))) {
        return "?";
    }
    return kFieldNames[index];
}

// One divergence line. Values are printed in decimal AND hex because half these
// fields (fid, flags) are only readable in hex and the other half only in decimal.
static void auditReportField(FILE* report, int netId, int pid, const char* field,
    long long theirs, long long ours)
{
    fprintf(report, "[audit] net=%d pid=%d %s: server=%lld (0x%llX) mirror=%lld (0x%llX)\n",
        netId, pid, field, theirs, (unsigned long long)theirs, ours, (unsigned long long)ours);
}

int stateAuditCompare(const std::vector<StateAuditRecord>& authoritative, FILE* report)
{
    if (report == nullptr) {
        return -1;
    }
    std::vector<StateAuditRecord> mine;
    stateAuditWalk(&mine);

    std::unordered_map<int, const StateAuditRecord*> mineByNetId;
    mineByNetId.reserve(mine.size() * 2);
    for (const StateAuditRecord& r : mine) {
        mineByNetId[r.netId] = &r;
    }

    int divergences = 0;
    std::unordered_map<int, bool> matched;
    matched.reserve(authoritative.size() * 2);

    for (const StateAuditRecord& theirs : authoritative) {
        auto it = mineByNetId.find(theirs.netId);
        if (it == mineByNetId.end()) {
            // ►► The server has an object this process cannot see AT ALL. Worse than a
            // wrong field: nothing about it can ever be right, and no delta will fix it
            // (a delta for an unknown netId is dropped).
            fprintf(report, "[audit] net=%d pid=%d MISSING — the server has it, the mirror does not\n",
                theirs.netId, theirs.pid);
            divergences += 1;
            continue;
        }
        matched[theirs.netId] = true;
        const StateAuditRecord& ours = *it->second;
        const bool isCritter = PID_TYPE(theirs.pid) == OBJ_TYPE_CRITTER;

#define AUDIT_FIELD(name)                                                        \
    if (theirs.name != ours.name) {                                              \
        auditReportField(report, theirs.netId, theirs.pid, #name,                \
            (long long)theirs.name, (long long)ours.name);                       \
        divergences += 1;                                                        \
    }
        AUDIT_FIELD(pid)
        AUDIT_FIELD(tile)
        AUDIT_FIELD(elevation)
        AUDIT_FIELD(fid)
        AUDIT_FIELD(frame)
        AUDIT_FIELD(rotation)
        AUDIT_FIELD(flags)
        AUDIT_FIELD(lightDistance)
        AUDIT_FIELD(lightIntensity)
        AUDIT_FIELD(inventoryCount)
        AUDIT_FIELD(inventoryHash)
        if (isCritter) {
            AUDIT_FIELD(hp)
            AUDIT_FIELD(radiation)
            AUDIT_FIELD(poison)
            AUDIT_FIELD(ap)
            AUDIT_FIELD(combatResults)
        }
#undef AUDIT_FIELD
    }

    for (const StateAuditRecord& ours : mine) {
        if (matched.find(ours.netId) == matched.end()) {
            // A ghost: something this process kept that the server has retired. This is
            // the shape of the phantom thrown rock and the un-reconciled steal victim —
            // a thing the player can see and click that no longer exists.
            fprintf(report, "[audit] net=%d pid=%d EXTRA — the mirror has it, the server does not\n",
                ours.netId, ours.pid);
            divergences += 1;
        }
    }

    fprintf(report, "[audit] %d server objects, %d mirrored, %d divergences\n",
        (int)authoritative.size(), (int)mine.size(), divergences);
    return divergences;
}

// Server side: snapshot every syncable object and ship it out in chunks. Chunked
// because one event's payload is u16-length-framed and a busy map is ~2000 objects;
// the receiver accumulates until the chunk marked final, then compares.
void stateAuditEmit()
{
    std::vector<StateAuditRecord> records;
    stateAuditWalk(&records);
    const int kPerChunk = 400; // 400 * 68 bytes, comfortably inside the 64 KB frame field
    int total = (int)records.size();
    if (total == 0) {
        presenter()->stateAuditChunk(nullptr, 0, true);
        return;
    }
    for (int offset = 0; offset < total; offset += kPerChunk) {
        int count = std::min(kPerChunk, total - offset);
        bool isFinal = (offset + count) >= total;
        presenter()->stateAuditChunk(&records[offset], count, isFinal);
    }
    fprintf(stderr, "f2_server: state audit sent (%d objects)\n", total);
}

} // namespace fallout
