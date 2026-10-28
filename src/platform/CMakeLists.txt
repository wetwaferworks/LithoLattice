# WASM checkpoint (2026-10-18): for the web build the platform target is the
# browser window (window_web.cpp: canvas sizing + HTML5 input backend
# since 2026-10-19, no GLFW); native builds keep the GLFW window + input
# backend exactly as before. (The if(EMSCRIPTEN) condition itself is the
# current toolchain's build switch; it retires with the toolchain
# replacement, not with the source purge.)
if(EMSCRIPTEN)
  add_library(litholattice_platform STATIC window_web.cpp)
  target_include_directories(litholattice_platform PUBLIC include)
  # window_web.cpp builds its GL address table from the loader's X-macro
  # (render/gl33/lk_gl_imports.h).
  target_include_directories(litholattice_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../render/gl33)
  target_link_libraries(litholattice_platform PUBLIC litholattice_input litholattice_core)
else()
  add_library(litholattice_platform STATIC window.cpp glfw_input_backend.cpp)
  target_include_directories(litholattice_platform PUBLIC include)
  target_link_libraries(litholattice_platform PUBLIC litholattice_input litholattice_core PRIVATE glfw Vulkan::Headers)
  if(WIN32)
    target_link_libraries(litholattice_platform PRIVATE dwmapi)  # 2026-10-25: DwmFlush for the GDI software present
    target_sources(litholattice_platform PRIVATE dxgi_present.cpp)  # 2026-10-25: LL_SW_PRESENT=dxgi
    target_link_libraries(litholattice_platform PRIVATE d3d11 dxgi)
  endif()
  target_compile_definitions(litholattice_platform PRIVATE GLFW_INCLUDE_NONE)
endif()
litholattice_module_options(litholattice_platform)
