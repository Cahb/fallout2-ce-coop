#include "path.h"

#include <stdlib.h>
#include <string.h>

#include "combat.h"
#include "critter.h"
#include "geometry.h"
#include "map.h"
#include "object.h"
#include "proto.h"
#include "proto_instance.h"
#include "tile.h"

// A* pathfinder + straight-line path builders extracted from animation.cc
// (REWRITE_PLAN 2.1). Pure hex-grid geometry + object-blocking queries — no
// SDL, lives in f2_core. `canUseDoor` moved here too: it's door-passability
// sim logic the pathfinder needs, and keeping it here preserves the client
// (animation.cc) -> core (path.cc) call direction. This move is mechanical
// and replay-identical.

namespace fallout {

typedef struct PathNode {
    int tile;
    int from;
    // actual type is likely char
    int rotation;
    int estimate;
    int cost;
} PathNode;

static int _idist(int a1, int a2, int a3, int a4);
static int _tile_idistance(int tile1, int tile2);

// 0x542FD4
// ►► TWO A* EXPLORATION BUDGETS, and the difference is the whole point.
//
// pathfinderFindPath returns 0 — "no route" — the moment it has expanded this many tiles.
// Vanilla's 2000 is small: a walk across a big map, or any route that has to feel its way
// around scenery, exhausts it while the destination is plainly walkable, and the caller
// then registers nothing. That is the "the game refuses to walk somewhere completely
// reachable, and the cursor says invalid move" report — it is not a distance check
// anywhere, it is the search giving up.
//
// A PLAYER'S OWN WALK gets the wide budget (kPathMaxNodesWide): they clicked a tile, they
// can see it, and refusing is the worst possible answer. EVERYTHING ELSE keeps vanilla's
// 2000 — AI reachability tests, script movement, party followers — because widening those
// changes what the AI decides, which is a simulation change: measured, it moves NPC routes
// on denbus1 and diverges six server goldens. Same reasoning as any other "fix the
// player's experience without editing the sim" seam.
static const int kPathMaxNodes = 2000;
static const int kPathMaxNodesWide = 12000;

// Max steps the reconstructed path may emit — VANILLA'S VALUE, deliberately unchanged.
//
// ⚠ It is not a clean bound: the reconstruction walks BACKWARDS from the destination, so
// hitting this cap yields the route's TAIL, i.e. a "path" that starts at a tile the actor
// is not standing on. I tried raising it to the callers' real buffer size (3200) and
// refusing a truncated path outright; MEASURED RESULT: an NPC that walks in vanilla
// stopped walking, six server goldens diverged. Something depends on getting that tail
// back, so tightening it is a divergence with a cost and no demonstrated benefit. Left
// alone, written down.
static const int kPathMaxSteps = 800;

static PathNode gClosedPathNodeList[kPathMaxNodesWide];

// 0x561814
static unsigned char gPathfinderProcessedTiles[5000];

// 0x562B9C
static PathNode gOpenPathNodeList[kPathMaxNodesWide];

// 0x415E24
bool canUseDoor(Object* critter, Object* door)
{
    if (critter == gDude) {
        if (!_obj_portal_is_walk_thru(door)) {
            return false;
        }
    }

    if (FID_TYPE(critter->fid) != OBJ_TYPE_CRITTER) {
        return false;
    }

    if (FID_TYPE(door->fid) != OBJ_TYPE_SCENERY) {
        return false;
    }

    int bodyType = critterGetBodyType(critter);
    if (bodyType != BODY_TYPE_BIPED && bodyType != BODY_TYPE_ROBOTIC) {
        return false;
    }

    Proto* proto;
    if (protoGetProto(door->pid, &proto) == -1) {
        return false;
    }

    if (proto->scenery.type != SCENERY_TYPE_DOOR) {
        return false;
    }

    if (objectIsLocked(door)) {
        return false;
    }

    if (critterGetKillType(critter) == KILL_TYPE_GECKO) {
        return false;
    }

    return true;
}

// 0x415EE8
int _make_path(Object* object, int from, int to, unsigned char* rotations, int a5)
{
    return pathfinderFindPath(object, from, to, rotations, a5, _obj_blocking_at);
}

// A PLAYER-REQUESTED walk: same pathfinder, wide search budget. Use this only where a
// player asked to go somewhere — see the budget comment for why the AI must not have it.
int _make_path_wide(Object* object, int from, int to, unsigned char* rotations, int a5)
{
    return pathfinderFindPathBudget(object, from, to, rotations, a5, _obj_blocking_at,
        kPathMaxNodesWide);
}

// TODO: move pathfinding into another unit
// 0x415EFC
int pathfinderFindPath(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback)
{
    return pathfinderFindPathBudget(object, from, to, rotations, a5, callback, kPathMaxNodes);
}

int pathfinderFindPathBudget(Object* object, int from, int to, unsigned char* rotations, int a5, PathBuilderCallback* callback, int maxNodes)
{
    if (maxNodes < 2 || maxNodes > kPathMaxNodesWide) {
        maxNodes = kPathMaxNodes;
    }

    if (a5) {
        if (callback(object, to, object->elevation) != nullptr) {
            return 0;
        }
    }

    bool isCritter = false;
    int critterType = 0;
    if (PID_TYPE(object->pid) == OBJ_TYPE_CRITTER) {
        isCritter = true;
        critterType = critterGetKillType(object);
    }

    bool isNotInCombat = !isInCombat();

    memset(gPathfinderProcessedTiles, 0, sizeof(gPathfinderProcessedTiles));

    gPathfinderProcessedTiles[from / 8] |= 1 << (from & 7);

    gOpenPathNodeList[0].tile = from;
    gOpenPathNodeList[0].from = -1;
    gOpenPathNodeList[0].rotation = 0;
    gOpenPathNodeList[0].estimate = _tile_idistance(from, to);
    gOpenPathNodeList[0].cost = 0;

    for (int index = 1; index < maxNodes; index += 1) {
        gOpenPathNodeList[index].tile = -1;
    }

    int toScreenX;
    int toScreenY;
    tileToScreenXY(to, &toScreenX, &toScreenY, object->elevation);

    int closedPathNodeListLength = 0;
    int openPathNodeListLength = 1;
    PathNode temp;

    while (1) {
        int v63 = -1;

        PathNode* prev = nullptr;
        int v12 = 0;
        for (int index = 0; v12 < openPathNodeListLength; index += 1) {
            PathNode* curr = &(gOpenPathNodeList[index]);
            if (curr->tile != -1) {
                v12++;
                if (v63 == -1 || (curr->estimate + curr->cost) < (prev->estimate + prev->cost)) {
                    prev = curr;
                    v63 = index;
                }
            }
        }

        PathNode* curr = &(gOpenPathNodeList[v63]);

        memcpy(&temp, curr, sizeof(temp));

        openPathNodeListLength -= 1;

        curr->tile = -1;

        if (temp.tile == to) {
            if (openPathNodeListLength == 0) {
                openPathNodeListLength = 1;
            }
            break;
        }

        PathNode* curr1 = &(gClosedPathNodeList[closedPathNodeListLength]);
        memcpy(curr1, &temp, sizeof(temp));

        closedPathNodeListLength += 1;

        if (closedPathNodeListLength == maxNodes) {
            return 0;
        }

        for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
            int tile = tileGetTileInDirection(temp.tile, rotation, 1);
            int bit = 1 << (tile & 7);
            if ((gPathfinderProcessedTiles[tile / 8] & bit) != 0) {
                continue;
            }

            if (tile != to) {
                Object* v24 = callback(object, tile, object->elevation);
                if (v24 != nullptr) {
                    if (!canUseDoor(object, v24)) {
                        continue;
                    }
                }
            }

            int v25 = 0;
            for (; v25 < maxNodes; v25++) {
                if (gOpenPathNodeList[v25].tile == -1) {
                    break;
                }
            }

            openPathNodeListLength += 1;

            if (openPathNodeListLength == maxNodes) {
                return 0;
            }

            gPathfinderProcessedTiles[tile / 8] |= bit;

            PathNode* v27 = &(gOpenPathNodeList[v25]);
            v27->tile = tile;
            v27->from = temp.tile;
            v27->rotation = rotation;

            int newX;
            int newY;
            tileToScreenXY(tile, &newX, &newY, object->elevation);

            v27->estimate = _idist(newX, newY, toScreenX, toScreenY);
            v27->cost = temp.cost + 50;

            if (isNotInCombat && temp.rotation != rotation) {
                v27->cost += 10;
            }

            if (isCritter) {
                Object* o = objectFindFirstAtLocation(object->elevation, v27->tile);
                while (o != nullptr) {
                    if (o->pid >= FIRST_RADIOACTIVE_GOO_PID && o->pid <= LAST_RADIOACTIVE_GOO_PID) {
                        break;
                    }
                    o = objectFindNextAtLocation();
                }

                if (o != nullptr) {
                    if (critterType == KILL_TYPE_GECKO) {
                        v27->cost += 100;
                    } else {
                        v27->cost += 400;
                    }
                }
            }
        }

        if (openPathNodeListLength == 0) {
            break;
        }
    }

