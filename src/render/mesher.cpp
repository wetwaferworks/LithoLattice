// Cortana 2026-10-15 1020: RND-H1 per-job def table
#include <algorithm>
#include <atomic>
#include <cstdlib>  // Cortana 2026-10-15 1705: LL_MESH_GREEDY
#include <cstring>
#include <memory>
#include <vector>
#include "litholattice/core/log.h"  // Cortana 2026-10-16 2335 F55-2
#include "litholattice/render/mesher.h"
#include "litholattice/render/sw_lod_rings.h"  // Archie Phase 1: canonical rings bounded to the view
#include "litholattice/render/unified_mesher.h"  // render pipeline M2
#include "litholattice/render/FallbackReasonStats.h"  // fix (a): per-reason fallback counters
#include "WorkerScratch.h"  // refactor step 2: per-worker scratch (owns GreedyGrid)
#include "MeshPipeline.h"  // refactor step 4: meshSubChunkPass stages
#include "MesherSelector.h"  // refactor step 5: the LL_* mesher env reads
#include "MesherKernels.h"
#include "UnifiedMesherCounters.h"  // refactor step 4: shared mesher internals
#include <bit>
#include "litholattice/render/trace_ramps.h"  // Cortana 2026-10-15 1600: Trace-mix pulse colour
#include "litholattice/render/block_materials.h"
#include "litholattice/render/block_appearance.h"
#include "litholattice/core/log.h"

#include <cstdlib>
static int llP2PerfOff() { static const int v = litholattice::cMesherSelector::envP2PerfOff(); return v; }  // Cortana 2026-10-16 1520: A/B perf switch (render/display only)

