#pragma once
// Cortana 2026-10-15 1530 (ENGINE, CloudAtlas CA-01): bleed RGB into fully transparent
// texels before GPU mip generation. Straight-alpha mips average the (usually black) RGB of
// alpha=0 texels into the visible colour, so thin or cutout art (Trace) goes dark and boxy at
// distance on GL33/Vulkan. Each pass gives every still-unfilled alpha=0 texel the mean RGB
// of its filled 8-neighbours; it repeats until nothing changes (at most max(w, h) passes).
// Alpha is never touched, so the cutout and blend results at mip 0 are unchanged. Used only
// by the GPU upload paths: the software renderer samples mip 0 with its own alpha rules and
// keeps its pixels bit-identical.
#include <cstdint>
#include <cstring>
#include <vector>

namespace litholattice {

inline void dilateTransparentRGB(uint8_t* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return;
  const size_t n = size_t(w) * size_t(h);
  std::vector<uint8_t> filled(n), next;
  size_t todo = 0;
  for (size_t i = 0; i < n; ++i) { filled[i] = rgba[i * 4 + 3] != 0; todo += !filled[i]; }
  if (todo == 0 || todo == n) return;  // fully opaque, or fully transparent (nothing to bleed from)
  const int maxPasses = w > h ? w : h;
  for (int pass = 0; pass < maxPasses && todo; ++pass) {
    next = filled;
    size_t changed = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        if (filled[i]) continue;
        unsigned r = 0, g = 0, b = 0, c = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = x + dx, yy = y + dy;
            if ((dx | dy) == 0 || xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            const size_t j = size_t(yy) * size_t(w) + size_t(xx);
            if (!filled[j]) continue;
            r += rgba[j * 4]; g += rgba[j * 4 + 1]; b += rgba[j * 4 + 2]; ++c;
          }
        if (!c) continue;
        rgba[i * 4] = uint8_t(r / c); rgba[i * 4 + 1] = uint8_t(g / c); rgba[i * 4 + 2] = uint8_t(b / c);
        next[i] = 1; ++changed;
      }
    filled.swap(next);
    todo -= changed;
    if (!changed) break;
  }
}

}  // namespace litholattice