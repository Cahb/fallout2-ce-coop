#include "combat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"
#include "animation.h"
#include "art.h"
#include "color.h"
#include "combat_ai.h"
#include "combat_intent.h"
#include "combat_ui.h"
#include "critter.h"
#include "db.h"
#include "debug.h"
#include "display_monitor.h"
#include "draw.h"
#include "client_net.h" // clientViewerActive — combat-attack commit fork (§3.b)
#include "elevator.h"
#include "game.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "loadsave.h"
#include "map.h"
#include "memory.h"
#include "message.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "pres_record.h"
#include "presenter.h"
#include "proto.h"
#include "queue.h"
#include "random.h"
#include "scripts.h"
#include "server_loop.h"
#include "server_players.h"
#include "settings.h"
#include "sim_clock.h"
#include "sfall_config.h"
#include "sfall_global_scripts.h"
#include "skill.h"
#include "stat.h"
#include "svga.h"
#include "text_font.h"
#include "tile.h"
#include "trait.h"
#include "window_manager.h"

namespace fallout {

typedef enum DamageCalculationType {
    DAMAGE_CALCULATION_TYPE_VANILLA = 0,
    DAMAGE_CALCULATION_TYPE_GLOVZ = 1,
    DAMAGE_CALCULATION_TYPE_GLOVZ_WITH_DAMAGE_MULTIPLIER_TWEAK = 2,
    DAMAGE_CALCULATION_TYPE_YAAM = 5,
} DamageCalculationType;

typedef struct CombatAiInfo {
    Object* friendlyDead;
    Object* lastTarget;
    Object* lastItem;
    int lastMove;
} CombatAiInfo;

typedef struct UnarmedHitDescription {
    int requiredLevel;
    int requiredSkill;
    int requiredStats[PRIMARY_STAT_COUNT];
    int minDamage;
    int maxDamage;
    int bonusDamage;
    int bonusCriticalChance;
    int actionPointCost;
    bool isPenetrate;
    bool isSecondary;
} UnarmedHitDescription;

typedef struct DamageCalculationContext {
    Attack* attack;
    int* damagePtr;
    int ammoQuantity;
    int damageResistance;
    int damageThreshold;
    int damageBonus;
    int bonusDamageMultiplier;
    int combatDifficultyDamageModifier;
} DamageCalculationContext;

static bool _combat_safety_invalidate_weapon_func(Object* attacker, Object* weapon, int hitMode, Object* defender, int* safeDistancePtr, Object* attackerFriend);
static void _combatInitAIInfoList();
static int aiInfoCopy(int srcIndex, int destIndex);
static int _combatAIInfoSetLastMove(Object* object, int move);
static void _combat_begin(Object* attacker);
static void _combat_begin_extra(Object* attacker);
void _combat_update_critters_in_los(bool a1);
static void _combat_over();
static void _combat_add_noncoms();
static int _compare_faster(const void* critter1Ptr, const void* critter2Ptr);
static void _combat_sequence_init(Object* attacker, Object* defender);
static void _combat_sequence();
void combatAttemptEnd();
static void _combat_set_move_all();
static int _combat_turn(Object* a1, bool a2);
static bool _combat_should_end();
static bool _check_ranged_miss(Attack* attack);
static int _shoot_along_path(Attack* attack, int endTile, int rounds, int anim);
static int _compute_spray(Attack* attack, int accuracy, int* roundsHitMainTargetPtr, int* roundsSpentPtr, int anim);
static int attackComputeEnhancedKnockout(Attack* attack);
static int attackCompute(Attack* attack);
static int attackComputeCriticalHit(Attack* a1);
static int _attackFindInvalidFlags(Object* a1, Object* a2);
static int attackComputeCriticalFailure(Attack* attack);
static void _do_random_cripple(int* flagsPtr);
static int attackDetermineToHit(Object* attacker, int tile, Object* defender, int hitLocation, int hitMode, bool useDistance);
static void attackComputeDamage(Attack* attack, int ammoQuantity, int a3);
static void _check_for_death(Object* a1, int a2, int* a3);
static void _set_new_results(Object* a1, int a2);
static void _damage_object(Object* a1, int damage, bool animated, int a4, Object* a5);
static void _combat_apply_attack_results(bool animated);
static void _combat_standup(Object* a1);

static void criticalsInit();
static void criticalsReset();
static void criticalsExit();
static void burstModInit();
static int burstModComputeRounds(int totalRounds, int* centerRoundsPtr, int* leftRoundsPtr, int* rightRoundsPtr);
static void unarmedInit();
static void unarmedInitVanilla();
static void unarmedInitCustom();
static int unarmedGetHitModeInRange(int firstHitMode, int lastHitMode, bool isSecondary);
static void damageModInit();
static void damageModCalculateGlovz(DamageCalculationContext* context);
static int damageModGlovzDivRound(int dividend, int divisor);
static void damageModCalculateYaam(DamageCalculationContext* context);

// 0x51093C
int _combat_turn_running = 0;

// 0x510940
int _combatNumTurns = 0;

// 0x510944
unsigned int gCombatState = COMBAT_STATE_0x02;

// 0x510948
static CombatAiInfo* _aiInfoList = nullptr;

// 0x51094C
static CombatStartData* _gcsd = nullptr;

// 0x510950
static bool _combat_call_display = false;

// Accuracy modifiers for hit locations.
//
// 0x510954
static int hit_location_penalty_default[HIT_LOCATION_COUNT] = {
    -40,
    -30,
    -30,
    0,
    -20,
    -20,
    -60,
    -30,
    0,
};

static int hit_location_penalty[HIT_LOCATION_COUNT];

// Critical hit tables for every kill type.
//
// 0x510978
static CriticalHitDescription gCriticalHitTables[SFALL_KILL_TYPE_COUNT][HIT_LOCATION_COUNT][CRTICIAL_EFFECT_COUNT] = {
    // KILL_TYPE_MAN
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5002, 5003 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5002, 5003 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5004, 5003 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 5005, 5006 },
            { 6, DAM_DEAD, -1, 0, 0, 5007, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5008, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 5009, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 5010, 5011 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5012, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5012, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5013, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5008, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 5009, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 5014, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5015, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5015, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5013, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5016, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5017, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5019, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5019, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5020, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5021, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5023, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5025, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5025, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5026, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5023, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5025, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5025, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5026, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 4, DAM_BLIND, 5027, 5028 },
            { 4, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 5029, 5028 },
            { 6, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 5029, 5028 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5030, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5031, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5032, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5033, 5000 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5034, 5035 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5035, 5036 },
            { 3, DAM_KNOCKED_OUT, -1, 0, 0, 5036, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5035, 5036 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5037, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5016, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5017, 5000 },
            { 4, 0, -1, 0, 0, 5018, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5019, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5020, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5021, 5000 },
        },
    },
    // KILL_TYPE_WOMAN
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5101, 5100 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5102, 5103 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5102, 5103 },
            { 6, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5104, 5103 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 5105, 5106 },
            { 6, DAM_DEAD, -1, 0, 0, 5107, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5108, 5100 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 5109, 5100 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_LEFT, 5110, 5111 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_LEFT, 5110, 5111 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5112, 5100 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5113, 5100 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5108, 5100 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 5109, 5100 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_RIGHT, 5114, 5100 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_RIGHT, 5114, 5100 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5115, 5100 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5113, 5100 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5116, 5100 },
            { 3, DAM_BYPASS, -1, 0, 0, 5117, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5119, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5119, 5100 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5120, 5100 },
            { 6, DAM_DEAD, -1, 0, 0, 5121, 5100 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5123, 5100 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5123, 5124 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 5123, 5124 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5125, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5125, 5126 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5126, 5100 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5123, 5100 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5123, 5124 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 5123, 5124 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5125, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5125, 5126 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5126, 5100 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 4, DAM_BLIND, 5127, 5128 },
            { 4, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 5129, 5128 },
            { 6, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 5129, 5128 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5130, 5100 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5131, 5100 },
            { 8, DAM_DEAD, -1, 0, 0, 5132, 5100 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5133, 5100 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5133, 5134 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5134, 5135 },
            { 3, DAM_KNOCKED_OUT, -1, 0, 0, 5135, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5134, 5135 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5135, 5100 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5116, 5100 },
            { 3, DAM_BYPASS, -1, 0, 0, 5117, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5119, 5100 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5119, 5100 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5120, 5100 },
            { 6, DAM_DEAD, -1, 0, 0, 5121, 5100 },
        },
    },
    // KILL_TYPE_CHILD
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5200, 5201 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, -2, DAM_KNOCKED_OUT, 5202, 5203 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, -2, DAM_KNOCKED_OUT, 5202, 5203 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5203, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5203, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5204, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5205, 5000 },
            { 4, DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5206, 5207 },
            { 4, DAM_LOSE_TURN, STAT_ENDURANCE, -2, DAM_CRIP_ARM_LEFT, 5206, 5207 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5209, 5000 },
            { 4, DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5206, 5207 },
            { 4, DAM_LOSE_TURN, STAT_ENDURANCE, -2, DAM_CRIP_ARM_RIGHT, 5206, 5207 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5208, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5210, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5211, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5212, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5212, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5213, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5214, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5215, 5000 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, DAM_CRIP_ARM_RIGHT | DAM_BLIND | DAM_ON_FIRE | DAM_EXPLODE, 5000, 0 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, DAM_CRIP_ARM_RIGHT | DAM_BLIND | DAM_ON_FIRE | DAM_EXPLODE, 5000, 0 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5215, 5000 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, DAM_CRIP_ARM_RIGHT | DAM_BLIND | DAM_ON_FIRE | DAM_EXPLODE, 5000, 0 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, DAM_CRIP_ARM_RIGHT | DAM_BLIND | DAM_ON_FIRE | DAM_EXPLODE, 5000, 0 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5217, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 5, DAM_BLIND, 5218, 5219 },
            { 4, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 5220, 5221 },
            { 6, DAM_BYPASS, STAT_LUCK, -1, DAM_BLIND, 5220, 5221 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5222, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5223, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5224, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5225, 5000 },
            { 3, 0, -1, 0, 0, 5225, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5226, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5226, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5226, 5000 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5226, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5210, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5211, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5211, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5212, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5213, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5214, 5000 },
        },
    },
    // KILL_TYPE_SUPER_MUTANT
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5300, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, -1, DAM_KNOCKED_DOWN, 5301, 5302 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -4, DAM_KNOCKED_DOWN, 5301, 5302 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5302, 5303 },
            { 6, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5302, 5303 },
            { 6, DAM_DEAD, -1, 0, 0, 5304, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_LOSE_TURN, 5300, 5306 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -1, DAM_CRIP_ARM_LEFT, 5307, 5308 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 5307, 5308 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5308, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5308, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_LOSE_TURN, 5300, 5006 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -1, DAM_CRIP_ARM_RIGHT, 5307, 5309 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 5307, 5309 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5309, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5309, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5301, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5302, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5302, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5310, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5311, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5300, 5312 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 5312, 5313 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5313, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5314, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5315, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5300, 5312 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 5312, 5313 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 5313, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5314, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5315, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5300, 5000 },
            { 4, DAM_BYPASS, STAT_LUCK, 5, DAM_BLIND, 5316, 5317 },
            { 6, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 5316, 5317 },
            { 6, DAM_BYPASS | DAM_LOSE_TURN, STAT_LUCK, 0, DAM_BLIND, 5318, 5319 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5320, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5321, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, 0, STAT_LUCK, 0, DAM_BYPASS, 5300, 5017 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, -2, DAM_KNOCKED_DOWN, 5301, 5302 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5312, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5302, 5303 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5303, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5300, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5301, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5302, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5302, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5310, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5311, 5000 },
        },
    },
    // KILL_TYPE_GHOUL
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5400, 5003 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -1, DAM_KNOCKED_OUT, 5400, 5003 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -2, DAM_KNOCKED_OUT, 5004, 5005 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_STRENGTH, 0, 0, 5005, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5401, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5016, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_DROP | DAM_LOSE_TURN, 5001, 5402 },
            { 4, DAM_DROP | DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5402, 5012 },
            { 4, DAM_BYPASS | DAM_DROP | DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5403, 5404 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS | DAM_DROP, -1, 0, 0, 5404, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS | DAM_DROP, -1, 0, 0, 5404, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5016, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_DROP | DAM_LOSE_TURN, 5001, 5402 },
            { 4, DAM_DROP | DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5402, 5015 },
            { 4, DAM_BYPASS | DAM_DROP | DAM_LOSE_TURN, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5403, 5404 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS | DAM_DROP, -1, 0, 0, 5404, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS | DAM_DROP, -1, 0, 0, 5404, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5017, 5000 },
            { 3, 0, -1, 0, 0, 5018, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5004, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5003, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5007, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5023 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5024, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5024, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5026, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5023 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 5024, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5024, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5026, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 3, DAM_BLIND, 5001, 5405 },
            { 4, DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 5406, 5407 },
            { 6, DAM_BYPASS, STAT_LUCK, -3, DAM_BLIND, 5406, 5407 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5030, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5031, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5408, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_LUCK, 0, DAM_BYPASS, 5001, 5033 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5033, 5035 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5004, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5035, 5036 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5036, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5017, 5000 },
            { 3, 0, -1, 0, 0, 5018, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5004, 5000 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 5003, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5007, 5000 },
        },
    },
    // KILL_TYPE_BRAHMIN
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 5, 0, STAT_ENDURANCE, 2, DAM_KNOCKED_DOWN, 5016, 5500 },
            { 5, 0, STAT_ENDURANCE, -1, DAM_KNOCKED_DOWN, 5016, 5500 },
            { 6, DAM_KNOCKED_OUT, STAT_STRENGTH, 0, 0, 5501, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5502, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5016, 5503 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5016, 5503 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5503, 5000 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5503, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5016, 5503 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5016, 5503 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5503, 5000 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5503, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5504, 5000 },
            { 3, 0, -1, 0, 0, 5504, 5000 },
            { 4, 0, -1, 0, 0, 5504, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5505, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5505, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5506, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5016, 5503 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5016, 5503 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5503, 5000 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5503, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5016, 5503 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5016, 5503 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5503, 5000 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5503, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 5029, 5507 },
            { 6, DAM_BYPASS, STAT_LUCK, -3, DAM_BLIND, 5029, 5507 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5508, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5509, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5510, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5511, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5511, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5512, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5512, 5000 },
            { 6, DAM_BYPASS, -1, 0, 0, 5513, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5504, 5000 },
            { 3, 0, -1, 0, 0, 5504, 5000 },
            { 4, 0, -1, 0, 0, 5504, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5505, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5505, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5506, 5000 },
        },
    },
    // KILL_TYPE_RADSCORPION
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 3, DAM_KNOCKED_DOWN, 5001, 5600 },
            { 5, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5001, 5600 },
            { 5, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5001, 5600 },
            { 6, DAM_KNOCKED_DOWN, -1, 0, 0, 5600, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5601, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5016, 5602 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5602, 5000 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5602, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, 2, DAM_CRIP_ARM_RIGHT, 5016, 5603 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5016, 5603 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5603, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5604, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5605, 5000 },
            { 4, DAM_BYPASS, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5605, 5606 },
            { 4, DAM_DEAD, -1, 0, 0, 5607, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 2, 0, 5001, 5600 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5600, 5608 },
            { 4, DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5609, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5608, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 5608, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 2, 0, 5001, 5600 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5600, 5008 },
            { 4, DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5609, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5608, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 5608, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_AGILITY, 3, DAM_BLIND, 5001, 5610 },
            { 6, 0, STAT_AGILITY, 0, DAM_BLIND, 5016, 5610 },
            { 6, 0, STAT_AGILITY, -3, DAM_BLIND, 5016, 5610 },
            { 8, 0, STAT_AGILITY, -3, DAM_BLIND, 5611, 5612 },
            { 8, DAM_DEAD, -1, 0, 0, 5613, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5614, 5000 },
            { 3, 0, -1, 0, 0, 5614, 5000 },
            { 4, 0, -1, 0, 0, 5614, 5000 },
            { 4, DAM_KNOCKED_OUT, -1, 0, 0, 5615, 5000 },
            { 4, DAM_KNOCKED_OUT, -1, 0, 0, 5615, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5616, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 5604, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5605, 5000 },
            { 4, DAM_BYPASS, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5605, 5606 },
            { 4, DAM_DEAD, -1, 0, 0, 5607, 5000 },
        },
    },
    // KILL_TYPE_RAT
    {
        // HIT_LOCATION_HEAD
        {
            { 4, DAM_BYPASS, -1, 0, 0, 5700, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5700, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5701, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5701, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5701, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5701, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
            { 3, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
            { 3, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5703, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
            { 3, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
            { 3, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5705, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5706, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5708, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
            { 3, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
            { 3, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5709, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
            { 3, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
            { 3, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 5710, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, DAM_BYPASS, -1, 0, 0, 5711, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5712, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5712, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5712, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5712, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5712, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5711, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5711, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5712, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 5712, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5706, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5707, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5708, 5000 },
        },
    },
    // KILL_TYPE_FLOATER
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 5, 0, STAT_AGILITY, -3, DAM_KNOCKED_DOWN, 5016, 5800 },
            { 5, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 5800, 5801 },
            { 6, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 5800, 5801 },
            { 6, DAM_DEAD, -1, 0, 0, 5802, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_LOSE_TURN, 5001, 5803 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_LOSE_TURN, 5001, 5803 },
            { 3, DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5804, 5000 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5804, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5805, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_LOSE_TURN, 5001, 5803 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_LOSE_TURN, 5001, 5803 },
            { 3, DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5804, 5000 },
            { 4, DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5804, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5805, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 3, 0, STAT_AGILITY, -2, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5800, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5804, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5805, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 1, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 4, 0, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -1, DAM_CRIP_LEG_LEFT | DAM_CRIP_LEG_RIGHT, 5800, 5806 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT | DAM_CRIP_LEG_RIGHT, 5804, 5806 },
            { 6, DAM_DEAD | DAM_ON_FIRE, -1, 0, 0, 5807, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 5803, 5000 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 5803, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_CRIP_ARM_RIGHT | DAM_LOSE_TURN, -1, 0, 0, 5808, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_CRIP_ARM_RIGHT | DAM_LOSE_TURN, -1, 0, 0, 5808, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5809, 5000 },
            { 5, 0, STAT_ENDURANCE, 0, DAM_BLIND, 5016, 5810 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_BLIND, 5809, 5810 },
            { 6, DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5810, 5000 },
            { 6, DAM_KNOCKED_DOWN | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5801, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 3, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5800, 5000 },
            { 3, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5800, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 3, 0, STAT_AGILITY, -2, DAM_KNOCKED_DOWN, 5001, 5800 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 5800, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5804, 5000 },
            { 4, DAM_DEAD, -1, 0, 0, 5805, 5000 },
        },
    },
    // KILL_TYPE_CENTAUR
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5016, 5900 },
            { 5, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5016, 5900 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5901, 5900 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5901, 5900 },
            { 6, DAM_DEAD, -1, 0, 0, 5902, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_LOSE_TURN, 5016, 5903 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5016, 5904 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 5904, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 5905, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_LOSE_TURN, 5016, 5903 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5016, 5904 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 5904, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 5905, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5901, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 2, 0, 5901, 5900 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5900, 5000 },
            { 5, DAM_DEAD, -1, 0, 0, 5902, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5900, 5000 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5900, 5906 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5906, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 5906, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_LOSE_TURN, -1, 0, 0, 5907, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 5900, 5000 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5900, 5906 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 5906, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 5906, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_LOSE_TURN, -1, 0, 0, 5907, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 1, DAM_BLIND, 5001, 5908 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -1, DAM_BLIND, 5901, 5908 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5909, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 5910, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 5911, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 2, 0, -1, 0, 0, 5912, 5000 },
            { 2, 0, -1, 0, 0, 5912, 5000 },
            { 2, 0, -1, 0, 0, 5912, 5000 },
            { 2, 0, -1, 0, 0, 5912, 5000 },
            { 2, 0, -1, 0, 0, 5912, 5000 },
            { 2, 0, -1, 0, 0, 5912, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5901, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 2, 0, 5901, 5900 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5900, 5000 },
            { 5, DAM_DEAD, -1, 0, 0, 5902, 5000 },
        },
    },
    // KILL_TYPE_ROBOT
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 6000, 5000 },
            { 4, 0, -1, 0, 0, 6000, 5000 },
            { 5, 0, -1, 0, 0, 6000, 5000 },
            { 5, DAM_KNOCKED_DOWN, -1, 0, 0, 6001, 5000 },
            { 6, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6002, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6003, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 6000, 6004 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 6000, 6004 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 6004, 5000 },
            { 4, DAM_CRIP_ARM_LEFT, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 6004, 6005 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 6000, 6004 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 6000, 6004 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 6004, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 6004, 6005 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6006, 5000 },
            { 4, 0, -1, 0, 0, 6007, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6008, 5000 },
            { 6, DAM_BYPASS, -1, 0, 0, 6009, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6010, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 4, 0, -1, 0, 0, 6007, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6000, 6004 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_LEG_RIGHT, 6007, 6004 },
            { 4, DAM_CRIP_LEG_RIGHT, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 6004, 6011 },
            { 4, DAM_CRIP_LEG_RIGHT, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, 6004, 6012 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 4, 0, -1, 0, 0, 6007, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 6000, 6004 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_LEG_LEFT, 6007, 6004 },
            { 4, DAM_CRIP_LEG_LEFT, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 6004, 6011 },
            { 4, DAM_CRIP_LEG_LEFT, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, 6004, 6012 },
        },
        // HIT_LOCATION_EYES
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_BLIND, 6000, 6013 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_BLIND, 6000, 6013 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_BLIND, 6000, 6013 },
            { 3, 0, STAT_ENDURANCE, -6, DAM_BLIND, 6000, 6013 },
            { 3, DAM_BLIND, -1, 0, 0, 6013, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, 0, STAT_ENDURANCE, -1, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 6000, 6002 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 6000, 6002 },
            { 3, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, STAT_ENDURANCE, 0, 0, 6002, 6003 },
            { 3, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, STAT_ENDURANCE, -4, 0, 6002, 6003 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 6000, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6006, 5000 },
            { 4, 0, -1, 0, 0, 6007, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6008, 5000 },
            { 6, DAM_BYPASS, -1, 0, 0, 6009, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6010, 5000 },
        },
    },
    // KILL_TYPE_DOG
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5016, 6100 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5016, 6100 },
            { 4, 0, STAT_ENDURANCE, -6, DAM_CRIP_ARM_LEFT | DAM_CRIP_ARM_RIGHT, 5016, 6101 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6100, 6102 },
            { 4, DAM_DEAD, -1, 0, 0, 6103, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, -1, DAM_CRIP_LEG_LEFT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -5, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, 5001, 6105 },
            { 3, DAM_CRIP_LEG_LEFT, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 6104, 6105 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 6105, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, -1, DAM_CRIP_LEG_RIGHT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -5, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, 5001, 6105 },
            { 3, DAM_CRIP_LEG_RIGHT, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 6104, 6105 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 6105, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 5001, 6100 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_AGILITY, -3, DAM_KNOCKED_DOWN, 5016, 6100 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 6100, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6103, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, STAT_ENDURANCE, 1, DAM_CRIP_LEG_RIGHT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_LEG_RIGHT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, 5001, 6105 },
            { 3, DAM_CRIP_LEG_RIGHT, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 6104, 6105 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 6105, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, STAT_ENDURANCE, 1, DAM_CRIP_LEG_LEFT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_LEG_LEFT, 5001, 6104 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, 5001, 6105 },
            { 3, DAM_CRIP_LEG_LEFT, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 6104, 6105 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 6105, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 5018, 5000 },
            { 6, DAM_BYPASS, -1, 0, 0, 5018, 5000 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, 3, DAM_BLIND, 5018, 6106 },
            { 8, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_BLIND, 5018, 6106 },
            { 8, DAM_DEAD, -1, 0, 0, 6107, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, -2, DAM_KNOCKED_DOWN, 5001, 6100 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_AGILITY, -5, DAM_KNOCKED_DOWN, 5016, 6100 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 6100, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6103, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_AGILITY, -1, DAM_KNOCKED_DOWN, 5001, 6100 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_AGILITY, -3, DAM_KNOCKED_DOWN, 5016, 6100 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 6100, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6103, 5000 },
        },
    },
    // KILL_TYPE_MANTIS
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5001, 6200 },
            { 5, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5016, 6200 },
            { 5, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -1, DAM_KNOCKED_OUT, 6200, 6201 },
            { 6, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6200, 6201 },
            { 6, DAM_DEAD, -1, 0, 0, 6202, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5001, 6203 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5001, 6203 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_LEFT, 5001, 6203 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_LEFT, 5016, 6203 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_LEFT, 5016, 6203 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_LOSE_TURN, -1, 0, 0, 6204, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5001, 6203 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5001, 6203 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_RIGHT, 5001, 6203 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_RIGHT, 5016, 6203 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_RIGHT, 5016, 6203 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_LOSE_TURN, -1, 0, 0, 6204, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 1000, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_BYPASS, 5001, 6205 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_BYPASS, 5001, 6205 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_BYPASS, 5016, 6205 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_BYPASS, 5016, 6205 },
            { 6, DAM_DEAD, -1, 0, 0, 6206, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 6201 },
            { 3, 0, STAT_AGILITY, -2, DAM_KNOCKED_DOWN, 5001, 6201 },
            { 4, 0, STAT_AGILITY, -4, DAM_KNOCKED_DOWN, 5001, 6201 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6201, 6203 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 6201, 6203 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 6207, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 6201 },
            { 3, 0, STAT_AGILITY, -3, DAM_KNOCKED_DOWN, 5001, 6201 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -2, DAM_CRIP_LEG_LEFT, 6201, 6208 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -2, DAM_CRIP_LEG_LEFT, 6201, 6208 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -5, DAM_CRIP_LEG_LEFT, 6201, 6208 },
            { 3, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 6208, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_LOSE_TURN, 6205, 6209 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_LOSE_TURN, 6205, 6209 },
            { 6, DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -3, DAM_BLIND, 6209, 6210 },
            { 8, DAM_KNOCKED_DOWN | DAM_BYPASS | DAM_LOSE_TURN, STAT_ENDURANCE, -3, DAM_BLIND, 6209, 6210 },
            { 8, DAM_DEAD, -1, 0, 0, 6202, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6205, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6209, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 1000, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_BYPASS, 5001, 6205 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_BYPASS, 5001, 6205 },
            { 4, 0, STAT_ENDURANCE, -2, DAM_BYPASS, 5016, 6205 },
            { 4, 0, STAT_ENDURANCE, -4, DAM_BYPASS, 5016, 6205 },
            { 6, DAM_DEAD, -1, 0, 0, 6206, 5000 },
        },
    },
    // KILL_TYPE_DEATH_CLAW
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 5016, 5023 },
            { 5, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 5016, 5023 },
            { 5, 0, STAT_ENDURANCE, -5, DAM_KNOCKED_DOWN, 5016, 5023 },
            { 6, 0, STAT_ENDURANCE, -4, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5016, 5004 },
            { 6, 0, STAT_ENDURANCE, -5, DAM_KNOCKED_DOWN | DAM_LOSE_TURN, 5016, 5004 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 5001, 5011 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_LEFT, 5001, 5011 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_LEFT, 5001, 5011 },
            { 3, 0, STAT_ENDURANCE, -6, DAM_CRIP_ARM_LEFT, 5001, 5011 },
            { 3, 0, STAT_ENDURANCE, -8, DAM_CRIP_ARM_LEFT, 5001, 5011 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 5001, 5014 },
            { 3, 0, STAT_ENDURANCE, -2, DAM_CRIP_ARM_RIGHT, 5001, 5014 },
            { 3, 0, STAT_ENDURANCE, -4, DAM_CRIP_ARM_RIGHT, 5001, 5014 },
            { 3, 0, STAT_ENDURANCE, -6, DAM_CRIP_ARM_RIGHT, 5001, 5014 },
            { 3, 0, STAT_ENDURANCE, -8, DAM_CRIP_ARM_RIGHT, 5001, 5014 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, -1, DAM_BYPASS, 5001, 6300 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, -1, DAM_BYPASS, 5016, 6300 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5004, 5000 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5005, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5004 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5001, 5004 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -2, DAM_CRIP_LEG_RIGHT, 5001, 5004 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -4, DAM_CRIP_LEG_RIGHT, 5016, 5022 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -5, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -6, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5001, 5004 },
            { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 5001, 5004 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -2, DAM_CRIP_LEG_RIGHT, 5001, 5004 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -4, DAM_CRIP_LEG_RIGHT, 5016, 5022 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -5, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -6, DAM_CRIP_LEG_RIGHT, 5023, 5024 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_LOSE_TURN, 5001, 6301 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -6, DAM_LOSE_TURN, 6300, 6301 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -2, DAM_BLIND, 6301, 6302 },
            { 8, DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6302, 5000 },
            { 8, DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6302, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5001, 5000 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 5, 0, STAT_AGILITY, 0, DAM_KNOCKED_DOWN, 5016, 5004 },
            { 5, 0, STAT_AGILITY, -3, DAM_KNOCKED_DOWN, 5016, 5004 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 5001, 5000 },
            { 3, 0, STAT_ENDURANCE, -1, DAM_BYPASS, 5001, 6300 },
            { 4, 0, -1, 0, 0, 5016, 5000 },
            { 4, 0, STAT_ENDURANCE, -1, DAM_BYPASS, 5016, 6300 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 5004, 5000 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 5005, 5000 },
        },
    },
    // KILL_TYPE_PLANT
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 6405, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6400, 5000 },
            { 5, 0, -1, 0, 0, 6401, 5000 },
            { 5, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_LOSE_TURN, 6402, 6403 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -6, DAM_LOSE_TURN, 6402, 6403 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6400, 5000 },
            { 4, 0, -1, 0, 0, 6401, 5000 },
            { 4, 0, -1, 0, 0, 6401, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, -1, 0, 0, 6405, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6400, 5000 },
            { 5, 0, -1, 0, 0, 6401, 5000 },
            { 5, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
            { 6, DAM_BYPASS, STAT_ENDURANCE, -4, DAM_BLIND, 6402, 6406 },
            { 6, DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6406, 6404 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_LOSE_TURN, 6402, 6403 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -6, DAM_LOSE_TURN, 6402, 6403 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, 0, -1, 0, 0, 6405, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6400, 5000 },
            { 4, 0, -1, 0, 0, 6401, 5000 },
            { 4, 0, -1, 0, 0, 6401, 5000 },
            { 4, DAM_BYPASS, -1, 0, 0, 6402, 5000 },
        },
    },
    // KILL_TYPE_GECKO
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 6701, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6700, 5003 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6700, 5003 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6700, 5003 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 6700, 5006 },
            { 6, DAM_DEAD, -1, 0, 0, 6700, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 6702, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6702, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 6702, 5011 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 6702, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6702, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 6702, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6702, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 6701, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6701, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6704, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6704, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6704, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6704, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6705, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6705, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 6705, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6705, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6705, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6705, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6705, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 6705, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 6705, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6705, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6705, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6705, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 4, DAM_BLIND, 6700, 5028 },
            { 4, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 6700, 5028 },
            { 6, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 6700, 5028 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 6700, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6700, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 6700, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 6703, 5000 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 6703, 5035 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6703, 5036 },
            { 3, DAM_KNOCKED_OUT, -1, 0, 0, 6703, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6703, 5036 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6703, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 6700, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6700, 5000 },
            { 4, 0, -1, 0, 0, 6700, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6700, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6700, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6700, 5000 },
        },
    },
    // KILL_TYPE_ALIEN
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 6801, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6800, 5003 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6800, 5003 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6803, 5003 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 6804, 5006 },
            { 6, DAM_DEAD, -1, 0, 0, 6804, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 6806, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6806, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 6806, 5011 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 6806, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6806, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 6806, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6806, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 6800, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6800, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6805, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6805, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 6805, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6805, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6805, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6805, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6805, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 6805, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 6805, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6805, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6805, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6805, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 4, DAM_BLIND, 6803, 5028 },
            { 4, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 6803, 5028 },
            { 6, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 6803, 5028 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 6803, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6803, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 6804, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 6801, 5000 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 6801, 5035 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6801, 5036 },
            { 3, DAM_KNOCKED_OUT, -1, 0, 0, 6801, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6804, 5036 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6804, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 6800, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 4, 0, -1, 0, 0, 6800, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6800, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6800, 5000 },
        },
    },
    // KILL_TYPE_GIANT_ANT
    {
        // HIT_LOCATION_HEAD
        {
            { 4, 0, -1, 0, 0, 6901, 5000 },
            { 4, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6901, 5003 },
            { 5, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6902, 5003 },
            { 5, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6902, 5003 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 0, DAM_BLIND, 6902, 5006 },
            { 6, DAM_DEAD, -1, 0, 0, 6902, 5000 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 6906, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6906, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_LEFT, 6906, 5011 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
            { 4, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 6906, 5000 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 6906, 5000 },
            { 4, 0, STAT_ENDURANCE, -3, DAM_CRIP_ARM_RIGHT, 6906, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
            { 4, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6906, 5000 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 6900, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6900, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6904, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6904, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6904, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6904, 5000 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6905, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6905, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 6905, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6905, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6905, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6905, 5000 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6905, 5000 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 6905, 5024 },
            { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 6905, 5024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6905, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6905, 5026 },
            { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6905, 5000 },
        },
        // HIT_LOCATION_EYES
        {
            { 4, 0, STAT_LUCK, 4, DAM_BLIND, 6900, 5028 },
            { 4, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 6906, 5028 },
            { 6, DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 6901, 5028 },
            { 6, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 6901, 5000 },
            { 8, DAM_KNOCKED_OUT | DAM_BLIND | DAM_BYPASS, -1, 0, 0, 6901, 5000 },
            { 8, DAM_DEAD, -1, 0, 0, 6901, 5000 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 6900, 5000 },
            { 3, DAM_BYPASS, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 6900, 5035 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_KNOCKED_OUT, 6900, 5036 },
            { 3, DAM_KNOCKED_OUT, -1, 0, 0, 6903, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_OUT, 6903, 5036 },
            { 4, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6903, 5000 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 6900, 5000 },
            { 3, DAM_BYPASS, -1, 0, 0, 6900, 5000 },
            { 4, 0, -1, 0, 0, 6904, 5000 },
            { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6904, 5000 },
            { 6, DAM_KNOCKED_OUT | DAM_BYPASS, -1, 0, 0, 6904, 5000 },
            { 6, DAM_DEAD, -1, 0, 0, 6904, 5000 },
        },
    },
    // KILL_TYPE_BIG_BAD_BOSS
    {
        // HIT_LOCATION_HEAD
        {
            { 3, 0, -1, 0, 0, 7101, 7100 },
            { 3, 0, -1, 0, 0, 7102, 7103 },
            { 4, 0, -1, 0, 0, 7102, 7103 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 7104, 7103 },
            { 5, DAM_KNOCKED_DOWN, STAT_LUCK, 0, DAM_BLIND, 7105, 7106 },
            { 6, DAM_KNOCKED_DOWN, -1, 0, 0, 7105, 7100 },
        },
        // HIT_LOCATION_LEFT_ARM
        {
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 7106, 7011 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 7106, 7100 },
            { 4, DAM_CRIP_ARM_LEFT, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_RIGHT_ARM
        {
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 7106, 7100 },
            { 4, DAM_CRIP_ARM_RIGHT, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_TORSO
        {
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 4, 0, -1, 0, 0, 7106, 7100 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
            { 5, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_RIGHT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 7106, 7106 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_RIGHT, 7060, 7106 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 7106, 7100 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT, -1, 0, 0, 7106, 7106 },
            { 4, DAM_CRIP_LEG_RIGHT, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_LEFT_LEG
        {
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 7106, 7024 },
            { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, -3, DAM_CRIP_LEG_LEFT, 7106, 7024 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 7106, 7100 },
            { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT, -1, 0, 0, 7106, 7106 },
            { 4, DAM_CRIP_LEG_LEFT, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_EYES
        {
            { 3, 0, -1, 0, 0, 7106, 7106 },
            { 3, 0, -1, 0, 0, 7106, 7106 },
            { 4, 0, STAT_LUCK, 2, DAM_BLIND, 7106, 7106 },
            { 4, DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
            { 5, DAM_BLIND | DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
            { 5, DAM_BLIND | DAM_LOSE_TURN, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_GROIN
        {
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, STAT_ENDURANCE, -3, DAM_KNOCKED_DOWN, 7106, 7106 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7106 },
            { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
            { 4, 0, -1, 0, 0, 7106, 7106 },
            { 4, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
        },
        // HIT_LOCATION_UNCALLED
        {
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 3, 0, -1, 0, 0, 7106, 7100 },
            { 4, 0, -1, 0, 0, 7106, 7100 },
            { 4, 0, -1, 0, 0, 7106, 7100 },
            { 5, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
            { 5, DAM_KNOCKED_DOWN, -1, 0, 0, 7106, 7100 },
        },
    },
};

// Player's criticals effects.
//
// 0x5179B0
static CriticalHitDescription gPlayerCriticalHitTable[HIT_LOCATION_COUNT][CRTICIAL_EFFECT_COUNT] = {
    {
        { 3, 0, -1, 0, 0, 6500, 5000 },
        { 3, DAM_BYPASS, STAT_ENDURANCE, 3, DAM_KNOCKED_DOWN, 6501, 6503 },
        { 3, DAM_BYPASS, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 6501, 6503 },
        { 3, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_ENDURANCE, 2, DAM_KNOCKED_OUT, 6503, 6502 },
        { 3, DAM_KNOCKED_OUT | DAM_BYPASS, STAT_LUCK, 2, DAM_BLIND, 6502, 6504 },
        { 6, DAM_BYPASS, STAT_ENDURANCE, -2, DAM_DEAD, 6501, 6505 },
    },
    {
        { 2, 0, -1, 0, 0, 6506, 5000 },
        { 2, DAM_LOSE_TURN, -1, 0, 0, 6507, 5000 },
        { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_LEFT, 6508, 6509 },
        { 3, DAM_BYPASS, -1, 0, 0, 6501, 5000 },
        { 3, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6510, 5000 },
        { 3, DAM_CRIP_ARM_LEFT | DAM_BYPASS, -1, 0, 0, 6510, 5000 },
    },
    {
        { 2, 0, -1, 0, 0, 6506, 5000 },
        { 2, DAM_LOSE_TURN, -1, 0, 0, 6507, 5000 },
        { 3, 0, STAT_ENDURANCE, 0, DAM_CRIP_ARM_RIGHT, 6508, 6509 },
        { 3, DAM_BYPASS, -1, 0, 0, 6501, 5000 },
        { 3, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6511, 5000 },
        { 3, DAM_CRIP_ARM_RIGHT | DAM_BYPASS, -1, 0, 0, 6511, 5000 },
    },
    {
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, DAM_BYPASS, -1, 0, 0, 6508, 5000 },
        { 3, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6503, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6503, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_LUCK, 2, DAM_DEAD, 6503, 6513 },
    },
    {
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6514, 5000 },
        { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_RIGHT, 6514, 6515 },
        { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6516, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6516, 5000 },
        { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_RIGHT | DAM_BYPASS, -1, 0, 0, 6517, 5000 },
    },
    {
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6514, 5000 },
        { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 0, DAM_CRIP_LEG_LEFT, 6514, 6515 },
        { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6516, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6516, 5000 },
        { 4, DAM_KNOCKED_OUT | DAM_CRIP_LEG_LEFT | DAM_BYPASS, -1, 0, 0, 6517, 5000 },
    },
    {
        { 3, 0, -1, 0, 0, 6518, 5000 },
        { 3, 0, STAT_LUCK, 3, DAM_BLIND, 6518, 6519 },
        { 3, DAM_BYPASS, STAT_LUCK, 3, DAM_BLIND, 6501, 6519 },
        { 4, DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 6520, 5000 },
        { 4, DAM_BLIND | DAM_BYPASS | DAM_LOSE_TURN, -1, 0, 0, 6521, 5000 },
        { 6, DAM_DEAD, -1, 0, 0, 6522, 5000 },
    },
    {
        { 3, 0, -1, 0, 0, 6523, 5000 },
        { 3, 0, STAT_ENDURANCE, 0, DAM_KNOCKED_DOWN, 6523, 6524 },
        { 3, DAM_KNOCKED_DOWN, -1, 0, 0, 6524, 5000 },
        { 3, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 4, DAM_KNOCKED_OUT, 6524, 6525 },
        { 4, DAM_KNOCKED_DOWN, STAT_ENDURANCE, 2, DAM_KNOCKED_OUT, 6524, 6525 },
        { 4, DAM_KNOCKED_OUT, -1, 0, 0, 6526, 5000 },
    },
    {
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, 0, -1, 0, 0, 6512, 5000 },
        { 3, DAM_BYPASS, -1, 0, 0, 6508, 5000 },
        { 3, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6503, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, -1, 0, 0, 6503, 5000 },
        { 4, DAM_KNOCKED_DOWN | DAM_BYPASS, STAT_LUCK, 2, DAM_DEAD, 6503, 6513 },
    },
};

// 0x517F98
static int _combat_end_due_to_load = 0;

// 0x517F9C
static bool _combat_cleanup_enabled = false;

// Provides effects caused by failing weapons.
//
// 0x517FA0
static const int _cf_table[WEAPON_CRITICAL_FAILURE_TYPE_COUNT][WEAPON_CRITICAL_FAILURE_EFFECT_COUNT] = {
    { 0, DAM_LOSE_TURN, DAM_LOSE_TURN, DAM_HURT_SELF | DAM_KNOCKED_DOWN, DAM_CRIP_RANDOM },
    { 0, DAM_LOSE_TURN, DAM_DROP, DAM_RANDOM_HIT, DAM_HIT_SELF },
    { 0, DAM_LOSE_AMMO, DAM_DROP, DAM_RANDOM_HIT, DAM_DESTROY },
    { DAM_LOSE_TURN, DAM_LOSE_TURN | DAM_LOSE_AMMO, DAM_DROP | DAM_LOSE_TURN, DAM_RANDOM_HIT, DAM_EXPLODE | DAM_LOSE_TURN },
    { DAM_DUD, DAM_DROP, DAM_DROP | DAM_HURT_SELF, DAM_RANDOM_HIT, DAM_EXPLODE },
    { DAM_LOSE_TURN, DAM_DUD, DAM_DESTROY, DAM_RANDOM_HIT, DAM_EXPLODE | DAM_LOSE_TURN | DAM_KNOCKED_DOWN },
    { 0, DAM_LOSE_TURN, DAM_RANDOM_HIT, DAM_DESTROY, DAM_EXPLODE | DAM_LOSE_TURN | DAM_ON_FIRE },
};

// 0x56D2B0
static Attack _main_ctd;

// combat.msg
//
// 0x56D368
MessageList gCombatMessageList;

// 0x56D378
static int _combat_elev;

// 0x56D37C
int _list_total;

// Probably last who_hit_me of obj_dude
//
// 0x56D380
static Object* _combat_ending_guy;

// 0x56D384
static int _list_noncom;

// 0x56D388
static Object* _combat_turn_obj;

// target_highlight
//
// 0x56D38C
static int _combat_highlight;

// 0x56D390
Object** _combat_list;

// 0x56D394
static int _list_com;

// Experience received for killing critters during current combat, PER PLAYER
// ACTOR (index = slot). Vanilla accrued one scalar here and paid it out at
// combat end; with N players the kill credit has to survive to the payout, so
// the accrual is bucketed by the slot of whoever landed the kill.
//
// ⚠ ACCRUAL AND PAYOUT MUST CHANGE TOGETHER (PLAYER_SHEET_DESIGN.md §7 risk 2):
// a slot-index bug here misattributes or silently zeroes XP, and there is no
// headless N>1 oracle to catch it. Slot 0 stays the vanilla bucket, so with an
// empty registry this is one scalar with extra zeros beside it.
static int _combat_exps[kMaxPlayerActors];

// bonus action points from BONUS_MOVE perk.
//
// 0x56D39C
int _combat_free_move;

// 0x56D3A0
static Attack _shoot_ctd;

// 0x56D458
static Attack _explosion_ctd;

static CriticalHitDescription gBaseCriticalHitTables[SFALL_KILL_TYPE_COUNT][HIT_LOCATION_COUNT][CRTICIAL_EFFECT_COUNT];
static CriticalHitDescription gBasePlayerCriticalHitTable[HIT_LOCATION_COUNT][CRTICIAL_EFFECT_COUNT];

static const char* gCritDataMemberKeys[CRIT_DATA_MEMBER_COUNT] = {
    "DamageMultiplier",
    "EffectFlags",
    "StatCheck",
    "StatMod",
    "FailureEffect",
    "Message",
    "FailMessage",
};

static bool gBurstModEnabled = false;
static int gBurstModCenterMultiplier = SFALL_CONFIG_BURST_MOD_DEFAULT_CENTER_MULTIPLIER;
static int gBurstModCenterDivisor = SFALL_CONFIG_BURST_MOD_DEFAULT_CENTER_DIVISOR;
static int gBurstModTargetMultiplier = SFALL_CONFIG_BURST_MOD_DEFAULT_TARGET_MULTIPLIER;
static int gBurstModTargetDivisor = SFALL_CONFIG_BURST_MOD_DEFAULT_TARGET_DIVISOR;
static UnarmedHitDescription gUnarmedHitDescriptions[HIT_MODE_COUNT];
static int gDamageCalculationType;
static bool gBonusHthDamageFix;
static bool gDisplayBonusDamage;

// combat_init
// 0x420CC0
int combatInit()
{
    int max_action_points;
    char path[COMPAT_MAX_PATH];

    _combat_turn_running = 0;
    _combatNumTurns = 0;
    _combat_list = nullptr;
    _aiInfoList = nullptr;
    _list_com = 0;
    _list_noncom = 0;
    _list_total = 0;
    _gcsd = nullptr;
    _combat_call_display = 0;
    gCombatState = COMBAT_STATE_0x02;

    max_action_points = critterGetStat(gDude, STAT_MAXIMUM_ACTION_POINTS);

    _combat_free_move = 0;
    _combat_ending_guy = nullptr;
    _combat_end_due_to_load = 0;

    gDude->data.critter.combat.ap = max_action_points;

    _combat_cleanup_enabled = 0;

    if (!messageListInit(&gCombatMessageList)) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "combat.msg");

    if (!messageListLoad(&gCombatMessageList, path)) {
        return -1;
    }

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_COMBAT, &gCombatMessageList);

    // SFALL
    criticalsInit();
    burstModInit();
    unarmedInit();
    damageModInit();
    combat_reset_hit_location_penalty();

    return 0;
}

// 0x420DA0
void combatReset()
{
    int max_action_points;

    _combat_turn_running = 0;
    _combatNumTurns = 0;
    _combat_list = nullptr;
    _aiInfoList = nullptr;
    _list_com = 0;
    _list_noncom = 0;
    _list_total = 0;
    _gcsd = nullptr;
    _combat_call_display = 0;
    gCombatState = COMBAT_STATE_0x02;

    max_action_points = critterGetStat(gDude, STAT_MAXIMUM_ACTION_POINTS);

    _combat_free_move = 0;
    _combat_ending_guy = nullptr;

    gDude->data.critter.combat.ap = max_action_points;

    // SFALL
    criticalsReset();
    combat_reset_hit_location_penalty();
}

// 0x420E14
void combatExit()
{
    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_COMBAT, nullptr);
    messageListFree(&gCombatMessageList);

    // SFALL
    criticalsExit();
}

// 0x420E24
int _find_cid(int a1, int cid, Object** critterList, int critterListLength)
{
    int index;

    for (index = a1; index < critterListLength; index++) {
        if (critterList[index]->cid == cid) {
            break;
        }
    }

    return index;
}

// 0x420E4C
int combatLoad(File* stream)
{
    if (fileReadUInt32(stream, &gCombatState) == -1) return -1;

    if (!isInCombat()) {
        Object* obj = objectFindFirst();
        while (obj != nullptr) {
            if (PID_TYPE(obj->pid) == OBJ_TYPE_CRITTER) {
                if (obj->data.critter.combat.whoHitMeCid == -1) {
                    obj->data.critter.combat.whoHitMe = nullptr;
                }
            }
            obj = objectFindNext();
        }
        return 0;
    }

    if (fileReadInt32(stream, &_combat_turn_running) == -1) return -1;
    if (fileReadInt32(stream, &_combat_free_move) == -1) return -1;
    // Slot 0 ONLY, so the on-disk shape is byte-identical. Extras' PENDING
    // in-combat XP is not persisted: a save taken mid-fight pays out the host's
    // bucket and drops theirs. Acceptable only because co-op saves do not exist
    // yet — N-actor-izing this record is stage 4 (PLAYER_SHEET_DESIGN.md §5).
    if (fileReadInt32(stream, &_combat_exps[0]) == -1) return -1;
    if (fileReadInt32(stream, &_list_com) == -1) return -1;
    if (fileReadInt32(stream, &_list_noncom) == -1) return -1;
    if (fileReadInt32(stream, &_list_total) == -1) return -1;

    if (objectListCreate(-1, gElevation, OBJ_TYPE_CRITTER, &_combat_list) != _list_total) {
        objectListFree(_combat_list);
        return -1;
    }

    if (fileReadInt32(stream, &(gDude->cid)) == -1) return -1;

    for (int index = 0; index < _list_total; index++) {
        if (_combat_list[index]->data.critter.combat.whoHitMeCid == -1) {
            _combat_list[index]->data.critter.combat.whoHitMe = nullptr;
        } else {
            // NOTE: Uninline.
            int found = _find_cid(0, _combat_list[index]->data.critter.combat.whoHitMeCid, _combat_list, _list_total);
            if (found == _list_total) {
                _combat_list[index]->data.critter.combat.whoHitMe = nullptr;
            } else {
                _combat_list[index]->data.critter.combat.whoHitMe = _combat_list[found];
            }
        }
    }

    for (int index = 0; index < _list_total; index++) {
        int cid;
        if (fileReadInt32(stream, &cid) == -1) return -1;

        // NOTE: Uninline.
        int found = _find_cid(index, cid, _combat_list, _list_total);
        if (found == _list_total) {
            return -1;
        }

        Object* obj = _combat_list[index];
        _combat_list[index] = _combat_list[found];
        _combat_list[found] = obj;
    }

    for (int index = 0; index < _list_total; index++) {
        _combat_list[index]->cid = index;
    }

    if (_aiInfoList != nullptr) {
        internal_free(_aiInfoList);
    }

    _aiInfoList = (CombatAiInfo*)internal_malloc(sizeof(*_aiInfoList) * _list_total);
    if (_aiInfoList == nullptr) {
        return -1;
    }

    for (int index = 0; index < _list_total; index++) {
        CombatAiInfo* aiInfo = &(_aiInfoList[index]);

        int friendlyId;
        if (fileReadInt32(stream, &friendlyId) == -1) return -1;

        if (friendlyId == -1) {
            aiInfo->friendlyDead = nullptr;
        } else {
            // SFALL: Fix incorrect object type search when loading a game in
            // combat mode.
            aiInfo->friendlyDead = objectTypedFindById(friendlyId, OBJ_TYPE_CRITTER);
            if (aiInfo->friendlyDead == nullptr) return -1;
        }

        int targetId;
        if (fileReadInt32(stream, &targetId) == -1) return -1;

        if (targetId == -1) {
            aiInfo->lastTarget = nullptr;
        } else {
            // SFALL: Fix incorrect object type search when loading a game in
            // combat mode.
            aiInfo->lastTarget = objectTypedFindById(targetId, OBJ_TYPE_CRITTER);
            if (aiInfo->lastTarget == nullptr) return -1;
        }

        int itemId;
        if (fileReadInt32(stream, &itemId) == -1) return -1;

        if (itemId == -1) {
            aiInfo->lastItem = nullptr;
        } else {
            // SFALL: Fix incorrect object type search when loading a game in
            // combat mode.
            aiInfo->lastItem = objectTypedFindById(itemId, OBJ_TYPE_ITEM);
            if (aiInfo->lastItem == nullptr) return -1;
        }

        if (fileReadInt32(stream, &(aiInfo->lastMove)) == -1) return -1;
    }

    _combat_begin_extra(gDude);

    return 0;
}

// 0x421244
int combatSave(File* stream)
{
    // v1 policy: no save point exists mid-fight on the resumable server path, so a
    // live session here would mean the roster/round cursor is being frozen without
    // its machine state. Log loud rather than silently persist an inconsistent CSD.
    if (combatSessionActive()) {
        fprintf(stderr, "SERVER: WARNING — combatSave while a resumable combat session is active (unsupported mid-fight save).\n");
    }

    if (fileWriteInt32(stream, gCombatState) == -1) return -1;

    if (!isInCombat()) return 0;

    if (fileWriteInt32(stream, _combat_turn_running) == -1) return -1;
    if (fileWriteInt32(stream, _combat_free_move) == -1) return -1;
    if (fileWriteInt32(stream, _combat_exps[0]) == -1) return -1;
    if (fileWriteInt32(stream, _list_com) == -1) return -1;
    if (fileWriteInt32(stream, _list_noncom) == -1) return -1;
    if (fileWriteInt32(stream, _list_total) == -1) return -1;
    if (fileWriteInt32(stream, gDude->cid) == -1) return -1;

    for (int index = 0; index < _list_total; index++) {
        if (fileWriteInt32(stream, _combat_list[index]->cid) == -1) return -1;
    }

    if (_aiInfoList == nullptr) {
        return -1;
    }

    for (int index = 0; index < _list_total; index++) {
        CombatAiInfo* aiInfo = &(_aiInfoList[index]);

        if (fileWriteInt32(stream, aiInfo->friendlyDead != nullptr ? aiInfo->friendlyDead->id : -1) == -1) return -1;
        if (fileWriteInt32(stream, aiInfo->lastTarget != nullptr ? aiInfo->lastTarget->id : -1) == -1) return -1;
        if (fileWriteInt32(stream, aiInfo->lastItem != nullptr ? aiInfo->lastItem->id : -1) == -1) return -1;
        if (fileWriteInt32(stream, aiInfo->lastMove) == -1) return -1;
    }

    return 0;
}

// 0x4213E8
bool _combat_safety_invalidate_weapon(Object* attacker, Object* weapon, int hitMode, Object* defender, int* safeDistancePtr)
{
    return _combat_safety_invalidate_weapon_func(attacker, weapon, hitMode, defender, safeDistancePtr, nullptr);
}

// 0x4213FC
static bool _combat_safety_invalidate_weapon_func(Object* attacker, Object* weapon, int hitMode, Object* defender, int* safeDistancePtr, Object* attackerFriend)
{
    if (safeDistancePtr != nullptr) {
        *safeDistancePtr = 0;
    }

    if (attacker->pid == PROTO_ID_0x10001E0) {
        return false;
    }

    int intelligence = critterGetStat(attacker, STAT_INTELLIGENCE);
    int team = attacker->data.critter.combat.team;
    int damageRadius = weaponGetDamageRadius(weapon, hitMode);
    int maxDamage;
    weaponGetDamageMinMax(weapon, nullptr, &maxDamage);
    int damageType = weaponGetDamageType(attacker, weapon);

    if (damageRadius > 0) {
        if (intelligence < 5) {
            damageRadius -= 5 - intelligence;
            if (damageRadius < 0) {
                damageRadius = 0;
            }
        }

        if (attackerFriend != nullptr) {
            if (objectGetDistanceBetween(defender, attackerFriend) < damageRadius) {
                debugPrint("Friendly was in the way!");
                return true;
            }
        }

        for (int index = 0; index < _list_total; index++) {
            Object* candidate = _combat_list[index];
            if (candidate->data.critter.combat.team == team
                && candidate != attacker
                && candidate != defender
                && !critterIsDead(candidate)) {
                int v14 = objectGetDistanceBetween(defender, candidate);
                if (v14 < damageRadius && candidate != candidate->data.critter.combat.whoHitMe) {
                    int damageThreshold = critterGetStat(candidate, STAT_DAMAGE_THRESHOLD + damageType);
                    int damageResistance = critterGetStat(candidate, STAT_DAMAGE_RESISTANCE + damageType);
                    if (damageResistance * (maxDamage - damageThreshold) / 100 > 0) {
                        return true;
                    }
                }
            }
        }

        if (objectGetDistanceBetween(defender, attacker) <= damageRadius) {
            if (safeDistancePtr != nullptr) {
                *safeDistancePtr = damageRadius - objectGetDistanceBetween(defender, attacker) + 1;
                return false;
            }

            return true;
        }

        return false;
    }

    int anim = weaponGetAnimationForHitMode(weapon, hitMode);
    if (anim != ANIM_FIRE_BURST && anim != ANIM_FIRE_CONTINUOUS) {
        return false;
    }

    Attack attack;
    attackInit(&attack, attacker, defender, hitMode, HIT_LOCATION_TORSO);

    int accuracy = attackDetermineToHit(attacker, attacker->tile, defender, HIT_LOCATION_TORSO, hitMode, true);
    int roundsHitMainTarget;
    int roundsSpent;
    _compute_spray(&attack, accuracy, &roundsHitMainTarget, &roundsSpent, anim);

    if (attackerFriend != nullptr) {
        for (int index = 0; index < attack.extrasLength; index++) {
            if (attack.extras[index] == attackerFriend) {
                debugPrint("Friendly was in the way!");
                return true;
            }
        }
    }

    for (int index = 0; index < attack.extrasLength; index++) {
        Object* candidate = attack.extras[index];
        if (candidate->data.critter.combat.team == team
            && candidate != attacker
            && candidate != defender
            && !critterIsDead(candidate)
            && candidate != candidate->data.critter.combat.whoHitMe) {
            int damageThreshold = critterGetStat(candidate, STAT_DAMAGE_THRESHOLD + damageType);
            int damageResistance = critterGetStat(candidate, STAT_DAMAGE_RESISTANCE + damageType);
            if (damageResistance * (maxDamage - damageThreshold) / 100 > 0) {
                return true;
            }
        }
    }

    return false;
}

// 0x4217BC
bool _combatTestIncidentalHit(Object* attacker, Object* defender, Object* attackerFriend, Object* weapon)
{
    return _combat_safety_invalidate_weapon_func(attacker, weapon, HIT_MODE_RIGHT_WEAPON_PRIMARY, defender, nullptr, attackerFriend);
}

// 0x4217D4
Object* _combat_whose_turn()
{
    if (isInCombat()) {
        return _combat_turn_obj;
    } else {
        return nullptr;
    }
}

// 0x4217E8
void _combat_data_init(Object* obj)
{
    obj->data.critter.combat.damageLastTurn = 0;
    obj->data.critter.combat.results = 0;
}

// NOTE: Inlined.
//
// 0x4217FC
static void _combatInitAIInfoList()
{
    int index;

    for (index = 0; index < _list_total; index++) {
        _aiInfoList[index].friendlyDead = nullptr;
        _aiInfoList[index].lastTarget = nullptr;
        _aiInfoList[index].lastItem = nullptr;
        _aiInfoList[index].lastMove = 0;
    }
}

// 0x421850
static int aiInfoCopy(int srcIndex, int destIndex)
{
    CombatAiInfo* src = &_aiInfoList[srcIndex];
    CombatAiInfo* dest = &_aiInfoList[destIndex];

    dest->friendlyDead = src->friendlyDead;
    dest->lastTarget = src->lastTarget;
    dest->lastItem = src->lastItem;
    dest->lastMove = src->lastMove;

    return 0;
}

// 0x421880
Object* aiInfoGetFriendlyDead(Object* obj)
{
    if (!isInCombat()) {
        return nullptr;
    }

    if (obj == nullptr) {
        return nullptr;
    }

    if (obj->cid == -1) {
        return nullptr;
    }

    return _aiInfoList[obj->cid].friendlyDead;
}

// 0x4218AC
int aiInfoSetFriendlyDead(Object* a1, Object* a2)
{
    if (!isInCombat()) {
        return 0;
    }

    if (a1 == nullptr) {
        return -1;
    }

    if (a1->cid == -1) {
        return -1;
    }

    if (a1 == a2) {
        return -1;
    }

    _aiInfoList[a1->cid].friendlyDead = a2;

    return 0;
}

// 0x4218EC
Object* aiInfoGetLastTarget(Object* obj)
{
    if (!isInCombat()) {
        return nullptr;
    }

    if (obj == nullptr) {
        return nullptr;
    }

    if (obj->cid == -1) {
        return nullptr;
    }

    return _aiInfoList[obj->cid].lastTarget;
}

// 0x421918
int aiInfoSetLastTarget(Object* a1, Object* a2)
{
    if (!isInCombat()) {
        return 0;
    }

    if (a1 == nullptr) {
        return -1;
    }

    if (a1->cid == -1) {
        return -1;
    }

    if (a1 == a2) {
        return -1;
    }

    if (critterIsDead(a2)) {
        a2 = nullptr;
    }

    _aiInfoList[a1->cid].lastTarget = a2;

    return 0;
}

// 0x42196C
Object* aiInfoGetLastItem(Object* obj)
{
    int v1;

    if (!isInCombat()) {
        return nullptr;
    }

    if (obj == nullptr) {
        return nullptr;
    }

    v1 = obj->cid;
    if (v1 == -1) {
        return nullptr;
    }

    return _aiInfoList[v1].lastItem;
}

// 0x421998
int aiInfoSetLastItem(Object* obj, Object* a2)
{
    int v2;

    if (!isInCombat()) {
        return 0;
    }

    if (obj == nullptr) {
        return -1;
    }

    v2 = obj->cid;
    if (v2 == -1) {
        return -1;
    }

    _aiInfoList[v2].lastItem = nullptr;

    return 0;
}

// NOTE: Inlined.
//
// 0x421A00
static int _combatAIInfoSetLastMove(Object* object, int move)
{
    if (!isInCombat()) {
        return 0;
    }

    if (object == nullptr) {
        return -1;
    }

    if (object->cid == -1) {
        return -1;
    }

    _aiInfoList[object->cid].lastMove = move;

    return 0;
}

// 0x421A34
static void _combat_begin(Object* attacker)
{
    _combat_turn_running = 0;
    animationStop();
    tickersRemove(_dude_fidget);
    _combat_elev = gElevation;

    if (!isInCombat()) {
        _combatNumTurns = 0;
        for (int slot = 0; slot < kMaxPlayerActors; slot++) {
            _combat_exps[slot] = 0;
        }
        _combat_list = nullptr;
        _list_total = objectListCreate(-1, _combat_elev, OBJ_TYPE_CRITTER, &_combat_list);
        _list_noncom = _list_total;
        _list_com = 0;
        _aiInfoList = (CombatAiInfo*)internal_malloc(sizeof(*_aiInfoList) * _list_total);
        if (_aiInfoList == nullptr) {
            return;
        }

        // NOTE: Uninline.
        _combatInitAIInfoList();

        Object* v1 = nullptr;
        for (int index = 0; index < _list_total; index++) {
            Object* critter = _combat_list[index];
            CritterCombatData* combatData = &(critter->data.critter.combat);
            combatData->maneuver &= CRITTER_MANEUVER_ENGAGING;
            combatData->damageLastTurn = 0;
            combatData->whoHitMe = nullptr;
            combatData->ap = 0;
            critter->cid = index;

            // NOTE: Uninline.
            _combatAIInfoSetLastMove(critter, 0);

            scriptSetObjects(critter->sid, nullptr, nullptr);
            scriptSetFixedParam(critter->sid, 0);
            if (critter->pid == 0x1000098) {
                if (!critterIsDead(critter)) {
                    v1 = critter;
                }
            }
        }

        gCombatState |= COMBAT_STATE_0x01;

        presenter()->worldInvalidate();
        gameUiDisable(0);
        presenter()->cursorSet(MOUSE_CURSOR_WAIT_WATCH);
        _combat_ending_guy = nullptr;
        _combat_begin_extra(attacker);
        _caiTeamCombatInit(_combat_list, _list_total);
        presenter()->hudEndButtonsShow(true);
        presenter()->scrollEnable();

        if (v1 != nullptr && !_isLoadingGame()) {
            int fid = buildFid(FID_TYPE(v1->fid),
                100,
                FID_ANIM_TYPE(v1->fid),
                (v1->fid & 0xF000) >> 12,
                (v1->fid & 0x70000000) >> 28);

            reg_anim_clear(v1);
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
            animationRegisterAnimate(v1, ANIM_UP_STAIRS_RIGHT, -1);
            animationRegisterSetFid(v1, fid, -1);
            reg_anim_end();

            while (animationIsBusy(v1)) {
                _process_bk();
            }
        }
    }
}

// 0x421C8C
static void _combat_begin_extra(Object* attacker)
{
    for (int index = 0; index < _list_total; index++) {
        _combat_update_critter_outline_for_los(_combat_list[index], 0);
    }

    attackInit(&_main_ctd, attacker, nullptr, 4, 3);

    _combat_turn_obj = attacker;

    _combat_ai_begin(_list_total, _combat_list);

    _combat_highlight = settings.preferences.target_highlight;
}

// NOTE: Inlined.
//
// 0x421D18
void _combat_update_critters_in_los(bool a1)
{
    int index;

    for (index = 0; index < _list_total; index++) {
        _combat_update_critter_outline_for_los(_combat_list[index], a1);
    }
}

// Something with outlining.
//
// 0x421D50
void _combat_update_critter_outline_for_los(Object* critter, bool a2)
{
    if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    if (critter == gDude) {
        return;
    }

    if (critterIsDead(critter)) {
        return;
    }

    bool v5 = false;
    if (!_combat_is_shot_blocked(gDude, gDude->tile, critter->tile, critter, nullptr)) {
        v5 = true;
    }

    if (v5) {
        int outlineType = critter->outline & OUTLINE_TYPE_MASK;
        if (outlineType != OUTLINE_TYPE_HOSTILE && outlineType != OUTLINE_TYPE_FRIENDLY) {
            int newOutlineType = gDude->data.critter.combat.team == critter->data.critter.combat.team
                ? OUTLINE_TYPE_FRIENDLY
                : OUTLINE_TYPE_HOSTILE;
            objectDisableOutline(critter, nullptr);
            objectClearOutline(critter, nullptr);
            objectSetOutline(critter, newOutlineType, nullptr);
            if (a2) {
                objectEnableOutline(critter, nullptr);
            } else {
                objectDisableOutline(critter, nullptr);
            }
        } else {
            if (critter->outline != 0 && (critter->outline & OUTLINE_DISABLED) == 0) {
                if (!a2) {
                    objectDisableOutline(critter, nullptr);
                }
            } else {
                if (a2) {
                    objectEnableOutline(critter, nullptr);
                }
            }
        }
    } else {
        int v7 = objectGetDistanceBetween(gDude, critter);
        int v8 = critterGetStat(gDude, STAT_PERCEPTION) * 5;
        if ((critter->flags & OBJECT_TRANS_GLASS) != 0) {
            v8 /= 2;
        }

        if (v7 <= v8) {
            v5 = true;
        }

        int outlineType = critter->outline & OUTLINE_TYPE_MASK;
        if (outlineType != OUTLINE_TYPE_32) {
            objectDisableOutline(critter, nullptr);
            objectClearOutline(critter, nullptr);

            if (v5) {
                objectSetOutline(critter, OUTLINE_TYPE_32, nullptr);

                if (a2) {
                    objectEnableOutline(critter, nullptr);
                } else {
                    objectDisableOutline(critter, nullptr);
                }
            }
        } else {
            if (critter->outline != 0 && (critter->outline & OUTLINE_DISABLED) == 0) {
                if (!a2) {
                    objectDisableOutline(critter, nullptr);
                }
            } else {
                if (a2) {
                    objectEnableOutline(critter, nullptr);
                }
            }
        }
    }
}

// Probably complete combat sequence.
//
// 0x421EFC
static void _combat_over()
{
    // MP_PROTOCOL.md §2: combat ended. Emitted at the teardown choke (covers the
    // normal end + _combat_over_from_load). No-op under null/client (byte-
    // identical). See Presenter::combatExit.
    presenter()->combatExit();

    if (_game_user_wants_to_quit == 0) {
        for (int index = 0; index < _list_com; index++) {
            Object* critter = _combat_list[index];
            if (critter != gDude) {
                // SFALL: Fix to prevent dead NPCs from reloading their weapons.
                if ((critter->data.critter.combat.results & DAM_DEAD) == 0) {
                    aiAttemptWeaponReload(critter, 0);
                }
            }
        }
    }

    tickersAdd(_dude_fidget);

    for (int index = 0; index < _list_noncom + _list_com; index++) {
        Object* critter = _combat_list[index];
        critter->data.critter.combat.damageLastTurn = 0;
        critter->data.critter.combat.maneuver = CRITTER_MANEUVER_NONE;
    }

    for (int index = 0; index < _list_total; index++) {
        Object* critter = _combat_list[index];
        critter->data.critter.combat.ap = 0;
        objectClearOutline(critter, nullptr);
        critter->data.critter.combat.whoHitMe = nullptr;

        scriptSetObjects(critter->sid, nullptr, nullptr);
        scriptSetFixedParam(critter->sid, 0);

        if (critter->pid == 0x1000098 && !critterIsDead(critter) && !_isLoadingGame()) {
            int fid = buildFid(FID_TYPE(critter->fid),
                99,
                FID_ANIM_TYPE(critter->fid),
                (critter->fid & 0xF000) >> 12,
                (critter->fid & 0x70000000) >> 28);
            reg_anim_clear(critter);
            reg_anim_begin(ANIMATION_REQUEST_RESERVED);
            animationRegisterAnimate(critter, ANIM_UP_STAIRS_RIGHT, -1);
            animationRegisterSetFid(critter, fid, -1);
            reg_anim_end();

            while (animationIsBusy(critter)) {
                _process_bk();
            }
        }
    }

    presenter()->worldInvalidate();

    int leftItemAction;
    int rightItemAction;
    interfaceGetItemActions(&leftItemAction, &rightItemAction);
    presenter()->hudItems(true, leftItemAction, rightItemAction);

    gDude->data.critter.combat.ap = critterGetStat(gDude, STAT_MAXIMUM_ACTION_POINTS);

    presenter()->hudActionPoints(0, 0);

    if (_game_user_wants_to_quit == 0) {
        // Pay every bucket to its own earner. playerActorCount() is 1 with an
        // empty registry, so this is the vanilla single call.
        for (int slot = 0; slot < playerActorCount(); slot++) {
            _combat_give_exps(_combat_exps[slot], playerActorAt(slot));
        }
    }

    // Cleared unconditionally, and for EVERY slot — including on the
    // wants-to-quit path that skips the payout above. A bucket that survives the
    // end of a fight is XP paid twice at the end of the next one.
    for (int slot = 0; slot < kMaxPlayerActors; slot++) {
        _combat_exps[slot] = 0;
    }

    gCombatState &= ~(COMBAT_STATE_0x01 | COMBAT_STATE_0x02);
    gCombatState |= COMBAT_STATE_0x02;

    if (_list_total != 0) {
        objectListFree(_combat_list);

        if (_aiInfoList != nullptr) {
            internal_free(_aiInfoList);
        }
        _aiInfoList = nullptr;
    }

    _list_total = 0;

    _combat_ai_over();
    gameUiEnable();
    presenter()->cursorModeSet(GAME_MOUSE_MODE_MOVE);
    presenter()->hudArmorClass(true);

    if (_critter_is_prone(gDude) && !critterIsDead(gDude) && _combat_ending_guy == nullptr) {
        queueRemoveEventsByType(gDude, EVENT_TYPE_KNOCKOUT);
        knockoutEventProcess(gDude, nullptr);
    }
}

// 0x422194
void _combat_over_from_load()
{
    _combat_over();
    gCombatState = 0;
    _combat_end_due_to_load = 1;
}

// Give exp for destroying critter.
//
// 0x4221B4
void _combat_give_exps(int exp_points, Object* earner)
{
    MessageListItem v7;
    MessageListItem v9;
    int current_hp;
    int max_hp;
    char text[132];

    if (exp_points <= 0) {
        return;
    }

    // nullptr is "no subject in hand" — the vanilla meaning of this function.
    Object* subject = earner != nullptr ? earner : gDude;

    // A dead earner banks nothing. Per-actor now: one player dying used to
    // cancel the whole party's payout, because this read gDude no matter who
    // the XP belonged to.
    if (critterIsDead(subject)) {
        return;
    }

    // SFALL: Display actual xp received.
    int xpGained;
    pcAddExperience(exp_points, &xpGained, subject);

    // The award above is per-actor; the LINE below is not. It is the host's
    // console, so an extra earns silently until per-client message routing
    // exists — which also spares the host a wall of "you earn N exp" lines for
    // kills that were not theirs. Note the flavour roll is inside this gate, so
    // an extra's payout consumes no RNG (it must not: the sim is shared).
    if (subject != gDude) {
        return;
    }

    v7.num = 621; // %s you earn %d exp. points.
    if (!messageListGetItem(&gProtoMessageList, &v7)) {
        return;
    }

    v9.num = randomBetween(0, 3) + 622; // generate prefix for message

    current_hp = critterGetStat(gDude, STAT_CURRENT_HIT_POINTS);
    max_hp = critterGetStat(gDude, STAT_MAXIMUM_HIT_POINTS);
    if (current_hp == max_hp && randomBetween(0, 100) > 65) {
        v9.num = 626; // Best possible prefix: For destroying your enemies without taking a scratch,
    }

    if (!messageListGetItem(&gProtoMessageList, &v9)) {
        return;
    }

    snprintf(text, sizeof(text), v7.text, v9.text, xpGained);
    presenter()->consoleMessage(text);
}

// 0x4222A8
static void _combat_add_noncoms()
{
    _combatai_notify_friends(gDude);

    for (int index = _list_com; index < _list_com + _list_noncom; index++) {
        Object* obj = _combat_list[index];
        if (_combatai_want_to_join(obj)) {
            obj->data.critter.combat.maneuver = CRITTER_MANEUVER_NONE;

            Object** objectPtr1 = &(_combat_list[index]);
            Object** objectPtr2 = &(_combat_list[_list_com]);
            Object* t = *objectPtr1;
            *objectPtr1 = *objectPtr2;
            *objectPtr2 = t;

            _list_com += 1;
            _list_noncom -= 1;

            int actionPoints = 0;
            if (obj != gDude) {
                actionPoints = critterGetStat(obj, STAT_MAXIMUM_ACTION_POINTS);
            }

            if (_gcsd != nullptr) {
                actionPoints += _gcsd->actionPointsBonus;
            }

            obj->data.critter.combat.ap = actionPoints;

            _combat_turn(obj, false);
        }
    }
}

// Compares critters by sequence.
//
// 0x4223C8
static int _compare_faster(const void* critter1Ptr, const void* critter2Ptr)
{
    Object* critter1 = *(Object**)critter1Ptr;
    Object* critter2 = *(Object**)critter2Ptr;

    int sequence1 = critterGetStat(critter1, STAT_SEQUENCE);
    int sequence2 = critterGetStat(critter2, STAT_SEQUENCE);
    if (sequence1 > sequence2) {
        return -1;
    } else if (sequence1 < sequence2) {
        return 1;
    }

    int luck1 = critterGetStat(critter1, STAT_LUCK);
    int luck2 = critterGetStat(critter2, STAT_LUCK);
    if (luck1 > luck2) {
        return -1;
    } else if (luck1 < luck2) {
        return 1;
    }

    return 0;
}

// Initializes combat sequence for the first round.
//
// 0x42243C
static void _combat_sequence_init(Object* attacker, Object* defender)
{
    // Always place attacker first (swap with critter at 0 index).
    int next = 0;
    if (attacker != nullptr) {
        for (int index = 0; index < _list_total; index++) {
            Object* obj = _combat_list[index];
            if (obj == attacker) {
                Object* temp = _combat_list[next];
                _combat_list[index] = temp;
                _combat_list[next] = obj;
                next += 1;
                break;
            }
        }
    }

    // Place defender second.
    if (defender != nullptr) {
        for (int index = 0; index < _list_total; index++) {
            Object* obj = _combat_list[index];
            if (obj == defender) {
                Object* temp = _combat_list[next];
                _combat_list[index] = temp;
                _combat_list[next] = obj;
                next += 1;
                break;
            }
        }
    }

    // Place dude third, if he's neither attacker, nor defender.
    if (attacker != gDude && defender != gDude) {
        for (int index = 0; index < _list_total; index++) {
            Object* obj = _combat_list[index];
            if (obj == gDude) {
                Object* temp = _combat_list[next];
                _combat_list[index] = temp;
                _combat_list[next] = obj;
                next += 1;
                break;
            }
        }
    }

    // Place the remaining player actors right after the dude, so the humans form
    // ONE contiguous block at the head of the round (MP_PROPOSAL Ch 8.2 / locked
    // decision #6) instead of being scattered through the NPCs by initiative. Same
    // swap idiom as the three blocks above. Being in _list_com (the count `next`
    // feeds) is also what makes them TURN-TAKING combatants rather than bystanders.
    // Inert with a single actor: the loop body only ever sees gDude, already placed.
    for (int slot = 0; slot < playerActorCount(); slot++) {
        Object* player = playerActorAt(slot);
        if (player == nullptr || player == attacker || player == defender || player == gDude) {
            continue;
        }
        for (int index = 0; index < _list_total; index++) {
            if (_combat_list[index] == player) {
                Object* temp = _combat_list[next];
                _combat_list[index] = temp;
                _combat_list[next] = player;
                next += 1;
                break;
            }
        }
    }

    _list_com = next;
    _list_noncom -= next;

    if (attacker != nullptr) {
        _critter_set_who_hit_me(attacker, defender);
    }

    if (defender != nullptr) {
        _critter_set_who_hit_me(defender, attacker);
    }
}

// Updates combat sequence for the next round.
//
// 0x422580
static void _combat_sequence()
{
    _combat_add_noncoms();

    int count = _list_com;

    // Remove dead critters from the combatant list.
    for (int index = 0; index < count; index++) {
        Object* critter = _combat_list[index];
        if ((critter->data.critter.combat.results & DAM_DEAD) != 0) {
            _combat_list[index] = _combat_list[count - 1];
            _combat_list[count - 1] = critter;

            _combat_list[count - 1] = _combat_list[_list_noncom + count - 1];
            _combat_list[_list_noncom + count - 1] = critter;

            index -= 1;
            count -= 1;
        }
    }

    // Move knocked out and disengaged critters to non-combatant list. Vanilla
    // exempts the dude (a KO'd player keeps its roster slot and burns the turn at
    // BEGIN instead of vanishing from the round); every player actor gets that same
    // exemption, or an extra would drop out of the fight the first time it is
    // knocked down. Identical to `critter != gDude` with one actor.
    for (int index = 0; index < count; index++) {
        Object* critter = _combat_list[index];
        if (!playerActorIs(critter)) {
            if ((critter->data.critter.combat.results & DAM_KNOCKED_OUT) != 0
                || critter->data.critter.combat.maneuver == CRITTER_MANEUVER_DISENGAGING) {
                critter->data.critter.combat.maneuver &= ~CRITTER_MANEUVER_ENGAGING;
                _list_noncom += 1;

                _combat_list[index] = _combat_list[count - 1];
                _combat_list[count - 1] = critter;

                count -= 1;
                index -= 1;
            }
        }
    }

    // Sort combatant list based on Sequence stat.
    if (count != 0) {
        _list_com = count;
        qsort(_combat_list, count, sizeof(*_combat_list), _compare_faster);
        count = _list_com;
    }

    // Re-form the contiguous player block the round-1 placement established
    // (MP_PROPOSAL Ch 8.2): the Sequence sort above is initiative-only and would
    // interleave the humans with NPCs from round 2 on. Players share the premade's
    // Sequence stat, so their relative order under _compare_faster is a qsort tie =
    // implementation-defined; ordering them by SLOT makes the round deterministic
    // and gives the barrier one contiguous input window instead of several.
    // Gated on N>1 so the single-player round order is untouched, byte for byte.
    if (count != 0 && playerActorCount() > 1) {
        int head = 0;
        for (int slot = 0; slot < playerActorCount(); slot++) {
            Object* player = playerActorAt(slot);
            if (player == nullptr) {
                continue;
            }
            for (int index = head; index < count; index++) {
                if (_combat_list[index] == player) {
                    // Rotate it up to `head`, shifting the critters it passes down
                    // one slot. A rotation and not a swap: swapping would fling one
                    // NPC to the far end and scramble the initiative order the sort
                    // just established for everyone else.
                    memmove(&_combat_list[head + 1], &_combat_list[head],
                        sizeof(*_combat_list) * (index - head));
                    _combat_list[head] = player;
                    head += 1;
                    break;
                }
            }
        }
    }

    _list_com = count;

    gameTimeAddSeconds(5);
}

// 0x422694
void combatAttemptEnd()
{
    if (_combat_elev == gDude->elevation) {
        MessageListItem messageListItem;
        int dudeTeam = gDude->data.critter.combat.team;

        for (int index = 0; index < _list_com; index++) {
            Object* critter = _combat_list[index];
            if (critter != gDude) {
                int critterTeam = critter->data.critter.combat.team;
                Object* critterWhoHitMe = critter->data.critter.combat.whoHitMe;
                if (critterTeam != dudeTeam || (critterWhoHitMe != nullptr && critterWhoHitMe->data.critter.combat.team == critterTeam)) {
                    if (!_combatai_want_to_stop(critter)) {
                        messageListItem.num = 103;
                        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                            presenter()->consoleMessage(messageListItem.text);
                        }
                        return;
                    }
                }
            }
        }

        for (int index = _list_com; index < _list_com + _list_noncom; index++) {
            Object* critter = _combat_list[index];
            if (critter != gDude) {
                int critterTeam = critter->data.critter.combat.team;
                Object* critterWhoHitMe = critter->data.critter.combat.whoHitMe;
                if (critterTeam != dudeTeam || (critterWhoHitMe != nullptr && critterWhoHitMe->data.critter.combat.team == critterTeam)) {
                    if (_combatai_want_to_join(critter)) {
                        messageListItem.num = 103;
                        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
                            presenter()->consoleMessage(messageListItem.text);
                        }
                        return;
                    }
                }
            }
        }
    }

    gCombatState |= COMBAT_STATE_0x08;
    _caiTeamCombatExit();
}

// 0x4227F4
// Ledger H-12: the player-turn end conditions — turn-end authority belongs to
// the sim, not the input pump. True = stop pumping input (forced combat end,
// incapacitated dude, quit, pending load).
bool combatPlayerTurnShouldBreak()
{
    if ((gCombatState & COMBAT_STATE_0x08) != 0) {
        return true;
    }

    if ((gDude->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) != 0) {
        return true;
    }

    if (_game_user_wants_to_quit != 0) {
        return true;
    }

    if (_combat_end_due_to_load != 0) {
        return true;
    }

    return false;
}

// Ledger H-12: the AP-exhaustion turn-end rule.
bool combatPlayerTurnOutOfAp()
{
    return gDude->data.critter.combat.ap <= 0 && _combat_free_move <= 0;
}

// Ledger H-12: resolve the player's turn once the pump stopped — consumes the
// COMBAT_STATE_0x08 end-combat handshake set by combatAttemptEnd. Returns -1
// when the player's combat participation is over, 0 when only the turn ended.
int combatPlayerTurnResolve()
{
    int v4 = _game_user_wants_to_quit;
    if (_game_user_wants_to_quit == 1) {
        _game_user_wants_to_quit = 0;
    }

    if ((gCombatState & COMBAT_STATE_0x08) != 0) {
        gCombatState &= ~COMBAT_STATE_0x08;
        return -1;
    }

    if (_game_user_wants_to_quit != 0 || v4 != 0 || _combat_end_due_to_load != 0) {
        return -1;
    }

    _scripts_check_state_in_combat();

    return 0;
}

// 0x422914
static void _combat_set_move_all()
{
    for (int index = 0; index < _list_com; index++) {
        Object* object = _combat_list[index];

        int actionPoints = critterGetStat(object, STAT_MAXIMUM_ACTION_POINTS);

        if (_gcsd) {
            actionPoints += _gcsd->actionPointsBonus;
        }

        object->data.critter.combat.ap = actionPoints;

        // NOTE: Uninline.
        _combatAIInfoSetLastMove(object, 0);
    }
}

// 0x42299C
static int _combat_turn(Object* obj, bool a2)
{
    _combat_turn_obj = obj;

    // MP_PROTOCOL.md §2: this combatant's turn begins. AP was reset for the round
    // by _combat_set_move_all; deadline stubbed 0 (no turn timer in v1). No-op
    // under null/client (byte-identical). See Presenter::turnStart.
    presenter()->turnStart(obj, playerActorIs(obj), obj->data.critter.combat.ap, 0);

    attackInit(&_main_ctd, obj, nullptr, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);

    if ((obj->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) != 0) {
        obj->data.critter.combat.results &= ~DAM_LOSE_TURN;
    } else {
        // Free move belongs to the actor whose turn is STARTING, not to the host:
        // this runs once per player actor's turn, and _combat_free_move is spent
        // by that actor. Reading the host's Bonus Move handed every extra the
        // host's free tiles (or none, when the host had the perk and the extra
        // did not). playerActorIs IS `obj == gDude` with an empty registry.
        if (playerActorIs(obj)) {
            _combat_free_move = 2 * perkGetRank(obj, PERK_BONUS_MOVE);
        }

        if (obj == gDude) {
            keyboardReset();
            presenter()->hudArmorClass(true);
            presenter()->hudActionPoints(gDude->data.critter.combat.ap, _combat_free_move);
        } else {
            soundContinueAll();
        }

        bool scriptOverrides = false;
        if (obj->sid != -1) {
            scriptSetObjects(obj->sid, nullptr, nullptr);
            scriptSetFixedParam(obj->sid, 4);
            scriptExecProc(obj->sid, SCRIPT_PROC_COMBAT);

            Script* scr;
            if (scriptGetScript(obj->sid, &scr) != -1) {
                scriptOverrides = scr->scriptOverrides;
            }

            if (_game_user_wants_to_quit == 1) {
                return -1;
            }
        }

        if (!scriptOverrides) {
            if (!a2 && _critter_is_prone(obj)) {
                _combat_standup(obj);
            }

            if (obj == gDude) {
                gameUiEnable();
                presenter()->cursorRefresh();

                if (_gcsd != nullptr) {
                    _combat_attack_this(_gcsd->defender);
                }

                if (!a2) {
                    gCombatState |= 0x02;
                }

                presenter()->hudEndButtonsGreen();

                // NOTE: Uninline.
                _combat_update_critters_in_los(false);

                if (_combat_highlight != 0) {
                    _combat_outline_on();
                }

                if (_combat_input() == -1) {
                    gameUiDisable(1);
                    presenter()->cursorSet(MOUSE_CURSOR_WAIT_WATCH);
                    obj->data.critter.combat.damageLastTurn = 0;
                    presenter()->hudEndButtonsRed();
                    _combat_outline_off();
                    presenter()->hudActionPoints(-1, -1);
                    presenter()->hudArmorClass(true);
                    _combat_free_move = 0;
                    return -1;
                }
            } else {
                Rect rect;
                if (objectEnableOutline(obj, &rect) == 0) {
                    presenter()->worldInvalidateRect(&rect, obj->elevation);
                }

                _combat_ai(obj, _gcsd != nullptr ? _gcsd->defender : nullptr);
            }
        }

        // NOTE: Uninline.
        _combat_turn_run();

        if (obj == gDude) {
            gameUiDisable(1);
            presenter()->cursorSet(MOUSE_CURSOR_WAIT_WATCH);
            presenter()->hudEndButtonsRed();
            _combat_outline_off();
            presenter()->hudActionPoints(-1, -1);
            _combat_turn_obj = nullptr;
            presenter()->hudArmorClass(true);
            _combat_turn_obj = gDude;
        } else {
            Rect rect;
            if (objectDisableOutline(obj, &rect) == 0) {
                presenter()->worldInvalidateRect(&rect, obj->elevation);
            }
        }
    }

    // -1 here ENDS THE FIGHT (combatSessionAfterTurn routes it straight to kEnding),
    // so with N players this vanilla "the player died -> end combat" check has to be
    // read as "ALL players are down". It runs at the tail of EVERY _combat_turn — AI
    // turns included — which is the resumable-session path an extra's fight flows
    // through, so a raw gDude test tore the fight down the instant any enemy acted
    // after the host died, even with other players still standing (owner-observed
    // 2026-07: P1 dead, P2 provoking, combat ends anyway). Mirrors the ALREADY-
    // generalized player-turn epilogue in combatSessionDudeFinalize — this is its
    // AI-turn sibling, the half that commit "any living player keeps combat alive"
    // missed. Single-actor byte-identical: the DAM_DEAD guard is vanilla's exact test
    // and "any player alive" IS "gDude alive" with one registered actor.
    if ((gDude->data.critter.combat.results & DAM_DEAD) != 0) {
        return playerActorAnyAlive() ? 0 : -1;
    }

    if (obj != gDude || _combat_elev == gDude->elevation) {
        _combat_free_move = 0;
        return 0;
    }

    return -1;
}

// 0x422C60
static bool _combat_should_end()
{
    if (_list_com <= 1) {
        return true;
    }

    // Co-op: ANY living player actor keeps combat alive — not gDude specifically.
    // Vanilla searched for gDude (the one player) and ended combat when it was no longer
    // in the active list. With N bodies sharing one character sheet, the host/gDude dying
    // drops it from _list_com and ended the fight for every OTHER player still standing
    // (owner-observed 2026-07: P1 dead, combat ends the moment P2 ends/skips its turn).
    // Search for any player actor instead; gDude stays in the OR so single-player is
    // byte-identical (SP: playerActorCount()==1 and the sole player actor IS gDude, so
    // this finds it in exactly the same slot — goldens unchanged). The deeper death/
    // respawn + all-players-down handling remains its own track (co-op elephants).
    int index;
    for (index = 0; index < _list_com; index++) {
        if (_combat_list[index] == gDude || playerActorIs(_combat_list[index])) {
            break;
        }
    }

    if (index == _list_com) {
        return true;
    }

    int team = gDude->data.critter.combat.team;

    for (index = 0; index < _list_com; index++) {
        Object* critter = _combat_list[index];
        if (critter->data.critter.combat.team != team) {
            break;
        }

        Object* critterWhoHitMe = critter->data.critter.combat.whoHitMe;
        if (critterWhoHitMe != nullptr && critterWhoHitMe->data.critter.combat.team == team) {
            break;
        }
    }

    if (index == _list_com) {
        return true;
    }

    return false;
}

// 0x422D2C
// SERVER-ONLY combat deadlock watchdog. The headless server auto-ends the dude's
// turn (combat_drain.cc: an empty intent queue resolves the turn immediately), so a
// fight with no human to land a killing blow rests entirely on the AI. On the two
// Monty-Python special-encounter maps (rndholy2 / rnduvilg) three+ mutually-hostile
// teams close distance but never kill, and _combat_should_end() (which fires only on
// a death) can never trip — _combat() spins forever. This fingerprint (roster size +
// total combatant HP) lets the loop detect "no state change for N turns" and break
// cleanly into _combat_over(). Purely a server guardrail: gated on serverLoopActive()
// so the client and every golden stay byte-identical. Not the real fix — the resumable
// per-tick combat turn (SERVER_LOOP_DESIGN.md, OPEN RISK a) is.
static const int kServerCombatStuckTurnLimit = 300;

static unsigned int _combat_server_progress_fingerprint()
{
    unsigned int fp = (unsigned int)_list_com;
    for (int i = 0; i < _list_com; i++) {
        // +bias keeps negative (dying/dead) HP contributing distinctly.
        fp = fp * 131u + (unsigned int)(critterGetHitPoints(_combat_list[i]) + 100000);
    }
    return fp;
}

// Resumable-combat seam (SERVER_LOOP_DESIGN.md OPEN RISK a). The body of
// _combat()'s round loop is broken into named per-beat steps so a later
// server-side state machine can drive them one beat at a time. P1 is a pure
// mechanical lift — behavior is byte-identical to the original inline loop.
//
// CombatRound carries the state that used to be stack-local across the round
// do-while: the roster cursor plus the server deadlock-watchdog trio.
//
// P2 NOTE: _gcsd remains a raw pointer to the caller's stack CombatStartData
// (lifetime unchanged in P1). A tick-resumable turn cannot hold that pointer
// across server ticks — P2 must store a by-value copy of *csd here and point
// _gcsd at it.
struct CombatRound {
    // Whether combat was already active on entry (save loaded mid-combat).
    bool wasInCombat;
    // Cursor into _combat_list; -1 means the forced dude turn ended combat.
    int curIndex;
    // Server watchdog: previous progress fingerprint / no-progress turn count /
    // whether a fingerprint has been recorded yet.
    unsigned int serverStuckPrev;
    int serverStuckTurns;
    bool serverStuckInit;
};

// Outcome of running the roster's turns for one round.
enum class CombatTurnRun {
    // Every combatant in [curIndex.._list_com) took a turn.
    kAllRan,
    // A turn ended combat (_combat_turn == -1 or _combat_ending_guy set); the
    // round loop must break without advancing the sequence.
    kEndingEarly,
};

// Round-start beat: refresh every combatant's movement allowance.
static void combatRoundBegin()
{
    _combat_set_move_all();
}

// Turn beat: run each remaining combatant's turn from round.curIndex onward,
// breaking on a turn that ends combat. Clears _gcsd after the first turn (the
// initiating attack has been consumed). Returns whether combat is ending.
static CombatTurnRun combatRunTurnsFrom(CombatRound& round)
{
    for (; round.curIndex < _list_com; round.curIndex++) {
        if (_combat_turn(_combat_list[round.curIndex], false) == -1) {
            break;
        }

        if (_combat_ending_guy != nullptr) {
            break;
        }

        _gcsd = nullptr;
    }

    // The for either ran to completion (curIndex == _list_com) or broke early
    // (curIndex < _list_com), which is exactly the original round-loop break.
    return round.curIndex < _list_com ? CombatTurnRun::kEndingEarly : CombatTurnRun::kAllRan;
}

// Round-end beat: advance the combat sequence, reset the cursor, bump the turn
// counter, then run the server deadlock watchdog. Returns true if the watchdog
// forced the round loop to break.
static bool combatRoundEnd(CombatRound& round)
{
    _combat_sequence();
    round.curIndex = 0;
    _combatNumTurns += 1;

    if (serverLoopActive()) {
        unsigned int fp = _combat_server_progress_fingerprint();
        if (round.serverStuckInit && fp == round.serverStuckPrev) {
            if (++round.serverStuckTurns >= kServerCombatStuckTurnLimit) {
                debugPrint("\nSERVER: combat made no progress for %d turns — force-ending deadlocked fight.", kServerCombatStuckTurnLimit);
                return true;
            }
        } else {
            round.serverStuckTurns = 0;
        }
        round.serverStuckPrev = fp;
        round.serverStuckInit = true;
    }

    return false;
}

// Post-loop teardown beat, preserving the _combat_end_due_to_load branch.
static void combatTeardown()
{
    if (_combat_end_due_to_load) {
        gameUiEnable();
        presenter()->cursorModeSet(GAME_MOUSE_MODE_MOVE);
    } else {
        presenter()->scrollDisable();
        presenter()->hudEndButtonsHide(true);
        presenter()->scrollEnable();
        _combat_over();
        scriptsExecMapUpdateProc();
    }

    _combat_end_due_to_load = 0;

    if (_game_user_wants_to_quit == 1) {
        _game_user_wants_to_quit = 0;
    }
}

// ===========================================================================
// Resumable server-combat session machine (P2; F2_SERVER_RESUMABLE_COMBAT).
//
// With the gate ON (server loop only), _combat() sets up a session and RETURNS
// instead of draining the whole fight inside one scriptsHandleRequests call;
// serverTick then advances the session one beat at a time. AI turns run to
// completion (one AI turn per beat); the player-actor block's turn WAITS across
// beats for intents, gated by a per-action idle timer. Gate OFF, _combat() runs
// the legacy nested loop above, byte-identical (the golden suite is the proof).
//
// The machine lives here as file-static so it can drive combat.cc's statics
// (the roster _combat_list/_list_com, _gcsd, _combat_ending_guy, the dude-turn
// internals) directly — the same precedent as the server watchdog, which is
// already file-local and gated on serverLoopActive().
// ===========================================================================

// Gate: server loop active AND F2_SERVER_RESUMABLE_COMBAT=1. Env cached like
// server_anim.cc's smoothWalkEnabled(); serverLoopActive() checked live.
static bool combatResumableEnabled()
{
    static bool enabled = [] {
        const char* v = getenv("F2_SERVER_RESUMABLE_COMBAT");
        return v != nullptr && strcmp(v, "1") == 0;
    }();
    return serverLoopActive() && enabled;
}

// F2_SERVER_TURN_WAIT=1 forces the player barrier to wait even with no claimant
// and no pending intents (headless gate driver). Cached.
static bool combatResumableTurnWaitForced()
{
    static bool forced = [] {
        const char* v = getenv("F2_SERVER_TURN_WAIT");
        return v != nullptr && strcmp(v, "1") == 0;
    }();
    return forced;
}

// FLAT human input budget in sim-ms (F2_SERVER_TURN_IDLE_MS, default 60000). This is
// the time a HUMAN gets to act once their turn is actually ON SCREEN. It is NOT the whole
// deadline: the AI-animation backlog that must present before the human sees their turn
// is added on top at turn-begin (serverTakePresentationCostMs). Any interactive action
// resets the deadline back to this flat base — there is no further animation to wait on
// after a single move/attack/inventory action (COMBAT_CLIENT_DESIGN.md §6 S6).
static int combatResumableBaseIdleMs()
{
    static int budget = [] {
        const char* v = getenv("F2_SERVER_TURN_IDLE_MS");
        int ms = v != nullptr ? atoi(v) : 60000;
        return ms > 0 ? ms : 60000;
    }();
    return budget;
}

// Rough per-attack presentation estimate (sim-ms) added to the AI backlog for each AI
// swing: swing + hit reaction + (often) a death fall. Deliberately generous — the failure
// we are avoiding is timing the player out BEFORE their turn is on screen, so a slight
// over-estimate (a little extra human time) is the safe direction.
static const unsigned int kAttackPresentationEstimateMs = 1500;

enum class CombatSessionState {
    kInactive,
    kRoundBegin,
    kTurns,
    kPlayerTurn,
    kRoundEnd,
    kEnding,
};

struct CombatSession {
    bool active = false;
    CombatSessionState state = CombatSessionState::kInactive;
    CombatRound round {};
    // By-value copy of the initiating CombatStartData; _gcsd points HERE. The
    // caller's *csd is a stack/soon-freed buffer (scripts.cc memsets gScriptsCSD
    // the instant _combat returns; _combat_attack_this builds a stack CSD), which
    // a beat-spanning turn cannot hold (P1 flagged this).
    CombatStartData csd {};
    // Player-turn barrier state. ONE set of fields is enough for N players
    // (MP_PROPOSAL Ch 8.3): the roster is sequential, so exactly one player turn
    // is ever live, and every field here resets at that turn's BEGIN.
    bool playerTurnBegun = false;
    int idleBudgetMs = 0;
    unsigned int lastActionSimTs = 0; // simClockNow() of the last committed action
    // Registry slot of the player whose turn the barrier is holding. Set when
    // kTurns lands on a player actor; it is what the pump, the wait test and the
    // idle timeout mean by "the player". 0 == the host == the only value reachable
    // with a single actor.
    int playerTurnSlot = 0;
};

static CombatSession gCombatSession;

bool combatSessionActive()
{
    return gCombatSession.active;
}

void combatSessionRearmIdleTimer()
{
    // The player is demonstrably still there — they just did something the intent
    // queue never sees. Opening the inventory screen is the case this exists for:
    // it is a wire verb, not a combat intent, so it does not reach the pump, and
    // without this a player browsing their pack for longer than the idle budget
    // would have their turn force-ended out from under an open screen.
    gCombatSession.lastActionSimTs = simClockNow();
}

// Which combatants get the HUMAN barrier instead of an AI turn. THE generalized
// "obj == gDude" (MP_PROPOSAL Ch 8.3): with an empty registry playerActorIs is
// literally that comparison, so single-player behavior is unchanged, and every
// registered extra now takes its own turn instead of being played by the AI.
static bool combatSessionIsPlayerActor(Object* obj)
{
    return playerActorIs(obj);
}

// The barrier only WAITS when there is someone to wait for: THIS actor's client
// is connected, THIS actor has intents queued, or the headless force knob is set.
// Otherwise the turn ends immediately = the legacy auto-end-turn, so a resumable
// server with no client never stalls a fight.
//
// Per-slot, not "any claimant": an unbound or disconnected body must not hold the
// round open for a human who is not there (MP_PROPOSAL Ch 6.3), and one player
// must not keep another's turn alive. serverSessionForSlot reports 0 (unbound) on
// every client/probe/golden path, where there is no control plane at all — and
// with one actor slot 0 IS the old single claimant, so this is the same test it
// replaced.
static bool combatSessionShouldWait(const CombatSession& s)
{
    return serverSessionForSlot(s.playerTurnSlot) != 0
        || combatIntentPendingForSlot(s.playerTurnSlot)
        || combatResumableTurnWaitForced();
}

static bool combatSessionIdleExpired(const CombatSession& s)
{
    return simClockNow() - s.lastActionSimTs >= (unsigned int)s.idleBudgetMs;
}

// The dude-turn tail shared by every non-error player-turn exit: the [_combat_
// turn_run + HUD epilogue] block and the final dude-dead/elevation checks. runRun
// bundles _combat_turn_run + the epilogue (they always co-occur inside _combat_
// turn's else-block; the KO/lose-turn skip runs neither). CROSS-PIN: _combat_turn
// (obj==gDude) tail. The HUD/cursor presenter calls are no-ops under the null/
// network presenter (it overrides only combatEnter/Exit/turnStart/attackResult/
// console), kept verbatim for fidelity.
static int combatSessionDudeFinalize(bool runRun, Object* actor)
{
    if (runRun) {
        // NOTE: Uninline.
        _combat_turn_run();

        gameUiDisable(1);
        presenter()->cursorSet(MOUSE_CURSOR_WAIT_WATCH);
        presenter()->hudEndButtonsRed();
        _combat_outline_off();
        presenter()->hudActionPoints(-1, -1);
        _combat_turn_obj = nullptr;
        presenter()->hudArmorClass(true);
        _combat_turn_obj = actor;
    }

    // -1 here means THE FIGHT IS OVER, not "this turn is over" (the caller feeds it
    // to combatSessionAfterTurn, which goes straight to kEnding) — so with N players
    // both of vanilla's dude-shaped end conditions have to be read as questions
    // about the PLAYERS COLLECTIVELY, or one casualty would end everyone's fight
    // (MP_PROPOSAL Ch 9.4). Single-actor behavior is untouched: with one player,
    // "any player alive" IS "the dude is alive".
    if ((actor->data.critter.combat.results & DAM_DEAD) != 0) {
        return playerActorAnyAlive() ? 0 : -1;
    }

    if (_combat_elev == actor->elevation) {
        _combat_free_move = 0;
        return 0;
    }

    // Off the combat elevation. Vanilla ends the FIGHT — only the dude could ever
    // be here. For an EXTRA that should end only their own participation, so drop
    // them out of the round rather than ending everyone's fight. Nothing can
    // actually produce this for an extra yet (in-combat elevation change is not
    // wired), so the host keeps vanilla's answer rather than us inventing
    // semantics for it. Revisit with intra-map elevation follow.
    return playerActorSlotOf(actor) == 0 ? -1 : 0;
}

enum class CombatPlayerPhase {
    kPump, // proceed to the cross-beat intent pump
    kFinalizeNoRun, // KO/lose-turn skip: final checks only (no _combat_turn_run)
    kFinalizeWithRun, // scriptOverrides: _combat_turn_run + epilogue + final checks
    kEndCombat, // quit requested mid-script: turn returns -1
};

// Player-turn BEGIN — the dude branch of _combat_turn up to (not including) the
// input pump, run once when the barrier is entered. CROSS-PIN: _combat_turn
// (obj==gDude, a2==false). A faithful copy, NOT shared, so the legacy monolith
// stays byte-identical; the one deliberate change is turnStart's deadlineMs =
// the idle budget (MP_PROTOCOL §7d: the reserved slot now has its producer).
static CombatPlayerPhase combatSessionPlayerTurnBegin(CombatSession& s, Object* obj)
{
    _combat_turn_obj = obj;

    // isPlayer=true is right for every actor here by construction — the barrier is
    // only entered for registry members. Each viewer decides "is it MY turn" by
    // matching the netId against its own actor, so the other players' clients read
    // this as "someone else is acting" and lock their input.
    presenter()->turnStart(obj, true, obj->data.critter.combat.ap, s.idleBudgetMs);

    attackInit(&_main_ctd, obj, nullptr, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);

    if ((obj->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) != 0) {
        obj->data.critter.combat.results &= ~DAM_LOSE_TURN;
        return CombatPlayerPhase::kFinalizeNoRun;
    }

    keyboardReset();
    presenter()->hudArmorClass(true);
    _combat_free_move = 2 * perkGetRank(obj, PERK_BONUS_MOVE);
    presenter()->hudActionPoints(obj->data.critter.combat.ap, _combat_free_move);

    bool scriptOverrides = false;
    if (obj->sid != -1) {
        scriptSetObjects(obj->sid, nullptr, nullptr);
        scriptSetFixedParam(obj->sid, 4);
        scriptExecProc(obj->sid, SCRIPT_PROC_COMBAT);

        Script* scr;
        if (scriptGetScript(obj->sid, &scr) != -1) {
            scriptOverrides = scr->scriptOverrides;
        }

        if (_game_user_wants_to_quit == 1) {
            return CombatPlayerPhase::kEndCombat;
        }
    }

    if (scriptOverrides) {
        return CombatPlayerPhase::kFinalizeWithRun;
    }

    if (_critter_is_prone(obj)) {
        _combat_standup(obj);
    }

    gameUiEnable();
    presenter()->cursorRefresh();

    // The initiating attack is replayed ONCE, by the player who threw it. Without
    // the attacker test it fires on whichever player's turn comes first — so an
    // extra could start the round by swinging at the host's chosen target for free.
    // s.csd is the by-value copy _gcsd points at, so csd.attacker is stable across
    // beats (MP_PROPOSAL Ch 8.3).
    if (_gcsd != nullptr && (s.csd.attacker == nullptr || s.csd.attacker == obj)) {
        _combat_attack_this(_gcsd->defender);
    }

    gCombatState |= 0x02;

    presenter()->hudEndButtonsGreen();

    // NOTE: Uninline.
    _combat_update_critters_in_los(false);

    if (_combat_highlight != 0) {
        _combat_outline_on();
    }

    // _combat_input holds kPlayerTurn for the pump's duration (ScopedGameMode in
    // combat_drain.cc); the session's pump spans beats, so enter it here and exit
    // it when the pump resolves. Mirrors the legacy scope exactly.
    GameMode::enterGameMode(GameMode::kPlayerTurn);
    return CombatPlayerPhase::kPump;
}

// Forward decl: the roster-cursor advance shared by AI and player turns.
static void combatSessionAfterTurn(CombatSession& s, int rc);

enum class CombatPlayerBeat {
    kWait, // barrier is still waiting on the player — yield the beat
    kTurnEnded, // the player turn resolved; s.state was advanced
};

// One beat of the player-actor barrier. On first entry runs BEGIN; then each beat
// pumps this beat's queued intents (combatServerPumpIntents, shared with the
// legacy _combat_input). On a drained queue with someone to wait for and idle
// budget remaining it yields (kWait); otherwise the turn ends exactly as
// _combat_input's return path — combatPlayerTurnResolve()'s -1 = combat over.
static CombatPlayerBeat combatSessionAdvancePlayerTurn(CombatSession& s)
{
    Object* actor = playerActorAt(s.playerTurnSlot);
    if (actor == nullptr) {
        actor = gDude; // unregistered == single-player == the host
    }

    // BIND gDude TO THE ACTOR FOR THE WHOLE BEAT. Everything below reaches core
    // code that asks "is this the player?" by comparing against gDude — the perk
    // and AP reads here, the attack/move entry points in the pump, the scripts
    // SCRIPT_PROC_COMBAT runs, combatPlayerTurnShouldBreak/OutOfAp, the resolve
    // tail. Threading an actor parameter through all of that is the post-v1
    // refactor (MP_PROPOSAL Ch 7.2); the scope is what makes an extra player's
    // turn act on their own body today. It must be held per BEAT, not per turn:
    // a player turn spans beats, and nothing may leak the swap past this return.
    ServerActorScope scope(actor);

    if (!s.playerTurnBegun) {
        s.playerTurnBegun = true;
        // The deadline = flat human budget + the AI backlog the client still has to
        // animate before this turn is on screen (§6 S6). serverTakePresentationCostMs
        // reads+clears the estimate the AI turns since our last turn accumulated.
        s.idleBudgetMs = combatResumableBaseIdleMs() + (int)serverTakePresentationCostMs();
        s.lastActionSimTs = simClockNow(); // arm the idle timer

        switch (combatSessionPlayerTurnBegin(s, actor)) {
        case CombatPlayerPhase::kEndCombat:
            combatSessionAfterTurn(s, -1);
            return CombatPlayerBeat::kTurnEnded;
        case CombatPlayerPhase::kFinalizeNoRun:
            combatSessionAfterTurn(s, combatSessionDudeFinalize(false, actor));
            return CombatPlayerBeat::kTurnEnded;
        case CombatPlayerPhase::kFinalizeWithRun:
            combatSessionAfterTurn(s, combatSessionDudeFinalize(true, actor));
            return CombatPlayerBeat::kTurnEnded;
        case CombatPlayerPhase::kPump:
            break; // fall through to pump this same beat
        }
    }

    CombatPumpOutcome outcome = combatServerPumpIntents(s.playerTurnSlot);
    if (outcome.consumed > 0) {
        s.lastActionSimTs = simClockNow(); // any committed action rearms the timer
        // An interactive action has no AI-animation backlog behind it, so the deadline
        // drops back to the FLAT human budget — no headroom (§6 S6).
        s.idleBudgetMs = combatResumableBaseIdleMs();
    }

    // A drained queue means "wait for the human's next action" ONLY when the turn is
    // still live. If the turn is already forced to break — the end-combat handshake is
    // armed (combatAttemptEnd set COMBAT_STATE_0x08 this beat), the dude was knocked
    // out / lost the turn, or a quit/load is pending — we must fall through to
    // combatPlayerTurnResolve() now, exactly as legacy _combat_input does the moment
    // its pump loop breaks. Without the !combatPlayerTurnShouldBreak() guard the
    // barrier would return kWait every beat (a connected claimant makes
    // combatSessionShouldWait() always true), stranding the 0x08 handshake UNCONSUMED
    // until a second cendcombat press re-enters the pump and trips shouldBreak there —
    // the "end-combat takes two presses" bug (confirmed live: attemptEnd ARMED 0x08
    // then N beats of kWait). Resolve() consumes 0x08 and returns -1 = combat over.
    if (outcome.stop == CombatPumpStop::kQueueDrained
        && !combatPlayerTurnShouldBreak()
        && combatSessionShouldWait(s)) {
        if (!combatSessionIdleExpired(s)) {
            return CombatPlayerBeat::kWait; // someone to wait for, budget remains
        }
        fprintf(stderr, "SERVER: resumable combat — player turn idle timeout (%d ms), ending turn.\n",
            s.idleBudgetMs);
    }

    // Turn ends. combatPlayerTurnResolve() mirrors _combat_input's return: -1 =
    // the player's combat participation is over (it runs _scripts_check_state_in_
    // combat on the 0-path, kept). The -1 branch matches _combat_turn's
    // `_combat_input()==-1` epilogue (damageLastTurn/_combat_free_move reset,
    // no _combat_turn_run).
    int inputRc = combatPlayerTurnResolve();
    GameMode::exitGameMode(GameMode::kPlayerTurn); // _combat_input's scope ends here
    if (inputRc == -1) {
        actor->data.critter.combat.damageLastTurn = 0;
        _combat_outline_off(); // no-op headless (matches _combat_turn's -1 branch)
        _combat_free_move = 0;
        combatSessionAfterTurn(s, -1);
    } else {
        combatSessionAfterTurn(s, combatSessionDudeFinalize(true, actor));
    }
    return CombatPlayerBeat::kTurnEnded;
}

// After a combatant's turn (AI or player): mirror the per-turn break logic of
// combatRunTurnsFrom — rc==-1 or a set _combat_ending_guy ends the fight;
// otherwise the initiating attack is consumed (_gcsd cleared) and the roster
// cursor advances. CROSS-PIN: combatRunTurnsFrom (legacy inline round loop).
static void combatSessionAfterTurn(CombatSession& s, int rc)
{
    if (rc == -1 || _combat_ending_guy != nullptr) {
        s.state = CombatSessionState::kEnding;
        return;
    }
    _gcsd = nullptr;
    s.round.curIndex++;
    s.state = CombatSessionState::kTurns;
}

// Set up a new session: the exact _combat() pre-loop work, then leave the machine
// at kRoundBegin (or kEnding if the save-load forced dude turn already ended it).
static void combatSessionBegin(CombatStartData* csd)
{
    CombatSession& s = gCombatSession;
    s = CombatSession();
    s.active = true;
    s.idleBudgetMs = combatResumableBaseIdleMs();
    // Discard any presentation-cost residue from a prior fight so this one's first
    // player turn measures only its own AI backlog.
    serverTakePresentationCostMs();

    CombatRound& round = s.round;
    round.wasInCombat = (gCombatState & 0x01) != 0;

    _combat_begin(nullptr);
    presenter()->combatEnter(csd != nullptr ? csd->attacker : nullptr);

    if (round.wasInCombat) {
        // Save loaded mid-combat: force the dude turn synchronously (legacy
        // parity). v1 server policy is that no save happens mid-combat, so this
        // branch is not expected on the server path; kept identical.
        if (_combat_turn(gDude, true) == -1) {
            round.curIndex = -1;
        } else {
            int index;
            for (index = 0; index < _list_com; index++) {
                if (_combat_list[index] == gDude) {
                    break;
                }
            }
            round.curIndex = index + 1;
        }
        _gcsd = nullptr;
    } else {
        Object* defender = csd != nullptr ? csd->defender : nullptr;
        Object* attacker = csd != nullptr ? csd->attacker : nullptr;
        _combat_sequence_init(attacker, defender);
        if (csd != nullptr) {
            s.csd = *csd; // by-value; _gcsd must outlive the beat
            _gcsd = &s.csd;
        } else {
            _gcsd = nullptr;
        }
        round.curIndex = 0;
    }

    round.serverStuckPrev = 0;
    round.serverStuckTurns = 0;
    round.serverStuckInit = false;

    s.state = (round.curIndex == -1) ? CombatSessionState::kEnding : CombatSessionState::kRoundBegin;
}

// Advance the session one beat (serverTick, after scriptsHandleRequests). At most
// ONE AI turn runs per beat; the player barrier may yield mid-turn. Round-boundary
// bookkeeping (kRoundBegin/kRoundEnd) does not itself consume the beat — it falls
// through to the next turn so combat keeps flowing.
void combatSessionAdvance()
{
    CombatSession& s = gCombatSession;
    if (!s.active) {
        return;
    }

    for (;;) {
        switch (s.state) {
        case CombatSessionState::kRoundBegin:
            combatRoundBegin();
            s.state = CombatSessionState::kTurns;
            continue;

        case CombatSessionState::kTurns: {
            if (s.round.curIndex >= _list_com) {
                s.state = CombatSessionState::kRoundEnd;
                continue;
            }
            // Re-read the roster each beat: _combat_sequence / a turn's scripts can
            // reshuffle _combat_list, so never cache Object* across beats.
            Object* combatant = _combat_list[s.round.curIndex];
            if (combatSessionIsPlayerActor(combatant)) {
                s.state = CombatSessionState::kPlayerTurn;
                s.playerTurnBegun = false;
                // WHOSE turn — everything the barrier does past here (which intents
                // to consume, whose client to wait for, whose body acts) keys off
                // this. Re-derived from the roster each turn rather than tracked,
                // because _combat_list is re-sorted every round.
                s.playerTurnSlot = playerActorSlotOf(combatant);
                if (s.playerTurnSlot < 0) {
                    s.playerTurnSlot = 0; // cannot happen: the predicate above IS registry membership
                }
                continue; // enter the barrier this beat
            }
            // One full AI turn per beat.
            int rc = _combat_turn(combatant, false);
            combatSessionAfterTurn(s, rc);
            if (s.state == CombatSessionState::kEnding) {
                // The turn ENDED THE FIGHT (rc == -1 / _combat_ending_guy). Legacy
                // _combat() tore the fight down synchronously before returning, so
                // no caller ever observed the in-between state. Ending is not a
                // turn — do not spend a beat on it, fall through to kEnding now.
                //
                // Load-bearing, not cosmetic: a script's op_terminate_combat sets
                // _game_user_wants_to_quit = 1 (the engine's in-band "break out of
                // combat" signal, NOT a process quit — see combatTeardown, which is
                // what clears it). Deferring teardown a beat leaks that flag past
                // serverTick, where serverServe's continue-predicate reads it and
                // shuts the server down mid-fight. Live repro: ACTemVil (Temple of
                // Trials challenger) calls terminate_combat from combat_p_proc the
                // moment his HP drops to half.
                continue;
            }
            return;
        }

        case CombatSessionState::kPlayerTurn:
            if (combatSessionAdvancePlayerTurn(s) == CombatPlayerBeat::kWait) {
                return; // barrier still waiting; resume next beat
            }
            continue; // turn ended; state was advanced (kTurns or kEnding)

        case CombatSessionState::kRoundEnd:
            // combatRoundEnd runs _combat_sequence (which may itself run a joining
            // noncom's _combat_turn synchronously — left synchronous by design),
            // the numTurns bump, and the server watchdog (state persists in round).
            if (combatRoundEnd(s.round)) {
                s.state = CombatSessionState::kEnding;
                continue;
            }
            if (_combat_should_end()) {
                s.state = CombatSessionState::kEnding;
                continue;
            }
            s.state = CombatSessionState::kRoundBegin;
            continue;

        case CombatSessionState::kEnding:
            combatTeardown();
            GameMode::exitGameMode(GameMode::kCombat);
            s.active = false;
            s.state = CombatSessionState::kInactive;
            return;

        case CombatSessionState::kInactive:
        default:
            return;
        }
    }
}

void _combat(CombatStartData* csd)
{
    bool guardOpen = (csd == nullptr
        || (csd->attacker == nullptr || csd->attacker->elevation == gElevation)
        || (csd->defender == nullptr || csd->defender->elevation == gElevation));

    // Resumable server path (F2_SERVER_RESUMABLE_COMBAT): set up a session and
    // return; serverTick drives it across beats. kCombat mode is owned by the
    // session (entered here, exited at teardown), so this is intercepted BEFORE
    // the ScopedGameMode below — whose destructor would clear the bit on the
    // early return. Gate OFF, this is never taken and the legacy path below runs
    // byte-identically (ScopedGameMode still spans the whole function).
    // The N-player turn barrier lives ONLY in the resumable session below. The
    // legacy path drains a whole fight inside one call with a single hardcoded dude
    // turn, so with extras registered it would silently hand their bodies to the AI
    // — the exact bug M3 exists to fix. Fail loud rather than ship that quietly.
    if (playerActorCount() > 1 && !(guardOpen && combatResumableEnabled())) {
        fprintf(stderr, "SERVER: combat with %d player actors requires resumable combat "
                        "(F2_SERVER_RESUMABLE_COMBAT); the legacy path would AI-drive the extras.\n",
            playerActorCount());
    }

    if (guardOpen && combatResumableEnabled()) {
        if (combatSessionActive()) {
            // scriptsHandleRequests can fire SCRIPT_REQUEST_COMBAT mid-fight now
            // that fights span beats; a session is already running — ignore it.
            fprintf(stderr, "SERVER: resumable combat — ignoring re-entrant _combat() (session already active).\n");
            return;
        }
        GameMode::enterGameMode(GameMode::kCombat);
        combatSessionBegin(csd);
        return;
    }

    ScopedGameMode gm(GameMode::kCombat);

    if (guardOpen) {
        CombatRound round;
        round.wasInCombat = (gCombatState & 0x01) != 0;

        _combat_begin(nullptr);

        // MP_PROTOCOL.md §2: combat entered. _combat_begin is always called with
        // a null attacker; the real initiator lives in csd. No-op under the null/
        // client presenter (byte-identical). See Presenter::combatEnter.
        presenter()->combatEnter(csd != nullptr ? csd->attacker : nullptr);

        // If we loaded a save in combat, we need to force dude turn and then continue with the next combatant.
        if (round.wasInCombat) {
            if (_combat_turn(gDude, true) == -1) {
                round.curIndex = -1;
            } else {
                int index;
                for (index = 0; index < _list_com; index++) {
                    if (_combat_list[index] == gDude) {
                        break;
                    }
                }
                round.curIndex = index + 1;
            }
            _gcsd = nullptr;
        } else {
            Object* defender;
            Object* attacker;
            if (csd != nullptr) {
                defender = csd->defender;
                attacker = csd->attacker;
            } else {
                defender = nullptr;
                attacker = nullptr;
            }
            _combat_sequence_init(attacker, defender);
            _gcsd = csd;
            round.curIndex = 0;
        }

        round.serverStuckPrev = 0;
        round.serverStuckTurns = 0;
        round.serverStuckInit = false;

        do {
            if (round.curIndex == -1) {
                break;
            }

            combatRoundBegin();

            if (combatRunTurnsFrom(round) == CombatTurnRun::kEndingEarly) {
                break;
            }

            if (combatRoundEnd(round)) {
                break;
            }
        } while (!_combat_should_end());

        combatTeardown();
    }
}

// 0x422EC4
void attackInit(Attack* attack, Object* attacker, Object* defender, int hitMode, int hitLocation)
{
    attack->attacker = attacker;
    attack->hitMode = hitMode;
    attack->weapon = critterGetWeaponForHitMode(attacker, hitMode);
    attack->attackHitLocation = HIT_LOCATION_TORSO;
    attack->attackerDamage = 0;
    attack->attackerFlags = 0;
    attack->ammoQuantity = 0;
    attack->criticalMessageId = -1;
    attack->defender = defender;
    attack->tile = defender != nullptr ? defender->tile : -1;
    attack->defenderHitLocation = hitLocation;
    attack->defenderDamage = 0;
    attack->defenderFlags = 0;
    attack->defenderKnockback = 0;
    attack->extrasLength = 0;
    attack->oops = defender;
}

// True iff this attack's presentation is carried by the record channel (EVENT_PRES_SEQ)
// on the dedicated server — so the parallel decoder-mirror cue (EVENT_ATTACK_RESULT ->
// clientCombatAnimPlay) must NOT also present it, or the viewer double-presents (e.g. a
// rocket gib plays twice; a grenade would double every victim's hit/death anim). The
// record stream already carries the animation AND reserves the same participants
// (reserveSeqRef -> clientCombatAnimReserve, the same-beat OBJECT_DELTA leak guard that
// attackResult performs), so suppressing the cue for a recorded attack is safe.
//
// Single source of truth for the gate at BOTH sites: _combat_attack (open the record
// section) and _combat_apply_attack_results (suppress the redundant cue). Off the record
// backend / SP client this is false, so EVENT_ATTACK_RESULT is emitted exactly as before
// (goldens byte-identical). THROW folds in with the throw-arc slice (recordAttack widens).
static bool combatAttackRecorded(Object* attacker, int hitMode)
{
    (void)attacker;
    (void)hitMode;
    // Every attack anim now presents through the record channel: melee, ranged, and THROW
    // (spears/grenades). The throw arc records with a transient flight projectile while its
    // authoritative consumption rides the STATE arm (actionThrowConsumeHeadless) — see
    // _action_ranged. Off the record backend / SP client this is false (env + backend gate).
    return serverLoopActive() && presRecordEnabled();
}

// True iff an in-combat MOVE bracket should record its walk over the record channel
// (COMBAT_MOVE_RECORD_DESIGN.md). Wraps the composite reg_anim brackets (combat_ai AI
// moves + combat_drain player move) in an AMBIENT record section so the RunTo/MoveTo leaf
// records its real args, replayed as-is on the client. Off the record backend / SP / out of
// combat this is false → today's EVENT_MOVE glide path, byte-identical. !presRecordActive()
// guards against nesting inside another open section (belt-and-braces; move brackets don't
// nest today).
bool combatMoveRecorded()
{
    return serverLoopActive() && presRecordEnabled() && isInCombat() && !presRecordActive();
}

// Close an in-combat MOVE record bracket (COMBAT_MOVE_RECORD_DESIGN.md): end the ambient
// section, ship the recorded RunTo/MoveTo leaf as the actor's presSeq (opCount > 2 = the
// bracket actually recorded a move, not a bare BEGIN/END from a leaf that early-outed), then
// commit the deferred authoritative walk — so the presentation event precedes its own state
// (EVENT_MOVE / AP delta) on the wire. No-op when not recording. Pair with
// `if (recording) presRecordAmbientBegin();` before reg_anim_begin at each move bracket.
void combatMoveRecordClose(bool recording, Object* actor)
{
    if (!recording) {
        return;
    }
    presRecordAmbientEnd();
    if (presRecordOpCount() > 2) {
        presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount(), actor->netId);
    }
    presRecordCommitDeferred();
}

// 0x422F3C
int _combat_attack(Object* attacker, Object* defender, int hitMode, int hitLocation)
{
    if (attacker != gDude && hitMode == HIT_MODE_PUNCH && randomBetween(1, 4) == 1) {
        int fid = buildFid(OBJ_TYPE_CRITTER, attacker->fid & 0xFFF, ANIM_KICK_LEG, (attacker->fid & 0xF000) >> 12, (attacker->fid & 0x70000000) >> 28);
        if (artExists(fid)) {
            hitMode = HIT_MODE_KICK;
        }
    }

    attackInit(&_main_ctd, attacker, defender, hitMode, hitLocation);
    debugPrint("computing attack...\n");

    if (attackCompute(&_main_ctd) == -1) {
        return -1;
    }

    if (_gcsd != nullptr) {
        _main_ctd.defenderDamage += _gcsd->damageBonus;

        if (_main_ctd.defenderDamage < _gcsd->minDamage) {
            _main_ctd.defenderDamage = _gcsd->minDamage;
        }

        if (_main_ctd.defenderDamage > _gcsd->maxDamage) {
            _main_ctd.defenderDamage = _gcsd->maxDamage;
        }

        if (_gcsd->overrideAttackResults) {
            // FIXME: looks like a bug, two different fields are used to set
            // one field.
            _main_ctd.defenderFlags = _gcsd->attackerResults;
            _main_ctd.defenderFlags = _gcsd->targetResults;
        }
    }

    // Init false: headless has no interface bar, so interfaceGetCurrentHitMode
    // returns -1 WITHOUT writing aiming (interface.cc). Without this init the
    // dude's uncalled-shot AP cost would read an uninitialized bool under the
    // server loop. On the client the call succeeds and overwrites this.
    bool aiming = false;
    if (_main_ctd.defenderHitLocation == HIT_LOCATION_TORSO || _main_ctd.defenderHitLocation == HIT_LOCATION_UNCALLED) {
        if (attacker == gDude) {
            interfaceGetCurrentHitMode(&hitMode, &aiming);
        } else {
            aiming = false;
        }
    } else {
        aiming = true;
    }

    int actionPoints = weaponGetActionPointCost(attacker, _main_ctd.hitMode, aiming);
    debugPrint("sequencing attack...\n");

    // Under the server loop the attack is NOT animated: _action_attack builds an
    // attack/projectile animation sequence that cannot finalize headless
    // (reg_anim_end rejects it, esp. ranged), so we skip it and apply the
    // computed outcome directly below (Phase 2.4 de-coupling). AP accounting and
    // the cleanup flags are identical to the animated path.
    //
    // PRESENTATION-RECORD (attack family, MELEE + RANGED slices — PRESENTATION_RECORD_
    // REPLAY_SPEC §8): on the dedicated server with F2_SERVER_PRES_RECORD set, RUN the
    // animate composite (normally skipped) inside a record section so _action_melee /
    // _action_ranged's leaves stream as EVENT_PRES_SEQ, then apply the outcome
    // authoritatively below exactly as today (_combat_apply_attack_results —
    // ammo/AP/damage/XP all live there, OUTSIDE _action_attack, so re-enabling the animate
    // branch adds no state). NON-THROW ONLY for now: a throw attack keeps the server-skip
    // path (its build phase mutates inventory — itemRemove/itemReplace/_obj_connect — which
    // lands in a later slice). The melee/ranged build phase is register-only save for the
    // projectile transient (created NO_SAVE + destroyed after record in _action_ranged);
    // its only recorded callbacks are _show_death (tagged) and hideProjectile (HIDE_FORCED).
    // Gate off / SP client → recording is false → today's path exactly.
    if (getenv("F2_TRACE_EVENTS") != nullptr) {
        Object* h1 = critterGetItem1(attacker); // left / item1
        Object* h2 = critterGetItem2(attacker); // right / item2
        fprintf(stderr, "[satk] net=%d fid=0x%x hitMode=%d item1_pid=%d item2_pid=%d\n",
            attacker->netId, attacker->fid, _main_ctd.hitMode,
            h1 ? h1->pid : -1, h2 ? h2->pid : -1);
    }
    bool recording = combatAttackRecorded(attacker, _main_ctd.hitMode);
    if (!serverLoopActive() || recording) {
        if (recording) {
            presRecordSectionBegin();
        }
        int attackRc = _action_attack(&_main_ctd);
        if (attackRc == -1) {
            if (recording) {
                presRecordSectionAbort();
            }
            return -1;
        }
        if (recording) {
            presRecordSectionEnd();
            presenter()->presSeq(presRecordData(), presRecordSize(), presRecordOpCount(), attacker->netId);
        }
    }

    if (actionPoints > attacker->data.critter.combat.ap) {
        attacker->data.critter.combat.ap = 0;
    } else {
        attacker->data.critter.combat.ap -= actionPoints;
    }

    if (attacker == gDude) {
        presenter()->hudActionPoints(attacker->data.critter.combat.ap, _combat_free_move);
        _critter_set_who_hit_me(attacker, defender);
    }

    // SFALL
    explosionSettingsReset();

    _combat_call_display = 1;
    _combat_cleanup_enabled = 1;
    aiInfoSetLastTarget(attacker, defender);
    debugPrint("running attack...\n");

    // Server loop: no animation to complete, so apply the outcome now (ammo,
    // damage, end-combat, standup) with animated=false instead of waiting for
    // _combat_anim_finished. _combat_turn_running is never incremented, so the
    // driver's _combat_turn_run() drain is a no-op.
    if (serverLoopActive()) {
        _combat_apply_attack_results(false);
    }

    return 0;
}

// Returns tile one step closer from [attacker] to [target]
//
// 0x423104
int _combat_bullet_start(const Object* attacker, const Object* target)
{
    int rotation = tileGetRotationTo(attacker->tile, target->tile);
    return tileGetTileInDirection(attacker->tile, rotation, 1);
}

// 0x423128
static bool _check_ranged_miss(Attack* attack)
{
    int range = weaponGetRange(attack->attacker, attack->hitMode);
    int to = _tile_num_beyond(attack->attacker->tile, attack->defender->tile, range);

    int roll = ROLL_FAILURE;
    Object* critter = attack->attacker;
    if (critter != nullptr) {
        int curr = attack->attacker->tile;
        while (curr != to) {
            _make_straight_path_func(attack->attacker, curr, to, nullptr, &critter, 32, _obj_shoot_blocking_at);
            if (critter != nullptr) {
                if ((critter->flags & OBJECT_SHOOT_THRU) == 0) {
                    if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
                        roll = ROLL_SUCCESS;
                        break;
                    }

                    if (critter != attack->defender) {
                        int v6 = attackDetermineToHit(attack->attacker, attack->attacker->tile, critter, attack->defenderHitLocation, attack->hitMode, true) / 3;
                        if (critterIsDead(critter)) {
                            v6 = 5;
                        }

                        if (randomBetween(1, 100) <= v6) {
                            roll = ROLL_SUCCESS;
                            break;
                        }
                    }

                    curr = critter->tile;
                }
            }

            if (critter == nullptr) {
                break;
            }
        }
    }

    attack->defenderHitLocation = HIT_LOCATION_TORSO;

    if (roll < ROLL_SUCCESS || critter == nullptr || (critter->flags & OBJECT_SHOOT_THRU) == 0) {
        return false;
    }

    attack->defender = critter;
    attack->tile = critter->tile;
    attack->attackerFlags |= DAM_HIT;
    attack->defenderHitLocation = HIT_LOCATION_TORSO;
    attackComputeDamage(attack, 1, 2);
    return true;
}

