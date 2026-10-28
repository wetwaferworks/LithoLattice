// Software present path, WebGL2 variant — compiled ONLY for the web build
// (the stock gl33/software_presenter.cpp compiles to an empty TU there;
// natively this file is the empty one). Same factory, same contract, same
// blit as the desktop presenter (see software_presenter.h); the deltas are
// the WebGL2 constraints, each handled explicitly:
//   - Shaders are GLSL ES 3.00 (WebGL2 rejects "#version 330 core" — the
//     exact fault that blacked out the first hardware-spike upload).
//   - GL_BGRA uploads are not core in WebGL2. Since the S1b change
//     (2026-10-28, Archie ruling) this path needs no extension at all:
//     the framebuffer's raw bytes (0xAARRGGBB words = B,G,R,A in memory)
//     upload as GL_RGBA / GL_UNSIGNED_BYTE and the present shader
//     swizzles .bgra when sampling, so the rendered result is identical
//     to the desktop BGRA read with NO CPU swizzle pass and no per-frame
//     error check on the upload.
// The window's WebGL2 context is created and made current by the platform
// layer (window_web.cpp), exactly as GLFW makes the 3.3 context
// current before the desktop presenter is built.
#if defined(__wasm__)

#include "litholattice/render/software_presenter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "gl_loader.h"
#include "gl_program.h"
#include "litholattice/core/log.h"

// Build 59 thin import (declared in gl33/lk_webio.js; the verbs are
// provided directly by the page layer, lkboot.js): the thin present
// verb — target 0 (canvas),
// then the GL names created at init, the sampler uniform name, and the
// texel pointer. One host crossing for the whole upload + draw; contract
// (including the error returns) is documented in lk_webio.js.
extern "C" int lkio_present(int target, int tex, int prog, int vao, const char* uniformName,
                            const uint32_t* pixels, int w, int h, int fbW, int fbH,
                            int internalFormat, int format, int fresh);

// The page-layer A/B switch (?lkthin=off), declared canonically in
// litholattice/platform/lk_web.h and declared locally here like
// lkio_present above: this target does not link the platform library.
extern "C" int lk_present_thin_ok(void);

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
  // S1b (2026-10-28): the upload is the framebuffer's raw bytes read as
  // GL_RGBA, so a sampled texel is (B, G, R, A) in (r, g, b, a); the
  // .bgra swizzle restores (R, G, B, A) — the exact result the desktop
  // presenter's BGRA upload produces, with no CPU swizzle pass.
  oColor = texture(uTex, vec2(vUv.x, 1.0 - vUv.y)).bgra;
}
)";

