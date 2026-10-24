// Software present path, WebGL2 variant — compiled ONLY under Emscripten
// (the stock gl33/software_presenter.cpp compiles to an empty TU there;
// natively this file is the empty one). Same factory, same contract, same
// blit as the desktop presenter (see software_presenter.h); the deltas are
// the WebGL2 constraints, each handled explicitly:
//   - Shaders are GLSL ES 3.00 (WebGL2 rejects "#version 330 core" — the
//     exact fault that blacked out the first hardware-spike upload).
//   - GL_BGRA uploads are not core in WebGL2. At init we try to enable
//     EXT_texture_format_BGRA8888 (zero-swizzle, byte-identical to the
//     desktop path); if the extension or the first BGRA upload fails, we
//     fall back to a CPU swizzle into RGBA and log the switch once. The
//     fallback costs a word-swizzle per texel per frame on this path only
//     — a known, measured-elsewhere delta, never a silent one.
// The window's WebGL2 context is created and made current by the platform
// layer (window_emscripten.cpp), exactly as GLFW makes the 3.3 context
// current before the desktop presenter is built.
#if defined(__EMSCRIPTEN__)

#include "litholattice/render/software_presenter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

#include "gl_loader.h"
#include "gl_program.h"
#include "litholattice/core/log.h"

// Build 59 thin import (gl33/lk_webio.js, linked via --js-library from
// the render CMakeLists): the thin present verb — target 0 (canvas),
// then the GL names created at init, the sampler uniform name, and the
// texel pointer. One host crossing for the whole upload + draw; contract
// (including the error returns) is documented in lk_webio.js.
extern "C" int lkio_present(int target, int tex, int prog, int vao, const char* uniformName,
                            const uint32_t* pixels, int w, int h, int fbW, int fbH,
                            int internalFormat, int format, int fresh);

namespace litholattice {
namespace {

// Fullscreen triangle from gl_VertexID — identical geometry math to the
// desktop presenter; only the dialect header differs.
constexpr const char* kPresentVert = R"(#version 300 es
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kPresentFrag = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() {
  oColor = texture(uTex, vec2(vUv.x, 1.0 - vUv.y));
}
)";

// GL_BGRA is 0x80E1 in desktop GL / GLES extension headers; not named by
// <GLES3/gl3.h>, so spell the value (Emscripten's GL layer maps it onto
// EXT_texture_format_BGRA8888 when that extension is enabled).
constexpr GLenum kFormatBGRA = 0x80E1;

class cSoftwarePresenterWebGL final : public ISoftwarePresenter {
 public:
  ~cSoftwarePresenterWebGL() override {
    if (tex_) litholattice_glDeleteTextures(1, &tex_);
    if (vao_) litholattice_glDeleteVertexArrays(1, &vao_);
  }

  bool init() {
    std::string log;
    std::optional<gl33::cGlProgram> prog = gl33::cGlProgram::build(kPresentVert, kPresentFrag, &log);
    if (!prog) {
      logError("software present (webgl): blit program failed: %s", log.c_str());
      return false;
    }
    prog_ = std::move(*prog);
    texLoc_ = prog_.uniform("uTex");
    litholattice_glGenTextures(1, &tex_);
    litholattice_glGenVertexArrays(1, &vao_);
    if (!tex_ || !vao_) {
      logError("software present (webgl): GL object creation failed");
      return false;
    }
    litholattice_glBindTexture(GL_TEXTURE_2D, tex_);
    // Same sampling contract as the desktop presenter: nearest, clamp.
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    litholattice_glBindTexture(GL_TEXTURE_2D, 0);

    // Zero-swizzle upload requires the BGRA extension; ask for it now so
    // the first frame's path is already decided.
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    bgraOk_ = ctx > 0 && emscripten_webgl_enable_extension(ctx, "EXT_texture_format_BGRA8888") == EM_TRUE;
    logInfo("software present (webgl): BGRA extension %s — upload path: %s",
            bgraOk_ ? "enabled" : "unavailable", bgraOk_ ? "BGRA (no swizzle)" : "RGBA (CPU swizzle)");
    // Build 59: the thin-import present is the default; ?lkthin=off in
    // the page URL restores the classic per-call path (A/B switch). Read
    // on the browser main thread: this code runs on the proxied pthread,
    // whose `location` is the worker script's URL (the ?debuginput trap).
    thinOk_ = MAIN_THREAD_EM_ASM_INT({
      return new URLSearchParams(location.search).get('lkthin') === 'off' ? 0 : 1;
    }) != 0;
    logInfo("software present (webgl): thin-import present %s",
            thinOk_ ? "engaged" : "disabled by ?lkthin=off (classic path)");
    return true;
  }