// 0x423284
static int _shoot_along_path(Attack* attack, int endTile, int rounds, int anim)
{
    int remainingRounds = rounds;
    int roundsHitMainTarget = 0;
    int currentTile = attack->attacker->tile;

    Object* critter = attack->attacker;
    while (critter != nullptr) {
        if ((remainingRounds <= 0 && anim != ANIM_FIRE_CONTINUOUS) || currentTile == endTile || attack->extrasLength >= 6) {
            break;
        }

        _make_straight_path_func(attack->attacker, currentTile, endTile, nullptr, &critter, 32, _obj_shoot_blocking_at);

        if (critter != nullptr) {
            if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
                break;
            }

            int accuracy = attackDetermineToHit(attack->attacker, attack->attacker->tile, critter, HIT_LOCATION_TORSO, attack->hitMode, true);
            if (anim == ANIM_FIRE_CONTINUOUS) {
                remainingRounds = 1;
            }

            int roundsHit = 0;
            while (randomBetween(1, 100) <= accuracy && remainingRounds > 0) {
                remainingRounds -= 1;
                roundsHit += 1;
            }

            if (roundsHit != 0) {
                if (critter == attack->defender) {
                    roundsHitMainTarget += roundsHit;
                } else {
                    int index;
                    for (index = 0; index < attack->extrasLength; index += 1) {
                        if (critter == attack->extras[index]) {
                            break;
                        }
                    }

                    attack->extrasHitLocation[index] = HIT_LOCATION_TORSO;
                    attack->extras[index] = critter;
                    attackInit(&_shoot_ctd, attack->attacker, critter, attack->hitMode, HIT_LOCATION_TORSO);
                    _shoot_ctd.attackerFlags |= DAM_HIT;
                    attackComputeDamage(&_shoot_ctd, roundsHit, 2);

                    if (index == attack->extrasLength) {
                        attack->extrasDamage[index] = _shoot_ctd.defenderDamage;
                        attack->extrasFlags[index] = _shoot_ctd.defenderFlags;
                        attack->extrasKnockback[index] = _shoot_ctd.defenderKnockback;
                        attack->extrasLength++;
                    } else {
                        if (anim == ANIM_FIRE_BURST) {
                            attack->extrasDamage[index] += _shoot_ctd.defenderDamage;
                            attack->extrasFlags[index] |= _shoot_ctd.defenderFlags;
                            attack->extrasKnockback[index] += _shoot_ctd.defenderKnockback;
                        }
                    }
                }
            }

            currentTile = critter->tile;
        }
    }

    if (anim == ANIM_FIRE_CONTINUOUS) {
        roundsHitMainTarget = 0;
    }

    return roundsHitMainTarget;
}

