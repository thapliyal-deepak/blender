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
struct Camera;
struct Light;
struct Main;
struct Material;
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
  bool is_muted = false;
  bool is_root = false;
};

/** One input on a USD shader prim, cached for the Material sidebar panel. */
struct UsdShaderInputSnapshot {
  std::string shader_prim_path;
  std::string input_name;
  std::string value_str;
  bool has_connection = false;
};

/** A surface network entry (e.g. UsdPreviewSurface or MaterialX) on a material prim. */
struct UsdMaterialNetworkSnapshot {
  std::string label;         /* "UsdPreviewSurface" or "MaterialX" */
  std::string shader_path;   /* SdfPath of the terminal shader prim */
};

struct SpaceUsdStage_Runtime {
  pxr::UsdStageRefPtr stage;

  /* Map to track Sdfpath -> Blender Object pointer */
  std::unordered_map<std::string, Object *> obj_map;

  /** Skeleton prim path -> the Blender armature object mirroring it. Kept apart from obj_map
   *  because an armature is not a prim proxy the user edits: it is the rig that deforms the
   *  meshes, and it is posed per frame rather than pushed back. */
  std::unordered_map<std::string, Object *> skel_map;

  /**
   * Cached Main pointer — set on populate/export so validate_obj_map() can
   * run without a bContext (e.g. from the area listener).
   */
  Main *bmain = nullptr;

  /** Flat cache of layer snapshots, refreshed on open/reload. */
  std::vector<UsdLayerSnapshot> layers;

  /** Cached properties for the active prim, refreshed on selection. */
  std::vector<std::pair<std::string, std::string>> prim_properties;
  std::vector<std::pair<std::string, std::string>> prim_variants;
  std::vector<std::pair<std::string, std::string>> prim_references;

  /** Material binding for mesh prims (empty when prim has no binding). */
  std::string prim_bound_material_path;
  /** True when the active prim is bindable geometry (imageable, not a material). */
  bool prim_is_bindable = false;
  /** Surface networks on material prims (UsdPreviewSurface / MaterialX). */
  std::vector<UsdMaterialNetworkSnapshot> prim_material_networks;
  /** Shader inputs on material/shader prims, for inline editing. */
  std::vector<UsdShaderInputSnapshot> prim_shader_inputs;

  /** Per-instance USD notice listener. Registered on open, revoked on close. */
  io::usd::UsdStageListener stage_listener;

  /** Strong references to all sublayers (anonymous and file-based).
   * When MuteLayer() is called, the PcpLayerStack drops its SdfLayerRefPtr to the muted layer.
   * If nothing else holds a ref, the layer is evicted from the SdfLayer registry and
   * SdfLayer::Find/FindRelativeToLayer return null — making the layer vanish from the panel.
   * Anonymous layers are added here by usd_new_layer_exec; file layers by refresh_layers(). */
  std::vector<pxr::SdfLayerRefPtr> anon_layer_refs;

  void open(const char *filepath);
  /** Create an empty stage.
   *  If filepath is given the stage is backed by a real file on disk (created fresh).
   *  If filepath is null/empty an anonymous in-memory stage is used instead.
   *  Either way a /World Xform root is added as the default prim. */
  void create_new(const char *filepath = nullptr);
  void close();
  void refresh_layers();
  void refresh_prim_detail(const char *sdf_path);
  void sync_active_prim(struct Object *ob, int dirty_bits, bool is_push = false);
  void populate_blender_from_stage(const bContext *C);
  void create_blender_mesh_for_prim(const bContext *C, pxr::UsdPrim prim);
  void create_blender_camera_for_prim(const bContext *C, pxr::UsdPrim prim);
  /** Mirror a UsdSkelSkeleton prim as a Blender armature object. */
  void create_blender_armature_for_prim(const bContext *C, pxr::UsdPrim prim);
  /** Pose every mirrored armature for `frame`. Cheap: only pose channels are touched, the
   *  armature modifiers do the deformation. */
  void pull_skel_poses(double frame);
  void create_blender_light_for_prim(const bContext *C, pxr::UsdPrim prim);
  /** Re-pull mesh + xform for every tracked object (called after a layer reload). */
  /** Re-read everything from USD into the Blender objects.
   *  `mesh_dirty_bits` lets a caller ask for a cheaper update: a frame change only moves points,
   *  so rebuilding topology and re-resolving materials 24 times a second is pure waste. */
  void repull_all_objects(int mesh_dirty_bits = -1);
  /** Create Blender objects for prims in the composed stage that are not yet in obj_map.
   *  Used after adding a sublayer so new prims appear without a full repopulate. */
  void resync_new_prims(const bContext *C);
  /** Sync live USD shader input values into the Blender material node tree (called on dirty). */
  void sync_active_material_to_blender(const char *sdf_path, struct Main *bmain);
  /** Push the current Blender transform of every tracked object to USD (called on transform). */
  void push_all_xforms();
  /** Push the current Blender mesh data of every tracked mesh object to USD (called on geom edit). */
  void push_all_meshes();
  /** Push light energy/colour for every tracked light to USD (called on NC_LAMP). */
  void push_all_light_data();
  /** Push all Blender material node values back to bound USD shader inputs (called on NC_MATERIAL). */
  void push_all_materials();
  /**
   * Export a Blender object to the USD stage for the first time.
   * If the object is already tracked in obj_map, pushes latest mesh+xform data instead.
   * Returns the SdfPath string that was written, or empty string on failure.
   */
  std::string export_object(Object *ob, Main *bmain);
  /** Push the current armature pose as joint-transform samples into the open stage.
   *  Searches for a UsdSkelSkeleton whose joints match arm_ob's bones, then writes
   *  the parent-relative pose matrices to the associated UsdSkelAnimation at `frame`.
   *  Returns true if at least one joint was written. */
  bool push_skel_pose(Object *arm_ob, double frame);
  /**
   * Scan obj_map for entries whose Object* is no longer in bmain->objects
   * (i.e. the Blender object was deleted). Removes the corresponding USD prim
   * from the stage and erases the entry. Safe to call with freed pointers because
   * BLI_findindex only compares pointer values, never dereferences them.
   */
  void validate_obj_map();
  static Collection *ensure_usd_sync_collection(Main *bmain, Scene *scene);

  bool is_open() const
  {
    return (bool)stage;
  }
};

/** Apply constant (non-texture-connected) shader inputs from a USD material prim to
 *  bl_mat's Principled BSDF.  Reads both UsdPreviewSurface (universal) and MaterialX
 *  (mtlx) render contexts.  Call this after the node tree has been set up so the
 *  Principled BSDF node already exists. */
void apply_usd_material_inputs_to_material(pxr::UsdStageRefPtr stage,
                                            const std::string &mat_prim_path,
                                            Material *bl_mat);

}  // namespace blender
