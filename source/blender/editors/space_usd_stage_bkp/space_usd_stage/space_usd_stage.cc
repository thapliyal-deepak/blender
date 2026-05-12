/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spusdstage
 *
 * USD Stage Editor — shows the USD prim hierarchy from an open UsdStage.
 */

#include <cstring>
#include <memory>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"

#include "ED_fileselect.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "BLF_api.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "space_usd_stage.hh"
#include "usd_prim_tree_view.hh"
#include "usd_stage_panels.hh"
#include "usd_stage_runtime.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Runtime helpers
 * \{ */

static SpaceUsdStage_Runtime *usd_stage_runtime_ensure(SpaceUsdStage *suss)
{
  if (!suss->runtime) {
    suss->runtime = MEM_new<SpaceUsdStage_Runtime>("SpaceUsdStage_Runtime");
  }
  return suss->runtime;
}

static void usd_stage_runtime_free(SpaceUsdStage *suss)
{
  if (suss->runtime) {
    MEM_delete(suss->runtime);
    suss->runtime = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Space callbacks
 * \{ */

static SpaceLink *usd_stage_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  SpaceUsdStage *suss = MEM_new<SpaceUsdStage>("SpaceUsdStage");
  suss->spacetype = SPACE_USD_STAGE;

  /* Header. */
  ARegion *region = BKE_area_region_new();
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;
  BLI_addtail(&suss->regionbase, region);

  /* Side panel (Properties / Variants / Layers). */
  region = BKE_area_region_new();
  region->regiontype = RGN_TYPE_UI;
  region->alignment = RGN_ALIGN_RIGHT;
  BLI_addtail(&suss->regionbase, region);

  /* Main prim tree. */
  region = BKE_area_region_new();
  region->regiontype = RGN_TYPE_WINDOW;
  BLI_addtail(&suss->regionbase, region);

  return reinterpret_cast<SpaceLink *>(suss);
}

static void usd_stage_free(SpaceLink *sl)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(sl);
  usd_stage_runtime_free(suss);
}

static void usd_stage_init(wmWindowManager * /*wm*/, ScrArea *area)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(area->spacedata.first);
  SpaceUsdStage_Runtime *rt = usd_stage_runtime_ensure(suss);

  /* Re-open stage if we have a filepath but no open stage (e.g. after undo). */
  if (!rt->is_open() && suss->filepath[0] != '\0') {
    rt->open(suss->filepath);
  }
}

static SpaceLink *usd_stage_duplicate(SpaceLink *sl)
{
  SpaceUsdStage *suss_old = reinterpret_cast<SpaceUsdStage *>(sl);
  SpaceUsdStage *suss_new = MEM_new<SpaceUsdStage>("SpaceUsdStage duplicate");
  *suss_new = *suss_old;
  suss_new->runtime = nullptr; /* Runtime is not shared. */
  return reinterpret_cast<SpaceLink *>(suss_new);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static wmOperatorStatus usd_stage_open_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  if (!suss) {
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  STRNCPY(suss->filepath, filepath);

  SpaceUsdStage_Runtime *rt = usd_stage_runtime_ensure(suss);
  rt->close();
  rt->open(suss->filepath);

  if (!rt->is_open()) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to open USD stage: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  ED_area_tag_redraw(CTX_wm_area(C));
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_stage_open_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return usd_stage_open_exec(C, op);
  }
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void USD_STAGE_OT_open(wmOperatorType *ot)
{
  ot->name = "Open USD Stage";
  ot->idname = "USD_STAGE_OT_open";
  ot->description = "Open a USD file in the stage editor";
  ot->exec = usd_stage_open_exec;
  ot->invoke = usd_stage_open_invoke;
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_USD,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

static void usd_stage_operatortypes()
{
  WM_operatortype_append(USD_STAGE_OT_open);
}

static void usd_stage_keymap(wmKeyConfig * /*keyconf*/) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Header
 * \{ */

static void usd_stage_header_draw(const bContext *C, Header *header)
{
  const SpaceUsdStage *suss = reinterpret_cast<const SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *header->layout;

  layout.op("USD_STAGE_OT_open", "Open", ICON_FILE_FOLDER);

  layout.separator();

  const char *label = (suss && suss->filepath[0] != '\0') ? suss->filepath :
                                                             "No stage open";
  layout.label(label, ICON_NONE);
}

static void usd_stage_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void usd_stage_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main region (prim tree)
 * \{ */

static void usd_stage_main_region_init(wmWindowManager *wm, ARegion *region)
{
  ui::view2d_region_reinit(&region->v2d, ui::V2D_COMMONVIEW_LIST, region->winx, region->winy);
  WM_keymap_ensure(wm->runtime->defaultconf, "USD Stage", SPACE_USD_STAGE, RGN_TYPE_WINDOW);
}

static void usd_stage_main_region_draw(const bContext *C, ARegion *region)
{
  ui::theme::frame_buffer_clear(TH_BACK);

  /* Build a layout block and draw the prim tree into it. */
  const uiStyle *style = ui::style_get_dpi();
  ui::Block *block = ui::block_begin(C, region, __func__, ui::EmbossType::None);
  ui::Layout &layout = ui::block_layout(block,
                                        ui::LayoutDirection::Vertical,
                                        ui::LayoutType::Panel,
                                        0,
                                        0,
                                        region->winx,
                                        0,
                                        0,
                                        style);

  usd_stage_prim_tree_draw(C, layout);

  const int2 size = ui::block_layout_resolve(block);
  ui::view2d_totRect_set(&region->v2d, size.x, -size.y);
  ui::view2d_scrollers_draw(&region->v2d, nullptr);
  ui::block_draw(C, block);
}

static void usd_stage_main_region_listener(const wmRegionListenerParams * /*params*/) {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI (side panel) region
 * \{ */

static void usd_stage_ui_region_init(wmWindowManager *wm, ARegion *region)
{
  ED_region_panels_init(wm, region);
}

static void usd_stage_ui_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels_draw(C, region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_spacetype_usd_stage()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();

  st->spaceid = SPACE_USD_STAGE;
  STRNCPY(st->name, "USD Stage");

  st->create = usd_stage_create;
  st->free = usd_stage_free;
  st->init = usd_stage_init;
  st->duplicate = usd_stage_duplicate;
  st->operatortypes = usd_stage_operatortypes;
  st->keymap = usd_stage_keymap;

  /* Main region — prim tree. */
  ARegionType *art = MEM_new<ARegionType>("spacetype usd stage main region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;
  art->init = usd_stage_main_region_init;
  art->draw = usd_stage_main_region_draw;
  art->listener = usd_stage_main_region_listener;
  BLI_addhead(&st->regiontypes, art);

  /* UI region — Properties / Variants / Layers panels. */
  art = MEM_new<ARegionType>("spacetype usd stage ui region");
  art->regionid = RGN_TYPE_UI;
  art->prefsizex = UI_SIDEBAR_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_UI;
  art->init = usd_stage_ui_region_init;
  art->draw = usd_stage_ui_region_draw;
  BLI_addhead(&st->regiontypes, art);

  usd_stage_panels_register(art);

  /* Header region. */
  art = MEM_new<ARegionType>("spacetype usd stage header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;
  art->init = usd_stage_header_region_init;
  art->draw = usd_stage_header_region_draw;

  HeaderType *ht = MEM_new_zeroed<HeaderType>("usd stage header type");
  STRNCPY(ht->idname, "USD_STAGE_HT_header");
  ht->space_type = SPACE_USD_STAGE;
  ht->region_type = RGN_TYPE_HEADER;
  ht->draw = usd_stage_header_draw;
  BLI_addtail(&art->headertypes, ht);

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

/** \} */

}  // namespace blender
