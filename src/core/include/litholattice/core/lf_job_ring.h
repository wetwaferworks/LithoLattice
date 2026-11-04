// cLfJobRing (JD 2026-11-01, LL_LF_JOBS): bounded multi-producer / multi-consumer ring of
// pointers, Vyukov sequence cells. Atomics only: no mutex, no condition variable, no wait.
// tryPush / tryPop never block; a full ring returns false (callers decide what to do).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding is intended
#endif

namespace litholattice {

template <class T>
class cLfJobRing {
  static_assert(std::is_trivially_copyable_v<T>, "cLfJobRing holds trivially copyable values (pointers)");

 public:
  explicit cLfJobRing(size_t capacity) : mask_(roundPow2(capacity) - 1), cells_(new Cell[mask_ + 1]) {
    for (size_t i = 0; i <= mask_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  cLfJobRing(const cLfJobRing&) = delete;
  cLfJobRing& operator=(const cLfJobRing&) = delete;

  size_t capacity() const { return mask_ + 1; }

  bool tryPush(T v) {
    size_t pos = enq_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos);
      if (dif == 0) {
        if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.val = v;
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

  bool tryPop(T& out) {
    size_t pos = deq_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const size_t seq = c.seq.load(std::memory_order_acquire);
      const intptr_t dif = intptr_t(seq) - intptr_t(pos + 1);
      if (dif == 0) {
        if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = c.val;
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;  // empty (or a producer mid-write)
      } else {
        pos = deq_.load(std::memory_order_relaxed);
      }
    }
  }

  size_t sizeApprox() const {
    const size_t e = enq_.load(std::memory_order_acquire), d = deq_.load(std::memory_order_acquire);
    return e > d ? e - d : 0;
  }

 private:
  static size_t roundPow2(size_t n) { size_t c = 2; while (c < n) c <<= 1; return c; }
  struct Cell {
    std::atomic<size_t> seq{0};
    T val{};
  };
  const size_t mask_;
  std::unique_ptr<Cell[]> cells_;
  alignas(64) std::atomic<size_t> enq_{0};
  alignas(64) std::atomic<size_t> deq_{0};
};

}  // namespace litholattice

#ifdef _MSC_VER
#pragma warning(pop)
#endif