// 0x423488
static int _compute_spray(Attack* attack, int accuracy, int* roundsHitMainTargetPtr, int* roundsSpentPtr, int anim)
{
    *roundsHitMainTargetPtr = 0;

    int ammoQuantity = ammoGetQuantity(attack->weapon);
    int burstRounds = weaponGetBurstRounds(attack->weapon);
    if (burstRounds < ammoQuantity) {
        ammoQuantity = burstRounds;
    }

    *roundsSpentPtr = ammoQuantity;

    int criticalChance = critterGetStat(attack->attacker, STAT_CRITICAL_CHANCE);
    int roll = randomRoll(accuracy, criticalChance, nullptr);

    if (roll == ROLL_CRITICAL_FAILURE) {
        return roll;
    }

    if (roll == ROLL_CRITICAL_SUCCESS) {
        accuracy += 20;
    }

    int leftRounds;
    int mainTargetRounds;
    int centerRounds;
    int rightRounds;
    if (anim == ANIM_FIRE_BURST) {
        // SFALL: Burst mod.
        if (gBurstModEnabled) {
            mainTargetRounds = burstModComputeRounds(ammoQuantity, &centerRounds, &leftRounds, &rightRounds);
        } else {
            centerRounds = ammoQuantity / 3;
            if (centerRounds == 0) {
                centerRounds = 1;
            }

            leftRounds = ammoQuantity / 3;
            rightRounds = ammoQuantity - centerRounds - leftRounds;
            mainTargetRounds = centerRounds / 2;
            if (mainTargetRounds == 0) {
                mainTargetRounds = 1;
                centerRounds -= 1;
            }
        }
    } else {
        leftRounds = 1;
        mainTargetRounds = 1;
        centerRounds = 1;
        rightRounds = 1;
    }

    for (int index = 0; index < mainTargetRounds; index += 1) {
        if (randomRoll(accuracy, 0, nullptr) >= ROLL_SUCCESS) {
            *roundsHitMainTargetPtr += 1;
        }
    }

    if (*roundsHitMainTargetPtr == 0 && _check_ranged_miss(attack)) {
        *roundsHitMainTargetPtr = 1;
    }

    int range = weaponGetRange(attack->attacker, attack->hitMode);
    int mainTargetEndTile = _tile_num_beyond(attack->attacker->tile, attack->defender->tile, range);
    *roundsHitMainTargetPtr += _shoot_along_path(attack, mainTargetEndTile, centerRounds - *roundsHitMainTargetPtr, anim);

    int centerTile;
    if (objectGetDistanceBetween(attack->attacker, attack->defender) <= 3) {
        centerTile = _tile_num_beyond(attack->attacker->tile, attack->defender->tile, 3);
    } else {
        centerTile = attack->defender->tile;
    }

    int rotation = tileGetRotationTo(centerTile, attack->attacker->tile);

    int leftTile = tileGetTileInDirection(centerTile, (rotation + 1) % ROTATION_COUNT, 1);
    int leftEndTile = _tile_num_beyond(attack->attacker->tile, leftTile, range);
    *roundsHitMainTargetPtr += _shoot_along_path(attack, leftEndTile, leftRounds, anim);

    int rightTile = tileGetTileInDirection(centerTile, (rotation + 5) % ROTATION_COUNT, 1);
    int rightEndTile = _tile_num_beyond(attack->attacker->tile, rightTile, range);
    *roundsHitMainTargetPtr += _shoot_along_path(attack, rightEndTile, rightRounds, anim);

    if (roll != ROLL_FAILURE || (*roundsHitMainTargetPtr <= 0 && attack->extrasLength <= 0)) {
        if (roll >= ROLL_SUCCESS && *roundsHitMainTargetPtr == 0 && attack->extrasLength == 0) {
            roll = ROLL_FAILURE;
        }
    } else {
        roll = ROLL_SUCCESS;
    }

    return roll;
}

