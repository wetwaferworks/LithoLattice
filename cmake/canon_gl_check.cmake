# Render pipeline M4: GL 3.3 compile check for assets/shaders/canonical_quad_decode.glsl.
# GL drivers have no #include, so splice the header in (as the GL renderer will) and validate
# with glslang as plain #version 330 GLSL. Args: GLSLANG, SRC, HDR, OUT (flattened), STAMP.
file(READ ${SRC} src)
file(READ ${HDR} hdr)
string(REPLACE "#extension GL_GOOGLE_include_directive : require\n" "" src "${src}")
string(REPLACE "#include \"canonical_quad_decode.glsl\"" "${hdr}" src "${src}")
file(WRITE ${OUT} "${src}")
execute_process(COMMAND ${GLSLANG} ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE e)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "canonical_quad_decode.glsl fails GL 3.3 validation:\n${o}${e}")
endif()
file(WRITE ${STAMP} "ok\n")
