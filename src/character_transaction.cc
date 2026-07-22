#include "character_transaction.h"

#include <string.h>

#include "critter.h"
#include "object.h"
#include "proto.h"
#include "stat.h"

namespace fallout {

// Verbatim the dude-proto/hit-points/name captures from the head of
// characterEditorSavePlayer.
void characterSnapshotTake(CharacterSnapshot* snapshot)
{
    Proto* proto;
    protoGetProto(gDude->pid, &proto);
    critterProtoDataCopy(&snapshot->dudeData, &(proto->critter.data));

    snapshot->hitPoints = critterGetHitPoints(gDude);

    strncpy(snapshot->name, critterGetName(gDude), 32);
}

// Verbatim the unspent-skill-points capture from characterEditorSavePlayer.
void characterSnapshotTakeSkillPoints(CharacterSnapshot* snapshot)
{
    snapshot->unspentSkillPoints = pcGetStat(PC_STAT_UNSPENT_SKILL_POINTS);
}

// Verbatim the dude-proto restore + name restore from
// characterEditorRestorePlayer. critterProtoDataCopy only memcpys from its
// source, so the const_cast preserves the const-snapshot contract.
void characterSnapshotRestore(const CharacterSnapshot* snapshot)
{
    Proto* proto;
    protoGetProto(gDude->pid, &proto);
    critterProtoDataCopy(&(proto->critter.data), const_cast<CritterProtoData*>(&snapshot->dudeData));

    dudeSetName(snapshot->name);
}

// Verbatim the unspent-skill-points restore from
// characterEditorRestorePlayer.
void characterSnapshotRestoreSkillPoints(const CharacterSnapshot* snapshot)
{
    pcSetStat(PC_STAT_UNSPENT_SKILL_POINTS, snapshot->unspentSkillPoints);
}

// Verbatim the derived-stat recompute + hit-points restore from the tail of
// characterEditorRestorePlayer.
void characterSnapshotRestoreHitPoints(const CharacterSnapshot* snapshot)
{
    critterUpdateDerivedStats(gDude);

    int cur_hp = critterGetHitPoints(gDude);
    critterAdjustHitPoints(gDude, snapshot->hitPoints - cur_hp);
}

} // namespace fallout
