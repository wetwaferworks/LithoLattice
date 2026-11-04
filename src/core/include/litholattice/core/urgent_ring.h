// Bounded urgent ring with atomic counters (JD t3557u / t3560u, 2026-10-30).
//
// The urgent lane of the lock-free on-demand pool (cLockFreePool, LL_POOL_LOCKFREE=1):
// tick / control / event work that must never queue behind render or gen/mesh work.
// Algorithm: Dmitry Vyukov's bounded ring; every slot carries a sequence counter, so the
// producer and any number of consumer workers coordinate through atomics only (no mutex,
// no condition variable). Built for one producer (the tick thread) but safe with several:
// the enqueue cursor is claimed by CAS. tryPush returns false when full WITHOUT touching
// the item (the caller spills it to the normal lane).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace litholattice {

template <typename T, size_t kCapacity = 256>
class cUrgentRing {
  static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

 public:
  cUrgentRing() {
    for (size_t i = 0; i < kCapacity; ++i) slots_[i].seq.store(i, std::memory_order_relaxed);
  }
  cUrgentRing(const cUrgentRing&) = delete;
  cUrgentRing& operator=(const cUrgentRing&) = delete;

  bool tryPush(T& item) {
    size_t pos = enq_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots_[pos & (kCapacity - 1)];
      const size_t seq = s.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos);
      if (dif == 0) {
        if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          count_.fetch_add(1, std::memory_order_acq_rel);  // before publish: pending() never goes negative
          s.value = std::move(item);
          s.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;  // full
      } else {
        pos = enq_.load(std::memory_order_relaxed);
      }
    }
  }

  bool tryPop(T& out) {
    size_t pos = deq_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& s = slots_[pos & (kCapacity - 1)];
      const size_t seq = s.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos + 1);
      if (dif == 0) {
        if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = std::move(s.value);
          s.seq.store(pos + kCapacity, std::memory_order_release);
          count_.fetch_sub(1, std::memory_order_acq_rel);
          return true;
        }
      } else if (dif < 0) {
        return false;  // empty
      } else {
        pos = deq_.load(std::memory_order_relaxed);
      }
    }
  }

  /// Items pushed and not yet popped.
  int pending() const { return count_.load(std::memory_order_acquire); }

 private:
  struct alignas(64) Slot {
    std::atomic<size_t> seq{0};
    T value{};
  };
  Slot slots_[kCapacity];
  alignas(64) std::atomic<size_t> enq_{0};
  alignas(64) std::atomic<size_t> deq_{0};
  alignas(64) std::atomic<int> count_{0};
};

}  // namespace litholattice