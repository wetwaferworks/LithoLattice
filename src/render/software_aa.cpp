// Software-renderer anti-aliasing: FXAA-class post filter, conservative
// morphological coverage restoration (CMAA lineage), SSAA resolve.
// Clean-room implementations from the published technique descriptions
// (see Software-Renderer-AA-2026-10-13.md for the survey + citations);
// no third-party code. FXAA (2026-11-17) is fixed-point, gated on depth
// and/or primitive-id discontinuities, and is safe to run in row bands
// over the rasterizer's existing worker split (disjoint writes, no locks).
#include "litholattice/render/software_aa.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <cstring>

namespace litholattice {
namespace {

inline float lumaOf(uint32_t p) {
  const float r = float((p >> 16) & 0xFF), g = float((p >> 8) & 0xFF),
              b = float(p & 0xFF);
  return (0.299f * r + 0.587f * g + 0.114f * b) * (1.f / 255.f);
}

inline uint32_t packPx(float r, float g, float b) {
  auto ch = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
  return 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

void computeLuma(const SoftwareFramebuffer& fb, SoftwareAaScratch& s) {
  const size_t n = size_t(fb.width) * fb.height;
  s.luma.resize(n);
  for (size_t i = 0; i < n; ++i) s.luma[i] = lumaOf(fb.pixels[i]);
}

// Bilinear sample in pixel-index space (centers on integers), clamped.
uint32_t bilinearAt(const uint32_t* src, int w, int h, float fx, float fy) {
  fx = std::clamp(fx, 0.f, float(w - 1));
  fy = std::clamp(fy, 0.f, float(h - 1));
  const int x0 = int(fx), y0 = int(fy);
  const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
  const float tx = fx - float(x0), ty = fy - float(y0);
  float r = 0, g = 0, b = 0;
  const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
  const float wx[2] = {1.f - tx, tx}, wy[2] = {1.f - ty, ty};
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const uint32_t p = src[size_t(ys[j]) * size_t(w) + size_t(xs[i])];
      const float wgt = wx[i] * wy[j];
      r += float((p >> 16) & 0xFF) * wgt;
      g += float((p >> 8) & 0xFF) * wgt;
      b += float(p & 0xFF) * wgt;
    }
  return packPx(r, g, b);
}

}  // namespace

const char* softwareAaModeName(SoftwareAaMode mode) {
  switch (mode) {
    case SoftwareAaMode::Fxaa: return "fxaa";
    case SoftwareAaMode::Cmaa: return "cmaa";
    case SoftwareAaMode::Ssaa2x: return "ssaa2x";
    default: return "off";
  }
}

bool softwareAaModeFromName(const char* name, SoftwareAaMode& out) {
  if (!name) return false;
  const std::string_view sv(name);
  if (sv == "off") { out = SoftwareAaMode::Off; return true; }
  if (sv == "fxaa") { out = SoftwareAaMode::Fxaa; return true; }
  if (sv == "cmaa") { out = SoftwareAaMode::Cmaa; return true; }
  if (sv == "ssaa2x" || sv == "ssaa") { out = SoftwareAaMode::Ssaa2x; return true; }
  return false;
}

// ---------------------------------------------------------------------------
// FXAA-class post filter (2026-11-17): geometry-edge gate + fixed-point math.
// A pixel is touched only if its 3x3 neighbourhood has a depth step and/or a
// primitive-id step, so pixel-art texture interiors stay bit-identical.
// Orientation, the subpixel term and the endpoint search follow the same
// FXAA-class shape as before, but every term is integer (luma 0-255, offsets
// in 1/256 px, bilinear with 8-bit weights). Row bands may run in parallel:
// each worker reads scratch and writes only its own rows.
// ---------------------------------------------------------------------------
static constexpr int depthEdgeQ(float t) { return int(double(t) * 65535.0) + 1; }
static int absI(int v) { return v < 0 ? -v : v; }

static uint32_t bilinearQ8(const uint32_t* src, int w, int h, int fx, int fy) {
  // fx, fy in 1/256 pixel units, origin at the pixel center index.
  if (fx < 0) fx = 0;
  if (fy < 0) fy = 0;
  const int maxx = (w - 1) << 8, maxy = (h - 1) << 8;
  if (fx > maxx) fx = maxx;
  if (fy > maxy) fy = maxy;
  const int x0 = fx >> 8, y0 = fy >> 8;
  const int x1 = x0 < w - 1 ? x0 + 1 : x0, y1 = y0 < h - 1 ? y0 + 1 : y0;
  const int tx = fx & 255, ty = fy & 255;
  const uint32_t p00 = src[size_t(y0) * size_t(w) + size_t(x0)];
  const uint32_t p10 = src[size_t(y0) * size_t(w) + size_t(x1)];
  const uint32_t p01 = src[size_t(y1) * size_t(w) + size_t(x0)];
  const uint32_t p11 = src[size_t(y1) * size_t(w) + size_t(x1)];
  auto ch = [&](uint32_t p, int shift) {
    const int a = int((p >> shift) & 0xFF);
    const int b = int((p10 >> shift) & 0xFF);
    const int c = int((p01 >> shift) & 0xFF);
    const int d = int((p11 >> shift) & 0xFF);
    const int r0 = a * (256 - tx) + b * tx;
    const int r1 = c * (256 - tx) + d * tx;
    return (r0 * (256 - ty) + r1 * ty + 32768) >> 16;
  };
  (void)p00;
  const int r = ch(p00, 16), g = ch(p00, 8), b = ch(p00, 0);
  return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

void prepareFxaaRows(const SoftwareFramebuffer& fb, SoftwareAaScratch& s, int y0, int y1) {
  const int w = int(fb.width), h = int(fb.height);
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 >= y1 || w <= 0) return;
  const uint32_t* px = fb.pixels.data();
  uint16_t* L = s.lumaQ.data();
  uint8_t* E = s.edge.data();
  for (int y = y0; y < y1; ++y) {
    const size_t row = size_t(y) * size_t(w);
    for (int x = 0; x < w; ++x) {
      const uint32_t p = px[row + size_t(x)];
      // Rec.601 coefficients in Q8: 77/256, 150/256, 29/256 (77+150+29 = 256).
      L[row + size_t(x)] = uint16_t((77 * ((p >> 16) & 0xFF) + 150 * ((p >> 8) & 0xFF) + 29 * (p & 0xFF)) >> 8);
      E[row + size_t(x)] = 0;
    }
  }
}

void prepareFxaa(const SoftwareFramebuffer& fb, SoftwareAaScratch& s) {
  const size_t n = size_t(fb.width) * size_t(fb.height);
  if (s.src.size() < n) s.src.resize(n);
  if (s.lumaQ.size() < n) s.lumaQ.resize(n);
  if (s.edge.size() < n) s.edge.resize(n);
  if (n == 0) return;
  prepareFxaaRows(fb, s, 0, int(fb.height));
}

static bool depthDiscon(int a, int b) {
  if (a == b) return false;
  // Clear/sky (0xFFFF) vs any written depth is a silhouette at any distance.
  if (a == 0xFFFF || b == 0xFFFF) return true;
  const int dMin = a < b ? a : b;
  const int dMax = a < b ? b : a;
  // Relative test in leftover far-code (65535 - dMin). Hyperbolic [0,1] depth
  // with near 0.05 / far 1000 clusters near 0xFFFF, so a fixed 164 (~0.0025)
  // missed block-vs-sky past about 20 blocks (z=50 is only ~62 LSBs from sky).
  // *16 is about a 6% jump — steep close-range face gradients stay under it,
  // occlusion and sky breaks stay over it.
  return (dMax - dMin) * 16 >= (0xFFFF - dMin);
}

void applyFxaaRows(SoftwareFramebuffer& fb, SoftwareAaScratch& s,
                   const uint16_t* depth, const uint16_t* prim, int y0, int y1) {
  const int w = int(fb.width), h = int(fb.height);
  if (w < 3 || h < 3 || !depth && !prim) return;
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 >= y1) return;
  const uint32_t* src = fb.pixels.data();
  const uint16_t* L = s.lumaQ.data();
  uint32_t* dest = s.src.data();
  uint8_t* edge = s.edge.data();
  auto lum = [&](int x, int y) -> int {
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return int(L[size_t(y) * size_t(w) + size_t(x)]);
  };
  constexpr int kSearch = 12;

