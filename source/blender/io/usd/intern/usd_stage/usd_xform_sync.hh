/* source/blender/io/usd/intern/usd_stage/usd_xform_sync.hh */
#pragma once

#include "DNA_object_types.h"
#include <pxr/usd/usd/prim.h>

namespace blender::io::usd {

void sync_xform_pull(pxr::UsdPrim prim, struct blender::Object *ob);
void sync_xform_push(struct blender::Object *ob, pxr::UsdPrim prim);
/** Returns true if the object's current Blender world matrix differs from the composed USD
 *  transform. Used by push_all_xforms() to skip writing override specs for stationary objects. */
bool sync_xform_differs(struct blender::Object *ob, const pxr::UsdPrim &prim);

}  // namespace blender::io::usd
