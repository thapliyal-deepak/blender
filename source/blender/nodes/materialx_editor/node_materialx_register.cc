/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#include "node_materialx_register.hh"

namespace blender {

void register_materialx_nodes()
{
  register_node_tree_type_mtlx();

  register_node_type_mtlx_surface_material();
  register_node_type_mtlx_standard_surface();
  register_node_type_mtlx_image();
  register_node_type_mtlx_texcoord();
  register_node_type_mtlx_constant();
  register_node_type_mtlx_multiply();
}

}  // namespace blender
