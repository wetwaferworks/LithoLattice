// IUnifiedMesher refactor step 4 (UML_UNIFIED_MESHER_2026-10-30.md): meshSubChunkPass as a
// pipeline of stages that run the existing code in the same order. Moved verbatim, no output
// change (Gate G anchors + LL_CANON_VERIFY at every sub-step). Private to the render library.
#pragma once
#include <cstddef>
#include <memory>

#include "litholattice/render/FallbackReasonStats.h"
#include "litholattice/render/mesher.h"

namespace litholattice {

struct GreedyGrid;
class cWorkerScratch;
class IBlockAppearance;

class cMeshPipeline {
 public:
  /// One meshing job: the snapshot, its context, the legacy stream being built and the
  /// canonical mesh (null = legacy-only pass).
  cMeshPipeline(const MeshInput& in, const MeshContext& ctx, ChunkMesh& out, CanonicalMesh* canonOut)
      : in_(in), ctx_(ctx), out_(out), canonOut_(canonOut) {}

  /// Stage: flush the greedy grid into the canonical mesh (M2) or, legacy greedy, into the stream.
  void flush(std::unique_ptr<GreedyGrid>& grid, bool greedy);
  /// Stage: Packed12 fallback of the per-cell stream (M7) and the M11 palette split.
  /// legacyVerts = stream size before the flush.
  void packedFallback(size_t legacyVerts, FallbackReasonStats::Local& fbr, cWorkerScratch& ws);
  /// Stage: M6 L1/L2 LODs from UnifiedMesher over the per-job material table, merged into
  /// the canonical mesh. look / fancyLeaves are the pass's values for this job.
  void canonLods(cWorkerScratch& ws, const IBlockAppearance& look, bool fancyLeaves);
  /// Stage: trim an oversized legacy stream (cannot happen for 16^3 cubes; guards custom shapes).
  void capGuard();
  /// Stage: per-mesh draw params (wafer ground pulse, Trace-mix pulse colour, ground traces).
  void perMeshParams();

 private:
  const MeshInput& in_;
  const MeshContext& ctx_;
  ChunkMesh& out_;
  CanonicalMesh* canonOut_;
};

}  // namespace litholattice
