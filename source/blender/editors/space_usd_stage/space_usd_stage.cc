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
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"

#include "ED_fileselect.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "BLF_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <stdexcept>

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
  suss->runtime = nullptr;
  suss->filepath[0] = '\0';
  suss->active_prim_path[0] = '\0';
  suss->edit_attr_name[0] = '\0';
  suss->edit_attr_value[0] = '\0';

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

  /* Main window — prim tree. */
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

  /* Re-open stage if filepath is stored but stage is not loaded (e.g. after undo/restart). */
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

/* When exec is called via the file-selector the file browser occupies
 * area->spacedata.first, so CTX_wm_space_data() returns a SpaceFile.
 * Walk the area's space stack to find the actual USD Stage space. */
static SpaceUsdStage *usd_stage_from_context(bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return nullptr;
  }
  for (SpaceLink &sl : area->spacedata) {
    if (sl.spacetype == SPACE_USD_STAGE) {
      return reinterpret_cast<SpaceUsdStage *>(&sl);
    }
  }
  return nullptr;
}

/* Find the SpaceUsdStage in a specific area, or fall back to searching the current screen. */
static SpaceUsdStage *usd_stage_find_or_open(bContext *C, ScrArea *hint_area, ScrArea **r_area)
{
  /* 1. Try the hinted area first (set during invoke). */
  if (hint_area) {
    for (SpaceLink &sl : hint_area->spacedata) {
      if (sl.spacetype == SPACE_USD_STAGE) {
        if (r_area) {
          *r_area = hint_area;
        }
        return reinterpret_cast<SpaceUsdStage *>(&sl);
      }
    }
  }

  /* 2. Search every area on the current screen. */
  bScreen *screen = CTX_wm_screen(C);
  if (screen) {
    for (ScrArea *area = static_cast<ScrArea *>(screen->areabase.first); area;
         area = static_cast<ScrArea *>(area->next))
    {
      for (SpaceLink &sl : area->spacedata) {
        if (sl.spacetype == SPACE_USD_STAGE) {
          if (r_area) {
            *r_area = area;
          }
          return reinterpret_cast<SpaceUsdStage *>(&sl);
        }
      }
    }
  }

  return nullptr;
}

