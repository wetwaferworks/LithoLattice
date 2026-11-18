// Engine render tests for the canonical packed format and UnifiedMesher (migration
// steps 2-3, 2026-10-29). Runs inside `litholattice_tests render`.
#include <cstdio>
#include <cstdint>
#include <set>
#include <tuple>
#include <thread>
#include <vector>
#include "litholattice/render/canonical_quad.h"
#include "litholattice/render/unified_mesher.h"
#include "litholattice/render/mesher.h"
#include <cstring>

namespace litholattice {
namespace {

int g_cmFails = 0;
void cmCheck(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_cmFails;
}

// M12: the GPU pull stream (canonPullReference, mirrored by canonical_pull_legacy.glsl)
// expands to the same quads as decodeCanonicalMesh (as sorted 64-byte quad sets).
bool pullMatches(const CanonicalMesh& m) {
  std::vector<PackedVertex> dec, got;
  CanonPullStream ps;
  if (!decodeCanonicalMesh(m, dec) || !buildCanonPullStream(m, ps)) return false;
  for (uint32_t q = 0; q < ps.fbQuads + ps.quads; ++q)
    for (uint32_t k = 0; k < 4; ++k) got.push_back(canonPullReference(ps, q, k));
  auto quads = [](const std::vector<PackedVertex>& v) {
    std::multiset<std::vector<uint8_t>> out;
    for (size_t i = 0; i + 4 <= v.size(); i += 4) {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&v[i]);
      out.emplace(b, b + 4 * sizeof(PackedVertex));
    }
    return out;
  };
  return dec.size() == got.size() && !dec.empty() && quads(dec) == quads(got);
}

uint32_t rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

struct Scene {
  std::vector<uint16_t> mat = std::vector<uint16_t>(18 * 18 * 18, 0);
  std::vector<uint16_t> light = std::vector<uint16_t>(18 * 18 * 18, 0);
  std::vector<UnifiedMaterial> mats;
  Scene(uint32_t count = 8) : mats(count) {
    for (uint32_t i = 0; i < count; ++i) { mats[i].palette.layerId = uint16_t(i); mats[i].uvRot = uint8_t(i & 3u); }
    mats[0].occludes = false;
  }
  uint16_t& at(int x, int y, int z) { return mat[size_t(UnifiedMeshInput::apronIdx(x, y, z))]; }
  UnifiedMeshInput input() const {
    UnifiedMeshInput in; in.matId = mat.data(); in.light = light.data(); in.materials = mats.data();
    in.materialCount = uint32_t(mats.size()); return in;
  }
};

bool occ(const Scene& s, int x, int y, int z) {
  const uint16_t m = s.mat[size_t(UnifiedMeshInput::apronIdx(x, y, z))];
  return m && s.mats[m].occludes;
}

// Scalar reference: every exposed face with its material, AO and light, then
// check the decoded quads cover each exactly once with matching attributes.
bool l0MatchesReference(const Scene& s, const CanonicalMesh& m, const char** why) {
  struct Key { uint32_t face; int x, y, z; bool operator<(const Key& o) const {
    return std::tie(face, x, y, z) < std::tie(o.face, o.x, o.y, o.z); } };
  std::set<Key> seen;
  const CanonicalRanges& L = m.ranges;
  auto walk = [&](BucketRange r) -> bool {
    for (uint32_t i = r.start; i < r.start + r.count; ++i) {
      const canon::DecodedQuad q = canon::unpackQuad(m.quads[i]);
      const int a = canon::faceAxis(q.face), ua = canon::faceUAxis(q.face), va = canon::faceVAxis(q.face);
      const int sg = (q.face & 1u) ? -1 : 1;
      const canon::PaletteFields pf = canon::unpackPalette(m.palette[q.paletteIdx]);
      if (canon::aoDiagFlipFor(q.cornerAo) != q.aoDiagFlip) { *why = "aoDiagFlip"; return false; }
      for (uint32_t dv = 0; dv < q.h; ++dv)
        for (uint32_t du = 0; du < q.w; ++du) {
          int p[3] = {int(q.ox), int(q.oy), int(q.oz)};
          if (sg > 0) p[a] -= 1;
          p[ua] += int(du); p[va] += int(dv);
          const int x = p[0] + 1, y = p[1] + 1, z = p[2] + 1;  // apron coords
          if (!seen.insert({q.face, x, y, z}).second) { *why = "face covered twice"; return false; }
          const uint16_t mt = s.mat[size_t(UnifiedMeshInput::apronIdx(x, y, z))];
          if (pf.layerId != mt) { *why = "material"; return false; }
          int f[3] = {x, y, z}; f[a] += sg;
          const uint16_t lt = s.light[size_t(UnifiedMeshInput::apronIdx(f[0], f[1], f[2]))];
          if (q.blockLight != (lt & 63u) || q.skyLight != ((lt >> 6) & 63u)) { *why = "light"; return false; }
          if (q.uvRot != (mt & 3u)) { *why = "uvRot"; return false; }
          auto o = [&](int du2, int dv2) { int t[3] = {f[0], f[1], f[2]}; t[ua] += du2; t[va] += dv2; return occ(s, t[0], t[1], t[2]) ? 1u : 0u; };
          auto vao = [](uint32_t s1, uint32_t s2, uint32_t c) { return (s1 && s2) ? 0u : 3u - (s1 + s2 + c); };
          const uint32_t ao = vao(o(-1, 0), o(0, -1), o(-1, -1)) | (vao(o(1, 0), o(0, -1), o(1, -1)) << 2) |
                              (vao(o(1, 0), o(0, 1), o(1, 1)) << 4) | (vao(o(-1, 0), o(0, 1), o(-1, 1)) << 6);
          if (ao != q.cornerAo) { *why = "AO"; return false; }
        }
    }
    return true;
  };
  for (const BucketRange& r : L.face) if (!walk(r)) return false;
  if (!walk(L.blend)) return false;
  size_t expected = 0;
  for (int y = 1; y <= 16; ++y) for (int z = 1; z <= 16; ++z) for (int x = 1; x <= 16; ++x) {
    const uint16_t mt = s.mat[size_t(UnifiedMeshInput::apronIdx(x, y, z))];
    if (!mt) continue;
    for (int f = 0; f < 6; ++f) {
      int t[3] = {x, y, z}; t[f >> 1] += (f & 1) ? -1 : 1;
      const uint16_t nm = s.mat[size_t(UnifiedMeshInput::apronIdx(t[0], t[1], t[2]))];
      if (nm && s.mats[nm].occludes) continue;
      if (!s.mats[mt].occludes && nm == mt) continue;
      ++expected;
    }
  }
  if (expected != seen.size()) { *why = "exposed face count"; return false; }
  return true;
}

}  // namespace

