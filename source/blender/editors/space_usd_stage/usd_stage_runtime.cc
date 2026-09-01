/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_stage_runtime.hh"
#include "usd_mesh_sync.hh"
#include "usd_skin_sync.hh"
#include "usd_notice_handler.hh"
#include "usd_scene_sync.hh"
#include "usd_types_sync.hh"
#include "usd_xform_sync.hh"

#include <algorithm>
#include <set>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/utils.h>

#include "BKE_action.hh"
#include "BKE_camera.h"
#include "BKE_armature.hh"
#include "BKE_collection.hh"
#include "BKE_light.h"
#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"

#include "BLI_ustring.hh"

#include "usd_reader_material.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "BKE_node_legacy_types.hh"
#include "WM_api.hh"
#include "WM_types.hh"
#include "DNA_camera_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_action_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender {

void SpaceUsdStage_Runtime::sync_active_prim(Object *ob, int dirty_bits, bool is_push)
{
  if (!stage || !ob)
    return;

  /* Find the USD Prim associated with this Blender Object by searching the map. */
  std::string prim_path_str;
  for (const auto &[path, obj] : obj_map) {
    if (obj == ob) {
      prim_path_str = path;
      break;
    }
  }
  if (prim_path_str.empty())
    return;

  pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(prim_path_str));

  if (!prim)
    return;

  if (is_push) {
    /* Blender -> USD */
    if (ob->type == OB_MESH)
      io::usd::sync_mesh_push((Mesh *)ob->data, prim, dirty_bits);
    else if (ob->type == OB_CAMERA)
      io::usd::sync_camera(prim, (Camera *)ob->data, true);
    else if (ob->type == OB_LAMP)
      io::usd::sync_light(prim, (Light *)ob->data, true);
    io::usd::sync_xform_push(ob, prim);
  }
  else {
    /* USD -> Blender */
    if (ob->type == OB_MESH)
      io::usd::sync_mesh_pull(prim, (Mesh *)ob->data, dirty_bits);
    else if (ob->type == OB_CAMERA)
      io::usd::sync_camera(prim, (Camera *)ob->data, false);
    else if (ob->type == OB_LAMP)
      io::usd::sync_light(prim, (Light *)ob->data, false);
    io::usd::sync_xform_pull(prim, ob);
  }
}

void SpaceUsdStage_Runtime::open(const char *filepath)
{
  stage = pxr::UsdStage::Open(filepath, pxr::UsdStage::LoadAll);
  if (stage) {
    refresh_layers();
    stage_listener.register_listener(stage, nullptr);
  }
}

void SpaceUsdStage_Runtime::create_new(const char *filepath)
{
  if (filepath && filepath[0] != '\0') {
    /* Try to create a new file-backed stage. CreateNew fails if the file already exists
     * (e.g. leftover from a previous session), so fall back to opening the existing file. */
    stage = pxr::UsdStage::CreateNew(std::string(filepath));
    if (!stage)
      stage = pxr::UsdStage::Open(std::string(filepath), pxr::UsdStage::LoadAll);
  }
  else {
    stage = pxr::UsdStage::CreateInMemory();
  }

  if (!stage)
    return;

  /* Only set up /World when this is truly a fresh stage (no default prim yet). */
  if (!stage->GetDefaultPrim().IsValid()) {
    pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/World"));
    stage->SetDefaultPrim(stage->GetPrimAtPath(pxr::SdfPath("/World")));
    /* Blender uses meters; USD default is 0.01 (cm). Without this Unreal reads
     * meter-scale coordinates as centimetres, making everything 100x too small. */
    pxr::UsdGeomSetStageMetersPerUnit(stage, 1.0);
    /* Flush to disk immediately so the file is valid for future sessions. */
    stage->GetRootLayer()->Save();
  }

  anon_layer_refs.push_back(stage->GetRootLayer());
  refresh_layers();
  stage_listener.register_listener(stage, nullptr);
}

Collection *SpaceUsdStage_Runtime::ensure_usd_sync_collection(Main *bmain, Scene *scene)
{
  const char *col_name = "USD_Sync_Internal";

  /* 1. Look for existing collection to avoid duplicates */
  Collection *col = (Collection *)BLI_findstring(
      &bmain->collections, col_name, offsetof(ID, name) + 2);

  if (!col) {
    /* 2. Create the collection if it doesn't exist */
    col = BKE_collection_add(bmain, scene->master_collection, col_name);
  }

  return col;
}

void SpaceUsdStage_Runtime::validate_obj_map()
{
  if (!bmain || !stage)
    return;

  std::vector<std::string> stale;
  for (const auto &[path, ob] : obj_map) {
    /* BLI_findindex compares pointer values only — never dereferences ob,
     * so this is safe even if ob points to already-freed memory. */
    if (BLI_findindex(&bmain->objects, ob) == -1) {
      stale.push_back(path);
    }
  }
  for (const std::string &path : stale) {
    stage->RemovePrim(pxr::SdfPath(path));
    obj_map.erase(path);
  }
}

void SpaceUsdStage_Runtime::populate_blender_from_stage(const bContext *C)
{
  bmain = CTX_data_main(C);
  obj_map.clear();
  skel_map.clear();
  material_map.clear();

  /* Skeletons first: a mesh binds to its armature as it is created, and traversal order gives no
   * guarantee that the Skeleton prim comes before the meshes it deforms. */
  for (pxr::UsdPrim prim : stage->Traverse()) {
    if (prim.IsA<pxr::UsdSkelSkeleton>()) {
      create_blender_armature_for_prim(C, prim);
    }
  }

  pxr::UsdPrimRange range = stage->Traverse();
  for (pxr::UsdPrim prim : range) {
    if (prim.IsA<pxr::UsdGeomMesh>()) {
      create_blender_mesh_for_prim(C, prim);
    }
    else if (prim.IsA<pxr::UsdGeomCamera>()) {
      create_blender_camera_for_prim(C, prim);
    }
    else if (prim.HasAPI<pxr::UsdLuxLightAPI>()) {
      create_blender_light_for_prim(C, prim);
    }
  }

  /* Pose the rigs for the frame the editor is showing. */
  pull_skel_poses(double(io::usd::skin_eval_time().GetValue()));

  /* Rebuild the view layer's object_bases_hash so BKE_view_layer_base_find()
   * finds the newly created objects immediately. Without this, prim-tree clicks
   * fail to select objects until the next full depsgraph evaluation. */
  if (Scene *scene = CTX_data_scene(C)) {
    if (ViewLayer *view_layer = CTX_data_view_layer(C)) {
      BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    }
  }

  DEG_relations_tag_update(bmain);
}

/** Bootstrap a Principled BSDF → Material Output on an empty material nodetree so EEVEE has a
 *  valid surface shader. Skips if a Material Output already exists. */
