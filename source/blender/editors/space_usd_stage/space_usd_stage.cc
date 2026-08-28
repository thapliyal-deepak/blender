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

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_tempfile.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "IMB_colormanagement.hh"

#include "BLI_ustring.hh"

#include "DNA_node_types.h"

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

#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdUtils/flattenLayerStack.h>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/reference.h>

#include <algorithm>
#include <optional>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <stdexcept>

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DNA_windowmanager_types.h"

#include "space_usd_stage.hh"
#include "usd_prim_tree_view.hh"
#include "usd_stage_panels.hh"
#include "usd_stage_runtime.hh"

#include "usd_reader_material.hh"
#include "usd_writer_material.hh"
#include "usd.hh"

#include "DNA_scene_types.h"

#include "DNA_object_types.h"

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
  suss->edit_attr_prim_path[0] = '\0';

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

  if (rt->is_open())
    return;

  if (suss->filepath[0] != '\0') {
    /* Re-open stage from the saved filepath (e.g. after blend reload). */
    rt->open(suss->filepath);
  }

  if (!rt->is_open()) {
    /* No filepath, or the file is gone — create a real empty stage at tmp/untitled.usd.
     * Using a file-backed stage means reloads always find the file without asking. */
    char tmpdir[FILE_MAX];
    BLI_temp_directory_path_get(tmpdir, sizeof(tmpdir));
    BLI_path_join(suss->filepath, sizeof(suss->filepath), tmpdir, "untitled.usd");
    rt->create_new(suss->filepath);
  }

  /* Tag every region so the prim tree draws /World immediately on first open
   * rather than briefly flashing "No stage open". */
  for (ARegion *region = static_cast<ARegion *>(area->regionbase.first); region;
       region = static_cast<ARegion *>(region->next))
  {
    ED_region_tag_redraw(region);
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

/**
 * Find any SpaceUsdStage that currently has a stage open.
 * Searches only the ACTIVE space of each area (spacedata.first) across ALL windows,
 * so it works regardless of whether the Stage Editor is docked or floating,
 * and never returns a stale history entry with a null runtime.
 */
static SpaceUsdStage *usd_stage_find_open(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm)
    return nullptr;
  for (wmWindow *win = static_cast<wmWindow *>(wm->windows.first); win;
       win = static_cast<wmWindow *>(win->next))
  {
    bScreen *screen = WM_window_get_active_screen(win);
    if (!screen)
      continue;
    for (ScrArea *area = static_cast<ScrArea *>(screen->areabase.first); area;
         area = static_cast<ScrArea *>(area->next))
    {
      SpaceLink *sl = static_cast<SpaceLink *>(area->spacedata.first);
      if (sl && sl->spacetype == SPACE_USD_STAGE) {
        SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(sl);
        if (suss->runtime && suss->runtime->is_open())
          return suss;
      }
    }
  }
  return nullptr;
}

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
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
    /* Tell all render engines to recompile GPU shaders for the newly imported materials. */
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);

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
  prop = RNA_def_string(ot->srna, "attr_name", nullptr, 256, "Attribute", "Name of the USD attribute");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_string(ot->srna, "new_value", nullptr, 512, "Value", "New value for the attribute");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Attribute operator (populates edit slot, no undo)
 * \{ */

/** Populate per-component float fields from a VtValue for numeric inline editing.
 *  Sets edit_attr_num_components=0 for non-numeric types (text-edit mode stays). */
static void populate_edit_components(SpaceUsdStage *suss, const pxr::VtValue &val)
{
  suss->edit_attr_num_components = 0;
  suss->edit_attr_cols = 0;

  auto set1 = [&](float v) {
    suss->edit_attr_row0[0] = v;
    suss->edit_attr_num_components = 1;
    suss->edit_attr_cols = 1;
  };

  if (val.IsHolding<float>())
    set1(val.UncheckedGet<float>());
  else if (val.IsHolding<double>())
    set1((float)val.UncheckedGet<double>());
  else if (val.IsHolding<int>())
    set1((float)val.UncheckedGet<int>());
  else if (val.IsHolding<bool>())
    set1(val.UncheckedGet<bool>() ? 1.0f : 0.0f);
  else if (val.IsHolding<pxr::GfVec2f>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec2f>();
    suss->edit_attr_row0[0] = v[0]; suss->edit_attr_row0[1] = v[1];
    suss->edit_attr_num_components = 2; suss->edit_attr_cols = 2;
  }
  else if (val.IsHolding<pxr::GfVec2d>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec2d>();
    suss->edit_attr_row0[0] = (float)v[0]; suss->edit_attr_row0[1] = (float)v[1];
    suss->edit_attr_num_components = 2; suss->edit_attr_cols = 2;
  }
  else if (val.IsHolding<pxr::GfVec3f>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec3f>();
    suss->edit_attr_row0[0] = v[0]; suss->edit_attr_row0[1] = v[1]; suss->edit_attr_row0[2] = v[2];
    suss->edit_attr_num_components = 3; suss->edit_attr_cols = 3;
  }
  else if (val.IsHolding<pxr::GfVec3d>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec3d>();
    suss->edit_attr_row0[0] = (float)v[0]; suss->edit_attr_row0[1] = (float)v[1]; suss->edit_attr_row0[2] = (float)v[2];
    suss->edit_attr_num_components = 3; suss->edit_attr_cols = 3;
  }
  else if (val.IsHolding<pxr::GfVec4f>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec4f>();
    suss->edit_attr_row0[0] = v[0]; suss->edit_attr_row0[1] = v[1];
    suss->edit_attr_row0[2] = v[2]; suss->edit_attr_row0[3] = v[3];
    suss->edit_attr_num_components = 4; suss->edit_attr_cols = 4;
  }
  else if (val.IsHolding<pxr::GfVec4d>()) {
    const auto &v = val.UncheckedGet<pxr::GfVec4d>();
    suss->edit_attr_row0[0] = (float)v[0]; suss->edit_attr_row0[1] = (float)v[1];
    suss->edit_attr_row0[2] = (float)v[2]; suss->edit_attr_row0[3] = (float)v[3];
    suss->edit_attr_num_components = 4; suss->edit_attr_cols = 4;
  }
  else if (val.IsHolding<pxr::GfMatrix4d>()) {
    const auto &m = val.UncheckedGet<pxr::GfMatrix4d>();
    float *rows[4] = {suss->edit_attr_row0, suss->edit_attr_row1,
                      suss->edit_attr_row2, suss->edit_attr_row3};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        rows[i][j] = (float)m[i][j];
    suss->edit_attr_num_components = 16; suss->edit_attr_cols = 4;
  }
  /* string / TfToken / other: leave num_components=0 → text-edit mode */
}

/** Write per-component float fields back to a USD attribute. */
static bool usd_write_attr_components(pxr::UsdPrim prim,
                                      const char *attr_name,
                                      const SpaceUsdStage *suss,
                                      ReportList *reports)
{
  pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(attr_name));
  if (!attr) return false;

  pxr::VtValue cur;
  if (!attr.Get(&cur, pxr::UsdTimeCode::Default())) return false;

  const float *r0 = suss->edit_attr_row0;
  const float *r1 = suss->edit_attr_row1;
  const float *r2 = suss->edit_attr_row2;
  const float *r3 = suss->edit_attr_row3;

  try {
    if (cur.IsHolding<float>())
      attr.Set(r0[0]);
    else if (cur.IsHolding<double>())
      attr.Set((double)r0[0]);
    else if (cur.IsHolding<int>())
      attr.Set((int)r0[0]);
    else if (cur.IsHolding<bool>())
      attr.Set(r0[0] != 0.0f);
    else if (cur.IsHolding<pxr::GfVec2f>())
      attr.Set(pxr::GfVec2f(r0[0], r0[1]));
    else if (cur.IsHolding<pxr::GfVec2d>())
      attr.Set(pxr::GfVec2d(r0[0], r0[1]));
    else if (cur.IsHolding<pxr::GfVec3f>())
      attr.Set(pxr::GfVec3f(r0[0], r0[1], r0[2]));
    else if (cur.IsHolding<pxr::GfVec3d>())
      attr.Set(pxr::GfVec3d(r0[0], r0[1], r0[2]));
    else if (cur.IsHolding<pxr::GfVec4f>())
      attr.Set(pxr::GfVec4f(r0[0], r0[1], r0[2], r0[3]));
    else if (cur.IsHolding<pxr::GfVec4d>())
      attr.Set(pxr::GfVec4d(r0[0], r0[1], r0[2], r0[3]));
    else if (cur.IsHolding<pxr::GfMatrix4d>()) {
      pxr::GfMatrix4d m;
      const float *rows[4] = {r0, r1, r2, r3};
      for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
          m[i][j] = rows[i][j];
      attr.Set(m);
    }
    else {
      if (reports)
        BKE_report(reports, RPT_WARNING, "Attribute type not supported for component edit");
      return false;
    }
  }
  catch (...) {
    if (reports)
      BKE_report(reports, RPT_ERROR, "Failed to write attribute components");
    return false;
  }
  return true;
}

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
  /* When editing a shader input the prim that owns the attr may differ from the selected prim. */
  if (auto ppath = CTX_data_string_get(C, "usd_shader_prim_path")) {
    BLI_strncpy(
        suss->edit_attr_prim_path, std::string(*ppath).c_str(), sizeof(suss->edit_attr_prim_path));
  }
  else {
    BLI_strncpy(
        suss->edit_attr_prim_path, suss->active_prim_path, sizeof(suss->edit_attr_prim_path));
  }

  /* Populate per-component fields for numeric types. */
  suss->edit_attr_num_components = 0;
  if (suss->runtime && suss->runtime->is_open() && suss->edit_attr_name[0] != '\0') {
    const char *prim_path = (suss->edit_attr_prim_path[0] != '\0') ? suss->edit_attr_prim_path :
                                                                      suss->active_prim_path;
    pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(pxr::SdfPath(prim_path));
    if (prim) {
      pxr::UsdAttribute attr = prim.GetAttribute(pxr::TfToken(suss->edit_attr_name));
      if (attr) {
        pxr::VtValue v;
        attr.Get(&v, pxr::UsdTimeCode::Default());
        populate_edit_components(suss, v);
      }
    }
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
      /* TfStringify produces "(x, y, z)"; also accept "x y z" and "x, y, z". */
      if (sscanf(new_value, "(%f, %f, %f)", &x, &y, &z) == 3 ||
          sscanf(new_value, "%f, %f, %f", &x, &y, &z) == 3 ||
          sscanf(new_value, "%f %f %f", &x, &y, &z) == 3)
      {
        attr.Set(pxr::GfVec3f(x, y, z));
      }
    }
    else if (cur.IsHolding<pxr::GfVec3d>()) {
      double x, y, z;
      if (sscanf(new_value, "(%lf, %lf, %lf)", &x, &y, &z) == 3 ||
          sscanf(new_value, "%lf, %lf, %lf", &x, &y, &z) == 3 ||
          sscanf(new_value, "%lf %lf %lf", &x, &y, &z) == 3)
      {
        attr.Set(pxr::GfVec3d(x, y, z));
      }
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

  /* Prefer edit_attr_prim_path (set when editing a shader input child prim). */
  const char *target_path = (suss->edit_attr_prim_path[0] != '\0') ? suss->edit_attr_prim_path :
                                                                       suss->active_prim_path;
  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(pxr::SdfPath(target_path));
  if (!prim)
    return OPERATOR_CANCELLED;

  bool ok = (suss->edit_attr_num_components > 0) ?
                usd_write_attr_components(prim, suss->edit_attr_name, suss, op->reports) :
                usd_write_attr_value(prim, suss->edit_attr_name, suss->edit_attr_value, op->reports);
  if (!ok)
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

  /* Use SdfLayer::Find rather than GetLayerStack so muted layers (excluded from the
   * PcpLayerStack) are still reachable. */
  pxr::SdfLayerHandle layer = pxr::SdfLayer::Find(id_buf);
  if (!layer) {
    /* Fall back to the strong-ref list for recently-added file layers. */
    for (const pxr::SdfLayerRefPtr &ref : suss->runtime->anon_layer_refs) {
      if (ref && ref->GetIdentifier() == id_buf) {
        layer = ref;
        break;
      }
    }
  }
  if (!layer) {
    BKE_reportf(op->reports, RPT_ERROR, "Layer not found: %s", id_buf);
    return OPERATOR_CANCELLED;
  }
  if (!layer->Save()) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to save layer: %s", id_buf);
    return OPERATOR_CANCELLED;
  }
  suss->runtime->refresh_layers();
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Saved: %s", layer->GetDisplayName().c_str());
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_save_layer(wmOperatorType *ot)
{
  ot->name = "Save Layer";
  ot->idname = "USD_STAGE_OT_save_layer";
  ot->description = "Save this USD layer to disk";
  ot->exec = usd_save_layer_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer to save");
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

  /* Use SdfLayer::Find so muted layers (excluded from the PcpLayerStack returned by
   * GetLayerStack) are still reachable. */
  pxr::SdfLayerHandle reload_layer = pxr::SdfLayer::Find(id_buf);
  if (!reload_layer) {
    for (const pxr::SdfLayerRefPtr &ref : suss->runtime->anon_layer_refs) {
      if (ref && ref->GetIdentifier() == id_buf) {
        reload_layer = ref;
        break;
      }
    }
  }
  if (!reload_layer) {
    BKE_reportf(op->reports, RPT_ERROR, "Layer not found: %s", id_buf);
    return OPERATOR_CANCELLED;
  }
  if (!reload_layer->Reload(true)) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to reload layer: %s", id_buf);
    return OPERATOR_CANCELLED;
  }
  suss->runtime->repull_all_objects();
  suss->runtime->refresh_layers();
  DEG_relations_tag_update(CTX_data_main(C));
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Reloaded: %s", reload_layer->GetDisplayName().c_str());
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_reload_layer(wmOperatorType *ot)
{
  ot->name = "Reload Layer";
  ot->idname = "USD_STAGE_OT_reload_layer";
  ot->description = "Reload this USD layer from disk and pull changes into Blender";
  ot->exec = usd_reload_layer_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer to reload");
}

