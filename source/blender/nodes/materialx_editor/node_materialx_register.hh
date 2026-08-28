/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Native MaterialX node editor tree type (authoring MaterialX shading graphs in Blender).
 */

#pragma once

#include <optional>

#include "BLI_ustring.hh"

#include "BKE_node.hh"

namespace blender {

/** Register the "MaterialX" node tree type. */
void register_node_tree_type_mtlx();

/** Register the MaterialX tree type and its node types. */
void register_materialx_nodes();

/** Base initializer for MaterialX node types: sets the tree-restriction poll + link insert. */
void mtlx_node_type_base(bke::bNodeType *ntype,
                         UString idname,
                         std::optional<int16_t> legacy_type = std::nullopt);

/* MaterialX node types. */
void register_node_type_mtlx_surface_material();
void register_node_type_mtlx_standard_surface();
void register_node_type_mtlx_image();
void register_node_type_mtlx_texcoord();
void register_node_type_mtlx_constant();
void register_node_type_mtlx_multiply();

}  // namespace blender
