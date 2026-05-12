/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_stage_panels.hh"
#include "usd_stage_runtime.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Properties panel
 * \{ */

/* Number of leading "info" rows (USD Path / Type / Active / Instance) that are read-only. */
static constexpr int USD_PROPS_INFO_COUNT = 4;

static void usd_panel_properties_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || suss->active_prim_path[0] == '\0') {
    layout.label("No prim selected.", ICON_INFO);
    return;
  }

  ui::Layout &col = layout.column(false);
  const auto &props = suss->runtime->prim_properties;
  for (int i = 0; i < (int)props.size(); i++) {
    const auto &[key, val] = props[i];
    ui::Layout &row = col.row(false);
    row.label(key.c_str(), ICON_NONE);

    if (i < USD_PROPS_INFO_COUNT) {
      /* Info entries (path, type, active, instance): read-only label. */
      row.label(val.c_str(), ICON_NONE);
    }
    else {
      /* Authored attribute: current value + small button to stage it for inline editing. */
      row.label(val.c_str(), ICON_NONE);
      row.context_string_set("usd_attr_name", key);
      row.context_string_set("usd_attr_cur_val", val);
      row.op("USD_STAGE_OT_select_attr", "", ICON_EDITMODE_HLT);
    }
  }

  /* Inline edit zone — shown when an attribute has been staged for editing. */
  if (suss->edit_attr_name[0] != '\0') {
    layout.separator();

    /* Label showing which attribute is being edited. */
    {
      ui::Layout &hdr = layout.row(false);
      hdr.label("Editing:", ICON_NONE);
      hdr.label(suss->edit_attr_name, ICON_NONE);
    }

    /* Editable text box bound to the RNA string property. */
    PointerRNA suss_ptr = RNA_pointer_create_discrete(
        reinterpret_cast<ID *>(CTX_wm_screen(C)), RNA_SpaceUsdStage, suss);
    layout.prop(&suss_ptr, "edit_attr_value", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    /* Apply button writes the value back to USD. */
    layout.op("USD_STAGE_OT_apply_attr_edit", "Apply", ICON_NONE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Variants panel
 * \{ */

static void usd_panel_variants_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || suss->runtime->prim_variants.empty()) {
    layout.label("No variants.", ICON_INFO);
    return;
  }

  ui::Layout &col = layout.column(false);
  for (const auto &[name, sel] : suss->runtime->prim_variants) {
    ui::Layout &row = col.row(false);
    row.label(name.c_str(), ICON_NONE);
    row.label(sel.c_str(), ICON_NONE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layers panel
 * \{ */

static void usd_panel_layers_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || !suss->runtime->is_open()) {
    layout.label("No layers.", ICON_INFO);
    return;
  }

  /* Live-refresh every draw so dirty state reflects current in-memory edits. */
  suss->runtime->refresh_layers();

  if (suss->runtime->layers.empty()) {
    layout.label("No layers.", ICON_INFO);
    return;
  }

  ui::Layout &col = layout.column(false);
  for (const UsdLayerSnapshot &layer : suss->runtime->layers) {
    ui::Layout &row = col.row(false);

    const int icon = layer.is_edit_target ? ICON_LAYER_ACTIVE :
                     layer.is_dirty       ? ICON_LAYER_USED :
                                            ICON_BLANK1;
    std::string label = layer.display_name;
    if (layer.is_edit_target) {
      label = "\xe2\x9c\x8f  " + label; /* UTF-8 pencil */
    }
    if (layer.is_dirty) {
      label += " *";
    }
    row.label(label.c_str(), icon);

    /* Save / Reload — only meaningful for on-disk (non-anonymous) layers. */
    if (!layer.is_anonymous) {
      /* Set context string so execs can find the layer even if RNA property routing breaks. */
      row.context_string_set("usd_layer_id", layer.identifier);

      PointerRNA save_ptr = row.op("USD_STAGE_OT_save_layer", "", ICON_DISK_DRIVE);
      RNA_string_set(&save_ptr, "layer_id", layer.identifier.c_str());

      PointerRNA reload_ptr = row.op("USD_STAGE_OT_reload_layer", "", ICON_FILE_REFRESH);
      RNA_string_set(&reload_ptr, "layer_id", layer.identifier.c_str());
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name References panel
 * \{ */

static void usd_panel_references_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || suss->runtime->prim_references.empty()) {
    layout.label("No references.", ICON_INFO);
    return;
  }

  ui::Layout &col = layout.column(false);
  for (const auto &[layer, prim] : suss->runtime->prim_references) {
    ui::Layout &row = col.row(false);
    row.label(layer.c_str(), ICON_NONE);
    row.label(prim.c_str(), ICON_NONE);
  }
}

/** \} */

void usd_stage_panels_register(ARegionType *art)
{
  /* Properties panel. */
  PanelType *pt = MEM_new_zeroed<PanelType>("usd stage properties panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_properties");
  STRNCPY_UTF8(pt->label, N_("Prim Properties"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw = usd_panel_properties_draw;
  BLI_addtail(&art->paneltypes, pt);

  /* Variants panel. */
  pt = MEM_new_zeroed<PanelType>("usd stage variants panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_variants");
  STRNCPY_UTF8(pt->label, N_("Variants"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw = usd_panel_variants_draw;
  BLI_addtail(&art->paneltypes, pt);

  /* Layers panel. */
  pt = MEM_new_zeroed<PanelType>("usd stage layers panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_layers");
  STRNCPY_UTF8(pt->label, N_("Layers"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw = usd_panel_layers_draw;
  BLI_addtail(&art->paneltypes, pt);

  /* References panel. */
  pt = MEM_new_zeroed<PanelType>("usd stage references panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_references");
  STRNCPY_UTF8(pt->label, N_("References"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw = usd_panel_references_draw;
  BLI_addtail(&art->paneltypes, pt);
}

}  // namespace blender
