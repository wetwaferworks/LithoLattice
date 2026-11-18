#include "MeshPipeline.h"

#include <cstddef>

#include "MesherKernels.h"
#include "UnifiedMesherCounters.h"
#include "WorkerScratch.h"
#include "litholattice/core/log.h"
#include "litholattice/render/trace_ramps.h"

namespace litholattice {

void cMeshPipeline::flush(std::unique_ptr<GreedyGrid>& greedyGrid, bool greedy) {
  if (canonOut_) cMesherKernels::flushCanonical(*greedyGrid, out_.verts, *canonOut_, greedy);  // render pipeline M2
  else if (greedy) cMesherKernels::flushGreedy(*greedyGrid, out_.verts);  // Cortana 2026-10-15 1705
}

void cMeshPipeline::packedFallback(size_t legacyVerts, FallbackReasonStats::Local& fbr, cWorkerScratch& ws) {
  CanonicalMesh* canonOut = canonOut_;
  if (canonOut && canonOut->magic == canon::kMeshMagic) {  // render pipeline M7: Packed12 fallback
    if (cMesherKernels::packFallback(out_.verts, legacyVerts, *canonOut)) {
      cUnifiedMesherCounters::fbVerts.fetch_add(legacyVerts, std::memory_order_relaxed);
      FallbackReasonStats::instance().flush(fbr, legacyVerts / 4, canonOut->quads.size());
    }
    else *canonOut = CanonicalMesh{}, canonOut->magic = 0;  // not publishable (counted as unpublished)
    if (canonOut->magic == canon::kMeshMagic && canonOut->palette.size() > canon::kMaxPalette) {  // M11: split, no legacy
      if (splitCanonicalTwo(*canonOut, ws.qSlot, ws.fSlot)) cUnifiedMesherCounters::splits.fetch_add(1, std::memory_order_relaxed);
      else *canonOut = CanonicalMesh{}, canonOut->magic = 0;
    }
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
