/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_stage_runtime.hh"

#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/sdf/layer.h>

namespace blender {

void SpaceUsdStage_Runtime::open(const char *filepath)
{
  stage = pxr::UsdStage::Open(filepath);
  if (!stage) {
    return;
  }
  refresh_layers();
}

void SpaceUsdStage_Runtime::close()
{
  stage.Reset();
  layers.clear();
  prim_properties.clear();
  prim_variants.clear();
  prim_references.clear();
}

void SpaceUsdStage_Runtime::refresh_layers()
{
  layers.clear();
  if (!stage) {
    return;
  }

  const pxr::SdfLayerHandle edit_layer = stage->GetEditTarget().GetLayer();

  for (const pxr::SdfLayerHandle &layer :
       stage->GetLayerStack(/*includeSessionLayers=*/true))
  {
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

  /* Basic metadata. */
  prim_properties.push_back({"USD Path", prim.GetPath().GetString()});
  prim_properties.push_back({"Type", prim.GetTypeName().GetString()});
  prim_properties.push_back({"Active", prim.IsActive() ? "Yes" : "No"});
  prim_properties.push_back({"Instance", prim.IsInstance() ? "Yes" : "No"});

  /* Attributes. */
  for (const pxr::UsdAttribute &attr : prim.GetAttributes()) {
    pxr::VtValue val;
    std::string val_str;
    if (attr.Get(&val)) {
      val_str = val.GetTypeName().c_str();
    }
    else {
      val_str = "(no value)";
    }
    prim_properties.push_back({attr.GetName().GetString(), val_str});
  }

  /* Variants. */
  pxr::UsdVariantSets vsets = prim.GetVariantSets();
  for (const std::string &vset_name : vsets.GetNames()) {
    pxr::UsdVariantSet vset = vsets.GetVariantSet(vset_name);
    std::string sel = vset.GetVariantSelection();
    prim_variants.push_back({vset_name, sel.empty() ? "(none)" : sel});
  }
}

}  // namespace blender
