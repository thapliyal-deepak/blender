/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WITH_USD

#  include "tree_display_usd_stage.hh"

#  include "../outliner_intern.hh"

#  include "DNA_outliner_types.h"
#  include "DNA_screen_types.h"
#  include "DNA_space_types.h"
#  include "DNA_workspace_types.h"

/* Access SpaceUsdStage_Runtime (stage, prim caches). */
#  include "../../../../editors/space_usd_stage/usd_stage_runtime.hh"

#  include <pxr/usd/usd/prim.h>
#  include <pxr/usd/usd/primRange.h>
#  include <pxr/usd/usd/stage.h>

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

static int usd_type_icon(const pxr::UsdPrim &prim)
{
  const std::string t = prim.GetTypeName().GetString();
  if (t == "Mesh")                          return ICON_MESH_DATA;
  if (t == "Xform")                         return ICON_EMPTY_DATA;
  if (t == "Scope")                         return ICON_SCENE_DATA;
  if (t == "Camera")                        return ICON_CAMERA_DATA;
  if (t == "BasisCurves" || t == "Curves")  return ICON_CURVE_DATA;
  if (t == "Points")                        return ICON_POINTCLOUD_DATA;
  if (t == "PointInstancer")                return ICON_PARTICLES;
  if (t == "Material")                      return ICON_MATERIAL_DATA;
  if (t == "Shader")                        return ICON_NODE_MATERIAL;
  if (t.find("Light") != std::string::npos) return ICON_LIGHT_DATA;
  if (t == "SkelRoot" || t == "Skeleton")   return ICON_ARMATURE_DATA;
  return ICON_OBJECT_DATA;
}

/** Find the first open UsdStage by walking all areas of the active workspace. */
static const pxr::UsdStageRefPtr *find_active_stage(const TreeSourceData &source_data)
{
  if (!source_data.workspace) {
    return nullptr;
  }
  for (WorkSpaceLayout *layout = static_cast<WorkSpaceLayout *>(
           source_data.workspace->layouts.first);
       layout;
       layout = static_cast<WorkSpaceLayout *>(layout->next))
  {
    bScreen *screen = layout->screen;
    if (!screen) {
      continue;
    }
    for (ScrArea *area = static_cast<ScrArea *>(screen->areabase.first); area;
         area = static_cast<ScrArea *>(area->next))
    {
      for (SpaceLink &sl : area->spacedata) {
        if (sl.spacetype == SPACE_USD_STAGE) {
          SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(&sl);
          if (suss->runtime && suss->runtime->is_open()) {
            return &suss->runtime->stage;
          }
        }
      }
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name TreeDisplayUsdStage
 * \{ */

TreeDisplayUsdStage::TreeDisplayUsdStage(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

void TreeDisplayUsdStage::build_prim_subtree(ListBaseT<TreeElement> &tree,
                                             TreeElement *parent,
                                             void *stage_raw,
                                             void *prim_raw)
{
  const pxr::UsdStageRefPtr &stage = *static_cast<const pxr::UsdStageRefPtr *>(stage_raw);
  const pxr::UsdPrim &prim = *static_cast<const pxr::UsdPrim *>(prim_raw);

  for (const pxr::UsdPrim &child : prim.GetChildren()) {
    const std::string type = child.GetTypeName().GetString();

    /* Build a label: "Name  [Type]"  or just "Name" for typeless prims. */
    std::string label = child.GetName().GetString();
    if (!type.empty()) {
      label += "  [" + type + "]";
    }
    if (child.HasPayload()) {
      label += "  \xf0\x9f\x93\xa6"; /* UTF-8 package emoji as payload marker */
    }
    prim_labels_.push_back(std::move(label));

    TreeElement *te = add_element(
        &tree, nullptr, (void *)prim_labels_.back().c_str(), parent, TSE_USD_PRIM, 0);

    te->name = prim_labels_.back().c_str();

    /* Recurse. */
    pxr::UsdPrim child_copy = child;
    build_prim_subtree(te->subtree, te, stage_raw, &child_copy);
  }
}

ListBaseT<TreeElement> TreeDisplayUsdStage::build_tree(const TreeSourceData &source_data)
{
  ListBaseT<TreeElement> tree = {nullptr};
  prim_labels_.clear();

  const pxr::UsdStageRefPtr *stage_ptr = find_active_stage(source_data);
  if (!stage_ptr || !*stage_ptr) {
    /* No open USD stage — show a placeholder. */
    prim_labels_.push_back("No USD stage open  (open a USD file via File \xe2\x86\x92 Import)");
    TreeElement *te = add_element(
        &tree, nullptr, (void *)prim_labels_.back().c_str(), nullptr, TSE_USD_PRIM, 0);
    te->name = prim_labels_.back().c_str();
    return tree;
  }

  const pxr::UsdStageRefPtr &stage = *stage_ptr;
  pxr::UsdPrim pseudo_root = stage->GetPseudoRoot();

  /* Build top-level prims from the pseudo-root. */
  for (const pxr::UsdPrim &child : pseudo_root.GetChildren()) {
    const std::string type = child.GetTypeName().GetString();
    std::string label = child.GetName().GetString();
    if (!type.empty()) {
      label += "  [" + type + "]";
    }
    prim_labels_.push_back(std::move(label));

    TreeElement *te = add_element(
        &tree, nullptr, (void *)prim_labels_.back().c_str(), nullptr, TSE_USD_PRIM, 0);
    te->name = prim_labels_.back().c_str();

    pxr::UsdPrim child_copy = child;
    build_prim_subtree(te->subtree, te, (void *)stage_ptr, &child_copy);
  }

  return tree;
}

/** \} */

}  // namespace blender::ed::outliner

#endif /* WITH_USD */
