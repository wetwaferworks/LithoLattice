# litholattice_tests: engine-only tests (links no game code). ctest runs one test per module.
add_executable(litholattice_tests engine_tests.cpp canonical_mesh_test.cpp glsl_attr_parse_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/render/software_test.cpp)
# PBR tests load the shipped defaults data file by absolute source path.
target_compile_definitions(litholattice_tests PRIVATE
  LITHOKRAFT_MATERIALS_PATH="${LithoLattice_SOURCE_DIR}/assets/materials/lithokraft.materials")
target_link_libraries(litholattice_tests PRIVATE litholattice_render_vk litholattice_storage litholattice_ai litholattice_sim litholattice_render litholattice_world litholattice_procgen
                      litholattice_ui litholattice_assets litholattice_audio litholattice_input litholattice_net litholattice_script litholattice_mods litholattice_console litholattice_crypto litholattice_index litholattice_mcp litholattice_defrag litholattice_subdiv litholattice_anim litholattice_texture litholattice_core)
litholattice_module_options(litholattice_tests)
# glsl_attr_parse_test.cpp reads the embedded built-in shaders (render's generated header).
target_include_directories(litholattice_tests PRIVATE $<TARGET_PROPERTY:litholattice_render,BINARY_DIR>/generated)
foreach(module core world render sim ai anim storage procgen trees shrubs water litho_formula ui assets audio input net script mods console crypto index mcp defrag subdiv texture)
  add_test(NAME engine_${module} COMMAND litholattice_tests ${module})
  # fix 3b: out-of-tree build dirs cannot find game/ assets (wafer.json) by walking up the cwd.
  set_tests_properties(engine_${module} PROPERTIES ENVIRONMENT "LITHO_SOURCE_ROOT=${LithoLattice_SOURCE_DIR}/..")
endforeach()
# Nemo View (the ASCII picture Little Nemo reads): integer frame builder, identical frames for
# every worker count, the software renderer's object-id buffer. Own executable, a few seconds.
add_executable(nemo_view_test nemo_view_test.cpp)
target_link_libraries(nemo_view_test PRIVATE litholattice_render litholattice_crypto litholattice_core)
litholattice_module_options(nemo_view_test)
add_test(NAME engine_nemo_view COMMAND nemo_view_test)
# Cortana 2026-10-15 1500 (TOOLS, MC-02): these modules write to fixed %TEMP% paths; never run them
# concurrently under parallel ctest.
set_tests_properties(engine_storage engine_assets engine_mods engine_world PROPERTIES RESOURCE_LOCK litho_tempdir)

# Fix run 2 (2026-10-23, R-SIM-1): engine_sim timed out once under parallel load in run 1;
# not reproduced in 6 clean -j8 runs (1.7 s each). Guard: a hang now fails in 300 s with
# ctest's timeout report instead of stalling the whole suite (default 1500 s).
if(TEST engine_sim)
  set_tests_properties(engine_sim PROPERTIES TIMEOUT 300)
endif()

# LithoRoot native suites (review follow-up): the LibC, stdio, Weave,
# Thread, WaferHeap and LithoAbi suites build with gcc on a Linux host
# through lithoroot/toolchain/run-native-suites.sh. On Windows they run
# inside WSL (the default distribution needs gcc/g++). Off by default so
# a machine without WSL is unaffected. wasm_guard reports SKIP when no
# wasm-capable clang is found; ctest shows that as skipped.
option(LITHOROOT_NATIVE_SUITES "Register the LithoRoot native suites with ctest (needs bash+gcc, or WSL on Windows)" OFF)
if(LITHOROOT_NATIVE_SUITES)
  get_filename_component(_lr_root "${LithoLattice_SOURCE_DIR}/lithoroot" ABSOLUTE)
  if(WIN32)
    find_program(LITHOROOT_WSL wsl REQUIRED)
    string(SUBSTRING "${_lr_root}" 0 1 _lr_drive)
    string(TOLOWER "${_lr_drive}" _lr_drive)
    string(SUBSTRING "${_lr_root}" 2 -1 _lr_rest)
    set(_lr_posix "/mnt/${_lr_drive}${_lr_rest}")
    set(_lr_shell ${LITHOROOT_WSL} bash -c)
  else()
    find_program(LITHOROOT_BASH bash REQUIRED)
    set(_lr_posix "${_lr_root}")
    set(_lr_shell ${LITHOROOT_BASH} -c)
  endif()
  foreach(suite waferheap libc_core lithoweave lithothread libc_stdio lithoabi libc_new_delete wasm_guard)
    add_test(NAME lithoroot_${suite}
             COMMAND ${_lr_shell} "cd '${_lr_posix}' && LR_OUT=/tmp/lr-ctest/${suite} bash toolchain/run-native-suites.sh ${suite}")
    set_tests_properties(lithoroot_${suite} PROPERTIES TIMEOUT 600 SKIP_REGULAR_EXPRESSION "SKIP")
  endforeach()
endif()
