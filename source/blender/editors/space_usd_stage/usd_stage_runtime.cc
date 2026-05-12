/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_stage_runtime.hh"
#include "usd_mesh_sync.hh"
#include "usd_notice_handler.hh"
#include "usd_scene_sync.hh"
#include "usd_types_sync.hh"
#include "usd_xform_sync.hh"

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
#include <pxr/usd/usdGeom/mesh.h>

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BLI_listbase.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DNA_mesh_types.h"
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
    /* Blender -> USD  */
    if (ob->type == OB_MESH) {
      io::usd::sync_mesh_push((Mesh *)ob->data, prim, dirty_bits);
    }
    io::usd::sync_xform_push(ob, prim);
  }
  else {
    /* USD -> Blender  */
    if (ob->type == OB_MESH) {
      io::usd::sync_mesh_pull(prim, (Mesh *)ob->data, dirty_bits);
    }
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

void SpaceUsdStage_Runtime::populate_blender_from_stage(const bContext *C)
{
  obj_map.clear();

  pxr::UsdPrimRange range = stage->Traverse();
  for (pxr::UsdPrim prim : range) {
    if (prim.IsA<pxr::UsdGeomMesh>()) {
      create_blender_mesh_for_prim(C, prim);
    }
  }

  Main *bmain = CTX_data_main(C);
  DEG_relations_tag_update(bmain);
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

  DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
}

void SpaceUsdStage_Runtime::push_all_xforms()
{
  if (!stage)
    return;
  for (auto &[path, ob] : obj_map) {
    if (!ob)
      continue;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (prim) {
      io::usd::sync_xform_push(ob, prim);
    }
  }
}

void SpaceUsdStage_Runtime::push_all_meshes()
{
  if (!stage)
    return;
  for (auto &[path, ob] : obj_map) {
    if (!ob || ob->type != OB_MESH)
      continue;
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (!prim)
      continue;

    Mesh *me = reinterpret_cast<Mesh *>(ob->data);

    if (ob->mode & OB_MODE_EDIT) {
      /* In Edit Mode the base mesh positions are stale — read from the live BMesh instead. */
      BMEditMesh *em = BKE_editmesh_from_object(ob);
      if (em && em->bm) {
        io::usd::sync_mesh_push_bm(em->bm, prim);
      }
    }
    else {
      io::usd::sync_mesh_push(me, prim, io::usd::USD_DIRTY_ALL);
    }
  }
}

void SpaceUsdStage_Runtime::repull_all_objects()
{
  if (!stage)
    return;
  for (auto &[path, ob] : obj_map) {
    pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(path));
    if (!prim || !ob)
      continue;
    if (ob->type == OB_MESH) {
      Mesh *me = reinterpret_cast<Mesh *>(ob->data);
      io::usd::sync_mesh_pull(prim, me, io::usd::USD_DIRTY_ALL);
      DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);
    }
    io::usd::sync_xform_pull(prim, ob);
    DEG_id_tag_update(&ob->id, ID_RECALC_ALL);
  }
}

void SpaceUsdStage_Runtime::close()
{
  if (stage) {
    stage_listener.unregister_listener();
    stage.Reset();
  }
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
  
  const pxr::SdfLayerHandle edit_layer = stage->GetEditTarget().GetLayer();
  for (const pxr::SdfLayerHandle &layer : stage->GetLayerStack(true)) {
    UsdLayerSnapshot snap;
    snap.identifier = layer->GetIdentifier();
    snap.display_name = layer->GetDisplayName();

    if (snap.display_name.empty()) {
      snap.display_name = snap.identifier;
    }

    snap.is_dirty = layer->IsDirty();
    snap.is_anonymous = layer->IsAnonymous();
    snap.is_edit_target = (layer == edit_layer);

    layers.push_back(std::move(snap));
  }
}

void SpaceUsdStage_Runtime::refresh_prim_detail(const char *sdf_path)
{
  prim_properties.clear();
  prim_variants.clear();
  prim_references.clear();

  if (!stage || !sdf_path || sdf_path[0] == '\0') {
    return;
  }

  pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(sdf_path));
  if (!prim) {
    return;
  }

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
}

}  // namespace blender
