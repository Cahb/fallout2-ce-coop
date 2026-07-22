#ifndef FALLOUT_SERVER_WORLDMAP_H_
#define FALLOUT_SERVER_WORLDMAP_H_

#include <functional>

namespace fallout {

int worldmapServerDriver();

void worldmapSetServerPump(std::function<bool()> pump);

bool worldmapServerActive();

} // namespace fallout

#endif /* FALLOUT_SERVER_WORLDMAP_H_ */