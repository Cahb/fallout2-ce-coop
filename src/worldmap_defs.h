#ifndef FALLOUT_WORLDMAP_DEFS_H_
#define FALLOUT_WORLDMAP_DEFS_H_

#include "art.h"
#include "db.h"
#include "message.h"

namespace fallout {

#define CITY_NAME_SIZE (40)
#define TILE_WALK_MASK_NAME_SIZE (40)
#define ENTRANCE_LIST_CAPACITY (10)

// Up from 6 to handle `Tartar 3rd Floor 2` and `Livos Living Rooms` sfx
// configuration in Olympus.
#define MAP_AMBIENT_SOUND_EFFECTS_CAPACITY (7)
#define MAP_STARTING_POINTS_CAPACITY (15)

#define SUBTILE_GRID_WIDTH (7)
#define SUBTILE_GRID_HEIGHT (6)

#define ENCOUNTER_ENTRY_SPECIAL (0x01)

#define ENCOUNTER_SUBINFO_DEAD (0x01)

#define WM_WINDOW_DIAL_X (532)
#define WM_WINDOW_DIAL_Y (48)

#define WM_TOWN_LIST_SCROLL_UP_X (480)
#define WM_TOWN_LIST_SCROLL_UP_Y (137)

#define WM_TOWN_LIST_SCROLL_DOWN_X (WM_TOWN_LIST_SCROLL_UP_X)
#define WM_TOWN_LIST_SCROLL_DOWN_Y (152)

#define WM_WINDOW_GLOBE_OVERLAY_X (495)
#define WM_WINDOW_GLOBE_OVERLAY_Y (330)

#define WM_WINDOW_CAR_X (514)
#define WM_WINDOW_CAR_Y (336)

#define WM_WINDOW_CAR_OVERLAY_X (499)
#define WM_WINDOW_CAR_OVERLAY_Y (330)

#define WM_WINDOW_CAR_FUEL_BAR_X (500)
#define WM_WINDOW_CAR_FUEL_BAR_Y (339)
#define WM_WINDOW_CAR_FUEL_BAR_HEIGHT (70)

#define WM_TOWN_WORLD_SWITCH_X (519)
#define WM_TOWN_WORLD_SWITCH_Y (439)

#define WM_TILE_WIDTH (350)
#define WM_TILE_HEIGHT (300)

#define WM_SUBTILE_SIZE (50)

#define WM_WINDOW_WIDTH (640)
#define WM_WINDOW_HEIGHT (480)

#define WM_VIEW_X (22)
#define WM_VIEW_Y (21)
#define WM_VIEW_WIDTH (450)
#define WM_VIEW_HEIGHT (443)

typedef enum EncounterFormationType {
    ENCOUNTER_FORMATION_TYPE_SURROUNDING,
    ENCOUNTER_FORMATION_TYPE_STRAIGHT_LINE,
    ENCOUNTER_FORMATION_TYPE_DOUBLE_LINE,
    ENCOUNTER_FORMATION_TYPE_WEDGE,
    ENCOUNTER_FORMATION_TYPE_CONE,
    ENCOUNTER_FORMATION_TYPE_HUDDLE,
    ENCOUNTER_FORMATION_TYPE_COUNT,
} EncounterFormationType;

typedef enum EncounterFrequencyType {
    ENCOUNTER_FREQUENCY_TYPE_NONE,
    ENCOUNTER_FREQUENCY_TYPE_RARE,
    ENCOUNTER_FREQUENCY_TYPE_UNCOMMON,
    ENCOUNTER_FREQUENCY_TYPE_COMMON,
    ENCOUNTER_FREQUENCY_TYPE_FREQUENT,
    ENCOUNTER_FREQUENCY_TYPE_FORCED,
    ENCOUNTER_FREQUENCY_TYPE_COUNT,
} EncounterFrequencyType;

typedef enum EncounterSceneryType {
    ENCOUNTER_SCENERY_TYPE_NONE,
    ENCOUNTER_SCENERY_TYPE_LIGHT,
    ENCOUNTER_SCENERY_TYPE_NORMAL,
    ENCOUNTER_SCENERY_TYPE_HEAVY,
    ENCOUNTER_SCENERY_TYPE_COUNT,
} EncounterSceneryType;

typedef enum EncounterSituation {
    ENCOUNTER_SITUATION_NOTHING,
    ENCOUNTER_SITUATION_AMBUSH,
    ENCOUNTER_SITUATION_FIGHTING,
    ENCOUNTER_SITUATION_AND,
    ENCOUNTER_SITUATION_COUNT,
} EncounterSituation;

typedef enum EncounterLogicalOperator {
    ENCOUNTER_LOGICAL_OPERATOR_NONE,
    ENCOUNTER_LOGICAL_OPERATOR_AND,
    ENCOUNTER_LOGICAL_OPERATOR_OR,
} EncounterLogicalOperator;

typedef enum EncounterConditionType {
    ENCOUNTER_CONDITION_TYPE_NONE = 0,
    ENCOUNTER_CONDITION_TYPE_GLOBAL = 1,
    ENCOUNTER_CONDITION_TYPE_NUMBER_OF_CRITTERS = 2,
    ENCOUNTER_CONDITION_TYPE_RANDOM = 3,
    ENCOUNTER_CONDITION_TYPE_PLAYER = 4,
    ENCOUNTER_CONDITION_TYPE_DAYS_PLAYED = 5,
    ENCOUNTER_CONDITION_TYPE_TIME_OF_DAY = 6,
} EncounterConditionType;

typedef enum EncounterConditionalOperator {
    ENCOUNTER_CONDITIONAL_OPERATOR_NONE,
    ENCOUNTER_CONDITIONAL_OPERATOR_EQUAL,
    ENCOUNTER_CONDITIONAL_OPERATOR_NOT_EQUAL,
    ENCOUNTER_CONDITIONAL_OPERATOR_LESS_THAN,
    ENCOUNTER_CONDITIONAL_OPERATOR_GREATER_THAN,
    ENCOUNTER_CONDITIONAL_OPERATOR_COUNT,
} EncounterConditionalOperator;

typedef enum EncounterRatioMode {
    ENCOUNTER_RATIO_MODE_USE_RATIO,
    ENCOUNTER_RATIO_MODE_SINGLE,
} EncounterRatioMode;

typedef enum Daytime {
    DAY_PART_MORNING,
    DAY_PART_AFTERNOON,
    DAY_PART_NIGHT,
    DAY_PART_COUNT,
} Daytime;

typedef enum LockState {
    LOCK_STATE_UNLOCKED,
    LOCK_STATE_LOCKED,
} LockState;

typedef enum SubtileState {
    SUBTILE_STATE_UNKNOWN,
    SUBTILE_STATE_KNOWN,
    SUBTILE_STATE_VISITED,
} SubtileState;

typedef enum SubtileFill {
    SUBTILE_FILL_NONE,
    SUBTILE_FILL_N,
    SUBTILE_FILL_S,
    SUBTILE_FILL_E,
    SUBTILE_FILL_W,
    SUBTILE_FILL_NW,
    SUBTILE_FILL_NE,
    SUBTILE_FILL_SW,
    SUBTILE_FILL_SE,
    SUBTILE_FILL_COUNT,
} SubtileFill;

typedef enum WorldMapEncounterFrm {
    WORLD_MAP_ENCOUNTER_FRM_RANDOM_BRIGHT,
    WORLD_MAP_ENCOUNTER_FRM_RANDOM_DARK,
    WORLD_MAP_ENCOUNTER_FRM_SPECIAL_BRIGHT,
    WORLD_MAP_ENCOUNTER_FRM_SPECIAL_DARK,
    WORLD_MAP_ENCOUNTER_FRM_COUNT,
} WorldMapEncounterFrm;

typedef enum WorldmapArrowFrm {
    WORLDMAP_ARROW_FRM_NORMAL,
    WORLDMAP_ARROW_FRM_PRESSED,
    WORLDMAP_ARROW_FRM_COUNT,
} WorldmapArrowFrm;

typedef enum CitySize {
    CITY_SIZE_SMALL,
    CITY_SIZE_MEDIUM,
    CITY_SIZE_LARGE,
    CITY_SIZE_COUNT,
} CitySize;

typedef struct EntranceInfo {
    int state;
    int x;
    int y;
    int map;
    int elevation;
    int tile;
    int rotation;
} EntranceInfo;

typedef struct CityInfo {
    char name[CITY_NAME_SIZE];
    int areaId;
    int x;
    int y;
    int size;
    int state;
    int lockState;
    int visitedState;
    int mapFid;
    int labelFid;
    int entrancesLength;
    EntranceInfo entrances[ENTRANCE_LIST_CAPACITY];
} CityInfo;

typedef struct MapAmbientSoundEffectInfo {
    char name[40];
    int chance;
} MapAmbientSoundEffectInfo;

typedef struct MapStartPointInfo {
    int elevation;
    int tile;
    int rotation;
} MapStartPointInfo;

typedef struct MapInfo {
    char lookupName[40];
    int field_28;
    int field_2C;
    char mapFileName[40];
    char music[40];
    int flags;
    int ambientSoundEffectsLength;
    MapAmbientSoundEffectInfo ambientSoundEffects[MAP_AMBIENT_SOUND_EFFECTS_CAPACITY];
    int startPointsLength;
    MapStartPointInfo startPoints[MAP_STARTING_POINTS_CAPACITY];
} MapInfo;

typedef struct Terrain {
    char lookupName[40];
    int difficulty;
    int mapsLength;
    int maps[20];
} Terrain;

typedef struct EncounterConditionEntry {
    int type;
    int conditionalOperator;
    int param;
    int value;
} EncounterConditionEntry;

typedef struct EncounterCondition {
    int entriesLength;
    EncounterConditionEntry entries[3];
    int logicalOperators[2];
} EncounterCondition;

typedef struct EncounterTableSubEntry {
    int minimumCount;
    int maximumCount;
    int encounterIndex;
    int situation;
} EncounterTableSubEntry;

typedef struct EncounterTableEntry {
    int flags;
    int map;
    int scenery;
    int chance;
    int counter;
    EncounterCondition condition;
    int subEntiesLength;
    EncounterTableSubEntry subEntries[6];
} EncounterTableEntry;

typedef struct EncounterTable {
    char lookupName[40];
    int index;
    int mapsLength;
    int maps[6];
    int field_48;
    int entriesLength;
    EncounterTableEntry entries[41];
} EncounterTable;

typedef struct EncounterItem {
    int pid;
    int minimumQuantity;
    int maximumQuantity;
    bool isEquipped;
} EncounterItem;

typedef struct EncounterEntry {
    char field_0[40];
    int field_28;
    int ratioMode;
    int ratio;
    int pid;
    int flags;
    int distance;
    int tile;
    int itemsLength;
    EncounterItem items[10];
    int team;
    int scriptIdx;
    EncounterCondition condition;
} EncounterEntry;

typedef struct Encounter {
    char name[40];
    int position;
    int spacing;
    int distance;
    int entriesLength;
    EncounterEntry entries[10];
} Encounter;

typedef struct SubtileInfo {
    int terrain;
    int fill;
    int encounterChance[DAY_PART_COUNT];
    int encounterType;
    int state;
} SubtileInfo;

// A worldmap tile is 7x6 area, thus consisting of 42 individual subtiles.
typedef struct TileInfo {
    int fid;
    CacheEntry* handle;
    unsigned char* data;
    char walkMaskName[TILE_WALK_MASK_NAME_SIZE];
    unsigned char* walkMaskData;
    int encounterDifficultyModifier;
    SubtileInfo subtiles[SUBTILE_GRID_HEIGHT][SUBTILE_GRID_WIDTH];
} TileInfo;

typedef struct CitySizeDescription {
    int fid;
    FrmImage frmImage;
} CitySizeDescription;

typedef struct WmGenData {
    bool mousePressed;
    bool didMeetFrankHorrigan;

    int currentAreaId;
    int worldPosX;
    int worldPosY;
    SubtileInfo* currentSubtile;

    int dword_672E18;

    bool isWalking;
    int walkDestinationX;
    int walkDestinationY;
    int walkDistance;
    int walkLineDelta;
    int walkLineDeltaMainAxisStep;
    int walkLineDeltaCrossAxisStep;
    int walkWorldPosMainAxisStepX;
    int walkWorldPosCrossAxisStepX;
    int walkWorldPosMainAxisStepY;
    int walkWorldPosCrossAxisStepY;

    bool encounterIconIsVisible;
    int encounterMapId;
    int encounterTableId;
    int encounterEntryId;
    int encounterCursorId;

    int oldWorldPosX;
    int oldWorldPosY;

    bool isInCar;
    int currentCarAreaId;
    int carFuel;

    CacheEntry* carImageFrmHandle;
    Art* carImageFrm;
    int carImageFrmWidth;
    int carImageFrmHeight;
    int carImageCurrentFrameIndex;

    FrmImage hotspotNormalFrmImage;
    FrmImage hotspotPressedFrmImage;

    FrmImage destinationMarkerFrmImage;
    FrmImage locationMarkerFrmImage;

    FrmImage encounterCursorFrmImages[WORLD_MAP_ENCOUNTER_FRM_COUNT];

    int viewportMaxX;
    int viewportMaxY;

    FrmImage tabsBackgroundFrmImage;
    int tabsOffsetY;

    FrmImage tabsBorderFrmImage;

    CacheEntry* dialFrmHandle;
    int dialFrmWidth;
    int dialFrmHeight;
    int dialFrmCurrentFrameIndex;
    Art* dialFrm;

    FrmImage carOverlayFrmImage;
    FrmImage globeOverlayFrmImage;

    int oldTabsOffsetY;
    int tabsScrollingDelta;

    FrmImage redButtonNormalFrmImage;
    FrmImage redButtonPressedFrmImage;

    FrmImage scrollUpButtonFrmImages[WORLDMAP_ARROW_FRM_COUNT];
    FrmImage scrollDownButtonFrmImages[WORLDMAP_ARROW_FRM_COUNT];

    FrmImage monthsFrmImage;
    FrmImage numbersFrmImage;

    int oldFont;
} WmGenData;

} // namespace fallout

#endif /* FALLOUT_WORLDMAP_DEFS_H_ */
