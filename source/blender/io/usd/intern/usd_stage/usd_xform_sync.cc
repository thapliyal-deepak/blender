/* source/blender/io/usd/intern/usd_stage/usd_xform_sync.cc */
#include "usd_xform_sync.hh"
#include "usd_types_sync.hh"

#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "DEG_depsgraph.hh"

#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>

namespace blender::io::usd {

/* USD Y-up → Blender Z-up: mapping (x,y,z) → (x,-z,y).
 * Applies the similarity transform C·M·C⁻¹ to a Blender column-major matrix.
 * Each direction column is remapped; the matrix identity is preserved for Z-up stages. */
static float4x4 yup_to_zup(const float4x4 &m)
{
  float4x4 out;
  /* col 0: remap Y/Z components of the X-axis column */
  out[0] = float4(m[0][0], -m[0][2], m[0][1], 0.0f);
  /* col 1: USD Z-axis col → Blender Y-axis col (negated) */
  out[1] = float4(-m[2][0], m[2][2], -m[2][1], 0.0f);
  /* col 2: USD Y-axis col → Blender Z-axis col */
  out[2] = float4(m[1][0], -m[1][2], m[1][1], 0.0f);
  /* col 3: translation (tx, ty, tz) → (tx, -tz, ty) */
  out[3] = float4(m[3][0], -m[3][2], m[3][1], 1.0f);
  return out;
}

/* Blender Z-up → USD Y-up: inverse of yup_to_zup. */
static float4x4 zup_to_yup(const float4x4 &m)
{
  float4x4 out;
  out[0] = float4(m[0][0],  m[0][2], -m[0][1], 0.0f);
  out[1] = float4(m[2][0],  m[2][2], -m[2][1], 0.0f);
  out[2] = float4(-m[1][0], -m[1][2],  m[1][1], 0.0f);
  out[3] = float4(m[3][0],  m[3][2], -m[3][1], 1.0f);
  return out;
}

static bool stage_is_y_up(const pxr::UsdPrim &prim)
{
  pxr::UsdStageRefPtr stage = prim.GetStage();
  return stage && pxr::UsdGeomGetStageUpAxis(stage) == pxr::UsdGeomTokens->y;
}

void sync_xform_pull(pxr::UsdPrim prim, blender::Object *ob)
{
  if (!prim || !ob)
    return;

  pxr::UsdGeomImageable img(prim);
  pxr::GfMatrix4d usd_mat = img.ComputeLocalToWorldTransform(pxr::UsdTimeCode::Default());

  /* Transpose: USD row-major (row-vector convention) → Blender column-major. */
  float4x4 bl_mat;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      bl_mat[i][j] = static_cast<float>(usd_mat[i][j]);
    }
  }

  if (stage_is_y_up(prim)) {
    bl_mat = yup_to_zup(bl_mat);
  }

  BKE_object_apply_mat4(ob, reinterpret_cast<const float (*)[4]>(bl_mat.ptr()), false, false);
  DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
}

void sync_xform_push(blender::Object *ob, pxr::UsdPrim prim)
{
  if (!prim || !ob)
    return;

  pxr::UsdGeomXformable xf(prim);
  pxr::GfMatrix4d usd_mat;

  const float4x4 &src = ob->runtime->object_to_world;
  const float4x4 bl_mat = stage_is_y_up(prim) ? zup_to_yup(src) : src;

  /* Transpose back: Blender column-major → USD row-major. */
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      usd_mat[i][j] = static_cast<double>(bl_mat[i][j]);
    }
  }

  xf.MakeMatrixXform().Set(usd_mat);
}

}  // namespace blender::io::usd
