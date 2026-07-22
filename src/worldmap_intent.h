#ifndef FALLOUT_WORLDMAP_INTENT_H_
#define FALLOUT_WORLDMAP_INTENT_H_

namespace fallout {

enum WorldmapIntentKind {
    WM_INTENT_MOVE,
    WM_INTENT_ENTER,
    WM_INTENT_ESCAPE,
};

struct WorldmapIntent {
    int kind;
    int x;
    int y;
};

void worldmapIntentPush(int kind, int x, int y);
bool worldmapIntentPeek(WorldmapIntent* out);
void worldmapIntentPop();
bool worldmapIntentPending();
void worldmapIntentClear();

} // namespace fallout

#endif /* FALLOUT_WORLDMAP_INTENT_H_ */
