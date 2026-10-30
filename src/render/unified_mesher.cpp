// UnifiedMesher implementation. See unified_mesher.h for the design and the four
// JD-approved fixes over Archie's sketch.
#include "litholattice/render/unified_mesher.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace litholattice {

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL, kFnvPrime = 0x100000001b3ULL;
inline void fnvU32(uint64_t& h, uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xFFu; h *= kFnvPrime; }
}
inline void fnvU64(uint64_t& h, uint64_t v) noexcept { fnvU32(h, uint32_t(v)); fnvU32(h, uint32_t(v >> 32)); }

// Corner AO LUT (fix 1). Neighbour-solid mask bits, in the face's (u,v) plane around
// the air cell in front of the face: b0(-u,-v) b1(0,-v) b2(+u,-v) b3(-u,0) b4(+u,0)
// b5(-u,+v) b6(0,+v) b7(+u,+v). Output: 2 bits per geometric corner g0(0,0) g1(1,0)
// g2(1,1) g3(0,1), 3 = unoccluded.
constexpr uint32_t vertexAo(uint32_t s1, uint32_t s2, uint32_t c) noexcept {
  return (s1 && s2) ? 0u : 3u - (s1 + s2 + c);
}
struct AoLut {
  uint8_t v[256];
  constexpr AoLut() : v{} {
    for (uint32_t m = 0; m < 256; ++m) {
      auto b = [m](int i) { return (m >> i) & 1u; };
      const uint32_t g0 = vertexAo(b(3), b(1), b(0)), g1 = vertexAo(b(4), b(1), b(2));
      const uint32_t g2 = vertexAo(b(4), b(6), b(7)), g3 = vertexAo(b(3), b(6), b(5));
      v[m] = uint8_t(g0 | (g1 << 2) | (g2 << 4) | (g3 << 6));
    }
  }
};
constexpr AoLut kAoLut{};

}  // namespace

uint64_t canonicalMeshHash(const CanonicalMesh& m) noexcept {
  uint64_t h = kFnvOffset;
  fnvU32(h, uint32_t(m.magic) | (uint32_t(m.version) << 16));
  for (const CanonicalLod& l : m.lods) {
    for (const BucketRange& r : l.face) { fnvU32(h, r.start); fnvU32(h, r.count); }
    fnvU32(h, l.skirt.start); fnvU32(h, l.skirt.count); fnvU32(h, l.blend.start); fnvU32(h, l.blend.count);
  }
  fnvU32(h, uint32_t(m.quads.size()));
  for (uint64_t q : m.quads) fnvU64(h, q);
  fnvU32(h, uint32_t(m.palette.size()));
  for (uint64_t p : m.palette) fnvU64(h, p);
  fnvU32(h, uint32_t(m.fallback.size()));
  for (const canon::Packed12& p : m.fallback) { fnvU32(h, p.w[0]); fnvU32(h, p.w[1]); fnvU32(h, p.w[2]); }
  return h;
}

struct UnifiedMesher::Grid {
  int dim = 16;
  uint16_t mat[18 * 18 * 18];
  uint16_t light[18 * 18 * 18];
  int n() const noexcept { return dim + 2; }
  int idx(int x, int y, int z) const noexcept { return (y * n() + z) * n() + x; }
};

UnifiedMesher::UnifiedMesher() : slotStamp_(size_t(65536) * 8, 0u) {}

int UnifiedMesher::paletteSlot(uint16_t mat, uint32_t face, CanonicalMesh& out) {
  const UnifiedMaterial& um = in_->materials[mat];
  const size_t key = size_t(mat) * 8 + (um.perFace ? face : 7u);
  const uint32_t e = slotStamp_[key];
  if ((e >> 10) == stamp_) return int(e & 0x3FFu);
  if (out.palette.size() >= canon::kMaxPalette) return -1;
  const uint32_t slot = uint32_t(out.palette.size());
  out.palette.push_back(canon::packPalette(um.perFace ? um.faces[face] : um.palette));
  slotStamp_[key] = (stamp_ << 10) | slot;
  return int(slot);
}

