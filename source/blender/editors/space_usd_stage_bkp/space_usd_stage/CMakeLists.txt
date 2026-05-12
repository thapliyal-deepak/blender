set(INC
  ../include
  ../../blenkernel
  ../../blenlib
  ../../blentranslation
  ../../makesdna
  ../../makesrna
  ../../windowmanager
  ../../python/intern
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS
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
  PRIVATE bf::dna
  PRIVATE bf::gpu
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::windowmanager
  PRIVATE bf::dependencies::optional::usd
  PRIVATE bf::dependencies::optional::python
)

if(WITH_USD)
  add_definitions(-DBOOST_DEBUG_PYTHON)
endif()

blender_add_lib(bf_editor_space_usd_stage "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_dependencies(bf_editor_space_usd_stage bf_rna)
