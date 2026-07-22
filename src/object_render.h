#ifndef FALLOUT_OBJECT_RENDER_H_
#define FALLOUT_OBJECT_RENDER_H_

#include "geometry.h"
#include "map_defs.h"
#include "obj_types.h"

namespace fallout {

// Client-side render seam for object.cc (Batch 7 / REWRITE_PLAN 1.4).
//
// The render/blit code that used to live in object.cc now lives in
// object_render.cc (the f2_client side). The two render entry points
// (_obj_render_pre_roof / _obj_render_post_roof) and the pixel blenders
// (_translucent/_dark/_dark_translucent/_intensity_mask _buf_to_buf) stay
// declared in object.h since existing callers include that.
//
// This header declares the pieces the split newly exposes:
//  - the render/blend table lifecycle called by core objectsInit/objectsExit;
//  - extern views of the object.cc state the render layer reads. Those
//    definitions remain in object.cc (the sim owns and populates them); the
//    render layer only reads them. This one-way exposure is the mechanical
//    stand-in for the eventual client-init hook (see CMake split task).

// Render/blend table lifecycle. Called by core objectsInit/objectsExit.
// (_obj_light_table_init stays in object.cc — it fills the light-sim offsets,
// not a render table.)
int _obj_render_table_init();
void _obj_render_table_exit();
void _obj_blend_table_init();
void _obj_blend_table_exit();

// object.cc state consumed (read-only) by the render layer.
extern bool gObjectsInitialized;
extern int gObjectsUpdateAreaHexSize;
extern int* _orderTable[2];
extern int* _offsetTable[2];
extern int* _offsetDivTable;
extern int* _offsetModTable;
extern ObjectListNode* gObjectListHead;
extern ObjectListNode* gObjectListHeadByTile[HEX_GRID_SIZE];
extern Rect gObjectsWindowRect;
extern unsigned char* gObjectsWindowBuffer;
extern int gObjectsWindowPitch;
extern int gObjectsWindowBufferSize;

} // namespace fallout

#endif /* FALLOUT_OBJECT_RENDER_H_ */