#include <cstdio>
namespace litholattice {
namespace {

constexpr int P = kMeshPad;
inline int pidx(int x, int y, int z) { return meshPadIndex(x, y, z); }

// 2026-10-18 (Crew Canopy, F-B): sparse-leaf density model (see mesher.h).
// A leaf voxel is "far" when its column's Chebyshev distance from the focus
// column exceeds kFoliageNearChunks; a far leaf voxel is kept iff its splitmix
// hash lands below the density fraction. Pure function of the world position,
// so both sides of a leaf-to-leaf interface always agree on the verdict.
constexpr int kFoliageNearChunks = 2;
constexpr uint64_t kFoliageKeepNum = 512, kFoliageKeepDen = 1024;  // density 1/2
inline uint64_t foliageHash(int wx, int wy, int wz) {
  uint64_t z = uint64_t(packPos(wx, wy, wz)) * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Face { int n[3], u[3], v[3]; };
// u x v == n, so corners (0,0),(1,0),(1,1),(0,1) wind CCW seen from outside.
constexpr Face kFaces[6] = {
  {{ 1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  // +X (east)
  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X (west)
  {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  // +Y
  {{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
  {{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  // +Z (south)
  {{ 0, 0,-1}, {0, 1, 0}, {1, 0, 0}},  // -Z (north)
};
constexpr int kCorner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
// kFaceDir: cMesherKernels::kFaceDir (MesherKernels.h)

struct Look {
  uint16_t layer = 0;  // Cortana 2026-10-15 1628: CA-03 [ENGINE]
  uint8_t value = 0, mode = kShadeTex, rot = 0;
  UvRect uv;
  uint32_t slot = 0;  ///< Step 1: strength-buffer slot (trace faces)
};

Look lookOf(const FaceMaterial& m, uint8_t mode, uint8_t value = 0, uint8_t rot = 0) {
  return Look{m.layer, value, mode, rot, m.uv};
}

// Maps a whole-tile coordinate (0..16) into the face's UV rect.
uint8_t inRect(int t, uint8_t lo, uint8_t hi) { return uint8_t(lo + t * (hi - lo) / 16); }

// Texture UV (1/16 units) for a corner at block-local p (0..16) on face f, rotated r quarter turns.
void faceUV(int f, const int p[3], int r, int& u, int& v) {
  switch (f) {
    case 0: u = 16 - p[2]; v = 16 - p[1]; break;
    case 1: u = p[2]; v = 16 - p[1]; break;
    case 4: u = p[0]; v = 16 - p[1]; break;
    case 5: u = 16 - p[0]; v = 16 - p[1]; break;
    case 2: u = p[0]; v = p[2]; break;
    default: u = p[0]; v = 16 - p[2]; break;
  }
  for (int i = 0; i < (r & 3); ++i) { const int t = u; u = v; v = 16 - t; }
}

void emitFace(std::vector<PackedVertex>& out, int f, const int mn[3], const int mx[3], const int blk[3],
              const int ao[4], const Look& lk) {
  const Face& F = kFaces[f];
  PackedVertex q[4];
  for (int c = 0; c < 4; ++c) {
    int pos[3], local[3];
    for (int a = 0; a < 3; ++a) {
      if (F.n[a]) pos[a] = F.n[a] > 0 ? mx[a] : mn[a];
      else if (F.u[a]) pos[a] = kCorner[c][0] ? mx[a] : mn[a];
      else pos[a] = kCorner[c][1] ? mx[a] : mn[a];
      local[a] = pos[a] - blk[a];
    }
    // Cortana 2026-10-16 2335 F55-2 (Discovered by Argon bit-drop review, Fixed by Cortana): a box-model min below 0
    // in the sub-chunk's first cell went negative here and wrapped to uint16 65520+ (~4095 blocks away).
    // Clamp to the sub-chunk edge and log once; only geometry that was already broken changes.
    for (int a = 0; a < 3; ++a)
      if (pos[a] < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) logWarn("mesher: box model face reaches %d on axis %d (below the sub-chunk); clamped to 0", pos[a], a);
        pos[a] = 0;
      }
    int u, v; faceUV(f, local, lk.rot, u, v);
    q[c] = {uint16_t(pos[0]), uint16_t(pos[1]), uint16_t(pos[2]), inRect(u, lk.uv.u0, lk.uv.u1), inRect(v, lk.uv.v0, lk.uv.v1),
            uint8_t(f | (ao[c] << 3) | ((lk.rot & 3) << 5)), uint8_t(lk.layer), lk.value, lk.mode, lk.slot};  // Cortana 2026-10-16 2335 F55-1: rot masked (bit-drop review)
    q[c].layerHi = uint8_t(lk.layer >> 8);  // Cortana 2026-10-15 1628: CA-03 [ENGINE]
  }
  const int start = (ao[0] + ao[2] < ao[1] + ao[3]) ? 1 : 0;  // flip diagonal for AO
  for (int c = 0; c < 4; ++c) out.push_back(q[(start + c) & 3]);
}

// Cross sprite (Shape::Cross): two crossed vertical quads, both sides.
void emitCross(std::vector<PackedVertex>& out, const int b[3], const Look& lk) {
  const int d[2][4] = {{0, 0, 16, 16}, {0, 16, 16, 0}};  // x0 z0 x1 z1
  for (auto& q : d)
    for (int side = 0; side < 2; ++side) {
      PackedVertex v[4];
      const int xs[4] = {q[0], q[2], q[2], q[0]}, zs[4] = {q[1], q[3], q[3], q[1]}, ys[4] = {0, 0, 16, 16};
      for (int c = 0; c < 4; ++c)
        v[c] = {uint16_t(b[0] + xs[c]), uint16_t(b[1] + ys[c]), uint16_t(b[2] + zs[c]), uint8_t(c == 1 || c == 2 ? 16 : 0),
                uint8_t(c >= 2 ? 0 : 16), uint8_t(2 | (3 << 3)), uint8_t(lk.layer), lk.value, lk.mode, lk.slot};
      for (int c = 0; c < 4; ++c) v[c].layerHi = uint8_t(lk.layer >> 8);  // Cortana 2026-10-15 1628: CA-03 [ENGINE]
      if (side) { out.push_back(v[0]); out.push_back(v[1]); out.push_back(v[2]); out.push_back(v[3]); }
      else { out.push_back(v[3]); out.push_back(v[2]); out.push_back(v[1]); out.push_back(v[0]); }
    }
}

// Cortana 2026-10-15 1705: greedy meshing [ENGINE]. Full-cube faces whose look is
// plain (Tex / Emissive / GroundPulse), unrotated, full-tile UV, uniform AO and no
// strength slot are parked in a per-direction grid instead of emitted; after the
// sub-chunk pass, each slice is merged greedily (widest run along texture-u, then
// as many matching rows along texture-v) into quads of at most 15x15 blocks, the
// u/v range a uint8 1/16-unit coordinate can hold (15 * 16 = 240). A 1x1 result
// goes through emitFace, so it is byte-identical to the unmerged face. Strip
// (Trace), readout, pips/cells, palette, cutout, glass and animated faces never
// merge, so the Trace-mix quad counts and every per-cell look are unchanged.
constexpr int kTexUAxis[6] = {2, 2, 0, 0, 0, 0};  // faceUV(): axis texture-u is taken from
constexpr int kTexVAxis[6] = {1, 1, 2, 2, 1, 1};  // faceUV(): axis texture-v is taken from
constexpr int kGreedyMaxTiles = 15;

// GreedyGrid: see GreedyGrid.h (refactor step 2).

bool greedyEligible(const Look& lk, const int ao[4]) {
  return (lk.mode == kShadeTex || lk.mode == kShadeEmissive || lk.mode == kShadeGroundPulse) && lk.rot == 0 &&
         lk.slot == 0 && lk.uv == UvRect{} && ao[0] == ao[1] && ao[1] == ao[2] && ao[2] == ao[3];
}

uint64_t greedyKey(const Look& lk, int ao) {
  return (uint64_t(1) << 63) | (uint64_t(lk.layer) << 24) | (uint64_t(lk.value) << 16) | (uint64_t(lk.mode) << 8) |
         uint64_t(ao & 3);
}

void park(GreedyGrid& g, int f, const int blk[3], const Look& lk, int ao) {
  const int ua = kTexUAxis[f], va = kTexVAxis[f], na = 3 - ua - va;
  g.key[f][blk[na]][blk[va]][blk[ua]] = greedyKey(lk, ao);
  g.sliceUsed[f] = uint16_t(g.sliceUsed[f] | (1u << blk[na]));
}

void emitGreedyQuad(std::vector<PackedVertex>& out, int f, int slice, int iu, int iv, int w, int h, uint64_t key) {
  const int ua = kTexUAxis[f], va = kTexVAxis[f], na = 3 - ua - va;
  int mn[3], mx[3];
  mn[na] = slice * 16; mx[na] = mn[na] + 16;
  mn[ua] = iu * 16; mx[ua] = (iu + w) * 16;
  mn[va] = iv * 16; mx[va] = (iv + h) * 16;
  Look lk;
  lk.layer = uint16_t((key >> 24) & 0xFFFFu);
  lk.value = uint8_t((key >> 16) & 0xFFu);
  lk.mode = uint8_t((key >> 8) & 0xFFu);
  const int a = int(key & 3);
  const int ao[4] = {a, a, a, a};
  const size_t first = out.size();
  emitFace(out, f, mn, mx, mn, ao, lk);
  if (w == 1 && h == 1) return;  // byte-identical to the unmerged face
  // Re-derive each corner's UV from its 0/16 single-tile value, scaled by the
  // run length along that texture axis (emitFace assumed a one-block face).
  for (size_t i = first; i < out.size(); ++i) {
    PackedVertex& pv = out[i];
    const int pos[3] = {pv.x, pv.y, pv.z};
    int local[3];
    for (int k = 0; k < 3; ++k) local[k] = pos[k] > mn[k] ? 16 : 0;
    int u, v;
    faceUV(f, local, 0, u, v);
    pv.u = uint8_t(u * w);
    pv.v = uint8_t(v * h);
  }
}

}  // namespace (refactor step 4: cMesherKernels member, see MesherKernels.h)
void cMesherKernels::flushGreedy(GreedyGrid& g, std::vector<PackedVertex>& out) {
  for (int f = 0; f < 6; ++f) {
    const uint16_t used = g.sliceUsed[f];
    g.sliceUsed[f] = 0;
    for (int s = 0; s < 16; ++s) {
      if (!(used & (1u << s))) continue;
      auto& k = g.key[f][s];
      for (int iv = 0; iv < 16; ++iv)
        for (int iu = 0; iu < 16; ++iu) {
          const uint64_t key = k[iv][iu];
          if (!key) continue;
          int w = 1;
          while (iu + w < 16 && w < kGreedyMaxTiles && k[iv][iu + w] == key) ++w;
          int h = 1;
          for (; iv + h < 16 && h < kGreedyMaxTiles; ++h) {
            bool row = true;
            for (int t = 0; t < w && row; ++t) row = k[iv + h][iu + t] == key;
            if (!row) break;
          }
          for (int dv = 0; dv < h; ++dv)
            for (int t = 0; t < w; ++t) k[iv + dv][iu + t] = 0;  // consumed: grid is all-zero after the flush
          emitGreedyQuad(out, f, s, iu, iv, w, h, key);
        }
    }
  }
}
namespace {


// Render pipeline M2 (2026-10-29, Archie migration step 3 on the game path): canonical
// flush. In unified-mesher mode every greedy-eligible face is parked (on every backend)
// and this pass merges the parked slices with the bitwise kernel (countr_zero /
// countr_one over 16-bit rows, integer key compares), writes the CanonicalMesh L0 face
// buckets (palette slot per look, first-seen, only on emit), and then DECODES the
// canonical quads back into PackedVertex for today's uploaders: merged quads when the
// backend prefers greedy meshes, one face per cell otherwise. The legacy vertex stream
// is therefore produced from the canonical bytes, which is what the decode-equivalence
// check in meshSubChunk compares against the legacy pass.
// Look -> palette: layer -> layerId; Emissive -> emissive 15; GroundPulse -> mode flag
// + pulse = value. A value > 15 cannot ride the palette: that mesh keeps its vertices
// (built from the keys) but publishes no CanonicalMesh (counted).
uint64_t canonPaletteOf(uint64_t key, bool& ok) {
  canon::PaletteFields p;
  p.layerId = uint16_t((key >> 24) & 0xFFFFu);
  const uint8_t value = uint8_t((key >> 16) & 0xFFu), mode = uint8_t((key >> 8) & 0xFFu);
  if (value > 15) ok = false;
  p.pulse = uint8_t(value & 15u);
  if (mode == kShadeEmissive) p.emissive = 15;
  else if (mode == kShadeGroundPulse) p.modeFlags = uint8_t(canon::kModeGroundPulse);
  else if (mode != kShadeTex) ok = false;
  return canon::packPalette(p);
}
uint64_t canonKeyOf(const canon::DecodedQuad& q, uint64_t pal) {
  const canon::PaletteFields p = canon::unpackPalette(pal);
  const uint8_t mode = (p.modeFlags & canon::kModeGroundPulse) ? uint8_t(kShadeGroundPulse)
                       : p.emissive == 15                    ? uint8_t(kShadeEmissive)
                                                             : uint8_t(kShadeTex);
  return (uint64_t(1) << 63) | (uint64_t(p.layerId) << 24) | (uint64_t(p.pulse) << 16) | (uint64_t(mode) << 8) |
         uint64_t(q.cornerAo & 3u);
}

}  // namespace (refactor step 4: cMesherKernels member, see MesherKernels.h)
void cMesherKernels::flushCanonical(GreedyGrid& g, std::vector<PackedVertex>& out, CanonicalMesh& cm, bool merged) {
  std::vector<uint32_t>& t_qSlot = cWorkerScratch::forThisThread().qSlot;  // M11 (refactor step 2: scratch-owned)
  cm = CanonicalMesh{};
  t_qSlot.clear();
  bool ok = true;
  std::vector<uint64_t> lookKeys;  // parallel to cm.palette (first-seen)
  for (int f = 0; f < 6; ++f) {
    const uint32_t start = uint32_t(cm.quads.size());
    const uint16_t used = g.sliceUsed[f];
    g.sliceUsed[f] = 0;
    const int ua = kTexUAxis[f], va = kTexVAxis[f], na = 3 - ua - va;
    for (int s = 0; s < 16; ++s) {
      if (!(used & (1u << s))) continue;
      auto& k = g.key[f][s];
      uint16_t rows[16];
      for (int iv = 0; iv < 16; ++iv) {
        uint32_t r = 0;
        for (int iu = 0; iu < 16; ++iu) r |= uint32_t(k[iv][iu] != 0) << iu;
        rows[iv] = uint16_t(r);
      }
      for (int iv = 0; iv < 16; ++iv)
        while (rows[iv]) {
          const int iu = std::countr_zero(rows[iv]);
          const uint64_t key = k[iv][iu];
          const int cap = merged ? kGreedyMaxTiles : 1;
          const int run = std::countr_one(uint32_t(rows[iv]) >> iu);
          int w = 1;
          while (w < run && w < cap && k[iv][iu + w] == key) ++w;
          const uint32_t span = ((1u << w) - 1u) << iu;
          int h = 1;
          while (iv + h < 16 && h < cap && (rows[iv + h] & span) == span) {
            bool same = true;
            for (int t = 0; t < w && same; ++t) same = k[iv + h][iu + t] == key;
            if (!same) break;
            ++h;
          }
          for (int dv = 0; dv < h; ++dv) {
            rows[iv + dv] = uint16_t(rows[iv + dv] & ~span);
            for (int t = 0; t < w; ++t) k[iv + dv][iu + t] = 0;  // grid is all-zero after the flush
          }
          size_t slot = 0;
          while (slot < lookKeys.size() && lookKeys[slot] != (key & ~uint64_t(3))) ++slot;
          if (slot == lookKeys.size()) {
            if (slot >= 2 * canon::kMaxPalette) ok = false;  // M11: > 1024 splits the mesh in two
            lookKeys.push_back(key & ~uint64_t(3));
            cm.palette.push_back(canonPaletteOf(key, ok));
          }
          canon::DecodedQuad q;
          int o[3]; o[na] = (f & 1) ? s : s + 1; o[ua] = iu; o[va] = iv;
          q.ox = uint32_t(o[0]); q.oy = uint32_t(o[1]); q.oz = uint32_t(o[2]);
          q.w = uint32_t(w); q.h = uint32_t(h); q.face = uint32_t(f);
          q.paletteIdx = uint32_t(slot & 0x3FFu);
          q.cornerAo = uint32_t(key & 3u) * 0x55u;  // parked faces carry uniform AO
          q.aoDiagFlip = canon::aoDiagFlipFor(q.cornerAo);
          q.skyLight = 63;  // lighting is not split yet (step 1): full sky, no block light
          const uint64_t raw = canon::packQuad(q);
          cm.quads.push_back(raw);
          t_qSlot.push_back(uint32_t(slot));
          // Decode back to the legacy stream from the canonical bytes.
          const canon::DecodedQuad d = canon::unpackQuad(raw);
          const uint64_t dk = ok ? canonKeyOf(d, cm.palette[slot]) : key;
          int c[3] = {int(d.ox), int(d.oy), int(d.oz)};
          if (!(d.face & 1u)) c[na] -= 1;
          emitGreedyQuad(out, int(d.face), c[na], c[ua], c[va], int(d.w), int(d.h), dk);
        }
    }
    cm.lods[0].face[size_t(f)] = BucketRange{start, uint32_t(cm.quads.size()) - start};
  }
  cm.lods[0].skirt.start = cm.lods[0].blend.start = uint32_t(cm.quads.size());
  if (!ok) cm = CanonicalMesh{}, cm.magic = 0;  // magic 0 = not publishable
}
namespace {

// Render pipeline M7 (Archie step 2/3 completion: fully canonical meshes). Every legacy
// vertex the greedy/canonical flush did not produce (rotated, strength-slot, trace-tint,
// cutout, alpha-blend, slabs, box models, crosses, custom shapes: the per-cell stream
// written before the flush) is stored as a Packed12 fallback vertex, 4 per quad, in the
// same CanonicalMesh. Encoding (lossless for every field a shader reads):
//   fx/fy/fz = legacy 1/16-block sub-chunk units (0..4095; > 4095 = not publishable)
//   u/v      = legacy 8-bit UV << 4
//   normalId = face (bits 0..2) | faceAo bit 7 << 3; ao = faceAo bits 3..4;
//   wind     = faceAo bits 5..6 (texture rotation)
//   palette  = layerId + kLegacyLookMarker + (mode << 8 | value) in rgb565Tint
//   light    = full sky, no block light (no light engine in the mesh path yet)
// The 24-bit strength-buffer slot is not carried (no shader reads it; see the Step 1
// note in the pass). decodeFallbackVertex is the exact inverse with slot = 0.
}  // namespace (refactor step 4: cMesherKernels member, see MesherKernels.h)
bool cMesherKernels::packFallback(const std::vector<PackedVertex>& v, size_t n, CanonicalMesh& cm) {
  std::vector<uint32_t>& t_fSlot = cWorkerScratch::forThisThread().fSlot;  // M11 (refactor step 2: scratch-owned)
  cm.fallback.clear();
  cm.fallback.reserve(n);
  t_fSlot.clear();
  uint64_t lastPal = ~uint64_t(0);
  uint32_t lastIdx = 0;
  for (size_t i = 0; i < n; ++i) {
    const PackedVertex& p = v[i];
    if (p.x > 4095 || p.y > 4095 || p.z > 4095) return false;
    canon::PaletteFields pf;
    pf.layerId = uint16_t(p.layerIndex());
    pf.pbrMatId = canon::kLegacyLookMarker;
    pf.rgb565Tint = uint16_t((uint32_t(p.mode) << 8) | p.value);
    const uint64_t pal = canon::packPalette(pf);
    if (pal != lastPal) {
      size_t j = 0;
      while (j < cm.palette.size() && cm.palette[j] != pal) ++j;
      if (j == cm.palette.size()) {
        if (j >= 2 * canon::kMaxPalette) return false;  // M11: split handles 1024..2047
        cm.palette.push_back(pal);
      }
      lastPal = pal; lastIdx = uint32_t(j);
    }
    canon::Packed12Fields f;
    f.fx = p.x; f.fy = p.y; f.fz = p.z;
    f.u = uint32_t(p.u) << 4; f.v = uint32_t(p.v) << 4;
    f.normalId = (p.faceAo & 7u) | (uint32_t(p.faceAo >> 7) << 3);
    f.ao = (p.faceAo >> 3) & 3u; f.wind = (p.faceAo >> 5) & 3u;
    f.paletteIdx = lastIdx & 0x3FFu; f.blockLight = 0; f.skyLight = 63;
    cm.fallback.push_back(canon::packPacked12(f));
    t_fSlot.push_back(lastIdx);
  }
  return true;
}
namespace {
}  // namespace
// M11 (Archie ruling 1): split in stream order (fallback, then L0 quads). The first half
// takes items while its palette has room; the first item that does not fit (a fallback
// quad = 4 vertices moves as a unit) and everything after it go to the second half.
bool splitCanonicalTwo(CanonicalMesh& cm, const std::vector<uint32_t>& qSlot, const std::vector<uint32_t>& fSlot) {
  if (qSlot.size() != cm.quads.size() || fSlot.size() != cm.fallback.size()) return false;
  const std::vector<uint64_t> wide = std::move(cm.palette);
  CanonicalMesh part[2];
  std::vector<int32_t> remap[2] = {std::vector<int32_t>(wide.size(), -1), std::vector<int32_t>(wide.size(), -1)};
  int cur = 0;
  auto need = [&](int p, const uint32_t* s, size_t n) {
    size_t add = 0;
    for (size_t i = 0; i < n; ++i) {
      if (s[i] >= wide.size()) return size_t(1) << 20;
      bool dup = remap[p][s[i]] >= 0;
      for (size_t j = 0; j < i && !dup; ++j) dup = s[j] == s[i];
      add += dup ? 0 : 1;
    }
    return add;
  };
  auto place = [&](const uint32_t* s, size_t n) -> int {
    if (cur == 0 && part[0].palette.size() + need(0, s, n) > canon::kMaxPalette) cur = 1;
    if (part[cur].palette.size() + need(cur, s, n) > canon::kMaxPalette) return -1;
    for (size_t i = 0; i < n; ++i)
      if (remap[cur][s[i]] < 0) { remap[cur][s[i]] = int32_t(part[cur].palette.size()); part[cur].palette.push_back(wide[s[i]]); }
    return cur;
  };
  for (size_t i = 0; i < cm.fallback.size(); i += 4) {
    const size_t n = std::min<size_t>(4, cm.fallback.size() - i);
    const int p = place(&fSlot[i], n);
    if (p < 0) return false;
    for (size_t k = i; k < i + n; ++k) {
      canon::Packed12Fields f = canon::unpackPacked12(cm.fallback[k]);
      f.paletteIdx = uint32_t(remap[p][fSlot[k]]);
      part[p].fallback.push_back(canon::packPacked12(f));
    }
  }
  for (size_t i = 0; i < cm.quads.size(); ++i) {
    const int p = place(&qSlot[i], 1);
    if (p < 0) return false;
    canon::DecodedQuad d = canon::unpackQuad(cm.quads[i]);
    d.paletteIdx = uint32_t(remap[p][qSlot[i]]);
    part[p].quads.push_back(canon::packQuad(d));
  }
  for (CanonicalMesh& m : part) {  // L0 face buckets (quads stay face-ordered)
    uint32_t at = 0;
    for (int f = 0; f < 6; ++f) {
      uint32_t c = 0;
      while (at + c < m.quads.size() && canon::unpackQuad(m.quads[at + c]).face == uint32_t(f)) ++c;
      m.lods[0].face[size_t(f)] = BucketRange{at, c};
      at += c;
    }
    m.lods[0].skirt.start = m.lods[0].blend.start = uint32_t(m.quads.size());
  }
  cm = std::move(part[0]);
  if (!part[1].quads.empty() || !part[1].fallback.empty()) cm.next = std::make_shared<CanonicalMesh>(std::move(part[1]));
  return true;
}
namespace {
PackedVertex decodeFallbackVertex(const canon::Packed12& raw, const CanonicalMesh& cm) {
  const canon::Packed12Fields f = canon::unpackPacked12(raw);
  const canon::PaletteFields pf = canon::unpackPalette(f.paletteIdx < cm.palette.size() ? cm.palette[f.paletteIdx] : 0);
  PackedVertex p{};
  p.x = uint16_t(f.fx); p.y = uint16_t(f.fy); p.z = uint16_t(f.fz);
  p.u = uint8_t(f.u >> 4); p.v = uint8_t(f.v >> 4);
  p.faceAo = uint8_t((f.normalId & 7u) | (f.ao << 3) | (f.wind << 5) | ((f.normalId >> 3) << 7));
  p.setLayer(pf.layerId);
  p.mode = uint8_t(pf.rgb565Tint >> 8); p.value = uint8_t(pf.rgb565Tint & 0xFFu);
  p.slot = 0;
  return p;
}
// (refactor step 4: counters now cUnifiedMesherCounters, UnifiedMesherCounters.h)

// Render pipeline M6 (step 3 on the game path: registry feed into UnifiedMesher). L1/L2
// come from UnifiedMesher over a per-job material table built from the registry: one
// local material per (block, active, facing), per-face looks from BlockMaterials (the
// same FaceSlot mapping as the L0 pass). Plain full cubes only; custom / cross / slab /
// attachable / shaped cells read as air at L1/L2 (sub-block detail, Q26). Light: no
// light engine feeds the mesh path yet (lightApron 0), so L1/L2 carry full sky like L0.
// The UnifiedMesher palette is merged into the L0 palette (dedup by packed entry) and
// its quads re-indexed; on palette overflow the mesh keeps L0 only (counted).
}  // namespace (refactor step 4: cMesherKernels member, see MesherKernels.h)
bool cMesherKernels::mergeCanonLods(const CanonicalMesh& src, CanonicalMesh& dst) {
  const size_t palBefore = dst.palette.size();
  std::vector<uint32_t> remap(src.palette.size());
  for (size_t i = 0; i < src.palette.size(); ++i) {
    size_t j = 0;
    while (j < dst.palette.size() && dst.palette[j] != src.palette[i]) ++j;
    if (j == dst.palette.size()) {
      if (j >= canon::kMaxPalette) { dst.palette.resize(palBefore); return false; }
      dst.palette.push_back(src.palette[i]);
    }
    remap[i] = uint32_t(j);
  }
  const uint32_t base = uint32_t(dst.quads.size());
  for (int lod = 1; lod <= 2; ++lod) {
    CanonicalLod l = src.lods[size_t(lod)];
    for (auto& r : l.face) r.start += base;
    l.skirt.start += base; l.blend.start += base;
    dst.lods[size_t(lod)] = l;
  }
  dst.quads.reserve(dst.quads.size() + src.quads.size());
  for (uint64_t q : src.quads) {
    canon::DecodedQuad d = canon::unpackQuad(q);
    d.paletteIdx = remap[d.paletteIdx];
    dst.quads.push_back(canon::packQuad(d));
  }
  return true;
}
namespace {
// (refactor step 4: counters now cUnifiedMesherCounters, UnifiedMesherCounters.h)

// -1: no override; 0/1: LL_MESH_GREEDY forces greedy off/on (A/B and captures).
int greedyEnvOverride() {
  static const int v = cMesherSelector::envGreedyOverride();
  return v;
}

const IBlockAppearance kPlainAppearance{};  // defaults only: no custom shapes, plain faces

}  // namespace

void MeshWriter::face(const CustomBlock& cell, int f, const int lo[3], const int hi[3], const FaceMaterial& material,
                      FaceLook look, int rot) {
  static const int full[4] = {3, 3, 3, 3};
  const int blk[3] = {cell.x * 16, cell.y * 16, cell.z * 16};
  const int mn[3] = {blk[0] + lo[0], blk[1] + lo[1], blk[2] + lo[2]}, mx[3] = {blk[0] + hi[0], blk[1] + hi[1], blk[2] + hi[2]};
  emitFace(out_, f, mn, mx, blk, full, lookOf(material, look.mode, look.value, uint8_t(rot)));
}

// Cortana 2026-10-16 [ENGINE]: free-corner quad for sloped custom shapes (ramps). Never merged by
// the greedy pass; renderers bucket it as a non-axis quad (software: never face-culled).
void MeshWriter::quad(const CustomBlock& cell, const int p[4][3], const int uv[4][2], int lightFace,
                      const FaceMaterial& material, FaceLook look) {
  const int blk[3] = {cell.x * 16, cell.y * 16, cell.z * 16};
  const Look lk = lookOf(material, look.mode, look.value, 0);
  for (int c = 0; c < 4; ++c) {
    PackedVertex v{uint16_t(blk[0] + p[c][0]), uint16_t(blk[1] + p[c][1]), uint16_t(blk[2] + p[c][2]),
                   inRect(uv[c][0], lk.uv.u0, lk.uv.u1), inRect(uv[c][1], lk.uv.v0, lk.uv.v1),
                   uint8_t((lightFace & 7) | (3 << 3)), uint8_t(lk.layer), lk.value, lk.mode, lk.slot};
    v.layerHi = uint8_t(lk.layer >> 8);
    out_.push_back(v);
  }
}

namespace {

// One meshing pass over the snapshot. meshSubChunk (below) wraps this with
// the definitions-epoch guard.
ChunkMesh meshSubChunkPass(const MeshInput& in, const MeshContext& ctx, CanonicalMesh* canonOut = nullptr) {
  ChunkMesh out; out.cx = in.cx; out.sy = in.sy; out.cz = in.cz; out.epoch = in.epoch; out.seq = in.seq; out.batch = in.batch; out.batchSize = in.batchSize;
  const cBlockReg& reg = ctx.blocks;
  const BlockMaterials& mats = ctx.materials;
  const IBlockAppearance& look = ctx.appearance ? *ctx.appearance : kPlainAppearance;
  MeshWriter writer(out.verts);
  // RND-H1 (2026-10-15): per-job definition table. reg.has()/reg.def()
  // copy a BlockDef by value under the registry's shared_mutex; doing that
  // per voxel (5,832 pad cells + every solid cell) made meshing slower the
  // more threads ran. Each distinct id is now fetched ONCE per job (a
  // consistent snapshot for the whole pass; the epoch guard still re-meshes
  // after a hot-swap) and voxels read a flat flags table.
  cWorkerScratch& ws = cWorkerScratch::forThisThread();  // refactor step 2: was function-static thread_locals
  std::vector<uint32_t>& jobGen = ws.jobGen;
  std::vector<uint16_t>& jobSlot = ws.jobSlot;
  std::vector<BlockDef>& jobDefs = ws.jobDefs;
  uint32_t& gen = ws.gen;
  if (++gen == 0) { std::fill(jobGen.begin(), jobGen.end(), 0u); gen = 1; }
  jobDefs.clear();
  // Cortana 2026-10-15 1410 (Elfie §2.1): jobDef() returns a reference into
  // jobDefs; reserving the worst case (every padded cell a distinct id) up
  // front means push_back can never reallocate during a job, so a reference
  // can never dangle. One-time per thread (the vector is thread_local).
  if (jobDefs.capacity() < size_t(kMeshPadVolume)) jobDefs.reserve(size_t(kMeshPadVolume));
  const auto jobDef = [&](BlockId id) -> const BlockDef& {
    if (jobGen[id] != gen) {
      jobGen[id] = gen;
      jobSlot[id] = uint16_t(jobDefs.size());
      jobDefs.push_back(reg.def(id));
    }
    return jobDefs[jobSlot[id]];
  };
  uint8_t (&opaque)[kMeshPadVolume] = ws.opaque;
  const bool fancyLeaves = leavesFancy();  // 2026-10-16 (JD leaf pass)
  // 2026-10-18 (Crew Canopy, F-B): sparse-leaf thinning (render.foliage_density).
  // Inert unless fancy leaves + sparse mode. The pad-wide near test skips every
  // per-cell check for sub-chunks that cannot contain a far cell: the pad spans
  // columns cx-1..cx+1 / cz-1..cz+1, so "all near" needs centre distance < ring.
  const bool sparseFoliage = fancyLeaves && foliageDensity() == 1;
  int foliageFocusCx = 0, foliageFocusCz = 0;
  bool sparseActive = false;
  if (sparseFoliage) {
    foliageFocus(foliageFocusCx, foliageFocusCz);
    sparseActive = std::max(std::abs(in.cx - foliageFocusCx), std::abs(in.cz - foliageFocusCz)) >= kFoliageNearChunks;
  }
  // (lx, ly, lz) below: sub-chunk-local cell coords, pad included (-1..16).
  // The near ring is inviolate: a cell is thinned only when it is itself FAR,
  // and a leaf-to-leaf interface is culled only when BOTH cells are far and the
  // neighbour is kept - so near cells, and every interface touching one, mesh
  // exactly as today; thinning bites strictly inside the far region.
  const auto leafFar = [&](int lx, int lz) -> bool {
    return std::max(std::abs(((in.cx * 16 + lx) >> 4) - foliageFocusCx),
                    std::abs(((in.cz * 16 + lz) >> 4) - foliageFocusCz)) > kFoliageNearChunks;
  };
  const auto leafHashKept = [&](int lx, int ly, int lz) -> bool {
    return foliageHash(in.cx * 16 + lx, in.sy * 16 + ly, in.cz * 16 + lz) % kFoliageKeepDen < kFoliageKeepNum;
  };
  const bool solidShell = shellSolid();  // 2026-10-16 1551 (JD): Trace Bulb shell Ghostly / Solid
  const uint32_t opaqueMask = (fancyLeaves ? uint32_t(kOpaque) : uint32_t(kOpaque | kLeaves)) | (solidShell ? uint32_t(kShell) : 0u);
  for (int i = 0; i < kMeshPadVolume; ++i) {
    const BlockDef& od = jobDef(in.id[i]);
    opaque[i] = uint8_t((od.flags & opaqueMask) != 0 && !look.shapedRender(od));  // 2026-10-24: slab components
  }
  static const int full[4] = {3, 3, 3, 3};

  out.verts.reserve(4096);
  FallbackReasonStats::Local fbr;  // fix (a): job-local, flushed once below
  // Cortana 2026-10-15 1705: greedy meshing (see GreedyGrid).
  const int greedyEnv = greedyEnvOverride();
  const bool greedy = greedyEnv < 0 ? ctx.greedy : greedyEnv == 1;
  const bool parkFaces = greedy || canonOut != nullptr;  // render pipeline M2: unified mode parks on every backend
  std::unique_ptr<GreedyGrid>& greedyGrid = ws.greedyGrid;
  if (parkFaces) {
    if (!greedyGrid) greedyGrid = std::make_unique<GreedyGrid>();
    else {  // a pass that threw mid-way can leave cells parked: clear in place (no 196 KB stack temporary)
      GreedyGrid& g = *greedyGrid;
      if (g.sliceUsed[0] | g.sliceUsed[1] | g.sliceUsed[2] | g.sliceUsed[3] | g.sliceUsed[4] | g.sliceUsed[5]) {
        std::memset(g.key, 0, sizeof(g.key));
        std::memset(g.sliceUsed, 0, sizeof(g.sliceUsed));
      }
    }
  }
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const int ci = pidx(x + 1, y + 1, z + 1);
        const BlockId b = in.id[ci];
        if (b == kAir) continue;
        const CellState st = in.st[ci];
        const size_t cellV0 = out.verts.size();  // fix (a): attributes non-face-loop fallback quads
        const int mn[3] = {x * 16, y * 16, z * 16};
        const BlockDef& def = jobDef(b);  // jobDefs only grows via jobDef(), called once per cell: ref stays valid for this cell
        // 2026-10-24 (JD: LEDs drawn as the X sprite): a legacy attachable with no mount face recorded
        // (scene-placed, older saves) draws the floor-mounted box model, matching the sim's floor default.
        if (def.flags & kAttachable) {
          if (const BoxModel* bm = look.boxModel(def, st)) {  // attached: the oriented box model
            const CustomBlock cell{in, reg, mats, b, st, x, y, z};
            const bool on = look.active(def, st);
            // 2026-10-16 (JD): a sprite-shaped attachable (Red/Blue LED) on a wall leans out like a
            // Minecraft wall torch: modelled upright, raised 3/16, its base against the wall, then
            // the top sheared 4/16 away from the wall. Render only (state and sim untouched).
            const int af = stAttach(st) ? stAttach(st) : 1;
            const bool tilt = def.shape == Shape::Cross && af >= 3 && af <= 6;
            int wdx = 0, wdy = 0, wdz = 0;
            attachOffset(af, wdx, wdy, wdz);
            const size_t tiltFrom = out.verts.size();
            const int turns = (tilt || af == 1 || af == 2) ? look.boxModelTurns(def, st) : 0;  // s2-PlayD: build 9 (5c22f7b) box-model turn gate restored  // PlayN (JD): the game decides per mount (wall Switches turn too)  // 2026-10-24: floor / ceiling facing
            for (BoxModel::Box box : bm->orientedBoxes(tilt ? 1 : af, turns)) {
              const FaceMaterial& fm = mats.face(b, bm->materials[size_t(box.material)].slot, on);
              FaceLook fl = look.faceLookAt(def, st, FaceSlot::Top, on, in.cx * 16 + x, in.sy * 16 + y,
                                             in.cz * 16 + z);
              // 2026-10-24 (JD 6:12 PM, LED see-through ribbon): box models draw solid (no sprite
              // alpha-test holes), and a face flush with the cell wall it mounts on is skipped (it
              // z-fought the support's face: the slivers beside the block below a floor LED).
              for (int f = 0; f < 6; ++f) {
                const int* n = kFaces[f].n;
                bool flush = false;
                for (int a = 0; a < 3; ++a)
                  if ((n[a] < 0 && box.min[a] <= 0) || (n[a] > 0 && box.max[a] >= 16)) flush = true;
                if (flush) continue;
                writer.face(cell, f, box.min, box.max, fm, fl);
              }
            }
            if (false) {  // 2026-10-24 LED v2: lean is baked into the wall model; was 7:00 PM (JD): lean 45 degrees out from the wall (solid model, no cutout)  // 2026-10-24 (JD 6:12 PM): the shear twisted the LED into a ribbon; upright model, raised against the wall
              for (size_t i = tiltFrom; i < out.verts.size(); ++i) {
                PackedVertex& v = out.verts[i];
                const int ly = int(v.y) - y * 16;  // 3 (base) .. 13 (top)
                const int off = std::clamp(ly - 3, 0, 10);  // 45 degrees: one out per one up
                v.x = uint16_t(int(v.x) - off * wdx);
                v.z = uint16_t(int(v.z) - off * wdz);
              }
            }
            fbr.addShape(FallbackShape::BoxModel, (out.verts.size() - cellV0) / 4);
            continue;
          }
        }
        if (def.shape == Shape::Cube && look.shapedRender(def) &&
            look.meshShaped(CustomBlock{in, reg, mats, b, st, x, y, z}, writer)) {
          fbr.addShape(FallbackShape::Shaped, (out.verts.size() - cellV0) / 4);
          continue;  // 2026-10-24: render-only slab model (collision stays a full cell)
        }
        if (def.shape == Shape::Custom) {  // the game builds it (e.g. connected strips)
          look.meshCustom(CustomBlock{in, reg, mats, b, st, x, y, z}, writer);
          fbr.addCustom(b, (out.verts.size() - cellV0) / 4);
          continue;
        }
        if (def.shape == Shape::Cross) {  // sprite: the active-state top face, alpha-tested
          emitCross(out.verts, mn, lookOf(mats.face(b, FaceSlot::Top, look.active(def, st)), kShadeCutout));
          fbr.addShape(FallbackShape::Cross, (out.verts.size() - cellV0) / 4);
          continue;
        }
        if (def.shape != Shape::Cube && def.shape != Shape::Slab) continue;
        // 2026-10-18 (Crew Canopy, F-B): an unkept far leaf voxel emits nothing
        // (appearance extras are no-ops for leaf blocks: Redirector/Catchor only).
        if (sparseActive && (def.flags & kLeaves) && leafFar(x, z) && !leafHashKept(x, y, z)) continue;
        const bool slab = def.shape == Shape::Slab;
        const int mx[3] = {mn[0] + 16, mn[1] + (slab ? 8 : 16), mn[2] + 16};
        const bool dir = def.flags & kDirectional;
        const int facing = dir ? stFacing(st) : 0;
        const bool active = look.active(def, st);
        const CustomBlock cellC{in, reg, mats, b, st, x, y, z};
        for (int f = 0; f < 6; ++f) {
          const Face& F = kFaces[f];
          const int fx = x + 1 + F.n[0], fy = y + 1 + F.n[1], fz = z + 1 + F.n[2];
          if (opaque[pidx(fx, fy, fz)] && !(slab && f == 2)) continue;  // hidden by neighbour
          const bool leaf = (def.flags & kLeaves) != 0;  // 2026-10-16 (JD leaf pass)
          if ((def.flags & kTransparent) && !leaf && in.id[pidx(fx, fy, fz)] == b) continue;  // glass against glass
          if (!(def.flags & (kOpaque | kTransparent)) && !(solidShell && (def.flags & kShell)) && !leaf && !slab && in.id[pidx(fx, fy, fz)] == b && !(llP2PerfOff() & 16)) continue;  // 1520 (Cortana build check): see-through cube (Trace Bulb shell) against itself: no inner faces
          // 2026-10-18 (Crew Canopy, F-B): cull interior interfaces between two
          // far leaves when the neighbour is kept (in fancy mode both blocks
          // mesh them today, two-sided each). Interfaces touching a near cell
          // are never culled - the near ring meshes full.
          if (sparseActive && leaf && leafFar(x, z)) {
            const BlockId nb = in.id[pidx(fx, fy, fz)];
            if (nb != kAir && (jobDef(nb).flags & kLeaves) && leafFar(fx - 1, fz - 1) &&
                leafHashKept(fx - 1, fy - 1, fz - 1))
              continue;
          }
          FaceSlot slot;
          if (f == 2) slot = FaceSlot::Top;
          else if (f == 3) slot = FaceSlot::Bottom;
          else {
            const int d = cMesherKernels::kFaceDir[f];
            slot = d == facing ? FaceSlot::Front : d == dirBack(facing) ? FaceSlot::Back : d == dirRight(facing) ? FaceSlot::Right : FaceSlot::Left;
          }
          const std::optional<FaceMaterial> over = look.faceMaterial(cellC, slot, active);
          const FaceMaterial fm = over ? *over : mats.face(b, slot, leaf ? !fancyLeaves : active);
          Look lk = lookOf(fm, kShadeTex, 0, uint8_t(f >= 2 && f <= 3 ? facing : 0));
          if (fm.animated()) { lk.mode = kShadeAnimated; lk.value = fm.animation; }
          else {
            // Cortana 2026-10-15 1755 [ENGINE]: neighbourhood-aware look first (Emitor beam).
            const std::optional<FaceLook> cellLook = look.faceLookCell(cellC, slot, active);
            const FaceLook fl = cellLook ? *cellLook
                                         : look.faceLookAt(def, st, slot, active, in.cx * 16 + x, in.sy * 16 + y,
                                                           in.cz * 16 + z);
            lk.mode = fl.mode;
            if (solidShell && (def.flags & kShell) && (lk.mode == kShadeCutout || lk.mode == kShadeAlphaBlend)) lk.mode = kShadeEmissive;  // JD Vulkan push: + blended shell  // 1551: Solid = old opaque glowing bulb
            // Step 1 (partial): trace faces write the strength-buffer slot for future
            // GPU use, but keep the baked strength in value until the shader
            // actually reads the buffer. Setting value=0 here renders black.
            if (ctx.strengths && (fl.mode == kShadeStripTint0 || fl.mode == kShadeStripTint1)) {
              // B-M4 (P3-2): the lookup key is the WORLD packPos key — the
              // same key the signal sim wrote the slot with. The old key
              // packed sub-chunk-LOCAL (x, y, z), so it could only miss or
              // collide with an unrelated cell. slotOf returns kNoSlot
              // (not 0) when the cell has no slot yet; the future shader
              // treats kNoSlot as "use the baked value".
              const uint64_t key = uint64_t(packPos(in.cx * 16 + x, in.sy * 16 + y, in.cz * 16 + z));
              lk.value = fl.value;  // baked strength (visible now)
              lk.slot = ctx.strengths->slotOf(key);  // buffer slot (for future shader)
            } else {
              lk.value = fl.value;
            }
          }
          if (leaf) { lk.mode = fancyLeaves ? kShadeLeaf : kShadeTex; lk.value = fancyLeaves ? mats.leafGlowSlot(b) : 0; }
          // 2026-10-16 (Wafer beta art): a face texture with a "tint:" mask takes the feeding Trace's
          // colour - the Trace behind the back (input) port: blue Trace (channel 2) = blue, else red.
          if (const uint8_t ts = (lk.mode == kShadeTex && !((llP2PerfOff() & 2) && def.channel)) ? mats.tintSlot(fm.layer) : 0) {
            const int bd = dirBack(facing);
            const BlockDef& nb = jobDef(in.id[pidx(x + 1 + kDirX[bd], y + 1, z + 1 + kDirZ[bd])]);
            // 2026-10-16 (CloudAtlas power-state art): a block with its OWN channel (Red / Blue Via)
            // tints with that channel and lights while it carries signal (value > 0); others keep the
            // feeding-Trace rule. Render-only.
            int ch = def.channel ? def.channel : nb.channel;
            if (ch == 0) {  // 2026-10-24 (JD 6:13 PM red flash): back source gone / colourless -> colour of the Trace it drives
              const BlockDef& fn = jobDef(in.id[pidx(x + 1 + kDirX[facing], y + 1, z + 1 + kDirZ[facing])]);
              ch = fn.channel;
            }
            const bool lit = active || (def.channel != 0 && (st & 0x7f) != 0);
            lk.mode = kShadeTraceTint;
            lk.value = uint8_t((ts - 1) | (ch == 2 ? 0x40 : 0) | (lit ? 0x80 : 0));
          }
          int ao[4];
          for (int c = 0; c < 4; ++c) {
            const int su = kCorner[c][0] ? 1 : -1, sv = kCorner[c][1] ? 1 : -1;
            auto at = [&](int a, int bb) {
              return opaque[pidx(fx + a * F.u[0] + bb * F.v[0], fy + a * F.u[1] + bb * F.v[1],
                                 fz + a * F.u[2] + bb * F.v[2])];
            };
            const int s1 = at(su, 0), s2 = at(0, sv), cr = at(su, sv);
            ao[c] = (s1 && s2) ? 0 : 3 - (s1 + s2 + cr);
          }
          // fix (a): same gate as !slab && greedyEligible(lk, ao), split so the first failing condition is counted
          const FallbackReason fbReason = FallbackReasonStats::checkCanonicalEligibility(slab, lk.mode, lk.rot, lk.slot, lk.uv == UvRect{}, ao);
          if (parkFaces && fbReason == FallbackReason::Count) {  // Cortana 2026-10-15 1705: park for merging
            const int blk[3] = {x, y, z};
            park(*greedyGrid, f, blk, lk, ao[0]);
            ++fbr.canonFaces;
            continue;
          }
          if (canonOut) fbr.add(fbReason, leaf && fancyLeaves ? 2u : 1u, lk.mode, ao);
          emitFace(out.verts, f, mn, mx, mn, slab && f == 2 ? full : ao, lk);
          if (leaf && fancyLeaves) {  // two-sided: the same quad again, reversed winding (no cull-state change)
            emitFace(out.verts, f, mn, mx, mn, slab && f == 2 ? full : ao, lk);
            std::swap(out.verts[out.verts.size() - 3], out.verts[out.verts.size() - 1]);
          }
        }
        {
          const size_t extraV0 = out.verts.size();
          look.meshExtra(cellC, writer);
          fbr.addShape(FallbackShape::Extra, (out.verts.size() - extraV0) / 4);
        }
      }
  cMeshPipeline pipe(in, ctx, out, canonOut);  // refactor step 4: the stages after the cell pass, same order
  const size_t legacyVerts = out.verts.size();  // render pipeline M7: per-cell stream before the canonical flush
  pipe.flush(greedyGrid, greedy);
  pipe.packedFallback(legacyVerts, fbr, ws);
  pipe.canonLods(ws, look, fancyLeaves);  // render pipeline M6 (refactor step 4c)
  pipe.capGuard();
  pipe.perMeshParams();
  return out;
}

}  // namespace

// 2026-10-16 (JD leaf pass): render.leaves (see mesher.h).
static std::atomic<bool> g_shellSolid{false};  // 2026-10-16 1551 (JD): render.bulb_shell
void setShellSolid(bool solid) { g_shellSolid.store(solid, std::memory_order_relaxed); }
bool shellSolid() { return g_shellSolid.load(std::memory_order_relaxed); }
static std::atomic<int> g_leavesFancy{-1};
void setLeavesFancy(bool fancy) { g_leavesFancy.store(fancy ? 1 : 0, std::memory_order_relaxed); }
bool leavesFancy() {
  int v = g_leavesFancy.load(std::memory_order_relaxed);
  if (v < 0) {
    v = cMesherSelector::envLeavesFancy();
    int expect = -1;
    g_leavesFancy.compare_exchange_strong(expect, v);
    v = g_leavesFancy.load(std::memory_order_relaxed);
  }
  return v == 1;
}

// 2026-10-18 (Crew Canopy, F-B): render.foliage_density + focus (see mesher.h).
static std::atomic<int> g_foliageDensity{-1};
void setFoliageDensity(int mode) { g_foliageDensity.store(mode ? 1 : 0, std::memory_order_relaxed); }
int foliageDensity() {
  int v = g_foliageDensity.load(std::memory_order_relaxed);
  if (v < 0) {
    v = cMesherSelector::envFoliageDensity();
    int expect = -1;
    g_foliageDensity.compare_exchange_strong(expect, v);
    v = g_foliageDensity.load(std::memory_order_relaxed);
  }
  return v;
}
static std::atomic<int> g_foliageFocusCx{0}, g_foliageFocusCz{0};
void setFoliageFocus(int cx, int cz) {
  g_foliageFocusCx.store(cx, std::memory_order_relaxed);
  g_foliageFocusCz.store(cz, std::memory_order_relaxed);
}
void foliageFocus(int& cx, int& cz) {
  cx = g_foliageFocusCx.load(std::memory_order_relaxed);
  cz = g_foliageFocusCz.load(std::memory_order_relaxed);
}

// Cortana 2026-10-15 1340: verify-mode state (see mesher.h).
static bool verifyForcedOff() { return cMesherSelector::envVerifyOff(); }  // not cached: g_meshVerify initialises before main, --no-verify sets the env later  // verify-off switch: LL_VERIFY_OFF=1 / --no-verify disables DebugOpt always-on verifies (benchmarks)
#ifdef NDEBUG
static std::atomic<bool> g_meshVerify{false};
#else
static std::atomic<bool> g_meshVerify{!verifyForcedOff()};
#endif
static std::atomic<uint64_t> g_mvChecked{0}, g_mvMismatch{0}, g_mvSkipped{0};
void setMeshVerify(bool on) { g_meshVerify.store(on, std::memory_order_relaxed); }
bool meshVerifyEnabled() { return g_meshVerify.load(std::memory_order_relaxed); }
MeshVerifyStats meshVerifyStats() { return {g_mvChecked.load(), g_mvMismatch.load(), g_mvSkipped.load()}; }
uint64_t meshChecksum(const ChunkMesh& m) {
  uint64_t h = 1469598103934665603ull;
  const auto* p = reinterpret_cast<const unsigned char*>(m.verts.data());
  const size_t n = m.verts.size() * sizeof(PackedVertex);
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

// Cortana 2026-10-15 1340: the shipped pass reads the stable def copy
// (cContentReg::StableDefScope) - no registry lock per voxel.
static ChunkMesh meshSubChunkStable(const MeshInput& in, const MeshContext& ctx, CanonicalMesh* canon = nullptr) {
  cContentReg::StableDefScope stable(ctx.blocks.content());
  return meshSubChunkPass(in, ctx, canon);
}

// Render pipeline M2: unified-mesher switch + decode-equivalence counters (see mesher.h).
static std::atomic<int> g_unifiedMesher{-1};
void setUnifiedMesher(bool on) { g_unifiedMesher.store(on ? 1 : 0, std::memory_order_relaxed); }
bool unifiedMesherEnabled() {
  int v = g_unifiedMesher.load(std::memory_order_relaxed);
  if (v < 0) {
    int expect = -1;
    g_unifiedMesher.compare_exchange_strong(expect, cMesherSelector::envUnifiedMesher());
    v = g_unifiedMesher.load(std::memory_order_relaxed);
  }
  return v == 1;
}
// (refactor step 4: meshes/checked/mismatch/unpublished/quads/bytes are cUnifiedMesherCounters members)
UnifiedMesherStats unifiedMesherStats() {
  return {cUnifiedMesherCounters::meshes.load(), cUnifiedMesherCounters::checked.load(), cUnifiedMesherCounters::mismatch.load(), cUnifiedMesherCounters::unpublished.load(), cUnifiedMesherCounters::quads.load(), cUnifiedMesherCounters::bytes.load(),
          cUnifiedMesherCounters::lodQuads.load(), cUnifiedMesherCounters::lodFallback.load(), cUnifiedMesherCounters::fbVerts.load(), cUnifiedMesherCounters::fbChecked.load(), cUnifiedMesherCounters::fbMismatch.load(), cUnifiedMesherCounters::skirtQuads.load(), cUnifiedMesherCounters::splits.load()};
}
bool decodeCanonicalMesh(const CanonicalMesh& cm, int lod, std::vector<PackedVertex>& out, uint8_t skirtMask) {
  out.clear();
  if (cm.magic != canon::kMeshMagic || lod < 0 || lod > 2) return false;
  if (lod == 0) {
    out.reserve(cm.fallback.size());
    for (const canon::Packed12& f : cm.fallback) out.push_back(decodeFallbackVertex(f, cm));
  }
  const CanonicalLod& L = cm.lods[size_t(lod)];
  const int s = 1 << lod;
  auto range = [&](BucketRange r, bool skirt) {
    for (uint32_t i = r.start; i < r.start + r.count && i < cm.quads.size(); ++i) {
      const canon::DecodedQuad d = canon::unpackQuad(cm.quads[i]);
      if (d.paletteIdx >= cm.palette.size() || d.face > 5 || d.w == 0 || d.h == 0) continue;
      if (skirt && !((skirtMask >> d.face) & 1u)) continue;  // M10: interior seam, no skirt
      const int f = int(d.face), ua = kTexUAxis[f], va = kTexVAxis[f], na = 3 - ua - va;
      int c[3] = {int(d.ox), int(d.oy), int(d.oz)};
      if (!(d.face & 1u)) c[na] -= 1;
      const size_t first = out.size();
      emitGreedyQuad(out, f, c[na], c[ua], c[va], int(d.w), int(d.h), canonKeyOf(d, cm.palette[d.paletteIdx]));
      if (s > 1)
        for (size_t k = first; k < out.size(); ++k) {
          out[k].x = uint16_t(out[k].x * s); out[k].y = uint16_t(out[k].y * s); out[k].z = uint16_t(out[k].z * s);
        }
    }
  };
  for (int f = 0; f < 6; ++f) range(L.face[size_t(f)], false);
  range(L.skirt, true);
  range(L.blend, false);
  if (cm.next) {  // M11: second half of a split mesh, appended in stream order
    std::vector<PackedVertex> tail;
    if (decodeCanonicalMesh(*cm.next, lod, tail, skirtMask)) out.insert(out.end(), tail.begin(), tail.end());
  }
  return true;
}
namespace {
void pullCollect(const CanonicalMesh& cm, int lod, uint8_t skirtMask, CanonPullStream& out, bool fbPass) {
  if (fbPass) {
    if (lod == 0)
      for (const canon::Packed12& f : cm.fallback) {
        const uint32_t pi = canon::unpackPacked12(f).paletteIdx;
        const canon::PaletteFields pf = canon::unpackPalette(pi < cm.palette.size() ? cm.palette[pi] : 0);
        out.texels.insert(out.texels.end(), {f.w[0], f.w[1], f.w[2], uint32_t(pf.layerId) | (uint32_t(pf.rgb565Tint) << 16)});
      }
  } else {
    const CanonicalLod& L = cm.lods[size_t(lod)];
    auto range = [&](BucketRange r, bool skirt) {
      for (uint32_t i = r.start; i < r.start + r.count && i < cm.quads.size(); ++i) {
        const canon::DecodedQuad d = canon::unpackQuad(cm.quads[i]);
        if (d.paletteIdx >= cm.palette.size() || d.face > 5 || d.w == 0 || d.h == 0) continue;
        if (skirt && !((skirtMask >> d.face) & 1u)) continue;
        const uint64_t key = canonKeyOf(d, cm.palette[d.paletteIdx]);
        const uint32_t layer = uint32_t((key >> 24) & 0xFFFFu), value = uint32_t((key >> 16) & 0xFFu), mode = uint32_t((key >> 8) & 0xFFu);
        out.texels.insert(out.texels.end(), {uint32_t(cm.quads[i]), uint32_t(cm.quads[i] >> 32), layer | (value << 16) | (mode << 24), 0u});
        ++out.quads;
      }
    };
    for (int f = 0; f < 6; ++f) range(L.face[size_t(f)], false);
    range(L.skirt, true);
    range(L.blend, false);
  }
  if (cm.next) pullCollect(*cm.next, lod, skirtMask, out, fbPass);
}
std::atomic<uint64_t> g_pullChecked{0}, g_pullMismatch{0};
}  // namespace
bool buildCanonPullStream(const CanonicalMesh& cm, int lod, uint8_t skirtMask, CanonPullStream& out) {
  out = CanonPullStream{};
  if (cm.magic != canon::kMeshMagic || lod < 0 || lod > 2) return false;
  pullCollect(cm, lod, skirtMask, out, true);
  out.fbQuads = uint32_t(out.texels.size() / 16);
  out.texels.resize(size_t(out.fbQuads) * 16);  // whole quads only (decode emits 4 per quad)
  pullCollect(cm, lod, skirtMask, out, false);
  return true;
}
PackedVertex canonPullReference(const CanonPullStream& st, uint32_t q, uint32_t corner, int lod) {
  PackedVertex o{};
  if (q < st.fbQuads) {  // canonPullFallback
    const uint32_t* t = &st.texels[(size_t(q) * 4 + corner) * 4];
    const uint32_t fx = t[0] & 0xFFFu, fy = (t[0] >> 12) & 0xFFFu, fz = ((t[0] >> 24) & 0xFFu) | ((t[1] & 0xFu) << 8);
    const uint32_t u = (t[1] >> 4) & 0xFFFu, v = (t[1] >> 16) & 0xFFFu, nid = (t[1] >> 28) & 0xFu;
    const uint32_t ao = (t[2] >> 10) & 3u, wind = (t[2] >> 12) & 0x3Fu, layer = t[3] & 0xFFFFu, tint = t[3] >> 16;
    o.x = uint16_t(fx); o.y = uint16_t(fy); o.z = uint16_t(fz);
    o.u = uint8_t((u >> 4) & 0xFFu); o.v = uint8_t((v >> 4) & 0xFFu);
    o.faceAo = uint8_t(((nid & 7u) | (ao << 3) | (wind << 5) | ((nid >> 3) << 7)) & 0xFFu);
    o.layer = uint8_t(layer & 0xFFu); o.value = uint8_t(tint & 0xFFu); o.mode = uint8_t((tint >> 8) & 0xFFu);
    o.layerHi = uint8_t((layer >> 8) & 0xFFu);
    return o;
  }
  const uint32_t* t = &st.texels[(size_t(st.fbQuads) * 4 + (q - st.fbQuads)) * 4];  // canonPullQuadLegacy
  const uint32_t lo = t[0], hi = t[1];
  int c[3] = {int(lo & 0x3Fu), int((lo >> 6) & 0x3Fu), int((lo >> 12) & 0x3Fu)};
  const int w = int((lo >> 18) & 0xFu) + 1, h = int((lo >> 22) & 0xFu) + 1;
  const uint32_t f = (lo >> 26) & 7u, ao = (hi >> 10) & 3u;
  if (f > 5) return o;
  const int ua = kTexUAxis[f], va = kTexVAxis[f], na = 3 - ua - va;
  if (!(f & 1u)) c[na] -= 1;
  int mn[3], mx[3];
  mn[na] = c[na] * 16; mx[na] = mn[na] + 16;
  mn[ua] = c[ua] * 16; mx[ua] = (c[ua] + w) * 16;
  mn[va] = c[va] * 16; mx[va] = (c[va] + h) * 16;
  static constexpr int kGeoU[6] = {1, 2, 2, 0, 0, 1};
  const int gu = kGeoU[f];
  const bool cu = corner == 1 || corner == 2, cv = corner >= 2, posN = !(f & 1u);
  int pos[3], local[3];
  for (int a = 0; a < 3; ++a) {
    if (a == na) pos[a] = posN ? mx[a] : mn[a];
    else if (a == gu) pos[a] = cu ? mx[a] : mn[a];
    else pos[a] = cv ? mx[a] : mn[a];
  }
  for (int a = 0; a < 3; ++a) local[a] = pos[a] > mn[a] ? 16 : 0;
  for (int a = 0; a < 3; ++a) pos[a] = std::max(pos[a], 0);
  int u, v;
  faceUV(int(f), local, 0, u, v);
  const uint32_t s = 1u << lod;
  o.x = uint16_t((uint32_t(pos[0]) * s) & 0xFFFFu); o.y = uint16_t((uint32_t(pos[1]) * s) & 0xFFFFu);
  o.z = uint16_t((uint32_t(pos[2]) * s) & 0xFFFFu);
  const uint32_t layer = t[2] & 0xFFFFu;
  o.u = uint8_t((uint32_t(u) * uint32_t(w)) & 0xFFu); o.v = uint8_t((uint32_t(v) * uint32_t(h)) & 0xFFu);
  o.faceAo = uint8_t((f | (ao << 3)) & 0xFFu);
  o.layer = uint8_t(layer & 0xFFu); o.value = uint8_t((t[2] >> 16) & 0xFFu); o.mode = uint8_t((t[2] >> 24) & 0xFFu);
  o.layerHi = uint8_t(layer >> 8);
  return o;
}
CanonPullVerifyStats canonPullVerifyStats() { return {g_pullChecked.load(), g_pullMismatch.load()}; }
uint8_t canonSkirtMask(int lod, const int nbrLod[6]) {
  uint8_t m = 0;
  for (int f = 0; f < 6; ++f) {
    if (nbrLod[f] == -2) continue;
    if ((nbrLod[f] < 0 ? 0 : nbrLod[f]) != lod) m = uint8_t(m | (1u << f));
  }
  return lod > 0 ? m : uint8_t(0);
}
int selectCanonLod(int d, int current, int ring1, int ring2) {
  auto base = [&](int dd) { return dd < ring1 ? 0 : dd < ring2 ? 1 : 2; };
  if (current < 0 || current > 2) return base(d);
  const int b = base(d);
  if (b > current) return std::max(current, base(d - 2));
  if (b < current) return std::min(current, base(d + 2));
  return current;
}
void canonLodRings(int& ring1, int& ring2) {
  static const std::pair<int, int> r = cMesherSelector::envCanonLodRings();
  ring1 = r.first; ring2 = r.second;
  // Archie Phase 1 (JD 2026-11-04): no canonical LOD ring lies beyond the view radius. Inside the
  // outer ring the ladder scales with the view ({12,24} at view 6 -> {3,6}); else it is clamped.
  const int view = cSwLodRings::shared().view();
  if (view > 0 && view < ring2) {
    if (view <= ring1) { ring1 = std::max(1, ring1 * view / std::max(1, ring2)); ring2 = view; }
    else ring2 = view;
  }
}
bool canonLodsEnabled() {
  static const bool v = cMesherSelector::envCanonLods();
  return v;
}
static bool canonVerifyOn() {
#if defined(LL_DEBUGOPT_BUILD) || !defined(NDEBUG)
  static const bool off = verifyForcedOff();  // first call is after argv parsing (first mesh)
  return !off;
#else
  static const bool v = cMesherSelector::envCanonVerify();
  return v || meshVerifyEnabled();
#endif
}
// Quad multiset of a vertex stream (4 x 16-byte vertices per quad), sorted: the
// non-greedy unified stream lists parked faces after the per-cell ones.
static std::vector<std::array<uint64_t, 8>> sortedQuads(const std::vector<PackedVertex>& v) {
  std::vector<std::array<uint64_t, 8>> q(v.size() / 4);
  for (size_t i = 0; i < q.size(); ++i) std::memcpy(q[i].data(), &v[i * 4], 64);
  std::sort(q.begin(), q.end());
  return q;
}

ChunkMesh meshSubChunk(const MeshInput& in, const MeshContext& ctx) {
  // Definitions-epoch guard (P3-3, Appendix D M4): the voxel data is an
  // immutable snapshot (MeshInput), but block DEFINITIONS are read live
  // from the registry on worker threads, and refreshStaged() can swap them
  // between ticks. Individual def reads are already torn-safe (the
  // registry's shared lock + by-value def()), but a refresh landing
  // MID-MESH would bake a mix of old and new defs into one mesh. Capture
  // the registry's definitionEpoch before and after the pass; on a change,
  // re-mesh ONCE against the now-stable set (refresh happens between
  // ticks, so the second pass sees one consistent generation). A second
  // change means refresh is running concurrently with meshing — outside
  // the between-ticks contract — so log loudly and keep the second pass
  // rather than loop forever.
  const uint64_t epoch0 = ctx.blocks.content().definitionEpoch();
  const bool unified = unifiedMesherEnabled();  // render pipeline M2 (--unified-mesher)
  auto canon = unified ? std::make_shared<CanonicalMesh>() : nullptr;
  ChunkMesh out = meshSubChunkStable(in, ctx, canon.get());
  const uint64_t epoch1 = ctx.blocks.content().definitionEpoch();
  if (epoch1 != epoch0) {
    out = meshSubChunkStable(in, ctx, canon.get());
    const uint64_t epoch2 = ctx.blocks.content().definitionEpoch();
    if (epoch2 != epoch1)
      logError("mesher: block definitions changed twice during one mesh of (%d,%d,%d) "
               "(epochs %llu -> %llu -> %llu); refreshStaged must run between ticks only",
               in.cx, in.sy, in.cz, (unsigned long long)epoch0, (unsigned long long)epoch1,
               (unsigned long long)epoch2);
  }
  if (unified) {
    cUnifiedMesherCounters::meshes.fetch_add(1, std::memory_order_relaxed);
    if (canonVerifyOn()) {  // decode-equivalence: canonical-decoded stream == legacy stream (as quad sets)
      const ChunkMesh legacy = meshSubChunkStable(in, ctx);
      if (ctx.blocks.content().definitionEpoch() == epoch1) {
        cUnifiedMesherCounters::checked.fetch_add(1, std::memory_order_relaxed);
        if (sortedQuads(out.verts) != sortedQuads(legacy.verts)) {
          cUnifiedMesherCounters::mismatch.fetch_add(1, std::memory_order_relaxed);
          logError("unified-mesher: decode MISMATCH sub-chunk (%d,%d,%d): canonical %zu quads vs legacy %zu quads",
                   in.cx, in.sy, in.cz, out.verts.size() / 4, legacy.verts.size() / 4);
        }
      }
    }
    if (canon->magic == canon::kMeshMagic && canonVerifyOn()) {  // M7: Packed12 fallback decodes to the legacy per-cell stream
      std::vector<std::pair<const canon::Packed12*, const CanonicalMesh*>> fbAll;  // M11: both halves
      for (const CanonicalMesh* m = canon.get(); m; m = m->next.get())
        for (const auto& f : m->fallback) fbAll.emplace_back(&f, m);
      bool same = fbAll.size() <= out.verts.size();
      for (size_t i = 0; same && i < fbAll.size(); ++i) {
        PackedVertex a = out.verts[i];
        a.slot = 0;
        const PackedVertex b = decodeFallbackVertex(*fbAll[i].first, *fbAll[i].second);
        same = std::memcmp(&a, &b, sizeof a) == 0;
      }
      cUnifiedMesherCounters::fbChecked.fetch_add(1, std::memory_order_relaxed);
      if (!same) {
        cUnifiedMesherCounters::fbMismatch.fetch_add(1, std::memory_order_relaxed);
        logError("unified-mesher: Packed12 fallback MISMATCH sub-chunk (%d,%d,%d) (%zu fallback verts)", in.cx, in.sy, in.cz,
                 canon->fallback.size());
      }
    }
    if (canon->magic == canon::kMeshMagic && canonVerifyOn()) {  // M12: GPU pull stream == CPU decode (quad sets), L0..L2
      for (int lod = 0; lod < 3; ++lod) {
        std::vector<PackedVertex> dec, pulled;
        CanonPullStream ps;
        if (!decodeCanonicalMesh(*canon, lod, dec) || !buildCanonPullStream(*canon, lod, 0x3F, ps)) continue;
        for (uint32_t q = 0; q < ps.fbQuads + ps.quads; ++q)
          for (uint32_t k = 0; k < 4; ++k) pulled.push_back(canonPullReference(ps, q, k, lod));
        g_pullChecked.fetch_add(1, std::memory_order_relaxed);
        if (sortedQuads(dec) != sortedQuads(pulled)) {
          g_pullMismatch.fetch_add(1, std::memory_order_relaxed);
          logError("unified-mesher: GPU pull MISMATCH sub-chunk (%d,%d,%d) lod %d: decode %zu verts vs pull %zu", in.cx, in.sy,
                   in.cz, lod, dec.size(), pulled.size());
        }
      }
    }
    if (canon->magic == canon::kMeshMagic) {
      cUnifiedMesherCounters::quads.fetch_add(canon->quads.size(), std::memory_order_relaxed);
      cUnifiedMesherCounters::bytes.fetch_add(canon->residentBytes(), std::memory_order_relaxed);
      out.canonical = std::move(canon);
    } else {
      cUnifiedMesherCounters::unpublished.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (meshVerifyEnabled()) {
    const uint64_t e0 = ctx.blocks.content().definitionEpoch();
    CanonicalMesh liveCanon;  // M7: same mode as the pass under test (unified orders verts differently)
    const ChunkMesh live = meshSubChunkPass(in, ctx, unified ? &liveCanon : nullptr);  // locked live registry reads
    const uint64_t e1 = ctx.blocks.content().definitionEpoch();
    if (e0 != e1 || e0 != epoch1) {
      g_mvSkipped.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_mvChecked.fetch_add(1, std::memory_order_relaxed);
      const uint64_t a = meshChecksum(out), b = meshChecksum(live);
      if (a != b || out.verts.size() != live.verts.size()) {
        g_mvMismatch.fetch_add(1, std::memory_order_relaxed);
        logError("mesh-verify: MISMATCH sub-chunk (%d,%d,%d) epoch %llu: stable %016llx (%zu verts) vs live %016llx (%zu verts)",
                 in.cx, in.sy, in.cz, (unsigned long long)e0, (unsigned long long)a, out.verts.size(),
                 (unsigned long long)b, live.verts.size());
      }
    }
  }
  return out;
}

}  // namespace litholattice
