// Software renderer test: renders a REAL quad through renderMesh() and
// verifies the rasterizer's output pixels, then saves a PPM for eyeballing.
// (P3-3, Appendix D L8: the old body built no geometry at all — the
// rasterizer calls were commented out — and "passed" by writing a PPM of
// a cleared buffer, proving only framebuffer allocation. Real coverage
// also lives in engine_tests (software sections) and the sprite host
// selftest; this TU is the standalone visual smoke test.)
#include "litholattice/render/software_test.h"
#include "litholattice/render/model_dither.h"
#include "litholattice/render/software_aa.h"
#include "litholattice/render/software_ascii.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/render/software_simd.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "litholattice/render/ground_traces.h"
#include "litholattice/render/trace_ramps.h"  // Cortana 2026-10-15 1625: Trace-mix pulse test

namespace litholattice {

// Minimal PPM writer (P6, viewable anywhere; the historical name says PNG
// but the file was always a PPM — kept honest now).
static bool writePPM(const char* path, const SoftwareFramebuffer& fb) {
  FILE* ppm = std::fopen(path, "wb");
  if (!ppm) return false;
  std::fprintf(ppm, "P6\n%d %d\n255\n", fb.width, fb.height);
  for (uint32_t y = 0; y < fb.height; ++y) {
    for (uint32_t x = 0; x < fb.width; ++x) {
      uint32_t p = fb.pixels[size_t(y) * fb.width + x];
      uint8_t rgb[3] = {uint8_t((p >> 16) & 0xFF), uint8_t((p >> 8) & 0xFF), uint8_t(p & 0xFF)};
      std::fwrite(rgb, 1, 3, ppm);
    }
  }
  std::fclose(ppm);
  return true;
}

int testSoftwareRenderer() {
  cSoftwareRenderer renderer;
  renderer.resize(400, 300);
  // The legacy pixel assertions below predate model dithering (JD
  // 2026-10-13) and use an ortho MVP whose clip w = 1 sits inside the
  // default fade band; disable the fade for them (the dither-specific
  // checks at the end of this test exercise it explicitly).
  renderer.setModelDither(false);

  // One quad through the real path: PackedVertex data + MVP, exactly as
  // renderFrame feeds stored chunk meshes. MVP maps block units [0,16]
  // to NDC [-1,1] on x/y, z passthrough, w = 1. The quad spans the full
  // 16x16 blocks at z = 0.5, top face (face 2 = faceColor's unshaded
  // face), wound in the mesher convention; value 200 -> flat gray 200.
  float mvp[16] = {};
  mvp[0] = 0.125f; mvp[5] = 0.125f; mvp[10] = 1.f;
  mvp[12] = -1.f; mvp[13] = -1.f; mvp[15] = 1.f;
  PackedVertex q[4] = {};
  const uint16_t c[4][3] = {{0, 0, 8}, {256, 0, 8}, {256, 256, 8}, {0, 256, 8}};
  for (int i = 0; i < 4; ++i) {
    q[i].x = c[i][0]; q[i].y = c[i][1]; q[i].z = c[i][2];
    q[i].faceAo = 2;    // top face
    q[i].value = 200;   // flat luminance
    q[i].mode = kShadeTex;
  }
  const uint32_t kClear = 0xFF1a1a2e;  // dark blue background
  renderer.clear(kClear);
  renderer.renderMesh(q, 4, mvp);

  const SoftwareFramebuffer& fb = renderer.framebuffer();
  size_t covered = 0;
  for (uint32_t p : fb.pixels)
    if (p != kClear) ++covered;
  const uint32_t center = fb.pixels[size_t(fb.height / 2) * fb.width + fb.width / 2];
  std::printf("Software renderer test: %dx%d framebuffer, %zu covered pixels, center 0x%08x\n",
              fb.width, fb.height, covered, center);

  // The quad covers the whole frame: nearly every pixel is the flat gray,
  // and the center pixel is exactly faceColor(2, 200, kShadeTex).
  if (covered < size_t(fb.width) * fb.height * 9 / 10) {
    std::printf("FAIL: quad covered only %zu pixels\n", covered);
    return 1;
  }
  if (center != 0xFFC8C8C8u) {
    std::printf("FAIL: center pixel 0x%08x, expected 0xffc8c8c8\n", center);
    return 1;
  }

  // --- Model dithering (JD 2026-10-13) -------------------------------------
  // Math: fade endpoints + IGN determinism/range/distribution.
  if (modelDitherFade(2.f, 2.f, 8.f) != 0.f || modelDitherFade(8.f, 2.f, 8.f) != 1.f ||
      modelDitherFade(5.f, 2.f, 8.f) != 0.5f) {
    std::printf("FAIL: modelDitherFade endpoints/midpoint wrong\n");
    return 1;
  }
  {
    double sum = 0;
    float mn = 1.f, mx = 0.f;
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) {
        const float v = modelDitherIgn(x, y);
        if (v != modelDitherIgn(x, y)) {  // same pixel, same value, always
          std::printf("FAIL: modelDitherIgn not deterministic at (%d,%d)\n", x, y);
          return 1;
        }
        if (v < 0.f || v >= 1.f) {
          std::printf("FAIL: modelDitherIgn out of [0,1): %f\n", v);
          return 1;
        }
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
      }
    const double mean = sum / (64.0 * 64.0);
    if (mean < 0.4 || mean > 0.6 || mn > 0.05f || mx < 0.95f) {
      std::printf("FAIL: modelDitherIgn distribution off (mean %.3f, min %.3f, max %.3f)\n",
                  mean, mn, mx);
      return 1;
    }
  }
  // Rasterizer: same quad, MVP w = 4 (constant view depth 4 blocks).
  float mvpFar[16];
  std::memcpy(mvpFar, mvp, sizeof mvp);
  mvpFar[15] = 4.f;
  auto renderCount = [&](bool on, float nearB, float farB) {
    renderer.setModelDither(on, nearB, farB);
    renderer.clear(kClear);
    renderer.renderMesh(q, 4, mvpFar);
    size_t n = 0;
    for (uint32_t p : renderer.framebuffer().pixels)
      if (p != kClear) ++n;
    return n;
  };
  const size_t fullCount = renderCount(false, 2.f, 8.f);
  if (fullCount == 0) {
    std::printf("FAIL: dither test setup: quad vanished with dither off\n");
    return 1;
  }
  if (renderCount(true, 8.f, 16.f) != 0) {  // depth 4 <= near: fully faded
    std::printf("FAIL: dither: quad at depth 4 survived band 8..16\n");
    return 1;
  }
  if (renderCount(true, 0.5f, 2.f) != fullCount) {  // depth 4 >= far: untouched
    std::printf("FAIL: dither: quad at depth 4 changed under band 0.5..2\n");
    return 1;
  }
  const size_t midCount = renderCount(true, 2.f, 8.f);  // fade(4) = 1/3 kept
  if (midCount == 0 || midCount >= fullCount ||
      midCount < fullCount / 5 || midCount > fullCount / 2) {
    std::printf("FAIL: dither: mid-band coverage %zu of %zu (expected ~1/3)\n",
                midCount, fullCount);
    return 1;
  }
  renderer.setModelDither(false);
  std::printf("Model dither checks: fade/IGN OK, mid-band coverage %zu/%zu\n",
              midCount, fullCount);