static void ensure_nodes(Main *bmain, Material *mat)
{
  if (!mat || !mat->nodetree)
    return;
  mat->use_nodes = true;
  for (bNode *n = static_cast<bNode *>(mat->nodetree->nodes.first); n;
       n = static_cast<bNode *>(n->next))
  {
    if (n->type_legacy == SH_NODE_OUTPUT_MATERIAL)
      return;
  }
  bNodeTree *ntree = mat->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, *ntree, SH_NODE_BSDF_PRINCIPLED);
  principled->location[0] = 0.0f;
  principled->location[1] = 300.0f;
  bNode *output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_MATERIAL);
  output->location[0] = 300.0f;
  output->location[1] = 300.0f;
  bNodeSocket *bsdf_sock = bke::node_find_socket(*principled, SOCK_OUT, UString("BSDF"));
  bNodeSocket *surf_sock = bke::node_find_socket(*output, SOCK_IN, UString("Surface"));
  if (bsdf_sock && surf_sock)
    bke::node_add_link(*ntree, *principled, *bsdf_sock, *output, *surf_sock);
  bke::node_set_active(*ntree, *output);
  BKE_ntree_update_after_single_tree_change(*bmain, *ntree);
}

/** Create (or reuse) the Blender material for the USD material bound to a mesh prim and assign
 *  it to slot 1 of the given object. Does nothing if the prim has no material binding. */
static void assign_bound_material(Main *bmain,
                                  Object *ob,
                                  const pxr::UsdPrim &prim,
                                  std::unordered_map<std::string, std::string> &material_map)
{
  pxr::UsdShadeMaterialBindingAPI binding(prim);
  pxr::UsdShadeMaterial bound_mat = binding.ComputeBoundMaterial();
  if (!bound_mat)
    return;

  /* One USD material is one Blender material, however many prims bind it. Reuse the one already
   * built for this path: a shared look really is shared, so editing it should reach every mesh
   * that binds it, and rebuilding the node graph per prim is wasted work besides. */
  const std::string mat_path = bound_mat.GetPath().GetString();
  auto cached = material_map.find(mat_path);
  if (cached != material_map.end()) {
    if (ID *id = BKE_libblock_find_name(bmain, ID_MA, cached->second.c_str())) {
      BKE_object_material_assign_single_obdata(bmain, ob, reinterpret_cast<Material *>(id), 1);
      return;
    }
    /* Gone (deleted by the user, say) -- fall through and build it again. */
    material_map.erase(cached);
  }

  io::usd::USDImportParams params{};
  params.import_usd_preview = true;
  params.set_material_blend = true;
  params.worker_status = nullptr;
  /* Bound stage materials carry both a MaterialX reference and a (lossy) UsdPreviewSurface for
   * non-MaterialX renderers; read the MaterialX network so the reopened material looks faithful. */
  params.prefer_mtlx_over_preview = true;

  /* USDMaterialReader builds the full Principled BSDF node graph for both UsdPreviewSurface
   * and MaterialX (mtlx standard_surface) sources, so no editor-side shader-value mapping is
   * needed here anymore — that logic now lives in the built-in reader (usd_reader_material.cc). */
  io::usd::USDMaterialReader reader(params, *bmain);
  Material *mat = reader.add_material(bound_mat);
  if (!mat)
    return;

  /* Fallback: guarantee a valid surface shader for materials whose surface source is neither
   * UsdPreviewSurface nor MaterialX standard_surface (leaves a bare Principled instead of black).
   * A no-op when the reader already populated the node tree. */
  ensure_nodes(bmain, mat);

  material_map[mat_path] = mat->id.name + 2;

  BKE_object_material_assign_single_obdata(bmain, ob, mat, 1);
  /* Tag material for GPU shader recompile and run the full invariant pass so
   * EEVEE picks up the freshly-created nodetree before the first draw. */
  DEG_id_tag_update(&mat->id, ID_RECALC_NTREE_OUTPUT);
  if (mat->nodetree) {
    BKE_main_ensure_invariants(*bmain, mat->nodetree->id);
  }
  BKE_material_make_node_previews_dirty(mat);
}

void SpaceUsdStage_Runtime::create_blender_mesh_for_prim(const bContext *C, pxr::UsdPrim prim)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  /* 1. Get our hidden management collection */
  Collection *sync_col = ensure_usd_sync_collection(bmain, scene);

  /* 2. Create Object and Mesh */
  Mesh *me = BKE_mesh_add(bmain, prim.GetName().GetText());
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, prim.GetName().GetText());
  ob->data = reinterpret_cast<ID *>(me);
  id_us_plus(&me->id); /* ob->data counts as a user */

  /* 3. Link to the HIDDEN collection */
  BKE_collection_object_add(bmain, sync_col, ob);

  /* 4. Sync and Register */
  std::string prim_path = prim.GetPath().GetString();
  obj_map[prim_path] = ob;

  io::usd::sync_mesh_pull(prim, me, io::usd::USD_DIRTY_ALL);
  io::usd::sync_xform_pull(prim, ob);
  assign_bound_material(bmain, ob, prim, material_map);

  /* Hand a skinned mesh to its armature: deform groups from the joint weights plus an armature
   * modifier, so Blender deforms it natively and the points stay the undeformed bind geometry. */
  if (const pxr::UsdPrim skel_prim = io::usd::skin_skeleton_for_mesh(prim)) {
    auto it = skel_map.find(skel_prim.GetPath().GetString());
    if (it != skel_map.end() && it->second) {
      io::usd::skin_bind_mesh(ob, it->second, prim);
    }
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
}

void SpaceUsdStage_Runtime::create_blender_armature_for_prim(const bContext *C,
                                                             pxr::UsdPrim prim)
{
  Main *bmain_local = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Collection *sync_col = ensure_usd_sync_collection(bmain_local, scene);

  bArmature *arm = BKE_armature_add(bmain_local, prim.GetName().GetText());
  Object *ob = BKE_object_add_only_object(bmain_local, OB_ARMATURE, prim.GetName().GetText());
  ob->data = reinterpret_cast<ID *>(arm);
  id_us_plus(&arm->id);

  BKE_collection_object_add(bmain_local, sync_col, ob);

  if (!io::usd::skin_build_armature(bmain_local, ob, prim)) {
    /* Nothing usable in the skeleton — most often joint names SdfPath cannot parse. Drop the
     * empty armature rather than leave a stray object in the scene. */
    BKE_collection_object_remove(bmain_local, sync_col, ob, true);
    return;
  }

  skel_map[prim.GetPath().GetString()] = ob;
  io::usd::skin_place_armature(ob, prim);
  DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
}