UnifiedMesher::Status UnifiedMesher::mesh(const UnifiedMeshInput& in, CanonicalMesh& out) {
  out = CanonicalMesh{};
  lastQuads_[0] = lastQuads_[1] = lastQuads_[2] = 0;
  if (!in.matId || !in.materials || in.materialCount == 0) return Status::BadInput;
  in_ = &in;
  if (++stamp_ >= (1u << 22)) { std::fill(slotStamp_.begin(), slotStamp_.end(), 0u); stamp_ = 1; }

  static thread_local Grid g0, gl;
  g0.dim = 16;
  for (int i = 0; i < 18 * 18 * 18; ++i) {
    const uint16_t m = in.matId[i];
    g0.mat[i] = m < in.materialCount ? m : 0;  // unknown IDs read as air (same as legacy)
    g0.light[i] = in.light ? uint16_t(in.light[i] & 0xFFFu) : uint16_t(63u << 6);
  }
  bool overflow = false;
  if (in.firstLod <= 0) buildLod(g0, 0, out, overflow);
  // Hull exposure for the LOD material pick: an occluding fine cell next to a
  // non-occluding one (apron included) is on the visible hull.
  static thread_local uint8_t exposed[18 * 18 * 18];
  if (!overflow) {
    std::memset(exposed, 0, sizeof exposed);
    auto occAt = [&](int x, int y, int z) {
      if (x < 0 || y < 0 || z < 0 || x > 17 || y > 17 || z > 17) return false;
      const uint16_t m = g0.mat[g0.idx(x, y, z)];
      return m != 0 && in.materials[m].occludes;
    };
    for (int y = 0; y < 18; ++y) for (int z = 0; z < 18; ++z) for (int x = 0; x < 18; ++x)
      if (occAt(x, y, z))
        exposed[g0.idx(x, y, z)] = uint8_t(!occAt(x + 1, y, z) || !occAt(x - 1, y, z) || !occAt(x, y + 1, z) ||
                                           !occAt(x, y - 1, z) || !occAt(x, y, z + 1) || !occAt(x, y, z - 1));
  }
  for (int lod = 1; lod <= 2 && !overflow; ++lod) {
    // Top-preserving, hull-conservative downsample (Q36c): a coarse cell is solid if any
    // fine cell is; its material is the topmost fine cell on the visible hull, else the
    // topmost occluding cell, else the topmost of any kind (no buried materials at L1/L2).
    const int s = lod == 1 ? 2 : 4, dim = 16 / s;
    gl.dim = dim;
    auto range = [&](int c, int& lo, int& hi) {
      if (c == 0) { lo = 0; hi = 1; } else if (c == dim + 1) { lo = 17; hi = 18; }
      else { lo = 1 + (c - 1) * s; hi = lo + s; }
    };
    for (int y = 0; y < dim + 2; ++y)
      for (int z = 0; z < dim + 2; ++z)
        for (int x = 0; x < dim + 2; ++x) {
          int y0, y1, z0, z1, x0, x1;
          range(y, y0, y1); range(z, z0, z1); range(x, x0, x1);
          uint16_t hullM = 0, occM = 0, anyM = 0; uint32_t bl = 0, sk = 0;
          for (int yy = y1 - 1; yy >= y0; --yy)
            for (int zz = z0; zz < z1; ++zz)
              for (int xx = x0; xx < x1; ++xx) {
                const int i = g0.idx(xx, yy, zz);
                const uint16_t m = g0.mat[i];
                if (m) {
                  if (!anyM) anyM = m;
                  if (!occM && in.materials[m].occludes) occM = m;
                  if (!hullM && exposed[i]) hullM = m;
                }
                bl = std::max<uint32_t>(bl, g0.light[i] & 63u);
                sk = std::max<uint32_t>(sk, (g0.light[i] >> 6) & 63u);
              }
          const int o = gl.idx(x, y, z);
          gl.mat[o] = hullM ? hullM : occM ? occM : anyM;
          gl.light[o] = uint16_t(bl | (sk << 6));
        }
    buildLod(gl, lod, out, overflow);
  }
  if (overflow) { out = CanonicalMesh{}; return Status::PaletteOverflow; }
  return Status::Ok;
}

