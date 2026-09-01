/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_skin_sync.hh"

#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BLI_listbase.h"
#include "BLI_math_vector.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "ED_armature.hh"

namespace blender::io::usd {

/* The frame the stage is evaluated at. A plain global rather than a parameter threaded through
 * every sync entry point: there is exactly one stage editor evaluation time at a time, the frame
 * listener sets it, and every puller wants the same value. */
static pxr::UsdTimeCode g_eval_time = pxr::UsdTimeCode::Default();

pxr::UsdTimeCode skin_eval_time()
{
  return g_eval_time;
}

void skin_set_eval_time(pxr::UsdTimeCode time)
{
  g_eval_time = time;
}

/**
 * Resolve the UsdSkel queries for `prim`.
 *
 * The cache is rebuilt per call. UsdSkelCache is a caching layer over composition, so holding one
 * across edits would hand back queries describing a stage that no longer exists — and this editor
 * exists to edit the stage. Populating from the prim's own SkelRoot keeps the cost proportional to
 * that rig rather than the whole stage.
 */
static bool resolve_queries(const pxr::UsdPrim &prim,
                            pxr::UsdSkelCache *cache,
                            pxr::UsdSkelSkinningQuery *r_skinning,
                            pxr::UsdSkelSkeletonQuery *r_skel,
                            pxr::UsdSkelSkeleton *r_skel_prim)
{
  if (!prim || prim.IsInstanceProxy()) {
    /* The binding API raises a USD error on instance proxies. */
    return false;
  }
  const pxr::UsdSkelRoot root = pxr::UsdSkelRoot::Find(prim);
  if (!root) {
    return false;
  }
  const pxr::UsdSkelBindingAPI binding(prim);
  if (!binding) {
    return false;
  }
  const pxr::UsdSkelSkeleton skel = binding.GetInheritedSkeleton();
  if (!skel) {
    return false;
  }

  cache->Populate(root, pxr::UsdTraverseInstanceProxies());

  const pxr::UsdSkelSkinningQuery skinning = cache->GetSkinningQuery(prim);
  if (!skinning.IsValid()) {
    return false;
  }
  const pxr::UsdSkelSkeletonQuery skel_query = cache->GetSkelQuery(skel);
  if (!skel_query) {
    return false;
  }

  *r_skinning = skinning;
  *r_skel = skel_query;
  *r_skel_prim = skel;
  return true;
}

bool skin_is_skinned(const pxr::UsdPrim &prim)
{
  pxr::UsdSkelCache cache;
  pxr::UsdSkelSkinningQuery skinning;
  pxr::UsdSkelSkeletonQuery skel_query;
  pxr::UsdSkelSkeleton skel;
  if (!resolve_queries(prim, &cache, &skinning, &skel_query, &skel)) {
    return false;
  }
  /* A binding without weights deforms nothing, so treat it as unskinned and let the ordinary
   * transform path place it. */
  return skinning.HasJointInfluences();
}

bool skin_pull_points(const pxr::UsdPrim &prim,
                      const pxr::UsdTimeCode time,
                      pxr::VtArray<pxr::GfVec3f> *r_points)
{
  pxr::UsdSkelCache cache;
  pxr::UsdSkelSkinningQuery skinning;
  pxr::UsdSkelSkeletonQuery skel_query;
  pxr::UsdSkelSkeleton skel;
  if (!resolve_queries(prim, &cache, &skinning, &skel_query, &skel)) {
    return false;
  }
  if (!skinning.HasJointInfluences()) {
    return false;
  }

  const pxr::UsdGeomMesh mesh(prim);
  if (!mesh) {
    return false;
  }
  pxr::VtArray<pxr::GfVec3f> points;
  if (!mesh.GetPointsAttr().Get(&points, time) || points.empty()) {
    return false;
  }

  pxr::VtArray<pxr::GfMatrix4d> xforms;
  if (!skel_query.ComputeSkinningTransforms(&xforms, time)) {
    return false;
  }
  /* Deforms in place, and applies geomBindTransform on the way. */
  if (!skinning.ComputeSkinnedPoints(xforms, &points, time)) {
    return false;
  }

  *r_points = std::move(points);
  return true;
}

bool skin_skel_to_world(const pxr::UsdPrim &prim,
                        const pxr::UsdTimeCode time,
                        pxr::GfMatrix4d *r_mat)
{
  pxr::UsdSkelCache cache;
  pxr::UsdSkelSkinningQuery skinning;
  pxr::UsdSkelSkeletonQuery skel_query;
  pxr::UsdSkelSkeleton skel;
  if (!resolve_queries(prim, &cache, &skinning, &skel_query, &skel)) {
    return false;
  }
  /* Skinned points come back in the Skeleton prim's space, so that prim's transform is what
   * carries them into the world. */
  *r_mat = pxr::UsdGeomImageable(skel.GetPrim()).ComputeLocalToWorldTransform(time);
  return true;
}

/* -------------------------------------------------------------------------------------------- */
/** \name Armature mirroring
 * \{ */

pxr::UsdPrim skin_skeleton_for_mesh(const pxr::UsdPrim &mesh_prim)
{
  if (!mesh_prim || mesh_prim.IsInstanceProxy()) {
    return pxr::UsdPrim();
  }
  const pxr::UsdSkelBindingAPI binding(mesh_prim);
  if (!binding) {
    return pxr::UsdPrim();
  }
  const pxr::UsdSkelSkeleton skel = binding.GetInheritedSkeleton();
  return skel ? skel.GetPrim() : pxr::UsdPrim();
}

/**
 * Row-vector matrix taking a point from a Y-up stage's axes to Blender's Z-up: (x, y, z) becomes
 * (x, -z, y), the same mapping usd_mesh_sync applies to points.
 *
 * Applied to the armature *object's* transform, not to the bones. Bones stay in the stage's axes
 * — as they do in Blender's own USD importer, which rotates the whole imported hierarchy at the
 * top instead — which keeps every pose delta a comparison between two matrices in one space, with
 * the conversion cancelling out of it. Converting each bone individually works too, but then a
 * root joint has nothing to cancel against and its delta needs separate handling.
 *
 * Note this cannot be left to sync_xform_pull: it converts a transform by conjugation, so a
 * Skeleton prim sitting at the origin yields the identity and the bones inside would keep the
 * stage's axes with nothing to stand them up.
 */
static pxr::GfMatrix4d yup_to_zup_matrix()
{
  return pxr::GfMatrix4d(1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1);
}

/**
 * The same transform with its basis rows normalised: rotation and position kept, scale dropped.
 *
 * A Blender bone's rest pose is a position and an orientation — ED_armature_ebone_from_mat4 takes
 * head, tail and roll — so it has nowhere to put scale. A joint chain carrying a uniform scale
 * (very common: an FBX "Armature" node holding the rig's unit conversion, which is exactly what
 * this asset has at x100) therefore loses that factor from the rest matrices while the pose deltas
 * are still computed against it, and every animated translation arrives divided by the scale. The
 * cure is to be uniformly scale-free: strip it here and use these matrices for both the bones and
 * the deltas. Genuine per-joint squash-and-stretch is lost, which Blender bones cannot express
 * anyway.
 */
static pxr::GfMatrix4d without_scale(const pxr::GfMatrix4d &m)
{
  pxr::GfMatrix4d out(m);
  for (int r = 0; r < 3; ++r) {
    pxr::GfVec3d axis(m[r][0], m[r][1], m[r][2]);
    const double len = axis.GetLength();
    if (len > 1e-12) {
      axis /= len;
    }
    out[r][0] = axis[0];
    out[r][1] = axis[1];
    out[r][2] = axis[2];
  }
  return out;
}

static bool skin_stage_is_y_up(const pxr::UsdPrim &prim)
{
  return prim && prim.GetStage() &&
         pxr::UsdGeomGetStageUpAxis(prim.GetStage()) == pxr::UsdGeomTokens->y;
}

/** Leaf name of a joint path token, so "a/b/Hips" becomes "Hips". */
static std::string joint_leaf_name(const pxr::TfToken &joint)
{
  const std::string str = joint.GetString();
  const size_t slash = str.rfind('/');
  return slash == std::string::npos ? str : str.substr(slash + 1);
}

/**
 * True when every joint token is a usable, unique path.
 *
 * UsdSkel encodes the joint hierarchy as an array of path-like tokens, so a token SdfPath cannot
 * parse — a colon most often, since Mixamo writes "mixamorig:Hips" — leaves the skeleton with no
 * hierarchy at all rather than with an oddly named bone. Blender's own USD importer refuses such a
 * skeleton outright; do the same here rather than build a bone soup with no parenting.
 */
static bool joint_paths_usable(const pxr::VtTokenArray &joints)
{
  std::vector<std::string> seen;
  seen.reserve(joints.size());
  for (const pxr::TfToken &joint : joints) {
    if (!pxr::SdfPath::IsValidPathString(joint.GetString())) {
      return false;
    }
    if (std::find(seen.begin(), seen.end(), joint.GetString()) != seen.end()) {
      return false;
    }
    seen.push_back(joint.GetString());
  }
  return true;
}

bool skin_build_armature(Main *bmain, Object *arm_ob, const pxr::UsdPrim &skel_prim)
{
  if (!bmain || !arm_ob || arm_ob->type != OB_ARMATURE || !arm_ob->data || !skel_prim) {
    return false;
  }
  const pxr::UsdSkelSkeleton skel(skel_prim);
  if (!skel) {
    return false;
  }

  pxr::UsdSkelCache cache;
  const pxr::UsdSkelSkeletonQuery skel_query = cache.GetSkelQuery(skel);
  if (!skel_query.IsValid()) {
    return false;
  }
  const pxr::UsdSkelTopology &topology = skel_query.GetTopology();
  const pxr::VtTokenArray joints = skel_query.GetJointOrder();
  if (joints.empty() || joints.size() != topology.size() || !joint_paths_usable(joints)) {
    return false;
  }

  pxr::VtMatrix4dArray bind_xforms;
  if (!skel_query.GetJointWorldBindTransforms(&bind_xforms) ||
      bind_xforms.size() != joints.size())
  {
    return false;
  }

  bArmature *arm = reinterpret_cast<bArmature *>(arm_ob->data);
  ED_armature_to_edit(arm);

  std::vector<EditBone *> bones(joints.size(), nullptr);
  for (size_t i = 0; i < joints.size(); ++i) {
    bones[i] = ED_armature_ebone_add(arm, joint_leaf_name(joints[i]).c_str());
  }

  /* Bind transforms are in skeleton space, which is this armature object's space, so a joint's
   * bind transform is its bone's rest matrix outright. The head/tail pair seeds a unit bone along
   * +Y for ED_armature_ebone_from_mat4 to orient; lengths are fixed up below. */
  for (size_t i = 0; i < joints.size(); ++i) {
    if (!bones[i]) {
      continue;
    }
    const pxr::GfMatrix4f mat(without_scale(bind_xforms[i]));
    float mat4[4][4];
    mat.Get(mat4);
    const float head[3] = {0.0f, 0.0f, 0.0f};
    const float tail[3] = {0.0f, 1.0f, 0.0f};
    copy_v3_v3(bones[i]->head, head);
    copy_v3_v3(bones[i]->tail, tail);
    ED_armature_ebone_from_mat4(bones[i], mat4);
  }

  /* Parent, collecting children so bone lengths can be derived from them. */
  std::vector<std::vector<size_t>> children(joints.size());
  for (size_t i = 0; i < joints.size(); ++i) {
    const int parent = topology.GetParent(i);
    if (parent < 0 || size_t(parent) >= joints.size()) {
      continue;
    }
    if (bones[i] && bones[parent]) {
      bones[i]->parent = bones[parent];
      children[parent].push_back(i);
    }
  }

  /* Every bone is unit length so far, which on a two-metre character draws as a thicket of
   * identical spikes. Stretch each towards the average head of its children, and give the
   * childless tips the average of those lengths. */
  float total_len = 0.0f;
  int len_count = 0;
  for (size_t i = 0; i < joints.size(); ++i) {
    if (children[i].empty() || !bones[i]) {
      continue;
    }
    float avg_child[3] = {0.0f, 0.0f, 0.0f};
    int n = 0;
    for (const size_t c : children[i]) {
      if (bones[c]) {
        add_v3_v3(avg_child, bones[c]->head);
        n++;
      }
    }
    if (n == 0) {
      continue;
    }
    mul_v3_fl(avg_child, 1.0f / float(n));
    float dir[3];
    sub_v3_v3v3(dir, avg_child, bones[i]->head);
    const float len = len_v3(dir);
    if (len > 1e-6f) {
      float unit[3];
      sub_v3_v3v3(unit, bones[i]->tail, bones[i]->head);
      normalize_v3(unit);
      mul_v3_fl(unit, len);
      add_v3_v3v3(bones[i]->tail, bones[i]->head, unit);
      total_len += len;
      len_count++;
    }
  }
  const float tip_len = len_count > 0 ? total_len / float(len_count) : 0.0f;
  if (tip_len > 1e-6f) {
    for (size_t i = 0; i < joints.size(); ++i) {
      if (!children[i].empty() || !bones[i]) {
        continue;
      }
      float unit[3];
      sub_v3_v3v3(unit, bones[i]->tail, bones[i]->head);
      normalize_v3(unit);
      mul_v3_fl(unit, tip_len);
      add_v3_v3v3(bones[i]->tail, bones[i]->head, unit);
    }
  }

  ED_armature_from_edit(bmain, arm);
  ED_armature_edit_free(arm);
  return true;
}

bool skin_bind_mesh(Object *mesh_ob, Object *arm_ob, const pxr::UsdPrim &mesh_prim)
{
  if (!mesh_ob || mesh_ob->type != OB_MESH || !arm_ob || !mesh_prim ||
      mesh_prim.IsInstanceProxy())
  {
    return false;
  }
  const pxr::UsdSkelBindingAPI binding(mesh_prim);
  if (!binding) {
    return false;
  }

  /* Joint order may be overridden per mesh, so prefer the mesh's own list. */
  pxr::VtTokenArray joints;
  if (binding.GetJointsAttr().HasAuthoredValue()) {
    binding.GetJointsAttr().Get(&joints);
  }
  if (joints.empty()) {
    if (const pxr::UsdSkelSkeleton skel = binding.GetInheritedSkeleton()) {
      skel.GetJointsAttr().Get(&joints);
    }
  }
  if (joints.empty()) {
    return false;
  }

  const pxr::UsdGeomPrimvar idx_pv = binding.GetJointIndicesPrimvar();
  const pxr::UsdGeomPrimvar wgt_pv = binding.GetJointWeightsPrimvar();
  if (!idx_pv || !wgt_pv || !idx_pv.HasAuthoredValue() || !wgt_pv.HasAuthoredValue()) {
    return false;
  }
  const int elem = idx_pv.GetElementSize();
  if (elem <= 0 || elem != wgt_pv.GetElementSize()) {
    return false;
  }
  pxr::VtIntArray indices;
  pxr::VtFloatArray weights;
  idx_pv.ComputeFlattened(&indices);
  wgt_pv.ComputeFlattened(&weights);
  if (indices.empty() || indices.size() != weights.size()) {
    return false;
  }

  Mesh *me = reinterpret_cast<Mesh *>(mesh_ob->data);
  if (!me || me->verts_num == 0) {
    return false;
  }

  /* One deform group per joint, named to match the bone. Blender pairs bones with groups by
   * name, so this naming has to agree with skin_build_armature's. */
  std::vector<int> group_index(joints.size(), -1);
  for (size_t i = 0; i < joints.size(); ++i) {
    const std::string name = joint_leaf_name(joints[i]);
    bDeformGroup *grp = BKE_object_defgroup_find_name(mesh_ob, name.c_str());
    if (!grp) {
      grp = BKE_object_defgroup_add_name(mesh_ob, name.c_str());
    }
    if (grp) {
      group_index[i] = BKE_object_defgroup_name_index(mesh_ob, grp->name);
    }
  }

  MutableSpan<MDeformVert> dverts = me->deform_verts_for_write();
  if (dverts.is_empty()) {
    return false;
  }
  const int verts = std::min(int(indices.size()) / elem, me->verts_num);
  for (int v = 0; v < verts; ++v) {
    for (int e = 0; e < elem; ++e) {
      const int ji = indices[v * elem + e];
      const float w = weights[v * elem + e];
      if (w <= 0.0f || ji < 0 || size_t(ji) >= group_index.size() || group_index[ji] < 0) {
        continue;
      }
      BKE_defvert_add_index_notest(&dverts[v], group_index[ji], w);
    }
  }

  ModifierData *md = BKE_modifiers_findby_type(mesh_ob, eModifierType_Armature);
  if (!md) {
    md = BKE_modifier_new(eModifierType_Armature);
    BLI_addtail(&mesh_ob->modifiers, md);
    BKE_modifiers_persistent_uid_init(*mesh_ob, *md);
  }
  reinterpret_cast<ArmatureModifierData *>(md)->object = arm_ob;
  return true;
}

void skin_place_armature(Object *arm_ob, const pxr::UsdPrim &skel_prim)
{
  if (!arm_ob || !skel_prim) {
    return;
  }
  pxr::GfMatrix4d world = pxr::UsdGeomImageable(skel_prim).ComputeLocalToWorldTransform(
      skin_eval_time());
  if (skin_stage_is_y_up(skel_prim)) {
    world = world * yup_to_zup_matrix();
  }
  float mat[4][4];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      mat[i][j] = float(world[i][j]);
    }
  }
  BKE_object_apply_mat4(arm_ob, mat, false, false);
}