void SpaceUsdStage_Runtime::pull_skel_poses(const double frame)
{
  if (!stage) {
    return;
  }
  for (auto &[path, arm_ob] : skel_map) {
    if (!arm_ob) {
      continue;
    }
    const pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (!prim) {
      continue;
    }
    if (io::usd::skin_pull_pose(arm_ob, prim, pxr::UsdTimeCode(frame))) {
      DEG_id_tag_update(&arm_ob->id, ID_RECALC_GEOMETRY);
    }
  }
}

void SpaceUsdStage_Runtime::create_blender_camera_for_prim(const bContext *C, pxr::UsdPrim prim)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Collection *sync_col = ensure_usd_sync_collection(bmain, scene);

  Camera *cam = BKE_camera_add(bmain, prim.GetName().GetText());
  Object *ob = BKE_object_add_only_object(bmain, OB_CAMERA, prim.GetName().GetText());
  ob->data = reinterpret_cast<ID *>(cam);
  id_us_plus(&cam->id);

  BKE_collection_object_add(bmain, sync_col, ob);

  const std::string prim_path = prim.GetPath().GetString();
  obj_map[prim_path] = ob;

  io::usd::sync_camera(prim, cam, false);
  io::usd::sync_xform_pull(prim, ob);

  DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
}

/** Map USD light prim → Blender light type constant.
 *  Spot lights in USD are SphereLight + UsdLuxShapingAPI (no dedicated SpotLight schema). */
static eLightType usd_prim_to_bl_light_type(const pxr::UsdPrim &prim)
{
  const pxr::TfToken t = prim.GetTypeName();
  if (t == pxr::TfToken("DistantLight"))
    return LA_SUN;
  if (t == pxr::TfToken("RectLight") ||
      t == pxr::TfToken("DiskLight") ||
      t == pxr::TfToken("CylinderLight"))
    return LA_AREA;
  /* SphereLight with ShapingAPI cone → spot. */
  if (t == pxr::TfToken("SphereLight") && prim.HasAPI<pxr::UsdLuxShapingAPI>())
    return LA_SPOT;
  return LA_LOCAL;
}

void SpaceUsdStage_Runtime::create_blender_light_for_prim(const bContext *C, pxr::UsdPrim prim)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Collection *sync_col = ensure_usd_sync_collection(bmain, scene);

  Light *light = BKE_light_add(bmain, prim.GetName().GetText());
  light->type = usd_prim_to_bl_light_type(prim);

  Object *ob = BKE_object_add_only_object(bmain, OB_LAMP, prim.GetName().GetText());
  ob->data = reinterpret_cast<ID *>(light);
  id_us_plus(&light->id);

  BKE_collection_object_add(bmain, sync_col, ob);

  const std::string prim_path = prim.GetPath().GetString();
  obj_map[prim_path] = ob;

  io::usd::sync_light(prim, light, false);
  io::usd::sync_xform_pull(prim, ob);

  DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
}

void SpaceUsdStage_Runtime::push_all_xforms()
{
  if (!stage)
    return;
  validate_obj_map();
  for (auto &[path, ob] : obj_map) {
    if (!ob)
      continue;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    /* Only push if the Blender transform actually changed — avoids writing override specs
     * to the current edit target for every object whenever any single object is moved. */
    if (prim && io::usd::sync_xform_differs(ob, prim)) {
      io::usd::sync_xform_push(ob, prim);
    }
  }
}

void SpaceUsdStage_Runtime::push_all_meshes()
{
  if (!stage)
    return;
  validate_obj_map();
  for (auto &[path, ob] : obj_map) {
    if (!ob || ob->type != OB_MESH)
      continue;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (!prim)
      continue;

    /* Vertex positions only change while in edit mode — skip objects not being edited.
     * Object-mode transforms are handled separately by push_all_xforms(). */
    if (!(ob->mode & OB_MODE_EDIT))
      continue;

    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (em && em->bm)
      io::usd::sync_mesh_push_bm(em->bm, prim);
  }
}

void SpaceUsdStage_Runtime::push_all_light_data()
{
  if (!stage)
    return;
  for (auto &[path, ob] : obj_map) {
    if (!ob || ob->type != OB_LAMP)
      continue;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (prim)
      io::usd::sync_light(prim, reinterpret_cast<Light *>(ob->data), true);
  }
}

void SpaceUsdStage_Runtime::resync_new_prims(const bContext *C)
{
  if (!stage || !C)
    return;
  /* Traverse the full composed scene and create Blender objects for any prim not yet tracked.
   * Existing entries in obj_map are left alone — repull_all_objects() handles their update. */
  for (pxr::UsdPrim prim : stage->Traverse()) {
    const std::string path = prim.GetPath().GetString();
    if (obj_map.count(path))
      continue;
    if (prim.IsA<pxr::UsdGeomMesh>())
      create_blender_mesh_for_prim(C, prim);
    else if (prim.IsA<pxr::UsdGeomCamera>())
      create_blender_camera_for_prim(C, prim);
    else if (prim.HasAPI<pxr::UsdLuxLightAPI>())
      create_blender_light_for_prim(C, prim);
  }
  if (Main *bmain_ctx = CTX_data_main(const_cast<bContext *>(C))) {
    if (Scene *scene = CTX_data_scene(C)) {
      if (ViewLayer *view_layer = CTX_data_view_layer(C)) {
        BKE_view_layer_synced_ensure(*bmain_ctx, scene, view_layer);
      }
    }
  }
  DEG_relations_tag_update(CTX_data_main(const_cast<bContext *>(C)));
}

void SpaceUsdStage_Runtime::repull_all_objects(int mesh_dirty_bits)
{
  if (!stage)
    return;
  const bool full = (mesh_dirty_bits < 0);
  const int mesh_bits = full ? io::usd::USD_DIRTY_ALL : mesh_dirty_bits;
  for (auto &[path, ob] : obj_map) {
    if (!ob)
      continue;

    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));

    /* A prim is "present" if it is defined (has a def specifier, not just over), active, and
     * not explicitly invisible. When a layer that solely defines a prim is muted the prim
     * loses its def → IsDefined() returns false → hide the Blender object. */
    bool prim_visible = false;
    if (prim && prim.IsDefined() && prim.IsActive()) {
      const pxr::TfToken vis = pxr::UsdGeomImageable(prim).ComputeVisibility();
      prim_visible = vis.IsEmpty() || vis != pxr::UsdGeomTokens->invisible;
    }

    if (prim_visible) {
      ob->visibility_flag &= ~OB_HIDE_VIEWPORT;
    }
    else {
      ob->visibility_flag |= OB_HIDE_VIEWPORT;
      DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
      continue;
    }

    if (ob->type == OB_MESH) {
      Mesh *me = reinterpret_cast<Mesh *>(ob->data);
      io::usd::sync_mesh_pull(prim, me, mesh_bits);
      if (full) {
        assign_bound_material(bmain, ob, prim, material_map);
      }
      DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
    }
    else if (ob->type == OB_CAMERA) {
      io::usd::sync_camera(prim, reinterpret_cast<Camera *>(ob->data), false);
      DEG_id_tag_update(&reinterpret_cast<Camera *>(ob->data)->id, ID_RECALC_ALL);
    }
    else if (ob->type == OB_LAMP) {
      io::usd::sync_light(prim, reinterpret_cast<Light *>(ob->data), false);
      DEG_id_tag_update(&reinterpret_cast<Light *>(ob->data)->id, ID_RECALC_ALL);
    }
    io::usd::sync_xform_pull(prim, ob);
    DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
  }
}

