/* source/blender/io/usd/intern/usd_stage/usd_xform_sync.cc */
#include "usd_xform_sync.hh"
#include <cmath>
#include "usd_types_sync.hh"

#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "DEG_depsgraph.hh"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
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
  /* Keep object_to_world in sync immediately so sync_xform_differs returns false if
   * push_all_xforms() is called before the depsgraph evaluates. Without this, the stale
   * pre-repull matrix is compared against the new USD value, triggering a spurious push
   * that overwrites the layer's authored opinions with wrong positions. */
  ob->runtime->object_to_world = bl_mat;
  DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
}

void sync_xform_push(blender::Object *ob, pxr::UsdPrim prim)
{
  if (!prim || !ob)
    return;

  /* Read from ob->loc/rot/scale via BKE_object_to_mat4 rather than ob->runtime->object_to_world.
   * The runtime matrix on the ORIGINAL object is only updated by sync_xform_pull; after the user
   * moves an object the depsgraph updates ob_eval->object_to_world but leaves the original's
   * runtime matrix stale. Using BKE_object_to_mat4 always reflects the current transform. */
  float4x4 src;
  BKE_object_to_mat4(ob, reinterpret_cast<float (*)[4]>(src.ptr()));
  const float4x4 bl_world = stage_is_y_up(prim) ? zup_to_yup(src) : src;

  /* Convert Blender world matrix to USD convention (implicit transpose via [i][j]=[i][j]). */
  pxr::GfMatrix4d world_usd;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      world_usd[i][j] = static_cast<double>(bl_world[i][j]);

  /* Convert world → local: local = world * parent_world^-1.
   * Required because ComputeLocalToWorldTransform was used on pull, and USD xform ops
   * are LOCAL. Without this, meshes with a parent Xform (e.g. Unreal root) appear at
   * a wrong position after any transform push. */
  pxr::GfMatrix4d local_usd = world_usd;
  const pxr::UsdPrim parent = prim.GetParent();
  if (parent && !parent.IsPseudoRoot()) {
    const pxr::GfMatrix4d parent_world =
        pxr::UsdGeomImageable(parent).ComputeLocalToWorldTransform(pxr::UsdTimeCode::Default());
    local_usd = world_usd * parent_world.GetInverse();
  }

  /* Decompose into translate + orient + scale so that Unreal Live Link xformOps
   * remain compatible. MakeMatrixXform() would replace xformOpOrder with a single
   * matrix op, causing Unreal's translate overrides to be silently ignored and
   * showing a "ghost" at the original position alongside the moved mesh. */
  pxr::UsdGeomXformable xf(prim);
  bool reset_stack = false;

  /* Factor decomposes M = p * t * u * s * r; we only need t, s, and the left rotation r. */
  pxr::GfMatrix4d r_mat, u_mat, p_mat;
  pxr::GfVec3d t_vec, s_vec;
  local_usd.Factor(&r_mat, &s_vec, &u_mat, &t_vec, &p_mat);
  /* GfMatrix4d::ExtractRotationQuat() returns GfQuatd directly, no GfRotation needed. */
  pxr::GfQuatd r_quat = r_mat.ExtractRotationQuat();

  /* Write individual ops, preserving any existing op names so Unreal's overrides survive. */
  pxr::UsdGeomXformOp translate_op, rotate_op, scale_op;
  bool found_translate = false, found_rotate = false, found_scale = false;
  bool has_incompatible_rotation = false;

  for (pxr::UsdGeomXformOp &op : xf.GetOrderedXformOps(&reset_stack)) {
    using Type = pxr::UsdGeomXformOp::Type;
    const Type t = op.GetOpType();
    if (t == Type::TypeTranslate) {
      translate_op = op;
      found_translate = true;
    }
    else if (t == Type::TypeOrient) {
      rotate_op = op;
      found_rotate = true;
    }
    else if (t == Type::TypeScale) {
      scale_op = op;
      found_scale = true;
    }
    else if (t >= Type::TypeRotateX && t <= Type::TypeRotateZYX) {
      /* Euler rotation in the base layer (e.g. rotateXYZ from Unreal-exported USD).
       * We cannot reuse it for a quaternion orient value — mark it for replacement. */
      has_incompatible_rotation = true;
    }
  }

  if (has_incompatible_rotation && !found_rotate) {
    /* Replace all Euler rotation tokens in the composed xformOpOrder with a single
     * orient token and author the cleaned order into the current edit target.
     * Using CreateXformOpOrderAttr (not GetXformOpOrderAttr().Set) so the attribute
     * is always created/valid even on prims that have no authored order yet. */
    static const pxr::TfToken kOrientTok("xformOp:orient", pxr::TfToken::Immortal);
    pxr::VtTokenArray cur_order;
    xf.GetXformOpOrderAttr().Get(&cur_order);
    pxr::VtTokenArray fixed_order;
    bool orient_placed = false;
    for (const pxr::TfToken &tok : cur_order) {
      /* "xformOp:rotate*" covers rotateXYZ/rotateXZY/rotateX/Y/Z etc. but not orient. */
      const bool is_euler = (tok.GetString().rfind("xformOp:rotate", 0) == 0);
      if (is_euler) {
        if (!orient_placed) {
          fixed_order.push_back(kOrientTok);
          orient_placed = true;
        }
        /* else: drop additional conflicting Euler ops */
      }
      else {
        fixed_order.push_back(tok);
      }
    }
    if (!orient_placed)
      fixed_order.push_back(kOrientTok);
    xf.CreateXformOpOrderAttr(pxr::VtValue(fixed_order));
    /* found_rotate stays false so AddOrientOp() creates the orient attribute below. */
  }

  if (!found_translate)
    translate_op = xf.AddTranslateOp();
  if (!found_rotate)
    rotate_op = xf.AddOrientOp();
  if (!found_scale)
    scale_op = xf.AddScaleOp();

  translate_op.Set(t_vec);
  rotate_op.Set(pxr::GfQuatf(r_quat));
  scale_op.Set(pxr::GfVec3f(s_vec));
}

bool sync_xform_differs(blender::Object *ob, const pxr::UsdPrim &prim)
{
  if (!prim || !ob)
    return false;

  pxr::UsdGeomImageable img(prim);
  pxr::GfMatrix4d usd_mat = img.ComputeLocalToWorldTransform(pxr::UsdTimeCode::Default());

  float4x4 bl_from_usd;
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      bl_from_usd[i][j] = static_cast<float>(usd_mat[i][j]);

  if (stage_is_y_up(prim))
    bl_from_usd = yup_to_zup(bl_from_usd);

  /* Same reasoning as sync_xform_push: use BKE_object_to_mat4 so we compare against the
   * current loc/rot/scale, not the stale ob->runtime->object_to_world. */
  float4x4 bl_cur;
  BKE_object_to_mat4(ob, reinterpret_cast<float (*)[4]>(bl_cur.ptr()));

  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      if (std::fabs(bl_cur[i][j] - bl_from_usd[i][j]) > 1e-5f)
        return true;

  return false;
}

}  // namespace blender::io::usd
