#include "gl_loader.h"
#include <cstdio>
#include <cstring>

#if defined(__EMSCRIPTEN__)
#include "gl_web_shims.h"
#endif

#define LITHOLATTICE_DEFINE(ret, name, args) PFN_litholattice_gl##name litholattice_gl##name = nullptr;
LITHOLATTICE_GL_FUNCS(LITHOLATTICE_DEFINE)
#undef LITHOLATTICE_DEFINE

// fix 3b (Elfie P3): WebGL2 has no glTexBuffer / glDrawElementsBaseVertex; the web shims cover
// them, so on web those two are expected (info on stdout), not errors.
static void reportMissingGl(const char* name) {
#if defined(__EMSCRIPTEN__)
  if (!std::strcmp(name, "TexBuffer") || !std::strcmp(name, "DrawElementsBaseVertex")) {
    std::printf("GL loader (web): gl%s is not in WebGL2; using the web shim (expected)\n", name);
    return;
  }
#endif
  std::fprintf(stderr, "GL loader: missing gl%s\n", name);
}

bool vcLoadGL(void* (*loader)(const char*)) {
  bool ok = true;
#define LITHOLATTICE_LOAD(ret, name, args)                                                     \
  litholattice_gl##name = reinterpret_cast<PFN_litholattice_gl##name>(loader("gl" #name)); \
  if (!litholattice_gl##name) { reportMissingGl(#name); ok = false; }
  LITHOLATTICE_GL_FUNCS(LITHOLATTICE_LOAD)
#undef LITHOLATTICE_LOAD
#if defined(__EMSCRIPTEN__)
  // WebGL2 (GLES 3.0) cannot provide the GLES 3.1 entry points in the table;
  // install their logged shims and re-check completeness (gl_web_shims.h).
  if (!ok) ok = litholattice_webGlLoadFixups();
#endif
  return ok;
}
