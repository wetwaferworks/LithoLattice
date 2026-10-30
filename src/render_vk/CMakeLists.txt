# render_vk: the Vulkan backend (IRenderer / ITextureArray from render). Vulkan-Headers only; the
# system loader is opened at run time. Shaders: assets/shaders/vk/*.vert|frag (+ the shared
# assets/shaders/chunk_shading.glsl) compiled to SPIR-V at build time by glslang into generated
# headers (spv/<name>_<stage>.h, `const uint32_t kSpv<Name><Stage>[]`).
set(LITHOLATTICE_VK_SHADER_DIR ${LithoLattice_SOURCE_DIR}/assets/shaders/vk)
set(LITHOLATTICE_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/spv)
file(MAKE_DIRECTORY ${LITHOLATTICE_SPV_DIR})
set(LITHOLATTICE_SPV_HEADERS)
foreach(prog chunk hud line sprite particle post)  # JD Vulkan push: + particle, post
  foreach(stage vert frag)
    string(SUBSTRING ${prog} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${prog} 1 -1 rest)
    string(SUBSTRING ${stage} 0 1 sfirst)
    string(TOUPPER ${sfirst} sfirst)
    string(SUBSTRING ${stage} 1 -1 srest)
    set(out ${LITHOLATTICE_SPV_DIR}/${prog}_${stage}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet -I${LithoLattice_SOURCE_DIR}/assets/shaders
              --vn kSpv${first}${rest}${sfirst}${srest} -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/${prog}.${stage}
      DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/${prog}.${stage} ${LITHOLATTICE_VK_SHADER_DIR}/frame.glsl
              ${LithoLattice_SOURCE_DIR}/assets/shaders/chunk_shading.glsl
              ${LITHOLATTICE_VK_SHADER_DIR}/chunk_vert_body.glsl  # 0.0.1.59 [P59-1]
      COMMENT "SPIR-V: vk/${prog}.${stage}"
      VERBATIM)
    list(APPEND LITHOLATTICE_SPV_HEADERS ${out})
  endforeach()
endforeach()

# 0.0.1.59 [P59-1] Packed12 (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the
# Bitmaster and the Bitmasker; layout correction and build by Cortana): the 12-byte chunk vertex shader
# (vertex stage only; it shares vk/chunk_vert_body.glsl and chunk.frag with the 16-byte path).
set(out ${LITHOLATTICE_SPV_DIR}/chunk12_vert.h)
add_custom_command(OUTPUT ${out}
  COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet -I${LithoLattice_SOURCE_DIR}/assets/shaders
          --vn kSpvChunk12Vert -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/chunk12.vert
  DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/chunk12.vert ${LITHOLATTICE_VK_SHADER_DIR}/chunk_vert_body.glsl
          ${LITHOLATTICE_VK_SHADER_DIR}/frame.glsl
  COMMENT "SPIR-V: vk/chunk12.vert"
  VERBATIM)
list(APPEND LITHOLATTICE_SPV_HEADERS ${out})

# Mega-draw V2 (Direction JD, design Archie, implementation Cortana): LITHO_MEGA variants of the chunk shaders
# (per-draw values from an instance buffer instead of push constants).
foreach(mega chunk_mega_vert:kSpvChunkMegaVert:chunk.vert chunk12_mega_vert:kSpvChunk12MegaVert:chunk12.vert
             chunk_mega_frag:kSpvChunkMegaFrag:chunk.frag)
  string(REPLACE ":" ";" parts ${mega})
  list(GET parts 0 name)
  list(GET parts 1 var)
  list(GET parts 2 src)
  set(out ${LITHOLATTICE_SPV_DIR}/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet -DLITHO_MEGA=1 -I${LithoLattice_SOURCE_DIR}/assets/shaders
            --vn ${var} -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/${src}
    DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/${src} ${LITHOLATTICE_VK_SHADER_DIR}/chunk_vert_body.glsl
            ${LITHOLATTICE_VK_SHADER_DIR}/frame.glsl ${LithoLattice_SOURCE_DIR}/assets/shaders/chunk_shading.glsl
    COMMENT "SPIR-V: vk/${src} (LITHO_MEGA)"
    VERBATIM)
  list(APPEND LITHOLATTICE_SPV_HEADERS ${out})
endforeach()
# Mega-draw V2b (Direction JD, design Archie, implementation Cortana): storage-buffer variants (name:var:src:defines).
foreach(v chunk_ssbo_vert:kSpvChunkSsboVert:chunk.vert:LITHO_MEGA_SSBO
          chunk12_ssbo_vert:kSpvChunk12SsboVert:chunk12.vert:LITHO_MEGA_SSBO
          chunk_ssbo_frag:kSpvChunkSsboFrag:chunk.frag:LITHO_MEGA_SSBO
          chunk_ssbo_flat_frag:kSpvChunkSsboFlatFrag:chunk.frag:LITHO_MEGA_SSBO+LITHO_FLAT_STUB
          chunk_ssbovary_vert:kSpvChunkSsboVaryVert:chunk.vert:LITHO_MEGA_SSBO+LITHO_MEGA_VARY
          chunk12_ssbovary_vert:kSpvChunk12SsboVaryVert:chunk12.vert:LITHO_MEGA_SSBO+LITHO_MEGA_VARY
          chunk12_pull_vert:kSpvChunk12PullVert:chunk12.vert:LITHO_MEGA_SSBO+LITHO_PULL)
  string(REPLACE ":" ";" parts ${v})
  list(GET parts 0 name)
  list(GET parts 1 var)
  list(GET parts 2 src)
  list(GET parts 3 defs)
  string(REPLACE "+" ";" defs ${defs})
  list(TRANSFORM defs PREPEND "-D")
  set(out ${LITHOLATTICE_SPV_DIR}/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet ${defs} -I${LithoLattice_SOURCE_DIR}/assets/shaders
            --vn ${var} -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/${src}
    DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/${src} ${LITHOLATTICE_VK_SHADER_DIR}/chunk_vert_body.glsl
            ${LITHOLATTICE_VK_SHADER_DIR}/frame.glsl ${LithoLattice_SOURCE_DIR}/assets/shaders/chunk_shading.glsl
    COMMENT "SPIR-V: vk/${src} (${name})"
    VERBATIM)
  list(APPEND LITHOLATTICE_SPV_HEADERS ${out})
