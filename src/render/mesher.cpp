// Cortana 2026-10-15 1020: RND-H1 per-job def table
#include <algorithm>
#include <atomic>
#include <cstdlib>  // Cortana 2026-10-15 1705: LL_MESH_GREEDY
#include <cstring>
#include <memory>
#include <vector>
#include "litholattice/core/log.h"  // Cortana 2026-10-16 2335 F55-2
#include "litholattice/render/mesher.h"
#include "litholattice/render/trace_ramps.h"  // Cortana 2026-10-15 1600: Trace-mix pulse colour
#include "litholattice/render/block_materials.h"
#include "litholattice/render/block_appearance.h"
#include "litholattice/core/log.h"

#include <cstdlib>
static int llP2PerfOff() { static const int v = [] { const char* e = std::getenv("LL_P2_PERF_OFF"); return e ? int(std::strtol(e, nullptr, 0)) : 0; }(); return v; }  // Cortana 2026-10-16 1520: A/B perf switch (render/display only)

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
constexpr int kFaceDir[6] = {1, 3, -1, -1, 2, 0};  // world face -> horizontal dir (N0 E1 S2 W3)

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

struct GreedyGrid {
  uint64_t key[6][16][16][16] = {};  // [face][slice][iv][iu]; 0 = empty
  uint16_t sliceUsed[6] = {};        // bit s: slice s holds at least one parked face
};

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