// S1a (2026-10-28, Archie S1): present-path timing, LL_SW_PROF-gated
// exactly like the software renderer's swprof (one getenv at startup,
// one branch per frame when off — zero cost otherwise). Printed as a
// companion line at the swprof cadence (60-frame windows):
// t_texSubImage_present is the whole upload + draw of one present()
// call; t_cpu_swizzle is reported for shape and is 0.000 by
// construction since S1b (the swizzle lives in the shader now).
struct PresentProf {
  bool on = std::getenv("LL_SW_PROF") != nullptr;
  double uploadMs = 0;
  int frames = 0;
  std::chrono::steady_clock::time_point begin() const {
    return on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  }
  void note(std::chrono::steady_clock::time_point t0) {
    if (!on) return;
    uploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++frames < 60) return;
    std::fprintf(stderr, "swprof present ms/frame: t_texSubImage_present=%.3f t_cpu_swizzle=0.000 (in-shader .bgra, S1b)\n",
                 uploadMs / frames);
    std::fflush(stderr);
    uploadMs = 0;
    frames = 0;
  }
};
PresentProf g_presentProf;

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

    // S1b (2026-10-28): no extension is requested — the raw-bytes RGBA
    // upload plus the shader .bgra swizzle is the only path now.
    logInfo("software present (webgl): upload path: raw RGBA + shader .bgra swizzle (no extension, no CPU swizzle)");
    // Build 59: the thin-import present is the default; ?lkthin=off in
    // the page URL restores the classic per-call path (A/B switch). Read
    // on the browser main thread: this code runs on the proxied pthread,
    // whose `location` is the worker script's URL (the ?debuginput trap).
    thinOk_ = lk_present_thin_ok() != 0;
    logInfo("software present (webgl): thin-import present %s",
            thinOk_ ? "engaged" : "disabled by ?lkthin=off (classic path)");
    return true;
  }

  void present(const uint32_t* pixels, int w, int h, int fbW, int fbH) override {
    if (!pixels || w <= 0 || h <= 0 || fbW <= 0 || fbH <= 0) return;
    const auto profT0 = g_presentProf.begin();  // S1a: t_texSubImage_present window
    const bool fresh = (w != texW_ || h != texH_);
    if (thinOk_) {
      // Build 59 thin import: the whole upload + draw in one host
      // crossing (lk_webio.js) — the same GL sequence and error contract
      // as the classic path below. A positive return is the upload's GL
      // error with nothing drawn; a negative return means the verb
      // cannot run here. Both fall through to the classic path, which
      // owns the frame from that point.
      // S1b: the upload is the framebuffer's raw bytes as GL_RGBA; the
      // present shader's .bgra swizzle restores the channel order (see
      // kPresentFrag), so no CPU swizzle exists on either path.
      const int err = lkio_present(0, (int)tex_, (int)prog_.id(), (int)vao_, "uTex", pixels, w, h, fbW, fbH,
                                   (int)GL_RGBA8, (int)GL_RGBA, fresh ? 1 : 0);
      if (err == 0) {
        texW_ = w;
        texH_ = h;
        g_presentProf.note(profT0);
        return;
      }
      if (err < 0) {
        thinOk_ = false;
        logError("software present (webgl): thin import unavailable — classic present path engaged");
      }
      // err > 0: the verb's upload errored and nothing drew; the classic
      // path below retries the same upload directly. (Pre-S1b this case
      // demoted the BGRA path; with the extension gone there is no
      // second upload mode to demote to.)
    }
    litholattice_glViewport(0, 0, fbW, fbH);
    litholattice_glDisable(GL_DEPTH_TEST);
    litholattice_glDisable(GL_BLEND);
    litholattice_glActiveTexture(GL_TEXTURE0);
    litholattice_glBindTexture(GL_TEXTURE_2D, tex_);

    // Raw bytes as GL_RGBA, fresh and update branches alike (S1b). The
    // per-frame glGetError check the BGRA path needed is gone with the
    // extension dependency — one less sync-class GL call per presented
    // frame on the software page.
    if (fresh) {
      litholattice_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
      litholattice_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    texW_ = w;
    texH_ = h;
    prog_.use();
    if (texLoc_ >= 0) litholattice_glUniform1i(texLoc_, 0);
    litholattice_glBindVertexArray(vao_);
    litholattice_glDrawArrays(GL_TRIANGLES, 0, 3);
    litholattice_glBindVertexArray(0);
    litholattice_glUseProgram(0);
    litholattice_glBindTexture(GL_TEXTURE_2D, 0);
    g_presentProf.note(profT0);
  }

 private:
  gl33::cGlProgram prog_;
  GLint texLoc_ = -1;
  GLuint tex_ = 0, vao_ = 0;
  int texW_ = 0, texH_ = 0;
  bool thinOk_ = false;  // build 59: lkio_present engaged (init decides)
};

}  // namespace

std::unique_ptr<ISoftwarePresenter> createSoftwarePresenter(GLProcLoader loader) {
  if (!vcLoadGL(loader)) return nullptr;
  auto p = std::make_unique<cSoftwarePresenterWebGL>();
  if (!p->init()) return nullptr;
  return p;
}

}  // namespace litholattice

#endif  // defined(__wasm__)
