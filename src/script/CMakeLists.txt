# script: sandboxed AngelScript engine. Depends on core only; the game registers its API.
set(LITHOLATTICE_AS_STRING ${angelscript_SOURCE_DIR}/add_on/scriptstdstring/scriptstdstring.cpp)
# LK_WEB (2026-11-16): compile a patched copy (regexFind = WEB-ONLY FAIL-SAFE STUB, no <regex>);
# see cmake/patch_as_stdstring_web.cmake. Native builds keep the pinned add-on unchanged.
if(LK_WEB)
  set(_lk_as_web ${CMAKE_CURRENT_BINARY_DIR}/web_addon/scriptstdstring)
  file(MAKE_DIRECTORY ${_lk_as_web})
  configure_file(${angelscript_SOURCE_DIR}/add_on/scriptstdstring/scriptstdstring.h ${_lk_as_web}/scriptstdstring.h COPYONLY)
  execute_process(COMMAND ${CMAKE_COMMAND} -DIN=${LITHOLATTICE_AS_STRING} -DOUT=${_lk_as_web}/scriptstdstring.cpp
                          -P ${LithoLattice_SOURCE_DIR}/cmake/patch_as_stdstring_web.cmake
                  RESULT_VARIABLE _lk_as_rc)
  if(NOT _lk_as_rc EQUAL 0)
    message(FATAL_ERROR "web configure: patching scriptstdstring.cpp failed (${_lk_as_rc})")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LITHOLATTICE_AS_STRING} ${LithoLattice_SOURCE_DIR}/cmake/patch_as_stdstring_web.cmake)
  set(LITHOLATTICE_AS_STRING ${_lk_as_web}/scriptstdstring.cpp)
endif()
add_library(litholattice_script STATIC script_engine.cpp script_binding_allowlist.cpp script_memory_hook.cpp suspension_policy.cpp ${LITHOLATTICE_AS_STRING})
target_include_directories(litholattice_script PUBLIC include ${angelscript_SOURCE_DIR}/add_on)
# script_memory_internal.h lives next to the sources (engine-internal, not installed).
target_include_directories(litholattice_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(litholattice_script PUBLIC litholattice_core litholattice_mods angelscript)
litholattice_module_options(litholattice_script)
# Third-party add-on: build it quietly (our warning level is for our code).
set_source_files_properties(${LITHOLATTICE_AS_STRING} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
