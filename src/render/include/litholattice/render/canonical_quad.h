// Canonical 64-bit packed quad, palette entry and Packed12 fallback (render pipeline
// unification, Archie rulings K1/K2, Q1-Q15, Q19; migration step 2, 2026-10-29).
//
// WHAT: the one mesh format every renderer reads. A quad is 8 bytes (two 32-bit words,
//   lo = position/size/face/flags, hi = palette/AO/light/uvRot); a palette entry is 8
//   bytes; Packed12 is a 12-byte vertex for non-cube geometry. Bit tables: see the
//   rulings doc, Tables 1-3. The GLSL mirror is canonical_quad_decode.glsl and must
//   match this file bit-for-bit (the engine render tests pin both against fixed vectors).
// GEOMETRY CONVENTION (ours, replaces the sketch's sign-anchored math):
//   (ox,oy,oz) is the MIN corner of the face rectangle in LOD-grid units, with the face
//   plane coordinate baked in (a +X face of cell x sits at ox = x+1). Face axes:
//   X faces: U=Z, V=Y; Y faces: U=X, V=Z; Z faces: U=X, V=Y. Geometric corners
//   g0=(0,0) g1=(1,0) g2=(1,1) g3=(0,1) in (u,v); cornerAo holds 2 bits per geometric
//   corner (3 = unoccluded). Faces whose U x V points inward (+X, +Y, -Z) are emitted
//   in mirrored order so the shared 0,1,2 / 2,3,0 index buffer is CCW from outside.
//   aoDiagFlip rotates the vertex order by one, moving the split to the g1-g3 diagonal.
#pragma once
#include <cstdint>

