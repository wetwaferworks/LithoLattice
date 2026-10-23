# litholattice_tests: engine-only tests (links no game code). ctest runs one test per module.
add_executable(litholattice_tests engine_tests.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/render/software_test.cpp)
# PBR tests load the shipped defaults data file by absolute source path.
target_compile_definitions(litholattice_tests PRIVATE
  LITHOKRAFT_MATERIALS_PATH="${LithoLattice_SOURCE_DIR}/assets/materials/lithokraft.materials")
target_link_libraries(litholattice_tests PRIVATE litholattice_render_vk litholattice_storage litholattice_ai litholattice_sim litholattice_render litholattice_world litholattice_procgen
                      litholattice_ui litholattice_assets litholattice_audio litholattice_input litholattice_net litholattice_script litholattice_mods litholattice_console litholattice_crypto litholattice_index litholattice_mcp litholattice_defrag litholattice_subdiv litholattice_anim litholattice_texture litholattice_core)
litholattice_module_options(litholattice_tests)
foreach(module core world render sim ai anim storage procgen trees shrubs water litho_formula ui assets audio input net script mods console crypto index mcp defrag subdiv texture)
  add_test(NAME engine_${module} COMMAND litholattice_tests ${module})
  # fix 3b: out-of-tree build dirs cannot find game/ assets (wafer.json) by walking up the cwd.
  set_tests_properties(engine_${module} PROPERTIES ENVIRONMENT "LITHO_SOURCE_ROOT=${LithoLattice_SOURCE_DIR}/..")
endforeach()
# Cortana 2026-10-15 1500 (TOOLS, MC-02): these modules write to fixed %TEMP% paths; never run them
# concurrently under parallel ctest.
set_tests_properties(engine_storage engine_assets engine_mods engine_world PROPERTIES RESOURCE_LOCK litho_tempdir)

# Fix run 2 (2026-10-23, R-SIM-1): engine_sim timed out once under parallel load in run 1;
# not reproduced in 6 clean -j8 runs (1.7 s each). Guard: a hang now fails in 300 s with
# ctest's timeout report instead of stalling the whole suite (default 1500 s).
if(TEST engine_sim)
  set_tests_properties(engine_sim PROPERTIES TIMEOUT 300)
endif()
