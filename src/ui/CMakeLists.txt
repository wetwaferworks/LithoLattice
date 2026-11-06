# ui: Litho GUI — the ILithoGui / cLithoGui retained-mode widget toolkit (widget tree,
# layout, themes, input routing, draw-list output). Renderer-agnostic: back ends consume
# the lithogui::DrawList; clipboard and script calls stay with the host.
# Litho GUI subsystem version: versionName 0.1, versionCode 27 (Build 27 state:
# universal Frame inheritance, 29 widget classes). This version is the GUI's own and
# is deliberately separate from the LithoLattice engine build counter.
add_library(litholattice_ui STATIC litho_gui.cpp litho_font.cpp json.cpp markdown.cpp litho_gui_selftest.cpp)
target_include_directories(litholattice_ui PUBLIC include)
target_link_libraries(litholattice_ui PUBLIC litholattice_core)
litholattice_module_options(litholattice_ui)
