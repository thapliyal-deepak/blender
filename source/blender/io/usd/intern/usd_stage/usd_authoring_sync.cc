/* source/blender/io/usd/intern/usd_stage/usd_authoring_sync.cc */

#include "usd_authoring_sync.hh"
#include "usd_types_sync.hh  "

/* 1. Blender Hybrid/C++ headers (Must be OUTSIDE extern "C") */
/* These headers handle their own C/C++ linkage internally. */
#include "BLI_utildefines.h"
#include "MEM_guardedalloc.h"

/* 2. Standard C++ and Pixar USD headers (Never in extern "C") */
#include <pxr/usd/usdGeom/mesh.h>

#include "BKE_mesh.h"
#include "DNA_mesh_types.h"

namespace blender::io::usd {

/* Ensure the return type matches your header (bool or void) */
void sync_mesh_push(Mesh *me, pxr::UsdPrim prim, int dirty_bits)
{
  if (!me || !prim) {
    return;
  }

  pxr::UsdGeomMesh usd_mesh(prim);
  if (!usd_mesh) {
    return;
  }

  /* Use the DirtyBits mask to update only what changed */
  if (dirty_bits & USD_DIRTY_POINTS) {
    pxr::VtArray<pxr::GfVec3f> pts;

    /* Accessing Blender's internal vertex data */
    const float (*bl_pos)[3] = (const float (*)[3])me->vert_positions().data();

    for (int i = 0; i < me->verts_num; i++) {
      /* Swizzle back: Blender Z-up -> USD Y-up */
      pts.push_back(pxr::GfVec3f(bl_pos[i][0], bl_pos[i][2], -bl_pos[i][1]));
    }
    usd_mesh.GetPointsAttr().Set(pts);
  }
}

}  // namespace blender::io::usd
