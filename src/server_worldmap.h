#ifndef FALLOUT_SERVER_WORLDMAP_H_
#define FALLOUT_SERVER_WORLDMAP_H_

#include <functional>

namespace fallout {

int worldmapServerDriver();

// Baseline companion: (re)send the worldmap knowledge every joiner needs
// (visited/known cities, subtile fog). Forces the subtile emit so a fresh
// viewer gets it even when nothing changed since the last emit.
void worldmapServerEmitBaseline();
// Beat hook: (re)send the city table when it changed (scripts mark towns known outside the worldmap screen).
void worldmapServerEmitAreasIfChanged();

void worldmapSetServerPump(std::function<bool()> pump);

bool worldmapServerActive();

} // namespace fallout

#endif /* FALLOUT_SERVER_WORLDMAP_H_ */