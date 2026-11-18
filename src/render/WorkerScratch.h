// Private to the mesher: per-worker meshing scratch (IUnifiedMesher refactor step 2).
// Every former function-static / namespace thread_local of mesher.cpp lives here, one
// instance per thread (forThisThread), so the later MeshPipeline stages can take it by
// reference and stay stateless. Types, sizes and lazy-allocation points are unchanged.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "GreedyGrid.h"
#include "litholattice/render/unified_mesher.h"
#include "litholattice/world/block.h"
#include "litholattice/world/mesh_input.h"

namespace litholattice {

class cWorkerScratch {
 public:
  static cWorkerScratch& forThisThread();

  // RND-H1 per-job BlockDef table (jobGen/jobSlot sized on first use, as before).
  std::vector<uint32_t> jobGen = std::vector<uint32_t>(size_t(1) << 16, 0u);
  std::vector<uint16_t> jobSlot = std::vector<uint16_t>(size_t(1) << 16, uint16_t(0));
  std::vector<BlockDef> jobDefs;
  uint32_t gen = 0;
  uint8_t opaque[kMeshPadVolume];
  std::unique_ptr<GreedyGrid> greedyGrid;
  std::vector<uint32_t> qSlot, fSlot;  // M11: wide palette slots (split input)
};

}  // namespace litholattice
