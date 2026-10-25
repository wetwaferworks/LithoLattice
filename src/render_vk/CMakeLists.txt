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

add_library(litholattice_render_vk STATIC vk_loader.cpp vk_handles.cpp vk_device.cpp vk_resources.cpp vk_vertex_arena.cpp
            vk_swapchain.cpp vk_texture_array.cpp vk_pipelines.cpp vk_renderer.cpp vk_probe.cpp ${LITHOLATTICE_SPV_HEADERS})
target_include_directories(litholattice_render_vk PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(litholattice_render_vk PUBLIC litholattice_render PRIVATE litholattice_core Vulkan::Headers ${CMAKE_DL_LIBS})
litholattice_module_options(litholattice_render_vk)
