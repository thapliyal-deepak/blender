#pragma once

#include <pxr/usd/usd/prim.h>

namespace blender {
struct Mesh;
struct BMesh;
}  // namespace blender

namespace blender::io::usd {

/** Synchronize USD geometry to a Blender Mesh (Pull). */
bool sync_mesh_pull(pxr::UsdPrim prim, Mesh *me, int dirty_bits);

/** Synchronize Blender Mesh edits to a USD Prim (Push). */
bool sync_mesh_push(Mesh *me, pxr::UsdPrim prim, int dirty_bits);

/** Push Edit-Mode vertex positions from a BMesh directly to USD (no base-mesh write-back needed). */
bool sync_mesh_push_bm(BMesh *bm, pxr::UsdPrim prim);

}  // namespace blender::io::usd
