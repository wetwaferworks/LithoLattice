#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <cassert>
#include "litholattice/world/chunk.h"
#include "litholattice/world/column_table.h"  // render pipeline M5
#include "litholattice/world/column_edge_shell.h"  // Q12 far demote
#include "litholattice/core/memory_ledger.h"  // Gate 0 readout
#include "litholattice/world/column_directory.h"  // t46u item 6b-2b

namespace litholattice {

/// t46u item 6b-2b: true on the tick side (the sim tick thread). Its column lookups read the directory
/// it adopted at its last phase boundary instead of the owner's live map.
inline thread_local bool tl_lkTickSide = false;

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
    const uint64_t k = ChunkKey::make(cx, cz);
    if (tl_lkTickSide && tickDir_) return tickDir_->find(k);  // t46u 6b-2b: the adopted directory
    auto it = columns_.find(k);
    return it == columns_.end() ? nullptr : it->second.get();
  }
  const ChunkColumn* column(int cx, int cz) const {
    const uint64_t k = ChunkKey::make(cx, cz);
    if (tl_lkTickSide && tickDir_) return tickDir_->find(k);  // t46u 6b-2b: the adopted directory
    auto it = columns_.find(k);
    return it == columns_.end() ? nullptr : it->second.get();
  }

  // ---- t46u item 6b-2b (JD 2026-11-16): published column directory ----
  // The structure owner (the game thread while the sim runs) publishes the column set after it changes
  // it; the tick side adopts at each phase boundary where it takes the world, and its column() lookups
  // read that directory. Removed columns are retired until the tick side has acknowledged a directory
  // without them. In this step the owner still changes structure only under the world gate and the
  // tick side adopts each time it takes the gate, so what the tick side sees is exactly the live map.
  /// Structure owner: publishes the column set if it changed since the last publish (no-op otherwise),
  /// then frees retired columns the tick side can no longer reach. 6b-2b: call it while still holding
  /// the world (before the hold is released), so the tick side's next adoption matches the live map.
  void publishDirectory() {
    dir_->reclaim();
    if (dir_->publishedSeq() != 0 && publishedGen_ == structureGen_) return;
    std::unordered_map<uint64_t, ChunkColumn*> m;
    m.reserve(columns_.size());
    for (auto& [k, c] : columns_) m.emplace(k, c.get());
    dir_->publish(std::move(m), structureGen_);
    publishedGen_ = structureGen_;
  }
  /// Tick side: adopts the newest directory whose generation matches the live structure.
  /// A publish that has not caught up is left unused (counted: staleAdoptions). JD 2026-11-17 r6:
  /// do not drop the last adopted directory and fall back to the live map — the owner may be
  /// changing structure without the world gate, so the tick side keeps the last consistent view
  /// until a matching publish arrives.
  void adoptDirectory() {
    const cColumnDirectory::Dir* d = dir_->adopt();
    if (d && d->gen == structureGen_) { tickDir_ = d; return; }
    if (d) dir_->noteStale();
  }
  /// After the tick thread is joined (the stopper owns everything): forget the adopted view and free
  /// every retired column.
  void tickSideStopped() {
    tickDir_ = nullptr;
    dir_->setConsumerLive(false);
    dir_->reclaimAllStopped();
    if (cow_) cow_->handOver();  // JD 2026-11-17: the joined tick thread's CoW table ownership comes back
  }
  /// Structure owner, before the tick thread starts: publish and mark the tick side live.
  void tickSideStarting() {
    publishDirectory();
    dir_->setConsumerLive(true);
    if (cow_) cow_->handOver();  // JD 2026-11-17: main -> tick thread; it claims the CoW table on first use
  }
  /// True when everything the owner removed before its last publish is unreachable from the tick side
  /// (deferred frees of extracted / demoted columns wait for this).
  bool tickSideCaughtUp() const { return dir_->consumerCaughtUp(); }
  uint64_t directoryStaleAdoptions() const { return dir_->staleAdoptions(); }
  size_t directoryRetired() const { return dir_->retired(); }
  const SubChunk* sub(int cx, int sy, int cz) const {
    if (sy < 0 || sy >= kSubChunks) return nullptr;
    const ChunkColumn* c = column(cx, cz);
    return c ? c->subs[sy].get() : nullptr;
  }
  // World-coordinate access (owner thread only: main before the sim starts, sim after).
  BlockId get(int x, int y, int z) const {
    if (y < 0 || (y >> 4) >= kSubChunks) return kAir;
    const ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return kAir;
    const SubChunk* s = c->subs[size_t(y >> 4)].get();
    StableId sid;
    if (s) sid = s->at(x & 15, y & 15, z & 15);
    else if (c->edgeShell) sid = c->edgeShell->at(y >> 4, x & 15, y & 15, z & 15);  // Q12: demoted ring
    else return kAir;
    if (isAirStableId(sid)) return kAir;
    assert(registry_ && "ChunkMap::get without setRegistry()");
    const auto id = registry_->findByStableId(sid);
    return id ? *id : kAir;  // unreachable in a consistent world; air is the safe answer
  }
  CellState getState(int x, int y, int z) const {
    if (y < 0 || (y >> 4) >= kSubChunks) return 0;
    const ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return 0;
    if (const SubChunk* s = c->subs[size_t(y >> 4)].get()) return s->state[SubChunk::index(x & 15, y & 15, z & 15)];
    return c->edgeShell ? c->edgeShell->stateAt(y >> 4, x & 15, y & 15, z & 15) : 0;  // Q12
  }
  /// Highest non-air y in world column (x, z), or -1 if empty or not generated.
  int surfaceY(int x, int z) const {
    const ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return -1;
    for (int sy = kSubChunks - 1; sy >= 0; --sy) {
      const SubChunk* s = c->subs[sy].get();
      if (s) {
        for (int y = kChunkSize - 1; y >= 0; --y)
          if (!isAirStableId(s->at(x & 15, y, z & 15))) return sy * kChunkSize + y;
      } else if (c->edgeShell) {  // Q12: demoted columns resolve through the shell, as get() does
        for (int y = kChunkSize - 1; y >= 0; --y)
          if (!isAirStableId(c->edgeShell->at(sy, x & 15, y, z & 15))) return sy * kChunkSize + y;
      }
    }
    return -1;
  }
  // Returns false if outside generated columns / world height.
  bool set(int x, int y, int z, BlockId b) {
    if (y < 0 || y >= kWorldHeight) return false;
    ChunkColumn* c = column(x >> 4, z >> 4);
    if (!c) return false;
    if (c->edgeShell) { cColumnEdgeShell::noteRefusedWrite(); return false; }  // Q12: demoted columns are read-only
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
    if (c && c->edgeShell) { cColumnEdgeShell::noteRefusedWrite(); return; }  // Q12
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
    if (c->edgeShell) { cColumnEdgeShell::noteRefusedWrite(); return false; }  // Q12
    const uint32_t k = ChunkColumn::cellTextKey(x, y, z);
    if (text.empty()) { if (c->cellText.erase(k)) c->markDirty(); else return true; }
    else { c->cellText[k] = std::move(text); c->markDirty(); }
    if (cow_) cow_->noteStale(x >> 4, y >> 4, z >> 4);  // Archie review H7: snapshot carries cellText
    return true;
  }
  /// Q12 far demote: replaces the column's sub-chunks with its 1-cell edge shell and hands
  /// the freed body back for deferred free. Null when
  /// not resident or already demoted. Owner thread only; the caller owns eligibility.
  std::unique_ptr<ChunkColumn> demote(int cx, int cz) {
    ChunkColumn* c = column(cx, cz);
    if (!c || c->edgeShell) return nullptr;
    c->edgeShell = cColumnEdgeShell::build(*c);
    auto body = std::make_unique<ChunkColumn>();
    body->cx = cx;
    body->cz = cz;
    body->subs.swap(c->subs);
    if (cow_) cow_->drop(cx, cz);  // republished (whole, from the shell) on its next pin
    ++structureGen_;               // sub pointers changed (random-tick cache, Archie C5)
    if (tl_lkTickSide) tickDir_ = nullptr;  // t46u 6b-2b: a tick-side change: read the live map until the next adoption
    return body;
  }
  void insert(std::unique_ptr<ChunkColumn> col) {
    const int cx = col->cx, cz = col->cz;
    cow_->drop(cx, cz);  // M5: a replaced column is republished on its next pin
    auto& slot = columns_[ChunkKey::make(cx, cz)];
    if (!slot) cMemoryLedger::add(cMemoryLedger::kChunkColumns, 1);  // Gate 0 (replace keeps the count)
    else dir_->retire(std::move(slot));  // t46u 6b-2b: the replaced column lives until the tick side has let go of it
    slot = std::move(col);
    ++structureGen_;  // Archie review C5
    if (tl_lkTickSide) tickDir_ = nullptr;  // t46u 6b-2b (see demote)
    if (editListener_) editListener_(cx, cz);
  }
  /// Column eviction (A2 / B-M1): removes the column at (cx, cz) and frees
  /// its sub-chunks. Returns false when the column was not resident. The
  /// caller owns the eviction discipline (save-first for dirty columns,
  /// cSignalSim::unscanColumn, mesh retire, streamer cleanup) -- this is
  /// only the map operation. Owner thread only, like insert().
  bool erase(int cx, int cz) {
    const auto it = columns_.find(ChunkKey::make(cx, cz));
    const bool gone = it != columns_.end();
    if (gone) { dir_->retire(std::move(it->second)); columns_.erase(it); }  // t46u 6b-2b: retired, not freed
    if (gone) { ++structureGen_; cMemoryLedger::sub(cMemoryLedger::kChunkColumns, 1); }  // Archie review C5; Gate 0
    if (gone && tl_lkTickSide) tickDir_ = nullptr;  // t46u 6b-2b (see demote)
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
    if (tl_lkTickSide) tickDir_ = nullptr;  // t46u 6b-2b (see demote). The caller must not free it before tickSideCaughtUp()
    cMemoryLedger::sub(cMemoryLedger::kChunkColumns, 1);  // Gate 0
    cow_->drop(cx, cz);  // M5
    if (editListener_) editListener_(cx, cz);
    return col;
  }
  /// Archie review C5 (2026-10-30): bumped by every insert/erase/extract, so
  /// a holder of raw ChunkColumn pointers can tell the column set changed
  /// even when the count did not (evict K + insert K, reload, replace).
  uint64_t structureGen() const { return structureGen_; }
  bool contains(int cx, int cz) const { return column(cx, cz) != nullptr; }  // t46u 6b-2b: same view as column()
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
  // t46u item 6b-2b. Held by pointer so ChunkMap stays movable (the directory holds atomics).
  std::unique_ptr<cColumnDirectory> dir_ = std::make_unique<cColumnDirectory>();
  uint64_t publishedGen_ = 0;                          // owner: structureGen_ at the last publish
  const cColumnDirectory::Dir* tickDir_ = nullptr;     // tick side: the adopted directory (null = live map)
};

}  // namespace litholattice