    if (openPathNodeListLength != 0) {
        unsigned char* v39 = rotations;
        int index = 0;
        for (; index < kPathMaxSteps; index++) {
            if (temp.tile == from) {
                break;
            }

            if (v39 != nullptr) {
                *v39 = temp.rotation & 0xFF;
                v39 += 1;
            }

            int j = 0;
            while (gClosedPathNodeList[j].tile != temp.from) {
                j++;
            }

            PathNode* v36 = &(gClosedPathNodeList[j]);
            memcpy(&temp, v36, sizeof(temp));
        }

        if (rotations != nullptr) {
            // Looks like array resevering, probably because A* finishes it's path from end to start,
            // this probably reverses it start-to-end.
            unsigned char* beginning = rotations;
            unsigned char* ending = rotations + index - 1;
            int middle = index / 2;
            for (int index = 0; index < middle; index++) {
                unsigned char rotation = *ending;
                *ending = *beginning;
                *beginning = rotation;

                ending -= 1;
                beginning += 1;
            }
        }

        return index;
    }

    return 0;
}

// 0x41633C
static int _idist(int x1, int y1, int x2, int y2)
{
    int dx = x2 - x1;
    if (dx < 0) {
        dx = -dx;
    }

    int dy = y2 - y1;
    if (dy < 0) {
        dy = -dy;
    }

    int dm = (dx <= dy) ? dx : dy;

    return dx + dy - (dm / 2);
}