void flushGreedy(GreedyGrid& g, std::vector<PackedVertex>& out) {
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

// -1: no override; 0/1: LL_MESH_GREEDY forces greedy off/on (A/B and captures).
int greedyEnvOverride() {
  static const int v = [] {
    const char* e = std::getenv("LL_MESH_GREEDY");
    return e && *e ? (e[0] == '0' ? 0 : 1) : -1;
  }();
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
ChunkMesh meshSubChunkPass(const MeshInput& in, const MeshContext& ctx) {
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
  static thread_local std::vector<uint32_t> jobGen(size_t(1) << 16, 0u);
  static thread_local std::vector<uint16_t> jobSlot(size_t(1) << 16, uint16_t(0));
  static thread_local std::vector<BlockDef> jobDefs;
  static thread_local uint32_t gen = 0;
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
  static thread_local uint8_t opaque[kMeshPadVolume];
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
  // Cortana 2026-10-15 1705: greedy meshing (see GreedyGrid).
  const int greedyEnv = greedyEnvOverride();
  const bool greedy = greedyEnv < 0 ? ctx.greedy : greedyEnv == 1;
  static thread_local std::unique_ptr<GreedyGrid> greedyGrid;
  if (greedy) {
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
            const int turns = (af == 1 || af == 2) ? look.boxModelTurns(def, st) : 0;  // 2026-10-24: floor / ceiling facing
            for (BoxModel::Box box : bm->orientedBoxes(tilt ? 1 : af, turns)) {
              if (tilt) {
                box.min[1] += 3; box.max[1] += 3;
                box.min[0] += 7 * wdx; box.max[0] += 7 * wdx;
                box.min[2] += 7 * wdz; box.max[2] += 7 * wdz;
              }
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
            if (tilt && false) {  // 2026-10-24 (JD 6:12 PM): the shear twisted the LED into a ribbon; upright model, raised against the wall
              for (size_t i = tiltFrom; i < out.verts.size(); ++i) {
                PackedVertex& v = out.verts[i];
                const int ly = int(v.y) - y * 16;  // 3 (base) .. 13 (top)
                const int off = (std::clamp(ly - 3, 0, 10) * 4 + 5) / 10;
                v.x = uint16_t(int(v.x) - off * wdx);
                v.z = uint16_t(int(v.z) - off * wdz);
              }
            }
            continue;
          }
        }
        if (def.shape == Shape::Cube && look.shapedRender(def) &&
            look.meshShaped(CustomBlock{in, reg, mats, b, st, x, y, z}, writer))
          continue;  // 2026-10-24: render-only slab model (collision stays a full cell)
        if (def.shape == Shape::Custom) {  // the game builds it (e.g. connected strips)
          look.meshCustom(CustomBlock{in, reg, mats, b, st, x, y, z}, writer);
          continue;
        }
        if (def.shape == Shape::Cross) {  // sprite: the active-state top face, alpha-tested
          emitCross(out.verts, mn, lookOf(mats.face(b, FaceSlot::Top, look.active(def, st)), kShadeCutout));
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
            const int d = kFaceDir[f];
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
            const int ch = def.channel ? def.channel : nb.channel;
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
          if (greedy && !slab && greedyEligible(lk, ao)) {  // Cortana 2026-10-15 1705: park for merging
            const int blk[3] = {x, y, z};
            park(*greedyGrid, f, blk, lk, ao[0]);
            continue;
          }
          emitFace(out.verts, f, mn, mx, mn, slab && f == 2 ? full : ao, lk);
          if (leaf && fancyLeaves) {  // two-sided: the same quad again, reversed winding (no cull-state change)
            emitFace(out.verts, f, mn, mx, mn, slab && f == 2 ? full : ao, lk);
            std::swap(out.verts[out.verts.size() - 3], out.verts[out.verts.size() - 1]);
          }
        }
        look.meshExtra(cellC, writer);
      }
  if (greedy) flushGreedy(*greedyGrid, out.verts);  // Cortana 2026-10-15 1705
  if (out.verts.size() > size_t(kMaxQuadsPerMesh) * 4) {  // cannot happen for 16^3 cubes; guards custom shapes
    logError("mesher: sub-chunk (%d,%d,%d) has %zu quads, trimmed to %u", in.cx, in.sy, in.cz, out.verts.size() / 4,
             kMaxQuadsPerMesh);
    out.verts.resize(size_t(kMaxQuadsPerMesh) * 4);
  }
  // Wafer ground pulse: the column's weight-blended params ride the mesh, so the
  // backends can push them per draw call (biome weights are worldgen-static, so
  // baking at mesh time costs nothing per frame). Null provider = zero params.
  if (ctx.pulseParams) ctx.pulseParams(in.cx, in.cz, out.pulseP0, out.pulseP1);
  // Cortana 2026-10-15 1600 (MIXED; credit: JD idea): Trace-mix pulse colour.
  // Count the red (StripTint0) and blue (StripTint1) Trace quads in this mesh
  // and blend the pulse colour by their ratio: all-blue pulses blue, all-red
  // pulses red, a mix pulses at the weighted mean. The biome colour's peak
  // brightness is kept, so the tuned amplitude still reads the same. This runs
  // once per (re)mesh, so it is recomputed only when the chunk's blocks
  // (and so its Trace) change. A mesh with no Trace keeps the biome colour.
  // Render-only: no sim state is read or written, so the hash can't move.
  {
    uint32_t nRed = 0, nBlue = 0;
    for (size_t i = 0; i < out.verts.size(); i += 4) {
      nRed += out.verts[i].mode == kShadeStripTint0;
      nBlue += out.verts[i].mode == kShadeStripTint1;
    }
    const float rgb[3] = {out.pulseP1[0], out.pulseP1[1], out.pulseP1[2]};
    traceMixPulseColor(nRed, nBlue, rgb, out.pulseP1);
  }
  // Wafer ground traces (JD 2026-10-11): same pattern as the pulse - the
  // column's weight-blended trace params ride the mesh for per-draw push.
  // Null provider = zero params (no traces).
  if (ctx.traceParams) ctx.traceParams(in.cx, in.cz, out.traceP0, out.traceP1);
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
    const char* e = std::getenv("LL_LEAVES");
    v = (e && std::string_view(e) == "fast") ? 0 : 1;
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
    const char* e = std::getenv("LL_FOLIAGE_DENSITY");
    v = (e && std::string_view(e) == "1") ? 1 : 0;
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
#ifdef NDEBUG
static std::atomic<bool> g_meshVerify{false};
#else
static std::atomic<bool> g_meshVerify{true};
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
static ChunkMesh meshSubChunkStable(const MeshInput& in, const MeshContext& ctx) {
  cContentReg::StableDefScope stable(ctx.blocks.content());
  return meshSubChunkPass(in, ctx);
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
  ChunkMesh out = meshSubChunkStable(in, ctx);
  const uint64_t epoch1 = ctx.blocks.content().definitionEpoch();
  if (epoch1 != epoch0) {
    out = meshSubChunkStable(in, ctx);
    const uint64_t epoch2 = ctx.blocks.content().definitionEpoch();
    if (epoch2 != epoch1)
      logError("mesher: block definitions changed twice during one mesh of (%d,%d,%d) "
               "(epochs %llu -> %llu -> %llu); refreshStaged must run between ticks only",
               in.cx, in.sy, in.cz, (unsigned long long)epoch0, (unsigned long long)epoch1,
               (unsigned long long)epoch2);
  }
  if (meshVerifyEnabled()) {
    const uint64_t e0 = ctx.blocks.content().definitionEpoch();
    const ChunkMesh live = meshSubChunkPass(in, ctx);  // locked live registry reads
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