  for (int y = y0; y < y1; ++y) {
    for (int x = 0; x < w; ++x) {
      // Geometry-edge gate first: depth and/or primitive-id discontinuities.
      bool geom = false;
      if (depth) {
        // Fast reject: 4-connected neighbours usually match on a flat surface
        // or open sky. Only then walk the 3x3 with the relative/sky test.
        const uint16_t mid = depth[size_t(y) * size_t(w) + size_t(x)];
        const int xl = x > 0 ? x - 1 : 0, xr = x + 1 < w ? x + 1 : w - 1;
        const int yu = y > 0 ? y - 1 : 0, yd = y + 1 < h ? y + 1 : h - 1;
        const size_t rw = size_t(w);
        if (depth[size_t(y) * rw + size_t(xl)] != mid ||
            depth[size_t(y) * rw + size_t(xr)] != mid ||
            depth[size_t(yu) * rw + size_t(x)] != mid ||
            depth[size_t(yd) * rw + size_t(x)] != mid ||
            depth[size_t(yu) * rw + size_t(xl)] != mid ||
            depth[size_t(yu) * rw + size_t(xr)] != mid ||
            depth[size_t(yd) * rw + size_t(xl)] != mid ||
            depth[size_t(yd) * rw + size_t(xr)] != mid) {
          int dMin = 0xFFFF, dMax = 0;
          bool sawSky = false, sawSolid = false;
          for (int dy = -1; dy <= 1; ++dy) {
            int yy = y + dy; if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
            const uint16_t* row = depth + size_t(yy) * rw;
            for (int dx = -1; dx <= 1; ++dx) {
              int xx = x + dx; if (xx < 0) xx = 0; else if (xx >= w) xx = w - 1;
              const int d = int(row[xx]);
              if (d == 0xFFFF) sawSky = true;
              else sawSolid = true;
              if (d < dMin) dMin = d;
              if (d > dMax) dMax = d;
            }
          }
          geom = (sawSky && sawSolid) || depthDiscon(dMin, dMax);
        }
      }
      if (!geom && prim) {
        int yy0 = y > 0 ? y - 1 : 0, yy1 = y + 1 < h ? y + 1 : h - 1;
        int xx0 = x > 0 ? x - 1 : 0, xx1 = x + 1 < w ? x + 1 : w - 1;
        const uint16_t mid = prim[size_t(y) * size_t(w) + size_t(x)];
        for (int yy = yy0; yy <= yy1 && !geom; ++yy) {
          const uint16_t* row = prim + size_t(yy) * size_t(w);
          for (int xx = xx0; xx <= xx1; ++xx) {
            if (row[xx] != mid) { geom = true; break; }
          }
        }
      }
      if (!geom) continue;

      const int lM = lum(x, y);
      const int lN = lum(x, y - 1), lS = lum(x, y + 1);
      const int lW = lum(x - 1, y), lE = lum(x + 1, y);
      const int lNW = lum(x - 1, y - 1), lNE = lum(x + 1, y - 1);
      const int lSW = lum(x - 1, y + 1), lSE = lum(x + 1, y + 1);
      int rMax = lM, rMin = lM;
      for (int v : {lN, lS, lE, lW, lNW, lNE, lSW, lSE}) {
        if (v > rMax) rMax = v;
        if (v < rMin) rMin = v;
      }
      const int range = rMax - rMin;
      if (range == 0) continue;  // same colour across the edge: nothing to blend

      // Subpixel term in Q8: t = |crossAvg - lM| / range, smoothstep, * 3/8 px.
      const int cross4 = lN + lS + lE + lW;
      int subNum = cross4 - 4 * lM; if (subNum < 0) subNum = -subNum;  // |cross-lM|*4
      int tQ8 = (subNum * 64) / range;  // 256 * (subNum/4) / range
      if (tQ8 > 256) tQ8 = 256;
      // smoothstep t^2 (3-2t) in Q8: (t*t*(3*256 - 2*t)) / 256 / 256
      const int subQ8 = (tQ8 * tQ8 * (768 - 2 * tQ8) + 32768) >> 16;
      const int subOff = (subQ8 * 96) >> 8;  // 96/256 = 3/8 px, result Q8

      const int gx = absI((lNE + 2 * lE + lSE) - (lNW + 2 * lW + lSW));
      const int gy = absI((lSW + 2 * lS + lSE) - (lNW + 2 * lN + lNE));
      const bool horzEdge = gy >= gx;

      auto searchH = [&](int dir, int avg0, int grad) {
        const int thresh = (grad + 3) >> 2;  // 1/4 of the across-edge gradient
        for (int t = 1; t <= kSearch; ++t) {
          const int a = (lum(x + dir * t, y - 1) + lum(x + dir * t, y + 1));
          int d = a - 2 * avg0; if (d < 0) d = -d;
          if (d >= 2 * thresh) return t;
        }
        return kSearch + 1;
      };
      auto searchV = [&](int dir, int avg0, int grad) {
        const int thresh = (grad + 3) >> 2;
        for (int t = 1; t <= kSearch; ++t) {
          const int a = (lum(x - 1, y + dir * t) + lum(x + 1, y + dir * t));
          int d = a - 2 * avg0; if (d < 0) d = -d;
          if (d >= 2 * thresh) return t;
        }
        return kSearch + 1;
      };

      int offQ8 = subOff;
      int sign = 1;
      uint32_t blended = 0;
      bool write = false;
      if (horzEdge) {
        const int stepN = absI(lN - lM), stepS = absI(lS - lM);
        sign = stepN >= stepS ? -1 : 1;
        const int grad = stepN >= stepS ? stepN : stepS;
        const int dPos = searchH(+1, (lN + lS) / 2, grad);
        const int dNeg = searchH(-1, (lN + lS) / 2, grad);
        const int len = dPos + dNeg;
        if (len > 0) {
          int nearer = dPos < dNeg ? dPos : dNeg;
          int edgeQ8 = 128 - (nearer * 256) / len;  // 0.5 - nearer/len, in Q8
          if (edgeQ8 < 0) edgeQ8 = 0;
          if (edgeQ8 > 128) edgeQ8 = 128;
          if (edgeQ8 > offQ8) offQ8 = edgeQ8;
        }
        if (offQ8 > 0) {
          blended = bilinearQ8(src, w, h, x << 8, (y << 8) + sign * offQ8);
          write = true;
        }
      } else {
        const int stepW = absI(lW - lM), stepE = absI(lE - lM);
        sign = stepW >= stepE ? -1 : 1;
        const int grad = stepW >= stepE ? stepW : stepE;
        const int dPos = searchV(+1, (lW + lE) / 2, grad);
        const int dNeg = searchV(-1, (lW + lE) / 2, grad);
        const int len = dPos + dNeg;
        if (len > 0) {
          int nearer = dPos < dNeg ? dPos : dNeg;
          int edgeQ8 = 128 - (nearer * 256) / len;
          if (edgeQ8 < 0) edgeQ8 = 0;
          if (edgeQ8 > 128) edgeQ8 = 128;
          if (edgeQ8 > offQ8) offQ8 = edgeQ8;
        }
        if (offQ8 > 0) {
          blended = bilinearQ8(src, w, h, (x << 8) + sign * offQ8, y << 8);
          write = true;
        }
      }
      if (write) {
        const size_t idx = size_t(y) * size_t(w) + size_t(x);
        dest[idx] = blended;
        edge[idx] = 1;
      }
    }
  }
}

