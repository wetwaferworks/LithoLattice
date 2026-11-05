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
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "litholattice/world/chunk.h"
#include "litholattice/world/open_addr_table.h"
#include "litholattice/core/lf_handoff.h"    // 2026-11-01 LL_LF_COWTABLE
#include "litholattice/core/lf_key_table.h"
#include <atomic>
#include <thread>

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
  // M9 (Archie ruling 4): no live apron read. Every in-place cell-state edit marks its
  // sub-chunk stale (noteStale); stale subs are republished in the per-tick batch
  // (flushStale) or, at the latest, when a pin needs that column. LL_COW_VERIFY checks it.
};

struct CowStats {
  uint64_t pins = 0, subsPublished = 0, columnsFullPublished = 0, evictedColumns = 0;
  uint64_t verifyChecked = 0, verifyMismatches = 0, retainedBytes = 0, gathers = 0;
  uint64_t staleNoted = 0, staleFlushed = 0, stalePinned = 0, maxProbe = 0;
  uint64_t lfForeignOwner = 0;  ///< LL_LF_COWTABLE: owner-only calls seen from a second thread
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
  /// M9: an in-place state edit of sub (cx, sy, cz) (any thread holding the map; locked).
  void noteStale(int cx, int sy, int cz);
  /// M9 (ruling 6): take the stale masks (column key -> sub mask) for the per-tick batch.
  std::vector<std::pair<uint64_t, uint64_t>> takeStale();
  /// 2026-11-01 (JD LL_LF_COWTABLE=1, default off): no mutex. noteStale (any thread) goes to a
  /// lock-free key table (spill: lock-free handoff); publish / pin / drop / takeStale run on the
  /// owner thread only (the thread that may mutate the map: sim thread, or main before the sim
  /// starts) and drain the stale marks first. columns() reads an atomic. Set before first use.
  void setLockFree(bool) { lf_ = true; }  // Archie PlayI: LF-only; mutex path removed
  bool lockFree() const { return true; }

 private:
  void lfEnter();  // owner-thread entry on the lock-free path: tripwire + drain stale marks
  bool lf_ = true;  // Archie PlayI: always lock-free
  cLfKeyTable lfStale_{size_t(1) << 15};
  cLfHandoff<std::pair<uint64_t, uint64_t>> lfSpill_;
  std::atomic<size_t> lfCount_{0};
  std::atomic<std::thread::id> lfOwner_{};
  struct Entry { std::shared_ptr<const ColumnSnap> snap; uint64_t lastUse = 0; size_t bytes = 0; };
  std::shared_ptr<const ColumnSnap> publishLocked(const ChunkMap& w, const cBlockReg& reg, int cx, int cz, uint64_t mask);
  void enforceBudgetLocked();
  // PlayI: mutex removed (LF-only)
  OaTable<Entry> entries_;     // M9: open addressing (ruling 6)
  OaTable<uint64_t> stale_;    // M9: column key -> stale sub mask
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