// 0x423714
static int attackComputeEnhancedKnockout(Attack* attack)
{
    if (weaponGetPerk(attack->weapon) == PERK_WEAPON_ENHANCED_KNOCKOUT) {
        int difficulty = critterGetStat(attack->attacker, STAT_STRENGTH) - 8;
        int chance = randomBetween(1, 100);
        if (chance <= difficulty) {
            Object* weapon = nullptr;
            if (attack->defender != gDude) {
                weapon = critterGetWeaponForHitMode(attack->defender, HIT_MODE_RIGHT_WEAPON_PRIMARY);
            }

            if (!(_attackFindInvalidFlags(attack->defender, weapon) & 1)) {
                attack->defenderFlags |= DAM_KNOCKED_OUT;
            }
        }
    }

    return 0;
}

// 0x42378C
static int attackCompute(Attack* attack)
{
    int range = weaponGetRange(attack->attacker, attack->hitMode);
    int distance = objectGetDistanceBetween(attack->attacker, attack->defender);

    if (range < distance) {
        return -1;
    }

    int anim = critterGetAnimationForHitMode(attack->attacker, attack->hitMode);
    int accuracy = attackDetermineToHit(attack->attacker, attack->attacker->tile, attack->defender, attack->defenderHitLocation, attack->hitMode, true);

    bool isGrenade = false;
    int damageType = weaponGetDamageType(attack->attacker, attack->weapon);
    // SFALL
    if (anim == ANIM_THROW_ANIM && (damageType == explosionGetDamageType() || damageType == DAMAGE_TYPE_PLASMA || damageType == DAMAGE_TYPE_EMP)) {
        isGrenade = true;
    }

    if (attack->defenderHitLocation == HIT_LOCATION_UNCALLED) {
        attack->defenderHitLocation = HIT_LOCATION_TORSO;
    }

    int attackType = weaponGetAttackTypeForHitMode(attack->weapon, attack->hitMode);
    int ammoQuantity = 1;
    int damageMultiplier = 2;
    int v26 = 1;

    int roll;

    if (anim == ANIM_FIRE_BURST || anim == ANIM_FIRE_CONTINUOUS) {
        roll = _compute_spray(attack, accuracy, &ammoQuantity, &v26, anim);
    } else {
        int chance = critterGetStat(attack->attacker, STAT_CRITICAL_CHANCE);
        roll = randomRoll(accuracy, chance - hit_location_penalty[attack->defenderHitLocation], nullptr);
    }

    if (roll == ROLL_FAILURE) {
        if (traitIsSelected(TRAIT_JINXED) || perkHasRank(gDude, PERK_JINXED)) {
            if (randomBetween(0, 1) == 1) {
                roll = ROLL_CRITICAL_FAILURE;
            }
        }
    }

    if (roll == ROLL_SUCCESS) {
        if ((attackType == ATTACK_TYPE_MELEE || attackType == ATTACK_TYPE_UNARMED) && playerActorIs(attack->attacker)) {
            if (perkHasRank(attack->attacker, PERK_SLAYER)) {
                roll = ROLL_CRITICAL_SUCCESS;
            }

            // ⚠ SILENT DEATH stays HOST-ONLY, deliberately. It is gated on
            // dudeHasState(DUDE_STATE_SNEAKING), which is still a PC-GLOBAL —
            // generalizing the perk without generalizing sneak would grant an
            // extra a x4 backstab whenever the HOST happened to be sneaking.
            // Unblocks when sneak becomes per-actor (PLAYER_SHEET_DESIGN.md §8).
            if (attack->attacker == gDude
                && perkHasRank(gDude, PERK_SILENT_DEATH)
                && !_is_hit_from_front(gDude, attack->defender)
                && dudeHasState(DUDE_STATE_SNEAKING)
                && gDude != attack->defender->data.critter.combat.whoHitMe) {
                damageMultiplier = 4;
            }

            // SFALL
            int bonusCriticalChance = unarmedGetBonusCriticalChance(attack->hitMode);
            if (bonusCriticalChance != 0) {
                if (randomBetween(1, 100) <= bonusCriticalChance) {
                    roll = ROLL_CRITICAL_SUCCESS;
                }
            }
        }
    }

    if (attackType == ATTACK_TYPE_RANGED) {
        attack->ammoQuantity = v26;

        if (roll == ROLL_SUCCESS && playerActorIs(attack->attacker)) {
            if (perkGetRank(attack->attacker, PERK_SNIPER) != 0) {
                int d10 = randomBetween(1, 10);
                int luck = critterGetStat(attack->attacker, STAT_LUCK);
                if (d10 <= luck) {
                    roll = ROLL_CRITICAL_SUCCESS;
                }
            }
        }
    } else {
        if (ammoGetCapacity(attack->weapon) > 0) {
            attack->ammoQuantity = 1;
        }
    }

    if (_item_w_compute_ammo_cost(attack->weapon, &(attack->ammoQuantity)) == -1) {
        return -1;
    }

    switch (roll) {
    case ROLL_CRITICAL_SUCCESS:
        damageMultiplier = attackComputeCriticalHit(attack);

        // SFALL: Fix Silent Death bonus not being applied to critical hits.
        // Host-only for the same reason as the other Silent Death site above:
        // it is gated on the PC-global sneak state, not on a per-actor one.
        if ((attackType == ATTACK_TYPE_MELEE || attackType == ATTACK_TYPE_UNARMED) && attack->attacker == gDude) {
            if (perkHasRank(gDude, PERK_SILENT_DEATH)
                && !_is_hit_from_front(gDude, attack->defender)
                && dudeHasState(DUDE_STATE_SNEAKING)
                && gDude != attack->defender->data.critter.combat.whoHitMe) {
                damageMultiplier *= 2;
            }
        }
        // FALLTHROUGH
    case ROLL_SUCCESS:
        attack->attackerFlags |= DAM_HIT;
        attackComputeEnhancedKnockout(attack);
        attackComputeDamage(attack, ammoQuantity, damageMultiplier);
        break;
    case ROLL_FAILURE:
        if (attackType == ATTACK_TYPE_RANGED || attackType == ATTACK_TYPE_THROW) {
            _check_ranged_miss(attack);
        }
        break;
    case ROLL_CRITICAL_FAILURE:
        attackComputeCriticalFailure(attack);
        break;
    }

    if (attackType == ATTACK_TYPE_RANGED || attackType == ATTACK_TYPE_THROW) {
        if ((attack->attackerFlags & (DAM_HIT | DAM_CRITICAL)) == 0) {
            int tile;
            if (isGrenade) {
                int throwDistance = randomBetween(1, distance / 2);
                if (throwDistance == 0) {
                    throwDistance = 1;
                }

                int rotation = randomBetween(0, 5);
                tile = tileGetTileInDirection(attack->defender->tile, rotation, throwDistance);
            } else {
                tile = _tile_num_beyond(attack->attacker->tile, attack->defender->tile, range);
            }

            attack->tile = tile;

            Object* accidentalTarget = attack->defender;
            _make_straight_path_func(accidentalTarget, attack->defender->tile, attack->tile, nullptr, &accidentalTarget, 32, _obj_shoot_blocking_at);
            if (accidentalTarget != nullptr && accidentalTarget != attack->defender) {
                attack->tile = accidentalTarget->tile;
            } else {
                accidentalTarget = _obj_blocking_at(nullptr, attack->tile, attack->defender->elevation);
            }

            if (accidentalTarget != nullptr && (accidentalTarget->flags & OBJECT_SHOOT_THRU) == 0) {
                attack->attackerFlags |= DAM_HIT;
                attack->defender = accidentalTarget;
                attackComputeDamage(attack, 1, 2);
            }
        }
    }

    // SFALL
    if ((damageType == explosionGetDamageType() || isGrenade) && ((attack->attackerFlags & DAM_HIT) != 0 || (attack->attackerFlags & DAM_CRITICAL) == 0)) {
        _compute_explosion_on_extras(attack, 0, isGrenade, 0);
    } else {
        if ((attack->attackerFlags & DAM_EXPLODE) != 0) {
            _compute_explosion_on_extras(attack, 1, isGrenade, 0);
        }
    }

    attackComputeDeathFlags(attack);

    return 0;
}

