#include "gl_loader.h"
#include <cstdio>

#if defined(__EMSCRIPTEN__)
#include "gl_web_shims.h"
#endif

#define LITHOLATTICE_DEFINE(ret, name, args) PFN_litholattice_gl##name litholattice_gl##name = nullptr;
LITHOLATTICE_GL_FUNCS(LITHOLATTICE_DEFINE)
#undef LITHOLATTICE_DEFINE

bool vcLoadGL(void* (*loader)(const char*)) {
  bool ok = true;
#define LITHOLATTICE_LOAD(ret, name, args)                                                     \
  litholattice_gl##name = reinterpret_cast<PFN_litholattice_gl##name>(loader("gl" #name)); \
  if (!litholattice_gl##name) { std::fprintf(stderr, "GL loader: missing gl%s\n", #name); ok = false; }
  LITHOLATTICE_GL_FUNCS(LITHOLATTICE_LOAD)
#undef LITHOLATTICE_LOAD
#if defined(__EMSCRIPTEN__)
  // WebGL2 (GLES 3.0) cannot provide the GLES 3.1 entry points in the table;
  // install their logged shims and re-check completeness (gl_web_shims.h).
  if (!ok) ok = litholattice_webGlLoadFixups();
#endif
  return ok;
}