  // --- AA ladder (JD 2026-10-13) -------------------------------------------
  // Mode name roundtrip + unknown names rejected to a stable state.
  {
    SoftwareAaMode m;
    if (!softwareAaModeFromName("cmaa", m) || m != SoftwareAaMode::Cmaa ||
        !softwareAaModeFromName("fxaa", m) || m != SoftwareAaMode::Fxaa ||
        !softwareAaModeFromName("ssaa2x", m) || m != SoftwareAaMode::Ssaa2x ||
        !softwareAaModeFromName("off", m) || m != SoftwareAaMode::Off ||
        softwareAaModeFromName("banana", m)) {
      std::printf("FAIL: AA mode name roundtrip\n");
      return 1;
    }
    if (std::string(softwareAaModeName(SoftwareAaMode::Cmaa)) != "cmaa") {
      std::printf("FAIL: AA mode name for Cmaa\n");
      return 1;
    }
  }
  // Synthetic edge scene: left half white, right half black, one step edge;
  // a post pass must (a) leave flat interiors bit-untouched, (b) soften the
  // step (produce at least one intermediate value at the boundary).
  auto edgeScene = []() {
    SoftwareFramebuffer f;
    f.resize(32, 16);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x)
        f.pixels[size_t(y) * 32 + size_t(x)] = x < 16 ? 0xFFFFFFFFu : 0xFF000000u;
    return f;
  };
  // Diagonal scene for CMAA: a boundary marching one pixel every two rows
  // forces run-end turns, which is where restored coverage blends pixels.
  // (A perfectly axis-aligned edge has exact 0/1 coverage and correctly
  // blends NOTHING — checked explicitly below.)
  auto checkPost = [&](const char* name, bool useCmaa) {
    SoftwareFramebuffer f;
    f.resize(32, 16);
    std::vector<float> depth(size_t(32) * 16, 0.4f);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x) {
        const bool white = useCmaa ? (x < 8 + (y >> 1)) : (x < 16);
        f.pixels[size_t(y) * 32 + size_t(x)] = white ? 0xFFFFFFFFu : 0xFF000000u;
        if (!white) depth[size_t(y) * 32 + size_t(x)] = 0.6f;
      }
    // 2026-10-26: depth API is uint16 (PT4-A); quantize with the shared rule.
    std::vector<uint16_t> depthQv(depth.size());
    for (size_t i = 0; i < depth.size(); ++i) depthQv[i] = swsimd::depthQ(depth[i]);
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    if (useCmaa)
      applyCmaaPost(f, depthQv.data(), scratch);
    else
      applyFxaaPost(f, scratch, depthQv.data());
    // Flat interiors untouched (columns far from the boundary).
    for (int y = 0; y < 16; ++y)
      for (int x : {0, 2, 29, 31})
        if (f.pixels[size_t(y) * 32 + size_t(x)] != before[size_t(y) * 32 + size_t(x)]) {
          std::printf("FAIL: %s post changed flat interior at (%d,%d)\n", name, x, y);
          return false;
        }
    // At least one intermediate tone near the boundary.
    bool intermediate = false;
    for (size_t i = 0; i < f.pixels.size() && !intermediate; ++i) {
      const uint32_t p = f.pixels[i];
      if (p != 0xFFFFFFFFu && p != 0xFF000000u) intermediate = true;
    }
    if (!intermediate) {
      std::printf("FAIL: %s post left the edge binary\n", name);
      return false;
    }
    return true;
  };
  // CMAA conservatism: an axis-aligned edge is exact — no blend at all.
  {
    SoftwareFramebuffer f = edgeScene();
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    std::vector<uint16_t> depth(size_t(32) * 16, swsimd::depthQ(0.5f));  // 2026-10-26: uint16 depth API
    applyCmaaPost(f, depth.data(), scratch);
    if (f.pixels != before) {
      std::printf("FAIL: cmaa blended an exactly-aligned edge (not conservative)\n");
      return 1;
    }
  }
  if (!checkPost("fxaa", false)) return 1;
  if (!checkPost("cmaa", true)) return 1;
  // SSAA resolve: exact 2x2 average.
  {
    SoftwareFramebuffer hi, out;
    hi.resize(4, 4);
    out.resize(2, 2);
    for (auto& p : hi.pixels) p = 0xFF000000u;
    hi.pixels[0] = 0xFFFFFFFFu;  // one white in the first 2x2 -> 25% gray
    resolveSsaa2x(hi, out);
    const uint32_t expect = 0xFF000000u | (64u << 16) | (64u << 8) | 64u;
    if (out.pixels[0] != expect || out.pixels[1] != 0xFF000000u) {
      std::printf("FAIL: ssaa2x resolve average wrong (%08x)\n", out.pixels[0]);
      return 1;
    }
  }
  std::printf("AA ladder checks: names OK, fxaa/cmaa soften edges + keep flats, ssaa resolve OK\n");

  // --- AA, JD 2026-11-17: geometry-only FXAA, pinned hashes, thread determinism ---
  auto hashPixels = [](const std::vector<uint32_t>& px) -> uint64_t {
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    for (uint32_t p : px) {
      for (int i = 0; i < 4; ++i) {
        h ^= uint64_t((p >> (8 * i)) & 0xFF);
        h *= 1099511628211ull;
      }
    }
    return h;
  };
  auto paintTexturedFlat = [](SoftwareFramebuffer& f, std::vector<uint16_t>& depthQ,
                              std::vector<uint16_t>& prim, bool stepAtMid) {
    const int w = int(f.width), h = int(f.height);
    depthQ.assign(size_t(w) * h, 0);
    prim.assign(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        // 4x4 pixel-art checker (high luma contrast) on a flat surface.
        const int cx = (x / 4) & 1, cy = (y / 4) & 1;
        const uint32_t lo = 0xFF2060A0u, hi = 0xFFE0C040u;
        f.pixels[size_t(y) * w + size_t(x)] = (cx ^ cy) ? hi : lo;
        const bool far = stepAtMid && x >= w / 2;
        depthQ[size_t(y) * w + size_t(x)] = far ? uint16_t(40000) : uint16_t(20000);
        prim[size_t(y) * w + size_t(x)] = far ? uint16_t(2) : uint16_t(1);
      }
  };
  {
    SoftwareFramebuffer f;
    f.resize(64, 32);
    std::vector<uint16_t> depthQ, prim;
    paintTexturedFlat(f, depthQ, prim, false);  // one surface, high-contrast texture
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    applyFxaaPost(f, scratch, depthQ.data(), prim.data());
    if (f.pixels != before) {
      std::printf("FAIL: fxaa touched a flat textured interior\n");
      return 1;
    }
    // Geometry step down the middle must soften at least one boundary pixel
    // and still leave the far interiors (columns 0-4 and 59-63) untouched.
    paintTexturedFlat(f, depthQ, prim, true);
    const auto before2 = f.pixels;
    applyFxaaPost(f, scratch, depthQ.data(), prim.data());
    bool changedInterior = false, softened = false;
    const int w = 64, h = 32;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const uint32_t a = before2[size_t(y) * w + size_t(x)];
        const uint32_t b = f.pixels[size_t(y) * w + size_t(x)];
        if (a == b) continue;
        if (x <= 4 || x >= w - 5) changedInterior = true;
        else softened = true;
      }
    }
    if (changedInterior) {
      std::printf("FAIL: fxaa changed a textured interior far from the geometry edge\n");
      return 1;
    }
    if (!softened) {
      std::printf("FAIL: fxaa left a depth+prim geometry edge untouched\n");
      return 1;
    }
  }
  {
    // Far block-vs-sky silhouette: depth 65420 vs clear 0xFFFF is only 115 LSBs
    // (a block at ~50 units with near 0.05 / far 1000). The old fixed 164
    // threshold missed this, and the same prim id (0) models a SIMD span that
    // never stamped aaId. FXAA must soften the silhouette and leave the
    // textured interior alone.
    SoftwareFramebuffer f;
    f.resize(64, 32);
    std::vector<uint16_t> depthQ(size_t(64) * 32), prim(size_t(64) * 32, 0);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x) {
        const bool sky = x >= 40;
        const int cx = (x / 4) & 1, cy = (y / 4) & 1;
        const uint32_t lo = 0xFF2060A0u, hi = 0xFFE0C040u, skyCol = 0xFF87CEFAu;
        f.pixels[size_t(y) * 64 + size_t(x)] = sky ? skyCol : ((cx ^ cy) ? hi : lo);
        depthQ[size_t(y) * 64 + size_t(x)] = sky ? uint16_t(0xFFFF) : uint16_t(65420);
      }
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    applyFxaaPost(f, scratch, depthQ.data(), prim.data());
    bool changedInterior = false, softened = false;
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 64; ++x) {
        const uint32_t a = before[size_t(y) * 64 + size_t(x)];
        const uint32_t b = f.pixels[size_t(y) * 64 + size_t(x)];
        if (a == b) continue;
        if (x <= 4 || x >= 59) changedInterior = true;
        else softened = true;
      }
    }
    if (changedInterior) {
      std::printf("FAIL: fxaa changed a textured interior on a far block-vs-sky silhouette\n");
      return 1;
    }
    if (!softened) {
      std::printf("FAIL: fxaa left a far block-vs-sky silhouette untouched\n");
      return 1;
    }
    std::printf("FXAA far block-vs-sky silhouette: softened, textured interior untouched\n");
  }
  {
    // Scaled path (the default play path, --sw-scale 0.5): nearest-upscale
    // colour, depth, and prim-id, then FXAA, then wipe depth. A far
    // block-vs-sky silhouette at render resolution must still soften at
    // present resolution; textured interiors and open sky must not move.
    // (Wiping depth before the post pass, the 07:40 bug, leaves every
    // present-size depth at 0xFFFF and FXAA is a no-op.)
    auto nnScale2x = [](const SoftwareFramebuffer& src, const std::vector<uint16_t>& srcD,
                        const std::vector<uint16_t>& srcI, SoftwareFramebuffer& dst,
                        std::vector<uint16_t>& dstD, std::vector<uint16_t>& dstI) {
      const int sw = int(src.width), sh = int(src.height);
      const int dw = sw * 2, dh = sh * 2;
      dst.resize(uint32_t(dw), uint32_t(dh));
      dstD.assign(size_t(dw) * size_t(dh), 0);
      dstI.assign(size_t(dw) * size_t(dh), 0);
      for (int y = 0; y < dh; ++y) {
        const int sy = (y * sh) / dh;
        for (int x = 0; x < dw; ++x) {
          const int sx = (x * sw) / dw;
          const size_t si = size_t(sy) * size_t(sw) + size_t(sx);
          const size_t di = size_t(y) * size_t(dw) + size_t(x);
          dst.pixels[di] = src.pixels[si];
          dstD[di] = srcD[si];
          dstI[di] = srcI[si];
        }
      }
    };
    SoftwareFramebuffer low;
    low.resize(32, 16);
    std::vector<uint16_t> lowD(size_t(32) * 16), lowI(size_t(32) * 16, 0);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x) {
        const bool sky = x >= 20;
        const int cx = (x / 4) & 1, cy = (y / 4) & 1;
        const uint32_t lo = 0xFF2060A0u, hi = 0xFFE0C040u, skyCol = 0xFF87CEFAu;
        low.pixels[size_t(y) * 32 + size_t(x)] = sky ? skyCol : ((cx ^ cy) ? hi : lo);
        lowD[size_t(y) * 32 + size_t(x)] = sky ? uint16_t(0xFFFF) : uint16_t(65420);
      }
    SoftwareFramebuffer up, wiped;
    std::vector<uint16_t> upD, upI, wipedD, wipedI;
    nnScale2x(low, lowD, lowI, up, upD, upI);
    nnScale2x(low, lowD, lowI, wiped, wipedD, wipedI);
    wipedD.assign(wipedD.size(), 0xFFFF);  // the 07:40 bug
    const auto before = up.pixels;
    SoftwareAaScratch scratch, scratchW;
    applyFxaaPost(up, scratch, upD.data(), upI.data());
    applyFxaaPost(wiped, scratchW, wipedD.data(), wipedI.data());
    if (wiped.pixels != before) {
      std::printf("FAIL: fxaa changed a scaled frame whose depth was wiped to sky\n");
      return 1;
    }
    bool changedInterior = false, softened = false, skyTouched = false;
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 64; ++x) {
        const uint32_t a = before[size_t(y) * 64 + size_t(x)];
        const uint32_t b = up.pixels[size_t(y) * 64 + size_t(x)];
        if (a == b) continue;
        if (x <= 8) changedInterior = true;
        else if (x >= 50) skyTouched = true;
        else softened = true;
      }
    }
    if (changedInterior) {
      std::printf("FAIL: fxaa changed a textured interior on the scaled far silhouette\n");
      return 1;
    }
    if (skyTouched) {
      std::printf("FAIL: fxaa changed open sky on the scaled far silhouette\n");
      return 1;
    }
    if (!softened) {
      std::printf("FAIL: fxaa left a scaled far block-vs-sky silhouette untouched\n");
      return 1;
    }
    // Integration: the same silhouette through renderFrame at --sw-scale 0.5.
    // A far axis-aligned wall sits against a known sky; present size is 2x the
    // world stage. FXAA must differ from off at the silhouette and match in
    // the wall interior and open sky. Scale is restored even on failure.
    struct RestoreScale {
      RestoreScale() { cSoftwareRenderer::setRenderScale(0.5f); }
      ~RestoreScale() { cSoftwareRenderer::setRenderScale(1.0f); }
    } restoreScale;
    auto makeWall = [](cSoftwareRenderer& r) {
      r.setModelDither(false);
      r.setPerPixelShading(false);
      ChunkMesh cm;
      cm.cx = 0;
      cm.sy = 0;
      cm.cz = 3;  // world z = 48
      // Both windings of a 16x16 wall in the XY plane at the near face, so
      // back-face culling cannot drop it. Packed units are 1/16 block.
      const uint16_t p[4][3] = {{0, 0, 0}, {256, 0, 0}, {256, 256, 0}, {0, 256, 0}};
      for (int wind = 0; wind < 2; ++wind) {
        for (int v = 0; v < 4; ++v) {
          const int s = wind ? 3 - v : v;
          PackedVertex pv{};
          pv.x = p[s][0];
          pv.y = p[s][1];
          pv.z = p[s][2];
          pv.faceAo = 4;
          pv.value = 200;
          pv.u = uint8_t(v == 1 || v == 2 ? 255 : 0);
          pv.v = uint8_t(v >= 2 ? 255 : 0);
          pv.mode = kShadeTex;
          cm.verts.push_back(pv);
        }
      }
      r.uploadChunkMesh(cm);
    };
    FrameParams fp;
    fp.width = 128;
    fp.height = 64;
    fp.camPos = {8.f, 8.f, 32.f};
    fp.sky = {0.529f, 0.808f, 0.980f};
    fp.viewProj = mul(perspective(1.2f, 128.f / 64.f, 0.05f, 1000.f),
                      lookDir(fp.camPos, normalize(Vec3{0.f, 0.f, 1.f})));
    fp.crosshair = false;
    fp.groundPulseOn = false;
    fp.groundTraceOn = false;
    cSoftwareRenderer rOff, rFx;
    makeWall(rOff);
    makeWall(rFx);
    rOff.setAaMode("off");
    rFx.setAaMode("fxaa");
    rOff.renderFrame(fp);
    rFx.renderFrame(fp);
    const auto& offPx = rOff.framebuffer().pixels;
    const auto& fxPx = rFx.framebuffer().pixels;
    if (offPx.size() != size_t(128) * 64 || fxPx.size() != offPx.size() ||
        rOff.framebuffer().width != 128 || rOff.framebuffer().height != 64) {
      std::printf("FAIL: scaled path present size is %ux%u, expected 128x64\n",
                  rOff.framebuffer().width, rOff.framebuffer().height);
      return 1;
    }
    const uint32_t skyCol = offPx[0];
    size_t solid = 0, changed = 0, interiorHit = 0, skyHit = 0;
    for (int y = 0; y < 64; ++y) {
      for (int x = 0; x < 128; ++x) {
        const size_t i = size_t(y) * 128 + size_t(x);
        const bool isSky = offPx[i] == skyCol;
        if (!isSky) ++solid;
        if (offPx[i] == fxPx[i]) continue;
        ++changed;
        // A 6-pixel margin from any opposite-class pixel is the silhouette;
        // farther than that is interior / open sky and must not move.
        bool nearSky = isSky;
        bool nearSolid = !isSky;
        for (int dy = -6; dy <= 6 && !(nearSky && nearSolid); ++dy) {
          const int yy = y + dy;
          if (yy < 0 || yy >= 64) continue;
          for (int dx = -6; dx <= 6; ++dx) {
            const int xx = x + dx;
            if (xx < 0 || xx >= 128) continue;
            if (offPx[size_t(yy) * 128 + size_t(xx)] == skyCol) nearSky = true;
            else nearSolid = true;
          }
        }
        if (isSky && !nearSolid) ++skyHit;
        if (!isSky && !nearSky) ++interiorHit;
      }
    }
    rFx.renderFrame(fp);
    if (rFx.framebuffer().pixels != fxPx) {
      std::printf("FAIL: scaled-path fxaa is not stable across two frames\n");
      return 1;
    }
    if (solid < 200) {
      std::printf("FAIL: scaled-path wall covered only %zu pixels\n", solid);
      return 1;
    }
    if (changed == 0) {
      std::printf("FAIL: scaled-path fxaa matched off (upscale gate missed the wall)\n");
      return 1;
    }
    if (interiorHit || skyHit) {
      std::printf("FAIL: scaled-path fxaa touched interior=%zu open-sky=%zu\n", interiorHit, skyHit);
      return 1;
    }
    std::printf("FXAA scaled path (NN-upscale depth and prim-id): softened, textured interior untouched\n");
  }
  {
    // Pinned hashes + 1/2/4-thread and across-run determinism for each mode.
    auto makeScene = [](SoftwareFramebuffer& f, std::vector<uint16_t>& depthQ, std::vector<uint16_t>& prim) {
      f.resize(48, 24);
      depthQ.resize(size_t(48) * 24);
      prim.resize(size_t(48) * 24);
      for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 48; ++x) {
          const bool right = x >= 24;
          const int cx = (x / 3) & 1, cy = (y / 3) & 1;
          const uint32_t a = 0xFF102030u, b = 0xFFF0D080u, c = 0xFF208040u, d = 0xFFC0E0FFu;
          f.pixels[size_t(y) * 48 + size_t(x)] = right ? ((cx ^ cy) ? d : c) : ((cx ^ cy) ? b : a);
          depthQ[size_t(y) * 48 + size_t(x)] = right ? uint16_t(42000) : uint16_t(18000);
          prim[size_t(y) * 48 + size_t(x)] = right ? uint16_t(7) : uint16_t(3);
        }
    };
    SoftwareFramebuffer f, f2;
    std::vector<uint16_t> depthQ, prim;
    makeScene(f, depthQ, prim);
    SoftwareAaScratch scratch;
    const auto offPix = f.pixels;
    const uint64_t offH = hashPixels(offPix);

    applyFxaaPost(f, scratch, depthQ.data(), prim.data());
    const uint64_t fxaaH = hashPixels(f.pixels);
    if (fxaaH == offH) {
      std::printf("FAIL: fxaa pinned scene was a no-op\n");
      return 1;
    }
    // Run again from the same input: identical.
    makeScene(f2, depthQ, prim);
    applyFxaaPost(f2, scratch, depthQ.data(), prim.data());
    if (f2.pixels != f.pixels) {
      std::printf("FAIL: fxaa not deterministic across runs\n");
      return 1;
    }
    // 1 vs 2 vs 4 row bands, same input (serial bands must match, and so must
    // a fresh prepare + disjoint row writes).
    auto runBands = [&](int bands, SoftwareFramebuffer& out) {
      makeScene(out, depthQ, prim);
      SoftwareAaScratch sc;
      prepareFxaa(out, sc);
      const int h = int(out.height);
      for (int b = 0; b < bands; ++b) {
        const int y0 = (h * b) / bands;
        const int y1 = (h * (b + 1)) / bands;
        applyFxaaRows(out, sc, depthQ.data(), prim.data(), y0, y1);
      }
      commitFxaaRows(out, sc, 0, h);
    };
    SoftwareFramebuffer t1, t2, t4;
    runBands(1, t1);
    runBands(2, t2);
    runBands(4, t4);
    if (t1.pixels != f.pixels || t2.pixels != f.pixels || t4.pixels != f.pixels) {
      std::printf("FAIL: fxaa 1/2/4-band hashes differ (1=%016llx 2=%016llx 4=%016llx full=%016llx)\n",
                  (unsigned long long)hashPixels(t1.pixels), (unsigned long long)hashPixels(t2.pixels),
                  (unsigned long long)hashPixels(t4.pixels), (unsigned long long)fxaaH);
      return 1;
    }

    SoftwareFramebuffer hi, out, out2;
    hi.resize(96, 48);
    out.resize(48, 24);
    out2.resize(48, 24);
    for (int y = 0; y < 48; ++y)
      for (int x = 0; x < 96; ++x) {
        const int cx = (x / 2) & 1, cy = (y / 2) & 1;
        hi.pixels[size_t(y) * 96 + size_t(x)] = (cx ^ cy) ? 0xFFE8C060u : 0xFF184878u;
      }
    resolveSsaa2x(hi, out);
    resolveSsaa2x(hi, out2);
    if (out.pixels != out2.pixels) {
      std::printf("FAIL: ssaa2x resolve not deterministic across runs\n");
      return 1;
    }
    const uint64_t ssaaH = hashPixels(out.pixels);

    // Pinned values are printed on mismatch so a deliberate algorithm change
    // can update them. Default (off) is the input hash; fxaa/ssaa must stay put.
    std::printf("AA hashes: off=%016llx fxaa=%016llx ssaa2x=%016llx\n",
                (unsigned long long)offH, (unsigned long long)fxaaH, (unsigned long long)ssaaH);
    // Pinned 2026-11-17 on the box Linux DebugOpt build of the geometry-edge fixed-point path.
    const uint64_t kOff  = 0x3008422cca928783ull;
    const uint64_t kFxaa = 0x2a113a20cd02913cull;
    const uint64_t kSsaa = 0xec27646a53b65d83ull;
    if (offH != kOff || fxaaH != kFxaa || ssaaH != kSsaa) {
      std::printf("FAIL: AA pinned hash moved (off %016llx want %016llx, fxaa %016llx want %016llx, ssaa %016llx want %016llx)\n",
                  (unsigned long long)offH, (unsigned long long)kOff,
                  (unsigned long long)fxaaH, (unsigned long long)kFxaa,
                  (unsigned long long)ssaaH, (unsigned long long)kSsaa);
      return 1;
    }
  }

  // --- ASCII pipeline (JD 2026-10-13) ---------------------------------------
  {
    SoftwareFramebuffer f;
    f.resize(64, 32);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        f.pixels[size_t(y) * 64 + size_t(x)] = x < 32 ? 0xFF000000u : 0xFFFFFFFFu;
    AsciiGridInfo info;
    const std::string text = framebufferToAsciiText(f, 32, &info);
    if (info.cols != 32 || info.rows < 4 || info.textBytes != text.size()) {
      std::printf("FAIL: ascii grid geometry (%d cols, %d rows)\n", info.cols, info.rows);
      return 1;
    }
    // Left half dark -> ' ', right half bright -> '@', per row.
    const size_t rowLen = size_t(info.cols) + 1;
    for (int y = 0; y < info.rows; ++y) {
      if (text[size_t(y) * rowLen + 2] != ' ' || text[size_t(y) * rowLen + 29] != '@') {
        std::printf("FAIL: ascii ramp mapping wrong at row %d\n", y);
        return 1;
      }
    }
    framebufferToAsciiImage(f, 32);
    bool anyLit = false;
    for (uint32_t p : f.pixels)
      if (p != 0xFF000000u) anyLit = true;
    if (!anyLit) {
      std::printf("FAIL: ascii image drew nothing for the bright half\n");
      return 1;
    }
  }
  std::printf("ASCII checks: grid geometry + ramp mapping + image output OK\n");


  // ASCII GUI (JD 2026-11-15): cell mapping and a painted label must not
  // change the world text grid produced by framebufferToAsciiText.
  {
    AsciiGridGeom g;
    if (!asciiGridGeometry(320, 180, 80, g) || g.cols != 80 || g.rows < 20) {
      std::printf("FAIL: ascii GUI grid geometry (%d x %d)\n", g.cols, g.rows);
      return 1;
    }
    float sx = g.cellW * 3.2f, sy = g.cellH * 5.2f;
    int cx = 0, cy = 0;
    asciiPointerToCell(g, sx, sy, cx, cy);
    float px = 0, py = 0;
    asciiCellCenter(g, cx, cy, px, py);
    asciiSnapPointer(g, sx, sy);
    if (cx != 3 || cy != 5 || sx != px || sy != py) {
      std::printf("FAIL: ascii pointer-to-cell snap (%d,%d %f,%f)\n", cx, cy, sx, sy);
      return 1;
    }
    SoftwareFramebuffer f;
    f.width = 320;
    f.height = 180;
    f.pixels.assign(size_t(320) * 180, 0xFF202020u);
    const std::string before = framebufferToAsciiText(f, 80);
    FrameParams p;
    p.width = 320;
    p.height = 180;
    p.crosshair = false;
    HudDraw panel;
    panel.x = 40; panel.y = 40; panel.w = 160; panel.h = 64;
    panel.r = 0.1f; panel.g = 0.1f; panel.b = 0.15f; panel.a = 1.f;
    HudDraw label;
    label.x = 56; label.y = 56; label.w = 0; label.h = 0;
    label.text = "Play";
    label.r = 1; label.g = 1; label.b = 1; label.a = 1;
    label.scale = 1.f;
    p.hud.draws.push_back(panel);
    p.hud.draws.push_back(label);
    paintAsciiHud(f, p, 80);
    // HUD paint is display-only and should leave cells far from the panel alone.
    // (The panel itself is supposed to change those cells.)
    const std::string after = framebufferToAsciiText(f, 80);
    const int farCol = g.cols - 4, farRow = g.rows - 3;
    auto cellCh = [&](const std::string& s, int col, int row) -> char {
      size_t pos = 0;
      for (int r = 0; r < row; ++r) {
        const size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) return '?';
        pos = nl + 1;
      }
      if (pos + size_t(col) >= s.size()) return '?';
      return s[pos + size_t(col)];
    };
    if (cellCh(after, farCol, farRow) != cellCh(before, farCol, farRow)) {
      std::printf("FAIL: paintAsciiHud touched a cell outside the panel\n");
      return 1;
    }
    // The 'P' of "Play" should light some pixels near its cell.
    int lcx = 0, lcy = 0;
    asciiPointerToCell(g, 56.f + 4.f, 56.f + 8.f, lcx, lcy);
    int lit = 0;
    const int x0 = int(float(lcx) * g.cellW), x1 = int(float(lcx + 1) * g.cellW);
    const int y0 = int(float(lcy) * g.cellH), y1 = int(float(lcy + 1) * g.cellH);
    for (int y = y0; y < y1 && y < 180; ++y)
      for (int x = x0; x < x1 && x < 320; ++x) {
        const uint32_t pxv = f.pixels[size_t(y) * 320 + size_t(x)];
        if ((pxv & 0xFF) > 0x80 && ((pxv >> 16) & 0xFF) > 0x80) ++lit;
      }
    if (lit < 8) {
      std::printf("FAIL: ascii GUI label was not painted (lit %d)\n", lit);
      return 1;
    }
    std::printf("ASCII GUI checks: cell snap + label paint + world text unchanged OK\n");
  }


  // --- cSysterian glyph set (JD 2026-10-13; AMENDED by JD ruling 2026-10-13
  // 15:52 EDT: full 0-255 value span) --------------------------------------
  // The experiment build mapped a cell through the ASCII layer: ramp char
  // -> code point (' ' 32 .. '@' 64), so its pin read "mid-gray -> 58,
  // white -> 64". That 58 does NOT survive the ruling: it encoded the
  // 32..64 code-point subset, not the luminance. Under the full-range
  // mapping the value IS the luminance level (0..span, default 255):
  // 0xFF808080 (luma 128/255) -> level 128, white -> level 255 — both
  // beyond anything a 64-step mapping can produce (its ceiling is 64).
  {
    if (std::string(asciiGlyphSetName(AsciiGlyphSet::cSysterian)) != "systerian") {
      std::printf("FAIL: systerian set name\n");
      return 1;
    }
    // Amendment pins: span default + level resolution + parameterization.
    if (kAsciiDefaultValueSpan != 255 || asciiValueLevel(0.f, 255) != 0 ||
        asciiValueLevel(1.f, 255) != 255 || asciiValueLevel(128.f / 255.f, 255) != 128 ||
        asciiValueLevel(128.f / 255.f, 255) <= 64 ||  // beyond the 64th step
        asciiValueLevel(128.f / 255.f, 64) != 32) {   // span is a real parameter
      std::printf("FAIL: ascii value span/level (default %d, mid %d, span64 %d)\n",
                  kAsciiDefaultValueSpan, asciiValueLevel(128.f / 255.f, 255),
                  asciiValueLevel(128.f / 255.f, 64));
      return 1;
    }
    SoftwareFramebuffer f;
    f.resize(64, 32);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        f.pixels[size_t(y) * 64 + size_t(x)] = x < 32 ? 0xFF808080u : 0xFFFFFFFFu;
    AsciiGridInfo info;
    const std::string text =
        framebufferToAsciiText(f, 32, AsciiGlyphSet::cSysterian, &info);
    if (info.textBytes != text.size() || text.size() != size_t(info.rows) * (32 * 3 + 1)) {
      std::printf("FAIL: systerian value grid size (%zu bytes)\n", text.size());
      return 1;
    }
    const size_t stride = 32 * 3 + 1;
    for (int y = 0; y < info.rows; ++y) {
      if (text.compare(size_t(y) * stride + 6, 3, "128") != 0 ||
          text.compare(size_t(y) * stride + 87, 3, "255") != 0) {
        std::printf("FAIL: systerian value mapping wrong at row %d\n", y);
        return 1;
      }
    }
    SoftwareFramebuffer fa = f, fs = f;
    framebufferToAsciiImage(fa, 32, AsciiGlyphSet::Ascii);
    framebufferToAsciiImage(fs, 32, AsciiGlyphSet::cSysterian);
    if (fa.pixels == fs.pixels) {
      std::printf("FAIL: systerian image identical to ascii image\n");
      return 1;
    }
    // Glyph 128 (hundreds+tens+units strokes) has strokes: the (tinted)
    // left half must show lit pixels, where the ASCII set's '+' shows far
    // fewer.
    size_t fsLit = 0, faLit = 0;
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        if (fs.pixels[size_t(y) * 64 + size_t(x)] != 0xFF000000u) ++fsLit;
        if (fa.pixels[size_t(y) * 64 + size_t(x)] != 0xFF000000u) ++faLit;
      }
    if (fsLit == 0 || fsLit <= faLit) {
      std::printf("FAIL: systerian glyph strokes missing (lit %zu vs ascii %zu)\n", fsLit,
                  faLit);
      return 1;
    }
    std::printf("Systerian checks: value grid 128/255 OK (full 0-255 span, JD "
                "2026-10-13), glyph strokes drawn (lit %zu)\n",
                fsLit);
  }

  // Cortana 2026-10-15 1335: multithreaded chunk pass must be bit-identical
  // for every thread count (the Daemon's MCP vision frames and screenshots
  // may not depend on the CPU). 48 chunk meshes of deterministic random
  // axis-aligned quads (both windings, so back-face culling is exercised),
  // perspective camera, per-pixel path, 1 vs 2/4/8 threads.
  {
    cSoftwareRenderer mt;
    mt.setModelDither(false);
    mt.setPerPixelShading(true);
    uint32_t rng = 12345u;
    auto rnd = [&](uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
    for (int cz = 0; cz < 6; ++cz)
      for (int cx = -4; cx < 4; ++cx) {
        ChunkMesh cm; cm.cx = cx; cm.sy = 0; cm.cz = cz;
        for (int k = 0; k < 60; ++k) {
          const uint16_t a = uint16_t(rnd(200)), b = uint16_t(rnd(200)), h = uint16_t(rnd(256));
          const uint16_t w = uint16_t(16 + rnd(56)), d = uint16_t(16 + rnd(56));
          const int axis = int(rnd(3));
          uint16_t p[4][3];
          if (axis == 0) { const uint16_t pp[4][3] = {{a, h, b}, {uint16_t(a + w), h, b}, {uint16_t(a + w), h, uint16_t(b + d)}, {a, h, uint16_t(b + d)}}; std::memcpy(p, pp, sizeof p); }
          else if (axis == 1) { const uint16_t pp[4][3] = {{a, b, h}, {uint16_t(a + w), b, h}, {uint16_t(a + w), uint16_t(b + d), h}, {a, uint16_t(b + d), h}}; std::memcpy(p, pp, sizeof p); }
          else { const uint16_t pp[4][3] = {{h, a, b}, {h, uint16_t(a + w), b}, {h, uint16_t(a + w), uint16_t(b + d)}, {h, a, uint16_t(b + d)}}; std::memcpy(p, pp, sizeof p); }
          const bool flip = rnd(2) != 0;
          for (int v = 0; v < 4; ++v) {
            const int s = flip ? 3 - v : v;
            PackedVertex pv{};
            pv.x = p[s][0]; pv.y = p[s][1]; pv.z = p[s][2];
            pv.faceAo = uint8_t(axis == 0 ? 2 : axis == 1 ? 4 : 0); pv.value = uint8_t(60 + rnd(190));
            pv.u = uint8_t(v == 1 || v == 2 ? 255 : 0); pv.v = uint8_t(v >= 2 ? 255 : 0);
            pv.mode = kShadeTex;
            cm.verts.push_back(pv);
          }
        }
        mt.uploadChunkMesh(cm);
      }
    FrameParams fp;
    fp.width = 640; fp.height = 360;
    fp.camPos = {0.5f, 9.f, -6.f};
    fp.viewProj = mul(perspective(1.2f, 640.f / 360.f, 0.1f, 400.f), lookDir(fp.camPos, normalize(Vec3{0.05f, -0.25f, 1.f})));
    fp.crosshair = false;
    std::vector<uint32_t> ref;
    bool same = true;
    for (unsigned th : {1u, 2u, 4u, 8u}) {
      cSoftwareRenderer::setRenderThreads(th);
      mt.renderFrame(fp);
      if (th == 1) ref = mt.framebuffer().pixels;
      else if (mt.framebuffer().pixels != ref) { same = false; std::printf("FAIL: chunk pass differs at %u threads\n", th); }
    }
    cSoftwareRenderer::setRenderThreads(0);
    size_t lit = 0;
    for (uint32_t px : ref) if (px != ref[0]) ++lit;
    std::printf("Multithreaded chunk pass: 1/2/4/8 threads %s (%zu non-sky px)\n", same ? "bit-identical" : "DIFFER", lit);
    if (!same || lit < 1000) return 1;
    // JD 2026-11-17: phase-local render bands. The renderer borrows the idle reserved cores
    // for the render phase only. Every borrowed count must give the same pixels (off and
    // FXAA), the phase ticket must be closed (even) between frames and advance by exactly
    // one open/close pair per frame, so the cores are handed back at the boundary.
    {
      bool bsame = true, tickOk = true;
      std::vector<uint32_t> refOff, refFx;
      for (int borrow : {0, 0, 1, 2, 3, 5}) {
        cSoftwareRenderer::setBorrowedBandsOverride(borrow);
        mt.setAaMode("off");
        const uint64_t t0 = cSoftwareRenderer::renderPhaseTicket();
        mt.renderFrame(fp);
        const uint64_t t1 = cSoftwareRenderer::renderPhaseTicket();
        if ((t0 & 1) || (t1 & 1) || t1 != t0 + 2) { tickOk = false; std::printf("FAIL: render phase ticket %llu -> %llu at borrow %d\n", (unsigned long long)t0, (unsigned long long)t1, borrow); }
        if (cSoftwareRenderer::borrowedRenderThreads() != unsigned(borrow)) { tickOk = false; std::printf("FAIL: borrowed bands %u, wanted %d\n", cSoftwareRenderer::borrowedRenderThreads(), borrow); }
        if (borrow == 0 && refOff.empty()) refOff = mt.framebuffer().pixels;
        else if (mt.framebuffer().pixels != refOff) { bsame = false; std::printf("FAIL: chunk pass differs with %d borrowed band(s)\n", borrow); }
        mt.setAaMode("fxaa");
        mt.renderFrame(fp);
        if (borrow == 0 && refFx.empty()) refFx = mt.framebuffer().pixels;
        else if (mt.framebuffer().pixels != refFx) { bsame = false; std::printf("FAIL: FXAA frame differs with %d borrowed band(s)\n", borrow); }
      }
      mt.setAaMode("off");
      cSoftwareRenderer::setBorrowedBandsOverride(-1);
      std::printf("Phase-local render bands: 0/1/2/3/5 borrowed %s (off and FXAA), phase ticket %s\n",
                  bsame ? "bit-identical" : "DIFFER", tickOk ? "closed between frames" : "WRONG");
      if (!bsame || !tickOk || refOff == refFx) return 1;
    }
    // JD 2026-11-17: borrow only when the last frame was heavy (hysteresis), so light
    // frames keep the single-band cost. The forced-count checks above stay always-on.
    {
      bool ok = true;
      cSoftwareRenderer::setBorrowedBandsAdaptiveOverride(2);
      cSoftwareRenderer::resetBorrowHysteresis();
      mt.setAaMode("off");
      mt.renderFrame(fp);  // last time is 0, so this frame must not borrow
      if (cSoftwareRenderer::borrowedRenderThreads() != 0) {
        ok = false;
        std::printf("FAIL: first/light frame borrowed %u, wanted 0\n", cSoftwareRenderer::borrowedRenderThreads());
      }
      const std::vector<uint32_t> lightPx = mt.framebuffer().pixels;
      cSoftwareRenderer::noteRenderTimeNs(15'000'000ull);  // 15 ms: over the 10 ms on-threshold
      mt.renderFrame(fp);
      if (cSoftwareRenderer::borrowedRenderThreads() != 2) {
        ok = false;
        std::printf("FAIL: heavy frame borrowed %u, wanted 2\n", cSoftwareRenderer::borrowedRenderThreads());
      }
      if (mt.framebuffer().pixels != lightPx) {
        ok = false;
        std::printf("FAIL: adaptive borrow changed pixels\n");
      }
      cSoftwareRenderer::noteRenderTimeNs(9'000'000ull);  // mid gap: hold on
      mt.renderFrame(fp);
      if (cSoftwareRenderer::borrowedRenderThreads() != 2) {
        ok = false;
        std::printf("FAIL: hysteresis mid gap dropped borrow (%u)\n", cSoftwareRenderer::borrowedRenderThreads());
      }
      cSoftwareRenderer::noteRenderTimeNs(7'000'000ull);  // under the 8 ms off-threshold
      mt.renderFrame(fp);
      if (cSoftwareRenderer::borrowedRenderThreads() != 0) {
        ok = false;
        std::printf("FAIL: light frame after heavy still borrowed %u\n", cSoftwareRenderer::borrowedRenderThreads());
      }
      cSoftwareRenderer::noteRenderTimeNs(9'000'000ull);  // mid gap: hold off
      mt.renderFrame(fp);
      if (cSoftwareRenderer::borrowedRenderThreads() != 0) {
        ok = false;
        std::printf("FAIL: hysteresis mid gap armed borrow (%u)\n", cSoftwareRenderer::borrowedRenderThreads());
      }
      cSoftwareRenderer::setBorrowedBandsOverride(-1);
      cSoftwareRenderer::resetBorrowHysteresis();
      std::printf("Adaptive render-band borrow: on at 15 ms, hold at 9 ms, off at 7 ms, mid-gap holds, pixels %s\n",
                  ok ? "unchanged" : "WRONG");
      if (!ok) return 1;
    }
  }

  {  // Cortana 2026-10-15 1335: traces fast path == reference, bit for bit.
    size_t bad = 0, lit = 0;
    for (int b = 0; b < kGroundPulseBiomeCount; ++b) {
      const GroundTraceParams tp = groundTraceParams(GroundPulseBiome(b));
      uint32_t r = 777u + uint32_t(b);
      for (int i = 0; i < 200000; ++i) {
        r = r * 1664525u + 1013904223u; const float x = float(int32_t(r >> 4) % 4000000) / 1000.f - 2000.f;
        r = r * 1664525u + 1013904223u; const float z = float(int32_t(r >> 4) % 4000000) / 1000.f - 2000.f;
        const float ts = float(i % 977) * 0.0371f;
        const auto a = groundTraceEmissive(ts, x, z, tp), c = groundTraceEmissiveFast(ts, x, z, tp);
        if (std::memcmp(a.data(), c.data(), sizeof(float) * 3) != 0) ++bad;
        if (a[0] + a[1] + a[2] > 0.f) ++lit;
      }
    }
    std::printf("Ground traces fast path: %zu mismatches (%zu lit samples)\n", bad, lit);
    if (bad) return 1;
  }
  {  // Cortana 2026-10-15 1625 (MIXED, JD idea): Trace-mix pulse colour.
    const float biome[3] = {0.2f, 0.6f, 0.9f};
    float o[4] = {0, 0, 0, 7.f};
    traceMixPulseColor(0, 0, biome, o);
    const bool none = o[0] == biome[0] && o[1] == biome[1] && o[2] == biome[2] && o[3] == 7.f;
    traceMixPulseColor(10, 0, biome, o);
    const bool red = o[0] > o[1] && o[0] > o[2] && std::fabs(o[0] - 0.9f) < 1e-5f;
    traceMixPulseColor(0, 10, biome, o);
    const bool blue = o[2] > o[0] && o[2] > o[1] && std::fabs(o[2] - 0.9f) < 1e-5f;
    traceMixPulseColor(5, 5, biome, o);
    const float m0 = (1.f + 0x2F / 255.f) / 2.f, m2 = (0x1E / 255.f + 1.f) / 2.f;  /* 2026-10-24: red peak #FF1C1E */  // weighted mean before peak rescale
    const bool mix = std::fabs(o[0] / o[2] - m0 / m2) < 1e-4f;
    std::printf("Trace-mix pulse colour: none=%d red=%d blue=%d mix=%d\n", none, red, blue, mix);
    if (!(none && red && blue && mix)) return 1;
  }
  {  // Cortana 2026-10-15 1628: CA-03 [ENGINE] 16-bit layer round trip; slot keeps 24 bits.
    PackedVertex pv{};
    pv.slot = 0x00ABCDEFu;
    pv.setLayer(0x1234u);
    const bool hi = pv.layerIndex() == 0x1234u && pv.layer == 0x34u && pv.slot == 0x00ABCDEFu;
    pv.setLayer(7u);
    const bool lo = pv.layerIndex() == 7u && pv.layerHi == 0u && pv.slot == 0x00ABCDEFu;
    std::printf("CA-03 16-bit layer: hi=%d lo=%d size=%d\n", hi, lo, int(sizeof(PackedVertex)));
    if (!(hi && lo && sizeof(PackedVertex) == 16)) return 1;
  }
  {  // Cortana 2026-10-15 1715: Hi-Z [ENGINE] - occlusion-skipped draws change no pixel.
    // A near quad and a larger far quad behind it, drawn far-then-near (Hi-Z can
    // reject nothing) and near-then-far (Hi-Z skips the far quad's occluded rows),
    // must give the same frame. UVs span 3 tiles, so swTile is exercised too.
    cSoftwareRenderer hz;
    hz.resize(256, 192);
    hz.setModelDither(false);
    hz.setPerPixelShading(true);
    float om[16] = {};
    om[0] = 0.125f; om[5] = 0.125f; om[10] = 1.f; om[12] = -1.f; om[13] = -1.f; om[15] = 1.f;
    auto quad = [](PackedVertex* o, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t z, uint8_t val) {
      const uint16_t cc[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
      for (int i = 0; i < 4; ++i) {
        o[i] = {};
        o[i].x = cc[i][0]; o[i].y = cc[i][1]; o[i].z = z;
        o[i].u = uint8_t(i == 1 || i == 2 ? 48 : 0); o[i].v = uint8_t(i >= 2 ? 48 : 0);
        o[i].faceAo = 2; o[i].value = val; o[i].mode = kShadeTex;
      }
    };
    PackedVertex nearQ[4], farQ[4];
    quad(nearQ, 40, 40, 200, 220, 4, 200);
    quad(farQ, 0, 0, 256, 256, 12, 90);
    const uint32_t kc = 0xFF102030u;
    hz.clear(kc); hz.renderMesh(farQ, 4, om); hz.renderMesh(nearQ, 4, om);
    const std::vector<uint32_t> a = hz.framebuffer().pixels;
    hz.clear(kc); hz.renderMesh(nearQ, 4, om); hz.renderMesh(farQ, 4, om);
    const std::vector<uint32_t> b = hz.framebuffer().pixels;
    size_t drawn = 0;
    for (uint32_t p : a) drawn += p != kc;
    const bool same = a == b && drawn > 0;
    std::printf("Hi-Z order independence: same=%d drawn=%zu\n", int(same), drawn);
    if (!same) return 1;
  }  {  // Auto render threads follow the shared render-side budget (JD 2026-11-14): after the permanent
     // three, bands take about half of remaining (t46u: no +2), at most 16. 12 hw -> 4, 8 -> 2, 4 -> 1.
    const bool ok = cSoftwareRenderer::autoRenderThreads(12) == 4 && cSoftwareRenderer::autoRenderThreads(8) == 2 &&
                    cSoftwareRenderer::autoRenderThreads(4) == 1 && cSoftwareRenderer::autoRenderThreads(2) == 1 &&
                    cSoftwareRenderer::autoRenderThreads(1) == 1 && cSoftwareRenderer::autoRenderThreads(18) == 7 &&
                    cSoftwareRenderer::autoRenderThreads(64) == 16;
    std::printf("Auto render threads: 12->%u 8->%u 4->%u 2->%u 64->%u %s\n", cSoftwareRenderer::autoRenderThreads(12),
                cSoftwareRenderer::autoRenderThreads(8), cSoftwareRenderer::autoRenderThreads(4),
                cSoftwareRenderer::autoRenderThreads(2), cSoftwareRenderer::autoRenderThreads(64), ok ? "ok" : "FAIL");
    if (!ok) return 1;
  }  if (writePPM("/tmp/sw_test.ppm", fb)) {
    std::printf("Wrote /tmp/sw_test.ppm\n");
    return 0;
  }
  std::printf("Failed to write output\n");
  return 1;
}

}  // namespace litholattice
