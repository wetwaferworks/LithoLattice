// canonical_quad_decode.glsl - render pipeline M4 (Archie Q18/Q19, migration step 2).
// Shared GLSL decode of the canonical 64-bit PackedQuad + PaletteEntry for Vulkan and GL 3.3.
// Bit-for-bit mirror of src/render/include/litholattice/render/canonical_quad.h
// (unpackQuad / unpackPalette / geomCorner / pullCorner); change both together.
// GLSL 330 compatible (uint ops, no 64-bit types): a quad / palette entry is a uvec2(lo, hi).
#ifndef LL_CANONICAL_QUAD_DECODE_GLSL
#define LL_CANONICAL_QUAD_DECODE_GLSL

const uint kCanonModeCutout = 1u, kCanonModeGlassDither = 2u, kCanonModeAlphaBlend = 4u, kCanonModeOit = 8u;
const uint kCanonModeAdditive = 16u, kCanonModeAnimated = 32u, kCanonModeLeafWind = 64u, kCanonModeGroundPulse = 128u;
const uint kCanonScaleCoarse1x = 0u, kCanonScaleMicroQtr = 1u, kCanonScaleLodSkirt = 2u;

struct CanonQuad {
  uvec3 o;          // 0..63 per axis (LOD-grid units)
  uint w, h;        // 1..16
  uint face;        // 0..5: +X -X +Y -Y +Z -Z
  uint aoDiagFlip, coordScale;
  uint paletteIdx;  // 0..1023
  uint cornerAo;    // 2 bits per geometric corner
  uint blockLight, skyLight;  // 0..63
  uint uvRot;       // 0..3
};

CanonQuad canonUnpackQuad(uvec2 raw) {
  uint lo = raw.x, hi = raw.y;
  CanonQuad q;
  q.o = uvec3(lo & 0x3Fu, (lo >> 6u) & 0x3Fu, (lo >> 12u) & 0x3Fu);
  q.w = ((lo >> 18u) & 0xFu) + 1u;
  q.h = ((lo >> 22u) & 0xFu) + 1u;
  q.face = (lo >> 26u) & 7u;
  q.aoDiagFlip = (lo >> 29u) & 1u;
  q.coordScale = (lo >> 30u) & 3u;
  q.paletteIdx = hi & 0x3FFu;
  q.cornerAo = (hi >> 10u) & 0xFFu;
  q.blockLight = (hi >> 18u) & 0x3Fu;
  q.skyLight = (hi >> 24u) & 0x3Fu;
  q.uvRot = (hi >> 30u) & 3u;
  return q;
}

struct CanonPalette { uint layerId, pbrMatId, rgb565Tint, modeFlags, emissive, pulse; };

CanonPalette canonUnpackPalette(uvec2 raw) {
  CanonPalette p;
  p.layerId = raw.x & 0xFFFFu;
  p.pbrMatId = raw.x >> 16u;
  p.rgb565Tint = raw.y & 0xFFFFu;
  p.modeFlags = (raw.y >> 16u) & 0xFFu;
  p.emissive = (raw.y >> 24u) & 0xFu;
  p.pulse = (raw.y >> 28u) & 0xFu;
  return p;
}

vec3 canonTintRgb(uint rgb565) {
  return vec3(float((rgb565 >> 11u) & 31u) / 31.0, float((rgb565 >> 5u) & 63u) / 63.0, float(rgb565 & 31u) / 31.0);
}

int canonFaceAxis(uint face) { return int(face >> 1u); }
int canonFaceUAxis(uint face) { return canonFaceAxis(face) == 0 ? 2 : 0; }
int canonFaceVAxis(uint face) { return canonFaceAxis(face) == 1 ? 2 : 1; }
bool canonFaceMirrored(uint face) { return face == 0u || face == 2u || face == 5u; }

// Q31: normal from the 3-bit face index.
vec3 canonFaceNormal(uint face) {
  vec3 n = vec3(0.0);
  n[canonFaceAxis(face)] = ((face & 1u) == 0u) ? 1.0 : -1.0;
  return n;
}

uint canonGeomCorner(uint face, uint aoDiagFlip, uint vertexIdx) {
  uint e = (vertexIdx + aoDiagFlip) & 3u;
  return canonFaceMirrored(face) ? ((4u - e) & 3u) : e;
}

struct CanonCorner { ivec3 pos; uint g; uint ao; uvec2 uv; };

// Integer decode of pulled vertex `vertexIdx` (0..3) in LOD-grid units (== pullCorner).
CanonCorner canonPullCorner(uvec2 raw, uint vertexIdx) {
  CanonQuad q = canonUnpackQuad(raw);
  CanonCorner c;
  c.g = canonGeomCorner(q.face, q.aoDiagFlip, vertexIdx);
  uint cu = (c.g == 1u || c.g == 2u) ? 1u : 0u;
  uint cv = (c.g >= 2u) ? 1u : 0u;
  c.pos = ivec3(q.o);
  c.pos[canonFaceUAxis(q.face)] += int(cu * q.w);
  c.pos[canonFaceVAxis(q.face)] += int(cv * q.h);
  c.ao = (q.cornerAo >> (c.g * 2u)) & 3u;
  uint ut = cu * q.w, vt = cv * q.h;
  if (q.uvRot == 1u) c.uv = uvec2(q.h - vt, ut);
  else if (q.uvRot == 2u) c.uv = uvec2(q.w - ut, q.h - vt);
  else if (q.uvRot == 3u) c.uv = uvec2(vt, q.w - ut);
  else c.uv = uvec2(ut, vt);
  return c;
}

// LOD-grid units -> sub-chunk blocks: cellSize = 1, 2, 4 for L0/L1/L2; micro quads are 1/4.
float canonUnitScale(uint coordScale, float lodCellSize) {
  return coordScale == kCanonScaleMicroQtr ? 0.25 : lodCellSize;
}

#endif  // LL_CANONICAL_QUAD_DECODE_GLSL
