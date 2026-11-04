// Refactor step 4 (private to the render library): the mesher kernels that the cMeshPipeline
// stages call, as static members (JD: no loose free functions). The bodies are the existing
// functions, unchanged, defined in mesher.cpp next to the emit core they share
// (emitGreedyQuad / emitFace / Look / canonKeyOf). Moving that core into its own class is a
// separate, larger split.
#pragma once
#include <cstddef>
#include <vector>

#include "GreedyGrid.h"
#include "litholattice/render/mesher.h"

namespace litholattice {

class cMesherKernels {
 public:
  cMesherKernels() = delete;
  /// Legacy greedy flush: merge the parked slices into the vertex stream.
  static void flushGreedy(GreedyGrid& g, std::vector<PackedVertex>& out);
  /// M2 canonical flush: L0 face buckets plus the decoded vertex stream.
  static void flushCanonical(GreedyGrid& g, std::vector<PackedVertex>& out, CanonicalMesh& cm, bool merged);
  /// M7: the per-cell stream as Packed12 fallback vertices.
  static bool packFallback(const std::vector<PackedVertex>& v, size_t n, CanonicalMesh& cm);
  /// M6: merge UnifiedMesher L1/L2 into the L0 mesh (palette dedup + re-index).
  static bool mergeCanonLods(const CanonicalMesh& src, CanonicalMesh& dst);
  static constexpr int kFaceDir[6] = {1, 3, -1, -1, 2, 0};  ///< world face -> horizontal dir (N0 E1 S2 W3)
};

}  // namespace litholattice
