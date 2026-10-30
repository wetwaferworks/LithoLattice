# Cortana 2026-10-15 1105: RND-MT AVX2 for software_renderer.cpp
# render: IRenderer / ITextureArray interfaces, BlockMaterials, mesher (public); backends are
# private sources. The gl33 backend embeds assets/shaders as its built-in fallback (generated
# header, rebuilt when a shader file changes).
# Shader directory the gl33 built-ins are embedded from. Native default is the
# engine's own assets/shaders (unchanged). A WASM/web build overrides it with
# the staged GLSL ES 3.00 translations, because the built-in fallback shaders
# are compiled at renderer init and the 330-core dialect cannot compile on
# WebGL2:  -DLITHOLATTICE_SHADER_DIR_PATH=<tree>/wasm/stage/engine-assets/shaders
if(NOT DEFINED LITHOLATTICE_SHADER_DIR_PATH)
  set(LITHOLATTICE_SHADER_DIR_PATH ${LithoLattice_SOURCE_DIR}/assets/shaders)
endif()
set(LITHOLATTICE_SHADER_DIR ${LITHOLATTICE_SHADER_DIR_PATH})
file(GLOB LITHOLATTICE_SHADER_FILES CONFIGURE_DEPENDS ${LITHOLATTICE_SHADER_DIR}/*.vert ${LITHOLATTICE_SHADER_DIR}/*.frag ${LITHOLATTICE_SHADER_DIR}/*.glsl)
set(LITHOLATTICE_BUILTIN_SHADERS ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_shaders.h)
add_custom_command(OUTPUT ${LITHOLATTICE_BUILTIN_SHADERS}
  COMMAND ${CMAKE_COMMAND} -DDIR=${LITHOLATTICE_SHADER_DIR} "-DPATTERNS=*.vert,*.frag,*.glsl" -DOUT=${LITHOLATTICE_BUILTIN_SHADERS}
          -DSTRUCT=BuiltinShaderFile -DARRAY=kBuiltinShaderFiles -P ${LithoLattice_SOURCE_DIR}/cmake/embed_text_files.cmake
  DEPENDS ${LITHOLATTICE_SHADER_FILES} ${LithoLattice_SOURCE_DIR}/cmake/embed_text_files.cmake
  COMMENT "Embedding base shaders")

add_library(litholattice_render STATIC mesher.cpp WorkerScratch.cpp MeshPipeline.cpp MesherSelector.cpp FallbackReasonStats.cpp unified_mesher.cpp binary_greedy_mesher.cpp micro_mesher.cpp block_materials.cpp pbr_materials.cpp pbr_brdf.cpp pbr_quad_mesh.cpp overlay_geometry.cpp hud_ui_renderer.cpp software_renderer.cpp SwDecodeCache.cpp SwCanonStore.cpp software_simd.cpp software_aa.cpp software_ascii.cpp ground_traces.cpp
            gl33/gl_loader.cpp gl33/gl_program.cpp gl33/gl_texture_array.cpp gl33/TexelFreeList.cpp gl33/GlTboSlabPool.cpp gl33/GlTboSlabUploader.cpp gl33/gl33_renderer.cpp gl33/software_presenter.cpp gl33/software_presenter_webgl.cpp gl33/gl_web_shims.cpp
            ${LITHOLATTICE_BUILTIN_SHADERS})
target_include_directories(litholattice_render PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(litholattice_render PUBLIC litholattice_assets litholattice_world litholattice_ui litholattice_core litholattice_subdiv PRIVATE litholattice_systerian)  # world: read-only block data; ui: IUiRenderer; subdiv: micro-face meshing; systerian: ASCII pipeline glyph codec (software_ascii.cpp)
litholattice_module_options(litholattice_render)
# Render pipeline M2: DebugOpt runs the unified-mesher decode-equivalence check.
target_compile_definitions(litholattice_render PRIVATE $<$<CONFIG:DebugOpt>:LL_DEBUGOPT_BUILD=1>)

# Build 59 (web seam, design note Web-Seam-Design-2026-10-20): the
# hand-written thin imports (gl33/lk_webio.js — plain verbs, target 0 =
# canvas present target) are provided directly by the page layer
# (lkboot.js) since the 2026-10-28 purge; no link-time library remains.

# RND perf (2026-10-15, Cortana), reworked 1455 per Elfie R3§6: NO
# translation unit is compiled with /arch:AVX2 or -mavx2 any more, so the
# binary runs on any x86-64 (SSE2) CPU. AVX2 kernels live in
# software_simd_avx2.cpp behind per-function target attributes and are
# selected at runtime by CPUID/XGETBV (software_simd.cpp); LL_SW_FORCE_SCALAR=1
# forces the baseline. -DLL_SW_AVX2=OFF leaves the AVX2 TU out entirely.
# Sim/circuit code is never touched (hash stays put); no FMA contraction.
option(LL_SW_AVX2 "Build the runtime-dispatched AVX2 kernels for the software renderer" ON)
if(LL_SW_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  target_sources(litholattice_render PRIVATE software_simd_avx2.cpp)
  set_property(SOURCE software_simd.cpp APPEND PROPERTY COMPILE_DEFINITIONS LL_SW_AVX2_TU=1)
endif()
# Ginny (Argon) 2026-10-16 Pass 2 step 5 (Cortana): SSE2 (x86-64 baseline) and SSE4.1 kernels,
# one translation unit per ISA, picked at runtime by CPUID (software_simd.cpp). No TU gets an
# arch flag: SSE4.1/AVX2 code sits behind per-function target attributes (GCC/Clang) or plain
# intrinsics (MSVC), so nothing above SSE2 can leak into code that runs before the check.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  target_sources(litholattice_render PRIVATE software_simd_sse2.cpp software_simd_sse41.cpp)
  set_property(SOURCE software_simd.cpp APPEND PROPERTY COMPILE_DEFINITIONS LL_SW_SSE_TU=1)
endif()
# Phone-framerate ignition (2026-10-28 — Archie: optimization design;
# Elfie: co-implementation; JD: direction + debugging): on the wasm
# target the same SSE2/SSE4.1 kernel TUs join the build through
# clang's SSE-to-WASM-SIMD translation. The kernels stay
# bit-identical (same span4.inl, same op order); dispatch treats the
# compile-time __wasm_simd128__ feature as the tier (software_simd.cpp).
# Keyed off the target processor, never the EMSCRIPTEN variable
# (JD's strict rule, 2026-10-28: no Emscripten, ever).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(wasm32|wasm64)$")
  target_sources(litholattice_render PRIVATE software_simd_sse2.cpp software_simd_sse41.cpp)
  set_property(SOURCE software_simd.cpp APPEND PROPERTY COMPILE_DEFINITIONS LL_SW_SSE_TU=1)
  set_property(SOURCE software_simd.cpp software_simd_sse2.cpp software_simd_sse41.cpp
               APPEND PROPERTY COMPILE_OPTIONS -msimd128 -msse4.1)
endif()
