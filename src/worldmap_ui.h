#ifndef FALLOUT_WORLDMAP_UI_H_
#define FALLOUT_WORLDMAP_UI_H_

// Seam between the worldmap core simulation (worldmap.cc) and its interface /
// HUD presentation (worldmap_ui.cc).
//
// NOTE: The core<->UI boundary here has INWARD by-name edges in BOTH
// directions (core calls UI functions directly, and UI calls core functions
// directly) rather than routing everything through a presenter. This is fine
// for a same-binary translation-unit split. A future link-level split would
// need to replace these direct cross-edges with presenter methods.
//
//   * SHARED mutable state: worldmap.cc owns the definitions; both TUs read and
//     write them. wmGenData in particular mixes simulation and chrome fields in
//     one struct (it is NOT read-only) -- flag for decomposition in a headless
//     split.
//   * INWARD (core -> UI): UI functions the core invokes by name; defined in
//     worldmap_ui.cc.
//   * OUTWARD (UI -> core): core functions the UI invokes by name; defined in
//     worldmap.cc.

#include "worldmap_defs.h"

namespace fallout {

// ---------------------------------------------------------------------------
// SHARED mutable state (defined in worldmap.cc, read/written by both TUs).
// ---------------------------------------------------------------------------
extern WmGenData wmGenData;
extern CityInfo* wmAreaInfoList;
extern int wmMaxAreaNum;
extern CitySizeDescription wmSphereData[CITY_SIZE_COUNT];
extern TileInfo* wmTileInfoList;
extern int wmMaxTileNum;
extern int wmNumHorizontalTiles;
extern MessageList wmMsgFile;
extern int wmWorldOffsetX;
extern int wmWorldOffsetY;
extern unsigned int wmLastRndTime;
extern bool gTownMapHotkeysFix;

// Worldmap streaming flags (viewer-side). Set by wire decoder, read by wmWorldMapFunc
// and the main loop.
extern bool gPendingWorldmapEnter;
extern bool gWorldmapStreaming;
extern bool gWorldmapStateDirty;

// ---------------------------------------------------------------------------
// INWARD (core -> UI): UI functions the core calls by name.
// Defined in worldmap_ui.cc.
// ---------------------------------------------------------------------------
bool wmCursorIsVisible();
int wmInterfaceCenterOnParty();
int wmInterfaceScrollPixel(int stepX, int stepY, int dx, int dy, bool* success, bool shouldRefresh);
void wmInterfaceDialSyncTime(bool shouldRefreshWindow);
void wmInterfaceRefreshDate(bool shouldRefreshWindow);
void wmBlinkRndEncounterIcon(bool special);
void wmFadeOut();
void wmFadeIn();
void wmFadeReset();

// ---------------------------------------------------------------------------
// OUTWARD (UI -> core): core functions the UI calls by name.
// Defined in worldmap.cc.
// ---------------------------------------------------------------------------
int wmMatchWorldPosToArea(int x, int y, int* areaIdxPtr);
int wmAreaFindFirstValidMap(int* mapIdxPtr);
int wmGetAreaName(CityInfo* city, char* name);

} // namespace fallout

#endif /* FALLOUT_WORLDMAP_UI_H_ */
