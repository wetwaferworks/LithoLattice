// Chunk handshake ingress (Thread Handshake P5, 2026-10-29).
//
//  - cSpscRing<T>: bounded single-producer/single-consumer ring.
//  - cMpscRing<T>: bounded multi-producer/single-consumer ring (Vyukov
//    sequence cells). Producers never block each other.
//  - cIngress<T>: an MPSC ring plus a lossless mutex overflow (the cold
//    path), with the cHandoffQueue push/drainInto shape. Sim Ingress
//    (worldgen/load jobs -> main promotion) and Render Ingress (mesh jobs
//    -> main upload) are cIngress instances. Nothing is ever dropped.
//  - cSimPromotionBudget: the sim pop budget (R7, Archie ruling): at most
//    kPerTick column promotions per sim tick stepped, spent at the commit
//    barrier, banked up to kMaxBank. Counted in sim ticks, not wall clock.
//    The spawn-spiral ring-center bypass is a separate predicate.
//  - RenderPopBudget: render pop N/K (at most N sub-chunks or K bytes per
//    frame; the first item always passes so a huge mesh cannot wedge).
//  - cBorderRing: lazy border ring - a deduped FIFO of columns whose
//    neighbor edges changed (BorderGen bumped), drained a few per frame.
//  - cDeferredFreeQueue<T>: retired objects are freed only after a fence
//    (frame count) passes, a bounded number per collect().
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from the alignas(64) head/tail split is intended
#endif

namespace litholattice {

namespace ingress_detail {
inline size_t roundPow2(size_t n) {
  size_t c = 2;
  while (c < n) c <<= 1;
  return c;
}
}  // namespace ingress_detail

template <class T>
class cSpscRing {
 public:
  explicit cSpscRing(size_t capacity) : mask_(ingress_detail::roundPow2(capacity) - 1), slots_(mask_ + 1) {}
  size_t capacity() const { return mask_ + 1; }
  bool tryPush(T&& v) {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) > mask_) return false;
    slots_[h & mask_] = std::move(v);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool tryPop(T& out) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = std::move(slots_[t & mask_]);
    slots_[t & mask_] = T{};
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  size_t sizeApprox() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

 private:
  const size_t mask_;
  std::vector<T> slots_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

template <class T>
class cMpscRing {
 public:
  explicit cMpscRing(size_t capacity) : mask_(ingress_detail::roundPow2(capacity) - 1), cells_(new Cell[mask_ + 1]) {
    for (size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  size_t capacity() const { return mask_ + 1; }
  bool tryPush(T&& v) {
    size_t pos = enq_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos);
      if (dif == 0) {
        if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.val = std::move(v);
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;  // full
      } else {
        pos = enq_.load(std::memory_order_relaxed);
      }
    }
  }
  /// Single consumer.
  bool tryPop(T& out) {
    Cell& c = cells_[deq_ & mask_];
    const size_t seq = c.seq.load(std::memory_order_acquire);
    if (intptr_t(seq) - intptr_t(deq_ + 1) < 0) return false;  // empty (or a producer mid-write)
    out = std::move(c.val);
    c.val = T{};
    c.seq.store(deq_ + mask_ + 1, std::memory_order_release);
    ++deq_;
    return true;
  }

 private:
  struct Cell {
    std::atomic<size_t> seq{0};
    T val{};
  };
  const size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(64) std::atomic<size_t> enq_{0};
  alignas(64) size_t deq_ = 0;
};

/// Default ingress weigher: items carry no byte weight.
struct IngressNoWeight {
  template <class T>
  size_t operator()(const T&) const { return 0; }
};

/// MPSC ingress: lock-free ring, lossless overflow. Same shape as cHandoffQueue.
/// `W` weighs each item in bytes; queuedBytes() is the live total (pushed, not
/// yet drained), which producers' owners use for backpressure - the ingress
/// itself never refuses or drops an item.
template <class T, class W = IngressNoWeight>
class cIngress {
 public:
  explicit cIngress(size_t capacity = 1024) : ring_(capacity) {}
  void push(T v) {
    bytes_.fetch_add(W{}(v), std::memory_order_relaxed);
    if (ring_.tryPush(std::move(v))) return;
    std::lock_guard<std::mutex> l(m_);
    overflow_.push_back(std::move(v));
    overflowN_.fetch_add(1, std::memory_order_release);
    overflowTotal_.fetch_add(1, std::memory_order_relaxed);
  }
  /// Single consumer: ring first, then the overflow. Order across the two is
  /// not FIFO (consumers coalesce by key/seq; nothing relies on arrival order).
  void drainInto(std::vector<T>& out) {
    T v{};
    size_t drained = 0;
    while (ring_.tryPop(v)) { drained += W{}(v); out.push_back(std::move(v)); }
    if (overflowN_.load(std::memory_order_acquire) == 0) { bytes_.fetch_sub(drained, std::memory_order_relaxed); return; }
    std::lock_guard<std::mutex> l(m_);
    for (auto& o : overflow_) { drained += W{}(o); out.push_back(std::move(o)); }
    bytes_.fetch_sub(drained, std::memory_order_relaxed);
    overflowN_.fetch_sub(overflow_.size(), std::memory_order_release);
    overflow_.clear();
  }
  uint64_t overflowTotal() const { return overflowTotal_.load(std::memory_order_relaxed); }
  size_t queuedBytes() const { return bytes_.load(std::memory_order_relaxed); }

