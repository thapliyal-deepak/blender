# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  ..
  ../intern
  ../../editors/include
  ../../makesrna
  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
)

set(SRC
  node_materialx_nodes.cc
  node_materialx_register.cc
  node_materialx_tree.cc

  node_materialx_register.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  bf_nodes
  PRIVATE bf::windowmanager
)

blender_add_lib(bf_nodes_materialx_editor "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.hh
add_dependencies(bf_nodes_materialx_editor bf_rna)
