# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Add-menu entries for the native MaterialX node editor (tree type "MaterialXNodeTree").

The node types themselves are registered in C++ (bf_nodes_materialx_editor), but the node
editor's Shift+A / "Add" menu is built by `NODE_MT_add` (bl_ui/space_node.py), which only
knows about the built-in tree types.  For any other tree type it falls through to the legacy
node-category system (empty here), so the MaterialX editor's Add menu looks empty.

This module appends the MaterialX node types to `NODE_MT_add` when the active editor is a
MaterialX node tree, so they can actually be added.
"""

import bpy

MTLX_TREE = "MaterialXNodeTree"

# (node bl_idname, menu label) — mirrors the C++ registrations in node_materialx_nodes.cc.
_MTLX_NODES = (
    ("MaterialXNodeStandardSurface", "Standard Surface"),
    ("MaterialXNodeSurfaceMaterial", "Surface Material"),
    None,  # separator
    ("MaterialXNodeImage", "Image"),
    ("MaterialXNodeTexCoord", "Texture Coordinates"),
    None,
    ("MaterialXNodeConstant", "Constant"),
    ("MaterialXNodeMultiply", "Multiply"),
)


def _draw_materialx_add(self, context):
    snode = context.space_data
    if not snode or getattr(snode, "tree_type", "") != MTLX_TREE:
        return
    layout = self.layout
    for item in _MTLX_NODES:
        if item is None:
            layout.separator()
            continue
        idname, label = item
        props = layout.operator("node.add_node", text=label)
        props.type = idname
        props.use_transform = True


def register():
    bpy.types.NODE_MT_add.append(_draw_materialx_add)


def unregister():
    bpy.types.NODE_MT_add.remove(_draw_materialx_add)


if __name__ == "__main__":
    register()
