#include "usd_mesh_sync.hh"
#include "usd_skin_sync.hh"
#include "usd_types_sync.hh"
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <algorithm>

#include "BKE_mesh.hh"
#include "DEG_depsgraph.hh"
#include "DNA_mesh_types.h"
#include "bmesh.hh"
#include <cmath>

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

    /* Authored points only. A skinned mesh keeps its undeformed geometry here and is deformed by
     * the armature modifier the runtime binds to it, so evaluating UsdSkel here as well would
     * deform it twice. */
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
      /* Writing positions leaves the caches derived from them — bounds and normals — holding
       * values for the old shape. ID_RECALC_GEOMETRY alone does not clear them, so an animated
       * mesh drew correctly while its bounding box stayed frozen at the first frame's, which
       * throws off view framing, selection and view-frustum culling. Same call the regular USD
       * mesh reader makes (usd_reader_mesh.cc). */
      me->tag_positions_changed();
    }
  }

  DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
  return true;
}

/** Blender -> USD (Push) */
bool sync_mesh_push(Mesh *me, pxr::UsdPrim prim, int dirty_bits)
{
  pxr::UsdGeomMesh usd_mesh(prim);
  const bool y_up = stage_is_y_up(prim);

  /* The Blender mesh of a skinned prim holds *deformed* points, so writing them back would
   * overwrite the undeformed geometry the weights are defined against — the rig would be baked
   * into its own bind pose, and every later frame would deform that. Skinning cannot be inverted
   * in general either (a point is a weighted blend of several joints), so refuse rather than
   * guess. Topology is still safe to write: it is not touched by deformation. */
  if ((dirty_bits & USD_DIRTY_POINTS) && !(dirty_bits & USD_DIRTY_TOPOLOGY) &&
      skin_is_skinned(prim))
  {
    return false;
  }

  if (dirty_bits & USD_DIRTY_TOPOLOGY) {
    /* Full export: write positions + face topology. Used when creating a new prim. */
    pxr::VtArray<pxr::GfVec3f> pts;
    for (const float3 &p : me->vert_positions()) {
      pts.push_back(y_up ? pxr::GfVec3f(p[0], p[2], -p[1]) : pxr::GfVec3f(p[0], p[1], p[2]));
    }
    usd_mesh.GetPointsAttr().Set(pts);

    blender::Span<int> face_offsets = me->face_offsets();
    blender::Span<int> corner_verts_span = me->corner_verts();

    pxr::VtArray<int> usd_counts;
    pxr::VtArray<int> usd_indices;
    usd_counts.reserve(me->faces_num);
    usd_indices.reserve(corner_verts_span.size());

    for (int f = 0; f < me->faces_num; ++f) {
      const int start = face_offsets[f];
      const int end = face_offsets[f + 1];
      usd_counts.push_back(end - start);
      for (int c = start; c < end; ++c) {
        usd_indices.push_back(corner_verts_span[c]);
      }
    }
    usd_mesh.GetFaceVertexCountsAttr().Set(usd_counts);
    usd_mesh.GetFaceVertexIndicesAttr().Set(usd_indices);
  }
  else if (dirty_bits & USD_DIRTY_POINTS) {
    /* Points-only update: topology is unchanged (live vertex editing). */
    pxr::VtArray<pxr::GfVec3f> pts;
    for (const float3 &p : me->vert_positions()) {
      pts.push_back(y_up ? pxr::GfVec3f(p[0], p[2], -p[1]) : pxr::GfVec3f(p[0], p[1], p[2]));
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

bool sync_mesh_bm_differs(BMesh *bm, const pxr::UsdPrim &prim)
{
  if (!bm || !prim)
    return false;

  pxr::UsdGeomMesh usd_mesh(prim);
  pxr::VtArray<pxr::GfVec3f> pts;
  if (!usd_mesh.GetPointsAttr().Get(&pts))
    return true;  /* no composed data → treat as different so we don't silently skip */

  if (bm->totvert != (int)pts.size())
    return true;

  const bool y_up = stage_is_y_up(prim);

  int i = 0;
  BMIter iter;
  BMVert *v;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    /* Convert USD position to Blender space for comparison. */
    const float bx = y_up ? pts[i][0] : pts[i][0];
    const float by = y_up ? -pts[i][2] : pts[i][1];
    const float bz = y_up ?  pts[i][1] : pts[i][2];
    if (std::fabs(v->co[0] - bx) > 1e-5f ||
        std::fabs(v->co[1] - by) > 1e-5f ||
        std::fabs(v->co[2] - bz) > 1e-5f)
      return true;
    ++i;
  }
  return false;
}

}  // namespace blender::io::usd