int testCanonicalMesh() {
  g_cmFails = 0;
  std::printf("canonical mesh / UnifiedMesher\n");
  // --- Step 2: bit layout pins and round trips ---------------------------------------
  {
    canon::DecodedQuad q; q.paletteIdx = 1023; q.cornerAo = 0;
    cmCheck(canon::packQuad(q) == (uint64_t(0x3FF) << 32), "quad: paletteIdx occupies hi bits 0..9");
    canon::DecodedQuad r; r.ox = 63; r.oy = 0; r.oz = 0; r.w = 16; r.h = 1; r.face = 5; r.aoDiagFlip = 1; r.coordScale = 2; r.cornerAo = 0;
    cmCheck(uint32_t(canon::packQuad(r)) == (63u | (15u << 18) | (5u << 26) | (1u << 29) | (2u << 30)), "quad: lo word bit table (ox, w-1, face, flip, scale)");
    uint32_t s = 0x1234567u; bool ok = true;
    for (int i = 0; i < 20000 && ok; ++i) {
      canon::DecodedQuad d;
      d.ox = rng(s) & 63; d.oy = rng(s) & 63; d.oz = rng(s) & 63; d.w = 1 + (rng(s) & 15); d.h = 1 + (rng(s) & 15);
      d.face = rng(s) % 6; d.aoDiagFlip = rng(s) & 1; d.coordScale = rng(s) % 3; d.paletteIdx = rng(s) & 1023;
      d.cornerAo = rng(s) & 255; d.blockLight = rng(s) & 63; d.skyLight = rng(s) & 63; d.uvRot = rng(s) & 3;
      ok = canon::unpackQuad(canon::packQuad(d)) == d;
      canon::PaletteFields p{uint16_t(rng(s)), uint16_t(rng(s)), uint16_t(rng(s)), uint8_t(rng(s)), uint8_t(rng(s) & 15), uint8_t(rng(s) & 15)};
      ok = ok && canon::unpackPalette(canon::packPalette(p)) == p;
      canon::Packed12Fields f; f.fx = rng(s) & 4095; f.fy = rng(s) & 4095; f.fz = rng(s) & 4095; f.u = rng(s) & 4095; f.v = rng(s) & 4095;
      f.normalId = rng(s) & 15; f.paletteIdx = rng(s) & 1023; f.ao = rng(s) & 3; f.wind = rng(s) & 63; f.blockLight = rng(s) & 63; f.skyLight = rng(s) & 63;
      ok = ok && canon::unpackPacked12(canon::packPacked12(f)) == f;
    }
    cmCheck(ok, "quad / palette / Packed12 round-trip 20k random records");
    // Winding: triangle (0,1,2) is CCW seen from outside for every face, with and without flip.
    bool wind = true;
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t flip = 0; flip < 2; ++flip) {
        canon::DecodedQuad d; d.face = face; d.aoDiagFlip = flip; d.w = 3; d.h = 2; d.ox = d.oy = d.oz = 5;
        const uint64_t raw = canon::packQuad(d);
        canon::PulledCorner c[4];
        for (uint32_t v = 0; v < 4; ++v) c[v] = canon::pullCorner(raw, v);
        const int e1[3] = {c[1].pos[0] - c[0].pos[0], c[1].pos[1] - c[0].pos[1], c[1].pos[2] - c[0].pos[2]};
        const int e2[3] = {c[2].pos[0] - c[0].pos[0], c[2].pos[1] - c[0].pos[1], c[2].pos[2] - c[0].pos[2]};
        const int cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const int na = canon::faceAxis(face), sg = (face & 1u) ? -1 : 1;
        wind = wind && cr[na] * sg > 0 && cr[(na + 1) % 3] == 0 && cr[(na + 2) % 3] == 0;
      }
    cmCheck(wind, "pulled corners: CCW from outside on all 6 faces, both diagonals");
  }
  // --- Step 3: UnifiedMesher ---------------------------------------------------------
  UnifiedMesher mesher;
  {
    Scene sc; sc.at(8, 8, 8) = 1;
    for (auto& l : sc.light) l = uint16_t(5u | (60u << 6));
    CanonicalMesh m; const auto st = mesher.mesh(sc.input(), m);
    uint32_t n0 = 0; bool unit = true;
    for (auto r : m.ranges.face) { n0 += r.count; for (uint32_t i = r.start; i < r.start + r.count; ++i) { auto q = canon::unpackQuad(m.quads[i]); unit = unit && q.w == 1 && q.h == 1 && q.cornerAo == 0xFF && q.blockLight == 5 && q.skyLight == 60; } }
    cmCheck(st == UnifiedMesher::Status::Ok && n0 == 6 && unit && m.palette.size() == 1, "single cube: 6 unshaded unit quads, light from the front cell, 1 palette entry");
  }
  {
    // Random fills, incl. non-occluding materials (5 = glass, 6 = water alpha-blend) and apron.
    bool ok = true; const char* why = "";
    for (uint32_t seed = 1; seed <= 24 && ok; ++seed) {
      Scene sc; sc.mats[5].occludes = false; sc.mats[5].palette.modeFlags = canon::kModeGlassDither;
      sc.mats[6].occludes = false; sc.mats[6].palette.modeFlags = canon::kModeAlphaBlend;
      uint32_t s = seed * 2654435761u; const uint32_t fill = 20 + seed * 3;
      for (int i = 0; i < 18 * 18 * 18; ++i) {
        sc.mat[size_t(i)] = (rng(s) % 100) < fill ? uint16_t(1 + rng(s) % 7) : 0;
        sc.light[size_t(i)] = uint16_t((rng(s) & 63) | ((rng(s) & 3) == 0 ? (rng(s) & 63) << 6 : 63u << 6));
      }
      CanonicalMesh m;
      ok = mesher.mesh(sc.input(), m) == UnifiedMesher::Status::Ok && l0MatchesReference(sc, m, &why);
    }
    char buf[160]; std::snprintf(buf, sizeof buf, "decode equivalence: L0 quads cover every exposed face once, exact material/AO/light (24 random fills)%s%s", ok ? "" : " - ", ok ? "" : why);
    cmCheck(ok, buf);
  }
  {
    // Fix 1: an inner corner darkens exactly the corners next to the occluder and flips the split.
    Scene sc; for (int x = 1; x <= 16; ++x) for (int z = 1; z <= 16; ++z) sc.at(x, 4, z) = 1;
    sc.at(8, 5, 8) = 2;
    CanonicalMesh m; mesher.mesh(sc.input(), m);
    int dark = 0, flips = 0; const BucketRange r = m.ranges.face[2];
    for (uint32_t i = r.start; i < r.start + r.count; ++i) { auto q = canon::unpackQuad(m.quads[i]); if (canon::unpackPalette(m.palette[q.paletteIdx]).layerId == 1 && q.cornerAo != 0xFF) { ++dark; flips += int(q.aoDiagFlip); } }
    cmCheck(dark >= 4 && flips >= 1, "real AO: floor quads around a block carry corner AO, diagonal flip fires");
  }
  {
    // Fix 2: buried materials never reach the palette.
    Scene sc; for (int y = 1; y <= 16; ++y) for (int z = 1; z <= 16; ++z) for (int x = 1; x <= 16; ++x)
      sc.at(x, y, z) = (x == 1 || x == 16 || y == 1 || y == 16 || z == 1 || z == 16) ? 1 : uint16_t(2 + ((x + y + z) % 5));
    CanonicalMesh m; mesher.mesh(sc.input(), m);
    cmCheck(m.palette.size() == 1 && canon::unpackPalette(m.palette[0]).layerId == 1, "no palette pollution: interior materials take no slots");
    uint32_t q0 = 0; for (auto r : m.ranges.face) q0 += r.count;
    cmCheck(q0 == 6, "greedy: a 16^3 shell meshes to 6 quads at L0");
  }
  {
    // Fix 2: >1024 distinct exposed materials -> overflow status, never a clamp.
    Scene sc(1200); bool any = false;
    uint16_t id = 1;
    for (int y = 1; y <= 16; ++y) for (int z = 1; z <= 16; ++z) for (int x = 1; x <= 16; ++x) if (((x + y + z) & 1) == 0 && id < 1200) { sc.at(x, y, z) = id++; any = true; }
    CanonicalMesh m; const auto st = mesher.mesh(sc.input(), m);
    cmCheck(any && st == UnifiedMesher::Status::PaletteOverflow && m.quads.empty() && m.palette.empty(), "palette overflow (>1024) reports PaletteOverflow with an empty mesh");
    for (auto& v : sc.mat) if (v > 512) v = 0;
    cmCheck(mesher.mesh(sc.input(), m) == UnifiedMesher::Status::Ok && m.palette.size() == 512, "same mesher recovers: 512 materials fit");
  }
  {
    // M12: the GPU pull stream matches the CPU decode on a slab scene (floor + wall).
    Scene sc;
    for (int x = 1; x <= 16; ++x) for (int z = 1; z <= 16; ++z) { sc.at(x, 1, z) = 1; sc.at(x, 2, z) = 1; }
    for (int y = 1; y <= 16; ++y) for (int z = 1; z <= 16; ++z) { sc.at(1, y, z) = 2; sc.at(2, y, z) = 2; }
    CanonicalMesh open; mesher.mesh(sc.input(), open);
    cmCheck(pullMatches(open), "GPU pull stream == CPU decode (canonical_pull_legacy.glsl mirror)");
  }
  {
    // M11 (Archie ruling 1): a 1500-entry palette splits in two, stream order preserved.
    CanonicalMesh w; std::vector<uint32_t> qs, fs; uint32_t r = 7;
    for (uint32_t i = 0; i < 1500; ++i) { canon::PaletteFields pf; pf.layerId = uint16_t(i); w.palette.push_back(canon::packPalette(pf)); }
    std::vector<PackedVertex> ref;
    for (uint32_t q = 0; q < 100; ++q)
      for (int v = 0; v < 4; ++v) {
        canon::Packed12Fields f; f.fx = rng(r) & 4095; f.fy = rng(r) & 4095; f.fz = rng(r) & 4095; f.u = (rng(r) & 255) << 4; f.v = (rng(r) & 255) << 4;
        f.normalId = rng(r) & 15; f.paletteIdx = 0; f.skyLight = 63;
        const uint32_t slot = q * 3;
        CanonicalMesh one; one.palette.push_back(w.palette[slot]); one.fallback.push_back(canon::packPacked12(f));
        std::vector<PackedVertex> t; decodeCanonicalMesh(one, t); ref.insert(ref.end(), t.begin(), t.end());
        f.paletteIdx = slot & 0x3FFu; w.fallback.push_back(canon::packPacked12(f)); fs.push_back(slot);
      }
    for (uint32_t f = 0; f < 6; ++f) {
      const uint32_t st = uint32_t(w.quads.size());
      for (uint32_t i = 0; i < 250; ++i) {
        canon::DecodedQuad d; d.face = f; d.ox = rng(r) % 16; d.oy = rng(r) % 16; d.oz = rng(r) % 16; d.w = 1 + rng(r) % 4; d.h = 1 + rng(r) % 4; d.cornerAo = 0xFF; d.skyLight = 63;
        const uint32_t slot = (f * 250 + i) % 1500;
        CanonicalMesh one; one.palette.push_back(w.palette[slot]); d.paletteIdx = 0; one.quads.push_back(canon::packQuad(d)); one.ranges.face[f] = BucketRange{0, 1};
        std::vector<PackedVertex> t; decodeCanonicalMesh(one, t); ref.insert(ref.end(), t.begin(), t.end());
        d.paletteIdx = slot & 0x3FFu; w.quads.push_back(canon::packQuad(d)); qs.push_back(slot);
      }
      w.ranges.face[f] = BucketRange{st, 250};
    }
    const bool ok = splitCanonicalTwo(w, qs, fs);
    std::vector<PackedVertex> got; decodeCanonicalMesh(w, got);
    cmCheck(ok && w.next && w.palette.size() <= canon::kMaxPalette && w.next->palette.size() <= canon::kMaxPalette &&
            got.size() == ref.size() && std::memcmp(got.data(), ref.data(), ref.size() * sizeof(PackedVertex)) == 0,
            "palette overflow split: two halves, each <= 1024 entries, decode == unsplit stream");
    cmCheck(pullMatches(w), "GPU pull stream == CPU decode on a split mesh (Packed12 fallback + both halves)");
    CanonicalMesh big; canon::PaletteFields gp; gp.layerId = 300; gp.modeFlags = canon::kModeGroundPulse; gp.pulse = 9;
    canon::PaletteFields em; em.layerId = 7; em.emissive = 15;
    big.palette = {canon::packPalette(gp), canon::packPalette(em)};
    for (uint32_t f = 0; f < 6; ++f) {
      canon::DecodedQuad d; d.face = f; d.ox = f & 1u ? 0 : 1; d.oy = 0; d.oz = 0; d.w = 16; d.h = 16; d.cornerAo = 0x55u * (f % 4); d.paletteIdx = f & 1u;
      big.ranges.face[f] = BucketRange{uint32_t(big.quads.size()), 1}; big.quads.push_back(canon::packQuad(d));
    }
    cmCheck(pullMatches(big), "GPU pull: 16x16 quads (uint8 UV wrap), ground-pulse / emissive keys, layer > 255");
  }
  {
    // Determinism: the same bytes from 1 and 8 threads, and from a fresh mesher.
    Scene sc; uint32_t s = 99;
    for (auto& v : sc.mat) v = (rng(s) % 3) ? uint16_t(rng(s) % 8) : 0;
    for (auto& l : sc.light) l = uint16_t(rng(s) & 0xFFF);
    CanonicalMesh m; mesher.mesh(sc.input(), m); const uint64_t ref = canonicalMeshHash(m);
    std::vector<uint64_t> got(8, 0); std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) ts.emplace_back([&, t] { UnifiedMesher um; CanonicalMesh mm; for (int k = 0; k < 4; ++k) um.mesh(sc.input(), mm); got[size_t(t)] = canonicalMeshHash(mm); });
    for (auto& t : ts) t.join();
    bool same = true; for (uint64_t h : got) same = same && h == ref;
    cmCheck(same && ref != 0, "MeshHash identical across 8 threads and fresh meshers");
  }
  {  // M6: per-face looks (registry feed)
    Scene sc; sc.at(8, 8, 8) = 1;
    sc.mats[1].perFace = true;
    for (int f = 0; f < 6; ++f) sc.mats[1].faces[size_t(f)].layerId = uint16_t(100 + f);
    CanonicalMesh m; const auto st = mesher.mesh(sc.input(), m);
    std::set<uint32_t> pal;
    bool faceOk = st == UnifiedMesher::Status::Ok;
    for (int f = 0; f < 6; ++f) {
      const auto r = m.ranges.face[size_t(f)];
      for (uint32_t i = r.start; i < r.start + r.count; ++i) {
        const auto q = canon::unpackQuad(m.quads[i]);
        pal.insert(q.paletteIdx);
        faceOk = faceOk && canon::unpackPalette(m.palette[q.paletteIdx]).layerId == uint16_t(100 + f);
      }
    }
    cmCheck(faceOk && m.palette.size() == 6 && pal.size() == 6, "per-face looks: each face direction takes its own palette entry");
  }
  return g_cmFails;
}

}  // namespace litholattice

int testCanonicalMeshEntry() { return litholattice::testCanonicalMesh(); }