// compute_explosion_on_extras
// 0x423C10
void _compute_explosion_on_extras(Attack* attack, bool isFromAttacker, bool isGrenade, bool noDamage)
{
    Object* targetObj;

    if (isFromAttacker) {
        targetObj = attack->attacker;
    } else {
        if ((attack->attackerFlags & DAM_HIT) != 0) {
            targetObj = attack->defender;
        } else {
            targetObj = nullptr;
        }
    }

    int explosionTile;
    if (targetObj != nullptr) {
        explosionTile = targetObj->tile;
    } else {
        explosionTile = attack->tile;
    }

    if (explosionTile == -1) {
        debugPrint("\nError: compute_explosion_on_extras: Called with bad target/tileNum");
        return;
    }

    int ringTileIdx;
    int radius = 0;
    int rotation = 0;
    int tile = -1;
    int ringFirstTile = explosionTile;

    // SFALL
    int maxTargets = explosionGetMaxTargets();
    // Check adjacent tiles for possible targets, going ring-by-ring
    while (attack->extrasLength < maxTargets) {
        if (radius != 0 && (tile == -1 || (tile = tileGetTileInDirection(tile, rotation, 1)) != ringFirstTile)) {
            ringTileIdx++;
            if (ringTileIdx % radius == 0) { // the larger the radius, the slower we rotate
                rotation += 1;
                if (rotation == ROTATION_COUNT) {
                    rotation = ROTATION_NE;
                }
            }
        } else {
            radius++; // go to the next ring
            if (isGrenade && weaponGetGrenadeExplosionRadius(attack->weapon) < radius) {
                tile = -1;
            } else if (isGrenade || weaponGetRocketExplosionRadius(attack->weapon) >= radius) {
                tile = tileGetTileInDirection(ringFirstTile, ROTATION_NE, 1);
            } else {
                tile = -1;
            }

            ringFirstTile = tile;
            rotation = ROTATION_SE;
            ringTileIdx = 0;
        }

        if (tile == -1) {
            break;
        }

        Object* obstacle = _obj_blocking_at(targetObj, tile, attack->attacker->elevation);
        if (obstacle != nullptr
            && FID_TYPE(obstacle->fid) == OBJ_TYPE_CRITTER
            && (obstacle->data.critter.combat.results & DAM_DEAD) == 0
            && (obstacle->flags & OBJECT_SHOOT_THRU) == 0
            && !_combat_is_shot_blocked(obstacle, obstacle->tile, explosionTile, nullptr, nullptr)) {
            if (obstacle == attack->attacker) {
                attack->attackerFlags &= ~DAM_HIT;
                attackComputeDamage(attack, 1, 2);
                attack->attackerFlags |= DAM_HIT;
                attack->attackerFlags |= DAM_BACKWASH;
            } else {
                int index;
                for (index = 0; index < attack->extrasLength; index++) {
                    if (attack->extras[index] == obstacle) {
                        break;
                    }
                }

                if (index == attack->extrasLength) {
                    attack->extrasHitLocation[index] = HIT_LOCATION_TORSO;
                    attack->extras[index] = obstacle;
                    attackInit(&_explosion_ctd, attack->attacker, obstacle, attack->hitMode, HIT_LOCATION_TORSO);
                    if (!noDamage) {
                        _explosion_ctd.attackerFlags |= DAM_HIT;
                        attackComputeDamage(&_explosion_ctd, 1, 2);
                    }

                    attack->extrasDamage[index] = _explosion_ctd.defenderDamage;
                    attack->extrasFlags[index] = _explosion_ctd.defenderFlags;
                    attack->extrasKnockback[index] = _explosion_ctd.defenderKnockback;
                    attack->extrasLength += 1;
                }
            }
        }
    }
}

