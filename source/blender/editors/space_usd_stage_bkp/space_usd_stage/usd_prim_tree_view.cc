/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_prim_tree_view.hh"
#include "usd_stage_runtime.hh"

#include "BKE_context.hh"

#include "BLI_string.h"

#include "DNA_space_types.h"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>

namespace blender {

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
    ui::Layout &sub = row.row(true);

    /* Name column. */
    sub.label(prim_.GetName().GetString().c_str(), ICON_NONE);

    /* Type column. */
    const std::string type = prim_.GetTypeName().GetString();
    sub.label(type.empty() ? "—" : type.c_str(), ICON_NONE);

    /* Payload indicator. */
    sub.label(prim_.HasPayload() ? "P" : "", ICON_NONE);
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
    }

    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  }

  std::optional<bool> should_be_active() const override
  {
    if (!suss_) {
      return false;
    }
    return prim_.GetPath().GetString() == suss_->active_prim_path;
  }

 private:
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

void usd_stage_prim_tree_draw(const bContext *C, ui::Layout &layout)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    layout.label("No USD stage open.", ICON_INFO);
    return;
  }

  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block,
      "USD Prim Tree",
      std::make_unique<UsdPrimTreeView>(suss->runtime->stage, suss));

  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout, /*add_box=*/false);
}

}  // namespace blender
