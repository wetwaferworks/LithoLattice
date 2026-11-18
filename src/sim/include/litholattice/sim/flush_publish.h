// JD 2026-11-17: double-buffered dirty set + chunk-state snapshot.
// The sim writes the back buffer, the main thread reads only the published
// front, and one atomic index swap (release/acquire) publishes it at the
// phase boundary. The retired buffer is reused only when the reader has
// moved off it (generation + per-slot reader count, no spin).
#pragma once
#include "litholattice/world/column_table.h"
#include "litholattice/world/chunk.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace litholattice {

inline BlockId columnSnapBlock(const ColumnSnap& snap, int x, int y, int z) {
  if (y < 0 || (y >> 4) >= kSubChunks) return kAir;
  const SubSnap* s = snap.subs[size_t(y >> 4)].get();
  if (!s) return kAir;
  const int si = SubChunk::index(x & 15, y & 15, z & 15);
  if (!s->full.empty()) return s->full[size_t(si)];
  if (s->lut.empty()) return kAir;
  return s->lut[s->data.blocks.rawIndex(size_t(si))];
}

inline CellState columnSnapState(const ColumnSnap& snap, int x, int y, int z) {
  if (y < 0 || (y >> 4) >= kSubChunks) return 0;
  const SubSnap* s = snap.subs[size_t(y >> 4)].get();
  if (!s) return 0;
  return s->data.state[size_t(SubChunk::index(x & 15, y & 15, z & 15))];
}

struct FlushSlot {
  uint64_t gen = 0;
  std::atomic<uint32_t> readers{0};
  std::vector<int64_t> dirtyWork;
  std::vector<int64_t> dirtyHold;
  std::vector<std::pair<uint64_t, std::shared_ptr<const ColumnSnap>>> cols;
  std::unordered_map<uint64_t, std::shared_ptr<const ColumnSnap>> map;

  const ColumnSnap* find(uint64_t key) const {
    auto it = map.find(key);
    return it == map.end() ? nullptr : it->second.get();
  }
};

class cFlushPublish {
 public:
  /// Sim thread: claim the retired slot to fill. False if a reader still holds it
  /// (skip this flush; do not spin).
  bool tryBeginWrite() {
    const uint32_t f = front_.load(std::memory_order_acquire);
    back_ = 1u - f;
    FlushSlot& b = slot_[back_];
    if (b.gen != 0 && b.readers.load(std::memory_order_acquire) != 0) return false;
    b.dirtyWork.clear();
    b.dirtyHold.clear();
    b.cols.clear();
    b.map.clear();
    return true;
  }
  FlushSlot& back() { return slot_[back_]; }

  /// Sim thread: finish the back buffer and publish it as the new front.
  void publish() {
    FlushSlot& b = slot_[back_];
    b.map.clear();
    b.map.reserve(b.cols.size() * 2 + 1);
    for (auto& p : b.cols) b.map.emplace(p.first, p.second);
    b.gen = ++nextGen_;
    front_.store(back_, std::memory_order_release);
  }

  /// Main (or any reader): pin the current front. Null if nothing has been published.
  class Read {
   public:
    Read() = default;
    explicit Read(cFlushPublish& pub) : pub_(&pub) {
      for (int i = 0; i < 3; ++i) {
        const uint32_t idx = pub.front_.load(std::memory_order_acquire);
        FlushSlot& s = pub.slot_[idx];
        if (s.gen == 0) return;
        s.readers.fetch_add(1, std::memory_order_acq_rel);
        if (pub.front_.load(std::memory_order_acquire) == idx) {
          slot_ = &s;
          gen_ = s.gen;
          return;
        }
        s.readers.fetch_sub(1, std::memory_order_acq_rel);
      }
    }
    ~Read() { reset(); }
    Read(const Read&) = delete;
    Read& operator=(const Read&) = delete;
    Read(Read&& o) noexcept { *this = std::move(o); }
    Read& operator=(Read&& o) noexcept {
      if (this != &o) { reset(); pub_ = o.pub_; slot_ = o.slot_; gen_ = o.gen_; o.slot_ = nullptr; o.gen_ = 0; }
      return *this;
    }
    void reset() {
      if (slot_) {
        slot_->readers.fetch_sub(1, std::memory_order_acq_rel);
        slot_ = nullptr;
        gen_ = 0;
      }
    }
    explicit operator bool() const { return slot_ != nullptr; }
    const FlushSlot* get() const { return slot_; }
    uint64_t gen() const { return gen_; }
    const ColumnSnap* column(int cx, int cz) const {
      return slot_ ? slot_->find(ChunkKey::make(cx, cz)) : nullptr;
    }
    BlockId block(int x, int y, int z) const {
      const ColumnSnap* c = column(x >> 4, z >> 4);
      return c ? columnSnapBlock(*c, x, y, z) : kAir;
    }
    CellState state(int x, int y, int z) const {
      const ColumnSnap* c = column(x >> 4, z >> 4);
      return c ? columnSnapState(*c, x, y, z) : 0;
    }

   private:
    cFlushPublish* pub_ = nullptr;
    FlushSlot* slot_ = nullptr;
    uint64_t gen_ = 0;
  };

 private:
  FlushSlot slot_[2];
  std::atomic<uint32_t> front_{0};
  uint32_t back_ = 1;
  uint64_t nextGen_ = 0;
};

}  // namespace litholattice