  void present(const uint32_t* pixels, int w, int h, int fbW, int fbH) override {
    if (!pixels || w <= 0 || h <= 0 || fbW <= 0 || fbH <= 0) return;
    const bool fresh = (w != texW_ || h != texH_);
    if (thinOk_) {
      // Build 59 thin import: the whole upload + draw in one host
      // crossing (lk_webio.js) — the same GL sequence and error contract
      // as the classic path below. A positive return is the upload's GL
      // error with nothing drawn; a negative return means the verb
      // cannot run here. Both fall through to the classic path, which
      // owns the frame from that point (including the BGRA demote).
      const uint32_t* src = pixels;
      GLenum internal = GL_RGBA8, format = kFormatBGRA;
      if (!bgraOk_) {
        scratch_.resize(size_t(w) * size_t(h));
        for (size_t i = 0, n = scratch_.size(); i < n; ++i) {
          const uint32_t p = pixels[i];
          scratch_[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
        src = scratch_.data();
        internal = GL_RGBA;
        format = GL_RGBA;
      }
      const int err = lkio_present(0, (int)tex_, (int)prog_.id(), (int)vao_, "uTex", src, w, h, fbW, fbH,
                                   (int)internal, (int)format, fresh ? 1 : 0);
      if (err == 0) {
        texW_ = w;
        texH_ = h;
        return;
      }
      if (err < 0) {
        thinOk_ = false;
        logError("software present (webgl): thin import unavailable — classic present path engaged");
      } else if (bgraOk_) {
        bgraOk_ = false;
        logError("software present (webgl): BGRA upload rejected by driver — switching to RGBA (CPU swizzle)");
      }
    }
    litholattice_glViewport(0, 0, fbW, fbH);
    litholattice_glDisable(GL_DEPTH_TEST);
    litholattice_glDisable(GL_BLEND);
    litholattice_glActiveTexture(GL_TEXTURE0);
    litholattice_glBindTexture(GL_TEXTURE_2D, tex_);

    if (bgraOk_) {
      // 0xAARRGGBB texels are B,G,R,A bytes in memory — same read as desktop.
      if (fresh) {
        litholattice_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, kFormatBGRA, GL_UNSIGNED_BYTE, pixels);
      } else {
        litholattice_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, kFormatBGRA, GL_UNSIGNED_BYTE, pixels);
      }
      if (litholattice_glGetError() != 0 /* GL_NO_ERROR */) {
        // The extension claimed support but the driver refused the upload:
        // demote permanently, say so once, and redo this frame swizzled.
        bgraOk_ = false;
        logError("software present (webgl): BGRA upload rejected by driver — switching to RGBA (CPU swizzle)");
      } else {
        texW_ = w;
        texH_ = h;
      }
    }
    if (!bgraOk_) {
      scratch_.resize(size_t(w) * size_t(h));
      for (size_t i = 0, n = scratch_.size(); i < n; ++i) {
        const uint32_t p = pixels[i];
        scratch_[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
      }
      if (fresh) {
        litholattice_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch_.data());
      } else {
        litholattice_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, scratch_.data());
      }
      texW_ = w;
      texH_ = h;
    }
    prog_.use();
    if (texLoc_ >= 0) litholattice_glUniform1i(texLoc_, 0);
    litholattice_glBindVertexArray(vao_);
    litholattice_glDrawArrays(GL_TRIANGLES, 0, 3);
    litholattice_glBindVertexArray(0);
    litholattice_glUseProgram(0);
    litholattice_glBindTexture(GL_TEXTURE_2D, 0);
  }

 private:
  gl33::cGlProgram prog_;
  GLint texLoc_ = -1;
  GLuint tex_ = 0, vao_ = 0;
  int texW_ = 0, texH_ = 0;
  bool bgraOk_ = false;
  bool thinOk_ = false;  // build 59: lkio_present engaged (init decides)
  std::vector<uint32_t> scratch_;
};

}  // namespace

std::unique_ptr<ISoftwarePresenter> createSoftwarePresenter(GLProcLoader loader) {
  if (!vcLoadGL(loader)) return nullptr;
  auto p = std::make_unique<cSoftwarePresenterWebGL>();
  if (!p->init()) return nullptr;
  return p;
}

}  // namespace litholattice

#endif  // defined(__EMSCRIPTEN__)