void UnifiedMesher::buildLod(const Grid& g, int lod, CanonicalMesh& out, bool& overflow) {
  const int dim = g.dim, n = g.n();
  const uint32_t dimMask = (1u << dim) - 1u;
  const UnifiedMaterial* mats = in_->materials;

  // Occupancy planes, built once per LOD: bits along X for (y,z) rows, and bits along Z
  // for (x,y) rows (X faces need U = Z). occ = occluders, any = non-air.
  uint32_t occX[18][18] = {}, anyX[18][18] = {}, occZ[18][18] = {}, anyZ[18][18] = {};
  for (int y = 0; y < n; ++y)
    for (int z = 0; z < n; ++z)
      for (int x = 0; x < n; ++x) {
        const uint16_t m = g.mat[g.idx(x, y, z)];
        if (!m) continue;
        anyX[y][z] |= 1u << x; anyZ[x][y] |= 1u << z;
        if (mats[m].occludes) { occX[y][z] |= 1u << x; occZ[x][y] |= 1u << z; }
      }

  CanonicalLod& L = out.lods[size_t(lod)];
  std::vector<uint64_t> skirts;
  blendTmp_.clear();
  uint16_t rows[16];
  uint64_t key[16][16];

  auto emit = [&](uint32_t face, const int mn[3], const int ext[3], uint64_t k, uint32_t scale,
                  std::vector<uint64_t>& dst) {
    const uint16_t m = uint16_t((k >> 0) & 0xFFFFu);
    const int slot = paletteSlot(m, face, out);
    if (slot < 0) { overflow = true; return; }
    canon::DecodedQuad q;
    q.ox = uint32_t(mn[0]); q.oy = uint32_t(mn[1]); q.oz = uint32_t(mn[2]);
    q.w = uint32_t(ext[canon::faceUAxis(face)]); q.h = uint32_t(ext[canon::faceVAxis(face)]);
    q.face = face; q.coordScale = scale; q.paletteIdx = uint32_t(slot);
    q.cornerAo = uint32_t((k >> 16) & 0xFFu);
    q.aoDiagFlip = canon::aoDiagFlipFor(q.cornerAo);
    q.blockLight = uint32_t((k >> 24) & 63u); q.skyLight = uint32_t((k >> 30) & 63u);
    q.uvRot = uint32_t((k >> 36) & 3u);
    dst.push_back(canon::packQuad(q));
  };

  for (uint32_t face = 0; face < 6 && !overflow; ++face) {
    const int a = canon::faceAxis(face), ua = canon::faceUAxis(face), va = canon::faceVAxis(face);
    const int s = (face & 1u) ? -1 : 1;
    const uint32_t start = uint32_t(out.quads.size());
    // Plane word accessors (bits along U, apron coords): c = coordinate along the face
    // axis, v = coordinate along V.
    auto occP = [&](int c, int v) -> uint32_t { return a == 0 ? occZ[c][v] : a == 1 ? occX[c][v] : occX[v][c]; };
    auto anyP = [&](int c, int v) -> uint32_t { return a == 0 ? anyZ[c][v] : a == 1 ? anyX[c][v] : anyX[v][c]; };
    auto cellIdx = [&](int c, int u, int v) {  // apron coords
      int p[3]; p[a] = c; p[ua] = u; p[va] = v;
      return g.idx(p[0], p[1], p[2]);
    };
    for (int sl = 0; sl < dim && !overflow; ++sl) {
      const int c = sl + 1, cf = c + s;
      bool anyRow = false;
      // Bitwise visibility + match-key grid (fixes 1, 2, 4): one pass per row.
      for (int vr = 0; vr < dim; ++vr) {
        const int v = vr + 1;
        uint32_t vis = (anyP(c, v) >> 1) & dimMask & ~((occP(cf, v) >> 1) & dimMask);
        // Non-occluding cells (glass, water) also hide faces against the same material.
        uint32_t soft = vis & ~((occP(c, v) >> 1) & dimMask);
        while (soft) {
          const int u = std::countr_zero(soft); soft &= soft - 1;
          if (g.mat[cellIdx(c, u + 1, v)] == g.mat[cellIdx(cf, u + 1, v)]) vis &= ~(1u << u);
        }
        rows[vr] = uint16_t(vis);
        if (!vis) continue;
        anyRow = true;
        const uint32_t wm = occP(cf, v - 1), w0 = occP(cf, v), wp = occP(cf, v + 1);
        uint32_t bits = vis;
        while (bits) {
          const int u = std::countr_zero(bits); bits &= bits - 1;
          // apron bit (u+1) is the cell itself; u and u+2 are its -u / +u neighbours.
          const uint32_t nb = ((wm >> u) & 1u) | (((wm >> (u + 1)) & 1u) << 1) | (((wm >> (u + 2)) & 1u) << 2) |
                              (((w0 >> u) & 1u) << 3) | (((w0 >> (u + 2)) & 1u) << 4) |
                              (((wp >> u) & 1u) << 5) | (((wp >> (u + 1)) & 1u) << 6) | (((wp >> (u + 2)) & 1u) << 7);
          const uint16_t m = g.mat[cellIdx(c, u + 1, v)];
          const uint64_t lt = g.light[cellIdx(cf, u + 1, v)] & 0xFFFu;
          key[vr][u] = (uint64_t(1) << 63) | m | (uint64_t(kAoLut.v[nb]) << 16) | (lt << 24) |
                       (uint64_t(mats[m].uvRot & 3u) << 36);
        }
      }
      if (!anyRow) continue;
      // Greedy merge: integer key compares only.
      for (int vr = 0; vr < dim && !overflow; ++vr) {
        while (rows[vr]) {
          const int u = std::countr_zero(rows[vr]);
          const uint64_t k = key[vr][u];
          const int run = std::countr_one(uint32_t(rows[vr]) >> u);
          int w = 1;
          while (w < run && key[vr][u + w] == k) ++w;
          const uint32_t span = ((1u << w) - 1u) << u;
          int h = 1;
          while (vr + h < dim && (rows[vr + h] & span) == span) {
            bool same = true;
            for (int i = 0; i < w && same; ++i) same = key[vr + h][u + i] == k;
            if (!same) break;
            ++h;
          }
          for (int i = 0; i < h; ++i) rows[vr + i] = uint16_t(rows[vr + i] & ~span);
          int mn[3], ext[3];
          mn[a] = s > 0 ? sl + 1 : sl; mn[ua] = u; mn[va] = vr;
          ext[a] = 0; ext[ua] = w; ext[va] = h;
          const bool blend = (mats[k & 0xFFFFu].palette.modeFlags & canon::kModeAlphaBlend) != 0;
          emit(face, mn, ext, k, canon::kScaleCoarse1x, blend ? blendTmp_ : out.quads);
          if (lod >= 1 && !blend && !overflow) {
            // Fix 3: a skirt on every quad edge lying on the sub-chunk perimeter, one cell
            // deep behind the face, facing out of the sub-chunk. All six directions.
            const uint64_t sk = (k & ~(uint64_t(0xFF) << 16)) | (uint64_t(0xFF) << 16);  // skirts unshaded by AO
            for (int e = 0; e < 4; ++e) {
              const int axis = e < 2 ? ua : va;
              const int lo = e < 2 ? u : vr, len = e < 2 ? w : h;
              const bool pos = (e & 1) != 0;
              if (pos ? (lo + len != dim) : (lo != 0)) continue;
              const uint32_t sface = uint32_t(axis * 2 + (pos ? 0 : 1));
              int smn[3], sext[3];
              smn[a] = sl; sext[a] = 1;
              smn[axis] = pos ? dim : 0; sext[axis] = 0;
              const int other = 3 - a - axis;
              smn[other] = other == ua ? u : vr; sext[other] = other == ua ? w : h;
              emit(sface, smn, sext, sk, canon::kScaleLodSkirt, skirts);
            }
          }
        }
      }
    }
    L.face[face] = BucketRange{start, uint32_t(out.quads.size()) - start};
  }
  L.skirt.start = uint32_t(out.quads.size());
  out.quads.insert(out.quads.end(), skirts.begin(), skirts.end());
  L.skirt.count = uint32_t(skirts.size());
  L.blend.start = uint32_t(out.quads.size());
  out.quads.insert(out.quads.end(), blendTmp_.begin(), blendTmp_.end());
  L.blend.count = uint32_t(blendTmp_.size());
  uint32_t total = 0;
  for (const auto& r : L.face) total += r.count;
  lastQuads_[lod] = total + L.skirt.count + L.blend.count;
}

}  // namespace litholattice