// 0x416360
static int _tile_idistance(int tile1, int tile2)
{
    int x1;
    int y1;
    tileToScreenXY(tile1, &x1, &y1, gElevation);

    int x2;
    int y2;
    tileToScreenXY(tile2, &x2, &y2, gElevation);

    return _idist(x1, y1, x2, y2);
}

// 0x4163AC
int _make_straight_path(Object* obj, int from, int to, StraightPathNode* straightPathNodeList, Object** obstaclePtr, int a6)
{
    return _make_straight_path_func(obj, from, to, straightPathNodeList, obstaclePtr, a6, _obj_blocking_at);
}

// TODO: Rather complex, but understandable, needs testing.
//
// 0x4163C8
int _make_straight_path_func(Object* obj, int from, int to, StraightPathNode* straightPathNodeList, Object** obstaclePtr, int a6, PathBuilderCallback* callback)
{
    if (obstaclePtr != nullptr) {
        Object* obstacle = callback(obj, from, obj->elevation);
        if (obstacle != nullptr) {
            if (obstacle != *obstaclePtr && (a6 != 32 || (obstacle->flags & OBJECT_SHOOT_THRU) == 0)) {
                *obstaclePtr = obstacle;
                return 0;
            }
        }
    }

    int fromX;
    int fromY;
    tileToScreenXY(from, &fromX, &fromY, obj->elevation);
    fromX += 16;
    fromY += 8;

    int toX;
    int toY;
    tileToScreenXY(to, &toX, &toY, obj->elevation);
    toX += 16;
    toY += 8;

    int stepX;
    int deltaX = toX - fromX;
    if (deltaX > 0) {
        stepX = 1;
    } else if (deltaX < 0) {
        stepX = -1;
    } else {
        stepX = 0;
    }

    int stepY;
    int deltaY = toY - fromY;
    if (deltaY > 0) {
        stepY = 1;
    } else if (deltaY < 0) {
        stepY = -1;
    } else {
        stepY = 0;
    }

    int ddx = 2 * abs(toX - fromX);
    int ddy = 2 * abs(toY - fromY);

    int tileX = fromX;
    int tileY = fromY;

    int pathNodeIndex = 0;
    int prevTile = from;
    int v22 = 0;
    int tile;

    if (ddx <= ddy) {
        int middle = ddx - ddy / 2;
        while (true) {
            tile = tileFromScreenXY(tileX, tileY, obj->elevation);

            v22 += 1;
            if (v22 == a6) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (straightPathNodeList != nullptr) {
                    StraightPathNode* pathNode = &(straightPathNodeList[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = obj->elevation;

                    tileToScreenXY(tile, &fromX, &fromY, obj->elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                v22 = 0;
                pathNodeIndex++;
            }

            if (tileY == toY) {
                if (obstaclePtr != nullptr) {
                    *obstaclePtr = nullptr;
                }
                break;
            }

            if (middle >= 0) {
                tileX += stepX;
                middle -= ddy;
            }

            tileY += stepY;
            middle += ddx;

            if (tile != prevTile) {
                if (obstaclePtr != nullptr) {
                    Object* obstacle = callback(obj, tile, obj->elevation);
                    if (obstacle != nullptr) {
                        if (obstacle != *obstaclePtr && (a6 != 32 || (obstacle->flags & OBJECT_SHOOT_THRU) == 0)) {
                            *obstaclePtr = obstacle;
                            break;
                        }
                    }
                }
                prevTile = tile;
            }
        }
    } else {
        int middle = ddy - ddx / 2;
        while (true) {
            tile = tileFromScreenXY(tileX, tileY, obj->elevation);

            v22 += 1;
            if (v22 == a6) {
                if (pathNodeIndex >= 200) {
                    return 0;
                }

                if (straightPathNodeList != nullptr) {
                    StraightPathNode* pathNode = &(straightPathNodeList[pathNodeIndex]);
                    pathNode->tile = tile;
                    pathNode->elevation = obj->elevation;

                    tileToScreenXY(tile, &fromX, &fromY, obj->elevation);
                    pathNode->x = tileX - fromX - 16;
                    pathNode->y = tileY - fromY - 8;
                }

                v22 = 0;
                pathNodeIndex++;
            }

            if (tileX == toX) {
                if (obstaclePtr != nullptr) {
                    *obstaclePtr = nullptr;
                }
                break;
            }

            if (middle >= 0) {
                tileY += stepY;
                middle -= ddx;
            }

            tileX += stepX;
            middle += ddy;

            if (tile != prevTile) {
                if (obstaclePtr != nullptr) {
                    Object* obstacle = callback(obj, tile, obj->elevation);
                    if (obstacle != nullptr) {
                        if (obstacle != *obstaclePtr && (a6 != 32 || (obstacle->flags & OBJECT_SHOOT_THRU) == 0)) {
                            *obstaclePtr = obstacle;
                            break;
                        }
                    }
                }
                prevTile = tile;
            }
        }
    }

    if (v22 != 0) {
        if (pathNodeIndex >= 200) {
            return 0;
        }

        if (straightPathNodeList != nullptr) {
            StraightPathNode* pathNode = &(straightPathNodeList[pathNodeIndex]);
            pathNode->tile = tile;
            pathNode->elevation = obj->elevation;

            tileToScreenXY(tile, &fromX, &fromY, obj->elevation);
            pathNode->x = tileX - fromX - 16;
            pathNode->y = tileY - fromY - 8;
        }

        pathNodeIndex += 1;
    } else {
        if (pathNodeIndex > 0 && straightPathNodeList != nullptr) {
            straightPathNodeList[pathNodeIndex - 1].elevation = obj->elevation;
        }
    }

    return pathNodeIndex;
}

} // namespace fallout