/** Sanitize a Blender ID name into a valid USD prim name token. */
static std::string make_valid_prim_name(const char *name)
{
  std::string result;
  result.reserve(std::strlen(name));
  for (const char *p = name; *p; ++p) {
    const char c = *p;
    result += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
  }
  /* USD prim names must not start with a digit. */
  if (!result.empty() && std::isdigit((unsigned char)result[0])) {
    result = "_" + result;
  }
  return result.empty() ? "Object" : result;
}

std::string SpaceUsdStage_Runtime::export_object(Object *ob, Main *bmain_in)
{
  if (!stage || !ob)
    return "";
  if (bmain_in)
    bmain = bmain_in;

  /* If already tracked, just push the latest data and return the existing path. */
  for (const auto &[path, obj] : obj_map) {
    if (obj == ob) {
      pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
      if (prim) {
        if (ob->type == OB_MESH)
          io::usd::sync_mesh_push((Mesh *)ob->data, prim, io::usd::USD_DIRTY_ALL);
        io::usd::sync_xform_push(ob, prim);
      }
      return path;
    }
  }

  /* Parent under the stage's default prim when one is set (e.g. /World), so new exports
   * appear inside the existing scene hierarchy. Fall back to pseudoroot if none is set. */
  std::string parent_path;
  pxr::UsdPrim default_prim = stage->GetDefaultPrim();
  if (default_prim) {
    parent_path = default_prim.GetPath().GetString();
  }

  const std::string base = make_valid_prim_name(ob->id.name + 2);
  std::string path_str = parent_path + "/" + base;
  for (int n = 1; stage->GetPrimAtPath(pxr::SdfPath(path_str)); ++n) {
    path_str = parent_path + "/" + base + "_" + std::to_string(n);
  }
  const pxr::SdfPath sdf_path(path_str);

  /* Define the prim using the stage's current edit target. */
  if (ob->type == OB_MESH) {
    pxr::UsdGeomMesh::Define(stage, sdf_path);
  }
  else if (ob->type == OB_CAMERA) {
    pxr::UsdGeomCamera::Define(stage, sdf_path);
  }
  else if (ob->type == OB_LAMP) {
    const Light *bl_light = reinterpret_cast<const Light *>(ob->data);
    switch (bl_light->type) {
      case LA_SUN:  pxr::UsdLuxDistantLight::Define(stage, sdf_path); break;
      case LA_AREA: pxr::UsdLuxRectLight::Define(stage, sdf_path);   break;
      case LA_SPOT:
        /* USD has no SpotLight schema — use SphereLight + ShapingAPI. */
        pxr::UsdLuxSphereLight::Define(stage, sdf_path);
        pxr::UsdLuxShapingAPI::Apply(stage->GetPrimAtPath(sdf_path));
        break;
      default:      pxr::UsdLuxSphereLight::Define(stage, sdf_path); break;
    }
  }
  else {
    pxr::UsdGeomXform::Define(stage, sdf_path);
  }

  pxr::UsdPrim prim = stage->GetPrimAtPath(sdf_path);
  if (!prim)
    return "";

  /* Sync data, then transform. */
  if (ob->type == OB_MESH)
    io::usd::sync_mesh_push((Mesh *)ob->data, prim, io::usd::USD_DIRTY_ALL);
  else if (ob->type == OB_CAMERA)
    io::usd::sync_camera(prim, (Camera *)ob->data, true);
  else if (ob->type == OB_LAMP)
    io::usd::sync_light(prim, (Light *)ob->data, true);
  io::usd::sync_xform_push(ob, prim);

  obj_map[path_str] = ob;
  return path_str;
}

void SpaceUsdStage_Runtime::close()
{
  if (stage) {
    stage_listener.unregister_listener();
    stage.Reset();
  }
  anon_layer_refs.clear();
  //obj_map.clear();
  //layers.clear();
  //prim_properties.clear();
  //prim_variants.clear();
  //prim_references.clear();
}

void SpaceUsdStage_Runtime::refresh_layers()
{
  layers.clear();
  if (!stage) {
    return;
  }

  const pxr::SdfLayerHandle root = stage->GetRootLayer();
  if (!root) {
    return;
  }

  const pxr::SdfLayerHandle edit_layer = stage->GetEditTarget().GetLayer();
  const std::vector<std::string> &muted = stage->GetMutedLayers();

  /* Walk the raw sublayer tree rather than stage->GetLayerStack(), because
   * GetLayerStack() uses the effective PcpLayerStack which *excludes* muted
   * layers — making them vanish from the panel on first mute. */
  std::vector<pxr::SdfLayerHandle> visited;
  std::function<void(const pxr::SdfLayerHandle &)> collect;
  collect = [&](const pxr::SdfLayerHandle &layer) {
    if (!layer)
      return;
    /* Skip already-visited layers (diamond sublayer patterns). */
    for (const pxr::SdfLayerHandle &v : visited) {
      if (v == layer)
        return;
    }
    visited.push_back(layer);

    /* Hold a strong ref to file-based layers so MuteLayer can't evict them from the SdfLayer
     * registry when the PcpLayerStack drops its reference. Anonymous layers are already held
     * by anon_layer_refs via usd_new_layer_exec; extend the same guard to saved layers. */
    if (!layer->IsAnonymous()) {
      pxr::SdfLayerRefPtr strong(layer);
      if (std::find(anon_layer_refs.begin(), anon_layer_refs.end(), strong) ==
          anon_layer_refs.end())
        anon_layer_refs.push_back(std::move(strong));
    }

    UsdLayerSnapshot snap;
    snap.identifier = layer->GetIdentifier();
    snap.display_name = layer->GetDisplayName();
    if (snap.display_name.empty())
      snap.display_name = snap.identifier;
    snap.is_dirty = layer->IsDirty();
    snap.is_anonymous = layer->IsAnonymous();
    snap.is_edit_target = (layer == edit_layer);
    snap.is_muted = std::find(muted.begin(), muted.end(), snap.identifier) != muted.end();
    snap.is_root = (layer == root);
    layers.push_back(std::move(snap));

    for (const std::string &sub_path : layer->GetSubLayerPaths()) {
      pxr::SdfLayerHandle sub = pxr::SdfLayer::FindRelativeToLayer(layer, sub_path);
      if (!sub)
        sub = pxr::SdfLayer::Find(sub_path);
      /* MuteLayer can cause SdfLayer::Find to return null for anonymous layers even when
       * the layer is still alive. Fall back to our own strong-ref list. */
      if (!sub) {
        for (const pxr::SdfLayerRefPtr &ref : anon_layer_refs) {
          if (ref && ref->GetIdentifier() == sub_path) {
            sub = ref;
            break;
          }
        }
      }
      if (sub)
        collect(sub);
    }
  };

  collect(root);
}