// 0x423EB4
static int attackComputeCriticalHit(Attack* attack)
{
    Object* defender = attack->defender;
    if (defender != nullptr && _critter_flag_check(defender->pid, CRITTER_INVULNERABLE)) {
        return 2;
    }

    if (defender != nullptr && PID_TYPE(defender->pid) != OBJ_TYPE_CRITTER) {
        return 2;
    }

    attack->attackerFlags |= DAM_CRITICAL;

    int chance = randomBetween(1, 100);

    chance += critterGetStat(attack->attacker, STAT_BETTER_CRITICALS);

    int effect;
    if (chance <= 20)
        effect = 0;
    else if (chance <= 45)
        effect = 1;
    else if (chance <= 70)
        effect = 2;
    else if (chance <= 90)
        effect = 3;
    else if (chance <= 100)
        effect = 4;
    else
        effect = 5;

    CriticalHitDescription* criticalHitDescription;
    if (defender == gDude) {
        criticalHitDescription = &(gPlayerCriticalHitTable[attack->defenderHitLocation][effect]);
    } else {
        int killType = critterGetKillType(defender);
        criticalHitDescription = &(gCriticalHitTables[killType][attack->defenderHitLocation][effect]);
    }

    attack->defenderFlags |= criticalHitDescription->flags;

    // NOTE: Original code is slightly different, it does not set message in
    // advance, instead using "else" statement.
    attack->criticalMessageId = criticalHitDescription->messageId;

    if (criticalHitDescription->massiveCriticalStat != -1) {
        if (statRoll(defender, criticalHitDescription->massiveCriticalStat, criticalHitDescription->massiveCriticalStatModifier, nullptr) <= ROLL_FAILURE) {
            attack->defenderFlags |= criticalHitDescription->massiveCriticalFlags;
            attack->criticalMessageId = criticalHitDescription->massiveCriticalMessageId;
        }
    }

    if ((attack->defenderFlags & DAM_CRIP_RANDOM) != 0) {
        // NOTE: Uninline.
        _do_random_cripple(&(attack->defenderFlags));
    }

    if (weaponGetPerk(attack->weapon) == PERK_WEAPON_ENHANCED_KNOCKOUT) {
        attack->defenderFlags |= DAM_KNOCKED_OUT;
    }

    Object* weapon = nullptr;
    if (defender != gDude) {
        weapon = critterGetWeaponForHitMode(defender, HIT_MODE_RIGHT_WEAPON_PRIMARY);
    }

    int flags = _attackFindInvalidFlags(defender, weapon);
    attack->defenderFlags &= ~flags;

    return criticalHitDescription->damageMultiplier;
}

// 0x424088
static int _attackFindInvalidFlags(Object* critter, Object* item)
{
    int flags = 0;

    if (critter != nullptr && PID_TYPE(critter->pid) == OBJ_TYPE_CRITTER && _critter_flag_check(critter->pid, CRITTER_NO_DROP)) {
        flags |= DAM_DROP;
    }

    if (item != nullptr && itemIsHidden(item)) {
        flags |= DAM_DROP;
    }

    return flags;
}

// 0x4240DC
static int attackComputeCriticalFailure(Attack* attack)
{
    attack->attackerFlags &= ~DAM_HIT;

    if (attack->attacker != nullptr && _critter_flag_check(attack->attacker->pid, CRITTER_INVULNERABLE)) {
        return 0;
    }

    if (attack->attacker == gDude) {
        // SFALL: Remove criticals time limits.
        bool criticalsTimeLimitsRemoved = false;
        configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_REMOVE_CRITICALS_TIME_LIMITS_KEY, &criticalsTimeLimitsRemoved);

        unsigned int gameTime = gameTimeGetTime();
        if (!criticalsTimeLimitsRemoved && gameTime / GAME_TIME_TICKS_PER_DAY < 6) {
            return 0;
        }
    }

    int attackType = weaponGetAttackTypeForHitMode(attack->weapon, attack->hitMode);
    int criticalFailureTableIndex = weaponGetCriticalFailureType(attack->weapon);
    if (criticalFailureTableIndex == -1) {
        criticalFailureTableIndex = 0;
    }

    int chance = randomBetween(1, 100) - 5 * (critterGetStat(attack->attacker, STAT_LUCK) - 5);

    int effect;
    if (chance <= 20)
        effect = 0;
    else if (chance <= 50)
        effect = 1;
    else if (chance <= 75)
        effect = 2;
    else if (chance <= 95)
        effect = 3;
    else
        effect = 4;

    int flags = _cf_table[criticalFailureTableIndex][effect];
    if (flags == 0) {
        return 0;
    }

    attack->attackerFlags |= DAM_CRITICAL;
    attack->attackerFlags |= flags;

    int v17 = _attackFindInvalidFlags(attack->attacker, attack->weapon);
    attack->attackerFlags &= ~v17;

    if ((attack->attackerFlags & DAM_HIT_SELF) != 0) {
        int ammoQuantity = attackType == ATTACK_TYPE_RANGED ? attack->ammoQuantity : 1;
        attackComputeDamage(attack, ammoQuantity, 2);
    } else if ((attack->attackerFlags & DAM_EXPLODE) != 0) {
        attackComputeDamage(attack, 1, 2);
    }

    if ((attack->attackerFlags & DAM_LOSE_TURN) != 0) {
        attack->attacker->data.critter.combat.ap = 0;
    }

    if ((attack->attackerFlags & DAM_LOSE_AMMO) != 0) {
        if (attackType == ATTACK_TYPE_RANGED) {
            attack->ammoQuantity = ammoGetQuantity(attack->weapon);
        } else {
            attack->attackerFlags &= ~DAM_LOSE_AMMO;
        }
    }

    if ((attack->attackerFlags & DAM_CRIP_RANDOM) != 0) {
        // NOTE: Uninline.
        _do_random_cripple(&(attack->attackerFlags));
    }

    if ((attack->attackerFlags & DAM_RANDOM_HIT) != 0) {
        attack->defender = _combat_ai_random_target(attack);
        if (attack->defender != nullptr) {
            attack->attackerFlags |= DAM_HIT;
            attack->defenderHitLocation = HIT_LOCATION_TORSO;
            attack->attackerFlags &= ~DAM_CRITICAL;

            int ammoQuantity = attackType == ATTACK_TYPE_RANGED ? attack->ammoQuantity : 1;
            attackComputeDamage(attack, ammoQuantity, 2);
        } else {
            attack->defender = attack->oops;
        }

        if (attack->defender != nullptr) {
            attack->tile = attack->defender->tile;
        }
    }

    return 0;
}

// 0x42432C
static void _do_random_cripple(int* flagsPtr)
{
    *flagsPtr &= ~DAM_CRIP_RANDOM;

    switch (randomBetween(0, 3)) {
    case 0:
        *flagsPtr |= DAM_CRIP_LEG_LEFT;
        break;
    case 1:
        *flagsPtr |= DAM_CRIP_LEG_RIGHT;
        break;
    case 2:
        *flagsPtr |= DAM_CRIP_ARM_LEFT;
        break;
    case 3:
        *flagsPtr |= DAM_CRIP_ARM_RIGHT;
        break;
    }
}

// 0x42436C
int _determine_to_hit(Object* attacker, Object* defender, int hitLocation, int hitMode)
{
    return attackDetermineToHit(attacker, attacker->tile, defender, hitLocation, hitMode, true);
}

// 0x424380
int _determine_to_hit_no_range(Object* attacker, Object* defender, int hitLocation, int hitMode, unsigned char* a5)
{
    return attackDetermineToHit(attacker, attacker->tile, defender, hitLocation, hitMode, false);
}

// 0x424394
int _determine_to_hit_from_tile(Object* attacker, int tile, Object* defender, int hitLocation, int hitMode)
{
    return attackDetermineToHit(attacker, tile, defender, hitLocation, hitMode, true);
}

