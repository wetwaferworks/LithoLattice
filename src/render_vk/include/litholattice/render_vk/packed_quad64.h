#pragma once
// Mega-Pipeline Phase 2 (Direction JD, design Archie (Mega-Pipeline brief), implementation Cortana): 64-bit packed
// quads read by vertex pulling (LL_VK_PULL=1, needs LL_VK_MEGA=3). Render-only; CPU meshing and every sim hash
// are unchanged.
//
// Deviations from the brief (noted in docs/roadmap/Report-for-Archie-MegaPipeline-2026-10-25.md):
//  - Only full-cell, axis-aligned quads are packed (all corners on 1/16-unit multiples of 16, one face, one
//    material). Sub-block, rotated-UV, foliage and other geometry stays on Packed12 in the same slice.
//  - The quad stores a 15-bit backward distance to a per-mesh material entry (layer 16, value 8, mode 8,
//    UV origin and UV sign map, faceAo high bits) instead of 16 material bits in the quad itself; bit
//    order differs from the brief's table. Every packed quad is decoded again on the CPU and kept only when
//    its four Packed12 vertices come back bit-identical, so the picture cannot change.
//  - Alpha-blend faces stay on Packed12 in the fallback range (Phase 4 B blend split); the rest of a blended
//    mesh is pulled. LL_VK_BLENDSPLIT=0 keeps blended meshes entirely on Packed12 (sorted blend pass).
//
// Quad (uvec2 lo, hi):
//   lo:  0-5 ox | 6-11 oy | 12-17 oz (cells) | 18-23 W | 24-29 H (cells, along axes a=(n+1)%3, b=(n+2)%3)
//        | 30-31 face bits 0-1
//   hi:  0 face bit 2 | 1-8 corner pattern (2 bits per corner k: ca, cb) | 9-16 AO (2 bits per corner)
//        | 17-31 distance back to the material entry (in 8-byte units)
// Material entry (uvec2 x, y):
//   x: layer 0-15 | value 16-23 | mode 24-31
//   y: U0 0-7 | V0 8-15 | UV map 16-23 (2-bit codes su_a, su_b, sv_a, sv_b: 0 = 0, 1 = +1, 2 = -1) | faceAo>>5 24-26
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "litholattice/render_vk/packed_vertex12.h"
// Review R2 (fix pass 2026-10-25): encodePulled compares decoded vertices with memcmp, which is only an exact
// value check while GpuVertex12 is 12 padding-free bytes.
static_assert(sizeof(litholattice::vk::GpuVertex12) == 12, "GpuVertex12 must stay 12 bytes");
static_assert(std::has_unique_object_representations_v<litholattice::vk::GpuVertex12>, "GpuVertex12 must have no padding (memcmp check)");

