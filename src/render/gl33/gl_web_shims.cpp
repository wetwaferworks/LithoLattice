// Web-only GL loader shims — see gl_web_shims.h for the rationale.
// The whole translation unit is web-only; native builds compile an
// empty TU and native preprocessing of every other file is unchanged.
#if defined(__wasm__)

#include "gl_web_shims.h"

#include <cstdio>

#include "gl_loader.h"

namespace {

void webTexBuffer(GLenum, GLenum, GLuint) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::fprintf(stdout,  // fix 3b: expected on web, info level
                 "GL loader (web): glTexBuffer shimmed (no buffer textures in "
                 "WebGL2); PBR material table upload is a no-op here\n");
  }
}

void webDrawElementsBaseVertex(GLenum, GLsizei, GLenum, const void*, GLint) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::fprintf(stdout,  // fix 3b: expected on web, info level
                 "GL loader (web): glDrawElementsBaseVertex shimmed (not in "
                 "WebGL2 core); arena draws are dropped — run with "
                 "--gl-arena off on web\n");
  }
}

}  // namespace

bool litholattice_webGlLoadFixups() {
  if (!litholattice_glTexBuffer) litholattice_glTexBuffer = &webTexBuffer;
  if (!litholattice_glDrawElementsBaseVertex)
    litholattice_glDrawElementsBaseVertex = &webDrawElementsBaseVertex;
  // VertexAttribDivisor is core WebGL2 and deliberately never shimmed: if it
  // is somehow unresolved, the completeness check below fails the load loudly.
  bool all = true;
#define LITHOLATTICE_WEB_CHECK(ret, name, args) \
  if (!litholattice_gl##name) all = false;
  LITHOLATTICE_GL_FUNCS(LITHOLATTICE_WEB_CHECK)
#undef LITHOLATTICE_WEB_CHECK
  return all;
}

#endif  // defined(__wasm__)
