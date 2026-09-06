/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * The "MaterialX" node tree type: a native editor for authoring MaterialX shading graphs.
 * Step 1 registers the (empty) tree type so it appears in the Node Editor's tree-type
 * dropdown and can be created; node types are added in a later step.
 */

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"

#include "BLI_string.hh"
#include "BLI_ustring.hh"
#include "BLI_utildefines.hh"

#include "BKE_node.hh"

#include "BLT_translation.hh"

#include "node_materialx_register.hh"
#include "node_util.hh"

#include "RNA_prototypes.hh"

#include "UI_resources.hh"

namespace blender {

static void foreach_nodeclass(void *calldata, bke::bNodeClassCallback func)
{
  func(calldata, NODE_CLASS_INPUT, N_("Input"));
  func(calldata, NODE_CLASS_SHADER, N_("Shader"));
  func(calldata, NODE_CLASS_TEXTURE, N_("Texture"));
  func(calldata, NODE_CLASS_OP_COLOR, N_("Color"));
  func(calldata, NODE_CLASS_CONVERTER, N_("Converter"));
  func(calldata, NODE_CLASS_OUTPUT, N_("Output"));
  func(calldata, NODE_CLASS_LAYOUT, N_("Layout"));
}

static bool materialx_node_tree_socket_type_valid(bke::bNodeTreeType * /*treetype*/,
                                                  bke::bNodeSocketType *socket_type)
{
  return bke::node_is_static_socket_type(*socket_type) &&
         ELEM(socket_type->type, SOCK_FLOAT, SOCK_VECTOR, SOCK_RGBA, SOCK_SHADER, SOCK_STRING);
}

/* MaterialX node types are only valid inside a MaterialX node tree. */
static bool mtlx_node_poll_default(const bke::bNodeType * /*ntype*/,
                                   const bNodeTree *ntree,
                                   const char **r_disabled_hint)
{
  if (!STREQ(ntree->idname, "MaterialXNodeTree")) {
    *r_disabled_hint = RPT_("Not a MaterialX node tree");
    return false;
  }
  return true;
}

void mtlx_node_type_base(bke::bNodeType *ntype,
                         UString idname,
                         const std::optional<int16_t> legacy_type)
{
  bke::node_type_base(*ntype, idname, legacy_type);
  ntype->poll = mtlx_node_poll_default;
  ntype->insert_link = node_insert_link_default;
}

bke::bNodeTreeType *ntreeType_MaterialX;

void register_node_tree_type_mtlx()
{
  bke::bNodeTreeType *tt = ntreeType_MaterialX = MEM_new<bke::bNodeTreeType>(__func__);

  tt->type = NTREE_MATERIALX;
  tt->idname = "MaterialXNodeTree"_ustr;
  tt->group_idname = "MaterialXNodeGroup"_ustr;
  tt->ui_name = N_("MaterialX Editor");
  tt->ui_icon = ICON_MATERIAL;
  tt->ui_description = N_("Author MaterialX shading graphs");

  tt->foreach_nodeclass = foreach_nodeclass;
  tt->valid_socket_type = materialx_node_tree_socket_type_valid;

  /* Node groups are not supported yet; hide the group-building UI. */
  tt->no_group_interface = 1;

  tt->rna_ext.srna = RNA_MaterialXNodeTree;

  bke::node_tree_type_add(*tt);
}

}  // namespace blender
