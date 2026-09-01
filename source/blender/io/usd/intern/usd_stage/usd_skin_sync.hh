/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/timeCode.h>

struct Main;
struct Object;

namespace blender::io::usd {

/**
 * UsdSkel evaluation for the stage editor's pull path.
 *
 * A skinned mesh stores its points in its own space, undeformed, and leaves the deformation to
 * UsdSkel: the joint transforms plus `primvars:skel:geomBindTransform` are what put the geometry
 * where it belongs. Reading `points` on its own therefore shows a rig at whatever scale its meshes
 * happened to be modelled at, piled on the origin, no matter how correct the file is.
 *
 * Points that come back from #skin_pull_points are in **skeleton space**, so an object holding them
 * is placed by #skin_skel_to_world -- not by the prim's own transform and not by its bind transform,
 * both of which UsdSkel has already accounted for.
 */

/** The time the stage editor evaluates the stage at; follows the scene's current frame. */
pxr::UsdTimeCode skin_eval_time();
void skin_set_eval_time(pxr::UsdTimeCode time);

/**
 * True when `prim` is a mesh UsdSkel deforms: it resolves a skeleton, carries joint weights and
 * sits under a SkelRoot. Prims that merely bind a skeleton without weights are not included, since
 * nothing would deform them.
 */
bool skin_is_skinned(const pxr::UsdPrim &prim);

/**
 * Deformed points for `prim` at `time`, in skeleton space, in USD's axis convention.
 * False when the prim is not skinned or UsdSkel could not evaluate it, in which case the caller
 * should fall back to the authored `points`.
 */
bool skin_pull_points(const pxr::UsdPrim &prim,
                      pxr::UsdTimeCode time,
                      pxr::VtArray<pxr::GfVec3f> *r_points);

/** Skeleton space to world transform for a skinned `prim`. */
bool skin_skel_to_world(const pxr::UsdPrim &prim, pxr::UsdTimeCode time, pxr::GfMatrix4d *r_mat);

/* -------------------------------------------------------------------------------------------
 * Armature mirroring
 *
 * Rather than deform points numerically, mirror the skeleton as a real Blender armature and let
 * Blender deform: the joints become visible and selectable, the mesh follows through an armature
 * modifier, and per frame there is nothing to do but set pose channels. It also gives
 * push_skel_pose() the counterpart it never had -- that operator writes an armature's pose back
 * into a UsdSkelAnimation but, until now, only for an armature the user built by hand.
 */

/** The Skeleton prim deforming `mesh_prim`; an invalid prim when there is none. */
pxr::UsdPrim skin_skeleton_for_mesh(const pxr::UsdPrim &mesh_prim);

/** Build the bones of `arm_ob`'s armature from the skeleton at `skel_prim`. */
bool skin_build_armature(Main *bmain, Object *arm_ob, const pxr::UsdPrim &skel_prim);

/** Deform groups + an armature modifier, so `arm_ob` drives `mesh_ob`. */
bool skin_bind_mesh(Object *mesh_ob, Object *arm_ob, const pxr::UsdPrim &mesh_prim);

/** Place `arm_ob` in the world for the skeleton at `skel_prim`. */
void skin_place_armature(Object *arm_ob, const pxr::UsdPrim &skel_prim);

/** Pose `arm_ob` from `skel_prim` at `time`. */
bool skin_pull_pose(Object *arm_ob, const pxr::UsdPrim &skel_prim, pxr::UsdTimeCode time);

}  // namespace blender::io::usd
