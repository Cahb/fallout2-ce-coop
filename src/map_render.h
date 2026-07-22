#ifndef FALLOUT_MAP_RENDER_H_
#define FALLOUT_MAP_RENDER_H_

#include "geometry.h"
#include "map.h"

namespace fallout {

// Client-side render seam for map.cc (REWRITE_PLAN, TU split).
//
// The iso-window/blit/scroll layer that used to live in map.cc now lives in
// map_render.cc (the f2_client side). The public render entry points
// (isoInit/isoExit/mapScroll) stay declared in map.h since existing callers
// include that.
//
// This header declares the pieces the split newly exposes:
//  - the two elevation-refresh procs that core _map_init wires into the
//    _map_scroll_refresh proc pointer by name (isoWindowRefreshRectGame/Mapper);
//  - the two core helpers the moved isoInit calls by name (square_init /
//    mapMakeMapsDirectory), whose definitions stay in map.cc;
//  - an extern view of the _map_scroll_refresh proc pointer, which core map.cc
//    owns (defines and assigns) and the render layer reads. This one-way
//    exposure is the mechanical stand-in for the eventual client-init hook
//    (see CMake split task).

// Refresh procs assigned to _map_scroll_refresh by core _map_init (and used as
// the _map_scroll_refresh initializer). Their bodies live in map_render.cc but
// must be visible to map.cc by name.
void isoWindowRefreshRectGame(Rect* rect);
void isoWindowRefreshRectMapper(Rect* rect);

// Core map.cc helpers called by the moved isoInit. Definitions stay in map.cc.
void square_init();
void mapMakeMapsDirectory();

// map.cc state consumed (read-only) by the render layer. The definition stays
// in map.cc (the sim owns it); the render layer only reads it.
extern IsoWindowRefreshProc* _map_scroll_refresh;

} // namespace fallout

#endif /* FALLOUT_MAP_RENDER_H_ */
