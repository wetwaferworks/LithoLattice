// Refactor step 4 (private to the render library): mesher.cpp internals that the
// cMeshPipeline stages (MeshPipeline.cpp) call. These are the existing functions and
// counters, unchanged; only their linkage moved out of mesher.cpp's anonymous namespace.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "GreedyGrid.h"
#include "litholattice/render/mesher.h"

namespace litholattice {

void flushGreedy(GreedyGrid& g, std::vector<PackedVertex>& out);
void flushCanonical(GreedyGrid& g, std::vector<PackedVertex>& out, CanonicalMesh& cm, bool merged);
bool packFallback(const std::vector<PackedVertex>& v, size_t n, CanonicalMesh& cm);
bool mergeCanonLods(const CanonicalMesh& src, CanonicalMesh& dst);

constexpr int kFaceDir[6] = {1, 3, -1, -1, 2, 0};  // world face -> horizontal dir (N0 E1 S2 W3)

extern std::atomic<uint64_t> g_umFbVerts, g_umFbChecked, g_umFbMismatch;
extern std::atomic<uint64_t> g_umLodQuads, g_umLodFallback, g_umSkirtQuads, g_umSplits;

}  // namespace litholattice