void commitFxaaRows(SoftwareFramebuffer& fb, SoftwareAaScratch& s, int y0, int y1) {
  const int w = int(fb.width), h = int(fb.height);
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 >= y1 || w <= 0) return;
  uint32_t* px = fb.pixels.data();
  const uint32_t* dest = s.src.data();
  uint8_t* edge = s.edge.data();
  for (int y = y0; y < y1; ++y) {
    const size_t row = size_t(y) * size_t(w);
    for (int x = 0; x < w; ++x) {
      const size_t i = row + size_t(x);
      if (edge[i]) {
        px[i] = dest[i];
        edge[i] = 0;
      }
    }
  }
}

void applyFxaaPost(SoftwareFramebuffer& fb, SoftwareAaScratch& s,
                   const uint16_t* depth, const uint16_t* prim) {
  prepareFxaa(fb, s);
  applyFxaaRows(fb, s, depth, prim, 0, int(fb.height));
  commitFxaaRows(fb, s, 0, int(fb.height));
}

void applyCmaaPost(SoftwareFramebuffer& fb, const uint16_t* depth, SoftwareAaScratch& s) {
  const int w = int(fb.width), h = int(fb.height);
  if (w < 2 || h < 2) return;
  const size_t n = size_t(w) * size_t(h);
  s.src = fb.pixels;
  computeLuma(fb, s);
  const float* L = s.luma.data();
  s.edgeR.assign(n, 0);
  s.edgeB.assign(n, 0);
  auto lum = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return L[size_t(y) * size_t(w) + size_t(x)];
  };
  constexpr float kLumaEdge = 0.08f;    // luma step that counts as an edge
  constexpr int kDepthEdgeQ = depthEdgeQ(0.0025f); // NDC-z step that counts as geometry (uint16 domain)
  // 2026-10-26 (Archie triage step 2): neighbours are always in-bounds here, so
  // compare raw uint16 depths directly (no per-tap clamp or float dequantize).
  auto dStep = [&](size_t a, size_t b) {
    return std::abs(int(depth[a]) - int(depth[b])) >= kDepthEdgeQ;
  };
  // A monotone ramp on BOTH sides of a boundary = texture gradient, not a
  // geometry step (CMAA2's conservatism): steps continue in the same
  // direction with comparable size on both columns/rows.
  auto rampV = [&](int x, int y, float step) {  // vertical ramp in column x at row y
    const float a = lum(x, y) - lum(x, y - 1), b = lum(x, y + 1) - lum(x, y);
    return (a > 0.35f * step && b > 0.35f * step) || (a < -0.35f * step && b < -0.35f * step);
  };
  auto rampH = [&](int x, int y, float step) {  // horizontal ramp in row y at column x
    const float a = lum(x, y) - lum(x - 1, y), b = lum(x + 1, y) - lum(x, y);
    return (a > 0.35f * step && b > 0.35f * step) || (a < -0.35f * step && b < -0.35f * step);
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = size_t(y) * size_t(w) + size_t(x);
      if (x + 1 < w) {
        const float dL = std::fabs(L[i + 1] - L[i]);
        const bool dEdge = depth && dStep(i + 1, i);
        bool e = depth ? dEdge : dL > kLumaEdge;  // Cortana 2026-10-16: with depth, geometry edges only
        if (e && !dEdge && rampV(x, y, dL) && rampV(x + 1, y, dL)) e = false;
        s.edgeR[i] = e ? 1 : 0;
      }
      if (y + 1 < h) {
        const float dL = std::fabs(L[i + size_t(w)] - L[i]);
        const bool dEdge = depth && dStep(i + size_t(w), i);
        bool e = depth ? dEdge : dL > kLumaEdge;  // Cortana 2026-10-16: with depth, geometry edges only
        if (e && !dEdge && rampH(x, y, dL) && rampH(x, y + 1, dL)) e = false;
        s.edgeB[i] = e ? 1 : 0;
      }
    }
  }

  s.acc.assign(n * 4, 0.f);
  auto addBlend = [&](int px, int py, int qx, int qy, float cov) {
    if (cov <= 0.004f) return;  // sub-1/255 coverage is invisible
    float* a = &s.acc[(size_t(py) * size_t(w) + size_t(px)) * 4];
    const uint32_t c = s.src[size_t(qy) * size_t(w) + size_t(qx)];
    a[0] += cov * float((c >> 16) & 0xFF);
    a[1] += cov * float((c >> 8) & 0xFF);
    a[2] += cov * float(c & 0xFF);
    a[3] += cov;
  };
  // Integral of clamp(v(t), 0, 1) for v linear from v0 to v1 over t in [0,1]
  // (the positive part of the line offset across one pixel's extent).
  auto coverageIntegral = [](float v0, float v1) {
    const float a = std::clamp(v0, -1.f, 1.f), b = std::clamp(v1, -1.f, 1.f);
    if (a <= 0.f && b <= 0.f) return 0.f;
    if (a >= 0.f && b >= 0.f) return 0.5f * (std::min(a, 1.f) + std::min(b, 1.f));
    const float t = a / (a - b);  // zero crossing in (0,1)
    return a > 0.f ? 0.5f * t * a : 0.5f * (1.f - t) * b;
  };
  constexpr int kMaxRun = 32;

  // Vertical edges: edgeR(x,y) separates A=(x,y) from B=(x+1,y). The line
  // offset o is measured in pixels from the stored boundary (+ = toward B).
  for (int x = 0; x + 1 < w; ++x) {
    int y = 0;
    while (y < h) {
      if (!s.edgeR[size_t(y) * size_t(w) + size_t(x)]) { ++y; continue; }
      const int yTop = y;
      while (y + 1 < h && s.edgeR[size_t(y + 1) * size_t(w) + size_t(x)] &&
             y - yTop + 1 < kMaxRun)
        ++y;
      const int yBot = y;
      ++y;
      // End offsets from the adjacent columns' edges in the rows just
      // outside the run: the boundary stepped toward B (+0.5) or A (-0.5).
      float oTop = 0.f, oBot = 0.f;
      if (yTop > 0) {
        if (x + 2 < w && s.edgeR[size_t(yTop - 1) * size_t(w) + size_t(x + 1)]) oTop = 0.5f;
        else if (x > 0 && s.edgeR[size_t(yTop - 1) * size_t(w) + size_t(x - 1)]) oTop = -0.5f;
      }
      if (yBot + 1 < h) {
        if (x + 2 < w && s.edgeR[size_t(yBot + 1) * size_t(w) + size_t(x + 1)]) oBot = 0.5f;
        else if (x > 0 && s.edgeR[size_t(yBot + 1) * size_t(w) + size_t(x - 1)]) oBot = -0.5f;
      }
      const float runLen = float(yBot - yTop + 1);
      for (int yy = yTop; yy <= yBot; ++yy) {
        const float u0 = float(yy - yTop) / runLen, u1 = float(yy - yTop + 1) / runLen;
        const float oA = oTop + (oBot - oTop) * u0;  // line offset at pixel top
        const float oB = oTop + (oBot - oTop) * u1;  // ... and at pixel bottom
        const float covA = coverageIntegral(-oA, -oB);  // A covered by B's side
        const float covB = coverageIntegral(oA, oB);    // B covered by A's side
        addBlend(x, yy, x + 1, yy, covA);
        addBlend(x + 1, yy, x, yy, covB);
      }
    }
  }
  // Horizontal edges: edgeB(x,y) separates A=(x,y) from B=(x,y+1); the
  // offset o is measured from the stored boundary (+ = toward B, down).
  for (int y = 0; y + 1 < h; ++y) {
    int x = 0;
    while (x < w) {
      if (!s.edgeB[size_t(y) * size_t(w) + size_t(x)]) { ++x; continue; }
      const int xLeft = x;
      while (x + 1 < w && s.edgeB[size_t(y) * size_t(w) + size_t(x + 1)] &&
             x - xLeft + 1 < kMaxRun)
        ++x;
      const int xRight = x;
      ++x;
      float oLeft = 0.f, oRight = 0.f;
      if (xLeft > 0) {
        if (y + 2 < h && s.edgeB[size_t(y + 1) * size_t(w) + size_t(xLeft - 1)]) oLeft = 0.5f;
        else if (y > 0 && s.edgeB[size_t(y - 1) * size_t(w) + size_t(xLeft - 1)]) oLeft = -0.5f;
      }
      if (xRight + 1 < w) {
        if (y + 2 < h && s.edgeB[size_t(y + 1) * size_t(w) + size_t(xRight + 1)]) oRight = 0.5f;
        else if (y > 0 && s.edgeB[size_t(y - 1) * size_t(w) + size_t(xRight + 1)]) oRight = -0.5f;
      }
      const float runLen = float(xRight - xLeft + 1);
      for (int xx = xLeft; xx <= xRight; ++xx) {
        const float u0 = float(xx - xLeft) / runLen, u1 = float(xx - xLeft + 1) / runLen;
        const float oA = oLeft + (oRight - oLeft) * u0;
        const float oB = oLeft + (oRight - oLeft) * u1;
        const float covA = coverageIntegral(-oA, -oB);
        const float covB = coverageIntegral(oA, oB);
        addBlend(xx, y, xx, y + 1, covA);
        addBlend(xx, y + 1, xx, y, covB);
      }
    }
  }

  // Composite: out = src * (1 - min(sum cov, 1)) + accumulated blends.
  for (size_t i = 0; i < n; ++i) {
    const float* a = &s.acc[i * 4];
    if (a[3] <= 0.f) continue;
    const uint32_t c = s.src[i];
    float r = float((c >> 16) & 0xFF), g = float((c >> 8) & 0xFF), b = float(c & 0xFF);
    if (a[3] >= 1.f) {
      const float inv = 1.f / a[3];
      r = a[0] * inv;
      g = a[1] * inv;
      b = a[2] * inv;
    } else {
      const float keep = 1.f - a[3];
      r = r * keep + a[0];
      g = g * keep + a[1];
      b = b * keep + a[2];
    }
    fb.pixels[i] = packPx(r, g, b);
  }
}

