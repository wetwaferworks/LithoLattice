// IUnifiedMesher refactor step 4 (UML_UNIFIED_MESHER_2026-10-30.md): meshSubChunkPass as a
// pipeline of stages that run the existing code in the same order. Moved verbatim, no output
// change (Gate G anchors + LL_CANON_VERIFY at every sub-step). Private to the render library.
#pragma once
#include "litholattice/render/mesher.h"

namespace litholattice {

class cMeshPipeline {
 public:
  /// One meshing job: the snapshot, its context, the legacy stream being built and the
  /// canonical mesh (null = legacy-only pass).
  cMeshPipeline(const MeshInput& in, const MeshContext& ctx, ChunkMesh& out, CanonicalMesh* canonOut)
      : in_(in), ctx_(ctx), out_(out), canonOut_(canonOut) {}

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
