#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <cassert>
#include "litholattice/world/chunk.h"
#include "litholattice/world/column_table.h"  // render pipeline M5

namespace litholattice {

// Chunk map of one dimension: hash of chunk coords -> column. Single owner thread at a
// time (main while loading, the sim thread while the dimension is active).
class ChunkMap {
 public:
  /// The registry used to translate between the BlockId currency of get()/set()
  /// and the StableId currency of sub-chunk voxel storage. Must be installed
  /// before any get()/set() call (the owner installs it once at startup; cSimThread
  /// installs it for the map it is given). get()/set() assert without it: there
  /// is no meaningful translation otherwise. Full migration of game logic to
  /// StableIds is WS1 (future work).
  void setRegistry(const cBlockReg* registry) { registry_ = registry; }
  const cBlockReg* registry() const { return registry_; }

  /// Edit notification (P0 nav-invalidation feed, 2026-10-14): after any
  /// structural change to a column -- a successful set(), an erase(), an
  /// insert() -- the listener is called with the COLUMN coords (cx, cz) on
  /// the mutating (owner) thread. Consumers must be cheap and thread-safe
  /// (e.g. post the column to a queue drained elsewhere); the map never
  /// calls back into itself. Optional; null (the default) changes nothing.
  /// setState() does NOT notify: cell state never changes a cell's BlockId,
  /// which is all derived data (nav bits) reads.
  void setEditListener(std::function<void(int cx, int cz)> fn) { editListener_ = std::move(fn); }