// determine_to_hit
// 0x4243A8
static int attackDetermineToHit(Object* attacker, int tile, Object* defender, int hitLocation, int hitMode, bool useDistance)
{
    Object* weapon = critterGetWeaponForHitMode(attacker, hitMode);

    bool targetIsCritter = defender != nullptr
        ? FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER
        : false;

    bool isRangedWeapon = false;

    int toHit;
    if (weapon == nullptr || isUnarmedHitMode(hitMode)) {
        toHit = skillGetValue(attacker, SKILL_UNARMED);
    } else {
        toHit = weaponGetSkillValue(attacker, hitMode);

        int attackType = weaponGetAttackTypeForHitMode(weapon, hitMode);
        if (attackType == ATTACK_TYPE_RANGED || attackType == ATTACK_TYPE_THROW) {
            isRangedWeapon = true;

            int perceptionBonusMult = 0;
            int minEffectiveDist = 0;

            int weaponPerk = weaponGetPerk(weapon);
            switch (weaponPerk) {
            case PERK_WEAPON_LONG_RANGE:
                perceptionBonusMult = 4;
                break;
            case PERK_WEAPON_SCOPE_RANGE:
                perceptionBonusMult = 5;
                minEffectiveDist = 8;
                break;
            default:
                perceptionBonusMult = 2;
                break;
            }

            int perception = critterGetStat(attacker, STAT_PERCEPTION);

            // SFALL: Fix Sharpshooter.
            if (playerActorIs(attacker)) {
                perception += 2 * perkGetRank(attacker, PERK_SHARPSHOOTER);
            }

            int distanceMod = 0;
            // SFALL: Fix for `determine_to_hit_func` function taking distance
            // into account when called from `determine_to_hit_no_range`.
            if (defender != nullptr && useDistance) {
                distanceMod = objectGetDistanceBetweenTiles(attacker, tile, defender, defender->tile);
            } else {
                distanceMod = 0;
            }

            if (distanceMod >= minEffectiveDist) {
                int perceptionBonus = attacker == gDude
                    ? perceptionBonusMult * (perception - 2)
                    : perceptionBonusMult * perception;

                distanceMod -= perceptionBonus;
            } else {
                distanceMod += minEffectiveDist;
            }

            if (distanceMod < -2 * perception) {
                distanceMod = -2 * perception;
            }

            if (distanceMod >= 0) {
                if ((attacker->data.critter.combat.results & DAM_BLIND) != 0) {
                    distanceMod *= -12;
                } else {
                    distanceMod *= -4;
                }
            } else {
                distanceMod *= -4;
            }

            if (useDistance || distanceMod > 0) {
                toHit += distanceMod;
            }

            int numCrittersInLof = 0;

            if (defender != nullptr && useDistance) {
                _combat_is_shot_blocked(attacker, tile, defender->tile, defender, &numCrittersInLof);
            }

            toHit -= 10 * numCrittersInLof;
        }

        if (playerActorIs(attacker) && traitIsSelected(TRAIT_ONE_HANDER, attacker)) {
            if (weaponIsTwoHanded(weapon)) {
                toHit -= 40;
            } else {
                toHit += 20;
            }
        }

        int minStrength = weaponGetMinStrengthRequired(weapon);
        int minStrengthMod = minStrength - critterGetStat(attacker, STAT_STRENGTH);
        if (playerActorIs(attacker) && perkGetRank(attacker, PERK_WEAPON_HANDLING) != 0) {
            minStrengthMod -= 3;
        }

        if (minStrengthMod > 0) {
            toHit -= 20 * minStrengthMod;
        }

        if (weaponGetPerk(weapon) == PERK_WEAPON_ACCURATE) {
            toHit += 20;
        }
    }

    if (targetIsCritter && defender != nullptr) {
        int armorClass = critterGetStat(defender, STAT_ARMOR_CLASS);
        armorClass += weaponGetAmmoArmorClassModifier(weapon);
        if (armorClass < 0) {
            armorClass = 0;
        }

        toHit -= armorClass;
    }

    if (isRangedWeapon) {
        toHit += hit_location_penalty[hitLocation];
    } else {
        toHit += hit_location_penalty[hitLocation] / 2;
    }

    if (defender != nullptr && (defender->flags & OBJECT_MULTIHEX) != 0) {
        toHit += 15;
    }

    if (attacker == gDude) {
        int lightIntensity;
        if (defender != nullptr) {
            lightIntensity = objectGetLightIntensity(defender);
            if (weaponGetPerk(weapon) == PERK_WEAPON_NIGHT_SIGHT) {
                lightIntensity = 65536;
            }
        } else {
            lightIntensity = 0;
        }

        if (lightIntensity <= 26214)
            toHit -= 40;
        else if (lightIntensity <= 39321)
            toHit -= 25;
        else if (lightIntensity <= 52428)
            toHit -= 10;
    }

    if (_gcsd != nullptr) {
        toHit += _gcsd->accuracyBonus;
    }

    if ((attacker->data.critter.combat.results & DAM_BLIND) != 0) {
        toHit -= 25;
    }

    if (targetIsCritter && defender != nullptr && (defender->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) {
        toHit += 40;
    }

    if (attacker->data.critter.combat.team != gDude->data.critter.combat.team) {
        switch (settings.preferences.combat_difficulty) {
        case 0:
            toHit -= 20;
            break;
        case 2:
            toHit += 20;
            break;
        }
    }

    if (toHit > 95) {
        toHit = 95;
    }

    if (toHit < -100) {
        debugPrint("Whoa! Bad skill value in determine_to_hit!\n");
    }

    return toHit;
}

// 0x4247B8
static void attackComputeDamage(Attack* attack, int ammoQuantity, int bonusDamageMultiplier)
{
    int* damagePtr;
    Object* critter;
    int* flagsPtr;
    int* knockbackDistancePtr;

    if ((attack->attackerFlags & DAM_HIT) != 0) {
        damagePtr = &(attack->defenderDamage);
        critter = attack->defender;
        flagsPtr = &(attack->defenderFlags);
        knockbackDistancePtr = &(attack->defenderKnockback);
    } else {
        damagePtr = &(attack->attackerDamage);
        critter = attack->attacker;
        flagsPtr = &(attack->attackerFlags);
        knockbackDistancePtr = nullptr;
    }

    *damagePtr = 0;

    if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    int damageType = weaponGetDamageType(attack->attacker, attack->weapon);
    int damageThreshold = critterGetStat(critter, STAT_DAMAGE_THRESHOLD + damageType);
    int damageResistance = critterGetStat(critter, STAT_DAMAGE_RESISTANCE + damageType);

    if ((*flagsPtr & DAM_BYPASS) != 0 && damageType != DAMAGE_TYPE_EMP) {
        damageThreshold = 20 * damageThreshold / 100;
        damageResistance = 20 * damageResistance / 100;
    } else {
        // SFALL
        if (weaponGetPerk(attack->weapon) == PERK_WEAPON_PENETRATE
            || unarmedIsPenetrating(attack->hitMode)) {
            damageThreshold = 20 * damageThreshold / 100;
        }

        if (playerActorIs(attack->attacker) && traitIsSelected(TRAIT_FINESSE, attack->attacker)) {
            damageResistance += 30;
        }
    }

    int damageBonus;
    if (playerActorIs(attack->attacker) && weaponGetAttackTypeForHitMode(attack->weapon, attack->hitMode) == ATTACK_TYPE_RANGED) {
        damageBonus = 2 * perkGetRank(attack->attacker, PERK_BONUS_RANGED_DAMAGE);
    } else {
        damageBonus = 0;
    }

    int combatDifficultyDamageModifier = 100;
    if (attack->attacker->data.critter.combat.team != gDude->data.critter.combat.team) {
        switch (settings.preferences.combat_difficulty) {
        case COMBAT_DIFFICULTY_EASY:
            combatDifficultyDamageModifier = 75;
            break;
        case COMBAT_DIFFICULTY_HARD:
            combatDifficultyDamageModifier = 125;
            break;
        }
    }

    // SFALL: Damage mod.
    DamageCalculationContext context;
    context.attack = attack;
    context.damagePtr = damagePtr;
    context.damageResistance = damageResistance;
    context.damageThreshold = damageThreshold;
    context.damageBonus = damageBonus;
    context.bonusDamageMultiplier = bonusDamageMultiplier;
    context.combatDifficultyDamageModifier = combatDifficultyDamageModifier;

    if (gDamageCalculationType == DAMAGE_CALCULATION_TYPE_GLOVZ || gDamageCalculationType == DAMAGE_CALCULATION_TYPE_GLOVZ_WITH_DAMAGE_MULTIPLIER_TWEAK) {
        damageModCalculateGlovz(&context);
    } else if (gDamageCalculationType == DAMAGE_CALCULATION_TYPE_YAAM) {
        damageModCalculateYaam(&context);
    } else {
        damageResistance += weaponGetAmmoDamageResistanceModifier(attack->weapon);
        if (damageResistance > 100) {
            damageResistance = 100;
        } else if (damageResistance < 0) {
            damageResistance = 0;
        }

        int damageMultiplier = bonusDamageMultiplier * weaponGetAmmoDamageMultiplier(attack->weapon);
        int damageDivisor = weaponGetAmmoDamageDivisor(attack->weapon);

        for (int index = 0; index < ammoQuantity; index++) {
            int damage = weaponGetDamage(attack->attacker, attack->hitMode);

            damage += damageBonus;

            damage *= damageMultiplier;

            if (damageDivisor != 0) {
                damage /= damageDivisor;
            }

            // TODO: Why we're halving it?
            damage /= 2;

            damage *= combatDifficultyDamageModifier;
            damage /= 100;

            damage -= damageThreshold;

            if (damage > 0) {
                damage -= damage * damageResistance / 100;
            }

            if (damage > 0) {
                *damagePtr += damage;
            }
        }
    }

    if (attack->attacker == gDude) {
        if (perkGetRank(attack->attacker, PERK_LIVING_ANATOMY) != 0) {
            int kt = critterGetKillType(attack->defender);
            if (kt != KILL_TYPE_ROBOT && kt != KILL_TYPE_ALIEN) {
                *damagePtr += 5;
            }
        }

        if (perkGetRank(attack->attacker, PERK_PYROMANIAC) != 0) {
            if (weaponGetDamageType(attack->attacker, attack->weapon) == DAMAGE_TYPE_FIRE) {
                *damagePtr += 5;
            }
        }
    }

    if (knockbackDistancePtr != nullptr
        && (critter->flags & OBJECT_MULTIHEX) == 0
        && (damageType == DAMAGE_TYPE_EXPLOSION || attack->weapon == nullptr || weaponGetAttackTypeForHitMode(attack->weapon, attack->hitMode) == ATTACK_TYPE_MELEE)
        && PID_TYPE(critter->pid) == OBJ_TYPE_CRITTER
        && !_critter_flag_check(critter->pid, CRITTER_NO_KNOCKBACK)) {
        bool shouldKnockback = true;
        bool hasStonewall = false;
        if (critter == gDude) {
            if (perkGetRank(critter, PERK_STONEWALL) != 0) {
                int chance = randomBetween(0, 100);
                hasStonewall = true;
                if (chance < 50) {
                    shouldKnockback = false;
                }
            }
        }

        if (shouldKnockback) {
            int knockbackDistanceDivisor = weaponGetPerk(attack->weapon) == PERK_WEAPON_KNOCKBACK ? 5 : 10;

            *knockbackDistancePtr = *damagePtr / knockbackDistanceDivisor;

            if (hasStonewall) {
                *knockbackDistancePtr /= 2;
            }
        }
    }
}

// 0x424BAC
void attackComputeDeathFlags(Attack* attack)
{
    _check_for_death(attack->attacker, attack->attackerDamage, &(attack->attackerFlags));
    _check_for_death(attack->defender, attack->defenderDamage, &(attack->defenderFlags));

    for (int index = 0; index < attack->extrasLength; index++) {
        _check_for_death(attack->extras[index], attack->extrasDamage[index], &(attack->extrasFlags[index]));
    }
}

// 0x424C04
void _apply_damage(Attack* attack, bool animated)
{
    Object* attacker = attack->attacker;
    bool attackerIsCritter = attacker != nullptr && FID_TYPE(attacker->fid) == OBJ_TYPE_CRITTER;
    bool v5 = attack->defender != attack->oops;

    if (attackerIsCritter && (attacker->data.critter.combat.results & DAM_DEAD) == 0) {
        _set_new_results(attacker, attack->attackerFlags);
        // TODO: Not sure about "attack->defender == attack->oops".
        _damage_object(attacker, attack->attackerDamage, animated, attack->defender == attack->oops, attacker);
    }

    Object* v7 = attack->oops;
    if (v7 != nullptr && v7 != attack->defender) {
        _combatai_notify_onlookers(v7);
    }

    Object* defender = attack->defender;
    bool defenderIsCritter = defender != nullptr && FID_TYPE(defender->fid) == OBJ_TYPE_CRITTER;

    if (!defenderIsCritter && !v5) {
        bool v9 = objectIsPartyMember(attack->defender) && objectIsPartyMember(attack->attacker) ? false : true;
        if (v9) {
            if (defender != nullptr) {
                if (defender->sid != -1) {
                    scriptSetFixedParam(defender->sid, attack->attackerDamage);
                    scriptSetObjects(defender->sid, attack->attacker, attack->weapon);
                    scriptExecProc(defender->sid, SCRIPT_PROC_DAMAGE);
                }
            }
        }
    }

    if (defenderIsCritter && (defender->data.critter.combat.results & DAM_DEAD) == 0) {
        _set_new_results(defender, attack->defenderFlags);

        if (defenderIsCritter) {
            if (defenderIsCritter) {
                if ((defender->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
                    if (!v5 || defender != gDude) {
                        _critter_set_who_hit_me(defender, attack->attacker);
                    }
                } else if (defender == attack->oops || defender->data.critter.combat.team != attack->attacker->data.critter.combat.team) {
                    _combatai_check_retaliation(defender, attack->attacker);
                }
            }
        }

        scriptSetObjects(defender->sid, attack->attacker, attack->weapon);
        _damage_object(defender, attack->defenderDamage, animated, attack->defender != attack->oops, attacker);

        if (defenderIsCritter) {
            _combatai_notify_onlookers(defender);
        }

        if (attack->defenderDamage >= 0 && (attack->attackerFlags & DAM_HIT) != 0) {
            scriptSetObjects(attack->attacker->sid, nullptr, attack->defender);
            scriptSetFixedParam(attack->attacker->sid, 2);
            scriptExecProc(attack->attacker->sid, SCRIPT_PROC_COMBAT);
        }
    }

    for (int index = 0; index < attack->extrasLength; index++) {
        Object* obj = attack->extras[index];
        if (FID_TYPE(obj->fid) == OBJ_TYPE_CRITTER && (obj->data.critter.combat.results & DAM_DEAD) == 0) {
            _set_new_results(obj, attack->extrasFlags[index]);

            if (defenderIsCritter) {
                if ((obj->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
                    _critter_set_who_hit_me(obj, attack->attacker);
                } else if (obj->data.critter.combat.team != attack->attacker->data.critter.combat.team) {
                    _combatai_check_retaliation(obj, attack->attacker);
                }
            }

            scriptSetObjects(obj->sid, attack->attacker, attack->weapon);
            // TODO: Not sure about defender == oops.
            _damage_object(obj, attack->extrasDamage[index], animated, attack->defender == attack->oops, attack->attacker);
            _combatai_notify_onlookers(obj);

            if (attack->extrasDamage[index] >= 0) {
                if ((attack->attackerFlags & DAM_HIT) != 0) {
                    scriptSetObjects(attack->attacker->sid, nullptr, obj);
                    scriptSetFixedParam(attack->attacker->sid, 2);
                    scriptExecProc(attack->attacker->sid, SCRIPT_PROC_COMBAT);
                }
            }
        }
    }
}

// 0x424EE8
static void _check_for_death(Object* object, int damage, int* flags)
{
    if (object == nullptr || !_critter_flag_check(object->pid, CRITTER_INVULNERABLE)) {
        if (object == nullptr || PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
            if (damage > 0) {
                if (critterGetHitPoints(object) - damage <= 0) {
                    *flags |= DAM_DEAD;
                }
            }
        }
    }
}

// 0x424F2C
static void _set_new_results(Object* critter, int flags)
{
    if (critter == nullptr) {
        return;
    }

    if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    if (_critter_flag_check(critter->pid, CRITTER_INVULNERABLE)) {
        return;
    }

    if (PID_TYPE(critter->pid) != OBJ_TYPE_CRITTER) {
        return;
    }

    if ((flags & DAM_DEAD) != 0) {
        queueRemoveEvents(critter);
    } else if ((flags & DAM_KNOCKED_OUT) != 0) {
        // SFALL: Fix multiple knockout events.
        queueRemoveEventsByType(critter, EVENT_TYPE_KNOCKOUT);

        int endurance = critterGetStat(critter, STAT_ENDURANCE);
        queueAddEvent(10 * (35 - 3 * endurance), critter, nullptr, EVENT_TYPE_KNOCKOUT);
    }

    if (critter == gDude && (flags & DAM_CRIP_ARM_ANY) != 0) {
        critter->data.critter.combat.results |= flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_CRIP | DAM_DEAD | DAM_LOSE_TURN);

        int leftItemAction;
        int rightItemAction;
        interfaceGetItemActions(&leftItemAction, &rightItemAction);
        presenter()->hudItems(true, leftItemAction, rightItemAction);
    } else {
        critter->data.critter.combat.results |= flags & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN | DAM_CRIP | DAM_DEAD | DAM_LOSE_TURN);
    }
}

// 0x425020
static void _damage_object(Object* a1, int damage, bool animated, int a4, Object* a5)
{
    if (a1 == nullptr) {
        return;
    }

    if (FID_TYPE(a1->fid) != OBJ_TYPE_CRITTER) {
        return;
    }

    if (_critter_flag_check(a1->pid, CRITTER_INVULNERABLE)) {
        return;
    }

    if (damage <= 0) {
        return;
    }

    critterAdjustHitPoints(a1, -damage);

    if (a1 == gDude) {
        presenter()->hudHitPoints(animated);
    }

    a1->data.critter.combat.damageLastTurn += damage;

    if (!a4) {
        // TODO: Not sure about this one.
        if (!objectIsPartyMember(a1) || !objectIsPartyMember(a5)) {
            scriptSetFixedParam(a1->sid, damage);
            scriptExecProc(a1->sid, SCRIPT_PROC_DAMAGE);
        }
    }

    if ((a1->data.critter.combat.results & DAM_DEAD) != 0) {
        scriptSetObjects(a1->sid, a1->data.critter.combat.whoHitMe, nullptr);
        scriptExecProc(a1->sid, SCRIPT_PROC_DESTROY);
        itemDestroyAllHidden(a1);

        if (a1 != gDude) {
            Object* whoHitMe = a1->data.critter.combat.whoHitMe;
            if (whoHitMe == gDude || (whoHitMe != nullptr && whoHitMe->data.critter.combat.team == gDude->data.critter.combat.team)) {
                bool scriptOverrides = false;
                Script* scr;
                if (scriptGetScript(a1->sid, &scr) != -1) {
                    scriptOverrides = scr->scriptOverrides;
                }

                if (!scriptOverrides) {
                    // Credit the KILLER's bucket. whoHitMe is a live pointer
                    // right here, which is why §4 takes the subject from the call
                    // site rather than from nearest-player geometry — that would
                    // hand a kill to whoever happened to be standing closest.
                    //
                    // A killer that is not a registered player actor (a companion,
                    // or the team-mate branch of the guard above) credits slot 0.
                    // That is exactly today's behavior for those cases — a
                    // disclosed default, not new guesswork (§4). The -1 is mapped
                    // to that policy explicitly and never used as an index — a
                    // negative one would corrupt whatever static precedes the
                    // array.
                    int slot = playerActorSlotOf(whoHitMe);
                    if (slot < 0) {
                        slot = 0;
                    }
                    _combat_exps[slot] += critterGetExp(a1);
                    killsIncByType(critterGetKillType(a1));
                }
            }
        }

        if (a1->sid != -1) {
            scriptRemove(a1->sid);
            a1->sid = -1;
        }

        partyMemberRemove(a1);
    }
}

// 0x425E3C
void _combat_anim_begin()
{
    if (++_combat_turn_running == 1 && gDude == _main_ctd.attacker) {
        gameUiDisable(1);
        presenter()->cursorSet(26);
        if (_combat_highlight == 2) {
            _combat_outline_off();
        }
    }
}

// 0x425E80
void _combat_anim_finished()
{
    _combat_turn_running -= 1;
    if (_combat_turn_running != 0) {
        return;
    }

    if (gDude == _main_ctd.attacker) {
        gameUiEnable();
    }

    // The attack outcome is applied when the animation completes (animated=true
    // registers the defender's death/knockback animation).
    _combat_apply_attack_results(true);
}

// The attack-outcome application (ammo spend, damage, end-combat check, _main_ctd
// reset, attacker standup) that _combat_anim_finished runs when an attack
// animation completes. Extracted (REWRITE_PLAN Phase 2.4) so the headless server
// can apply the same outcome WITHOUT the attack animation, which cannot build
// headless (the projectile-travel sequence is rejected by reg_anim_end). The
// `animated` flag is forwarded to _apply_damage: true keeps the death animation
// (client), false applies the results with no animation (server). Gated by
// _combat_cleanup_enabled exactly as the inline block was, so it is a no-op when
// no attack is pending cleanup.
static void _combat_apply_attack_results(bool animated)
{
    if (!_combat_cleanup_enabled) {
        return;
    }

    _combat_cleanup_enabled = false;

    Object* weapon = critterGetWeaponForHitMode(_main_ctd.attacker, _main_ctd.hitMode);
    if (weapon != nullptr) {
        if (ammoGetCapacity(weapon) > 0) {
            int ammoQuantity = ammoGetQuantity(weapon);
            ammoSetQuantity(weapon, ammoQuantity - _main_ctd.ammoQuantity);

            if (_main_ctd.attacker == gDude) {
                _intface_update_ammo_lights();
            }
        }
    }

    if (_combat_call_display) {
        _combat_display(&_main_ctd);
        _combat_call_display = false;
    }

    // The client displaces knocked-back critters to a new tile via the
    // actionKnockdown animation inside _show_damage_to_object; the server skips
    // that animation, so apply the same tile move directly. This MUST run before
    // _apply_damage: the animated path completes the knockdown move before
    // _apply_damage (which is deferred to _combat_anim_finished), so the
    // knockback/prone decision has to observe each critter's PRE-damage state.
    // _apply_damage ORs the fresh DAM_KNOCKED_DOWN/OUT into combat.results, and
    // _critter_is_prone tests exactly those bits — running knockback afterwards
    // would make the guard suppress the very knockback we want. Non-dead only
    // (dead corpses are finalized NO_BLOCK below; their knockback is death-
    // animation dependent — see _combat_knockback_headless). The death flags read
    // here were computed by attackComputeDeathFlags before this function.
    // Knockback COMPUTE now (pre-damage board: the prone/knockdown guard must see each
    // victim's state before _apply_damage ORs in DAM_KNOCKED_DOWN). The APPLY (the MOVE
    // emission) is DEFERRED to after attackResult() below, so the knockback delta rides the
    // wire AFTER the event that reserves the victim on the client — else the client can't
    // hold the durMs=0 snap and the recorded slide plays from a stale origin (bug J; pacing
    // design §8.5). Non-animated (headless/server) only, exactly as before.
    Object* kbVictims[1 + EXPLOSION_TARGET_COUNT];
    int kbDests[1 + EXPLOSION_TARGET_COUNT];
    int kbCount = 0;
    if (!animated) {
        kbCount = _combat_compute_knockback(&_main_ctd, kbVictims, kbDests);
    }

    _apply_damage(&_main_ctd, animated);

    // The client finalizes a killed critter's physical state (death FID, go
    // flat, OBJECT_NO_BLOCK, light off) via the _show_death animation callback
    // registered inside _action_attack. The server skips that animation, so
    // finalize newly dead critters directly with critterKill (mirrors the
    // non-animated branches of actionExplode / _action_dmg). _apply_damage
    // already flagged DAM_DEAD, awarded XP and removed the death script;
    // critterKill only applies the physical/corpse state and does not repeat
    // those. Without this a killed critter stays blocking its tile and blocks
    // line-of-sight, diverging AI pathing and to-hit.
    if (!animated) {
        if (_main_ctd.attacker != nullptr && (_main_ctd.attackerFlags & DAM_DEAD) != 0) {
            critterKill(_main_ctd.attacker, -1, false);
        }
        if (_main_ctd.defender != nullptr && (_main_ctd.defenderFlags & DAM_DEAD) != 0) {
            critterKill(_main_ctd.defender, -1, false);
        }
        for (int index = 0; index < _main_ctd.extrasLength; index++) {
            if ((_main_ctd.extrasFlags[index] & DAM_DEAD) != 0) {
                critterKill(_main_ctd.extras[index], -1, false);
            }
        }
    }

    // Thrown-weapon consumption. The animated path runs itemRemove/itemReplace/object
    // placement inline inside _action_ranged's throw branch; the server skips that
    // composite, so apply the same authoritative state directly (else a thrown weapon is
    // never consumed — pre-existing bug). No-op for non-throws. Headless only: the client
    // (animated=true) still consumes via _action_ranged, so there is no double-apply.
    if (!animated) {
        actionThrowConsumeHeadless(&_main_ctd);
    }

    // MP_PROTOCOL.md §2: one resolved attack (the causal envelope; state rode
    // objectDelta). Emitted after damage/death are final and BEFORE the _main_ctd
    // re-init below, so all fields are readable. No-op under null/client (byte-
    // identical). See Presenter::attackResult.
    //
    // CONVERGENCE (record channel = sole attack presenter): when this attack's animation
    // is carried by the record stream (EVENT_PRES_SEQ), suppress this decoder-mirror cue —
    // else the viewer double-presents it (rocket gib twice; a grenade would double every
    // victim). The presseq already carries the anim AND reserves the same participants, so
    // nothing is lost. Off record / SP → still emitted (goldens byte-identical).
    if (!combatAttackRecorded(_main_ctd.attacker, _main_ctd.hitMode)) {
        presenter()->attackResult(&_main_ctd);
    }

    // Knockback APPLY — deferred to HERE so its EVENT_MOVE is buffered AFTER the reserving
    // event above (attackResult, or the recorded presSeq emitted earlier in _action_attack).
    // The client then holds the durMs=0 knockback snap against the victim's now-reserved
    // replay and commits it at the slide's action frame (resolveHeld). Dead victims were
    // computed as no-move, so a critterKill above never strands a freed pointer. Pointers +
    // tiles were captured pre-damage; independent of the _main_ctd re-init below.
    if (!animated) {
        _combat_commit_knockback(kbVictims, kbDests, kbCount);
    }

    // Idle-deadline pacing (server_loop.h): an AI attack adds a swing estimate to the
    // backlog the client animates before the player's turn is on screen. Only inside a
    // resumable session and only for AI attackers (the player's own swing is already
    // shown); off the resumable path there is no session, so goldens are byte-identical.
    if (combatSessionActive() && _main_ctd.attacker != gDude) {
        serverAddPresentationCostMs(kAttackPresentationEstimateMs);
    }

    Object* attacker = _main_ctd.attacker;
    if (attacker == gDude && _combat_highlight == 2) {
        _combat_outline_on();
    }

    if (_scr_end_combat()) {
        if ((gDude->data.critter.combat.results & DAM_KNOCKED_OUT) != 0) {
            if (attacker->data.critter.combat.team == gDude->data.critter.combat.team) {
                _combat_ending_guy = gDude->data.critter.combat.whoHitMe;
            } else {
                _combat_ending_guy = attacker;
            }
        }
    }

    attackInit(&_main_ctd, _main_ctd.attacker, nullptr, HIT_MODE_PUNCH, HIT_LOCATION_TORSO);

    if ((attacker->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) {
        if ((attacker->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_DEAD | DAM_LOSE_TURN)) == 0) {
            _combat_standup(attacker);
        }
    }
}

// 0x425FBC
static void _combat_standup(Object* a1)
{
    int v2;

    v2 = 3;
    if (playerActorIs(a1) && perkGetRank(a1, PERK_QUICK_RECOVERY)) {
        v2 = 1;
    }

    if (v2 > a1->data.critter.combat.ap) {
        a1->data.critter.combat.ap = 0;
    } else {
        a1->data.critter.combat.ap -= v2;
    }

    if (a1 == gDude) {
        presenter()->hudActionPoints(gDude->data.critter.combat.ap, _combat_free_move);
    }

    _dude_standup(a1);

    // NOTE: Uninline.
    _combat_turn_run();
}

// check for possibility of performing attacking
// 0x426614
int _combat_check_bad_shot(Object* attacker, Object* defender, int hitMode, bool aiming)
{
    int range = 1;
    int tile = -1;
    if (defender != nullptr) {
        tile = defender->tile;
        range = objectGetDistanceBetween(attacker, defender);
        if ((defender->data.critter.combat.results & DAM_DEAD) != 0) {
            return COMBAT_BAD_SHOT_ALREADY_DEAD;
        }
    }

    Object* weapon = critterGetWeaponForHitMode(attacker, hitMode);
    if (weapon != nullptr) {
        if ((attacker->data.critter.combat.results & DAM_CRIP_ARM_LEFT) != 0
            && (attacker->data.critter.combat.results & DAM_CRIP_ARM_RIGHT) != 0) {
            return COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED;
        }

        if ((attacker->data.critter.combat.results & DAM_CRIP_ARM_ANY) != 0) {
            if (weaponIsTwoHanded(weapon)) {
                return COMBAT_BAD_SHOT_ARM_CRIPPLED;
            }
        }
    }

    if (weaponGetActionPointCost(attacker, hitMode, aiming) > attacker->data.critter.combat.ap) {
        return COMBAT_BAD_SHOT_NOT_ENOUGH_AP;
    }

    if (weaponGetRange(attacker, hitMode) < range) {
        return COMBAT_BAD_SHOT_OUT_OF_RANGE;
    }

    int attackType = weaponGetAttackTypeForHitMode(weapon, hitMode);

    if (ammoGetCapacity(weapon) > 0) {
        if (ammoGetQuantity(weapon) == 0) {
            return COMBAT_BAD_SHOT_NO_AMMO;
        }
    }

    if (attackType == ATTACK_TYPE_RANGED
        || attackType == ATTACK_TYPE_THROW
        || weaponGetRange(attacker, hitMode) > 1) {
        if (_combat_is_shot_blocked(attacker, attacker->tile, tile, defender, nullptr)) {
            return COMBAT_BAD_SHOT_AIM_BLOCKED;
        }
    }

    return COMBAT_BAD_SHOT_OK;
}

// 0x426744
bool _combat_to_hit(Object* target, int* accuracy)
{
    int hitMode;
    bool aiming;
    if (interfaceGetCurrentHitMode(&hitMode, &aiming) == -1) {
        return false;
    }

    if (_combat_check_bad_shot(gDude, target, hitMode, aiming) != COMBAT_BAD_SHOT_OK) {
        return false;
    }

    *accuracy = attackDetermineToHit(gDude, gDude->tile, target, HIT_LOCATION_UNCALLED, hitMode, true);

    return true;
}

// 0x4267CC
// Wire-viewer attack commit hook (COMBAT_CLIENT_DESIGN.md §3.b). When the SDL wire
// viewer runs _combat_attack_this, the whole vanilla selection UX (turn guard, hit
// mode, bad-shot messages, called-shot picker) runs locally exactly as in the real
// game; only the COMMIT point — where the local sim would begin — forwards the fully
// selected attack UPSTREAM through this hook instead. Held as a pointer so f2_core /
// f2_server link without f2_client (client_net.cc's implementation lives there); it
// is null on every non-viewer path, and the fork is additionally gated on
// clientViewerActive() so it is inert wherever the flag is unset (all goldens).
static void (*gViewerAttackHook)(Object* target, int hitMode, int hitLocation) = nullptr;

void combatSetViewerAttackHook(void (*hook)(Object* target, int hitMode, int hitLocation))
{
    gViewerAttackHook = hook;
}

void _combat_attack_this(Object* target)
{
    if (target == nullptr) {
        return;
    }

    if ((gCombatState & 0x02) == 0) {
        return;
    }

    int hitMode;
    bool aiming;
    if (interfaceGetCurrentHitMode(&hitMode, &aiming) == -1) {
        return;
    }

    MessageListItem messageListItem;
    Object* item;
    char formattedText[80];
    const char* sfx;

    int rc = _combat_check_bad_shot(gDude, target, hitMode, aiming);
    switch (rc) {
    case COMBAT_BAD_SHOT_NO_AMMO:
        item = critterGetWeaponForHitMode(gDude, hitMode);
        messageListItem.num = 101; // Out of ammo.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }

        sfx = sfxBuildWeaponName(WEAPON_SOUND_EFFECT_OUT_OF_AMMO, item, hitMode, nullptr);
        presenter()->sfxPlay(sfx);
        return;
    case COMBAT_BAD_SHOT_OUT_OF_RANGE:
        messageListItem.num = 102; // Target out of range.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }
        return;
    case COMBAT_BAD_SHOT_NOT_ENOUGH_AP:
        item = critterGetWeaponForHitMode(gDude, hitMode);
        messageListItem.num = 100; // You need %d action points.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            int actionPointsRequired = weaponGetActionPointCost(gDude, hitMode, aiming);
            snprintf(formattedText, sizeof(formattedText), messageListItem.text, actionPointsRequired);
            presenter()->consoleMessage(formattedText);
        }
        return;
    case COMBAT_BAD_SHOT_ALREADY_DEAD:
        return;
    case COMBAT_BAD_SHOT_AIM_BLOCKED:
        messageListItem.num = 104; // Your aim is blocked.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }
        return;
    case COMBAT_BAD_SHOT_ARM_CRIPPLED:
        messageListItem.num = 106; // You cannot use two-handed weapons with a crippled arm.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }
        return;
    case COMBAT_BAD_SHOT_BOTH_ARMS_CRIPPLED:
        messageListItem.num = 105; // You cannot use weapons with both arms crippled.
        if (messageListGetItem(&gCombatMessageList, &messageListItem)) {
            presenter()->consoleMessage(messageListItem.text);
        }
        return;
    }

    if (!isInCombat()) {
        CombatStartData combat;
        combat.attacker = gDude;
        combat.defender = target;
        combat.actionPointsBonus = 0;
        combat.accuracyBonus = 0;
        combat.damageBonus = 0;
        combat.minDamage = 0;
        combat.maxDamage = INT_MAX;
        combat.overrideAttackResults = 0;
        _combat(&combat);
        return;
    }

    if (!aiming) {
        // §3.b commit fork: forward the unaimed shot upstream instead of simulating.
        if (clientViewerActive() && gViewerAttackHook != nullptr) {
            gViewerAttackHook(target, hitMode, HIT_LOCATION_UNCALLED);
            return;
        }
        _combat_attack(gDude, target, hitMode, HIT_LOCATION_UNCALLED);
        return;
    }

    if (aiming != 1) {
        debugPrint("Bad called shot value %d\n", aiming);
    }

    int hitLocation;
    if (calledShotSelectHitLocation(target, &hitLocation, hitMode) != -1) {
        // §3.b commit fork: the called-shot picker (modal, local, pure) already ran;
        // forward the aimed shot upstream instead of simulating.
        if (clientViewerActive() && gViewerAttackHook != nullptr) {
            gViewerAttackHook(target, hitMode, hitLocation);
            return;
        }
        _combat_attack(gDude, target, hitMode, hitLocation);
    }
}

// 0x426C64
void _combat_highlight_change()
{
    int targetHighlight = settings.preferences.target_highlight;
    if (targetHighlight != _combat_highlight && isInCombat()) {
        if (targetHighlight != 0) {
            if (_combat_highlight == 0) {
                _combat_outline_on();
            }
        } else {
            _combat_outline_off();
        }
    }

    _combat_highlight = targetHighlight;
}

// Checks if line of fire to the target object is blocked or not. Optionally calculate number of critters on the line of fire.
//
// 0x426CC4
bool _combat_is_shot_blocked(Object* sourceObj, int from, int to, Object* targetObj, int* numCrittersOnLof)
{
    if (numCrittersOnLof != nullptr) {
        *numCrittersOnLof = 0;
    }

    Object* obstacle = sourceObj;
    int current = from;
    while (obstacle != nullptr && current != to) {
        _make_straight_path_func(sourceObj, current, to, nullptr, &obstacle, 32, _obj_shoot_blocking_at);
        if (obstacle != nullptr) {
            if (FID_TYPE(obstacle->fid) != OBJ_TYPE_CRITTER && obstacle != targetObj) {
                return true;
            }

            if (numCrittersOnLof != nullptr && obstacle != targetObj && targetObj != nullptr) {
                // SFALL: Fix for combat_is_shot_blocked_ engine
                // function not taking the flags of critters in the
                // line of fire into account when calculating the hit
                // chance penalty of ranged attacks in
                // determine_to_hit_func_ engine function.
                if ((obstacle->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_DOWN | DAM_KNOCKED_OUT)) == 0) {
                    *numCrittersOnLof += 1;

                    if ((obstacle->flags & OBJECT_MULTIHEX) != 0) {
                        *numCrittersOnLof += 1;
                    }
                }
            }

            if ((obstacle->flags & OBJECT_MULTIHEX) != 0) {
                // SFALL: Fix obtaining the next tile from a multihex object.
                // This bug does not cause any noticeable error in the function.
                current = obstacle->tile;
                if (current != to) {
                    int rotation = tileGetRotationTo(current, to);
                    current = tileGetTileInDirection(current, rotation, 1);
                }
            } else {
                current = obstacle->tile;
            }
        }
    }

    return false;
}

// 0x426D94
int _combat_player_knocked_out_by()
{
    if ((gDude->data.critter.combat.results & DAM_DEAD) != 0) {
        return -1;
    }

    if (_combat_ending_guy == nullptr) {
        return -1;
    }

    return _combat_ending_guy->data.critter.combat.team;
}

// 0x426DB8
int _combat_explode_scenery(Object* a1, Object* a2)
{
    _scr_explode_scenery(a1, a1->tile, weaponGetRocketExplosionRadius(nullptr), a1->elevation);
    return 0;
}