 private:
  cMpscRing<T> ring_;
  std::mutex m_;
  std::vector<T> overflow_;
  std::atomic<size_t> overflowN_{0};
  std::atomic<uint64_t> overflowTotal_{0};
  std::atomic<size_t> bytes_{0};
};

/// Sim pop budget (R7): kPerTick promotions per sim tick stepped, banked to kMaxBank.
class cSimPromotionBudget {
 public:
  static constexpr int kPerTick = 4;
  static constexpr int kMaxBank = 16;
  /// Idle-sim fallback: after this many consecutive opens with no tick stepped
  /// (sim paused / in a menu: there is no tick budget to protect), each further
  /// open grants one tick's worth so the world keeps filling behind the menu.
  static constexpr int kIdleOpens = 8;
  /// Call at the commit barrier with the sim's ticks-stepped counter. The first
  /// call seeds one tick's worth. Returns the promotions available now.
  int open(uint64_t ticksNow) {
    if (!seeded_) {
      seeded_ = true;
      last_ = ticksNow;
      bank_ = kPerTick;
    } else if (ticksNow > last_) {
      const uint64_t d = ticksNow - last_;
      last_ = ticksNow;
      idleOpens_ = 0;
      bank_ = int(std::min<uint64_t>(uint64_t(kMaxBank), uint64_t(bank_) + d * uint64_t(kPerTick)));
    } else if (++idleOpens_ >= kIdleOpens) {
      bank_ = std::max(bank_, kPerTick);
    }
    return bank_;
  }
  void spend(int n) { bank_ = n >= bank_ ? 0 : bank_ - n; }
  int bank() const { return bank_; }

 private:
  bool seeded_ = false;
  int idleOpens_ = 0;
  uint64_t last_ = 0;
  int bank_ = 0;
};

/// Ring-center bypass: within `radius` (Chebyshev) of the center, and only while
/// the initial spawn spiral is still filling.
inline bool spawnSpiralBypass(bool spiralActive, int cx, int cz, int centerCx, int centerCz, int radius = 1) {
  if (!spiralActive) return false;
  const int dx = cx > centerCx ? cx - centerCx : centerCx - cx;
  const int dz = cz > centerCz ? cz - centerCz : centerCz - cz;
  return (dx > dz ? dx : dz) <= radius;
}

/// Render pop N/K.
struct RenderPopBudget {
  int maxItems = 32;             ///< N
  size_t maxBytes = 3u << 20;    ///< K
  int items = 0;
  size_t bytes = 0;
  /// True when an item of `itemBytes` may pop now (the first always may).
  bool admit(size_t itemBytes) const {
    if (items >= maxItems) return false;
    return items == 0 || bytes + itemBytes <= maxBytes;
  }
  void take(size_t itemBytes) { ++items; bytes += itemBytes; }
};

/// Lazy border ring: deduped FIFO of column keys. Single thread.
class cBorderRing {
 public:
  bool note(uint64_t key) {
    if (!set_.insert(key).second) return false;
    q_.push_back(key);
    return true;
  }
  size_t popUpTo(size_t n, std::vector<uint64_t>& out) {
    size_t k = 0;
    while (k < n && !q_.empty()) {
      const uint64_t key = q_.front();
      q_.pop_front();
      set_.erase(key);
      out.push_back(key);
      ++k;
    }
    return k;
  }
  void forget(uint64_t key) {
    if (set_.erase(key)) std::erase(q_, key);
  }
  void clear() { q_.clear(); set_.clear(); }
  bool empty() const { return q_.empty(); }
  size_t size() const { return q_.size(); }

 private:
  std::deque<uint64_t> q_;
  std::unordered_set<uint64_t> set_;
};

/// Deferred free queue: objects die only after their fence has passed.
template <class T>
class cDeferredFreeQueue {
 public:
  void retire(std::unique_ptr<T> p, uint64_t fence) {
    if (p) q_.push_back({fence, std::move(p)});
  }
  /// Frees up to `maxFrees` entries whose fence <= `completed`. Returns the count freed.
  size_t collect(uint64_t completed, size_t maxFrees = SIZE_MAX) {
    size_t n = 0;
    while (n < maxFrees && !q_.empty() && q_.front().fence <= completed) {
      q_.pop_front();
      ++n;
    }
    freed_ += n;
    return n;
  }
  size_t pending() const { return q_.size(); }
  uint64_t freedTotal() const { return freed_; }

 private:
  struct E {
    uint64_t fence;
    std::unique_ptr<T> p;
  };
  std::deque<E> q_;
  uint64_t freed_ = 0;
};

}  // namespace litholattice

#ifdef _MSC_VER
#pragma warning(pop)
#endif