// ---------------------------------------------------------------------------
// Bloom (Cortana 2026-10-16, render.bloom; suggested and implemented by Cortana).
// Bright pass: luma above kThresh (emissive Trace, lamps, glowing crystal leaves are
// the frame's brightest surfaces); sky pixels (depth at the clear value) are excluded.
// Quarter-resolution box downsample, separable 9-tap binomial blur (H then V),
// additive composite with bilinear upsampling. Reads only the finished frame, so it
// is presentational: the sim and its determinism hash never see it.
// ---------------------------------------------------------------------------
void applyBloomPost(SoftwareFramebuffer& fb, const uint16_t* depth, SoftwareAaScratch& s) {
  const int w = int(fb.width), h = int(fb.height);
  if (w < 16 || h < 16) return;
  constexpr int kDiv = 4;
  constexpr float kThresh = 0.72f, kGain = 0.9f;
  const int bw = (w + kDiv - 1) / kDiv, bh = (h + kDiv - 1) / kDiv;
  const size_t bn = size_t(bw) * size_t(bh);
  s.bloomA.assign(bn * 3, 0.f);
  s.bloomB.assign(bn * 3, 0.f);
  bool any = false;
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      float r = 0, g = 0, b = 0;
      int n = 0;
      for (int y = by * kDiv; y < std::min(h, by * kDiv + kDiv); ++y)
        for (int x = bx * kDiv; x < std::min(w, bx * kDiv + kDiv); ++x, ++n) {
          const size_t i = size_t(y) * size_t(w) + size_t(x);
          if (depth && depth[i] >= 65534) continue;  // sky / clear (PT4-A: quantized domain, was >= 0.99999f)
          const uint32_t p = fb.pixels[i];
          const float l = lumaOf(p);
          if (l <= kThresh) continue;
          const float k = (l - kThresh) / (1.f - kThresh) / l;  // keep hue, scale by excess
          r += float((p >> 16) & 0xFF) * k;
          g += float((p >> 8) & 0xFF) * k;
          b += float(p & 0xFF) * k;
        }
      if (n && (r + g + b) > 0.f) {
        float* o = &s.bloomA[(size_t(by) * size_t(bw) + size_t(bx)) * 3];
        o[0] = r / float(n); o[1] = g / float(n); o[2] = b / float(n);
        any = true;
      }
    }
  if (!any) return;  // nothing bright: leave the frame untouched (cheap early out)
  static constexpr float kW[5] = {70.f / 256.f, 56.f / 256.f, 28.f / 256.f, 8.f / 256.f, 1.f / 256.f};
  auto blur = [&](const std::vector<float>& src, std::vector<float>& dst, int sx, int sy) {
    for (int y = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x) {
        float acc[3] = {0, 0, 0};
        for (int t = -4; t <= 4; ++t) {
          const int xx = std::clamp(x + t * sx, 0, bw - 1), yy = std::clamp(y + t * sy, 0, bh - 1);
          const float* p = &src[(size_t(yy) * size_t(bw) + size_t(xx)) * 3];
          const float wt = kW[t < 0 ? -t : t];
          acc[0] += p[0] * wt; acc[1] += p[1] * wt; acc[2] += p[2] * wt;
        }
        float* o = &dst[(size_t(y) * size_t(bw) + size_t(x)) * 3];
        o[0] = acc[0]; o[1] = acc[1]; o[2] = acc[2];
      }
  };
  blur(s.bloomA, s.bloomB, 1, 0);
  blur(s.bloomB, s.bloomA, 0, 1);
  for (int y = 0; y < h; ++y) {
    const float fy = std::clamp((float(y) + 0.5f) / kDiv - 0.5f, 0.f, float(bh - 1));
    const int y0 = int(fy), y1 = std::min(y0 + 1, bh - 1);
    const float ty = fy - float(y0);
    for (int x = 0; x < w; ++x) {
      const float fx = std::clamp((float(x) + 0.5f) / kDiv - 0.5f, 0.f, float(bw - 1));
      const int x0 = int(fx), x1 = std::min(x0 + 1, bw - 1);
      const float tx = fx - float(x0);
      const float* a = &s.bloomA[(size_t(y0) * size_t(bw) + size_t(x0)) * 3];
      const float* b = &s.bloomA[(size_t(y0) * size_t(bw) + size_t(x1)) * 3];
      const float* c = &s.bloomA[(size_t(y1) * size_t(bw) + size_t(x0)) * 3];
      const float* d = &s.bloomA[(size_t(y1) * size_t(bw) + size_t(x1)) * 3];
      float add[3];
      for (int k = 0; k < 3; ++k)
        add[k] = kGain * ((a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty);
      if (add[0] + add[1] + add[2] < 0.5f) continue;
      uint32_t& p = fb.pixels[size_t(y) * size_t(w) + size_t(x)];
      p = packPx(float((p >> 16) & 0xFF) + add[0], float((p >> 8) & 0xFF) + add[1], float(p & 0xFF) + add[2]);
    }
  }
}

void resolveSsaa2x(const SoftwareFramebuffer& hi, SoftwareFramebuffer& out) {
  const uint32_t w = out.width, h = out.height;
  if (hi.width != w * 2 || hi.height != h * 2) return;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const uint32_t p = hi.pixels[size_t(2 * y + dy) * hi.width + (2 * x + dx)];
          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
        }
      // Round-to-nearest integer box filter (same 64 from a single 255 as the
      // old float path: (255+2)/4 = 64). Deterministic across hosts.
      out.pixels[size_t(y) * w + x] =
          0xFF000000u | (((r + 2) >> 2) << 16) | (((g + 2) >> 2) << 8) | ((b + 2) >> 2);
    }
  }
}

}  // namespace litholattice
