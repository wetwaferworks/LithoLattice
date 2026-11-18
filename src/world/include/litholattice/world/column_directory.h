// t46u item 6b-2 (JD 2026-11-16): the published column directory.
// The structure owner (the game thread while the sim runs) is the only thread that inserts, erases,
// extracts or demotes columns. After its structural changes it publishes an immutable directory
// (column key -> column pointer) through one publish pointer. The tick thread adopts the newest
// directory at its phase boundary (the start of its mutation window) and acknowledges the sequence it
// adopted. A column the owner removed is retired, not freed, until the tick thread has acknowledged a
// directory published after the removal, so a pointer the tick thread can still reach never dangles.
// No lock: one exchange to publish, one exchange to adopt, one release store to acknowledge.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include "litholattice/world/chunk.h"

namespace litholattice {

class cColumnDirectory {
 public:
  struct Dir {
    uint64_t seq = 0;
    uint64_t gen = 0;  // the owner's structure generation this directory was built from (6b-2b staleness check)
    std::unordered_map<uint64_t, ChunkColumn*> cols;
    ChunkColumn* find(uint64_t key) const {
      const auto it = cols.find(key);
      return it == cols.end() ? nullptr : it->second;
    }
  };

  cColumnDirectory() = default;
  cColumnDirectory(const cColumnDirectory&) = delete;
  cColumnDirectory& operator=(const cColumnDirectory&) = delete;
  ~cColumnDirectory() { delete pending_.exchange(nullptr, std::memory_order_acq_rel); }

  // ---- structure owner ----
  /// Publishes `cols` as the newest directory and returns its sequence. A directory the tick thread
  /// never adopted is replaced (and freed here: the tick thread can only reach a directory it adopted).
  uint64_t publish(std::unordered_map<uint64_t, ChunkColumn*> cols, uint64_t gen = 0) {
    auto* d = new Dir{++pubSeq_, gen, std::move(cols)};
    delete pending_.exchange(d, std::memory_order_acq_rel);
    return d->seq;
  }
  /// Keeps `col` alive until the tick thread has acknowledged a directory published after this call.
  /// The caller has already taken it out of its own map; the next publish() is the one without it.
  void retire(std::unique_ptr<ChunkColumn> col) {
    if (col) retired_.emplace_back(pubSeq_ + 1, std::move(col));
  }
  /// Frees the retired columns the tick thread can no longer reach. Returns how many were freed.
  size_t reclaim() { return reclaimUpTo(acked_.load(std::memory_order_acquire)); }
  /// Frees every retired column. Only when no tick thread is running (stopped, or never started).
  size_t reclaimAllStopped() { return reclaimUpTo(~uint64_t(0)); }
  size_t retired() const { return retired_.size(); }
  uint64_t publishedSeq() const { return pubSeq_; }

  // ---- tick thread ----
  /// Adopts the newest published directory if there is one, acknowledges it, and returns the current
  /// directory (null before the first publish). The previous directory is freed here, on this thread.
  const Dir* adopt() {
    if (Dir* d = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
      cur_.reset(d);
      acked_.store(d->seq, std::memory_order_release);
    }
    return cur_.get();
  }
  const Dir* current() const { return cur_.get(); }  // tick thread only
  uint64_t ackedSeq() const { return acked_.load(std::memory_order_acquire); }

  // ---- either side ----
  /// 6b-2b: true while a tick side may adopt (set by the owner around the tick thread's life).
  void setConsumerLive(bool on) { consumerLive_.store(on, std::memory_order_release); }
  /// True when nothing published is still unacknowledged, or no tick side is live: whatever the
  /// owner took out of its map before its last publish is unreachable from the tick side.
  bool consumerCaughtUp() const {
    return !consumerLive_.load(std::memory_order_acquire) || ackedSeq() >= pubSeq_;  // pubSeq_: owner reads its own
  }
  /// 6b-2b diagnostics: adoptions that found the owner's structure changed with no publish (the tick
  /// side then read the live map, still under the world gate). Should stay 0; nonzero = a missed publish site.
  void noteStale() { stale_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t staleAdoptions() const { return stale_.load(std::memory_order_relaxed); }

 private:
  size_t reclaimUpTo(uint64_t seq) {
    size_t n = 0;
    for (size_t i = 0; i < retired_.size();) {
      if (retired_[i].first <= seq) {
        retired_[i] = std::move(retired_.back());
        retired_.pop_back();
        ++n;
      } else {
        ++i;
      }
    }
    return n;
  }

  uint64_t pubSeq_ = 0;                                                       // owner only
  std::vector<std::pair<uint64_t, std::unique_ptr<ChunkColumn>>> retired_;  // owner only: (visible-from seq, column)
  std::atomic<Dir*> pending_{nullptr};                                       // newest unadopted directory
  alignas(64) std::atomic<uint64_t> acked_{0};                               // tick thread -> owner
  std::atomic<bool> consumerLive_{false};                                    // owner: a tick side may adopt
  std::atomic<uint64_t> stale_{0};                                           // diagnostics (see noteStale)
  std::unique_ptr<Dir> cur_;                                                 // tick thread only
};

}  // namespace litholattice
