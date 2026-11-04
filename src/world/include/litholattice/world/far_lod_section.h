#pragma once
// Voxy idea #1 (clean-room, 2026-10-31; written from JD's description only): far-column LOD
// built in one pass at demotion time. Each sub-chunk mips 16^3 -> 8^3 -> 4^3 -> 2^3 -> 1^3;
// a parent cell takes the most-opaque child (opaque > other non-air > air; ties -> the most
// frequent id), light takes the max, and an 8-bit child mask records non-air children.
// Cells are addressed by a 64-bit key lvl(4) | y(12) | z(24) | x(24) in level-cell units, so
// a 32^3 world section is a 2x2x2 group of level-1 parents. Immutable after build.
// LL_FAR_LOD=1 (default off; needs LL_FAR_DEMOTE=1). Not drawn yet: storage + lookup only.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "litholattice/world/chunk.h"

namespace litholattice {

class cBlockReg;

class cFarLodSection {
 public:
  static constexpr int kLevels = 4;  // 1..4: 8^3, 4^3, 2^3, 1^3
  struct Cell {
    uint16_t id = 0;        // index into palette(); 0 == air
    uint8_t childMask = 0;  // bit c: child c (x | y<<1 | z<<2) is non-air
    uint8_t light = 0;      // max child light (0 until Q2 real light feeds level 0)
  };

  static uint64_t key(int lvl, int x, int y, int z) {
    return (uint64_t(lvl & 15) << 60) | (uint64_t(uint32_t(y) & 0xfff) << 48) | (uint64_t(uint32_t(z) & 0xffffff) << 24) |
           uint64_t(uint32_t(x) & 0xffffff);
  }
  static std::unique_ptr<cFarLodSection> build(const ChunkColumn& c, const cBlockReg* reg);

  /// Cell for a key inside this column, or null (other column, absent sub, bad level).
  const Cell* find(uint64_t k) const;
  StableId stableOf(const Cell& c) const { return palette_[c.id]; }
  size_t bytes() const;

 private:
  static constexpr int dim(int lvl) { return 16 >> lvl; }
  static constexpr int offset(int lvl) {  // cells before level lvl inside one sub
    int o = 0;
    for (int l = 1; l < lvl; ++l) o += dim(l) * dim(l) * dim(l);
    return o;
  }
  static constexpr int kCellsPerSub = 512 + 64 + 8 + 1;
  int cx_ = 0, cz_ = 0;
  std::vector<StableId> palette_{kAirStableId};
  std::array<std::unique_ptr<std::array<Cell, kCellsPerSub>>, kSubChunks> subs_{};
};

}  // namespace litholattice