/** Collect authored inputs of a UsdShadeShader into prim_shader_inputs. */
static void collect_shader_inputs(const pxr::UsdShadeShader &shader,
                                  std::vector<UsdShaderInputSnapshot> &out)
{
  const std::string shader_path = shader.GetPrim().GetPath().GetString();
  for (const pxr::UsdShadeInput &inp : shader.GetInputs()) {
    UsdShaderInputSnapshot snap;
    snap.shader_prim_path = shader_path;
    snap.input_name = inp.GetBaseName().GetString();
    snap.has_connection = inp.HasConnectedSource();
    if (!snap.has_connection) {
      pxr::VtValue val;
      if (inp.Get(&val)) {
        snap.value_str = val.IsEmpty() ? "(empty)" : pxr::TfStringify(val);
      }
      else {
        snap.value_str = "(no value)";
      }
    }
    else {
      snap.value_str = "(connected)";
    }
    out.push_back(std::move(snap));
  }
}

void SpaceUsdStage_Runtime::refresh_prim_detail(const char *sdf_path)
{
  prim_properties.clear();
  prim_variants.clear();
  prim_references.clear();
  prim_bound_material_path.clear();
  prim_material_networks.clear();
  prim_shader_inputs.clear();
  prim_is_bindable = false;

  if (!stage || !sdf_path || sdf_path[0] == '\0') {
    return;
  }

  pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(sdf_path));
  if (!prim) {
    return;
  }

  /* A material can be bound to any imageable geometry prim (but not to a material prim). */
  prim_is_bindable = prim.IsA<pxr::UsdGeomImageable>() && !prim.IsA<pxr::UsdShadeMaterial>();

  /* -------------------------
   * BASIC PROPERTIES
   * ------------------------- */
  prim_properties.push_back({"USD Path", prim.GetPath().GetString()});
  prim_properties.push_back({"Type", prim.GetTypeName().GetString()});
  prim_properties.push_back({"Active", prim.IsActive() ? "Yes" : "No"});
  prim_properties.push_back({"Instance", prim.IsInstance() ? "Yes" : "No"});

  /* -------------------------
   * ATTRIBUTES
   * ------------------------- */
  for (const pxr::UsdAttribute &attr : prim.GetAuthoredAttributes()) {
    pxr::VtValue val;
    std::string val_str;

    if (attr.Get(&val)) {
      val_str = val.IsEmpty() ? "(empty)" : pxr::TfStringify(val);
    }
    else {
      val_str = "(no value)";
    }

    prim_properties.push_back({attr.GetName().GetString(), val_str});
  }

  /* -------------------------
   * VARIANTS
   * ------------------------- */
  pxr::UsdVariantSets vsets = prim.GetVariantSets();

  for (const std::string &name : vsets.GetNames()) {
    pxr::UsdVariantSet vset = vsets.GetVariantSet(name);
    std::string sel = vset.GetVariantSelection();
    prim_variants.push_back({name, sel.empty() ? "(none)" : sel});
  }

  /* -------------------------
   * REFERENCES
   * ------------------------- */
  {
    /* Using GetMetadata to retrieve the list of authored references. */
    pxr::SdfReferenceListOp refs;
    if (prim.GetMetadata(pxr::SdfFieldKeys->References, &refs)) {
      for (const pxr::SdfReference &ref : refs.GetPrependedItems()) {
        std::string asset = ref.GetAssetPath().empty() ? "(internal)" : ref.GetAssetPath();
        std::string primPath = ref.GetPrimPath().GetString();
        prim_references.push_back({asset, primPath.empty() ? "." : primPath});
      }
    }
  }

  /* -------------------------
   * PAYLOADS
   * ------------------------- */
  {
    /* Using GetMetadata to retrieve the list of authored payloads. */
    pxr::SdfPayloadListOp payloads;
    if (prim.GetMetadata(pxr::SdfFieldKeys->Payload, &payloads)) {
      for (const pxr::SdfPayload &payload : payloads.GetPrependedItems()) {
        std::string asset = payload.GetAssetPath().empty() ? "(internal)" : payload.GetAssetPath();
        std::string primPath = payload.GetPrimPath().GetString();
        prim_references.push_back({"PAYLOAD: " + asset, primPath.empty() ? "." : primPath});
      }
    }
  }

  /* -------------------------
   * MATERIAL BINDING (mesh prims)
   * ------------------------- */
  if (prim.IsA<pxr::UsdGeomMesh>()) {
    pxr::UsdShadeMaterialBindingAPI binding(prim);
    pxr::UsdShadeMaterial bound_mat = binding.ComputeBoundMaterial();
    if (bound_mat) {
      prim_bound_material_path = bound_mat.GetPrim().GetPath().GetString();
    }
  }

  /* -------------------------
   * MATERIAL NETWORKS + SHADER INPUTS (material prims)
   * ------------------------- */
  if (prim.IsA<pxr::UsdShadeMaterial>()) {
    pxr::UsdShadeMaterial mat(prim);

    /* UsdPreviewSurface output (universal render context). */
    pxr::UsdShadeOutput surface_out = mat.GetSurfaceOutput();
    if (surface_out) {
      pxr::UsdShadeConnectableAPI source;
      pxr::TfToken source_name;
      pxr::UsdShadeAttributeType source_type;
      if (surface_out.GetConnectedSource(&source, &source_name, &source_type)) {
        pxr::UsdShadeShader shader(source.GetPrim());
        if (shader) {
          UsdMaterialNetworkSnapshot net;
          net.label = "UsdPreviewSurface";
          net.shader_path = shader.GetPrim().GetPath().GetString();
          prim_material_networks.push_back(std::move(net));
          collect_shader_inputs(shader, prim_shader_inputs);
        }
      }
    }

    /* MaterialX output (mtlx render context). */
    static const pxr::TfToken kMtlx("mtlx", pxr::TfToken::Immortal);
    pxr::UsdShadeOutput mtlx_out = mat.GetSurfaceOutput(kMtlx);
    if (mtlx_out) {
      pxr::UsdShadeConnectableAPI source;
      pxr::TfToken source_name;
      pxr::UsdShadeAttributeType source_type;
      if (mtlx_out.GetConnectedSource(&source, &source_name, &source_type)) {
        pxr::UsdShadeShader shader(source.GetPrim());
        if (shader) {
          UsdMaterialNetworkSnapshot net;
          net.label = "MaterialX";
          net.shader_path = shader.GetPrim().GetPath().GetString();
          prim_material_networks.push_back(std::move(net));
          collect_shader_inputs(shader, prim_shader_inputs);
        }
      }
    }
  }

  /* -------------------------
   * SHADER INPUTS (shader prims selected directly)
   * ------------------------- */
  {
    pxr::UsdShadeShader shader(prim);
    if (shader && prim_shader_inputs.empty()) {
      collect_shader_inputs(shader, prim_shader_inputs);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Live material sync (USD → Blender node tree)
 * \{ */

/** Write a USD VtValue into a named Principled BSDF socket default value. */
static void apply_to_principled_socket(bNode *node,
                                       const char *socket_name,
                                       const pxr::VtValue &val)
{
  for (bNodeSocket *sock = static_cast<bNodeSocket *>(node->inputs.first); sock;
       sock = static_cast<bNodeSocket *>(sock->next))
  {
    if (!STREQ(sock->name, socket_name))
      continue;
    if (sock->type == SOCK_RGBA) {
      auto *sv = static_cast<bNodeSocketValueRGBA *>(sock->default_value);
      if (val.IsHolding<pxr::GfVec3f>()) {
        const auto &v = val.UncheckedGet<pxr::GfVec3f>();
        sv->value[0] = v[0];
        sv->value[1] = v[1];
        sv->value[2] = v[2];
        sv->value[3] = 1.0f;
      }
      else if (val.IsHolding<pxr::GfVec4f>()) {
        const auto &v = val.UncheckedGet<pxr::GfVec4f>();
        sv->value[0] = v[0];
        sv->value[1] = v[1];
        sv->value[2] = v[2];
        sv->value[3] = v[3];
      }
    }
    else if (sock->type == SOCK_FLOAT && val.IsHolding<float>()) {
      static_cast<bNodeSocketValueFloat *>(sock->default_value)->value =
          val.UncheckedGet<float>();
    }
    break;
  }
}

/** Map a Principled BSDF socket name to a USD shader input name (reverse of below). */
static const char *principled_socket_to_usd_input(const char *socket_name, bool is_mtlx)
{
  if (!is_mtlx) {
    if (STREQ(socket_name, "Base Color"))          return "diffuseColor";
    if (STREQ(socket_name, "Metallic"))            return "metallic";
    if (STREQ(socket_name, "Roughness"))           return "roughness";
    if (STREQ(socket_name, "IOR"))                 return "ior";
    if (STREQ(socket_name, "Alpha"))               return "opacity";
    if (STREQ(socket_name, "Emission Color"))      return "emissiveColor";
    if (STREQ(socket_name, "Coat Weight"))         return "clearcoat";
    if (STREQ(socket_name, "Coat Roughness"))      return "clearcoatRoughness";
  }
  else {
    if (STREQ(socket_name, "Base Color"))            return "base_color";
    if (STREQ(socket_name, "Metallic"))              return "metalness";
    if (STREQ(socket_name, "Roughness"))             return "specular_roughness";
    if (STREQ(socket_name, "Diffuse Roughness"))     return "diffuse_roughness";
    if (STREQ(socket_name, "IOR"))                   return "specular_IOR";
    if (STREQ(socket_name, "Specular Tint"))         return "specular_color";
    if (STREQ(socket_name, "Anisotropic"))           return "specular_anisotropy";
    if (STREQ(socket_name, "Emission Strength"))     return "emission";
    if (STREQ(socket_name, "Emission Color"))        return "emission_color";
    if (STREQ(socket_name, "Coat Weight"))           return "coat";
    if (STREQ(socket_name, "Coat Roughness"))        return "coat_roughness";
    if (STREQ(socket_name, "Coat IOR"))              return "coat_IOR";
    if (STREQ(socket_name, "Coat Tint"))             return "coat_color";
    if (STREQ(socket_name, "Transmission Weight"))   return "transmission";
    if (STREQ(socket_name, "Subsurface Weight"))     return "subsurface";
    if (STREQ(socket_name, "Subsurface Scale"))      return "subsurface_scale";
    if (STREQ(socket_name, "Subsurface Radius"))     return "subsurface_radius";
    if (STREQ(socket_name, "Subsurface Anisotropy")) return "subsurface_anisotropy";
    if (STREQ(socket_name, "Sheen Weight"))          return "sheen";
    if (STREQ(socket_name, "Sheen Roughness"))       return "sheen_roughness";
    if (STREQ(socket_name, "Sheen Tint"))            return "sheen_color";
    if (STREQ(socket_name, "Thin Film Thickness"))   return "thin_film_thickness";
    if (STREQ(socket_name, "Thin Film IOR"))         return "thin_film_IOR";
  }
  return nullptr;
}

/** Map a USD shader input name to a Principled BSDF socket name. Returns nullptr if unmapped. */
static const char *usd_input_to_principled_socket(const std::string &input_name, bool is_mtlx)
{
  if (!is_mtlx) {
    /* UsdPreviewSurface */
    if (input_name == "diffuseColor")        return "Base Color";
    if (input_name == "metallic")            return "Metallic";
    if (input_name == "roughness")           return "Roughness";
    if (input_name == "ior")                 return "IOR";
    if (input_name == "opacity")             return "Alpha";
    if (input_name == "emissiveColor")       return "Emission Color";
    if (input_name == "clearcoat")           return "Coat Weight";
    if (input_name == "clearcoatRoughness")  return "Coat Roughness";
  }
  else {
    /* MaterialX ND_standard_surface (Autodesk standard_surface).
     * Socket names verified against node_shader_bsdf_principled.cc. */
    if (input_name == "base_color")            return "Base Color";
    if (input_name == "metalness")             return "Metallic";
    if (input_name == "specular_roughness")    return "Roughness";
    if (input_name == "diffuse_roughness")     return "Diffuse Roughness";
    if (input_name == "specular_IOR")          return "IOR";
    if (input_name == "specular_color")        return "Specular Tint";
    if (input_name == "specular_anisotropy")   return "Anisotropic";
    if (input_name == "emission")              return "Emission Strength";
    if (input_name == "emission_color")        return "Emission Color";
    if (input_name == "coat")                  return "Coat Weight";
    if (input_name == "coat_roughness")        return "Coat Roughness";
    if (input_name == "coat_IOR")              return "Coat IOR";
    if (input_name == "coat_color")            return "Coat Tint";
    if (input_name == "transmission")          return "Transmission Weight";
    if (input_name == "subsurface")            return "Subsurface Weight";
    if (input_name == "subsurface_scale")      return "Subsurface Scale";
    if (input_name == "subsurface_radius")     return "Subsurface Radius";
    if (input_name == "subsurface_anisotropy") return "Subsurface Anisotropy";
    if (input_name == "sheen")                 return "Sheen Weight";
    if (input_name == "sheen_roughness")       return "Sheen Roughness";
    if (input_name == "sheen_color")           return "Sheen Tint";
    if (input_name == "thin_film_thickness")   return "Thin Film Thickness";
    if (input_name == "thin_film_IOR")         return "Thin Film IOR";
  }
  return nullptr;
}

void SpaceUsdStage_Runtime::sync_active_material_to_blender(const char *sdf_path, Main *bmain)
{
  if (!stage || !bmain || !sdf_path || sdf_path[0] == '\0')
    return;

  pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(sdf_path));
  if (!prim)
    return;

  /* If the active prim is a mesh (not a material), follow its material binding. */
  if (!prim.IsA<pxr::UsdShadeMaterial>()) {
    if (prim_bound_material_path.empty())
      return;
    prim = stage->GetPrimAtPath(pxr::SdfPath(prim_bound_material_path));
    if (!prim || !prim.IsA<pxr::UsdShadeMaterial>())
      return;
  }

  /* Find Blender material by USD prim name. */
  const std::string mat_name = prim.GetName().GetString();
  Material *mat = static_cast<Material *>(
      BLI_findstring(&bmain->materials, mat_name.c_str(), offsetof(ID, name) + 2));
  if (!mat || !mat->nodetree)
    return;

  /* Find the Principled BSDF node. */
  bNode *principled = nullptr;
  for (bNode *n = static_cast<bNode *>(mat->nodetree->nodes.first); n;
       n = static_cast<bNode *>(n->next))
  {
    if (n->type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = n;
      break;
    }
  }
  if (!principled)
    return;

  pxr::UsdShadeMaterial usd_mat(prim);

  auto sync_shader = [&](const pxr::UsdShadeShader &shader, bool is_mtlx) {
    for (const pxr::UsdShadeInput &inp : shader.GetInputs()) {
      if (inp.HasConnectedSource())
        continue;
      pxr::VtValue val;
      if (!inp.Get(&val))
        continue;
      const char *socket = usd_input_to_principled_socket(inp.GetBaseName().GetString(), is_mtlx);
      if (socket)
        apply_to_principled_socket(principled, socket, val);
    }
  };

  /* UsdPreviewSurface */
  {
    pxr::UsdShadeOutput surf = usd_mat.GetSurfaceOutput();
    if (surf) {
      pxr::UsdShadeConnectableAPI src;
      pxr::TfToken sname;
      pxr::UsdShadeAttributeType stype;
      if (surf.GetConnectedSource(&src, &sname, &stype))
        sync_shader(pxr::UsdShadeShader(src.GetPrim()), false);
    }
  }

  /* MaterialX */
  {
    static const pxr::TfToken kMtlx("mtlx", pxr::TfToken::Immortal);
    pxr::UsdShadeOutput mtlx = usd_mat.GetSurfaceOutput(kMtlx);
    if (mtlx) {
      pxr::UsdShadeConnectableAPI src;
      pxr::TfToken sname;
      pxr::UsdShadeAttributeType stype;
      if (mtlx.GetConnectedSource(&src, &sname, &stype))
        sync_shader(pxr::UsdShadeShader(src.GetPrim()), true);
    }
  }

  /* Mirror what rna_Node_update does: tag the specific node, then run the full
   * invariants pass so the node tree, depsgraph, and GPU material are all updated. */
  BKE_ntree_update_tag_node_property(mat->nodetree, principled);
  BKE_main_ensure_invariants(*bmain, mat->nodetree->id);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, mat);
}

void apply_usd_material_inputs_to_material(pxr::UsdStageRefPtr stage,
                                            const std::string &mat_prim_path,
                                            Material *bl_mat)
{
  if (!stage || mat_prim_path.empty() || !bl_mat || !bl_mat->nodetree)
    return;

  pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(mat_prim_path));
  if (!prim || !prim.IsA<pxr::UsdShadeMaterial>())
    return;

  bNode *principled = nullptr;
  for (bNode *n = static_cast<bNode *>(bl_mat->nodetree->nodes.first); n;
       n = static_cast<bNode *>(n->next))
  {
    if (n->type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = n;
      break;
    }
  }
  if (!principled)
    return;

  pxr::UsdShadeMaterial usd_mat(prim);

  auto sync_shader = [&](const pxr::UsdShadeShader &shader, bool is_mtlx) {
    for (const pxr::UsdShadeInput &inp : shader.GetInputs()) {
      if (inp.HasConnectedSource())
        continue;
      pxr::VtValue val;
      if (!inp.Get(&val))
        continue;
      const char *socket = usd_input_to_principled_socket(inp.GetBaseName().GetString(), is_mtlx);
      if (socket)
        apply_to_principled_socket(principled, socket, val);
    }
  };

  /* UsdPreviewSurface (universal context) */
  {
    pxr::UsdShadeOutput surf = usd_mat.GetSurfaceOutput();
    if (surf) {
      pxr::UsdShadeConnectableAPI src;
      pxr::TfToken sname;
      pxr::UsdShadeAttributeType stype;
      if (surf.GetConnectedSource(&src, &sname, &stype))
        sync_shader(pxr::UsdShadeShader(src.GetPrim()), false);
    }
  }

  /* MaterialX (mtlx context) — AMD GPUOpen files only expose this */
  {
    static const pxr::TfToken kMtlx("mtlx", pxr::TfToken::Immortal);
    pxr::UsdShadeOutput mtlx = usd_mat.GetSurfaceOutput(kMtlx);
    if (mtlx) {
      pxr::UsdShadeConnectableAPI src;
      pxr::TfToken sname;
      pxr::UsdShadeAttributeType stype;
      if (mtlx.GetConnectedSource(&src, &sname, &stype))
        sync_shader(pxr::UsdShadeShader(src.GetPrim()), true);
    }
  }

  BKE_ntree_update_tag_node_property(bl_mat->nodetree, principled);
}