namespace litholattice::vk {

struct PulledMesh {
  std::vector<PackedVertex> fallback;  ///< quads (4 verts each) left on Packed12
  std::vector<uint64_t> palette;       ///< material entries (x | y << 32)
  std::vector<uint64_t> quads;         ///< packed quads, distance field still 0 (set by writePulled)
  std::vector<uint32_t> quadMat;       ///< palette index per quad
  uint32_t bucket[7] = {};             ///< Phase 3: quads sorted by face; bucket f = [bucket[f], bucket[f+1])
  bool active = false;                 ///< renderer: this slice uses the pulled layout
};

inline int pullSign(uint32_t c) { return c == 1u ? 1 : (c == 2u ? -1 : 0); }

/// Decodes corner k of a packed quad + its material entry into a Packed12 vertex (mirrors chunk_vert_body.glsl).
inline GpuVertex12 decodePulled(uint64_t q, uint64_t e, uint32_t k) {
  const uint32_t lo = uint32_t(q), hi = uint32_t(q >> 32), ex = uint32_t(e), ey = uint32_t(e >> 32);
  uint32_t p[3] = {lo & 63u, (lo >> 6) & 63u, (lo >> 12) & 63u};
  const uint32_t W = (lo >> 18) & 63u, H = (lo >> 24) & 63u, face = (lo >> 30) | ((hi & 1u) << 2);
  const uint32_t ca = (hi >> (1u + 2u * k)) & 1u, cb = (hi >> (2u + 2u * k)) & 1u, ao = (hi >> (9u + 2u * k)) & 3u;
  const uint32_t n = face >> 1, a = (n + 1u) % 3u, b = (n + 2u) % 3u;
  p[a] += ca * W; p[b] += cb * H;
  const uint32_t m = ey >> 16;
  const int du = (pullSign(m & 3u) * int(ca * W) + pullSign((m >> 2) & 3u) * int(cb * H)) * 16;
  const int dv = (pullSign((m >> 4) & 3u) * int(ca * W) + pullSign((m >> 6) & 3u) * int(cb * H)) * 16;
  GpuVertex12 g{};
  g.x = uint16_t(p[0] * 16u); g.y = uint16_t(p[1] * 16u);
  const uint32_t layer = ex & 0xFFFFu;
  g.zl = uint16_t((p[2] * 16u) | ((layer >> 8) << 9));
  g.u = uint8_t(uint32_t(int(ey & 255u) + du) & 255u);
  g.v = uint8_t(uint32_t(int((ey >> 8) & 255u) + dv) & 255u);
  g.faceAo = uint8_t(face | (ao << 3) | (((ey >> 24) & 7u) << 5));
  g.layerLo = uint8_t(layer & 255u);
  g.value = uint8_t((ex >> 16) & 255u);
  g.mode = uint8_t(ex >> 24);
  return g;
}

/// Tries to pack one quad. Returns false (leave it on Packed12) unless the decode is bit-identical.
/// Phase 4.0 quad-rejection counter (for Archie): why a quad stayed on Packed12. Indexes into a
/// kPullReasons array; render-side statistics only.
enum PullReason : uint32_t { kPullOk = 0, kPullBadFace = 1, kPullSubCell = 2, kPullOutOfRange = 3, kPullDegenerate = 4,
                             kPullNoOrigin = 5, kPullUvMismatch = 6, kPullBlendMesh = 7, kPullDistance = 8, kPullBlock = 9,
                             kPullReasons = 10 };
inline const char* pullReasonName(uint32_t r) {
  static const char* n[kPullReasons] = {"ok", "bad-face", "sub-cell", "out-of-range", "degenerate", "no-origin",
                                        "uv-or-bit-mismatch", "blend-mesh", "distance-overflow", "block>=64"};
  return r < kPullReasons ? n[r] : "?";
}
inline bool pullReject(uint32_t* why, uint32_t r) { if (why) ++why[r]; return false; }
/// Phase 4 C (Archie Q3): unique sub-cell tuple census. Tuple = face | n-nibble | a-min/a-max | b-min/b-max nibbles
/// (1/16-unit offsets inside the cell). Counted only when the reason counters are on (LL_STREAM_COUNTERS) and LL_VK_SUBCELL_CENSUS=1.
struct SubCellCensus {
  std::mutex mu;
  std::unordered_map<uint32_t, uint64_t> tuples;
  std::unordered_map<uint64_t, uint64_t> raw;
};
inline SubCellCensus& subCellCensus() { static SubCellCensus s; return s; }
inline bool subCellCensusOn() { static const bool on = [] { const char* e = std::getenv("LL_VK_SUBCELL_CENSUS"); return e && *e == '1'; }(); return on; }
inline void noteSubCell(const GpuVertex12* g) {
  if (!subCellCensusOn()) return;
  const uint32_t face = g[0].faceAo & 7u, n = face >> 1, a = (n + 1u) % 3u, b = (n + 2u) % 3u;
  uint32_t mn[3] = {0xFFFFu, 0xFFFFu, 0xFFFFu}, mx[3] = {0, 0, 0}; uint64_t rw = 0;
  for (int k = 0; k < 4; ++k) {
    const uint32_t xyz[3] = {g[k].x, g[k].y, uint32_t(g[k].zl & kPacked12MaxZ)};
    for (int i = 0; i < 3; ++i) { mn[i] = std::min(mn[i], xyz[i]); mx[i] = std::max(mx[i], xyz[i]); rw = (rw << 4) | (xyz[i] & 15u); }
  }
  const uint32_t t = face | ((mn[n] & 15u) << 3) | ((mn[a] & 15u) << 7) | ((mx[a] & 15u) << 11) | ((mn[b] & 15u) << 15) |
                     ((mx[b] & 15u) << 19) | (uint32_t(mn[n] != mx[n]) << 23);
  SubCellCensus& s = subCellCensus(); std::lock_guard<std::mutex> lk(s.mu); ++s.tuples[t];
  // Review C5 (fix pass 2026-10-25): the raw map only feeds a count; cap it so a long census cannot grow without bound.
  constexpr size_t kRawCap = size_t(1) << 20;
  if (const uint64_t key = rw | (uint64_t(face) << 48); s.raw.size() < kRawCap || s.raw.count(key)) ++s.raw[key];
}
inline bool encodePulled(const PackedVertex* v, uint64_t& qOut, uint64_t& eOut, uint32_t* why = nullptr) {
  GpuVertex12 g[4];
  for (int k = 0; k < 4; ++k) packVertex12(v[k], g[k]);
  const uint32_t face = g[0].faceAo & 7u;
  if (face > 5u) return pullReject(why, 1);
  uint32_t c[4][3];
  for (int k = 0; k < 4; ++k) {
    const uint32_t xyz[3] = {g[k].x, g[k].y, uint32_t(g[k].zl & kPacked12MaxZ)};
    for (int i = 0; i < 3; ++i) {
      if (xyz[i] & 15u) { if (why) noteSubCell(g); return pullReject(why, 2); }
      c[k][i] = xyz[i] >> 4;
      if (c[k][i] > 63u) return pullReject(why, 3);
    }
  }
  const uint32_t n = face >> 1, a = (n + 1u) % 3u, b = (n + 2u) % 3u;
  uint32_t mina = 99, maxa = 0, minb = 99, maxb = 0;
  for (int k = 0; k < 4; ++k) {
    mina = std::min(mina, c[k][a]); maxa = std::max(maxa, c[k][a]);
    minb = std::min(minb, c[k][b]); maxb = std::max(maxb, c[k][b]);
  }
  const uint32_t W = maxa - mina, H = maxb - minb;
  if (W == 0 || H == 0 || W > 63 || H > 63) return pullReject(why, 4);
  uint32_t o[3]; o[n] = c[0][n]; o[a] = mina; o[b] = minb;
  uint32_t pat = 0, ao = 0, k0 = 4;
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t ca = c[k][a] == maxa ? 1u : 0u, cb = c[k][b] == maxb ? 1u : 0u;
    pat |= (ca | (cb << 1)) << (2 * k);
    ao |= ((g[k].faceAo >> 3) & 3u) << (2 * k);
    if (!ca && !cb) k0 = k;
  }
  if (k0 == 4) return pullReject(why, 5);
  const uint32_t layer = uint32_t(g[0].layerLo) | (uint32_t(g[0].zl >> 9) << 8);
  const uint32_t ex = layer | (uint32_t(g[0].value) << 16) | (uint32_t(g[0].mode) << 24);
  const uint64_t q = uint64_t(o[0] | (o[1] << 6) | (o[2] << 12) | (W << 18) | (H << 24) | ((face & 3u) << 30)) |
                     (uint64_t((face >> 2) | (pat << 1) | (ao << 9)) << 32);
  static constexpr uint32_t kCodes[3] = {0, 1, 2};
  for (uint32_t sua : kCodes) for (uint32_t sub : kCodes) for (uint32_t sva : kCodes) for (uint32_t svb : kCodes) {
    const uint32_t ey = uint32_t(g[k0].u) | (uint32_t(g[k0].v) << 8) | ((sua | (sub << 2) | (sva << 4) | (svb << 6)) << 16) |
                        (uint32_t(g[0].faceAo >> 5) << 24);
    const uint64_t e = uint64_t(ex) | (uint64_t(ey) << 32);
    bool ok = true;
    for (uint32_t k = 0; k < 4 && ok; ++k) {
      const GpuVertex12 d = decodePulled(q, e, k);
      ok = std::memcmp(&d, &g[k], sizeof d) == 0;
    }
    if (ok) { qOut = q; eOut = e; return true; }
  }
  return pullReject(why, 6);
}

