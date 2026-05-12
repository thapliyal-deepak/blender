/* source/blender/io/usd/intern/usd_stage/usd_xform_sync.hh */
#pragma once

#include "DNA_object_types.h"
#include <pxr/usd/usd/prim.h>

namespace blender::io::usd {

void sync_xform_pull(pxr::UsdPrim prim, struct blender::Object *ob);
void sync_xform_push(struct blender::Object *ob, pxr::UsdPrim prim);

}  // namespace blender::io::usd
