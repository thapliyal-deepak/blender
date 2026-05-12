/* source/blender/io/usd/intern/usd_stage/usd_authoring_sync.hh */
#pragma once

#include <pxr/usd/usd/prim.h>

/* Forward declaration */
struct Mesh;

namespace blender::io::usd {

/* Match the signature exactly */
void sync_mesh_push(struct Mesh *me, pxr::UsdPrim prim, int dirty_bits);

}  // namespace blender::io::usd
