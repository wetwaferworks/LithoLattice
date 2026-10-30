// Render pipeline M5 (Archie migration step 1): pinned copy-on-write column snapshots.
//
// The live ChunkMap stays mutable in place (the signal sim caches raw CellState pointers
// into SubChunk::state, so the live storage cannot itself be CoW without reworking those
// fast paths). Instead the map's owner thread PUBLISHES immutable per-sub-chunk copies into
// a ColumnTable when a sub-chunk is dirty (CoW at column granularity: a publish swaps one
// immutable ColumnSnap for a new one sharing every untouched sub). A mesh request PINS the
// 3x3 neighbor column snaps (nine shared_ptr copies) instead of copying the 18^3 slab on the
// owner thread; the mesh worker gathers the slab itself (gatherPinned) from the pins.
// Retained snapshots are bounded (LL_COW_BUDGET_MB, default 128); an evicted column is
// republished on its next pin. LL_LEGACY_SNAPSHOT=1 restores the owner-thread slab copy.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "litholattice/world/chunk.h"

namespace litholattice {

class ChunkMap;
enum class MeshEdge : uint8_t;

/// One immutable published sub-chunk: a copy of the paletted storage plus its BlockId
/// lookup resolved on the owner thread at publish time (registry mapping of that epoch).
struct SubSnap {
  SubChunk data;
  std::vector<BlockId> lut;   // lut[rawIndex(i)] -> BlockId (paletted widths)
  std::vector<BlockId> full;  // per-voxel BlockIds (64-bit width only)
  size_t bytes() const;
};

struct ColumnSnap {
  uint64_t instanceId = 0;    // ChunkColumn::instanceId this was published from
  uint64_t commitEpoch = 0;   // table publish counter at the last publish
  MaterialPalette matPalette;  // interned handle (immutable)
  std::vector<std::pair<uint32_t, std::string>> cellText;  // ordered copy (std::map order)
  std::array<std::shared_ptr<const SubSnap>, kSubChunks> subs;  // nullptr == all air
};

/// What a mesh worker needs to build one MeshInput: the 3x3 column neighborhood (index
/// (dz+1)*3+(dx+1)), present[] = the live column existed at pin time.
struct MeshPin {
  int cx = 0, sy = 0, cz = 0;
  BlockId belowWorld = 0;
  MeshEdge edge{};
  uint64_t commitEpoch = 0;
  std::array<std::shared_ptr<const ColumnSnap>, 9> cols;
  std::array<bool, 9> present{};
  // Apron cell STATE read live at pin time (the 1736 border cells outside the own 16^3, in
  // gather order): the sim mutates some cell state in place without a dirty flag (signal
  // timers etc.), so neighbor snapshots can lag on state alone. Ids/materials always come
  // from the snapshots (every block / material write is dirty-flagged).
  std::array<CellState, (kChunkSize + 2) * (kChunkSize + 2) * (kChunkSize + 2) - kSubChunkVolume> apronSt{};
};

struct CowStats {
  uint64_t pins = 0, subsPublished = 0, columnsFullPublished = 0, evictedColumns = 0;
  uint64_t verifyChecked = 0, verifyMismatches = 0, retainedBytes = 0, gathers = 0;
};

class ColumnTable {
 public:
  /// Republish the sub-chunks in `mask` (bit sy) of live column (cx, cz); a column whose
  /// instance changed (evicted + reloaded) or that was never published is published whole.
  void publish(const ChunkMap& w, const cBlockReg& reg, int cx, int cz, uint64_t mask);
  /// Pin the neighborhood of (cx, sy, cz); unpublished / stale neighbor columns are published
  /// whole first. Owner thread (the thread that may mutate `w`).
  std::shared_ptr<const MeshPin> pin(const ChunkMap& w, const cBlockReg& reg, int cx, int sy, int cz,
                                     BlockId belowWorld, MeshEdge edge);
  void drop(int cx, int cz);
  size_t columns() const;

 private:
  struct Entry { std::shared_ptr<const ColumnSnap> snap; uint64_t lastUse = 0; size_t bytes = 0; };
  std::shared_ptr<const ColumnSnap> publishLocked(const ChunkMap& w, const cBlockReg& reg, int cx, int cz, uint64_t mask);
  void enforceBudgetLocked();
  mutable std::mutex m_;
  std::unordered_map<uint64_t, Entry> entries_;
  uint64_t epoch_ = 0, use_ = 0;
  size_t bytes_ = 0;
};

/// Process-wide switch: true = pinned CoW (default), false = legacy owner-thread slab copy
/// (LL_LEGACY_SNAPSHOT=1, or setCowColumns(false)).
bool cowColumnsEnabled();
void setCowColumns(bool on);
CowStats cowStats();
void noteCowVerify(bool match);

}  // namespace litholattice
