#include "gl_program.h"

namespace litholattice::gl33 {
namespace {

#if defined(__wasm__)
// Mobile WebGL shader pipeline (Elfie, 2026-10-28; JD assigned the
// mobile lane and ordered the pipeline: "whatever's stopping the
// picture from drawing, we need that"). The engine's shader sources
// (built-in files and pack overrides alike) are desktop GLSL 330;
// WebGL2 compiles GLSL ES 3.00 only, so a desktop source dies on the
// version line ('core' is not an ES profile) and everything after it
// is parsed as ES 1.00. This stage rewrites a 330 source to ES 3.00
// at the one seam every program passes through. The dialects are
// near-identical for what these shaders use (layout locations, in /
// out, flat, uint attributes, texelFetch, textureGrad are all ES
// 3.00 native), so the rewrite is deliberately small:
//   - "#version 330 core" -> "#version 300 es" (first line kept
//     first; the mega-draw variant's injected #define follows it
//     and is preserved).
//   - Fragment shaders gain default precision declarations after
//     the leading directive block: ES fragment shaders have no
//     default float precision, and sampler2DArray gets highp so
//     the layer coordinate survives low-end drivers.
//   - texture2D( -> texture( for pack shaders written pre-330.
// Sources without a 330 version line (the presenter's hand-written
// ES 3.00, anything versionless) pass through untouched. Desktop
// builds never see this code.
std::string toGlslEs300(const std::string& src, GLenum type) {
  const size_t vpos = src.find("#version");
  if (vpos == std::string::npos || vpos > 64) return src;
  const size_t vend = src.find('\n', vpos);
  if (vend == std::string::npos) return src;
  if (src.compare(vpos, 11, "#version 33") != 0) return src;  // not a 330 source
  std::string out = "#version 300 es\n";
  // Skip the rest of the leading directive block (injected #define
  // lines from the mega-draw variant) so the precision block lands
  // after it, still ahead of every declaration.
  size_t cursor = vend + 1;
  for (;;) {
    const size_t eol = src.find('\n', cursor);
    const size_t len = (eol == std::string::npos ? src.size() : eol) - cursor;
    if (len > 0 && src[cursor] == '#') {
      out.append(src, cursor, len + (eol == std::string::npos ? 0 : 1));
      if (eol == std::string::npos) { cursor = src.size(); break; }
      cursor = eol + 1;
    } else break;
  }
  if (type == GL_FRAGMENT_SHADER) {
    out += "precision highp float;\nprecision highp int;\nprecision highp sampler2DArray;\n";
  }
  out.append(src, cursor, std::string::npos);
  // Pre-330 spelling that pack shaders may still carry.
  for (size_t p = out.find("texture2D("); p != std::string::npos; p = out.find("texture2D(", p))
    out.replace(p, 10, "texture(");
  return out;
}
#endif  // defined(__wasm__)

GLuint compile(GLenum type, const std::string& srcIn, std::string* log) {
#if defined(__wasm__)
  const std::string src = toGlslEs300(srcIn, type);
#else
  const std::string& src = srcIn;
#endif
  const GLuint s = glCreateShader(type);
  const char* text = src.c_str();
  glShaderSource(s, 1, &text, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok) return s;
  char buf[2048] = {};
  glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
  if (log) *log += std::string(type == GL_VERTEX_SHADER ? "vertex: " : "fragment: ") + buf;
  glDeleteShader(s);
  return 0;
}

}  // namespace

cGlProgram::~cGlProgram() {
  if (id_) glDeleteProgram(id_);
}

cGlProgram& cGlProgram::operator=(cGlProgram&& o) noexcept {
  if (this != &o) {
    if (id_) glDeleteProgram(id_);
    id_ = o.id_;
    o.id_ = 0;
  }
  return *this;
}

std::optional<cGlProgram> cGlProgram::build(const std::string& vertex, const std::string& fragment, std::string* log) {
  const GLuint vs = compile(GL_VERTEX_SHADER, vertex, log);
  const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, log);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return std::nullopt;
  }
  cGlProgram p(glCreateProgram());
  glAttachShader(p.id_, vs);
  glAttachShader(p.id_, fs);
  glLinkProgram(p.id_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p.id_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char buf[2048] = {};
    glGetProgramInfoLog(p.id_, sizeof buf, nullptr, buf);
    if (log) *log += std::string("link: ") + buf;
    return std::nullopt;
  }
  return p;
}

}  // namespace litholattice::gl33