endforeach()

# Render pipeline M4 (Archie Q18/Q19, step 2): shared canonical quad decode header
# (assets/shaders/canonical_quad_decode.glsl). Build-time checks: Vulkan SPIR-V compile of a
# pulling vertex shader, and GL 3.3 validation of the TBO pulling variant (header spliced in).
set(LITHOLATTICE_CANON_GLSL ${LithoLattice_SOURCE_DIR}/assets/shaders/canonical_quad_decode.glsl)
set(out ${LITHOLATTICE_SPV_DIR}/canon_decode_check_vk_vert.h)
add_custom_command(OUTPUT ${out}
  COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet -I${LithoLattice_SOURCE_DIR}/assets/shaders
          --vn kSpvCanonDecodeCheckVkVert -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/canon_decode_check_vk.vert
  DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/canon_decode_check_vk.vert ${LITHOLATTICE_CANON_GLSL}
  COMMENT "SPIR-V: vk/canon_decode_check_vk.vert (canonical decode header)"
  VERBATIM)
list(APPEND LITHOLATTICE_SPV_HEADERS ${out})
set(out ${LITHOLATTICE_SPV_DIR}/canon_decode_check_gl33.stamp)
add_custom_command(OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -DGLSLANG=$<TARGET_FILE:glslang-standalone>
          -DSRC=${LITHOLATTICE_VK_SHADER_DIR}/canon_decode_check_gl33.vert -DHDR=${LITHOLATTICE_CANON_GLSL}
          -DOUT=${LITHOLATTICE_SPV_DIR}/canon_decode_check_gl33.vert -DSTAMP=${out}
          -P ${LithoLattice_SOURCE_DIR}/cmake/canon_gl_check.cmake
  DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/canon_decode_check_gl33.vert ${LITHOLATTICE_CANON_GLSL}
          ${LithoLattice_SOURCE_DIR}/cmake/canon_gl_check.cmake
  COMMENT "GLSL 330: canonical decode header (TBO pulling)"
  VERBATIM)
list(APPEND LITHOLATTICE_SPV_HEADERS ${out})

# Render pipeline M12: the legacy-attribute pull header (canonical_pull_legacy.glsl) used by the
# GL TBO uploader and the Vulkan pull path: GL 3.3 validation (spliced) + SPIR-V compile.
set(LITHOLATTICE_PULL_GLSL ${LithoLattice_SOURCE_DIR}/assets/shaders/canonical_pull_legacy.glsl)
set(out ${LITHOLATTICE_SPV_DIR}/canon_pull_check_gl33.stamp)
add_custom_command(OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -DGLSLANG=$<TARGET_FILE:glslang-standalone>
          -DSRC=${LITHOLATTICE_VK_SHADER_DIR}/canon_pull_check_gl33.vert -DHDR=${LITHOLATTICE_PULL_GLSL}
          -DINC=canonical_pull_legacy.glsl
          -DOUT=${LITHOLATTICE_SPV_DIR}/canon_pull_check_gl33.vert -DSTAMP=${out}
          -P ${LithoLattice_SOURCE_DIR}/cmake/canon_gl_check.cmake
  DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/canon_pull_check_gl33.vert ${LITHOLATTICE_PULL_GLSL}
          ${LithoLattice_SOURCE_DIR}/cmake/canon_gl_check.cmake
  COMMENT "GLSL 330: canonical legacy pull header (GL TBO uploader)"
  VERBATIM)
list(APPEND LITHOLATTICE_SPV_HEADERS ${out})

add_library(litholattice_render_vk STATIC vk_loader.cpp vk_handles.cpp vk_device.cpp vk_resources.cpp vk_vertex_arena.cpp
            vk_swapchain.cpp vk_texture_array.cpp vk_pipelines.cpp vk_renderer.cpp vk_probe.cpp ${LITHOLATTICE_SPV_HEADERS})
target_include_directories(litholattice_render_vk PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(litholattice_render_vk PUBLIC litholattice_render PRIVATE litholattice_core Vulkan::Headers ${CMAKE_DL_LIBS})
# Phase 4.0 (Direction JD, research Elfie, implementation Cortana): streaming / culling counters (logging only).
option(LITHO_STREAM_COUNTERS "Phase 4.0 streaming / culling counters in the Vulkan perf log" ON)
target_compile_definitions(litholattice_render_vk PRIVATE LL_STREAM_COUNTERS=$<BOOL:${LITHO_STREAM_COUNTERS}>)
litholattice_module_options(litholattice_render_vk)