void SpaceUsdStage_Runtime::push_all_materials()
{
  if (!stage || !bmain)
    return;

  /* Track material prim paths we've already pushed so shared materials are only written once. */
  std::set<std::string> pushed;

  auto push_shader = [](bNode *principled, const pxr::UsdShadeShader &shader, bool is_mtlx) {
    for (bNodeSocket *sock = static_cast<bNodeSocket *>(principled->inputs.first); sock;
         sock = static_cast<bNodeSocket *>(sock->next))
    {
      const char *usd_name = principled_socket_to_usd_input(sock->name, is_mtlx);
      if (!usd_name)
        continue;
      pxr::UsdShadeInput inp = shader.GetInput(pxr::TfToken(usd_name));
      if (!inp || inp.HasConnectedSource())
        continue;
      if (sock->type == SOCK_RGBA) {
        auto *sv = static_cast<bNodeSocketValueRGBA *>(sock->default_value);
        inp.Set(pxr::GfVec3f(sv->value[0], sv->value[1], sv->value[2]));
      }
      else if (sock->type == SOCK_FLOAT) {
        inp.Set(static_cast<bNodeSocketValueFloat *>(sock->default_value)->value);
      }
    }
  };

  for (const auto &kv : obj_map) {
    pxr::UsdPrim obj_prim = stage->GetPrimAtPath(pxr::SdfPath(kv.first));
    if (!obj_prim)
      continue;

    pxr::UsdShadeMaterialBindingAPI binding(obj_prim);
    pxr::UsdShadeMaterial usd_mat = binding.ComputeBoundMaterial();
    if (!usd_mat)
      continue;

    const std::string mat_path_str = usd_mat.GetPrim().GetPath().GetString();
    if (!pushed.insert(mat_path_str).second)
      continue;  /* already pushed this material */

    /* Find the Blender material by USD prim name. */
    const std::string mat_name = usd_mat.GetPrim().GetName().GetString();
    Material *mat = static_cast<Material *>(
        BLI_findstring(&bmain->materials, mat_name.c_str(), offsetof(ID, name) + 2));
    if (!mat || !mat->nodetree)
      continue;

    bNode *principled = nullptr;
    for (bNode *n = static_cast<bNode *>(mat->nodetree->nodes.first); n;
         n = static_cast<bNode *>(n->next))
    {
      if (n->type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        principled = n;
        break;
      }
    }
    if (!principled)
      continue;

    /* UsdPreviewSurface */
    {
      pxr::UsdShadeOutput surf = usd_mat.GetSurfaceOutput();
      if (surf) {
        pxr::UsdShadeConnectableAPI src;
        pxr::TfToken sname;
        pxr::UsdShadeAttributeType stype;
        if (surf.GetConnectedSource(&src, &sname, &stype))
          push_shader(principled, pxr::UsdShadeShader(src.GetPrim()), false);
      }
    }

    /* MaterialX */
    {
      static const pxr::TfToken kMtlx("mtlx", pxr::TfToken::Immortal);
      pxr::UsdShadeOutput mtlx = usd_mat.GetSurfaceOutput(kMtlx);
      if (mtlx) {
        pxr::UsdShadeConnectableAPI src;
        pxr::TfToken sname;
        pxr::UsdShadeAttributeType stype;
        if (mtlx.GetConnectedSource(&src, &sname, &stype))
          push_shader(principled, pxr::UsdShadeShader(src.GetPrim()), true);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UsdSkel pose push — write current armature pose into UsdSkelAnimation
 * \{ */

bool SpaceUsdStage_Runtime::push_skel_pose(Object *arm_ob, double frame)
{
  if (!stage || !arm_ob || arm_ob->type != OB_ARMATURE || !arm_ob->pose)
    return false;

  /* ── Step 1: Find the UsdSkelSkeleton prim that best matches this armature ── */
  pxr::UsdPrim best_skel_prim;
  int best_score = 0;

  /* First check obj_map — if we exported this armature, it's already tracked. */
  for (const auto &[path, obj] : obj_map) {
    if (obj != arm_ob)
      continue;
    pxr::UsdPrim p = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (p && p.IsA<pxr::UsdSkelSkeleton>()) {
      best_skel_prim = p;
      best_score = INT_MAX; /* exact match */
      break;
    }
  }

  /* Fallback: scan all Skeleton prims, score by matching bone names. */
  if (!best_skel_prim) {
    for (pxr::UsdPrim prim : stage->Traverse()) {
      if (!prim.IsA<pxr::UsdSkelSkeleton>())
        continue;
      pxr::UsdSkelSkeleton skel(prim);
      pxr::VtTokenArray joints;
      skel.GetJointsAttr().Get(&joints);
      int score = 0;
      for (const pxr::TfToken &jt : joints) {
        const std::string jstr = jt.GetString();
        const size_t pos = jstr.rfind('/');
        const std::string bname = (pos == std::string::npos) ? jstr : jstr.substr(pos + 1);
        if (BKE_pose_channel_find_name(arm_ob->pose, bname.c_str()))
          score++;
      }
      if (score > best_score) {
        best_score = score;
        best_skel_prim = prim;
      }
    }
  }

  if (!best_skel_prim || best_score == 0)
    return false;

  /* ── Step 2: Find the UsdSkelAnimation child of the Skeleton ── */
  pxr::UsdSkelAnimation skel_anim;
  for (const pxr::UsdPrim &child : best_skel_prim.GetChildren()) {
    if (child.IsA<pxr::UsdSkelAnimation>()) {
      skel_anim = pxr::UsdSkelAnimation(child);
      break;
    }
  }
  if (!skel_anim)
    return false;

  /* ── Step 3: Get the ordered joint list from the animation ── */
  pxr::VtTokenArray joints;
  skel_anim.GetJointsAttr().Get(&joints);
  if (joints.empty())
    return false;

  /* ── Step 4: Build joint-local transform matrix for each joint ──
   * Mirrors parent_relative_pose_mat() from usd_writer_armature.cc. */
  pxr::VtArray<pxr::GfMatrix4d> xforms(joints.size(), pxr::GfMatrix4d(1.0));
  bool any_found = false;

  for (int i = 0; i < (int)joints.size(); i++) {
    const std::string jstr = joints[i].GetString();
    const size_t pos = jstr.rfind('/');
    const std::string bname = (pos == std::string::npos) ? jstr : jstr.substr(pos + 1);

    const bPoseChannel *pchan = BKE_pose_channel_find_name(arm_ob->pose, bname.c_str());
    if (!pchan)
      continue;

    any_found = true;
    const pxr::GfMatrix4f pose_mat(pchan->pose_mat);
    if (pchan->parent) {
      const pxr::GfMatrix4f parent_mat(pchan->parent->pose_mat);
      xforms[i] = pxr::GfMatrix4d(pose_mat * parent_mat.GetInverse());
    }
    else {
      xforms[i] = pxr::GfMatrix4d(pose_mat);
    }
  }

  if (!any_found)
    return false;

  /* ── Step 5: Decompose to TRS and write to the animation ── */
  pxr::VtArray<pxr::GfVec3f> translations;
  pxr::VtArray<pxr::GfQuatf> rotations;
  pxr::VtArray<pxr::GfVec3h> scales;

  if (!pxr::UsdSkelDecomposeTransforms(xforms, &translations, &rotations, &scales))
    return false;

  const pxr::UsdTimeCode time_code(frame);
  skel_anim.GetTranslationsAttr().Set(translations, time_code);
  skel_anim.GetRotationsAttr().Set(rotations, time_code);
  skel_anim.GetScalesAttr().Set(scales, time_code);
  return true;
}

/** \} */

}  // namespace blender
