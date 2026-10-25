# WASM checkpoint (2026-10-18): under EMSCRIPTEN the platform target is the
# browser window (window_emscripten.cpp: canvas sizing + HTML5 input backend
# since 2026-10-19, no GLFW); native builds keep the GLFW window + input
# backend exactly as before.
if(EMSCRIPTEN)
  add_library(litholattice_platform STATIC window_emscripten.cpp)
  target_include_directories(litholattice_platform PUBLIC include)
  target_link_libraries(litholattice_platform PUBLIC litholattice_input litholattice_core)
else()
  add_library(litholattice_platform STATIC window.cpp glfw_input_backend.cpp)
  target_include_directories(litholattice_platform PUBLIC include)
  target_link_libraries(litholattice_platform PUBLIC litholattice_input litholattice_core PRIVATE glfw Vulkan::Headers)
  if(WIN32)
    target_link_libraries(litholattice_platform PRIVATE dwmapi)  # 2026-10-25: DwmFlush for the GDI software present
  endif()
  target_compile_definitions(litholattice_platform PRIVATE GLFW_INCLUDE_NONE)
endif()
litholattice_module_options(litholattice_platform)
