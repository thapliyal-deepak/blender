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

#include "DNA_space_types.h"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Properties panel
 * \{ */

static void usd_panel_properties_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || suss->active_prim_path[0] == '\0') {
    layout.label("No prim selected.", ICON_INFO);
    return;
  }

  ui::Layout &col = layout.column(false);
  for (const auto &[key, val] : suss->runtime->prim_properties) {
    ui::Layout &row = col.row(false);
    row.label(key.c_str(), ICON_NONE);
    row.label(val.c_str(), ICON_NONE);
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

  if (!suss || !suss->runtime || suss->runtime->layers.empty()) {
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
    row.label(label.c_str(), icon);
    row.label(layer.is_dirty ? "dirty" : "clean", ICON_NONE);
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
}

}  // namespace blender
