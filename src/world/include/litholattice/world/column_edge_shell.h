#pragma once
// Q12 (Gate 1, 2026-10-31): far-column demote. A demoted column frees its SubChunks and
// keeps only this 1-cell edge shell: the ring of cells with lx or lz on 0/15 (60 per layer,
// 16 layers per sub-chunk), which is all a resident neighbour's mesh apron ever reads.
// Immutable once built; shared by the column and the CoW publisher (no locks).
// LL_FAR_DEMOTE=1 (default off). Known limit: light stops at the ring (logged once).
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "litholattice/world/chunk.h"

namespace litholattice {


class cColumnEdgeShell {
 public:
  cColumnEdgeShell() = default;
  static constexpr int kRingPerLayer = 60;
  static constexpr int kRingCells = kRingPerLayer * kChunkSize;  // per sub-chunk

  /// Ring index of (lx, lz) in 0..59, or -1 for an interior cell.
  static int ringIndex(int lx, int lz);
  static bool onRing(int lx, int lz) { return ringIndex(lx, lz) >= 0; }

  /// One pass over the column's sub-chunks.
  static std::shared_ptr<const cColumnEdgeShell> build(const ChunkColumn& c);

  bool hasSub(int sy) const { return sy >= 0 && sy < kSubChunks && subs_[size_t(sy)] != nullptr; }
  /// Cell reads in column-local coords; interior and absent sub-chunks read air / 0.
  StableId at(int sy, int lx, int y, int lz) const;
  CellState stateAt(int sy, int lx, int y, int lz) const;
  /// Rebuilds sub-chunk sy as a SubChunk: ring cells restored, interior air (CoW publish).
  void expand(int sy, SubChunk& out) const;
  size_t bytes() const { return bytes_; }

  // Process-wide counters (relaxed atomics).
  static void noteRefusedWrite() { refused_.fetch_add(1, std::memory_order_relaxed); }
  static uint64_t refusedWrites() { return refused_.load(std::memory_order_relaxed); }

 private:
  struct Layer {
    std::vector<StableId> palette;              // distinct ring ids of this sub-chunk
    std::array<uint16_t, kRingCells> idx{};     // palette index per ring cell
    std::array<CellState, kRingCells> state{};  // 6+6 light and cell bits ride here
    std::array<uint8_t, kRingCells> mat{};      // material palette byte
  };
  static int slot(int lx, int y, int lz) { return (y & 15) * kRingPerLayer + ringIndex(lx, lz); }
  std::array<std::unique_ptr<Layer>, kSubChunks> subs_{};
  size_t bytes_ = 0;
  static inline std::atomic<uint64_t> refused_{0};
};

}  // namespace litholattice
