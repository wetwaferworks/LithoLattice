#pragma once
// 2026-10-16 (JD leaf pass, ENGINE): coverage-preserving mips for alpha-tested (cutout) art.
// A plain box/linear mip averages alpha, so a 50 % leaf texture's holes and leaves blur
// into alpha ~0.5 and the 0.5 alpha test eats the canopy at distance (or fattens it).
// Castano's fix: after each downsample, scale that level's alpha so the fraction of texels
// passing the test (a >= 128) matches level 0. Only layers that opt in use it (see
// coverageMipLayer); every other layer keeps its old mips, so other scenes don't change.
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace litholattice {

/// Content opt-in: binary-alpha leaf art. Matched by texture name ("_leaves_" or "leaves_").
inline bool coverageMipLayer(std::string_view name) {
  return name.find("leaves") != std::string_view::npos && name.find("_opaque") == std::string_view::npos &&
         name.find("_glow") == std::string_view::npos;
}

/// Fraction of texels with alpha >= cutoff (RGBA8, w*h texels).
inline float alphaCoverage(const uint8_t* rgba, int w, int h, int cutoff = 128, float scale = 1.f) {
  int pass = 0;
  for (int i = 0; i < w * h; ++i) pass += std::min(255.f, rgba[i * 4 + 3] * scale) >= float(cutoff);
  return w * h ? float(pass) / float(w * h) : 0.f;
}

/// Scales alpha in place so alphaCoverage() matches `target` as closely as possible.
inline void preserveAlphaCoverage(uint8_t* rgba, int w, int h, float target, int cutoff = 128) {
  float lo = 0.f, hi = 8.f, best = 1.f, bestErr = 2.f;
  for (int it = 0; it < 20; ++it) {
    const float mid = 0.5f * (lo + hi), c = alphaCoverage(rgba, w, h, cutoff, mid);
    const float err = c > target ? c - target : target - c;
    if (err < bestErr) { bestErr = err; best = mid; }
    if (c < target) lo = mid; else hi = mid;
  }
  for (int i = 0; i < w * h; ++i) rgba[i * 4 + 3] = uint8_t(std::min(255.f, rgba[i * 4 + 3] * best + 0.5f));
}

/// 2x2 box downsample of a square RGBA8 level (size >= 2) into `out` (size/2 squared).
inline void boxDownsample(const uint8_t* in, int size, std::vector<uint8_t>& out) {
  const int n = std::max(1, size / 2);
  out.assign(size_t(n) * n * 4, 0);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x)
      for (int c = 0; c < 4; ++c) {
        const int x0 = std::min(size - 1, 2 * x), x1 = std::min(size - 1, 2 * x + 1);
        const int y0 = std::min(size - 1, 2 * y), y1 = std::min(size - 1, 2 * y + 1);
        const int s = in[(y0 * size + x0) * 4 + c] + in[(y0 * size + x1) * 4 + c] + in[(y1 * size + x0) * 4 + c] +
                      in[(y1 * size + x1) * 4 + c];
        out[(size_t(y) * n + x) * 4 + c] = uint8_t((s + 2) / 4);
      }
}

/// Mip levels 1..(bit_width(size)-1) of a square, already RGB-dilated level 0, each with
/// level 0's alpha-test coverage. levels[i] is level i+1.
inline std::vector<std::vector<uint8_t>> coverageMips(const uint8_t* level0, int size, int cutoff = 128) {
  std::vector<std::vector<uint8_t>> levels;
  const float target = alphaCoverage(level0, size, size, cutoff);
  const uint8_t* prev = level0;
  for (int s = size; s > 1; s /= 2) {
    std::vector<uint8_t> next;
    boxDownsample(prev, s, next);
    preserveAlphaCoverage(next.data(), s / 2, s / 2, target, cutoff);
    levels.push_back(std::move(next));
    prev = levels.back().data();
  }
  return levels;
}

}  // namespace litholattice