  ChunkColumn* column(int cx, int cz) {
    auto it = columns_.find(ChunkKey::make(cx, cz));
    return it == columns_.end() ? nullptr : it->second.get();
  }
  const ChunkColumn* column(int cx, int cz) const {
    auto it = columns_.find(ChunkKey::make(cx, cz));
    return it == columns_.end() ? nullptr : it->second.get();
  }
  const SubChunk* sub(int cx, int sy, int cz) const {
    if (sy < 0 || sy >= kSubChunks) return nullptr;
    const ChunkColumn* c = column(cx, cz);
    return c ? c->subs[sy].get() : nullptr;
  }
  // World-coordinate access (owner thread only: main before the sim starts, sim after).
  BlockId get(int x, int y, int z) const {
    const SubChunk* s = sub(x >> 4, y >> 4, z >> 4);
    if (!s) return kAir;
    const StableId sid = s->at(x & 15, y & 15, z & 15);
    if (isAirStableId(sid)) return kAir;
    assert(registry_ && "ChunkMap::get without setRegistry()");
    const auto id = registry_->findByStableId(sid);
    return id ? *id : kAir;  // unreachable in a consistent world; air is the safe answer
  }
  CellState getState(int x, int y, int z) const {
    const SubChunk* s = sub(x >> 4, y >> 4, z >> 4);
    return s ? s->state[SubChunk::index(x & 15, y & 15, z & 15)] : 0;
  }
  /// Highest non-air y in world column (x, z), or -1 if empty or not generated.
  int surfaceY(int x, int z) const {
    const ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return -1;
    for (int sy = kSubChunks - 1; sy >= 0; --sy) {
      const SubChunk* s = c->subs[sy].get();
      if (!s) continue;
      for (int y = kChunkSize - 1; y >= 0; --y)
        if (!isAirStableId(s->at(x & 15, y, z & 15))) return sy * kChunkSize + y;
    }
    return -1;
  }
  // Returns false if outside generated columns / world height.
  bool set(int x, int y, int z, BlockId b) {
    if (y < 0 || y >= kWorldHeight) return false;
    ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return false;
    auto& s = c->subs[y >> 4];
    if (!s) { if (b == kAir) return true; s = std::make_unique<SubChunk>(); }
    assert(registry_ && "ChunkMap::set without setRegistry()");
    s->set(x & 15, y & 15, z & 15, registry_->stableIdOf(b));
    if (!c->cellText.empty()) c->cellText.erase(ChunkColumn::cellTextKey(x, y, z));  // Posts: text dies with its block
    c->markDirty();  // A2/B-M1: resident-column mutation (eviction save-first)
    if (cow_) cow_->noteStale(x >> 4, y >> 4, z >> 4);  // M9
    if (editListener_) editListener_(x >> 4, z >> 4);
    return true;
  }
  void setState(int x, int y, int z, CellState v) {
    if (y < 0 || y >= kWorldHeight) return;
    ChunkColumn* c = column(x >> 4, z >> 4);
    if (c && c->subs[y >> 4]) {
      CellState& cell = c->subs[y >> 4]->state[SubChunk::index(x & 15, y & 15, z & 15)];
      if (cell != v) { cell = v; c->markDirty(); if (cow_) cow_->noteStale(x >> 4, y >> 4, z >> 4); }  // A2/B-M1; M9 ruling 4
    }
  }
  /// 2026-10-25 (JD, Posts): per-cell text. Null when the cell has none (or its column is absent).
  const std::string* cellText(int x, int y, int z) const {
    const ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c || c->cellText.empty() || y < 0 || y >= kWorldHeight) return nullptr;
    auto it = c->cellText.find(ChunkColumn::cellTextKey(x, y, z));
    return it == c->cellText.end() ? nullptr : &it->second;
  }
  /// Sets (empty erases) a cell's text; marks the column dirty. False outside resident columns.
  bool setCellText(int x, int y, int z, std::string text) {
    if (y < 0 || y >= kWorldHeight) return false;
    ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return false;
    const uint32_t k = ChunkColumn::cellTextKey(x, y, z);
    if (text.empty()) { if (c->cellText.erase(k)) c->markDirty(); else return true; }
    else { c->cellText[k] = std::move(text); c->markDirty(); }
    if (cow_) cow_->noteStale(x >> 4, y >> 4, z >> 4);  // Archie review H7: snapshot carries cellText
    return true;
  }
  void insert(std::unique_ptr<ChunkColumn> col) {
    const int cx = col->cx, cz = col->cz;
    cow_->drop(cx, cz);  // M5: a replaced column is republished on its next pin
    columns_[ChunkKey::make(cx, cz)] = std::move(col);
    ++structureGen_;  // Archie review C5
    if (editListener_) editListener_(cx, cz);
  }
  /// Column eviction (A2 / B-M1): removes the column at (cx, cz) and frees
  /// its sub-chunks. Returns false when the column was not resident. The
  /// caller owns the eviction discipline (save-first for dirty columns,
  /// cSignalSim::unscanColumn, mesh retire, streamer cleanup) -- this is
  /// only the map operation. Owner thread only, like insert().
  bool erase(int cx, int cz) {
    const bool gone = columns_.erase(ChunkKey::make(cx, cz)) != 0;
    if (gone) ++structureGen_;  // Archie review C5
    cow_->drop(cx, cz);  // M5
    if (gone && editListener_) editListener_(cx, cz);
    return gone;
  }
  /// P5 deferred free: like erase(), but hands the column back instead of
  /// freeing it, so the caller can release the memory outside the sim hold
  /// (cDeferredFreeQueue). Null when the column was not resident.
  std::unique_ptr<ChunkColumn> extract(int cx, int cz) {
    auto it = columns_.find(ChunkKey::make(cx, cz));
    if (it == columns_.end()) return nullptr;
    std::unique_ptr<ChunkColumn> col = std::move(it->second);
    columns_.erase(it);
    ++structureGen_;  // Archie review C5
    cow_->drop(cx, cz);  // M5
    if (editListener_) editListener_(cx, cz);
    return col;
  }
  /// Archie review C5 (2026-10-30): bumped by every insert/erase/extract, so
  /// a holder of raw ChunkColumn pointers can tell the column set changed
  /// even when the count did not (evict K + insert K, reload, replace).
  uint64_t structureGen() const { return structureGen_; }
  bool contains(int cx, int cz) const { return columns_.find(ChunkKey::make(cx, cz)) != columns_.end(); }
  auto& columns() { return columns_; }
  const auto& columns() const { return columns_; }
  /// Render pipeline M5: published copy-on-write column snapshots for mesh workers
  /// (column_table.h). Mutated only by the map's current owner thread (internally locked).
  ColumnTable& cowTable() const { return *cow_; }
  /// M9 (Archie ruling 4): callers that write SubChunk::state through cached raw pointers
  /// (the signal sim fast paths) report the cell here so the CoW snapshot is republished.
  void noteStateEdit(int x, int y, int z) const {
    if (cow_ && y >= 0 && y < kWorldHeight) cow_->noteStale(x >> 4, y >> 4, z >> 4);
  }

 private:
  std::unordered_map<uint64_t, std::unique_ptr<ChunkColumn>> columns_;
  uint64_t structureGen_ = 0;  // Archie review C5
  const cBlockReg* registry_ = nullptr;
  std::function<void(int cx, int cz)> editListener_;
  std::unique_ptr<ColumnTable> cow_ = std::make_unique<ColumnTable>();  // M5
};

}  // namespace litholattice
