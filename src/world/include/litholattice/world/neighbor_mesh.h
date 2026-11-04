// Neighbor-border meshing (Thread Handshake P8, 2026-10-29; Archie's rulings Q1-Q10).
//
//  - MeshEdge: what the 1-voxel apron holds where a neighbor column is not
//    resident. Clamp = the sub-chunk's own edge cell (Archie Q3: clamp-to-edge,
//    NOT air), so an unloaded border never shows a wall of side faces; the
//    column is re-meshed when the neighbor arrives (BorderGen bump).
//  - kMoore8: the 8 Moore neighbors (4 edge + 4 corner, Archie Q4). Bit i of
//    a neighbor mask maps 1:1 to kMoore8[i].
//  - cWorkerScratch / SubChunk16 / CowColumn / cColumnTable: Archie's section-7 sketch
//    (render-pipeline rulings): thread-local 18^3 scratch with 1,296-byte occupancy rows
//    and face masks; immutable CoW sub-chunks; lock-free column table pinned by commit_epoch.
//  - cMeshHashLedger: render-only MeshHash (Archie Q7). Kept out of the world
//    and anchor hashes; read only at quiescence (every resident column meshed
//    against its current BorderGen, nothing in flight).
//
// Memory layout follows Archie's sketch (ARCHIE_RULINGS_RENDER_PIPELINE_PACKING section 7).
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "litholattice/world/mesh_input.h"

namespace litholattice {

constexpr int kMoore8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};

/// Neighbor bit for an offset (dx, dz) in {-1,0,1}^2 \ {0,0}; -1 for (0, 0).
constexpr int moore8Bit(int dx, int dz) {
  for (int i = 0; i < 8; ++i)
    if (kMoore8[i][0] == dx && kMoore8[i][1] == dz) return i;
  return -1;
}

/// Thread-local worker scratch (Archie's render-pipeline sketch, section 7: WorkerScratch).
/// occRows[y][z] bit x = non-air over the 18^3 apron (1,296 bytes, Archie Q8);
/// faceMask[dir][y][z] = exposed faces of the 16^3 core (+X,-X,+Y,-Y,+Z,-Z);
/// matApron / lightApron = flattened 18^3 aprons for AO and greedy-run compares
/// (lightApron stays zero until the lighting pre-bake lands).
struct alignas(64) cWorkerScratch {
  uint32_t occRows[kMeshPad][kMeshPad];
  uint16_t faceMask[6][kChunkSize][kChunkSize];
  uint16_t matApron[kMeshPadVolume];
  uint8_t lightApron[kMeshPadVolume];
  bool solid(int x, int y, int z) const { return (occRows[y][z] >> x) & 1u; }
  void buildFaceVisibilityMasks() noexcept {
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z) {
        const uint32_t r = occRows[y + 1][z + 1];
        const uint32_t core = (r >> 1) & 0xFFFFu;
        faceMask[0][y][z] = uint16_t(core & ~((r >> 2) & 0xFFFFu));
        faceMask[1][y][z] = uint16_t(core & ~(r & 0xFFFFu));
        faceMask[2][y][z] = uint16_t(core & ~((occRows[y + 2][z + 1] >> 1) & 0xFFFFu));
        faceMask[3][y][z] = uint16_t(core & ~((occRows[y][z + 1] >> 1) & 0xFFFFu));
        faceMask[4][y][z] = uint16_t(core & ~((occRows[y + 1][z + 2] >> 1) & 0xFFFFu));
        faceMask[5][y][z] = uint16_t(core & ~((occRows[y + 1][z] >> 1) & 0xFFFFu));
      }
  }
};
static_assert(sizeof(cWorkerScratch::occRows) == 1296, "Archie Q8: 1,296-byte bit-packed occupancy rows");

/// Fill the scratch from an 18^3 MeshInput apron (occupancy, materials, face masks).
inline void buildWorkerScratch(const MeshInput& in, cWorkerScratch& out) {
  for (int y = 0; y < kMeshPad; ++y)
    for (int z = 0; z < kMeshPad; ++z) {
      uint32_t row = 0;
      for (int x = 0; x < kMeshPad; ++x) {
        const int i = meshPadIndex(x, y, z);
        if (in.id[i] != kAir) row |= 1u << x;
        out.matApron[i] = in.mat[i];
        out.lightApron[i] = 0;
      }
      out.occRows[y][z] = row;
    }
  out.buildFaceVisibilityMasks();
}

/// The calling worker's scratch (one per thread, never shared; heap-backed, not on the stack).
inline cWorkerScratch& workerScratchForThisThread() {
  thread_local std::unique_ptr<cWorkerScratch> pad = std::make_unique<cWorkerScratch>();
  return *pad;
}

