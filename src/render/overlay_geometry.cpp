#include "litholattice/render/overlay_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include "font_data.h"

namespace litholattice {
namespace {

constexpr int kAtlasW = 16 * font::kCellW, kAtlasH = 6 * font::kCellH;

void quad(std::vector<HudVertex>& out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
          const std::array<float, 4>& c, float tex = 0.f) {
  const float q[6][4] = {{x, y, u0, v0}, {x + w, y, u1, v0}, {x + w, y + h, u1, v1},
                         {x, y, u0, v0}, {x + w, y + h, u1, v1}, {x, y + h, u0, v1}};
  for (const auto& v : q) out.push_back({v[0], v[1], v[2], v[3], c[0], c[1], c[2], c[3], tex});
}

void text(std::vector<HudVertex>& out, float x, float y, const std::string& s, const std::array<float, 4>& c,
          float scale) {
  const float gw = font::kCellW * scale, gh = font::kCellH * scale;
  const float du = float(font::kCellW) / kAtlasW, dv = float(font::kCellH) / kAtlasH;
  float cx = x;
  for (const char ch : s) {
    if (ch == '\n') { y += gh; cx = x; continue; }
    const int g = int(static_cast<unsigned char>(ch)) - font::kFirst;
    if (g > 0 && g < font::kCount) {
      const float u0 = float((g % 16) * font::kCellW) / kAtlasW, v0 = float((g / 16) * font::kCellH) / kAtlasH;
      quad(out, cx, y, gw, gh, u0, v0, u0 + du, v0 + dv, c);
    }
    cx += gw;
  }
}

/// Quad clipped to `c` (u, v interpolated); nothing if fully outside.
void clippedQuad(std::vector<HudVertex>& out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 const std::array<float, 4>& col, const HudClip& c, float tex = 0.f) {
  const float x0 = std::max(x, c.x0), y0 = std::max(y, c.y0), x1 = std::min(x + w, c.x1), y1 = std::min(y + h, c.y1);
  if (x1 <= x0 || y1 <= y0 || w <= 0 || h <= 0) return;
  const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  const float tu0 = (x0 - x) / w, tu1 = (x1 - x) / w, tv0 = (y0 - y) / h, tv1 = (y1 - y) / h;
  quad(out, x0, y0, x1 - x0, y1 - y0, lerp(u0, u1, tu0), lerp(v0, v1, tv0), lerp(u0, u1, tu1), lerp(v0, v1, tv1), col, tex);
}

/// One text draw rendered at an explicit origin and colour, so a caller
/// can render the same draw twice (drop shadow, then face) straight from
/// the source draw. The shadow pass used to copy the whole HudDraw
/// (string included) just to move it one HUD scale and blacken it (GUI
/// lag fix, item 2, 2026-11-09: the copy is gone; output is unchanged...
/// same origin, same colour, same clip, glyph for glyph).
void clippedText(std::vector<HudVertex>& out, const HudDraw& d, float x, float y,
                 const std::array<float, 4>& col) {
  const float gw = font::kCellW * d.scale, gh = font::kCellH * d.scale;
  const float du = float(font::kCellW) / kAtlasW, dv = float(font::kCellH) / kAtlasH;
  if (y + gh <= d.clip.y0 || y >= d.clip.y1) return;
  float cx = x;
  for (const char ch : d.text) {
    if (cx >= d.clip.x1) break;
    const int g = int(static_cast<unsigned char>(ch)) - font::kFirst;
    if (g > 0 && g < font::kCount && cx + gw > d.clip.x0) {
      const float u0 = float((g % 16) * font::kCellW) / kAtlasW, v0 = float((g / 16) * font::kCellH) / kAtlasH;
      clippedQuad(out, cx, y, gw, gh, u0, v0, u0 + du, v0 + dv, col, d.clip);
    }
    cx += gw;
  }
}

/// Content fingerprint for HudVertexCache (GUI lag fix, item 2): a
/// word-wise FNV-1a pass over every field buildHudVertices reads.
/// Floats mix by bit pattern; strings mix by length, then by bytes
/// packed into words; every vector's size mixes in first, so element
/// boundaries can never alias. A pure function of the layer's content:
/// identical content, identical key. The mixing runs over contiguous
/// float runs (one loop per run, not one call per field) because this
/// pass runs on every presented frame, in Debug builds too... it is
/// the cached path's whole per-frame cost.
uint64_t mixFloatRun(uint64_t h, const float* p, size_t n) {
  size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
  }
  if (i < n) {
    uint32_t u;
    std::memcpy(&u, p + i, 4);
    h = (h ^ uint64_t(u)) * 0x100000001b3ull;
  }
  return h;
}

uint64_t mixString(uint64_t h, const std::string& s) {
  h = (h ^ uint64_t(s.size())) * 0x100000001b3ull;
  size_t i = 0;
  for (; i + 8 <= s.size(); i += 8) {
    uint64_t w;
    std::memcpy(&w, s.data() + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
  }
  if (i < s.size()) {
    uint64_t w = 0;
    std::memcpy(&w, s.data() + i, s.size() - i);
    h = (h ^ w) * 0x100000001b3ull;
  }
  return h;
}

uint64_t hudContentKeyImpl(const HudLayer& hud) {
  uint64_t h = 0xcbf29ce484222325ull;
  // Textured icons (Phase B): the atlas version mixes in, so a layer
  // whose icon UVs address a retired snapshot can never key-collide
  // with the same draws addressing the new one.
  h = (h ^ hud.iconAtlasVersion) * 0x100000001b3ull;
  h = mixFloatRun(h, &hud.scale, 1);
  h = (h ^ uint64_t(hud.rects.size())) * 0x100000001b3ull;
  if (!hud.rects.empty())
    h = mixFloatRun(h, &hud.rects.front().x, hud.rects.size() * 8);  // HudRect is 8 floats, no padding
  h = (h ^ uint64_t(hud.texts.size())) * 0x100000001b3ull;
  for (const HudText& t : hud.texts) {
    h = mixFloatRun(h, &t.x, 2);
    h = mixString(h, t.text);
    h = mixFloatRun(h, &t.r, 4);
  }
  h = (h ^ uint64_t(hud.draws.size())) * 0x100000001b3ull;
  for (const HudDraw& d : hud.draws) {
    h = mixFloatRun(h, &d.x, 13);     // x..scale (9) + icon UV rect (4): adjacent floats
    h = (h ^ uint64_t(d.icon ? 1 : 0)) * 0x100000001b3ull;
    h = mixString(h, d.text);
    h = mixFloatRun(h, &d.clip.x0, 4);
  }
  return h;
}

}  // namespace

uint64_t hudContentKey(const HudLayer& hud) { return hudContentKeyImpl(hud); }

HudFontAtlas buildHudFontAtlas() {
  HudFontAtlas a;
  a.width = kAtlasW;
  a.height = kAtlasH;
  a.pixels.assign(size_t(kAtlasW) * kAtlasH, 0);
  for (int g = 0; g < font::kCount; ++g) {
    const int ox = (g % 16) * font::kCellW, oy = (g / 16) * font::kCellH;
    const char* s = font::kGlyphs[g];
    for (int y = 0; y < font::kCellH; ++y)
      for (int x = 0; x < font::kCellW; ++x) {
        const char ch = s[y * font::kCellW + x];
        const int v = ch <= '9' ? ch - '0' : ch - 'a' + 10;
        a.pixels[size_t(oy + y) * kAtlasW + size_t(ox + x)] = uint8_t(v * 17);
      }
  }
  return a;
}

void buildHudVertices(const HudLayer& hud, std::vector<HudVertex>& out) {
  // next-build-2 (GUI diagnosis Section 6): time the expansion into the
  // tickprof slot when profiling; one clock read pair, nothing else.
  const bool prof = tickprof::enabled();
  const int64_t profT0 = prof ? tickprof::nowNs() : 0;
  struct ExpandTimer {
    bool on; int64_t t0;
    ~ExpandTimer() { if (on) tickprof::hudExpandNs() = tickprof::nowNs() - t0; }
  } expandTimer{prof, profT0};
  (void)expandTimer;
  out.clear();
  for (const HudRect& r : hud.rects) quad(out, r.x, r.y, r.w, r.h, -1, -1, -1, -1, {r.r, r.g, r.b, r.a});
  const std::array<float, 4> shadow{0, 0, 0, 0.75f};
  // Cortana 2026-10-16 (JD crisp GUI): whole-pixel origin + integer glyph scale (no stretched texels).
  const float hs = hud.scale < 1.f ? 1.f : std::floor(hud.scale + 0.5f);
  for (const HudText& t : hud.texts) {
    const float tx = std::floor(t.x + 0.5f), ty = std::floor(t.y + 0.5f);
    text(out, tx + hs, ty + hs, t.text, shadow, hs);  // drop shadow for contrast
    text(out, tx, ty, t.text, {t.r, t.g, t.b, t.a}, hs);
  }
  for (const HudDraw& d : hud.draws) {
    if (d.icon && d.text.empty())
      // Textured icon (Phase B): one quad over the draw rect sampling
      // the layer's icon atlas; clippedQuad interpolates the UVs, so a
      // scroll-clipped icon at a palette edge stays exact.
      clippedQuad(out, d.x, d.y, d.w, d.h, d.iu0, d.iv0, d.iu1, d.iv1, {d.r, d.g, d.b, d.a}, d.clip, 1.f);
    else if (d.text.empty()) clippedQuad(out, d.x, d.y, d.w, d.h, -1, -1, -1, -1, {d.r, d.g, d.b, d.a}, d.clip);
    else {
      // 2026-11-08 (JD, layout diagnosis item 5): EVERY GUI text draw
      // gets the slight drop shadow the legacy HudText path above has
      // always had (same recipe: offset by the rounded HUD scale, 75%
      // black), so migrated-GUI text stays readable on bright
      // backgrounds. Both render backends consume buildHudVertices, so
      // this one pass covers Vulkan and software alike. The facade's
      // old per-label shadow draw retired the same day; this is the
      // single shadow path now (slot captions included).
      // 2026-11-09 (GUI lag fix, item 2): the shadow renders straight
      // from the source draw at its offset origin... no per-draw
      // HudDraw copy (string included) is made for it any more.
      clippedText(out, d, d.x + hs, d.y + hs, shadow);
      clippedText(out, d, d.x, d.y, {d.r, d.g, d.b, d.a});
    }
  }
}

bool buildHudVerticesCached(const HudLayer& hud, HudVertexCache& cache, std::vector<HudVertex>& out) {
  // GUI lag fix, item 2 (2026-11-09): the expansion stage's per-frame
  // cost, keyed on the layer's content. The profiling run convicted the
  // old shape: expand ran at full price on every presented frame
  // (settled in-world avg ~20 bench ms, max 72.72) because nothing
  // remembered the last expansion... including on frames where the
  // facade replayed an unchanged layer into the frame params.
  const bool prof = tickprof::enabled();
  const int64_t t0 = prof ? tickprof::nowNs() : 0;
  const uint64_t key = hudContentKeyImpl(hud);
  if (cache.valid && cache.key == key) {
    // Replay frame: `out` already holds this layer's expansion, vertex
    // for vertex. The frame's expand slot reads the fingerprint pass
    // alone, which is the cache's win made visible to the profile.
    if (prof) tickprof::hudExpandNs() = tickprof::nowNs() - t0;
    return false;
  }
  buildHudVertices(hud, out);
  cache.key = key;
  cache.valid = true;
  // Overwrite the expansion-only timing buildHudVertices just recorded
  // with this stage's full cost (fingerprint + expansion), so the
  // profile's expand field prices the cached entry point honestly on
  // miss frames too.
  if (prof) tickprof::hudExpandNs() = tickprof::nowNs() - t0;
  return true;
}

bool hudSlotRangeNeedsCopy(HudSlotRange& r, uint64_t contentKey, size_t count, size_t at) {
  const bool need = r.count != count || r.at != at || r.key != contentKey;
  r.key = contentKey;
  r.count = count;
  r.at = at;
  return need;
}

bool hudVertsForStream(const HudLayer& hud, uint64_t generation, HudStreamCache& state, std::vector<HudVertex>& out) {
  // GUI draw-cost program, cure 1 (2026-11-10): the generation fast
  // path. The producer (game facade) rebuilds a stream's layer only
  // when its generation advances, so a repeated generation at a valid
  // cache means `out` already holds this exact content: skip even the
  // fingerprint pass (measured 2026-11-10: the pass over the in-world
  // HUD's composed layer cost real time on every replayed frame at
  // 24.5k elements, and over the creative screen's far larger layer it
  // is proportionally worse). Correctness does not rest on the stamp:
  // any generation CHANGE falls through to the item-2 fingerprint,
  // which keys on content alone, so the worst case of a producer
  // mutating without stamping is one honest fingerprint per change of
  // stamp... and a producer mutating at an UNCHANGED stamp would be a
  // producer bug of the same class the old composed path already had
  // no defence against (its cache had exactly the same trust in `out`).
  if (generation != 0 && state.content.valid && state.builtGeneration == generation) {
    if (tickprof::enabled()) tickprof::hudExpandNs() = 0;
    return false;
  }
  const bool expanded = buildHudVerticesCached(hud, state.content, out);
  state.builtGeneration = generation;
  return expanded;
}

std::array<Vec3, 24> blockOutlineLines(int x, int y, int z) {
  const float e = 0.004f;
  const float x0 = float(x) - e, y0 = float(y) - e, z0 = float(z) - e;
  const float x1 = float(x) + 1 + e, y1 = float(y) + 1 + e, z1 = float(z) + 1 + e;
  return {{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x1, y0, z1}, {x0, y0, z1}, {x0, y0, z1}, {x0, y0, z0},
           {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y1, z1}, {x0, y1, z0},
           {x0, y0, z0}, {x0, y1, z0}, {x1, y0, z0}, {x1, y1, z0}, {x1, y0, z1}, {x1, y1, z1}, {x0, y0, z1}, {x0, y1, z1}}};
}

std::array<Vec3, 4> crosshairLines(int width, int height) {
  const float s = 0.025f, aspect = height > 0 ? float(width) / float(height) : 1.f;
  return {{{-s / aspect, 0, 0}, {s / aspect, 0, 0}, {0, -s, 0}, {0, s, 0}}};
}

void snapshotHudStreams(const FrameParams& src, FrameParams& job, HudStreamSnapshot& own) {
  // D9 (see overlay_geometry.h): the job never borrows a producer layer.
  auto one = [](const HudLayerRef& in, HudLayerRef& out, HudLayer& copy, const HudLayer*& from, uint64_t& gen,
                uint64_t& copies) {
    if (!in.layer) {
      out = {};
      from = nullptr;
      gen = 0;
      return;
    }
    if (in.layer != from || in.generation != gen) {
      copy = *in.layer;
      from = in.layer;
      gen = in.generation;
      ++copies;
    }
    out = {&copy, in.generation};
  };
  one(src.hudStatic, job.hudStatic, own.staticLayer, own.staticSrc, own.staticGen, own.staticCopies);
  one(src.hudDynamic, job.hudDynamic, own.dynamicLayer, own.dynamicSrc, own.dynamicGen, own.dynamicCopies);
}

}  // namespace litholattice
