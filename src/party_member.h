#ifndef PARTY_MEMBER_H
#define PARTY_MEMBER_H

#include <vector>

#include "db.h"
#include "obj_types.h"
#include "scripts.h"

namespace fallout {

extern int gPartyMemberDescriptionsLength;
extern int* gPartyMemberPids;

// Ledger H-42 (moved from pipboy.cc with the rest-intent decoder): the
// rest-duration menu options. Values are the alarm-clock menu order (and the
// [kind] vocabulary of the rest sim); names kept verbatim.
typedef enum PipboyRestDuration {
    PIPBOY_REST_DURATION_TEN_MINUTES,
    PIPBOY_REST_DURATION_THIRTY_MINUTES,
    PIPBOY_REST_DURATION_ONE_HOUR,
    PIPBOY_REST_DURATION_TWO_HOURS,
    PIPBOY_REST_DURATION_THREE_HOURS,
    PIPBOY_REST_DURATION_FOUR_HOURS,
    PIPBOY_REST_DURATION_FIVE_HOURS,
    PIPBOY_REST_DURATION_SIX_HOURS,
    PIPBOY_REST_DURATION_UNTIL_MORNING,
    PIPBOY_REST_DURATION_UNTIL_NOON,
    PIPBOY_REST_DURATION_UNTIL_EVENING,
    PIPBOY_REST_DURATION_UNTIL_MIDNIGHT,
    PIPBOY_REST_DURATION_UNTIL_HEALED,
    PIPBOY_REST_DURATION_UNTIL_PARTY_HEALED,
    PIPBOY_REST_DURATION_COUNT,
    PIPBOY_REST_DURATION_COUNT_WITHOUT_PARTY = PIPBOY_REST_DURATION_COUNT - 1,
} PipboyRestDuration;

// Ledger H-40: outcome of one rest-sim clock step.
typedef enum RestSimTickResult {
    REST_SIM_TICK_ADVANCED,
    REST_SIM_TICK_EVENT,
    REST_SIM_TICK_QUIT,
} RestSimTickResult;

int partyMembersInit();
void partyMembersReset();
void partyMembersExit();
int partyMemberAdd(Object* object);
int partyMemberRemove(Object* object);
int _partyMemberPrepSave();
int _partyMemberUnPrepSave();
int partyMembersSave(File* stream);
int _partyMemberPrepLoad();
int _partyMemberRecoverLoad();
int partyMembersLoad(File* stream);
void _partyMemberClear();
int _partyMemberSyncPosition();
// Co-op: the player this member follows (its recruiter when reachable, else the
// nearest live player on its elevation, else gDude). gDude in single-player.
Object* partyMemberLeader(Object* member);
int _partyMemberRestingHeal(int a1);
void restHealReset();
bool restHealCheck(int minutes);
bool restHealApply();
void restSimPacing(int hours, int minutes, double* minutesPhaseFrames, double* hoursPhaseFrames);
int restSimMinutesTick(unsigned int startTime, int frame, double frameCount, int minutes);
void restSimMinutesFinish(unsigned int startTime, int minutes);
int restSimHoursTick(unsigned int startTime, int frame, double frameCount, int hours);
void restSimHoursFinish(unsigned int startTime, int hours);
bool restSimOverdueEvents();
int restUntilHealedDuration();
void restUntilHourDuration(int* hours, int* minutes, int wakeUpHour);
void restOptionDecode(int option, int* hours, int* minutes, int* kind);
Object* partyMemberFindByPid(int a1);
bool _isPotentialPartyMember(Object* object);
bool objectIsPartyMember(Object* object);
// ►► COMPANIONS ONLY, and it must stay that way: scripts REFUSE ENTRY on this number
// ("leave some of your friends outside"), so counting player actors here walls a co-op
// party out of Vault City, the Sierra doors and every escort gate. See the body.
int _getPartyMemberCount();
// How many of us are actually standing here — companions plus every online living
// player. For sizing and offering things, NEVER for gating them.
int partyGroupSize();
bool partyHasRecruitedMembers();
int _partyMemberPrepItemSaveAll();
int partyMemberGetBestSkill(Object* object);
// ►► THE PARTY-SKILL READERS, WIDENED FOR CO-OP (party_member.cc has the why).
//
// Extra player actors are deliberately NOT in gPartyMembers — that list is saved,
// levelled, position-synced and garbage-collected as COMPANIONS, and one of its
// maintenance passes destroys objects. So "how good is the party at X" widens its
// CANDIDATE SET here instead of the registry growing.
//
// Two scopes, because the acts differ:
//   * `actor`-taking form = a SOLO act (a trade, a skill use). Companions plus THAT
//     player. Another player standing nearby does not haggle for you (owner ruling
//     2026-07-25: "BARTER is solo trade only; player barters -> his skill is taken
//     into consideration").
//   * no-actor form = a GROUP act (travelling, avoiding an encounter). Companions
//     plus every ONLINE, LIVING player actor.
// With one player both collapse to vanilla: slot 0 IS gPartyMembers[0].
Object* partyMemberGetBestInSkill(int skill, Object* actor = nullptr);
int partyGetBestSkillValue(int skill);
int partyGetBestSkillValueFor(int skill, Object* actor);
// The player actor holding the highest value of `skill` among online living players,
// or nullptr. For crediting a group roll to whoever actually carried it.
Object* partyGetBestSkillPlayerActor(int skill);
void _partyMemberSaveProtos();
bool partyMemberSupportsDisposition(Object* object, int disposition);
bool partyMemberSupportsAreaAttackMode(Object* object, int areaAttackMode);
bool partyMemberSupportsRunAwayMode(Object* object, int runAwayMode);
bool partyMemberSupportsBestWeapon(Object* object, int bestWeapon);
bool partyMemberSupportsDistance(Object* object, int distanceMode);
bool partyMemberSupportsAttackWho(Object* object, int attackWho);
bool partyMemberSupportsChemUse(Object* object, int chemUse);
int _partyMemberIncLevels();
bool partyIsAnyoneCanBeHealedByRest();
int partyGetMaxWoundToHealByRest();
std::vector<Object*> get_all_party_members_objects(bool include_hidden);

} // namespace fallout

#endif /* PARTY_MEMBER_H */
