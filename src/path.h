#ifndef FALLOUT_PATH_H_
#define FALLOUT_PATH_H_

#include "obj_types.h"

namespace fallout {

// A* pathfinder + straight-line path builders, extracted from animation.cc
// (REWRITE_PLAN 2.1). Pure hex-grid geometry + object-blocking queries —
// SDL-free, lives in f2_core.

typedef struct StraightPathNode {
    int tile;
    int elevation;
    int x;
    int y;
} StraightPathNode;

typedef Object* PathBuilderCallback(Object* object, int tile, int elevation);

bool canUseDoor(Object* critter, Object* door);
int _make_path(Object* object, int from, int to, unsigned char* a4, int a5);
int pathfinderFindPath(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback);
int _make_straight_path(Object* object, int from, int to, StraightPathNode* straightPathNodeList, Object** obstaclePtr, int a6);
int _make_straight_path_func(Object* object, int from, int to, StraightPathNode* straightPathNodeList, Object** obstaclePtr, int a6, PathBuilderCallback* callback);

} // namespace fallout

#endif /* FALLOUT_PATH_H_ */
