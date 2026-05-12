#include "usd_mesh_sync.hh"
#include "usd_types_sync.hh"
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <algorithm>

#include "BKE_mesh.hh"
#include "DEG_depsgraph.hh"
#include "DNA_mesh_types.h"
#include "bmesh.hh"

namespace blender::io::usd {

static bool stage_is_y_up(const pxr::UsdPrim &prim)
{
  pxr::UsdStageRefPtr stage = prim.GetStage();
  return stage && pxr::UsdGeomGetStageUpAxis(stage) == pxr::UsdGeomTokens->y;
}

/** USD -> Blender (Pull) */
bool sync_mesh_pull(pxr::UsdPrim prim, Mesh *me, int dirty_bits)
{
  pxr::UsdGeomMesh usd_mesh(prim);
  if (!usd_mesh)
    return false;

  if (dirty_bits & USD_DIRTY_TOPOLOGY) {
    /* Full resync: read topology and positions together via a nomain mesh so
     * we never call vert_positions_for_write() on a zero-vert mesh. */
    pxr::VtArray<pxr::GfVec3f> pts;
    pxr::VtArray<int> face_vertex_counts;
    pxr::VtArray<int> face_vertex_indices;

    usd_mesh.GetPointsAttr().Get(&pts);
    usd_mesh.GetFaceVertexCountsAttr().Get(&face_vertex_counts);
    usd_mesh.GetFaceVertexIndicesAttr().Get(&face_vertex_indices);

    if (pts.empty()) {
      DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
      return true;
    }

    const int verts_num = int(pts.size());
    const int faces_num = int(face_vertex_counts.size());
    const int corners_num = int(face_vertex_indices.size());

    Mesh *tmp = BKE_mesh_new_nomain(verts_num, 0, faces_num, corners_num);

    const bool y_up = stage_is_y_up(prim);
    MutableSpan<float3> positions = tmp->vert_positions_for_write();
    for (int i = 0; i < verts_num; i++) {
      positions[i] = y_up ? float3{pts[i][0], -pts[i][2], pts[i][1]} :
                            float3{pts[i][0], pts[i][1], pts[i][2]};
    }

    if (faces_num > 0) {
      MutableSpan<int> face_offsets = tmp->face_offsets_for_write();
      int offset = 0;
      for (int f = 0; f < faces_num; f++) {
        face_offsets[f] = offset;
        offset += face_vertex_counts[f];
      }
      face_offsets[faces_num] = corners_num;

      MutableSpan<int> corner_verts = tmp->corner_verts_for_write();
      for (int c = 0; c < corners_num; c++) {
        corner_verts[c] = face_vertex_indices[c];
      }
    }

    BKE_mesh_nomain_to_mesh(tmp, me, nullptr);
    /* Build explicit edges from face corners so loose_edges() has a valid edge array. */
    bke::mesh_calc_edges(*me, false, false);
  }
  else if (dirty_bits & USD_DIRTY_POINTS) {
    /* Incremental point-only update: topology is unchanged. */
    if (me->verts_num > 0) {
      pxr::VtArray<pxr::GfVec3f> pts;
      usd_mesh.GetPointsAttr().Get(&pts);
      const bool y_up = stage_is_y_up(prim);
      MutableSpan<float3> positions = me->vert_positions_for_write();
      const int count = std::min(int(pts.size()), me->verts_num);
      for (int i = 0; i < count; i++) {
        positions[i] = y_up ? float3{pts[i][0], -pts[i][2], pts[i][1]} :
                              float3{pts[i][0], pts[i][1], pts[i][2]};
      }
    }
  }

  DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
  return true;
}

/** Blender -> USD (Push) */
bool sync_mesh_push(Mesh *me, pxr::UsdPrim prim, int dirty_bits)
{
  pxr::UsdGeomMesh usd_mesh(prim);
  if (dirty_bits & USD_DIRTY_POINTS) {
    const bool y_up = stage_is_y_up(prim);
    pxr::VtArray<pxr::GfVec3f> pts;
    Span<float3> positions = me->vert_positions();
    for (int i = 0; i < positions.size(); i++) {
      pts.push_back(y_up ? pxr::GfVec3f(positions[i][0], positions[i][2], -positions[i][1]) :
                           pxr::GfVec3f(positions[i][0], positions[i][1], positions[i][2]));
    }
    usd_mesh.GetPointsAttr().Set(pts);
  }
  return true;
}

/** Push Edit-Mode vertex positions from a BMesh directly to USD.
 * Used when ob->data positions are stale (Blender only writes the base mesh on Edit Mode exit). */
bool sync_mesh_push_bm(BMesh *bm, pxr::UsdPrim prim)
{
  if (!bm || !prim)
    return false;

  pxr::UsdGeomMesh usd_mesh(prim);
  const bool y_up = stage_is_y_up(prim);

  pxr::VtArray<pxr::GfVec3f> pts;
  pts.reserve(bm->totvert);

  BMIter iter;
  BMVert *v;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    pts.push_back(y_up ? pxr::GfVec3f(v->co[0], v->co[2], -v->co[1]) :
                         pxr::GfVec3f(v->co[0], v->co[1], v->co[2]));
  }

  usd_mesh.GetPointsAttr().Set(pts);
  return true;
}

}  // namespace blender::io::usd
