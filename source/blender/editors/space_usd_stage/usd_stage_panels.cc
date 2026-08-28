/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_stage_panels.hh"
#include "usd_stage_runtime.hh"

#include <algorithm>
#include <cstddef>

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DNA_material_types.h"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
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

  PointerRNA suss_ptr = RNA_pointer_create_discrete(
      reinterpret_cast<ID *>(CTX_wm_screen(C)), RNA_SpaceUsdStage, suss);

  /* Same action as the prim tree's right-click menu, kept here so it is discoverable. */
  {
    PointerRNA ptr = layout.op(
        "USD_STAGE_OT_prim_to_new_layer", "Move to New Layer", ICON_ADD);
    RNA_string_set(&ptr, "prim_path", suss->active_prim_path);
    layout.separator();
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
      const bool editing = (suss->edit_attr_name[0] != '\0' && key == suss->edit_attr_name);
      if (editing) {
        /* Inline edit: replace value label with RNA widget + confirm button. */
        const int nc = suss->edit_attr_num_components;
        if (nc == 0) {
          row.prop(&suss_ptr, "edit_attr_value", UI_ITEM_NONE, "", ICON_NONE);
        }
        else if (nc == 1) {
          row.prop(&suss_ptr, "edit_attr_f0", UI_ITEM_NONE, "", ICON_NONE);
        }
        else if (nc == 2) {
          row.prop(&suss_ptr, "edit_attr_row0_2", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else if (nc == 3) {
          row.prop(&suss_ptr, "edit_attr_row0_3", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else if (nc == 4) {
          row.prop(&suss_ptr, "edit_attr_row0", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else {
          /* Matrix — too wide for one row; value shown read-only here, rows below. */
          row.label(val.c_str(), ICON_NONE);
        }
        row.op("USD_STAGE_OT_apply_attr_edit", "", ICON_CHECKMARK);
      }
      else {
        row.label(val.c_str(), ICON_NONE);
        row.context_string_set("usd_attr_name", key);
        row.context_string_set("usd_attr_cur_val", val);
        row.op("USD_STAGE_OT_select_attr", "", ICON_EDITMODE_HLT);
      }
    }
  }

  /* Matrix attrs (nc == 16) are too wide for a single row — expand below the list. */
  if (suss->edit_attr_name[0] != '\0' && suss->edit_attr_num_components == 16) {
    layout.separator();
    layout.prop(&suss_ptr, "edit_attr_row0", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(&suss_ptr, "edit_attr_row1", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(&suss_ptr, "edit_attr_row2", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.prop(&suss_ptr, "edit_attr_row3", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    layout.op("USD_STAGE_OT_apply_attr_edit", "Apply", ICON_NONE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Variants panel
 * \{ */

static void usd_panel_variants_draw_header(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  if (!suss || !suss->runtime || suss->active_prim_path[0] == '\0')
    return;
  ui::Layout &layout = *panel->layout;
  layout.op("USD_STAGE_OT_add_variant_set", "", ICON_ADD);
}

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
    /* "+" adds a new variant to this specific set. */
    row.context_string_set("usd_variant_set", name.c_str());
    row.op("USD_STAGE_OT_add_variant", "", ICON_ADD);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layers panel
 * \{ */

static void usd_panel_layers_draw_header(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  if (!suss || !suss->runtime || !suss->runtime->is_open())
    return;
  ui::Layout &layout = *panel->layout;
  layout.op("USD_STAGE_OT_new_layer", "", ICON_ADD);
  layout.op("USD_STAGE_OT_add_sublayer", "", ICON_FILEBROWSER);
}

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
    /* Edit-target row gets a box (raised background). Non-target rows are dimmed so the
     * active layer stands out clearly even with subtle theme colors. */
    ui::Layout &row = layer.is_edit_target ? col.box().row(false) : col.row(false);
    row.alignment_set(ui::LayoutAlign::Left);

    /* ── Mute toggle (eye icon) ── */
    {
      const int eye_icon = layer.is_muted ? ICON_HIDE_ON : ICON_HIDE_OFF;
      PointerRNA mute_ptr = row.op("USD_STAGE_OT_toggle_layer_mute", "", eye_icon);
      RNA_string_set(&mute_ptr, "layer_id", layer.identifier.c_str());
    }

    /* ── Layer name — clickable to set edit target (unless already active) ── */
    {
      std::string label = layer.display_name;
      if (layer.is_dirty)
        label += " *";

      if (layer.is_edit_target) {
        /* ICON_EDITMODE_HLT is a bright orange pencil icon — immediately legible as
         * "this is what you are editing", unlike the grey ICON_LAYER_ACTIVE dot. */
        row.label(label.c_str(), ICON_EDITMODE_HLT);
      }
      else {
        /* Clickable button for all non-active layers (muted or not) — set as edit target.
         * The operator handles muted layers correctly via SdfLayer::Find. */
        const int icon = layer.is_muted ? ICON_BLANK1 : ICON_LAYER_USED;
        PointerRNA sel_ptr = row.op("USD_STAGE_OT_set_edit_target", label.c_str(), icon);
        RNA_string_set(&sel_ptr, "layer_id", layer.identifier.c_str());
      }
    }

    /* ── Save / Reload ── */
    if (!layer.is_anonymous) {
      /* On-disk layer: save when dirty, reload always available. */
      if (layer.is_dirty) {
        PointerRNA save_ptr = row.op("USD_STAGE_OT_save_layer", "", ICON_DISK_DRIVE);
        RNA_string_set(&save_ptr, "layer_id", layer.identifier.c_str());
      }
      PointerRNA reload_ptr = row.op("USD_STAGE_OT_reload_layer", "", ICON_FILE_REFRESH);
      RNA_string_set(&reload_ptr, "layer_id", layer.identifier.c_str());
    }
    else {
      /* Anonymous layer: offer Save As to give it a file path. */
      PointerRNA sa_ptr = row.op("USD_STAGE_OT_save_layer_as", "", ICON_EXPORT);
      RNA_string_set(&sa_ptr, "layer_id", layer.identifier.c_str());
    }

    /* ── Remove (×) — not available on the root layer ── */
    if (!layer.is_root) {
      PointerRNA rm_ptr = row.op("USD_STAGE_OT_remove_sublayer", "", ICON_X);
      RNA_string_set(&rm_ptr, "layer_id", layer.identifier.c_str());
    }

    /* ── Context menu (⋮) ── */
    {
      PointerRNA ctx_ptr = row.op("USD_STAGE_OT_layer_context_menu", "", ICON_COLLAPSEMENU);
      RNA_string_set(&ctx_ptr, "layer_id", layer.identifier.c_str());
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material panel
 * \{ */

static void usd_panel_material_draw(const bContext *C, Panel *panel)
{
  SpaceUsdStage *suss = reinterpret_cast<SpaceUsdStage *>(CTX_wm_space_data(C));
  ui::Layout &layout = *panel->layout;

  if (!suss || !suss->runtime || suss->active_prim_path[0] == '\0') {
    layout.label("No prim selected.", ICON_INFO);
    return;
  }

  SpaceUsdStage_Runtime *rt = suss->runtime;

  /* When the USD stage changes externally (e.g. Unreal Live Link write), refresh the panel
   * cache and sync updated shader input values into any imported Blender material. */
  if (rt->stage_listener.is_dirty()) {
    rt->refresh_prim_detail(suss->active_prim_path);
    rt->sync_active_material_to_blender(suss->active_prim_path, CTX_data_main(C));
    rt->stage_listener.clear_dirty();
  }

  /* ---- Mesh: show material binding ---- */
  if (!rt->prim_bound_material_path.empty()) {
    ui::Layout &row = layout.row(false);
    row.label("Bound:", ICON_NONE);
    row.label(rt->prim_bound_material_path.c_str(), ICON_MATERIAL);
  }

  /* ---- Geometry prim: bind an external MaterialX (.mtlx) ---- */
  if (rt->prim_is_bindable) {
    layout.op("USD_STAGE_OT_bind_mtlx_material",
              rt->prim_bound_material_path.empty() ? "Bind MaterialX..." : "Rebind MaterialX...",
              ICON_NODE_MATERIAL);
  }

  /* ---- Material prim: show surface networks ---- */
  if (!rt->prim_material_networks.empty()) {
    ui::Layout &col = layout.column(false);
    for (const UsdMaterialNetworkSnapshot &net : rt->prim_material_networks) {
      ui::Layout &row = col.row(false);
      const int icon = (net.label == "MaterialX") ? ICON_NODE_MATERIAL : ICON_SHADING_RENDERED;
      row.label(net.label.c_str(), icon);
      row.label(net.shader_path.c_str(), ICON_NONE);
    }
    layout.separator();

    /* Import to Blender shader nodes. */
    layout.op("USD_STAGE_OT_import_material", "Import to Blender", ICON_IMPORT);
  }

  /* ---- Shader inputs (for material or shader prims) ---- */
  if (!rt->prim_shader_inputs.empty()) {
    layout.separator();

    PointerRNA suss_ptr = RNA_pointer_create_discrete(
        reinterpret_cast<ID *>(CTX_wm_screen(C)), RNA_SpaceUsdStage, suss);

    ui::Layout &col = layout.column(false);
    for (const UsdShaderInputSnapshot &inp : rt->prim_shader_inputs) {
      ui::Layout &row = col.row(false);
      row.label(inp.input_name.c_str(), ICON_NONE);

      const std::string attr_name = "inputs:" + inp.input_name;
      const bool editing = (!inp.has_connection && suss->edit_attr_name[0] != '\0' &&
                            attr_name == suss->edit_attr_name);
      if (editing) {
        const int nc = suss->edit_attr_num_components;
        if (nc == 0) {
          row.prop(&suss_ptr, "edit_attr_value", UI_ITEM_NONE, "", ICON_NONE);
        }
        else if (nc == 1) {
          row.prop(&suss_ptr, "edit_attr_f0", UI_ITEM_NONE, "", ICON_NONE);
        }
        else if (nc == 2) {
          row.prop(&suss_ptr, "edit_attr_row0_2", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else if (nc == 3) {
          row.prop(&suss_ptr, "edit_attr_row0_3", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else if (nc == 4) {
          row.prop(&suss_ptr, "edit_attr_row0", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
        }
        else {
          row.label(inp.value_str.c_str(), ICON_NONE);
        }
        row.op("USD_STAGE_OT_apply_attr_edit", "", ICON_CHECKMARK);
      }
      else {
        row.label(inp.value_str.c_str(), ICON_NONE);
        if (!inp.has_connection) {
          row.context_string_set("usd_attr_name", attr_name.c_str());
          row.context_string_set("usd_attr_cur_val", inp.value_str);
          row.context_string_set("usd_shader_prim_path", inp.shader_prim_path);
          row.op("USD_STAGE_OT_select_attr", "", ICON_EDITMODE_HLT);
        }
      }
    }

    /* Matrix attrs (nc == 16) expand below. */
    if (suss->edit_attr_name[0] != '\0' && suss->edit_attr_num_components == 16) {
      layout.separator();
      layout.prop(&suss_ptr, "edit_attr_row0", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      layout.prop(&suss_ptr, "edit_attr_row1", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      layout.prop(&suss_ptr, "edit_attr_row2", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      layout.prop(&suss_ptr, "edit_attr_row3", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
      layout.op("USD_STAGE_OT_apply_attr_edit", "Apply", ICON_NONE);
    }
  }

  if (rt->prim_bound_material_path.empty() && rt->prim_material_networks.empty() &&
      rt->prim_shader_inputs.empty())
  {
    layout.label("No material data.", ICON_INFO);
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
  pt->draw_header = usd_panel_variants_draw_header;
  pt->draw = usd_panel_variants_draw;
  BLI_addtail(&art->paneltypes, pt);

  /* Layers panel. */
  pt = MEM_new_zeroed<PanelType>("usd stage layers panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_layers");
  STRNCPY_UTF8(pt->label, N_("Layers"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw_header = usd_panel_layers_draw_header;
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

  /* Material panel. */
  pt = MEM_new_zeroed<PanelType>("usd stage material panel");
  STRNCPY_UTF8(pt->idname, "USD_STAGE_PT_material");
  STRNCPY_UTF8(pt->label, N_("Material"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->space_type = SPACE_USD_STAGE;
  pt->region_type = RGN_TYPE_UI;
  pt->draw = usd_panel_material_draw;
  BLI_addtail(&art->paneltypes, pt);
}

}  // namespace blender