/* -------------------------------------------------------------------- */
/** \name Set Edit Target operator
 * \{ */

static wmOperatorStatus usd_set_edit_target_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char layer_id[1024] = {};
  RNA_string_get(op->ptr, "layer_id", layer_id);
  if (layer_id[0] == '\0')
    return OPERATOR_CANCELLED;

  /* Flush pending edit-mode mesh edits only — NOT xforms. Object transforms are written live
   * by the area listener (NC_OBJECT|ND_TRANSFORM) and use BKE_object_to_mat4 which is always
   * current. Flushing xforms here with a potentially-stale ob->runtime->object_to_world used
   * to corrupt the new target by writing wrong positions to it. */
  suss->runtime->push_all_meshes();

  /* Use SdfLayer::Find so muted layers (excluded from GetLayerStack) can also be set as
   * the edit target via the context menu. */
  pxr::SdfLayerHandle target = pxr::SdfLayer::Find(layer_id);
  if (!target) {
    for (const pxr::SdfLayerRefPtr &ref : suss->runtime->anon_layer_refs) {
      if (ref && ref->GetIdentifier() == layer_id) {
        target = ref;
        break;
      }
    }
  }
  if (target) {
    suss->runtime->stage->SetEditTarget(pxr::UsdEditTarget(target));
    suss->runtime->refresh_layers();
    WM_event_add_notifier(C, NC_SPACE, nullptr);
    return OPERATOR_FINISHED;
  }

  BKE_reportf(op->reports, RPT_WARNING, "Layer not found: %s", layer_id);
  return OPERATOR_CANCELLED;
}

