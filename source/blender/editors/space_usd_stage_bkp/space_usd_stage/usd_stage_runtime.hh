/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>
#include <vector>

#include <pxr/usd/usd/stage.h>

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

  /** Flat cache of layer snapshots, refreshed on open/reload. */
  std::vector<UsdLayerSnapshot> layers;

  /** Cached properties for the active prim, refreshed on selection. */
  std::vector<std::pair<std::string, std::string>> prim_properties;
  std::vector<std::pair<std::string, std::string>> prim_variants;
  std::vector<std::pair<std::string, std::string>> prim_references;

  void open(const char *filepath);
  void close();
  void refresh_layers();
  void refresh_prim_detail(const char *sdf_path);

  bool is_open() const
  {
    return (bool)stage;
  }
};

}  // namespace blender
