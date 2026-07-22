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
int _getPartyMemberCount();
int _partyMemberPrepItemSaveAll();
int partyMemberGetBestSkill(Object* object);
Object* partyMemberGetBestInSkill(int skill);
int partyGetBestSkillValue(int skill);
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