bool skin_pull_pose(Object *arm_ob, const pxr::UsdPrim &skel_prim, const pxr::UsdTimeCode time)
{
  if (!arm_ob || arm_ob->type != OB_ARMATURE || !arm_ob->pose || !skel_prim) {
    return false;
  }
  const pxr::UsdSkelSkeleton skel(skel_prim);
  if (!skel) {
    return false;
  }
  pxr::UsdSkelCache cache;
  const pxr::UsdSkelSkeletonQuery skel_query = cache.GetSkelQuery(skel);
  if (!skel_query.IsValid()) {
    return false;
  }
  const pxr::UsdSkelTopology &topology = skel_query.GetTopology();
  const pxr::VtTokenArray joints = skel_query.GetJointOrder();
  pxr::VtMatrix4dArray bind_xforms;
  pxr::VtMatrix4dArray local_xforms;
  if (!skel_query.GetJointWorldBindTransforms(&bind_xforms) ||
      !skel_query.ComputeJointLocalTransforms(&local_xforms, time))
  {
    return false;
  }
  if (local_xforms.size() != joints.size() || bind_xforms.size() != joints.size()) {
    return false;
  }

  for (size_t i = 0; i < joints.size(); ++i) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(arm_ob->pose,
                                                     joint_leaf_name(joints[i]).c_str());
    if (!pchan) {
      continue;
    }
    /* Blender wants a pose as a delta from the bone's rest matrix, so express the joint's
     * animated local transform relative to its *bind* transform in the same parent space.
     *
     * The Y-up-to-Z-up change skin_build_armature applied to the rest matrices cancels out of
     * this delta for any joint that has a parent — both terms carry the same factor — but a root
     * joint has nothing to cancel against, so its delta has to be re-expressed in Blender's axes.
     * Skip it and the rig animates in place: the limbs move but the root translation, which is
     * what walks the character forward, never arrives. */
    pxr::GfMatrix4d bind_local = without_scale(bind_xforms[i]);
    const int parent = topology.GetParent(i);
    if (parent >= 0 && size_t(parent) < bind_xforms.size()) {
      bind_local = bind_local * without_scale(bind_xforms[parent]).GetInverse();
    }
    const pxr::GfMatrix4f basis(without_scale(local_xforms[i]) * bind_local.GetInverse());
    BKE_pchan_apply_mat4(pchan, (float(*)[4])basis.data(), false);
  }
  return true;
}

/** \} */

}  // namespace blender::io::usd