static void USD_STAGE_OT_set_edit_target(wmOperatorType *ot)
{
  ot->name = "Set Edit Target";
  ot->idname = "USD_STAGE_OT_set_edit_target";
  ot->description = "Make this layer the active edit target for new USD opinions";
  ot->exec = usd_set_edit_target_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Toggle Layer Mute operator
 * \{ */

/**
 * Move the edit target back to the root layer when `layer_id` is the layer being edited.
 *
 * A stage's edit target has to be a layer of its layer stack. Taking the target out of that
 * stack — removing the sublayer, or muting it — leaves the stage unable to make a prim spec to
 * author into, and the next transform push then walks into a null dereference inside USD, which
 * aborts the process rather than reporting. Returns true when the target was moved, so the
 * caller can say so.
 */
static bool usd_edit_target_back_to_root(SpaceUsdStage *suss, const std::string &layer_id)
{
  const pxr::UsdStageRefPtr &stage = suss->runtime->stage;
  const pxr::SdfLayerHandle target = stage->GetEditTarget().GetLayer();
  if (!target || target->GetIdentifier() != layer_id) {
    return false;
  }
  const pxr::SdfLayerHandle root = stage->GetRootLayer();
  if (!root) {
    return false;
  }
  stage->SetEditTarget(pxr::UsdEditTarget(root));
  return true;
}

static wmOperatorStatus usd_toggle_layer_mute_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char layer_id[1024] = {};
  RNA_string_get(op->ptr, "layer_id", layer_id);
  if (layer_id[0] == '\0')
    return OPERATOR_CANCELLED;

  const std::string layer_str(layer_id);

  /* Pcp refuses to mute the layer a stage is rooted at (PcpCache::RequestLayerMuting emits
   * "Cannot mute cache's root layer"), so the toggle would raise a coding error and then report
   * success while nothing changed. The Layers panel greys the eye out for the root layer; this
   * guard covers the operator being run from anywhere else. */
  if (layer_str == suss->runtime->stage->GetRootLayer()->GetIdentifier()) {
    BKE_report(op->reports,
               RPT_ERROR,
               "The root layer cannot be muted, it defines the stage. Mute a sublayer instead");
    return OPERATOR_CANCELLED;
  }

  const std::vector<std::string> &muted = suss->runtime->stage->GetMutedLayers();
  const bool currently_muted = std::find(muted.begin(), muted.end(), layer_str) != muted.end();

  printf("[USD] toggle_mute: layer='%s' currently_muted=%d muted_count=%d\n",
         layer_str.c_str(), (int)currently_muted, (int)muted.size());

  /* Muting the layer being edited would strand the edit target outside the layer stack. */
  const bool moved_target = !currently_muted && usd_edit_target_back_to_root(suss, layer_str);

  if (!currently_muted) {
    /* MUTING: flush edit-mode mesh edits only. Xform push is intentionally skipped — with the
     * BKE_object_to_mat4 fix, sync_xform_differs now reads ob->loc directly (always current), so
     * a push here is always a no-op anyway and was the historical source of root-layer corruption
     * when the edit target had drifted to root. */
    suss->runtime->push_all_meshes();
    suss->runtime->stage->MuteLayer(layer_str);
  }
  else {
    suss->runtime->stage->UnmuteLayer(layer_str);
  }

  if (moved_target) {
    BKE_report(op->reports,
               RPT_INFO,
               "Muted the layer being edited, so the edit target moved to the root layer");
  }

  suss->runtime->refresh_layers();
  suss->runtime->repull_all_objects();

  /* Force synchronous DEG evaluation so 3D viewports see the correct ob_eval->object_to_world
   * before they redraw. repull_all_objects() sets ob->loc via BKE_object_apply_mat4 and tags
   * ID_RECALC_ALL, but the depsgraph is lazy — without this call the viewport still draws using
   * the stale eval copies even though ob->loc is already correct. */
  CTX_data_ensure_evaluated_depsgraph(C);

  /* Diagnostic: print translation of first tracked prim after repull. */
  for (const auto &[path, ob] : suss->runtime->obj_map) {
    if (!ob) continue;
    pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(pxr::SdfPath(path));
    if (prim) {
      pxr::GfMatrix4d m = pxr::UsdGeomImageable(prim).ComputeLocalToWorldTransform(
          pxr::UsdTimeCode::Default());
      printf("[USD] after repull: prim='%s' USD_tx=%.4f ob_loc=(%.4f,%.4f,%.4f)\n",
             path.c_str(), m[3][0], ob->loc[0], ob->loc[1], ob->loc[2]);
    }
    break;  /* first prim only */
  }

  /* Use ND_TRANSFORM (not ND_DRAW) so the 3D viewport triggers a proper DEG transform
   * re-evaluation pass. ND_DRAW only marks display properties dirty, which may not force
   * object transform re-evaluation. Removing DEG_relations_tag_update avoids discarding the
   * ID_RECALC_ALL dirty flags that repull_all_objects set via DEG_id_tag_update. */
  WM_main_add_notifier(NC_OBJECT | ND_TRANSFORM, nullptr);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_toggle_layer_mute(wmOperatorType *ot)
{
  ot->name = "Toggle Layer Mute";
  ot->idname = "USD_STAGE_OT_toggle_layer_mute";
  ot->description = "Toggle whether this layer contributes to the composed scene";
  ot->exec = usd_toggle_layer_mute_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Save Layer As operator — give an anonymous layer a file path
 * \{ */

static wmOperatorStatus usd_save_layer_as_exec(bContext *C, wmOperator *op)
{
  ScrArea *hint_area = static_cast<ScrArea *>(op->customdata);
  op->customdata = nullptr;
  SpaceUsdStage *suss = usd_stage_find_or_open(C, hint_area, nullptr);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char filepath[FILE_MAX] = {};
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No file path provided");
    return OPERATOR_CANCELLED;
  }

  char layer_id[1024] = {};
  RNA_string_get(op->ptr, "layer_id", layer_id);
  if (layer_id[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No layer ID provided");
    return OPERATOR_CANCELLED;
  }

  /* Find the layer by identifier — SdfLayer::Find searches the global in-memory registry,
   * which includes anonymous layers that are not on disk. */
  pxr::SdfLayerRefPtr target_layer = pxr::SdfLayer::Find(layer_id);
  if (!target_layer) {
    BKE_reportf(op->reports, RPT_ERROR, "Layer not found: %s", layer_id);
    return OPERATOR_CANCELLED;
  }

  /* Write all specs from the in-memory layer to the chosen file. */
  if (!target_layer->Export(filepath)) {
    BKE_reportf(op->reports, RPT_ERROR, "Export failed: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Open the freshly-written file as a proper on-disk SdfLayer. */
  pxr::SdfLayerRefPtr new_layer = pxr::SdfLayer::FindOrOpen(filepath);
  if (!new_layer) {
    BKE_reportf(op->reports, RPT_ERROR, "Could not open saved layer: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Replace the anonymous identifier in the root layer's sublayer path list so USD
   * composition now references the on-disk file instead of the in-memory blob. */
  pxr::SdfLayerHandle root = suss->runtime->stage->GetRootLayer();
  const std::string old_id = target_layer->GetIdentifier();
  std::vector<std::string> new_paths;
  for (const std::string &p : root->GetSubLayerPaths())
    new_paths.push_back((p == old_id) ? std::string(filepath) : p);
  root->SetSubLayerPaths(new_paths);

  /* Keep the edit target pointing to the same content, now on disk. */
  if (suss->runtime->stage->GetEditTarget().GetLayer() == target_layer)
    suss->runtime->stage->SetEditTarget(pxr::UsdEditTarget(new_layer));

  /* Swap the anonymous ref for the on-disk layer ref.
   * new_layer is a local SdfLayerRefPtr — if USD hasn't lazily loaded it into Pcp yet,
   * the next refresh_layers() call would find nothing and the row would vanish.
   * Keeping it in anon_layer_refs ensures SdfLayer::Find always succeeds. */
  auto &refs = suss->runtime->anon_layer_refs;
  refs.erase(std::remove(refs.begin(), refs.end(), target_layer), refs.end());
  refs.push_back(new_layer);

  suss->runtime->refresh_layers();
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Layer saved as: %s", filepath);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_save_layer_as_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath"))
    return usd_save_layer_as_exec(C, op);

  /* Stash the originating area so exec can find the SpaceUsdStage after the
   * file browser replaces the active area's context. */
  op->customdata = CTX_wm_area(C);
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void USD_STAGE_OT_save_layer_as(wmOperatorType *ot)
{
  ot->name = "Save Layer As";
  ot->idname = "USD_STAGE_OT_save_layer_as";
  ot->description =
      "Save an anonymous in-memory layer to a file, replacing its entry in the layer stack "
      "with the on-disk path";
  ot->exec = usd_save_layer_as_exec;
  ot->invoke = usd_save_layer_as_invoke;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer to save");
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_USD,
                                 FILE_SPECIAL,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material helpers
 * \{ */

/** If add_material returned an empty node tree (no UsdPreviewSurface found — typical for pure
 *  .mtlx files that only have an `mtlx` context surface), bootstrap a connected Principled BSDF
 *  → Material Output so EEVEE has a valid surface shader and doesn't render black.
 *  Returns true when nodes were added. */
static bool ensure_material_shader_nodes(Main *bmain, Material *mat)
{
  if (!mat || !mat->nodetree)
    return false;
  mat->use_nodes = true;
  /* Robust check: walk for an existing Material Output rather than relying on nodes.first. */
  for (bNode *n = static_cast<bNode *>(mat->nodetree->nodes.first); n;
       n = static_cast<bNode *>(n->next))
  {
    if (n->type_legacy == SH_NODE_OUTPUT_MATERIAL)
      return false;
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
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Import Material operator
 * \{ */

static wmOperatorStatus usd_import_material_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;
  if (suss->active_prim_path[0] == '\0')
    return OPERATOR_CANCELLED;

  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(
      pxr::SdfPath(suss->active_prim_path));
  if (!prim || !prim.IsA<pxr::UsdShadeMaterial>()) {
    BKE_report(op->reports, RPT_ERROR, "Active prim is not a UsdShadeMaterial");
    return OPERATOR_CANCELLED;
  }

  io::usd::USDImportParams params{};
  params.import_usd_preview = true;
  params.set_material_blend = true;
  params.worker_status = nullptr;

  Main *bmain = CTX_data_main(C);
  io::usd::USDMaterialReader reader(params, *bmain);

  pxr::UsdShadeMaterial usd_mat(prim);
  Material *mat = reader.add_material(usd_mat);
  if (!mat) {
    BKE_report(op->reports, RPT_ERROR, "Failed to import material");
    return OPERATOR_CANCELLED;
  }

  /* If the USD material had no UsdPreviewSurface (e.g. pure MaterialX), add a default
   * Principled BSDF so EEVEE has a valid shader instead of rendering black. */
  ensure_material_shader_nodes(bmain, mat);

  /* Assign to the Blender mesh object that is bound to this material prim (if any).
   * Walk the obj_map looking for a mesh whose bound material matches. */
  {
    const std::string mat_sdf_path = prim.GetPath().GetString();
    for (auto &[path, ob] : suss->runtime->obj_map) {
      if (!ob || ob->type != OB_MESH)
        continue;
      pxr::UsdPrim mesh_prim = suss->runtime->stage->GetPrimAtPath(pxr::SdfPath(path));
      if (!mesh_prim)
        continue;
      pxr::UsdShadeMaterialBindingAPI binding(mesh_prim);
      pxr::UsdShadeMaterial bound = binding.ComputeBoundMaterial();
      if (bound && bound.GetPrim().GetPath().GetString() == mat_sdf_path) {
        BKE_object_material_assign_single_obdata(bmain, ob, mat, 1);
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      }
    }
  }

  BKE_reportf(op->reports, RPT_INFO, "Imported material: %s", mat->id.name + 2);
  DEG_id_tag_update(&mat->id, ID_RECALC_NTREE_OUTPUT);
  if (mat->nodetree) {
    BKE_main_ensure_invariants(*bmain, mat->nodetree->id);
  }
  BKE_material_make_node_previews_dirty(mat);
  DEG_relations_tag_update(bmain);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, mat);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, mat);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_import_material(wmOperatorType *ot)
{
  ot->name = "Import Material";
  ot->idname = "USD_STAGE_OT_import_material";
  ot->description = "Import the selected USD material prim into Blender's shader node tree";
  ot->exec = usd_import_material_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bind MaterialX operator
 *
 * Reference an external MaterialX (.mtlx) document as a UsdShadeMaterial prim in the open stage
 * and bind it to the selected geometry prim (e.g. a plane).  The material can be one authored in
 * the MaterialX node editor and exported via `wm.materialx_tree_export`.  After authoring the
 * binding it also imports the material into Blender and assigns it to the bound object so the
 * result is visible in the viewport.
 * \{ */

static bool usd_bind_mtlx_material_poll(bContext *C)
{
  /* Prefer the stage in the current editor, but also allow binding when the operator is invoked
   * from another editor (e.g. the GPUOpen browser in Properties > Material) as long as some USD
   * Stage Editor has a stage open with a prim selected. */
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    suss = usd_stage_find_open(C);
  }
  return suss && suss->runtime && suss->runtime->is_open() && suss->active_prim_path[0] != '\0';
}

static wmOperatorStatus usd_bind_mtlx_material_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return op->type->exec(C, op);
  }
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus usd_bind_mtlx_material_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    suss = usd_stage_find_open(C);
  }
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    BKE_report(op->reports, RPT_ERROR, "No open USD stage");
    return OPERATOR_CANCELLED;
  }
  if (suss->active_prim_path[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Select a prim in the USD Stage Editor to bind to");
    return OPERATOR_CANCELLED;
  }

  char filepath[1024];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No MaterialX (.mtlx) file selected");
    return OPERATOR_CANCELLED;
  }

  pxr::UsdStageRefPtr stage = suss->runtime->stage;
  pxr::UsdPrim target_prim = stage->GetPrimAtPath(pxr::SdfPath(suss->active_prim_path));
  if (!target_prim) {
    BKE_report(op->reports, RPT_ERROR, "Active prim not found in stage");
    return OPERATOR_CANCELLED;
  }
  if (!target_prim.IsA<pxr::UsdGeomImageable>() || target_prim.IsA<pxr::UsdShadeMaterial>()) {
    BKE_report(op->reports, RPT_ERROR, "Active prim is not a bindable geometry prim");
    return OPERATOR_CANCELLED;
  }

  /* Open the .mtlx to find the material prim to target.  A .mtlx opened as USD usually has no
   * defaultPrim, so a whole-file reference would be unresolved; target the first
   * UsdShadeMaterial prim explicitly and fall back to a whole-file reference otherwise. */
  pxr::SdfPath ref_prim_path;
  std::string mtlx_mat_name;
  if (pxr::UsdStageRefPtr mtlx_stage = pxr::UsdStage::Open(filepath)) {
    if (pxr::UsdPrim def_prim = mtlx_stage->GetDefaultPrim()) {
      mtlx_mat_name = def_prim.GetName().GetString();
    }
    else {
      for (const pxr::UsdPrim &p : mtlx_stage->Traverse()) {
        if (p.IsA<pxr::UsdShadeMaterial>()) {
          ref_prim_path = p.GetPath();
          mtlx_mat_name = p.GetName().GetString();
          break;
        }
      }
    }
  }
  else {
    BKE_report(op->reports, RPT_ERROR, "Failed to open MaterialX file");
    return OPERATOR_CANCELLED;
  }

  if (mtlx_mat_name.empty()) {
    mtlx_mat_name = "MtlxMaterial";
  }
  const std::string mat_id = pxr::TfMakeValidIdentifier(mtlx_mat_name);

  /* The binding and material prim must be authored on a layer that gets saved.  If the current
   * edit target is an anonymous (unsaved) layer, redirect authoring to the root layer, otherwise
   * "Save Layer" would never persist it and the binding would vanish on reload. */
  const bool stage_saved = stage->GetRootLayer() &&
                           !stage->GetRootLayer()->GetRealPath().empty();
  std::optional<pxr::UsdEditContext> edit_ctx;
  {
    pxr::SdfLayerHandle edit_layer = stage->GetEditTarget().GetLayer();
    if (edit_layer && edit_layer->IsAnonymous() && stage->GetRootLayer()) {
      edit_ctx.emplace(stage, stage->GetRootLayer());
      BKE_report(op->reports,
                 RPT_WARNING,
                 "Edit target was an unsaved layer; authored the material on the root layer");
    }
  }
  if (!stage_saved) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Stage is not saved: texture/MaterialX paths stay absolute until you save it");
  }

  /* Author a Material prim under a /Materials scope and reference the .mtlx into it. */
  stage->DefinePrim(pxr::SdfPath("/Materials"), pxr::TfToken("Scope"));
  const pxr::SdfPath full_mat_path = pxr::SdfPath("/Materials").AppendChild(pxr::TfToken(mat_id));
  pxr::UsdShadeMaterial usd_mat = pxr::UsdShadeMaterial::Define(stage, full_mat_path);
  if (!usd_mat) {
    BKE_report(op->reports, RPT_ERROR, "Failed to define material prim");
    return OPERATOR_CANCELLED;
  }

  /* Avoid stacking duplicate references when re-binding the same material. */
  usd_mat.GetPrim().GetReferences().ClearReferences();
  if (ref_prim_path.IsEmpty()) {
    usd_mat.GetPrim().GetReferences().AddReference(pxr::SdfReference(filepath));
  }
  else {
    usd_mat.GetPrim().GetReferences().AddReference(pxr::SdfReference(filepath, ref_prim_path));
  }

  /* Bind the material to the selected geometry prim. */
  pxr::UsdShadeMaterialBindingAPI binding = pxr::UsdShadeMaterialBindingAPI::Apply(target_prim);
  binding.Bind(usd_mat);

  /* Import the referenced material into Blender (an approximated Principled tree).  This drives
   * two things: the viewport preview on the bound object, and — crucially for importers that do
   * NOT read MaterialX (e.g. Unreal) — authoring a UsdPreviewSurface back onto the stage material
   * prim so the saved USD carries an `outputs:surface`, not just `outputs:mtlx:surface`. */
  Main *bmain = CTX_data_main(C);
  Object *bound_ob = nullptr;
  {
    const std::string target_path_str = target_prim.GetPath().GetString();
    for (auto &[path, ob] : suss->runtime->obj_map) {
      if (ob && ob->type == OB_MESH && path == target_path_str) {
        bound_ob = ob;
        break;
      }
    }
  }

  {
    io::usd::USDImportParams params{};
    params.import_usd_preview = true;
    params.set_material_blend = true;
    params.worker_status = nullptr;
    io::usd::USDMaterialReader reader(params, *bmain);
    if (Material *mat = reader.add_material(usd_mat)) {
      ensure_material_shader_nodes(bmain, mat);
      /* Make the freshly-created node tree consistent BEFORE anything reads it. */
      if (mat->nodetree) {
        BKE_main_ensure_invariants(*bmain, mat->nodetree->id);
      }

      /* Write a UsdPreviewSurface onto the stage material prim from the imported nodes.  Copy
       * textures beside the .usd (relative paths) when the stage is saved, so it stays portable.
       * Use the non-forcing depsgraph accessor: forcing a full evaluation here (mid-operator, on
       * a just-created node tree) can crash — and the preview-surface writer does not need an
       * evaluated graph (it only needs the depsgraph to reach `Main`). */
      Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
      io::usd::author_preview_surface_from_blender_material(
          stage, full_mat_path, mat, depsgraph, stage_saved, op->reports);

      if (bound_ob) {
        BKE_object_material_assign_single_obdata(bmain, bound_ob, mat, 1);
        DEG_id_tag_update(&bound_ob->id, ID_RECALC_GEOMETRY);
      }
      DEG_id_tag_update(&mat->id, ID_RECALC_NTREE_OUTPUT);
      BKE_material_make_node_previews_dirty(mat);
      DEG_relations_tag_update(bmain);
      WM_main_add_notifier(NC_MATERIAL | ND_SHADING, mat);
    }
  }

  suss->runtime->refresh_prim_detail(suss->active_prim_path);
  suss->runtime->refresh_layers();
  WM_main_add_notifier(NC_SPACE | ND_SPACE_CHANGED, nullptr);

  BKE_reportf(op->reports,
              RPT_INFO,
              "Bound MaterialX '%s' to %s (with UsdPreviewSurface for Unreal)",
              mat_id.c_str(),
              suss->active_prim_path);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_bind_mtlx_material(wmOperatorType *ot)
{
  ot->name = "Bind MaterialX";
  ot->idname = "USD_STAGE_OT_bind_mtlx_material";
  ot->description =
      "Reference a MaterialX (.mtlx) file as a material prim and bind it to the selected "
      "geometry prim";
  ot->invoke = usd_bind_mtlx_material_invoke;
  ot->exec = usd_bind_mtlx_material_exec;
  ot->poll = usd_bind_mtlx_material_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_filesel(ot,
                                 0,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Export Object operator — push a selected Blender object into the open USD stage
 * \{ */

static bool usd_stage_export_object_poll(bContext *C)
{
  /* Enabled whenever there is an active object — exec reports a clear error if no stage is open. */
  return CTX_data_active_object(C) != nullptr;
}

static wmOperatorStatus usd_stage_export_object_exec(bContext *C, wmOperator *op)
{
  /* Search all windows for any USD Stage with a loaded stage. */
  SpaceUsdStage *suss = usd_stage_find_open(C);
  if (!suss) {
    BKE_report(op->reports, RPT_ERROR, "Open a USD file in the USD Stage Editor first");
    return OPERATOR_CANCELLED;
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    BKE_report(op->reports, RPT_ERROR, "No active object");
    return OPERATOR_CANCELLED;
  }

  const std::string path = suss->runtime->export_object(ob, CTX_data_main(C));
  if (path.empty()) {
    BKE_report(op->reports, RPT_ERROR, "Failed to export object to USD stage");
    return OPERATOR_CANCELLED;
  }

  /* Make the newly exported prim active in the stage editor. */
  BLI_strncpy(suss->active_prim_path, path.c_str(), sizeof(suss->active_prim_path));
  suss->runtime->refresh_prim_detail(path.c_str());
  suss->runtime->refresh_layers();

  BKE_reportf(op->reports, RPT_INFO, "Exported '%s' -> %s", ob->id.name + 2, path.c_str());
  /* Broadcast to all windows: the export operator may run from the 3D viewport context while
   * the USD Stage Editor is in a different area, so WM_event_add_notifier alone (which only
   * covers the current window) would leave the prim tree stale. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_CHANGED, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_export_object(wmOperatorType *ot)
{
  ot->name = "Export Object to USD";
  ot->idname = "USD_STAGE_OT_export_object";
  ot->description =
      "Export the active Blender object to the open USD stage. "
      "Creates a new prim if not yet tracked; otherwise pushes the latest mesh and transform";
  ot->poll = usd_stage_export_object_poll;
  ot->exec = usd_stage_export_object_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
        /* Object moved/rotated/scaled in object mode, AND vertex transforms in edit mode
         * (transform.cc viewRedrawForce always fires NC_OBJECT|ND_TRANSFORM for SPACE_VIEW3D).
         * push_all_meshes() skips objects not in edit mode, so this is safe. */
        suss->runtime->push_all_meshes();
        suss->runtime->push_all_xforms();
        if (suss->active_prim_path[0] != '\0') {
          suss->runtime->refresh_prim_detail(suss->active_prim_path);
        }
        ED_area_tag_redraw(area);
      }
      break;

    case NC_GEOM:
      if (wmn->data == ND_DATA || wmn->data == ND_VERTEX_GROUP) {
        /* Topology-changing ops (extrude, merge, loop-cut…) fire NC_GEOM|ND_DATA.
         * Vertex position transforms fire NC_OBJECT|ND_TRANSFORM (handled above). */
        suss->runtime->push_all_meshes();
        if (suss->active_prim_path[0] != '\0') {
          suss->runtime->refresh_prim_detail(suss->active_prim_path);
        }
        ED_area_tag_redraw(area);
      }
      break;

    case NC_MATERIAL:
      /* Material shader values changed — push Principled BSDF sockets to USD. */
      suss->runtime->push_all_materials();
      ED_area_tag_redraw(area);
      break;

    case NC_LAMP:
      /* Light property changed (energy, colour, etc.) — push data to USD. */
      suss->runtime->push_all_light_data();
      ED_area_tag_redraw(area);
      break;

    case NC_SCENE:
      /* Objects may have been added or deleted — validate obj_map to remove
       * any entries whose Blender object no longer exists and delete their prims. */
      suss->runtime->validate_obj_map();
      ED_area_tag_redraw(area);
      break;

    default:
      break;
  }
}

/** \} */

static void usd_stage_file_menu_draw(const bContext * /*C*/, Menu *menu)
{
  ui::Layout &layout = *menu->layout;
  layout.op("USD_STAGE_OT_new_stage", "New Stage", ICON_FILE_NEW);
  layout.op("USD_STAGE_OT_open", "Open...", ICON_FILE_FOLDER);
  layout.separator(1.0f, ui::LayoutSeparatorType::Line);
  layout.op("USD_STAGE_OT_save_stage", "Save", ICON_FILE_TICK);
  layout.op("USD_STAGE_OT_save_stage_as", "Save As...", ICON_NONE);
  layout.separator(1.0f, ui::LayoutSeparatorType::Line);
  layout.op("USD_STAGE_OT_export_scene", "Export Scene to USD Stage...", ICON_EXPORT);
  layout.op("USD_STAGE_OT_export_object", "Export Active Object", ICON_OBJECT_DATA);
  layout.op("USD_STAGE_OT_push_skel_pose", "Push Skeleton Pose", ICON_ARMATURE_DATA);
}

/* -------------------------------------------------------------------- */
/** \name New Layer operator — create anonymous sublayer and set as edit target
 * \{ */

static wmOperatorStatus usd_new_layer_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  pxr::SdfLayerRefPtr new_layer = pxr::SdfLayer::CreateAnonymous("edit");
  if (!new_layer) {
    BKE_report(op->reports, RPT_ERROR, "Failed to create anonymous layer");
    return OPERATOR_CANCELLED;
  }

  /* Insert at position 0 so it has the strongest opinion in the layer stack. */
  suss->runtime->stage->GetRootLayer()->InsertSubLayerPath(new_layer->GetIdentifier(), 0);
  suss->runtime->stage->SetEditTarget(new_layer);
  /* Hold a strong ref so MuteLayer can't GC the layer by dropping the PcpLayerStack ref. */
  suss->runtime->anon_layer_refs.push_back(new_layer);
  suss->runtime->refresh_layers();

  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Created layer: %s", new_layer->GetIdentifier().c_str());
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_new_layer(wmOperatorType *ot)
{
  ot->name = "New Layer";
  ot->idname = "USD_STAGE_OT_new_layer";
  ot->description =
      "Create a new anonymous in-memory sublayer on the stage and set it as the edit target";
  ot->exec = usd_new_layer_exec;
  ot->flag = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Prim to New Layer — split one prim out into a sublayer of its own
 * \{ */

/** Remove the prim spec at `path` from `layer`. Returns true when a spec was removed. */
static bool usd_layer_remove_prim_spec(const pxr::SdfLayerHandle &layer, const pxr::SdfPath &path)
{
  pxr::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
  if (!spec) {
    return false;
  }
  const pxr::SdfPath parent_path = path.GetParentPath();
  if (parent_path.IsAbsoluteRootPath()) {
    layer->RemoveRootPrim(spec);
    return true;
  }
  pxr::SdfPrimSpecHandle parent = layer->GetPrimAtPath(parent_path);
  if (!parent) {
    return false;
  }
  return parent->RemoveNameChild(spec);
}

/**
 * True when a layer of the stage's own layer stack *defines* the prim (specifier `def`), as
 * opposed to merely carrying `over`s for it. A published asset typically defines nothing
 * locally: the geometry arrives through a payload or a reference and the root layer holds only
 * overrides, so moving the local specs out would hand the new layer the transform and nothing
 * else — a layer that tracks the mesh rather than owning it.
 */
static bool usd_prim_defined_in_layer_stack(const pxr::UsdStageRefPtr &stage,
                                            const pxr::SdfPath &path)
{
  for (const pxr::SdfLayerHandle &layer : stage->GetLayerStack(false)) {
    if (!layer) {
      continue;
    }
    const pxr::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
    if (spec && spec->GetSpecifier() == pxr::SdfSpecifierDef) {
      return true;
    }
  }
  return false;
}

/**
 * Find the file and path where the asset actually defines this prim, so a new layer can
 * reference it. Walks the prim's composition stack (strongest first, and it spans the layers
 * pulled in by references and payloads, unlike GetLayerStack) for the first `def` that comes
 * from outside the local layer stack and lives in a file on disk. Note the path inside that
 * file need not match the prim's path on the stage — a reference or payload retargets it — so
 * the spec's own path is what gets recorded.
 */
static bool usd_find_external_prim_def(const pxr::UsdPrim &prim,
                                       const pxr::UsdStageRefPtr &stage,
                                       std::string *r_layer_path,
                                       pxr::SdfPath *r_prim_path)
{
  std::vector<std::string> local_ids;
  for (const pxr::SdfLayerHandle &layer : stage->GetLayerStack(false)) {
    if (layer) {
      local_ids.push_back(layer->GetIdentifier());
    }
  }

  for (const pxr::SdfPrimSpecHandle &spec : prim.GetPrimStack()) {
    if (!spec || spec->GetSpecifier() != pxr::SdfSpecifierDef) {
      continue;
    }
    const pxr::SdfLayerHandle layer = spec->GetLayer();
    if (!layer) {
      continue;
    }
    if (std::find(local_ids.begin(), local_ids.end(), layer->GetIdentifier()) != local_ids.end()) {
      continue;
    }
    const std::string real_path = layer->GetRealPath();
    if (real_path.empty()) {
      /* Anonymous layer — there is no asset path to reference. */
      continue;
    }
    *r_layer_path = real_path;
    *r_prim_path = spec->GetPath();
    return true;
  }
  return false;
}

static wmOperatorStatus usd_prim_to_new_layer_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    return OPERATOR_CANCELLED;
  }
  pxr::UsdStageRefPtr stage = suss->runtime->stage;

  char prim_path[1024] = {};
  RNA_string_get(op->ptr, "prim_path", prim_path);
  if (prim_path[0] == '\0') {
    /* Called without an explicit prim, e.g. from a menu: act on the selected prim. */
    BLI_strncpy(prim_path, suss->active_prim_path, sizeof(prim_path));
  }
  if (prim_path[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No prim selected");
    return OPERATOR_CANCELLED;
  }

  const pxr::SdfPath path(prim_path);
  if (!path.IsPrimPath()) {
    BKE_reportf(op->reports, RPT_ERROR, "'%s' is not a prim path", prim_path);
    return OPERATOR_CANCELLED;
  }
  const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
  if (!prim) {
    BKE_reportf(op->reports, RPT_ERROR, "No prim at '%s'", prim_path);
    return OPERATOR_CANCELLED;
  }

  pxr::SdfLayerHandle root = stage->GetRootLayer();
  if (!root) {
    return OPERATOR_CANCELLED;
  }

  /* Layers of this stage that author opinions for the prim, strongest first. Layers reached
   * through a reference or a payload are not part of the layer stack and are left alone. */
  std::vector<pxr::SdfLayerHandle> contributing;
  for (const pxr::SdfLayerHandle &layer : stage->GetLayerStack(false)) {
    if (layer && layer->GetPrimAtPath(path)) {
      contributing.push_back(layer);
    }
  }
  /* A prim can perfectly well have nothing of its own in the layer stack — anything brought in
   * by a payload or a reference (which is most of a published asset) is like that. Then there
   * may be nothing to move, but the useful half of this operator still applies. */
  const bool has_local_opinions = !contributing.empty();

  /* When the layer stack only *overrides* the prim, moving those specs would give the new layer
   * the transform and nothing else. Author it as a `def` that references the file where the
   * asset really defines the prim instead, so the layer owns the prim without duplicating the
   * geometry, and a version bump of the source is a path edit. */
  const bool defined_locally = usd_prim_defined_in_layer_stack(stage, path);
  std::string src_layer_path;
  pxr::SdfPath src_prim_path;
  const bool reference_the_source =
      !defined_locally &&
      usd_find_external_prim_def(prim, stage, &src_layer_path, &src_prim_path);

  /* The new layer is written next to the root layer, so the stage has to live on disk. */
  const std::string root_path = root->GetRealPath();
  if (root_path.empty()) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Save the stage to a file first, the new layer is written next to it");
    return OPERATOR_CANCELLED;
  }
  char root_dir[FILE_MAX];
  BLI_path_split_dir_part(root_path.c_str(), root_dir, sizeof(root_dir));

  char filepath[FILE_MAX] = {};
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    /* Name the layer after the prim, adding a suffix rather than overwriting an existing file.
     * USD prim names are identifiers, so they need no escaping to be used as a file name. */
    char filename[FILE_MAXFILE];
    int suffix = 0;
    do {
      if (suffix == 0) {
        SNPRINTF(filename, "%s.usda", prim.GetName().GetText());
      }
      else {
        SNPRINTF(filename, "%s_%03d.usda", prim.GetName().GetText(), suffix);
      }
      BLI_path_join(filepath, sizeof(filepath), root_dir, filename);
    } while (BLI_exists(filepath) && ++suffix < 1000);

    if (BLI_exists(filepath)) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "No free file name for '%s' in %s",
                  prim.GetName().GetText(),
                  root_dir);
      return OPERATOR_CANCELLED;
    }
  }
  else if (BLI_exists(filepath)) {
    BKE_reportf(op->reports, RPT_ERROR, "File already exists: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* USD identifiers use forward slashes on every platform. */
  std::string layer_identifier(filepath);
  std::replace(layer_identifier.begin(), layer_identifier.end(), '\\', '/');

  pxr::SdfLayerRefPtr new_layer = pxr::SdfLayer::CreateNew(layer_identifier);
  if (!new_layer) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to create layer file: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Reference the layer by name alone when it sits next to the root layer, so the stage stays
   * relocatable. */
  char layer_dir[FILE_MAX];
  BLI_path_split_dir_part(filepath, layer_dir, sizeof(layer_dir));
  std::string sublayer_path = layer_identifier;
  if (BLI_path_cmp_normalized(layer_dir, root_dir) == 0) {
    char filename[FILE_MAXFILE];
    BLI_path_split_file_part(filepath, filename, sizeof(filename));
    sublayer_path = std::string("./") + filename;
  }

  /* When the prim has opinions in several layers, copying them one after the other would have
   * each copy overwrite the previous one. Flatten the layer stack and take the composed prim
   * from there instead, so the new layer reproduces what the stage shows right now. */
  pxr::SdfLayerHandle source;
  pxr::SdfLayerRefPtr flattened;
  if (contributing.size() == 1) {
    source = contributing.front();
  }
  else if (contributing.size() > 1) {
    flattened = pxr::UsdUtilsFlattenLayerStack(stage);
    if (!flattened) {
      BKE_report(op->reports, RPT_ERROR, "Failed to flatten the layer stack");
      return OPERATOR_CANCELLED;
    }
    source = flattened;
  }

  {
    /* Batch the sublayer insert, the copy and the removals into a single recomposition so the
     * prim never briefly vanishes from the stage. */
    pxr::SdfChangeBlock change_block;

    /* Insert at position 0 so the new layer holds the strongest opinion. */
    root->InsertSubLayerPath(sublayer_path, 0);

    const pxr::SdfPath parent_path = path.GetParentPath();
    if (!parent_path.IsAbsoluteRootPath()) {
      /* Ancestors are created as overs; the new layer only takes ownership of the prim. */
      pxr::SdfJustCreatePrimInLayer(new_layer, parent_path);
    }

    if (has_local_opinions) {
      pxr::SdfCopySpec(source, path, new_layer, path);
      for (const pxr::SdfLayerHandle &layer : contributing) {
        usd_layer_remove_prim_spec(layer, path);
      }
    }
    else {
      /* Nothing to move, so just open the prim for editing in the new layer. */
      pxr::SdfJustCreatePrimInLayer(new_layer, path);
    }

    if (!defined_locally) {
      pxr::SdfPrimSpecHandle spec = new_layer->GetPrimAtPath(path);
      if (spec) {
        /* SdfCopySpec brings the `over` specifier across with it, and JustCreatePrimInLayer
         * makes overs too — promote to a definition and give it the composed type so the layer
         * stands on its own. */
        spec->SetSpecifier(pxr::SdfSpecifierDef);
        if (!prim.GetTypeName().IsEmpty()) {
          spec->SetTypeName(prim.GetTypeName().GetString());
        }
        if (reference_the_source) {
          /* Keep the asset path relative when the source sits under the stage's directory, so
           * the layer travels with the asset the way the sublayer entry above does. */
          std::string asset_path(src_layer_path);
          std::replace(asset_path.begin(), asset_path.end(), '\\', '/');
          std::string root_dir_fwd(root_dir);
          std::replace(root_dir_fwd.begin(), root_dir_fwd.end(), '\\', '/');
          if (!root_dir_fwd.empty() && asset_path.rfind(root_dir_fwd, 0) == 0) {
            asset_path = std::string("./") + asset_path.substr(root_dir_fwd.size());
          }
          spec->GetReferenceList().Prepend(pxr::SdfReference(asset_path, src_prim_path));
        }
      }
    }
  }

  /* Write the prim out straight away so the moved data is never held only in memory. */
  if (!new_layer->Save()) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to write layer: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Persist the layers this changed as well, rather than leaving them dirty the way the rest of
   * this editor does. The new layer file is already committed to disk, so a half-saved result is
   * not a neutral "save it yourself later": the sublayer entry and the prim removal live only in
   * memory, and anything that drops them — reopening the stage, reloading a layer, quitting —
   * leaves an orphan layer file sitting next to a stage that never referenced it. That is
   * exactly what it looks like when this operator "does nothing". */
  std::vector<pxr::SdfLayerHandle> to_save = contributing;
  to_save.push_back(root);
  std::vector<std::string> unsaved;
  std::vector<std::string> seen_ids;
  for (const pxr::SdfLayerHandle &layer : to_save) {
    if (!layer || !layer->IsDirty()) {
      continue;
    }
    /* `contributing` can already hold the root layer, so skip the repeat. */
    const std::string id = layer->GetIdentifier();
    if (std::find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end()) {
      continue;
    }
    seen_ids.push_back(id);
    if (layer->IsAnonymous()) {
      /* No file to save to — Save Layer As is the way out, so say which one needs it. */
      unsaved.push_back(layer->GetDisplayName());
      continue;
    }
    if (!layer->Save()) {
      unsaved.push_back(layer->GetDisplayName());
    }
  }
  if (!unsaved.empty()) {
    std::string names;
    for (const std::string &name : unsaved) {
      if (!names.empty()) {
        names += ", ";
      }
      names += name;
    }
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Moved the prim, but these layers still hold unsaved changes: %s",
                names.c_str());
  }

  stage->SetEditTarget(new_layer);
  /* Hold a strong ref so muting a layer cannot drop the last reference to this one. */
  suss->runtime->anon_layer_refs.push_back(new_layer);
  suss->runtime->refresh_layers();
  suss->runtime->refresh_prim_detail(prim_path);

  WM_event_add_notifier(C, NC_SPACE, nullptr);
  if (defined_locally) {
    BKE_reportf(op->reports, RPT_INFO, "Moved '%s' to %s", prim_path, filepath);
  }
  else if (reference_the_source) {
    BKE_reportf(op->reports,
                RPT_INFO,
                "%s now defines '%s', referencing %s",
                filepath,
                prim_path,
                src_layer_path.c_str());
  }
  else {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "'%s' is only overridden in this stage and its definition could not be traced to "
                "a file, so %s holds overrides alone",
                prim_path,
                filepath);
  }
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_prim_to_new_layer(wmOperatorType *ot)
{
  ot->name = "Move Prim to New Layer";
  ot->idname = "USD_STAGE_OT_prim_to_new_layer";
  ot->description =
      "Move this prim and its children into a USD layer file of their own, next to the stage, "
      "and make it the edit target. The prim is removed from the layers that defined it, so "
      "the composed stage is unchanged";
  ot->exec = usd_prim_to_new_layer_exec;
  ot->flag = 0;
  RNA_def_string(ot->srna,
                 "prim_path",
                 nullptr,
                 1024,
                 "Prim Path",
                 "Path of the prim to move, defaults to the selected prim");
  PropertyRNA *prop = RNA_def_string(
      ot->srna,
      "filepath",
      nullptr,
      FILE_MAX,
      "File Path",
      "Where to write the new layer. Defaults to a file named after the prim, next to the stage");
  RNA_def_property_subtype(prop, PROP_FILEPATH);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Sublayer operator — load an existing USD file as a sublayer
 * \{ */

static wmOperatorStatus usd_add_sublayer_exec(bContext *C, wmOperator *op)
{
  ScrArea *hint_area = static_cast<ScrArea *>(op->customdata);
  op->customdata = nullptr;
  SpaceUsdStage *suss = usd_stage_find_or_open(C, hint_area, nullptr);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0')
    return OPERATOR_CANCELLED;

  pxr::SdfLayerHandle root = suss->runtime->stage->GetRootLayer();
  if (!root)
    return OPERATOR_CANCELLED;

  /* Insert at position 0 so the new layer has the strongest opinion. */
  root->InsertSubLayerPath(std::string(filepath), 0);

  /* Hold a strong ref so the layer survives any future MuteLayer call. */
  pxr::SdfLayerRefPtr new_layer = pxr::SdfLayer::FindOrOpen(std::string(filepath));
  if (new_layer) {
    suss->runtime->anon_layer_refs.push_back(new_layer);
  }

  /* Create Blender objects for prims that the new sublayer contributes. */
  suss->runtime->resync_new_prims(C);
  suss->runtime->repull_all_objects();
  suss->runtime->refresh_layers();

  DEG_relations_tag_update(CTX_data_main(C));
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_add_sublayer_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath"))
    return usd_add_sublayer_exec(C, op);
  op->customdata = CTX_wm_area(C);
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void USD_STAGE_OT_add_sublayer(wmOperatorType *ot)
{
  ot->name = "Add Sublayer";
  ot->idname = "USD_STAGE_OT_add_sublayer";
  ot->description = "Add an existing USD file as a sublayer on the root layer";
  ot->exec = usd_add_sublayer_exec;
  ot->invoke = usd_add_sublayer_invoke;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_USD,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Sublayer operator — detach a layer from the root layer's sublayer stack
 * \{ */

static wmOperatorStatus usd_remove_sublayer_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char layer_id[1024] = {};
  RNA_string_get(op->ptr, "layer_id", layer_id);
  if (layer_id[0] == '\0')
    return OPERATOR_CANCELLED;

  const std::string target_id(layer_id);
  pxr::SdfLayerHandle root = suss->runtime->stage->GetRootLayer();
  if (!root)
    return OPERATOR_CANCELLED;

  if (root->GetIdentifier() == target_id) {
    BKE_report(op->reports, RPT_WARNING, "Cannot remove the root layer");
    return OPERATOR_CANCELLED;
  }

  /* Rebuild sublayer path list without the target. */
  std::vector<std::string> new_paths;
  bool found = false;
  for (const std::string &sub_path : root->GetSubLayerPaths()) {
    pxr::SdfLayerHandle sub = pxr::SdfLayer::FindRelativeToLayer(root, sub_path);
    if (!sub)
      sub = pxr::SdfLayer::Find(sub_path);
    if (!sub) {
      for (const pxr::SdfLayerRefPtr &ref : suss->runtime->anon_layer_refs) {
        if (ref && ref->GetIdentifier() == sub_path) {
          sub = ref;
          break;
        }
      }
    }
    if (sub && sub->GetIdentifier() == target_id) {
      found = true;
      /* Drop our strong ref so the layer can be released. */
      auto &refs = suss->runtime->anon_layer_refs;
      refs.erase(std::remove_if(refs.begin(),
                                refs.end(),
                                [&](const pxr::SdfLayerRefPtr &r) {
                                  return r && r->GetIdentifier() == target_id;
                                }),
                 refs.end());
      continue;
    }
    new_paths.push_back(sub_path);
  }

  if (!found) {
    BKE_reportf(op->reports, RPT_WARNING, "Sublayer not found: %s", layer_id);
    return OPERATOR_CANCELLED;
  }

  /* Same as muting: the layer is about to leave the layer stack, so it cannot stay the target. */
  if (usd_edit_target_back_to_root(suss, target_id)) {
    BKE_report(op->reports,
               RPT_INFO,
               "Removed the layer being edited, so the edit target moved to the root layer");
  }

  root->SetSubLayerPaths(new_paths);

  /* Remove Blender objects whose prims no longer exist in the composed scene. */
  Main *bmain = CTX_data_main(C);
  std::vector<std::string> dead;
  for (const auto &[path, ob] : suss->runtime->obj_map) {
    if (!ob) {
      dead.push_back(path);
      continue;
    }
    pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(pxr::SdfPath(path));
    if (!prim) {
      BKE_id_delete(bmain, ob);
      dead.push_back(path);
    }
  }
  for (const std::string &p : dead)
    suss->runtime->obj_map.erase(p);

  suss->runtime->refresh_layers();

  DEG_relations_tag_update(bmain);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_remove_sublayer(wmOperatorType *ot)
{
  ot->name = "Remove Sublayer";
  ot->idname = "USD_STAGE_OT_remove_sublayer";
  ot->description = "Remove this layer from the root layer's sublayer stack";
  ot->exec = usd_remove_sublayer_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_string(
      ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer to remove");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Variant Set operator — add a new variant set to the active prim
 * \{ */

static wmOperatorStatus usd_add_variant_set_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;
  if (suss->active_prim_path[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No active prim selected");
    return OPERATOR_CANCELLED;
  }

  char name[256] = {};
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Variant set name is empty");
    return OPERATOR_CANCELLED;
  }

  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(
      pxr::SdfPath(suss->active_prim_path));
  if (!prim) {
    BKE_report(op->reports, RPT_ERROR, "Active prim not found in stage");
    return OPERATOR_CANCELLED;
  }

  prim.GetVariantSets().AddVariantSet(name);
  suss->runtime->refresh_prim_detail(suss->active_prim_path);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Added variant set: %s", name);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_add_variant_set(wmOperatorType *ot)
{
  ot->name = "Add Variant Set";
  ot->idname = "USD_STAGE_OT_add_variant_set";
  ot->description = "Add a new variant set to the active USD prim";
  ot->exec = usd_add_variant_set_exec;
  ot->invoke = WM_operator_props_popup_confirm;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_string(ot->srna, "name", "VariantSet", 256, "Name", "Variant set name");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Variant operator — add a named variant to an existing variant set
 * \{ */

static wmOperatorStatus usd_add_variant_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  /* Pre-fill variant_set from context string set by the "+" button on each row. */
  char set_name[256] = {};
  RNA_string_get(op->ptr, "variant_set", set_name);
  if (set_name[0] == '\0') {
    if (auto ctx = CTX_data_string_get(C, "usd_variant_set")) {
      RNA_string_set(op->ptr, "variant_set", std::string(*ctx).c_str());
    }
  }
  return WM_operator_props_popup_confirm(C, op, event);
}

static wmOperatorStatus usd_add_variant_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;
  if (suss->active_prim_path[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No active prim selected");
    return OPERATOR_CANCELLED;
  }

  char set_name[256] = {};
  char var_name[256] = {};
  RNA_string_get(op->ptr, "variant_set", set_name);
  RNA_string_get(op->ptr, "variant_name", var_name);

  if (set_name[0] == '\0' || var_name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Variant set or variant name is empty");
    return OPERATOR_CANCELLED;
  }

  pxr::UsdPrim prim = suss->runtime->stage->GetPrimAtPath(
      pxr::SdfPath(suss->active_prim_path));
  if (!prim) {
    BKE_report(op->reports, RPT_ERROR, "Active prim not found in stage");
    return OPERATOR_CANCELLED;
  }

  pxr::UsdVariantSet vset = prim.GetVariantSets().GetVariantSet(set_name);
  if (!vset.AddVariant(var_name)) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to add variant '%s' to set '%s'",
                var_name, set_name);
    return OPERATOR_CANCELLED;
  }

  suss->runtime->refresh_prim_detail(suss->active_prim_path);
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Added variant: %s/%s", set_name, var_name);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_add_variant(wmOperatorType *ot)
{
  ot->name = "Add Variant";
  ot->idname = "USD_STAGE_OT_add_variant";
  ot->description = "Add a new variant to a variant set on the active USD prim";
  ot->exec = usd_add_variant_exec;
  ot->invoke = usd_add_variant_invoke;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* variant_set is filled via context string from the row button — hide it from the popup. */
  PropertyRNA *prop = RNA_def_string(
      ot->srna, "variant_set", nullptr, 256, "Variant Set", "Name of the variant set");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  RNA_def_string(ot->srna, "variant_name", "Variant", 256, "Variant Name",
                 "Name for the new variant");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Stage operator — replace current stage with a fresh empty in-memory stage
 * \{ */

static wmOperatorStatus usd_new_stage_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss)
    return OPERATOR_CANCELLED;

  SpaceUsdStage_Runtime *rt = usd_stage_runtime_ensure(suss);
  rt->close();
  rt->create_new();

  /* Clear filepath so the header shows "(New Stage)". */
  suss->filepath[0] = '\0';
  suss->active_prim_path[0] = '\0';

  WM_event_add_notifier(C, NC_SPACE, nullptr);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_new_stage(wmOperatorType *ot)
{
  ot->name = "New Stage";
  ot->idname = "USD_STAGE_OT_new_stage";
  ot->description = "Discard the current stage and start a fresh empty in-memory stage";
  ot->exec = usd_new_stage_exec;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Export Scene to USD Stage operator
 * \{ */

static wmOperatorStatus usd_export_scene_exec(bContext *C, wmOperator *op)
{
  ScrArea *hint_area = static_cast<ScrArea *>(op->customdata);
  op->customdata = nullptr;
  SpaceUsdStage *suss = usd_stage_find_or_open(C, hint_area, nullptr);
  if (!suss) {
    BKE_report(op->reports, RPT_ERROR, "Open a USD Stage Editor first");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX] = {};
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No file path provided");
    return OPERATOR_CANCELLED;
  }

  io::usd::USDExportParams params{};
  params.export_armatures = true;
  params.export_shapekeys = RNA_boolean_get(op->ptr, "export_shapekeys");
  params.only_deform_bones = false;
  params.export_animation = RNA_boolean_get(op->ptr, "export_animation");
  params.export_meshes = true;
  params.export_lights = true;
  params.export_cameras = true;
  params.export_materials = true;
  params.generate_preview_surface = true;
  params.selected_objects_only = RNA_boolean_get(op->ptr, "selected_objects_only");
  params.evaluation_mode = DAG_EVAL_VIEWPORT;
  /* Frame range comes from scene->r.sfra / r.efra set in Blender's Output Properties. */

  const bool ok = io::usd::USD_export(C, filepath, &params, false, op->reports);
  if (!ok) {
    BKE_reportf(op->reports, RPT_ERROR, "USD export failed: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Open the exported file in the Stage Editor. */
  SpaceUsdStage_Runtime *rt = usd_stage_runtime_ensure(suss);
  rt->close();
  rt->open(filepath);
  STRNCPY(suss->filepath, filepath);

  if (rt->is_open()) {
    rt->populate_blender_from_stage(C);
    DEG_relations_tag_update(CTX_data_main(C));
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
    WM_event_add_notifier(C, NC_SPACE, nullptr);
    BKE_reportf(op->reports, RPT_INFO, "Exported and loaded: %s", filepath);
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_export_scene_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath"))
    return usd_export_scene_exec(C, op);

  SpaceUsdStage *suss = usd_stage_find_or_open(C, CTX_wm_area(C), nullptr);
  if (suss && suss->filepath[0] != '\0') {
    /* Re-use the stage's existing filepath — no browser needed. */
    RNA_string_set(op->ptr, "filepath", suss->filepath);
    return usd_export_scene_exec(C, op);
  }

  /* In-memory stage with no saved path — silently default to tmp/untitled.usd. */
  {
    char tmpdir[FILE_MAX];
    char default_path[FILE_MAX];
    BLI_temp_directory_path_get(tmpdir, sizeof(tmpdir));
    BLI_path_join(default_path, sizeof(default_path), tmpdir, "untitled.usd");
    RNA_string_set(op->ptr, "filepath", default_path);
  }
  return usd_export_scene_exec(C, op);
}

static void USD_STAGE_OT_export_scene(wmOperatorType *ot)
{
  ot->name = "Export Scene to USD Stage";
  ot->idname = "USD_STAGE_OT_export_scene";
  ot->description =
      "Export the current Blender scene (including armatures and skinned meshes) to a USD file "
      "and load the result in the USD Stage Editor";
  ot->exec = usd_export_scene_exec;
  ot->invoke = usd_export_scene_invoke;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_USD,
                                 FILE_SPECIAL,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
  RNA_def_boolean(ot->srna, "export_animation", false, "Export Animation",
                  "Export the full animation range as UsdSkelAnimation time samples");
  RNA_def_boolean(ot->srna, "export_shapekeys", true, "Export Shape Keys",
                  "Export shape keys as UsdSkelBlendShape targets");
  RNA_def_boolean(ot->srna, "selected_objects_only", false, "Selected Objects Only",
                  "Export only the selected objects instead of the entire scene");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Push Skeleton Pose operator — write current pose to UsdSkelAnimation
 * \{ */

static bool usd_push_skel_pose_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_ARMATURE;
}

static wmOperatorStatus usd_push_skel_pose_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_find_open(C);
  if (!suss) {
    BKE_report(op->reports, RPT_ERROR, "Open a USD file in the USD Stage Editor first");
    return OPERATOR_CANCELLED;
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_ARMATURE) {
    BKE_report(op->reports, RPT_ERROR, "Active object must be an armature");
    return OPERATOR_CANCELLED;
  }

  const Scene *scene = CTX_data_scene(C);
  const double frame = scene ? (double)scene->r.cfra : 0.0;

  if (!suss->runtime->push_skel_pose(ob, frame)) {
    BKE_report(op->reports, RPT_WARNING,
               "No matching UsdSkelSkeleton found in the stage for this armature. "
               "Export the scene first via \"Export Scene to USD Stage\".");
    return OPERATOR_CANCELLED;
  }

  suss->runtime->refresh_layers();
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Pushed pose of '%s' at frame %.1f", ob->id.name + 2, frame);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_push_skel_pose(wmOperatorType *ot)
{
  ot->name = "Push Skeleton Pose to USD";
  ot->idname = "USD_STAGE_OT_push_skel_pose";
  ot->description =
      "Write the current armature pose as joint transforms into the UsdSkelAnimation "
      "on the open USD stage at the current frame";
  ot->poll = usd_push_skel_pose_poll;
  ot->exec = usd_push_skel_pose_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layer context menu — right-click popup with all layer operations
 * \{ */

static wmOperatorStatus usd_layer_context_menu_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char layer_id[1024];
  RNA_string_get(op->ptr, "layer_id", layer_id);
  if (layer_id[0] == '\0')
    return OPERATOR_CANCELLED;

  const UsdLayerSnapshot *snap = nullptr;
  for (const UsdLayerSnapshot &s : suss->runtime->layers) {
    if (s.identifier == layer_id) {
      snap = &s;
      break;
    }
  }
  if (!snap)
    return OPERATOR_CANCELLED;

  ui::PopupMenu *pup = ui::popup_menu_begin(C, snap->display_name.c_str(), ICON_NONE);
  ui::Layout &menu_layout = *ui::popup_menu_layout(pup);

  if (!snap->is_edit_target) {
    PointerRNA set_ptr = menu_layout.op(
        "USD_STAGE_OT_set_edit_target", "Set as Edit Target", ICON_EDITMODE_HLT);
    RNA_string_set(&set_ptr, "layer_id", layer_id);
    menu_layout.separator(1.0f, ui::LayoutSeparatorType::Line);
  }

  if (!snap->is_anonymous) {
    if (snap->is_dirty) {
      PointerRNA save_ptr = menu_layout.op("USD_STAGE_OT_save_layer", "Save Layer", ICON_DISK_DRIVE);
      RNA_string_set(&save_ptr, "layer_id", layer_id);
    }
    PointerRNA reload_ptr = menu_layout.op(
        "USD_STAGE_OT_reload_layer", "Reload Layer", ICON_FILE_REFRESH);
    RNA_string_set(&reload_ptr, "layer_id", layer_id);
  }
  else {
    PointerRNA sa_ptr = menu_layout.op(
        "USD_STAGE_OT_save_layer_as", "Save Layer As...", ICON_EXPORT);
    RNA_string_set(&sa_ptr, "layer_id", layer_id);
  }

  menu_layout.separator(1.0f, ui::LayoutSeparatorType::Line);

  {
    const char *mute_label = snap->is_muted ? "Unmute Layer" : "Mute Layer";
    const int mute_icon = snap->is_muted ? ICON_HIDE_OFF : ICON_HIDE_ON;
    PointerRNA mute_ptr = menu_layout.op("USD_STAGE_OT_toggle_layer_mute", mute_label, mute_icon);
    RNA_string_set(&mute_ptr, "layer_id", layer_id);
  }

  if (!snap->is_root) {
    menu_layout.separator(1.0f, ui::LayoutSeparatorType::Line);
    PointerRNA rm_ptr = menu_layout.op("USD_STAGE_OT_remove_sublayer", "Remove Layer", ICON_X);
    RNA_string_set(&rm_ptr, "layer_id", layer_id);
  }

  ui::popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

static void USD_STAGE_OT_layer_context_menu(wmOperatorType *ot)
{
  ot->name = "Layer Context Menu";
  ot->idname = "USD_STAGE_OT_layer_context_menu";
  ot->description = "Show operations for this layer";
  ot->invoke = usd_layer_context_menu_invoke;
  ot->flag = 0;
  RNA_def_string(ot->srna, "layer_id", nullptr, 1024, "Layer ID", "Identifier of the layer");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Save Stage / Save Stage As operators
 * \{ */

static wmOperatorStatus usd_save_stage_exec(bContext *C, wmOperator *op)
{
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  if (!suss->runtime->stage->GetRootLayer()->Save()) {
    BKE_report(op->reports, RPT_ERROR, "Failed to save stage");
    return OPERATOR_CANCELLED;
  }
  suss->runtime->refresh_layers();
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Saved: %s", suss->filepath);
  return OPERATOR_FINISHED;
}

static void USD_STAGE_OT_save_stage(wmOperatorType *ot)
{
  ot->name = "Save Stage";
  ot->idname = "USD_STAGE_OT_save_stage";
  ot->description = "Save the USD stage to its current file";
  ot->exec = usd_save_stage_exec;
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus usd_save_stage_as_exec(bContext *C, wmOperator *op)
{
  ScrArea *hint_area = static_cast<ScrArea *>(op->customdata);
  op->customdata = nullptr;
  SpaceUsdStage *suss = usd_stage_find_or_open(C, hint_area, nullptr);
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return OPERATOR_CANCELLED;

  char filepath[FILE_MAX] = {};
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No file path provided");
    return OPERATOR_CANCELLED;
  }

  /* Export the root layer (preserves sublayer references, no flattening). */
  if (!suss->runtime->stage->GetRootLayer()->Export(std::string(filepath))) {
    BKE_reportf(op->reports, RPT_ERROR, "Failed to export stage: %s", filepath);
    return OPERATOR_CANCELLED;
  }

  /* Reopen from the new path so the stage is properly backed by that file. */
  suss->runtime->close();
  suss->runtime->open(filepath);
  STRNCPY(suss->filepath, filepath);

  suss->runtime->refresh_layers();
  WM_event_add_notifier(C, NC_SPACE, nullptr);
  BKE_reportf(op->reports, RPT_INFO, "Stage saved as: %s", filepath);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus usd_save_stage_as_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "filepath"))
    return usd_save_stage_as_exec(C, op);

  op->customdata = CTX_wm_area(C);
  /* Pre-fill with the current filepath so the browser opens at the right location. */
  SpaceUsdStage *suss = usd_stage_from_context(C);
  if (suss && suss->filepath[0] != '\0')
    RNA_string_set(op->ptr, "filepath", suss->filepath);

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void USD_STAGE_OT_save_stage_as(wmOperatorType *ot)
{
  ot->name = "Save Stage As";
  ot->idname = "USD_STAGE_OT_save_stage_as";
  ot->description = "Save the USD stage to a new file";
  ot->exec = usd_save_stage_as_exec;
  ot->invoke = usd_save_stage_as_invoke;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_USD,
                                 FILE_SPECIAL,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Open Stage Window operator — usable from the global File menu
 * \{ */

static wmOperatorStatus usd_open_window_exec(bContext *C, wmOperator * /*op*/)
{
  /* Re-use any existing Stage Editor area on the current screen first. */
  bScreen *screen = CTX_wm_screen(C);
  if (screen) {
    for (ScrArea *area = static_cast<ScrArea *>(screen->areabase.first); area;
         area = static_cast<ScrArea *>(area->next))
    {
      if (area->spacetype == SPACE_USD_STAGE) {
        WM_event_add_notifier(C, NC_SPACE, nullptr);
        return OPERATOR_FINISHED;
      }
    }
  }
  /* No Stage Editor found — open a new floating window.
   * usd_stage_init will auto-create the empty stage so no browser is needed. */
  wmWindow *win = WM_window_open_temp(C, "USD Stage", SPACE_USD_STAGE, false);
  return win ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static void USD_STAGE_OT_open_window(wmOperatorType *ot)
{
  ot->name = "USD Stage Editor";
  ot->idname = "USD_STAGE_OT_open_window";
  ot->description = "Open the USD Stage Editor in a new window (auto-creates an empty stage)";
  ot->exec = usd_open_window_exec;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

static void usd_stage_operatortypes()
{
  WM_operatortype_append(USD_STAGE_OT_save_stage);
  WM_operatortype_append(USD_STAGE_OT_save_stage_as);
  WM_operatortype_append(USD_STAGE_OT_open_window);
  WM_operatortype_append(USD_STAGE_OT_open);
  WM_operatortype_append(USD_STAGE_OT_set_attr);
  WM_operatortype_append(USD_STAGE_OT_select_attr);
  WM_operatortype_append(USD_STAGE_OT_apply_attr_edit);
  WM_operatortype_append(USD_STAGE_OT_save_layer);
  WM_operatortype_append(USD_STAGE_OT_save_layer_as);
  WM_operatortype_append(USD_STAGE_OT_reload_layer);
  WM_operatortype_append(USD_STAGE_OT_new_layer);
  WM_operatortype_append(USD_STAGE_OT_prim_to_new_layer);
  WM_operatortype_append(USD_STAGE_OT_add_sublayer);
  WM_operatortype_append(USD_STAGE_OT_remove_sublayer);
  WM_operatortype_append(USD_STAGE_OT_set_edit_target);
  WM_operatortype_append(USD_STAGE_OT_toggle_layer_mute);
  WM_operatortype_append(USD_STAGE_OT_add_variant_set);
  WM_operatortype_append(USD_STAGE_OT_add_variant);
  WM_operatortype_append(USD_STAGE_OT_import_material);
  WM_operatortype_append(USD_STAGE_OT_bind_mtlx_material);
  WM_operatortype_append(USD_STAGE_OT_export_object);
  WM_operatortype_append(USD_STAGE_OT_layer_context_menu);
  WM_operatortype_append(USD_STAGE_OT_new_stage);
  WM_operatortype_append(USD_STAGE_OT_export_scene);
  WM_operatortype_append(USD_STAGE_OT_push_skel_pose);

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

  if (suss && suss->runtime && suss->runtime->is_open()) {
    layout.op("USD_STAGE_OT_export_object", "Export Object", ICON_EXPORT);
    layout.separator();
  }

  const char *label;
  if (suss && suss->filepath[0] != '\0')
    label = suss->filepath;
  else if (suss && suss->runtime && suss->runtime->is_open())
    label = "(New Stage)";
  else
    label = "No stage open";
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