/// Immutable CoW 16^3 sub-chunk (Archie sketch: SubChunk16). Published once, never mutated.
struct SubChunk16 {
  uint64_t commitEpoch = 0;
  uint16_t paletteSize = 0;
  uint8_t uniformBlockFlag = 0;  ///< 1 if all 4096 voxels are identical
  uint8_t caveGraphMask = 0;     ///< 6-bit directional connectivity (later pass)
  uint64_t occupancyBits[64]{};  ///< 4096 bits, 1 = non-air
  uint16_t voxelMatIdx[4096]{};  ///< BlockId per voxel (codebase currency)
  uint8_t packedLight[4096]{};   ///< high 4b sky, low 4b block
};

/// Immutable column version (Archie sketch: Column).
struct CowColumn {
  int32_t cx = 0, cz = 0;
  uint64_t commitEpoch = 0;
  std::shared_ptr<const SubChunk16> subChunks[kSubChunks];
};

/// Lock-free column table (Archie sketch: ColumnTable). Readers pin a column version
/// with an acquire load; GEN versions publish at once (release), sim edits are staged
/// and published together at commitEpoch() (the sim-tick barrier, Archie Q2).
/// Deviation: lookupPinned checks the coords (a hash collision returns null instead of
/// the wrong column); a colliding publish overwrites the slot (16384 slots ~ radius 63).
class cColumnTable {
 public:
  using Ptr = std::shared_ptr<const CowColumn>;
  Ptr lookupPinned(int32_t cx, int32_t cz) const noexcept {
    Ptr p = slots_[slotOf(cx, cz)].load(std::memory_order_acquire);
    return p && p->cx == cx && p->cz == cz ? p : nullptr;
  }
  void publishGen(Ptr col) noexcept {
    const size_t s = slotOf(col->cx, col->cz);
    slots_[s].store(std::move(col), std::memory_order_release);
  }
  void stageEdit(Ptr col) {
    std::lock_guard l(stageMu_);
    staged_.push_back(std::move(col));
  }
  uint64_t commitEpoch() {
    std::vector<Ptr> batch;
    { std::lock_guard l(stageMu_); batch.swap(staged_); }
    for (auto& c : batch) publishGen(std::move(c));
    return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }
  uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }

  static constexpr size_t kTableSlots = 16384;

 private:
  static size_t slotOf(int32_t x, int32_t z) noexcept {
    return size_t((uint64_t(uint32_t(x)) * 0x9E3779B97F4A7C15ULL) ^ (uint64_t(uint32_t(z)) * 0xBF58476D1CE4E5B9ULL)) &
           (kTableSlots - 1);
  }
  std::unique_ptr<std::atomic<Ptr>[]> slots_ = std::make_unique<std::atomic<Ptr>[]>(kTableSlots);
  std::mutex stageMu_;
  std::vector<Ptr> staged_;
  std::atomic<uint64_t> epoch_{0};
};
/// FNV-1a 64 over raw bytes (render-only MeshHash input).
inline uint64_t meshBytesHash(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

/// Render-only MeshHash: latest landed mesh hash per sub-chunk, folded in key
/// order (independent of worker count and landing order).
class cMeshHashLedger {
 public:
  void note(int64_t key, uint64_t h) { subs_[key] = h; }
  void erase(int64_t key) { subs_.erase(key); }
  void clear() { subs_.clear(); meshedBorder_.clear(); }
  size_t size() const { return subs_.size(); }
  uint64_t combined() const {
    uint64_t h = 1469598103934665603ull;
    for (const auto& [k, v] : subs_) { h = meshBytesHash(&k, sizeof k, h); h = meshBytesHash(&v, sizeof v, h); }
    return h;
  }
  /// Column `col` was snapshotted for meshing against BorderGen `g`.
  void noteMeshedBorder(uint64_t col, uint16_t g) { meshedBorder_[col] = g; }
  void forgetColumn(uint64_t col) { meshedBorder_.erase(col); }
  /// Quiescent when nothing is in flight and every tracked column was meshed
  /// against its current BorderGen (MeshGen == BorderGen, Archie Q7).
  template <class BorderGenOf>
  bool quiescent(size_t inFlight, BorderGenOf current) const {
    if (inFlight) return false;
    for (const auto& [col, g] : meshedBorder_)
      if (current(col) != g) return false;
    return true;
  }

 private:
  std::map<int64_t, uint64_t> subs_;
  std::unordered_map<uint64_t, uint16_t> meshedBorder_;
};

}  // namespace litholattice