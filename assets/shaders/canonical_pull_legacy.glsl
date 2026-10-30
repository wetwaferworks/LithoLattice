// Render pipeline M12 (GL TBO uploader, Archie ruling 5: 1/16 units, crack-free integer unpack).
// Pulls one legacy PackedVertex attribute set (aPos / aInfo / aShade / aLayerHi, see chunk.vert)
// from a canonical "pull texel" (uvec4). Mirrors canonPullReference() in mesher.cpp line for line,
// which itself is checked against decodeCanonicalMesh (emitFace + emitGreedyQuad + canonKeyOf):
//   quad texel     = (quad lo, quad hi, layer | value << 16 | mode << 24, 0)
//   fallback texel = (Packed12 w0, w1, w2, layerId | rgb565Tint << 16)   (one texel per vertex)
// corner = output vertex 0..3 of the quad (uniform AO: emitFace's diagonal start is 0).
// All math is integer; positions stay in 1/16 block units (x lodScale), so neighbouring
// quads share bit-identical corners (no cracks).
struct CanonLegacyVertex { uvec3 pos; uvec4 info; uvec2 shade; uint layerHi; };

int canonTexU(uint f) { return f < 2u ? 2 : 0; }
int canonTexV(uint f) { return f < 2u ? 1 : (f < 4u ? 2 : 1); }
// mesher.cpp kFaces: geometric u axis / v axis per face.
int canonGeoU(uint f) { return f == 0u ? 1 : f == 1u ? 2 : f == 2u ? 2 : f == 3u ? 0 : f == 4u ? 0 : 1; }

uvec2 canonFaceUV(uint f, ivec3 p) {
  int u, v;
  if (f == 0u) { u = 16 - p.z; v = 16 - p.y; }
  else if (f == 1u) { u = p.z; v = 16 - p.y; }
  else if (f == 4u) { u = p.x; v = 16 - p.y; }
  else if (f == 5u) { u = 16 - p.x; v = 16 - p.y; }
  else if (f == 2u) { u = p.x; v = p.z; }
  else { u = p.x; v = 16 - p.z; }
  return uvec2(u, v);
}

CanonLegacyVertex canonPullFallback(uvec4 t) {
  CanonLegacyVertex o;
  uint fx = t.x & 0xFFFu, fy = (t.x >> 12u) & 0xFFFu, fz = ((t.x >> 24u) & 0xFFu) | ((t.y & 0xFu) << 8u);
  uint u = (t.y >> 4u) & 0xFFFu, v = (t.y >> 16u) & 0xFFFu, nid = (t.y >> 28u) & 0xFu;
  uint ao = (t.z >> 10u) & 3u, wind = (t.z >> 12u) & 0x3Fu;
  uint layer = t.w & 0xFFFFu, tint = t.w >> 16u;
  o.pos = uvec3(fx, fy, fz);
  o.info = uvec4((u >> 4u) & 0xFFu, (v >> 4u) & 0xFFu, ((nid & 7u) | (ao << 3u) | (wind << 5u) | ((nid >> 3u) << 7u)) & 0xFFu,
                 layer & 0xFFu);
  o.shade = uvec2(tint & 0xFFu, (tint >> 8u) & 0xFFu);
  o.layerHi = (layer >> 8u) & 0xFFu;
  return o;
}

CanonLegacyVertex canonPullQuadLegacy(uvec4 t, uint corner, uint lod) {
  CanonLegacyVertex o;
  uint lo = t.x, hi = t.y;
  ivec3 c = ivec3(int(lo & 0x3Fu), int((lo >> 6u) & 0x3Fu), int((lo >> 12u) & 0x3Fu));
  int w = int((lo >> 18u) & 0xFu) + 1, h = int((lo >> 22u) & 0xFu) + 1;
  uint f = (lo >> 26u) & 7u;
  uint ao = (hi >> 10u) & 3u;
  int ua = canonTexU(f), va = canonTexV(f), na = 3 - ua - va;
  if ((f & 1u) == 0u) c[na] -= 1;
  ivec3 mn, mx;
  mn[na] = c[na] * 16; mx[na] = mn[na] + 16;
  mn[ua] = c[ua] * 16; mx[ua] = (c[ua] + w) * 16;
  mn[va] = c[va] * 16; mx[va] = (c[va] + h) * 16;
  int gu = canonGeoU(f);
  bool cu = corner == 1u || corner == 2u, cv = corner >= 2u;
  bool posN = (f & 1u) == 0u;
  ivec3 pos;
  for (int a = 0; a < 3; ++a) {
    if (a == na) pos[a] = posN ? mx[a] : mn[a];
    else if (a == gu) pos[a] = cu ? mx[a] : mn[a];
    else pos[a] = cv ? mx[a] : mn[a];
  }
  ivec3 local;
  for (int a = 0; a < 3; ++a) local[a] = pos[a] > mn[a] ? 16 : 0;
  pos = max(pos, ivec3(0));
  uvec2 uv = canonFaceUV(f, local);
  uint s = 1u << lod;
  o.pos = (uvec3(pos) * s) & 0xFFFFu;
  uint layer = t.z & 0xFFFFu;
  o.info = uvec4((uv.x * uint(w)) & 0xFFu, (uv.y * uint(h)) & 0xFFu, (f | (ao << 3u)) & 0xFFu, layer & 0xFFu);
  o.shade = uvec2((t.z >> 16u) & 0xFFu, (t.z >> 24u) & 0xFFu);
  o.layerHi = layer >> 8u;
  return o;
}