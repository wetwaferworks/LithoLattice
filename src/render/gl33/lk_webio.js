// lk_webio.js — hand-written thin imports for the LithoKraft web seam
// (build 59; design note Web-Seam-Design-2026-10-20, JD 2026-10-20).
//
// The surface is deliberately minimal — plain thin imports: every verb
// takes a small target index as its first argument (0 is the page's
// canvas present target) followed by plain scalars and pointers, returns
// an int status, and does its whole job in ONE host crossing. The
// implementations are these browser shims, linked via --js-library (see
// src/render/CMakeLists.txt). (An earlier draft shaped this surface
// after WASI fds; JD ruled that shaping dropped 2026-10-20 — it carried
// no runtime cost and added nothing the plain surface doesn't.)
//
// Why this file exists: the stock Emscripten GL glue is a general
// emulation of the whole GL state machine, and every call crosses the
// C++/JS boundary with full argument marshaling. A present is ~15 such
// crossings. The verb below performs the identical GL call sequence
// inside one JS function, reading the same GL registry the stock glue
// maintains (GL.currentContext / GL.textures / GL.programs / GL.vaos),
// so the observable GL state transitions are the stock ones — only the
// boundary traffic changes.
//
// Contract of lkio_present (used by software_presenter_webgl.cpp):
//   target 0; tex/prog/vao are the GL names the C++ side created through the
//   stock path at init; uniformName is the sampler uniform to pin to
//   texture unit 0; pixels points at w*h 32-bit texels in WASM memory.
//   Returns 0 after a successful upload + draw; >0 returns the GL error
//   code from the upload WITHOUT drawing (the caller owns the retry
//   policy — e.g. the BGRA demote); -1 when the verb cannot run at all
//   (no current context / unknown GL object) — the caller then falls
//   back to the classic per-call path permanently.
addToLibrary({
  // Phase 2 mega-draw verbs (gl33_renderer.cpp, Emscripten only):
  // WEBGL_multi_draw entry points are extension functions, absent
  // from the GL proc-address table, so probe + draw travel as thin
  // verbs. Probe honestly reports the extension; draw performs the
  // multi-draw, or the identical sequence of rebased draws when the
  // extension is missing (the arena EBO's indices already carry the
  // base vertex, so the pixels are the same either way). On the thin
  // runtime lkboot registers its own probe (0 — the thin GL command
  // stream has no multi-draw op yet), so this draw is reached only
  // in glue builds. (Landing completed 2026-10-28 for the phone
  // build — Elfie; Phase 2 design: JD/Cortana; Archie: direction.)
  lk_mega_probe__sig: 'i',
  lk_mega_probe: function() {
    try {
      var ctx = (typeof GL !== 'undefined' && GL.currentContext) ? GL.currentContext.GLctx : null;
      if (!ctx) return 0;
      return ctx.getExtension('WEBGL_multi_draw') ? 1 : 0;
    } catch (e) {
      return 0;
    }
  },
  lk_mega_draw__sig: 'iiiii',
  lk_mega_draw: function(mode, countsPtr, offsetsPtr, drawCount) {
    try {
      var ctx = (typeof GL !== 'undefined' && GL.currentContext) ? GL.currentContext.GLctx : null;
      if (!ctx) return -1;
      var counts = HEAP32.subarray(countsPtr >> 2, (countsPtr >> 2) + drawCount);
      var offsets = HEAP32.subarray(offsetsPtr >> 2, (offsetsPtr >> 2) + drawCount);
      var ext = ctx.getExtension('WEBGL_multi_draw');
      if (ext) {
        ext.multiDrawElementsWEBGL(mode, counts, 0, ctx.UNSIGNED_INT, offsets, 0, drawCount);
      } else {
        for (var i = 0; i < drawCount; ++i) ctx.drawElements(mode, counts[i], ctx.UNSIGNED_INT, offsets[i]);
      }
      return 0;
    } catch (e) {
      return -1;
    }
  },
  lkio_present__sig: 'iiiiiiiiiiiiii',
  lkio_present: function(target, tex, prog, vao, namePtr, ptr, w, h, fbW, fbH, internalFormat, format, fresh) {
    if (target !== 0) return -1;
    try {
      var ctx = (typeof GL !== 'undefined' && GL.currentContext) ? GL.currentContext.GLctx : null;
      if (!ctx) return -1;
      var texObj = GL.textures[tex];
      var progObj = GL.programs[prog];
      var vaoObj = GL.vaos[vao];
      if (!texObj || !progObj || !vaoObj) return -1;
      var gl = ctx;
      gl.viewport(0, 0, fbW, fbH);
      gl.disable(gl.DEPTH_TEST);
      gl.disable(gl.BLEND);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, texObj);
      var data = HEAPU8.subarray(ptr, ptr + w * h * 4);
      if (fresh) {
        gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, w, h, 0, format, gl.UNSIGNED_BYTE, data);
      } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, format, gl.UNSIGNED_BYTE, data);
      }
      var err = gl.getError();
      if (err !== 0) return err;
      gl.useProgram(progObj);
      var loc = gl.getUniformLocation(progObj, UTF8ToString(namePtr));
      if (loc) gl.uniform1i(loc, 0);
      gl.bindVertexArray(vaoObj);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      gl.bindVertexArray(null);
      gl.useProgram(null);
      gl.bindTexture(gl.TEXTURE_2D, null);
      return 0;
    } catch (e) {
      return -1;
    }
  },
});