static wmOperatorStatus usd_stage_open_exec(bContext *C, wmOperator *op)
{
  ScrArea *hint_area = static_cast<ScrArea *>(op->customdata);
  op->customdata = nullptr;

  ScrArea *suss_area = nullptr;
  SpaceUsdStage *suss = usd_stage_find_or_open(C, hint_area, &suss_area);

  if (!suss) {
    /* No USD Stage Editor open — open a new floating window. */
    wmWindow *win = WM_window_open_temp(C, "USD Stage Viewer", SPACE_USD_STAGE, false);
    if (!win) {
      return OPERATOR_CANCELLED;
    }
    /* The new window's screen has exactly one area with SPACE_USD_STAGE. */
    bScreen *new_screen = WM_window_get_active_screen(win);
    suss_area = BKE_screen_find_big_area(new_screen, SPACE_USD_STAGE, 0);
    if (!suss_area) {
      return OPERATOR_CANCELLED;
    }
    suss = reinterpret_cast<SpaceUsdStage *>(suss_area->spacedata.first);
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  STRNCPY(suss->filepath, filepath);

  SpaceUsdStage_Runtime *rt = usd_stage_runtime_ensure(suss);
  rt->close();
  rt->open(suss->filepath);

  if (rt->is_open()) {
    rt->populate_blender_from_stage(C);

    DEG_relations_tag_update(CTX_data_main(C));
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);

    if (suss_area) {
      ED_area_tag_redraw(suss_area);
    }
    WM_event_add_notifier(C, NC_SPACE, nullptr);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

static wmOperatorStatus usd_stage_open_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return usd_stage_open_exec(C, op);
  }
  /* Stash the originating area so exec can find it after the file browser replaces the context. */
  op->customdata = CTX_wm_area(C);
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

/* -------------------------------------------------------------------- */
/** \name Set Attribute Value operator
 * \{ */

/* Forward declaration — defined after USD_STAGE_OT_set_attr. */
static bool usd_write_attr_value(pxr::UsdPrim prim,
                                 const char *attr_name,
                                 const char *new_value,
                                 ReportList *reports);

static wmOperatorStatus usd_set_attr_exec(bContext *C, wmOperator *op)
{
  char attr_name[256];
  char new_value[512];
  RNA_string_get(op->ptr, "attr_name", attr_name);
  RNA_string_get(op->ptr, "new_value", new_value);

  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;
  if (suss->active_prim_path[0] == '\0')
    return OPERATOR_CANCELLED;

  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(
      pxr::SdfPath(suss->active_prim_path));
  if (!prim)
    return OPERATOR_CANCELLED;

  if (!usd_write_attr_value(prim, attr_name, new_value, op->reports))
    return OPERATOR_CANCELLED;

  suss->runtime->refresh_prim_detail(suss->active_prim_path);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_set_attr_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* Pre-populate from context strings written by the panel button. */
  if (auto name = CTX_data_string_get(C, "usd_attr_name")) {
    RNA_string_set(op->ptr, "attr_name", std::string(*name).c_str());
  }
  if (auto cur = CTX_data_string_get(C, "usd_attr_cur_val")) {
    RNA_string_set(op->ptr, "new_value", std::string(*cur).c_str());
  }
  return WM_operator_props_popup(C, op, event);
}

static void USD_STAGE_OT_set_attr(wmOperatorType *ot)
{
  ot->name = "Set Attribute Value";
  ot->idname = "USD_STAGE_OT_set_attr";
  ot->description = "Edit the value of a USD prim attribute";
  ot->exec = usd_set_attr_exec;
  ot->invoke = usd_set_attr_invoke;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "attr_name", "", 256, "Attribute", "Name of the USD attribute");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_string(ot->srna, "new_value", "", 512, "Value", "New value for the attribute");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Attribute operator (populates edit slot, no undo)
 * \{ */

static wmOperatorStatus usd_select_attr_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss)
    return OPERATOR_CANCELLED;

  if (auto name = CTX_data_string_get(C, "usd_attr_name")) {
    BLI_strncpy(suss->edit_attr_name, std::string(*name).c_str(), sizeof(suss->edit_attr_name));
  }
  if (auto val = CTX_data_string_get(C, "usd_attr_cur_val")) {
    BLI_strncpy(suss->edit_attr_value, std::string(*val).c_str(), sizeof(suss->edit_attr_value));
  }
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_select_attr(wmOperatorType *ot)
{
  ot->name = "Select Attribute for Editing";
  ot->idname = "USD_STAGE_OT_select_attr";
  ot->description = "Stage a USD prim attribute for inline editing";
  ot->exec = usd_select_attr_exec;
  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply Attribute Edit operator (writes edit_attr_value to USD)
 * \{ */

static bool usd_write_attr_value(pxr::UsdPrim prim,
                                 const char *attr_name,
                                 const char *new_value,
                                 ReportList *reports)
{
  pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(attr_name));
  if (!attr)
    return false;

  pxr::VtValue cur;
  if (!attr.Get(&cur, pxr::UsdTimeCode::Default()))
    return false;

  try {
    if (cur.IsHolding<float>()) {
      attr.Set(std::stof(new_value));
    }
    else if (cur.IsHolding<double>()) {
      attr.Set(std::stod(new_value));
    }
    else if (cur.IsHolding<int>()) {
      attr.Set(std::stoi(new_value));
    }
    else if (cur.IsHolding<bool>()) {
      std::string sv(new_value);
      attr.Set(bool(sv == "1" || sv == "true" || sv == "True"));
    }
    else if (cur.IsHolding<std::string>()) {
      attr.Set(std::string(new_value));
    }
    else if (cur.IsHolding<pxr::TfToken>()) {
      attr.Set(pxr::TfToken(new_value));
    }
    else if (cur.IsHolding<pxr::GfVec3f>()) {
      float x, y, z;
      if (sscanf(new_value, "%f %f %f", &x, &y, &z) == 3)
        attr.Set(pxr::GfVec3f(x, y, z));
    }
    else if (cur.IsHolding<pxr::GfVec3d>()) {
      double x, y, z;
      if (sscanf(new_value, "%lf %lf %lf", &x, &y, &z) == 3)
        attr.Set(pxr::GfVec3d(x, y, z));
    }
  }
  catch (...) {
    if (reports)
      BKE_report(reports, RPT_ERROR, "Failed to parse value for attribute type");
    return false;
  }
  return true;
}

static wmOperatorStatus usd_apply_attr_edit_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;
  if (suss->edit_attr_name[0] == '\0' || suss->active_prim_path[0] == '\0')
    return OPERATOR_CANCELLED;

  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(
      pxr::SdfPath(suss->active_prim_path));
  if (!prim)
    return OPERATOR_CANCELLED;

  if (!usd_write_attr_value(prim, suss->edit_attr_name, suss->edit_attr_value, op->reports))
    return OPERATOR_CANCELLED;

  suss->runtime->refresh_prim_detail(suss->active_prim_path);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_apply_attr_edit(wmOperatorType *ot)
{
  ot->name = "Apply Attribute Edit";
  ot->idname = "USD_STAGE_OT_apply_attr_edit";
  ot->description = "Write the edited attribute value back to the USD stage";
  ot->exec = usd_apply_attr_edit_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Save / Reload Layer operators
 * \{ */

static wmOperatorStatus usd_save_layer_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  /* Try RNA property first, then fall back to context string. */
  char id_buf[1024] = {};
  RNA_string_get(op->ptr, "layer_id", id_buf);
  if (id_buf[0] == '\0') {
    if (auto ctx = CTX_data_string_get(C, "usd_layer_id")) {
      BLI_strncpy(id_buf, std::string(*ctx).c_str(), sizeof(id_buf));
    }
  }
  if (id_buf[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No layer ID provided");
    return OPERATOR_CANCELLED;
  }

  for (const pxr::SdfLayerHandle &layer : suss->runtime->stage->GetLayerStack(true)) {
    if (layer->GetIdentifier() == id_buf) {
      if (!layer->Save()) {
        BKE_reportf(op->reports, RPT_ERROR, "Failed to save layer: %s", id_buf);
        return OPERATOR_CANCELLED;
      }
      suss->runtime->refresh_layers();
      WM_event_add_notifier(C, NC_SPACE, nullptr);
      BKE_reportf(op->reports, RPT_INFO, "Saved: %s", layer->GetDisplayName().c_str());
      return OPERATOR_FINISHED;
    }
  }
  BKE_reportf(op->reports, RPT_ERROR, "Layer not found: %s", id_buf);
  return OPERATOR_CANCELLED;
}

static void USD_STAGE_OT_save_layer(wmOperatorType *ot)
{
  ot->name = "Save Layer";
  ot->idname = "USD_STAGE_OT_save_layer";
  ot->description = "Save this USD layer to disk";
  ot->exec = usd_save_layer_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", "", 1024, "Layer ID", "Identifier of the layer to save");
}

static wmOperatorStatus usd_reload_layer_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  /* Try RNA property first, then fall back to context string. */
  char id_buf[1024] = {};
  RNA_string_get(op->ptr, "layer_id", id_buf);
  if (id_buf[0] == '\0') {
    if (auto ctx = CTX_data_string_get(C, "usd_layer_id")) {
      BLI_strncpy(id_buf, std::string(*ctx).c_str(), sizeof(id_buf));
    }
  }
  if (id_buf[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No layer ID provided");
    return OPERATOR_CANCELLED;
  }

  for (const pxr::SdfLayerHandle &layer : suss->runtime->stage->GetLayerStack(true)) {
    if (layer->GetIdentifier() == id_buf) {
      /* force=true: always read from disk regardless of timestamp. */
      if (!layer->Reload(true)) {
        BKE_reportf(op->reports, RPT_ERROR, "Failed to reload layer: %s", id_buf);
        return OPERATOR_CANCELLED;
      }
      suss->runtime->repull_all_objects();
      suss->runtime->refresh_layers();
      DEG_relations_tag_update(CTX_data_main(C));
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);
      WM_event_add_notifier(C, NC_SPACE, nullptr);
      BKE_reportf(op->reports, RPT_INFO, "Reloaded: %s", layer->GetDisplayName().c_str());
      return OPERATOR_FINISHED;
    }
  }
  BKE_reportf(op->reports, RPT_ERROR, "Layer not found in stack: %s", id_buf);
  return OPERATOR_CANCELLED;
}

static void USD_STAGE_OT_reload_layer(wmOperatorType *ot)
{
  ot->name = "Reload Layer";
  ot->idname = "USD_STAGE_OT_reload_layer";
  ot->description = "Reload this USD layer from disk and pull changes into Blender";
  ot->exec = usd_reload_layer_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", "", 1024, "Layer ID", "Identifier of the layer to reload");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Area listener — push Blender transforms into the USD stage live
 * \{ */

static void usd_stage_area_listener(const wmSpaceTypeListenerParams *params)
{
  ScrArea *area = params->area;
  const wmNotifier *wmn = params->notifier;

  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(area->spacedata.first);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return;

  switch (wmn->category) {
    case NC_OBJECT:
      if (wmn->data == ND_TRANSFORM) {
        /* Object moved/rotated/scaled in object mode, OR vertices transformed in edit mode.
         * Both dispatch NC_OBJECT|ND_TRANSFORM from SPACE_VIEW3D (transform.cc viewRedrawForce).
         * Push xforms always; push meshes when any tracked object is in Edit Mode. */
        suss->runtime->push_all_xforms();
        suss->runtime->push_all_meshes();
        if (suss->active_prim_path[0] != '\0') {
          suss->runtime->refresh_prim_detail(suss->active_prim_path);
        }
        ED_area_tag_redraw(area);
      }
      break;

    case NC_GEOM:
      if (wmn->data == ND_DATA || wmn->data == ND_VERTEX_GROUP) {
        /* Edit-mode vertex/topology edits — push mesh data. */
        suss->runtime->push_all_meshes();
        if (suss->active_prim_path[0] != '\0') {
          suss->runtime->refresh_prim_detail(suss->active_prim_path);
        }
        ED_area_tag_redraw(area);
      }
      break;

    default:
      break;
  }
}

/** \} */

static void usd_stage_file_menu_draw(const bContext * /*C*/, Menu *menu)
{
  ui::Layout &layout = *menu->layout;
  layout.op("USD_STAGE_OT_open", "Open...", ICON_FILE_FOLDER);
}

static void usd_stage_operatortypes()
{
  WM_operatortype_append(USD_STAGE_OT_open);
  WM_operatortype_append(USD_STAGE_OT_set_attr);
  WM_operatortype_append(USD_STAGE_OT_select_attr);
  WM_operatortype_append(USD_STAGE_OT_apply_attr_edit);
  WM_operatortype_append(USD_STAGE_OT_save_layer);
  WM_operatortype_append(USD_STAGE_OT_reload_layer);

  MenuType *mt = MEM_new_zeroed<MenuType>("usd stage file menu");
  STRNCPY(mt->idname, "USD_STAGE_MT_file");
  STRNCPY(mt->label, "File");
  mt->draw = usd_stage_file_menu_draw;
  WM_menutype_add(mt);
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

  layout.menu("USD_STAGE_MT_file", std::nullopt, ICON_NONE);

  layout.separator();

  const char *label = (suss && suss->filepath[0] != '\0') ? suss->filepath : "No stage open";
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
  ED_region_panels_init(wm, region);
}

static void usd_stage_main_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels(C, region);
}

static void usd_stage_main_region_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_SPACE:
      ED_region_tag_redraw(params->region);
      break;
  }
}

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
  ED_region_panels(C, region);
}

static void usd_stage_ui_region_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_SPACE:
      ED_region_tag_redraw(params->region);
      break;
  }
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
  st->listener = usd_stage_area_listener;

  /* Main window region — prim tree. */
  ARegionType *art = MEM_new<ARegionType>("spacetype usd stage main region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;
  art->init = usd_stage_main_region_init;
  art->draw = usd_stage_main_region_draw;
  art->listener = usd_stage_main_region_listener;
  BLI_addhead(&st->regiontypes, art);

  usd_stage_prim_tree_panel_register(art);

  /* UI region — Properties / Variants / Layers panels. */
  art = MEM_new<ARegionType>("spacetype usd stage ui region");
  art->regionid = RGN_TYPE_UI;
  art->prefsizex = UI_SIDEBAR_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_UI;
  art->init = usd_stage_ui_region_init;
  art->draw = usd_stage_ui_region_draw;
  art->listener = usd_stage_ui_region_listener;
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