/// Splits a mesh's first `maxQuads` quads into packed quads (by face bucket) and Packed12 fallback quads.
inline void buildPulled(const std::vector<PackedVertex>& verts, uint32_t maxQuads, bool allow, PulledMesh& out,
                        uint32_t* why = nullptr, int blendMode = -1) {
  // Phase 4 B blend split (Archie Q1): blendMode >= 0 keeps quads of that shade mode (alpha-blend faces) on
  // Packed12 so the rest of a blended mesh can still be pulled; the blend pass draws only the fallback range.
  out.fallback.clear(); out.palette.clear(); out.quads.clear(); out.quadMat.clear();
  const uint32_t nq = std::min<uint32_t>(uint32_t(verts.size() / 4), maxQuads);
  std::unordered_map<uint64_t, uint32_t> pal;
  std::vector<uint64_t> qs[6];
  std::vector<uint32_t> qm[6];
  for (uint32_t i = 0; i < nq; ++i) {
    const PackedVertex* v = &verts[size_t(i) * 4];
    uint64_t q = 0, e = 0;
    const bool blendQuad = blendMode >= 0 && (v[0].mode & 63) == uint32_t(blendMode);
    if (blendQuad && why) ++why[kPullBlendMesh];
    if (allow && !blendQuad && encodePulled(v, q, e, why)) {
      auto [it, added] = pal.try_emplace(e, uint32_t(out.palette.size()));
      if (added) out.palette.push_back(e);
      const uint32_t face = uint32_t((q >> 30) & 3u) | uint32_t(((q >> 32) & 1u) << 2);
      qs[face].push_back(q); qm[face].push_back(it->second);
    } else {
      out.fallback.insert(out.fallback.end(), v, v + 4);
    }
  }
  for (uint32_t f = 0; f < 6; ++f) {
    out.bucket[f] = uint32_t(out.quads.size());
    out.quads.insert(out.quads.end(), qs[f].begin(), qs[f].end());
    out.quadMat.insert(out.quadMat.end(), qm[f].begin(), qm[f].end());
  }
  out.bucket[6] = uint32_t(out.quads.size());
  if (out.palette.size() + out.quads.size() > 32767u) {  // distance field is 15 bits: keep everything on Packed12
    if (why) why[kPullDistance] += uint32_t(out.quads.size());
    out.fallback.assign(verts.begin(), verts.begin() + std::ptrdiff_t(nq) * 4);
    out.palette.clear(); out.quads.clear(); out.quadMat.clear();
    for (uint32_t& b : out.bucket) b = 0;
  }
}

/// Bytes the slice needs: Packed12 fallback verts, then the palette, then the quads (8-byte aligned).
inline size_t pulledBytes(const PulledMesh& m) {
  return m.fallback.size() * sizeof(GpuVertex12) + (m.palette.size() + m.quads.size()) * 8;
}

/// Writes the slice payload. Returns the number of Packed12 clamps.
inline uint32_t writePulled(const PulledMesh& m, uint8_t* dst) {
  const uint32_t misses = packVertices12(m.fallback.data(), m.fallback.size(), dst);
  uint8_t* p = dst + m.fallback.size() * sizeof(GpuVertex12);
  std::memcpy(p, m.palette.data(), m.palette.size() * 8);
  p += m.palette.size() * 8;
  const uint32_t pn = uint32_t(m.palette.size());
  for (size_t i = 0; i < m.quads.size(); ++i) {
    const uint64_t dist = uint64_t(pn + uint32_t(i) - m.quadMat[i]);
    const uint64_t q = m.quads[i] | (dist << 49);
    std::memcpy(p + i * 8, &q, 8);
  }
  return misses;
}

}  // namespace litholattice::vk
