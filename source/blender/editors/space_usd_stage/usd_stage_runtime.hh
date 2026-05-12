/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_ID.h"
#include "usd_notice_handler.hh"
#include <string>
#include <unordered_map>
#include <vector>

#include <pxr/usd/usd/stage.h>

namespace blender {
struct bContext;
struct Object;
struct Mesh;
struct Main;
struct Collection;
struct Scene;
}  // namespace blender

namespace blender {

struct UsdPrimSnapshot {
  std::string path;
  std::string name;
  std::string type_name;
  bool has_payload = false;
  bool is_instance = false;
};

struct UsdLayerSnapshot {
  std::string identifier;
  std::string display_name;
  bool is_dirty = false;
  bool is_anonymous = false;
  bool is_edit_target = false;
};

struct SpaceUsdStage_Runtime {
  pxr::UsdStageRefPtr stage;

  /* Map to track Sdfpath -> Blender Object pointer */
  std::unordered_map<std::string, Object *> obj_map;

  /** Flat cache of layer snapshots, refreshed on open/reload. */
  std::vector<UsdLayerSnapshot> layers;

  /** Cached properties for the active prim, refreshed on selection. */
  std::vector<std::pair<std::string, std::string>> prim_properties;
  std::vector<std::pair<std::string, std::string>> prim_variants;
  std::vector<std::pair<std::string, std::string>> prim_references;

  /** Per-instance USD notice listener. Registered on open, revoked on close. */
  io::usd::UsdStageListener stage_listener;

  void open(const char *filepath);
  void close();
  void refresh_layers();
  void refresh_prim_detail(const char *sdf_path);
  void sync_active_prim(struct Object *ob, int dirty_bits, bool is_push = false);
  void populate_blender_from_stage(const bContext *C);
  void create_blender_mesh_for_prim(const bContext *C, pxr::UsdPrim prim);
  /** Re-pull mesh + xform for every tracked object (called after a layer reload). */
  void repull_all_objects();
  /** Push the current Blender transform of every tracked object to USD (called on transform). */
  void push_all_xforms();
  /** Push the current Blender mesh data of every tracked mesh object to USD (called on geom edit). */
  void push_all_meshes();
  static Collection *ensure_usd_sync_collection(Main *bmain, Scene *scene);

  bool is_open() const
  {
    return (bool)stage;
  }
};

}  // namespace blender