namespace litholattice::canon {

inline constexpr uint16_t kMeshMagic = 0x4C51;   // 'QL'
inline constexpr uint16_t kMeshVersion = 1;
inline constexpr uint32_t kMaxPalette = 1024;

enum : uint32_t {
  kModeCutout = 1u << 0, kModeGlassDither = 1u << 1, kModeAlphaBlend = 1u << 2, kModeOit = 1u << 3,
  kModeAdditive = 1u << 4, kModeAnimated = 1u << 5, kModeLeafWind = 1u << 6, kModeGroundPulse = 1u << 7,
};
enum : uint32_t { kScaleCoarse1x = 0, kScaleMicroQtr = 1, kScaleLodSkirt = 2 };

struct DecodedQuad {
  uint32_t ox = 0, oy = 0, oz = 0;  // 0..63
  uint32_t w = 1, h = 1;            // 1..16
  uint32_t face = 0;                // 0..5: +X -X +Y -Y +Z -Z
  uint32_t aoDiagFlip = 0, coordScale = 0;
  uint32_t paletteIdx = 0;          // 0..1023
  uint32_t cornerAo = 0xFF;         // 2 bits per geometric corner g0..g3
  uint32_t blockLight = 0, skyLight = 0;  // 0..63
  uint32_t uvRot = 0;               // 0..3
  constexpr bool operator==(const DecodedQuad&) const = default;
};

constexpr uint64_t packQuad(const DecodedQuad& q) noexcept {
  const uint32_t lo = (q.ox & 0x3Fu) | ((q.oy & 0x3Fu) << 6) | ((q.oz & 0x3Fu) << 12) |
                      (((q.w - 1u) & 0xFu) << 18) | (((q.h - 1u) & 0xFu) << 22) | ((q.face & 7u) << 26) |
                      ((q.aoDiagFlip & 1u) << 29) | ((q.coordScale & 3u) << 30);
  const uint32_t hi = (q.paletteIdx & 0x3FFu) | ((q.cornerAo & 0xFFu) << 10) | ((q.blockLight & 0x3Fu) << 18) |
                      ((q.skyLight & 0x3Fu) << 24) | ((q.uvRot & 3u) << 30);
  return (uint64_t(hi) << 32) | lo;
}

constexpr DecodedQuad unpackQuad(uint64_t raw) noexcept {
  const uint32_t lo = uint32_t(raw), hi = uint32_t(raw >> 32);
  DecodedQuad q;
  q.ox = lo & 0x3Fu; q.oy = (lo >> 6) & 0x3Fu; q.oz = (lo >> 12) & 0x3Fu;
  q.w = ((lo >> 18) & 0xFu) + 1u; q.h = ((lo >> 22) & 0xFu) + 1u; q.face = (lo >> 26) & 7u;
  q.aoDiagFlip = (lo >> 29) & 1u; q.coordScale = (lo >> 30) & 3u;
  q.paletteIdx = hi & 0x3FFu; q.cornerAo = (hi >> 10) & 0xFFu; q.blockLight = (hi >> 18) & 0x3Fu;
  q.skyLight = (hi >> 24) & 0x3Fu; q.uvRot = (hi >> 30) & 3u;
  return q;
}

struct PaletteFields {
  uint16_t layerId = 0, pbrMatId = 0, rgb565Tint = 0xFFFF;
  uint8_t modeFlags = 0, emissive = 0, pulse = 0;  // emissive/pulse 0..15
  constexpr bool operator==(const PaletteFields&) const = default;
};

constexpr uint64_t packPalette(const PaletteFields& p) noexcept {
  const uint32_t lo = uint32_t(p.layerId) | (uint32_t(p.pbrMatId) << 16);
  const uint32_t hi = uint32_t(p.rgb565Tint) | (uint32_t(p.modeFlags) << 16) |
                      (uint32_t((p.emissive & 0xFu) | ((p.pulse & 0xFu) << 4)) << 24);
  return (uint64_t(hi) << 32) | lo;
}
constexpr PaletteFields unpackPalette(uint64_t raw) noexcept {
  const uint32_t lo = uint32_t(raw), hi = uint32_t(raw >> 32);
  PaletteFields p;
  p.layerId = uint16_t(lo); p.pbrMatId = uint16_t(lo >> 16); p.rgb565Tint = uint16_t(hi);
  p.modeFlags = uint8_t(hi >> 16); p.emissive = uint8_t((hi >> 24) & 0xFu); p.pulse = uint8_t((hi >> 28) & 0xFu);
  return p;
}

/// M7 fallback palette marker: pbrMatId == kLegacyLookMarker means rgb565Tint carries the
/// legacy look (shade mode << 8 | value) of a Packed12 fallback vertex, not a colour.
inline constexpr uint16_t kLegacyLookMarker = 0xFFFF;

/// Packed12 fallback vertex (Table 3): fixed-point 1/256 position, 1/2048 UV.
/// TODO(Archie ruling 3, 2026-10-30): rotated / custom-UV faces ride Packed12 for now
/// (accepted). Refactor: give rotated cube faces a canonical quad path (uvRot already
/// exists in the quad) and keep Packed12 for true non-cube geometry only.
struct Packed12Fields {
  uint32_t fx = 0, fy = 0, fz = 0;  // 0..4095
  uint32_t u = 0, v = 0;            // 0..4095
  uint32_t normalId = 0;            // 0..15
  uint32_t paletteIdx = 0, ao = 3, wind = 0, blockLight = 0, skyLight = 0;
  constexpr bool operator==(const Packed12Fields&) const = default;
};
struct Packed12 { uint32_t w[3]; };
static_assert(sizeof(Packed12) == 12);

constexpr Packed12 packPacked12(const Packed12Fields& f) noexcept {
  Packed12 p{};
  p.w[0] = (f.fx & 0xFFFu) | ((f.fy & 0xFFFu) << 12) | ((f.fz & 0xFFu) << 24);
  p.w[1] = ((f.fz >> 8) & 0xFu) | ((f.u & 0xFFFu) << 4) | ((f.v & 0xFFFu) << 16) | ((f.normalId & 0xFu) << 28);
  p.w[2] = (f.paletteIdx & 0x3FFu) | ((f.ao & 3u) << 10) | ((f.wind & 0x3Fu) << 12) | ((f.blockLight & 0x3Fu) << 18) |
           ((f.skyLight & 0x3Fu) << 24);
  return p;
}
constexpr Packed12Fields unpackPacked12(const Packed12& p) noexcept {
  Packed12Fields f;
  f.fx = p.w[0] & 0xFFFu; f.fy = (p.w[0] >> 12) & 0xFFFu; f.fz = ((p.w[0] >> 24) & 0xFFu) | ((p.w[1] & 0xFu) << 8);
  f.u = (p.w[1] >> 4) & 0xFFFu; f.v = (p.w[1] >> 16) & 0xFFFu; f.normalId = (p.w[1] >> 28) & 0xFu;
  f.paletteIdx = p.w[2] & 0x3FFu; f.ao = (p.w[2] >> 10) & 3u; f.wind = (p.w[2] >> 12) & 0x3Fu;
  f.blockLight = (p.w[2] >> 18) & 0x3Fu; f.skyLight = (p.w[2] >> 24) & 0x3Fu;
  return f;
}

/// Face axis helpers: normal axis, U axis, V axis (0=X,1=Y,2=Z).
constexpr int faceAxis(uint32_t face) noexcept { return int(face >> 1); }
constexpr int faceUAxis(uint32_t face) noexcept { return faceAxis(face) == 0 ? 2 : 0; }
constexpr int faceVAxis(uint32_t face) noexcept { return faceAxis(face) == 1 ? 2 : 1; }
/// True for faces whose U x V points inward; those emit corners in mirrored order.
constexpr bool faceMirrored(uint32_t face) noexcept { return face == 0 || face == 2 || face == 5; }

/// Geometric corner (0..3) used by pulled vertex `vertexIdx` (0..3) of a quad.
constexpr uint32_t geomCorner(uint32_t face, uint32_t aoDiagFlip, uint32_t vertexIdx) noexcept {
  const uint32_t e = (vertexIdx + aoDiagFlip) & 3u;
  return faceMirrored(face) ? ((4u - e) & 3u) : e;  // mirrored: g0, g3, g2, g1
}

struct PulledCorner { int32_t pos[3]; uint32_t g; uint32_t ao; uint32_t uv[2]; };

/// Integer decode of one pulled vertex in LOD-grid units (multiply by the LOD cell size,
/// or 0.25 for micro quads, to get sub-chunk space). This is the reference the GLSL
/// mirror and the software span decoder are tested against.
constexpr PulledCorner pullCorner(uint64_t raw, uint32_t vertexIdx) noexcept {
  const DecodedQuad q = unpackQuad(raw);
  PulledCorner c{};
  c.g = geomCorner(q.face, q.aoDiagFlip, vertexIdx);
  const uint32_t cu = (c.g == 1u || c.g == 2u) ? 1u : 0u, cv = (c.g >= 2u) ? 1u : 0u;
  c.pos[0] = int32_t(q.ox); c.pos[1] = int32_t(q.oy); c.pos[2] = int32_t(q.oz);
  c.pos[faceUAxis(q.face)] += int32_t(cu * q.w);
  c.pos[faceVAxis(q.face)] += int32_t(cv * q.h);
  c.ao = (q.cornerAo >> (c.g * 2u)) & 3u;
  const uint32_t ut = cu * q.w, vt = cv * q.h;
  switch (q.uvRot) {
    default: case 0: c.uv[0] = ut; c.uv[1] = vt; break;
    case 1: c.uv[0] = q.h - vt; c.uv[1] = ut; break;
    case 2: c.uv[0] = q.w - ut; c.uv[1] = q.h - vt; break;
    case 3: c.uv[0] = vt; c.uv[1] = q.w - ut; break;
  }
  return c;
}

/// Standard anisotropy fix: split along the g1-g3 diagonal when g0+g2 is brighter.
constexpr uint32_t aoDiagFlipFor(uint32_t cornerAo) noexcept {
  const uint32_t a0 = cornerAo & 3u, a1 = (cornerAo >> 2) & 3u, a2 = (cornerAo >> 4) & 3u, a3 = (cornerAo >> 6) & 3u;
  return (a0 + a2 > a1 + a3) ? 1u : 0u;
}

}  // namespace litholattice::canon