// 0x426DDC
void _combat_delete_critter(Object* obj)
{
    // TODO: Check entire function.
    if (!isInCombat()) {
        return;
    }

    if (_list_total == 0) {
        return;
    }

    int i;
    for (i = 0; i < _list_total; i++) {
        if (obj == _combat_list[i]) {
            break;
        }
    }

    if (i == _list_total) {
        return;
    }

    while (i < (_list_total - 1)) {
        _combat_list[i] = _combat_list[i + 1];
        aiInfoCopy(i + 1, i);
        i++;
    }

    _list_total--;

    _combat_list[_list_total] = obj;

    if (i >= _list_com) {
        if (i < (_list_noncom + _list_com)) {
            _list_noncom--;
        }
    } else {
        _list_com--;
    }

    obj->data.critter.combat.ap = 0;
    objectClearOutline(obj, nullptr);

    obj->data.critter.combat.whoHitMe = nullptr;
    _combatai_delete_critter(obj);
}

// 0x426EC4
void _combatKillCritterOutsideCombat(Object* critter_obj, char* msg)
{
    if (critter_obj != gDude) {
        presenter()->consoleMessage(msg);
        scriptExecProc(critter_obj->sid, SCRIPT_PROC_DESTROY);
        critterKill(critter_obj, -1, 1);
    }
}

int combatGetTargetHighlight()
{
    return _combat_highlight;
}

static void criticalsInit()
{
    int mode = 2;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_OVERRIDE_CRITICALS_MODE_KEY, &mode);
    if (mode < 0 || mode > 3) {
        mode = 0;
    }

    if (mode == 2 || mode == 3) {
        // Men
        criticalsSetValue(KILL_TYPE_MAN, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_FLAGS, DAM_KNOCKED_DOWN | DAM_BYPASS);
        criticalsSetValue(KILL_TYPE_MAN, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5019);

        // Children
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, 0);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5216);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5000);

        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, 0);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5216);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_RIGHT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5000);

        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, 0);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5216);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5000);

        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, 0);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5216);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5000);

        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_UNCALLED, 1, CRIT_DATA_MEMBER_DAMAGE_MULTIPLIER, 4);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_FLAGS, DAM_KNOCKED_DOWN | DAM_BYPASS);
        criticalsSetValue(KILL_TYPE_CHILD, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5212);

        // Super Mutants
        criticalsSetValue(KILL_TYPE_SUPER_MUTANT, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5306);

        // Ghouls
        criticalsSetValue(KILL_TYPE_GHOUL, HIT_LOCATION_HEAD, 4, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_STAT, -1);

        // Brahmin
        criticalsSetValue(KILL_TYPE_BRAHMIN, HIT_LOCATION_HEAD, 4, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_STAT, -1);

        // Radscorpions
        criticalsSetValue(KILL_TYPE_RADSCORPION, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_KNOCKED_DOWN);

        criticalsSetValue(KILL_TYPE_RADSCORPION, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_KNOCKED_DOWN);
        criticalsSetValue(KILL_TYPE_RADSCORPION, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 5608);

        // Centaurs
        criticalsSetValue(KILL_TYPE_CENTAUR, HIT_LOCATION_TORSO, 3, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_KNOCKED_DOWN);

        criticalsSetValue(KILL_TYPE_CENTAUR, HIT_LOCATION_UNCALLED, 3, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_KNOCKED_DOWN);

        // Deathclaws
        criticalsSetValue(KILL_TYPE_DEATH_CLAW, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_CRIP_LEG_LEFT);
        criticalsSetValue(KILL_TYPE_DEATH_CLAW, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_CRIP_LEG_LEFT);
        criticalsSetValue(KILL_TYPE_DEATH_CLAW, HIT_LOCATION_LEFT_LEG, 3, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_CRIP_LEG_LEFT);
        criticalsSetValue(KILL_TYPE_DEATH_CLAW, HIT_LOCATION_LEFT_LEG, 4, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_CRIP_LEG_LEFT);
        criticalsSetValue(KILL_TYPE_DEATH_CLAW, HIT_LOCATION_LEFT_LEG, 5, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_FLAGS, DAM_CRIP_LEG_LEFT);

        // Geckos
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 6701);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 6701);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_FLAGS, DAM_KNOCKED_DOWN | DAM_BYPASS);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 6704);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 6704);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 6704);
        criticalsSetValue(KILL_TYPE_GECKO, HIT_LOCATION_UNCALLED, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 6704);

        // Aliens
        criticalsSetValue(16, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_FLAGS, DAM_KNOCKED_DOWN | DAM_BYPASS);

        // Giant Ants
        criticalsSetValue(17, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_FLAGS, DAM_KNOCKED_DOWN | DAM_BYPASS);

        // Big Bad Boss
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5001);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5001);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5001);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 7105);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 4, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7104);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_HEAD, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5008);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5008);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5009);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 5009);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7102);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_ARM, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7102);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5008);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5008);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5009);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 5009);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7102);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_ARM, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7102);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_TORSO, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_TORSO, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5023);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_RIGHT_LEG, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5023);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 2, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_LEFT_LEG, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7103);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5027);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5027);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 5027);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 5027);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7104);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_EYES, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7104);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 0, CRIT_DATA_MEMBER_MESSAGE_ID, 5033);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 1, CRIT_DATA_MEMBER_MESSAGE_ID, 5027);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 1, CRIT_DATA_MEMBER_MASSIVE_CRITICAL_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 2, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 3, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_GROIN, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);

        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_UNCALLED, 2, CRIT_DATA_MEMBER_DAMAGE_MULTIPLIER, 3);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_UNCALLED, 4, CRIT_DATA_MEMBER_DAMAGE_MULTIPLIER, 4);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_UNCALLED, 4, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
        criticalsSetValue(KILL_TYPE_BIG_BAD_BOSS, HIT_LOCATION_UNCALLED, 5, CRIT_DATA_MEMBER_MESSAGE_ID, 7101);
    }

    if (mode == 1 || mode == 3) {
        Config criticalsConfig;
        if (configInit(&criticalsConfig)) {
            char* criticalsConfigFilePath;
            configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_OVERRIDE_CRITICALS_FILE_KEY, &criticalsConfigFilePath);
            if (criticalsConfigFilePath != nullptr && *criticalsConfigFilePath == '\0') {
                criticalsConfigFilePath = nullptr;
            }

            if (configRead(&criticalsConfig, criticalsConfigFilePath, false)) {
                if (mode == 1) {
                    char sectionKey[16];

                    // Read original kill types (19) plus one for the player.
                    for (int killType = 0; killType < KILL_TYPE_COUNT + 1; killType++) {
                        for (int hitLocation = 0; hitLocation < HIT_LOCATION_COUNT; hitLocation++) {
                            for (int effect = 0; effect < CRTICIAL_EFFECT_COUNT; effect++) {
                                snprintf(sectionKey, sizeof(sectionKey), "c_%02d_%d_%d", killType, hitLocation, effect);

                                // Update player kill type if needed.
                                int newKillType = killType == KILL_TYPE_COUNT ? SFALL_KILL_TYPE_COUNT : killType;
                                for (int dataMember = 0; dataMember < CRIT_DATA_MEMBER_COUNT; dataMember++) {
                                    int value = criticalsGetValue(newKillType, hitLocation, effect, dataMember);
                                    if (configGetInt(&criticalsConfig, sectionKey, gCritDataMemberKeys[dataMember], &value)) {
                                        criticalsSetValue(newKillType, hitLocation, effect, dataMember, value);
                                    }
                                }
                            }
                        }
                    }
                } else if (mode == 3) {
                    char ktSectionKey[32];
                    char hitLocationSectionKey[32];
                    char key[32];

                    // Read Sfall kill types (38) plus one for the player.
                    for (int killType = 0; killType < SFALL_KILL_TYPE_COUNT + 1; killType++) {
                        snprintf(ktSectionKey, sizeof(ktSectionKey), "c_%02d", killType);

                        int enabled = 0;
                        configGetInt(&criticalsConfig, ktSectionKey, "Enabled", &enabled);
                        if (enabled == 0) {
                            continue;
                        }

                        for (int hitLocation = 0; hitLocation < HIT_LOCATION_COUNT; hitLocation++) {
                            if (enabled < 2) {
                                bool hitLocationChanged = false;

                                snprintf(key, sizeof(key), "Part_%d", hitLocation);
                                configGetBool(&criticalsConfig, ktSectionKey, key, &hitLocationChanged);

                                if (!hitLocationChanged) {
                                    continue;
                                }
                            }

                            snprintf(hitLocationSectionKey, sizeof(hitLocationSectionKey), "c_%02d_%d", killType, hitLocation);

                            for (int effect = 0; effect < CRTICIAL_EFFECT_COUNT; effect++) {
                                for (int dataMember = 0; dataMember < CRIT_DATA_MEMBER_COUNT; dataMember++) {
                                    int value = criticalsGetValue(killType, hitLocation, effect, dataMember);
                                    snprintf(key, sizeof(key), "e%d_%s", effect, gCritDataMemberKeys[dataMember]);
                                    if (configGetInt(&criticalsConfig, hitLocationSectionKey, key, &value)) {
                                        criticalsSetValue(killType, hitLocation, effect, dataMember, value);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            configFree(&criticalsConfig);
        }
    }

    memcpy(gBaseCriticalHitTables, gCriticalHitTables, sizeof(gCriticalHitTables));
    memcpy(gBasePlayerCriticalHitTable, gPlayerCriticalHitTable, sizeof(gPlayerCriticalHitTable));
}

static void criticalsReset()
{
    memcpy(gCriticalHitTables, gBaseCriticalHitTables, sizeof(gBaseCriticalHitTables));
    memcpy(gPlayerCriticalHitTable, gBasePlayerCriticalHitTable, sizeof(gBasePlayerCriticalHitTable));
}

static void criticalsExit()
{
    criticalsReset();
}

int criticalsGetValue(int killType, int hitLocation, int effect, int dataMember)
{
    if (killType == SFALL_KILL_TYPE_COUNT) {
        return gPlayerCriticalHitTable[hitLocation][effect].values[dataMember];
    } else {
        return gCriticalHitTables[killType][hitLocation][effect].values[dataMember];
    }
}

void criticalsSetValue(int killType, int hitLocation, int effect, int dataMember, int value)
{
    if (killType == SFALL_KILL_TYPE_COUNT) {
        gPlayerCriticalHitTable[hitLocation][effect].values[dataMember] = value;
    } else {
        gCriticalHitTables[killType][hitLocation][effect].values[dataMember] = value;
    }
}

void criticalsResetValue(int killType, int hitLocation, int effect, int dataMember)
{
    if (killType == SFALL_KILL_TYPE_COUNT) {
        gPlayerCriticalHitTable[hitLocation][effect].values[dataMember] = gBasePlayerCriticalHitTable[hitLocation][effect].values[dataMember];
    } else {
        gCriticalHitTables[killType][hitLocation][effect].values[dataMember] = gBaseCriticalHitTables[killType][hitLocation][effect].values[dataMember];
    }
}

static void burstModInit()
{
    configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BURST_MOD_ENABLED_KEY, &gBurstModEnabled);

    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BURST_MOD_CENTER_MULTIPLIER_KEY, &gBurstModCenterMultiplier);
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BURST_MOD_CENTER_DIVISOR_KEY, &gBurstModCenterDivisor);
    if (gBurstModCenterDivisor < 1) {
        gBurstModCenterDivisor = 1;
    }
    if (gBurstModCenterMultiplier > gBurstModCenterDivisor) {
        gBurstModCenterMultiplier = gBurstModCenterDivisor;
    }

    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BURST_MOD_TARGET_MULTIPLIER_KEY, &gBurstModTargetMultiplier);
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BURST_MOD_TARGET_DIVISOR_KEY, &gBurstModTargetDivisor);
    if (gBurstModTargetDivisor < 1) {
        gBurstModTargetDivisor = 1;
    }
    if (gBurstModTargetMultiplier > gBurstModTargetDivisor) {
        gBurstModTargetMultiplier = gBurstModTargetDivisor;
    }
}

static int burstModComputeRounds(int totalRounds, int* centerRoundsPtr, int* leftRoundsPtr, int* rightRoundsPtr)
{
    int totalRoundsMultiplied = totalRounds * gBurstModCenterMultiplier;
    int centerRounds = totalRoundsMultiplied / gBurstModCenterDivisor;
    if ((totalRoundsMultiplied % gBurstModCenterDivisor) != 0) {
        centerRounds++;
    }

    if (centerRounds == 0) {
        centerRounds++;
    }
    *centerRoundsPtr = centerRounds;

    int leftRounds = (totalRounds - centerRounds) / 2;
    *leftRoundsPtr = leftRounds;
    *rightRoundsPtr = totalRounds - centerRounds - leftRounds;

    int centerRoundsMultiplied = centerRounds * gBurstModTargetMultiplier;
    int mainTargetRounds = centerRoundsMultiplied / gBurstModTargetDivisor;
    if ((centerRoundsMultiplied % gBurstModTargetDivisor) != 0) {
        mainTargetRounds++;
    }

    return mainTargetRounds;
}

static void unarmedInit()
{
    unarmedInitVanilla();
    unarmedInitCustom();
}

static void unarmedInitVanilla()
{
    UnarmedHitDescription* hitDescription;

    // Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_PUNCH]);
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->actionPointCost = 3;

    // Strong Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_STRONG_PUNCH]);
    hitDescription->requiredSkill = 55;
    hitDescription->requiredStats[STAT_AGILITY] = 6;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 3;
    hitDescription->actionPointCost = 3;
    hitDescription->isPenetrate = false;
    hitDescription->isSecondary = false;

    // Hammer Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_HAMMER_PUNCH]);
    hitDescription->requiredLevel = 6;
    hitDescription->requiredSkill = 75;
    hitDescription->requiredStats[STAT_STRENGTH] = 5;
    hitDescription->requiredStats[STAT_AGILITY] = 6;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 5;
    hitDescription->bonusCriticalChance = 5;
    hitDescription->actionPointCost = 3;

    // Lightning Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_HAYMAKER]);
    hitDescription->requiredLevel = 9;
    hitDescription->requiredSkill = 100;
    hitDescription->requiredStats[STAT_STRENGTH] = 5;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 7;
    hitDescription->bonusCriticalChance = 15;
    hitDescription->actionPointCost = 3;

    // Chop Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_JAB]);
    hitDescription->requiredLevel = 5;
    hitDescription->requiredSkill = 75;
    hitDescription->requiredStats[STAT_STRENGTH] = 5;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 3;
    hitDescription->bonusCriticalChance = 10;
    hitDescription->actionPointCost = 3;
    hitDescription->isSecondary = true;

    // Dragon Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_PALM_STRIKE]);
    hitDescription->requiredLevel = 12;
    hitDescription->requiredSkill = 115;
    hitDescription->requiredStats[STAT_STRENGTH] = 5;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 7;
    hitDescription->bonusCriticalChance = 20;
    hitDescription->actionPointCost = 6;
    hitDescription->isPenetrate = true;
    hitDescription->isSecondary = true;

    // Force Punch
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_PIERCING_STRIKE]);
    hitDescription->requiredLevel = 16;
    hitDescription->requiredSkill = 130;
    hitDescription->requiredStats[STAT_STRENGTH] = 5;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 10;
    hitDescription->bonusCriticalChance = 40;
    hitDescription->actionPointCost = 8;
    hitDescription->isPenetrate = true;
    hitDescription->isSecondary = true;

    // Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_KICK]);
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->actionPointCost = 3;

    // Strong Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_STRONG_KICK]);
    hitDescription->requiredSkill = 40;
    hitDescription->requiredStats[STAT_AGILITY] = 6;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 5;
    hitDescription->actionPointCost = 4;

    // Snap Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_SNAP_KICK]);
    hitDescription->requiredLevel = 6;
    hitDescription->requiredSkill = 60;
    hitDescription->requiredStats[STAT_AGILITY] = 6;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 7;
    hitDescription->actionPointCost = 4;

    // Roundhouse Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_POWER_KICK]);
    hitDescription->requiredLevel = 9;
    hitDescription->requiredSkill = 80;
    hitDescription->requiredStats[STAT_STRENGTH] = 6;
    hitDescription->requiredStats[STAT_AGILITY] = 6;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 9;
    hitDescription->bonusCriticalChance = 5;
    hitDescription->actionPointCost = 4;

    // Kip Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_HIP_KICK]);
    hitDescription->requiredLevel = 6;
    hitDescription->requiredSkill = 60;
    hitDescription->requiredStats[STAT_STRENGTH] = 6;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 7;
    hitDescription->actionPointCost = 7;
    hitDescription->isSecondary = true;

    // Jump Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_HOOK_KICK]);
    hitDescription->requiredLevel = 12;
    hitDescription->requiredSkill = 100;
    hitDescription->requiredStats[STAT_STRENGTH] = 6;
    hitDescription->requiredStats[STAT_AGILITY] = 7;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 9;
    hitDescription->bonusCriticalChance = 10;
    hitDescription->actionPointCost = 7;
    hitDescription->isPenetrate = true;
    hitDescription->isSecondary = true;

    // Death Blossom Kick
    hitDescription = &(gUnarmedHitDescriptions[HIT_MODE_PIERCING_KICK]);
    hitDescription->requiredLevel = 15;
    hitDescription->requiredSkill = 125;
    hitDescription->requiredStats[STAT_STRENGTH] = 6;
    hitDescription->requiredStats[STAT_AGILITY] = 8;
    hitDescription->minDamage = 1;
    hitDescription->maxDamage = 2;
    hitDescription->bonusDamage = 12;
    hitDescription->bonusCriticalChance = 50;
    hitDescription->actionPointCost = 9;
    hitDescription->isPenetrate = true;
    hitDescription->isSecondary = true;
}

static void unarmedInitCustom()
{
    char* unarmedFileName = nullptr;
    configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_UNARMED_FILE_KEY, &unarmedFileName);
    if (unarmedFileName != nullptr && *unarmedFileName == '\0') {
        unarmedFileName = nullptr;
    }

    if (unarmedFileName == nullptr) {
        return;
    }

    Config unarmedConfig;
    if (configInit(&unarmedConfig)) {
        if (configRead(&unarmedConfig, unarmedFileName, false)) {
            char section[4];
            char statKey[6];

            for (int hitMode = 0; hitMode < HIT_MODE_COUNT; hitMode++) {
                if (!isUnarmedHitMode(hitMode)) {
                    continue;
                }

                UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[hitMode]);
                snprintf(section, sizeof(section), "%d", hitMode);

                configGetInt(&unarmedConfig, section, "ReqLevel", &(hitDescription->requiredLevel));
                configGetInt(&unarmedConfig, section, "SkillLevel", &(hitDescription->requiredSkill));
                configGetInt(&unarmedConfig, section, "MinDamage", &(hitDescription->minDamage));
                configGetInt(&unarmedConfig, section, "MaxDamage", &(hitDescription->maxDamage));
                configGetInt(&unarmedConfig, section, "BonusDamage", &(hitDescription->bonusDamage));
                configGetInt(&unarmedConfig, section, "BonusCrit", &(hitDescription->bonusCriticalChance));
                configGetInt(&unarmedConfig, section, "APCost", &(hitDescription->actionPointCost));
                configGetBool(&unarmedConfig, section, "BonusDamage", &(hitDescription->isPenetrate));
                configGetBool(&unarmedConfig, section, "Secondary", &(hitDescription->isSecondary));

                for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
                    snprintf(statKey, sizeof(statKey), "Stat%d", stat);
                    configGetInt(&unarmedConfig, section, statKey, &(hitDescription->requiredStats[stat]));
                }
            }
        }

        configFree(&unarmedConfig);
    }
}

int unarmedGetDamage(int hitMode, int* minDamagePtr, int* maxDamagePtr)
{
    UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[hitMode]);
    *minDamagePtr = hitDescription->minDamage;
    *maxDamagePtr = hitDescription->maxDamage;
    return hitDescription->bonusDamage;
}

int unarmedGetBonusCriticalChance(int hitMode)
{
    UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[hitMode]);
    return hitDescription->bonusCriticalChance;
}

int unarmedGetActionPointCost(int hitMode)
{
    UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[hitMode]);
    return hitDescription->actionPointCost;
}

bool unarmedIsPenetrating(int hitMode)
{
    UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[hitMode]);
    return hitDescription->isPenetrate;
}

int unarmedGetPunchHitMode(bool isSecondary)
{
    int hitMode = unarmedGetHitModeInRange(FIRST_ADVANCED_PUNCH_HIT_MODE, LAST_ADVANCED_PUNCH_HIT_MODE, isSecondary);
    if (hitMode == -1) {
        hitMode = HIT_MODE_PUNCH;
    }
    return hitMode;
}

int unarmedGetKickHitMode(bool isSecondary)
{
    int hitMode = unarmedGetHitModeInRange(FIRST_ADVANCED_KICK_HIT_MODE, LAST_ADVANCED_KICK_HIT_MODE, isSecondary);
    if (hitMode == -1) {
        hitMode = HIT_MODE_KICK;
    }
    return hitMode;
}

static int unarmedGetHitModeInRange(int firstHitMode, int lastHitMode, bool isSecondary)
{
    int hitMode = -1;

    int unarmed = skillGetValue(gDude, SKILL_UNARMED);
    int level = pcGetStat(PC_STAT_LEVEL);
    int stats[PRIMARY_STAT_COUNT];
    for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
        stats[stat] = critterGetStat(gDude, stat);
    }

    for (int candidateHitMode = firstHitMode; candidateHitMode <= lastHitMode; candidateHitMode++) {
        UnarmedHitDescription* hitDescription = &(gUnarmedHitDescriptions[candidateHitMode]);
        if (isSecondary != hitDescription->isSecondary) {
            continue;
        }

        if (unarmed < hitDescription->requiredSkill) {
            continue;
        }

        if (level < hitDescription->requiredLevel) {
            continue;
        }

        bool missingStats = false;
        for (int stat = 0; stat < PRIMARY_STAT_COUNT; stat++) {
            if (stats[stat] < hitDescription->requiredStats[stat]) {
                missingStats = true;
                break;
            }
        }
        if (missingStats) {
            continue;
        }

        hitMode = candidateHitMode;
    }

    return hitMode;
}

static void damageModInit()
{
    gDamageCalculationType = DAMAGE_CALCULATION_TYPE_VANILLA;
    configGetInt(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_DAMAGE_MOD_FORMULA_KEY, &gDamageCalculationType);

    gBonusHthDamageFix = true;
    configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_BONUS_HTH_DAMAGE_FIX_KEY, &gBonusHthDamageFix);

    gDisplayBonusDamage = false;
    configGetBool(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_DISPLAY_BONUS_DAMAGE_KEY, &gDisplayBonusDamage);
}

bool damageModGetBonusHthDamageFix()
{
    return gBonusHthDamageFix;
}

bool damageModGetDisplayBonusDamage()
{
    return gDisplayBonusDamage;
}

static void damageModCalculateGlovz(DamageCalculationContext* context)
{
    int ammoX = weaponGetAmmoDamageMultiplier(context->attack->weapon);
    if (ammoX <= 0) {
        ammoX = 1;
    }

    int ammoY = weaponGetAmmoDamageDivisor(context->attack->weapon);
    if (ammoY <= 0) {
        ammoY = 1;
    }

    int ammoDamageResistance = weaponGetAmmoDamageResistanceModifier(context->attack->weapon);
    if (ammoDamageResistance > 0) {
        ammoDamageResistance = -ammoDamageResistance;
    }

    int calculatedDamageThreshold = context->damageThreshold;
    if (calculatedDamageThreshold > 0) {
        calculatedDamageThreshold = damageModGlovzDivRound(calculatedDamageThreshold, ammoY);
    }

    int calculatedDamageResistance = context->damageResistance;
    if (calculatedDamageResistance > 0) {
        if (context->combatDifficultyDamageModifier > 100) {
            calculatedDamageResistance -= 20;
        } else if (context->combatDifficultyDamageModifier < 100) {
            calculatedDamageResistance += 20;
        }

        calculatedDamageResistance += ammoDamageResistance;

        calculatedDamageResistance = damageModGlovzDivRound(calculatedDamageResistance, ammoX);

        if (calculatedDamageResistance >= 100) {
            return;
        }
    }

    for (int index = 0; index < context->ammoQuantity; index++) {
        int damage = weaponGetDamage(context->attack->attacker, context->attack->hitMode);

        damage += context->damageBonus;
        if (damage <= 0) {
            continue;
        }

        if (context->damageThreshold > 0) {
            damage -= calculatedDamageThreshold;
            if (damage <= 0) {
                continue;
            }
        }

        if (context->damageResistance > 0) {
            damage -= damageModGlovzDivRound(damage * calculatedDamageResistance, 100);
            if (damage <= 0) {
                continue;
            }
        }

        if (context->damageThreshold <= 0 && context->damageResistance <= 0) {
            if (ammoX > 1 && ammoY > 1) {
                damage += damageModGlovzDivRound(damage * 15, 100);
            } else if (ammoX > 1) {
                damage += damageModGlovzDivRound(damage * 20, 100);
            } else if (ammoY > 1) {
                damage += damageModGlovzDivRound(damage * 10, 100);
            }
        }

        if (gDamageCalculationType == DAMAGE_CALCULATION_TYPE_GLOVZ_WITH_DAMAGE_MULTIPLIER_TWEAK) {
            damage += damageModGlovzDivRound(damage * context->bonusDamageMultiplier * 25, 100);
        } else {
            damage += damage * context->bonusDamageMultiplier / 2;
        }

        if (damage > 0) {
            *context->damagePtr += damage;
        }
    }
}

static int damageModGlovzDivRound(int dividend, int divisor)
{
    if (dividend < divisor) {
        return dividend != divisor && dividend * 2 <= divisor ? 0 : 1;
    }

    int quotient = dividend / divisor;
    dividend %= divisor;

    if (dividend == 0) {
        return quotient;
    }

    dividend *= 2;

    if (dividend > divisor || (dividend == divisor && (quotient & 1) != 0)) {
        quotient += 1;
    }

    return quotient;
}

static void damageModCalculateYaam(DamageCalculationContext* context)
{
    int damageMultiplier = context->bonusDamageMultiplier * weaponGetAmmoDamageMultiplier(context->attack->weapon);
    int damageDivisor = weaponGetAmmoDamageDivisor(context->attack->weapon);

    int ammoDamageResistance = weaponGetAmmoDamageResistanceModifier(context->attack->weapon);

    int calculatedDamageThreshold = context->damageThreshold - ammoDamageResistance;
    int damageResistance = calculatedDamageThreshold;

    if (calculatedDamageThreshold >= 0) {
        damageResistance = 0;
    } else {
        calculatedDamageThreshold = 0;
        damageResistance *= 10;
    }

    int calculatedDamageResistance = context->damageResistance + damageResistance;
    if (calculatedDamageResistance < 0) {
        calculatedDamageResistance = 0;
    } else if (calculatedDamageResistance >= 100) {
        return;
    }

    for (int index = 0; index < context->ammoQuantity; index++) {
        int damage = weaponGetDamage(context->attack->attacker, context->attack->hitMode);
        damage += context->damageBonus;

        damage -= calculatedDamageThreshold;
        if (damage <= 0) {
            continue;
        }

        damage *= damageMultiplier;
        if (damageDivisor != 0) {
            damage /= damageDivisor;
        }

        damage /= 2;
        damage *= context->combatDifficultyDamageModifier;
        damage /= 100;

        damage -= damage * damageResistance / 100;

        if (damage > 0) {
            context->damagePtr += damage;
        }
    }
}

int combat_get_hit_location_penalty(int hit_location)
{
    if (hit_location >= 0 && hit_location < HIT_LOCATION_COUNT) {
        return hit_location_penalty[hit_location];
    } else {
        return 0;
    }
}

void combat_set_hit_location_penalty(int hit_location, int penalty)
{
    if (hit_location >= 0 && hit_location < HIT_LOCATION_COUNT) {
        hit_location_penalty[hit_location] = penalty;
    }
}

void combat_reset_hit_location_penalty()
{
    for (int hit_location = 0; hit_location < HIT_LOCATION_COUNT; hit_location++) {
        hit_location_penalty[hit_location] = hit_location_penalty_default[hit_location];
    }
}

Attack* combat_get_data()
{
    return &_main_ctd;
}

} // namespace fallout
