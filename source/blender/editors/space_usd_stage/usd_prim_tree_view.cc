/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_prim_tree_view.hh"
#include "usd_stage_runtime.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_screen.hh"
#include "DEG_depsgraph.hh"
#include "ED_screen.hh"

#include "BLI_listbase.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"

#include "BLT_translation.hh"

#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"

#include "WM_api.hh"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Type → icon
 * \{ */

static int usd_prim_type_icon(const pxr::UsdPrim &prim)
{
  const std::string t = prim.GetTypeName().GetString();
  if (t == "Mesh")                         return ICON_MESH_DATA;
  if (t == "Xform")                        return ICON_EMPTY_DATA;
  if (t == "Scope")                        return ICON_SCENE_DATA;
  if (t == "Camera")                       return ICON_CAMERA_DATA;
  if (t == "BasisCurves" || t == "Curves") return ICON_CURVE_DATA;
  if (t == "Points")                       return ICON_POINTCLOUD_DATA;
  if (t == "PointInstancer")               return ICON_PARTICLES;
  if (t == "Material")                     return ICON_MATERIAL_DATA;
  if (t == "Shader")                       return ICON_NODE_MATERIAL;
  if (t.find("Light") != std::string::npos) return ICON_LIGHT_DATA;
  if (t == "SkelRoot" || t == "Skeleton")  return ICON_ARMATURE_DATA;
  return ICON_OBJECT_DATA;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Prim tree item
 * \{ */

class UsdPrimItem : public ui::AbstractTreeViewItem {
 public:
  UsdPrimItem(const pxr::UsdPrim &prim, SpaceUsdStage *suss)
      : prim_(prim), suss_(suss)
  {
    label_ = prim.GetName().GetString();
  }

  void build_row(ui::Layout &row) override
  {
    const std::string type = prim_.GetTypeName().GetString();
    const bool has_payload   = prim_.HasPayload();
    const bool has_reference = prim_.HasAuthoredReferences();
    const bool is_instance   = prim_.IsInstance();
    const bool is_inactive   = !prim_.IsActive();

    /* Name column — icon reflects prim type. */
    ui::Layout &name_col = row.row(false);
    name_col.label(prim_.GetName().GetString().c_str(),
                   is_inactive ? ICON_GHOST_DISABLED : usd_prim_type_icon(prim_));

    /* Type label. */
    {
      ui::Layout &type_col = row.row(false);
      type_col.scale_x_set(0.55f);
      type_col.label(type.empty() ? "\xe2\x80\x94" /* em-dash */ : type.c_str(), ICON_NONE);
    }

    /* Indicator icons (payload / reference / instance). */
    {
      ui::Layout &flag_col = row.row(true);
      flag_col.scale_x_set(0.30f);
      if (has_payload)   flag_col.label("", ICON_PACKAGE);
      if (has_reference) flag_col.label("", ICON_LINK_BLEND);
      if (is_instance)   flag_col.label("", ICON_LIBRARY_DATA_INDIRECT);
    }
  }

  void on_activate(bContext &C) override
  {
    if (!suss_) {
      return;
    }
    const std::string path = prim_.GetPath().GetString();
    BLI_strncpy(suss_->active_prim_path, path.c_str(), sizeof(suss_->active_prim_path));

    if (suss_->runtime) {
      suss_->runtime->refresh_prim_detail(suss_->active_prim_path);

      /* Select the corresponding Blender object in the viewport if one was synced. */
      auto it = suss_->runtime->obj_map.find(path);
      if (it != suss_->runtime->obj_map.end() && it->second) {
        ViewLayer *view_layer = CTX_data_view_layer(&C);
        Scene *scene = CTX_data_scene(&C);
        Main *bmain = CTX_data_main(&C);
        if (view_layer && scene && bmain) {
          BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
          Base *base = BKE_view_layer_base_find(view_layer, it->second);
          if (base) {
            for (Base *b = view_layer->object_bases.first_as<Base>();
                 b != nullptr;
                 b = b->next)
            {
              b->flag &= ~BASE_SELECTED;
            }
            base->flag |= BASE_SELECTED;
            view_layer->basact = base;
            DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
            WM_event_add_notifier(&C, NC_SCENE | ND_OB_SELECT, scene);
          }
        }
      }
    }

    ScrArea *area = CTX_wm_area(&C);
    if (area) {
      ED_area_tag_redraw(area);
    }
    WM_event_add_notifier(&C, NC_SPACE, nullptr);
  }

  std::optional<bool> should_be_active() const override
  {
    if (!suss_) {
      return false;
    }
    return prim_.GetPath().GetString() == suss_->active_prim_path;
  }

  void build_context_menu(bContext & /*C*/, ui::Layout &column) const override
  {
    const std::string path = prim_.GetPath().GetString();

    ui::Layout &row = column.row(false);
    /* Only prims this stage defines itself can be moved; ones coming from a reference or a
     * payload live in a layer that is not part of this stage's layer stack. */
    row.active_set(prim_defined_in_layer_stack());
    PointerRNA ptr = row.op("USD_STAGE_OT_prim_to_new_layer", "Move to New Layer", ICON_ADD);
    RNA_string_set(&ptr, "prim_path", path.c_str());
  }

 private:
  /** True when a layer of the stage's own layer stack authors this prim. */
  bool prim_defined_in_layer_stack() const
  {
    const pxr::UsdStagePtr stage = prim_.GetStage();
    if (!stage) {
      return false;
    }
    const pxr::SdfPath &path = prim_.GetPath();
    for (const pxr::SdfLayerHandle &layer : stage->GetLayerStack(false)) {
      if (layer && layer->GetPrimAtPath(path)) {
        return true;
      }
    }
    return false;
  }

  pxr::UsdPrim prim_;
  SpaceUsdStage *suss_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Prim tree view
 * \{ */

class UsdPrimTreeView : public ui::AbstractTreeView {
 public:
  UsdPrimTreeView(pxr::UsdStageRefPtr stage, SpaceUsdStage *suss)
      : stage_(stage), suss_(suss)
  {
  }

  void build_tree() override
  {
    if (!stage_) {
      return;
    }
    build_children(*this, stage_->GetPseudoRoot());
  }

 private:
  void build_children(ui::TreeViewItemContainer &parent, const pxr::UsdPrim &prim)
  {
    for (const pxr::UsdPrim &child : prim.GetChildren()) {
      UsdPrimItem &item = parent.add_tree_item<UsdPrimItem>(child, suss_);
      build_children(item, child);
    }
  }

  pxr::UsdStageRefPtr stage_;
  SpaceUsdStage *suss_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Walk spacedata to find SpaceUsdStage even if file browser is on top
 * \{ */

static SpaceUsdStage *suss_from_context(const bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return nullptr;
  }
  /* The file browser pushes SpaceFile on top; walk to find our space. */
  for (SpaceLink &sl : area->spacedata) {
    if (sl.spacetype == SPACE_USD_STAGE) {
      return reinterpret_cast<SpaceUsdStage *>(&sl);
    }
  }
  return nullptr;
}

/** \} */

void usd_stage_prim_tree_draw(const bContext *C, ui::Layout &layout)
{
  SpaceUsdStage *suss = suss_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    if (suss && suss->filepath[0] != '\0') {
      layout.label("Opening stage\xe2\x80\xa6", ICON_TIME);
    }
    else {
      layout.label("No USD stage open. Use File > Open.", ICON_INFO);
    }
    return;
  }

  /* Column header row. */
  {
    ui::Layout &hdr = layout.row(false);
    hdr.label("Name", ICON_NONE);
    {
      ui::Layout &tc = hdr.row(false);
      tc.scale_x_set(0.55f);
      tc.label("Type", ICON_NONE);
    }
    {
      ui::Layout &fc = hdr.row(false);
      fc.scale_x_set(0.30f);
      fc.label("", ICON_NONE);
    }
  }

  layout.separator();

  ui::Block *block = layout.block();
  ui::block_layout_set_current(block, &layout);
  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block,
      "USD Prim Tree",
      std::make_unique<UsdPrimTreeView>(suss->runtime->stage, suss));

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout, /*add_box=*/false);
}

/* -------------------------------------------------------------------- */
/** \name Panel registration for the WINDOW region
 * \{ */

static void usd_prim_tree_panel_draw(const bContext *C, Panel *panel)
{
  usd_stage_prim_tree_draw(C, *panel->layout);
}

void usd_stage_prim_tree_panel_register(ARegionType *art)
{
  PanelType *pt = MEM_new_zeroed<PanelType>("usd stage prim tree panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_prim_tree");
  STRNCPY_UTF8(pt->label, N_("Prim Tree"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_WINDOW;
  pt->flag = PANEL_TYPE_NO_HEADER;
  pt->draw = usd_prim_tree_panel_draw;
  BLI_addtail(&art->paneltypes, pt);
}

/** \} */

}  // namespace blender
