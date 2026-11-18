# Assets: resource packs, pack stack, texture registry, block models, shader sources (CPU only;
# depends on core; never includes GL). stb_image is third-party: its own tiny library, built
# without our warning flags.
# LK_WEB: neither third-party decoder is built (JD 2026-11-16 12:41 ET: in-house PNG/SVG
# decoder, its own stage); image.cpp's decode paths fail safe on __wasm__ until then.
if(NOT LK_WEB)
add_library(litholattice_stb STATIC stb_image_impl.c)
target_include_directories(litholattice_stb PUBLIC ${LITHOLATTICE_STB_DIR})

# NanoSVG (Litho SVG rasterizer): third-party, own tiny library without our warning flags.
add_library(litholattice_nanosvg STATIC svg/nanosvg_impl.c)
target_include_directories(litholattice_nanosvg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/svg)
endif()

add_library(litholattice_assets STATIC image.cpp resource_pack.cpp texture_registry.cpp block_model.cpp box_model.cpp mod_manifest.cpp
            mod_signature.cpp resource_loader.cpp shader_library.cpp assets_selftest.cpp)
target_include_directories(litholattice_assets PUBLIC include)
if(NOT LK_WEB)
  target_link_libraries(litholattice_assets PUBLIC litholattice_core PRIVATE litholattice_stb litholattice_nanosvg litholattice_crypto)
else()
  target_link_libraries(litholattice_assets PUBLIC litholattice_core PRIVATE litholattice_crypto)
endif()
litholattice_module_options(litholattice_assets)
