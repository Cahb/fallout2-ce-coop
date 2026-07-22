#ifndef FALLOUT_TILE_RENDER_H_
#define FALLOUT_TILE_RENDER_H_

#include "geometry.h"
#include "map.h"
#include "tile.h"

namespace fallout {

// Client-side render seam for tile.cc (REWRITE_PLAN, TU split).
//
// The pixel/blit render layer that used to live in tile.cc now lives in
// tile_render.cc (the f2_client side). The public render entry points
// (tileWindowRefreshRect/tileWindowRefresh, tileRenderRoofsInRect,
// tileRenderFloorsInRect, _grid_render) stay declared in tile.h since existing
// callers include that.
//
// This header declares the pieces the split newly exposes:
//  - the two elevation-refresh procs that core tileInit wires into the
//    refresh proc pointer by name (tileRefreshGame/tileRefreshMapper);
//  - extern views of the tile.cc state the render layer reads. Those
//    definitions remain in tile.cc (the sim owns and populates them); the
//    render layer only reads them. This one-way exposure is the mechanical
//    stand-in for the eventual client-init hook (see CMake split task).

// Elevation-refresh procs assigned to gTileWindowRefreshElevationProc by core
// tileInit. Their bodies live in tile_render.cc but must be visible to tile.cc.
void tileRefreshGame(Rect* rect, int elevation);
void tileRefreshMapper(Rect* rect, int elevation);

// tile.cc state consumed (read-only) by the render layer.
extern unsigned char* gTileWindowBuffer;
extern int gTileWindowPitch;
extern int gTileWindowWidth;
extern int gTileWindowHeight;
extern Rect gTileWindowRect;
extern TileData** gTileSquares;
extern int gSquareGridWidth;
extern int gSquareGridHeight;
extern bool gTileRoofIsVisible;
extern bool gTileGridIsVisible;
extern bool gTileEnabled;
extern TileWindowRefreshProc* gTileWindowRefreshProc;
extern TileWindowRefreshElevationProc* gTileWindowRefreshElevationProc;
extern unsigned char _tile_grid_blocked[512];
extern unsigned char _tile_grid_occupied[512];

} // namespace fallout

#endif /* FALLOUT_TILE_RENDER_H_ */
