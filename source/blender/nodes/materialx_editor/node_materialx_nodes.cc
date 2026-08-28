/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Starter node types for the MaterialX node editor tree.  Socket names match MaterialX
 * input/output names so the graph can be serialized to a .mtlx document (a later step).
 * Sockets reuse the built-in static socket types (Float/Color/Vector/Shader/String).
 */

#include "BLI_ustring.hh"

#include "BKE_node.hh"

#include "NOD_node_declaration.hh"
#include "NOD_socket_declarations.hh"

#include "node_materialx_register.hh"

namespace blender {

using nodes::NodeDeclarationBuilder;
namespace decl = nodes::decl;

/* -------------------------------------------------------------------- */
/** \name Declarations
 * \{ */

static void surface_material_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Shader>("surfaceshader"_ustr);
}

static void standard_surface_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("base_color"_ustr).default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Float>("metalness"_ustr).default_value(0.0f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("specular_roughness"_ustr).default_value(0.2f).min(0.0f).max(1.0f);
  b.add_input<decl::Float>("specular_IOR"_ustr).default_value(1.5f).min(0.0f).max(3.0f);
  b.add_input<decl::Vector>("normal"_ustr);
  b.add_output<decl::Shader>("out"_ustr);
}

static void image_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("file"_ustr);
  b.add_input<decl::Vector>("texcoord"_ustr);
  b.add_output<decl::Color>("out"_ustr);
}

static void texcoord_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("out"_ustr);
}

static void constant_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("value"_ustr).default_value(0.0f);
  b.add_output<decl::Float>("out"_ustr);
}

static void multiply_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("in1"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_input<decl::Color>("in2"_ustr).default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>("out"_ustr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void register_node_type_mtlx_surface_material()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeSurfaceMaterial"_ustr);
  ntype.ui_name = "Surface Material";
  ntype.ui_description = "MaterialX surface material output";
  ntype.nclass = NODE_CLASS_OUTPUT;
  ntype.declare = surface_material_declare;
  bke::node_register_type(ntype);
}

void register_node_type_mtlx_standard_surface()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeStandardSurface"_ustr);
  ntype.ui_name = "Standard Surface";
  ntype.ui_description = "Autodesk standard_surface shader";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.declare = standard_surface_declare;
  bke::node_register_type(ntype);
}

void register_node_type_mtlx_image()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeImage"_ustr);
  ntype.ui_name = "Image";
  ntype.ui_description = "Sample an image file";
  ntype.nclass = NODE_CLASS_TEXTURE;
  ntype.declare = image_declare;
  bke::node_register_type(ntype);
}

void register_node_type_mtlx_texcoord()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeTexCoord"_ustr);
  ntype.ui_name = "Texture Coordinates";
  ntype.ui_description = "Geometry texture coordinates";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = texcoord_declare;
  bke::node_register_type(ntype);
}

void register_node_type_mtlx_constant()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeConstant"_ustr);
  ntype.ui_name = "Constant";
  ntype.ui_description = "Constant value";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = constant_declare;
  bke::node_register_type(ntype);
}

void register_node_type_mtlx_multiply()
{
  static bke::bNodeType ntype;
  mtlx_node_type_base(&ntype, "MaterialXNodeMultiply"_ustr);
  ntype.ui_name = "Multiply";
  ntype.ui_description = "Component-wise multiply";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = multiply_declare;
  bke::node_register_type(ntype);
}

/** \} */

}  // namespace blender
