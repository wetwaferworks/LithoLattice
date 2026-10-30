#include "MeshPipeline.h"

#include <cstddef>

#include "MesherDetail.h"
#include "WorkerScratch.h"
#include "litholattice/core/log.h"
#include "litholattice/render/unified_mesher.h"
#include "litholattice/render/block_appearance.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/trace_ramps.h"

namespace litholattice {

void cMeshPipeline::flush(std::unique_ptr<GreedyGrid>& greedyGrid, bool greedy) {
  if (canonOut_) flushCanonical(*greedyGrid, out_.verts, *canonOut_, greedy);  // render pipeline M2
  else if (greedy) flushGreedy(*greedyGrid, out_.verts);  // Cortana 2026-10-15 1705
}

void cMeshPipeline::packedFallback(size_t legacyVerts, FallbackReasonStats::Local& fbr, cWorkerScratch& ws) {
  CanonicalMesh* canonOut = canonOut_;
  if (canonOut && canonOut->magic == canon::kMeshMagic) {  // render pipeline M7: Packed12 fallback
    if (packFallback(out_.verts, legacyVerts, *canonOut)) {
      g_umFbVerts.fetch_add(legacyVerts, std::memory_order_relaxed);
      FallbackReasonStats::instance().flush(fbr, legacyVerts / 4, canonOut->quads.size());
    }
    else *canonOut = CanonicalMesh{}, canonOut->magic = 0;  // not publishable (counted as unpublished)
    if (canonOut->magic == canon::kMeshMagic && canonOut->palette.size() > canon::kMaxPalette) {  // M11: split, no legacy
      if (splitCanonicalTwo(*canonOut, ws.qSlot, ws.fSlot)) g_umSplits.fetch_add(1, std::memory_order_relaxed);
      else *canonOut = CanonicalMesh{}, canonOut->magic = 0;
    }
  }
}

void cMeshPipeline::canonLods(cWorkerScratch& ws, const IBlockAppearance& look, bool fancyLeaves) {
  // Aliases so the stage body below is the pass's text, verbatim (refactor step 4c).
  const MeshInput& in = in_;
  CanonicalMesh* canonOut = canonOut_;
  const cBlockReg& reg = ctx_.blocks;
  const BlockMaterials& mats = ctx_.materials;
  std::vector<uint32_t>& jobGen = ws.jobGen;
  std::vector<uint16_t>& jobSlot = ws.jobSlot;
  std::vector<BlockDef>& jobDefs = ws.jobDefs;
  uint32_t& gen = ws.gen;
  uint8_t (&opaque)[kMeshPadVolume] = ws.opaque;
  const auto jobDef = [&](BlockId id) -> const BlockDef& {  // the pass's per-job def table (shared scratch)
    if (jobGen[id] != gen) {
      jobGen[id] = gen;
      jobSlot[id] = uint16_t(jobDefs.size());
      jobDefs.push_back(reg.def(id));
    }
    return jobDefs[jobSlot[id]];
  };
  if (canonOut && canonOut->magic == canon::kMeshMagic && canonLodsEnabled()) {  // render pipeline M6
    cWorkerScratch::Lod& wl = ws.lod();  // refactor step 2: was function-static thread_locals
    std::vector<uint32_t>& keyGen = wl.keyGen;
    std::vector<uint16_t>& keyLocal = wl.keyLocal;
    std::vector<UnifiedMaterial>& umats = wl.umats;
    uint16_t (&umId)[kMeshPadVolume] = wl.umId;
    std::unique_ptr<UnifiedMesher>& um = wl.um;
    CanonicalMesh& lodMesh = wl.lodMesh;
    if (!um) um = std::make_unique<UnifiedMesher>();
    umats.assign(1, UnifiedMaterial{});  // 0 = air
    umats[0].occludes = false;
    bool tableOk = true;
    for (int i = 0; i < kMeshPadVolume && tableOk; ++i) {
      umId[i] = 0;
      const BlockId b = in.id[i];
      if (b == kAir) continue;
      const BlockDef& d = jobDef(b);
      if (d.shape != Shape::Cube || look.shapedRender(d) || (d.flags & kAttachable)) continue;
      const CellState st = in.st[i];
      const bool active = look.active(d, st);
      const int facing = (d.flags & kDirectional) ? stFacing(st) : 0;
      const uint32_t key = uint32_t(b) | (uint32_t(active) << 16) | (uint32_t(facing & 3) << 17);
      if (keyGen[key] != gen) {
        if (umats.size() >= 65535) { tableOk = false; break; }
        keyGen[key] = gen;
        keyLocal[key] = uint16_t(umats.size());
        UnifiedMaterial um1;
        um1.occludes = opaque[i] != 0;
        um1.perFace = true;
        const bool leaf = (d.flags & kLeaves) != 0;
        for (int f = 0; f < 6; ++f) {
          FaceSlot slot;
          if (f == 2) slot = FaceSlot::Top;
          else if (f == 3) slot = FaceSlot::Bottom;
          else {
            const int dd = kFaceDir[f];
            slot = dd == facing ? FaceSlot::Front : dd == dirBack(facing) ? FaceSlot::Back : dd == dirRight(facing) ? FaceSlot::Right : FaceSlot::Left;
          }
          const FaceMaterial fm = mats.face(b, slot, leaf ? !fancyLeaves : active);
          canon::PaletteFields p;
          p.layerId = fm.layer;
          if (fm.animated()) { p.modeFlags |= uint8_t(canon::kModeAnimated); p.pulse = uint8_t(fm.animation & 15u); }
          if (leaf && fancyLeaves) p.modeFlags |= uint8_t(canon::kModeLeafWind | canon::kModeCutout);
          else if (d.flags & kTransparent) p.modeFlags |= uint8_t(canon::kModeGlassDither);
          um1.faces[size_t(f)] = p;
        }
        um1.palette = um1.faces[2];
        umats.push_back(um1);
      }
      umId[i] = keyLocal[key];
    }
    UnifiedMeshInput ui;
    ui.matId = umId; ui.materials = umats.data(); ui.materialCount = uint32_t(umats.size()); ui.firstLod = 1;
    if (tableOk && um->mesh(ui, lodMesh) == UnifiedMesher::Status::Ok &&
        (mergeCanonLods(lodMesh, *canonOut) || (canonOut->next && mergeCanonLods(lodMesh, *canonOut->next))))
    {
      g_umLodQuads.fetch_add(lodMesh.quads.size(), std::memory_order_relaxed);
      g_umSkirtQuads.fetch_add(uint64_t(lodMesh.lods[1].skirt.count) + lodMesh.lods[2].skirt.count, std::memory_order_relaxed);
    }
    else
      g_umLodFallback.fetch_add(1, std::memory_order_relaxed);
  }
}

void cMeshPipeline::capGuard() {
  if (out_.verts.size() > size_t(kMaxQuadsPerMesh) * 4) {  // cannot happen for 16^3 cubes; guards custom shapes
    logError("mesher: sub-chunk (%d,%d,%d) has %zu quads, trimmed to %u", in_.cx, in_.sy, in_.cz, out_.verts.size() / 4,
             kMaxQuadsPerMesh);
    out_.verts.resize(size_t(kMaxQuadsPerMesh) * 4);
  }
}

void cMeshPipeline::perMeshParams() {
  // Wafer ground pulse: the column's weight-blended params ride the mesh, so the
  // backends can push them per draw call (biome weights are worldgen-static, so
  // baking at mesh time costs nothing per frame). Null provider = zero params.
  if (ctx_.pulseParams) ctx_.pulseParams(in_.cx, in_.cz, out_.pulseP0, out_.pulseP1);
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
    for (size_t i = 0; i < out_.verts.size(); i += 4) {
      nRed += out_.verts[i].mode == kShadeStripTint0;
      nBlue += out_.verts[i].mode == kShadeStripTint1;
    }
    const float rgb[3] = {out_.pulseP1[0], out_.pulseP1[1], out_.pulseP1[2]};
    traceMixPulseColor(nRed, nBlue, rgb, out_.pulseP1);
  }
  // Wafer ground traces (JD 2026-10-11): same pattern as the pulse - the
  // column's weight-blended trace params ride the mesh for per-draw push.
  // Null provider = zero params (no traces).
  if (ctx_.traceParams) ctx_.traceParams(in_.cx, in_.cz, out_.traceP0, out_.traceP1);
}

}  // namespace litholattice
