// Private to the mesher: the greedy-merge parking grid (refactor step 2: split out of
// mesher.cpp so cWorkerScratch can own it). Layout unchanged.
#pragma once
#include <cstdint>

namespace litholattice {

struct GreedyGrid {
  uint64_t key[6][16][16][16] = {};  // [face][slice][iv][iu]; 0 = empty
  uint16_t sliceUsed[6] = {};        // bit s: slice s holds at least one parked face
};

}  // namespace litholattice
