set(INC
  ../include
  ../../blenkernel
  ../../blenlib
  ../../blentranslation
  ../../bmesh
  ../../imbuf
  ../../makesdna
  ../../makesrna
  ../../windowmanager
  ../../python/intern
  ../../depsgraph
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
  ../../io/usd/intern/usd_stage
  ../../io/usd/intern
  ../../io/usd
)

set(INC_SYS
  ${PYTHON_INCLUDE_DIR}
)

set(SRC
  space_usd_stage.cc
  usd_prim_tree_view.cc
  usd_stage_panels.cc
  usd_stage_runtime.cc
  space_usd_stage.hh
  usd_prim_tree_view.hh
  usd_stage_panels.hh
  usd_stage_runtime.hh
)

set(LIB
  PRIVATE bf::blenfont
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::bmesh
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::imbuf
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
  PRIVATE bf::dependencies::optional::usd
  PRIVATE bf::dependencies::optional::python
  PRIVATE bf_io_usd
  PRIVATE bf_editor_interface
)

if(WITH_USD)
  add_definitions(-DBOOST_DEBUG_PYTHON)
endif()

blender_add_lib(bf_editor_space_usd_stage "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_dependencies(bf_editor_space_usd_stage bf_rna)
