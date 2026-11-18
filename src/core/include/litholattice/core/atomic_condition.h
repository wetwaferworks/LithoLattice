// PlayN (JD Next #1): condition_variable replacement (no <condition_variable>, no mutex).
// Notify bumps a generation counter. A waiter releases its gate and polls the
// generation with a pause -> yield -> short-sleep backoff; it never parks on
// another thread (no atomic::wait / OS wait object), so nothing can block-wait
// on a peer. Same contract as std::condition_variable: spurious returns are
// allowed, so callers re-check their predicate under the gate.
#pragma once
#include "litholattice/core/spin_stats.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#if defined(_MSC_VER)
#  include <intrin.h>
#  define LL_ATOMIC_COND_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#  define LL_ATOMIC_COND_PAUSE() __builtin_ia32_pause()
#else
#  define LL_ATOMIC_COND_PAUSE() ((void)0)
#endif

namespace litholattice {

class cAtomicCondition {
 public:
  cAtomicCondition() = default;
  cAtomicCondition(const cAtomicCondition&) = delete;
  cAtomicCondition& operator=(const cAtomicCondition&) = delete;

  void notify_one() noexcept { gen_.fetch_add(1, std::memory_order_acq_rel); }
  void notify_all() noexcept { gen_.fetch_add(1, std::memory_order_acq_rel); }

  /// Release `lk`, poll until notified (or a bounded poll slice passes), re-acquire.
  template <typename Lock>
  void wait(Lock& lk) {
    const uint32_t g = gen_.load(std::memory_order_acquire);
    lk.unlock();
    pollUntil(g, std::chrono::steady_clock::time_point::max());
    lk.lock();
  }
  template <typename Lock, typename Pred>
  void wait(Lock& lk, Pred pred) {
    while (!pred()) wait(lk);
  }
  /// true = notified, false = timed out.
  template <typename Lock, typename Rep, typename Period>
  bool wait_for(Lock& lk, const std::chrono::duration<Rep, Period>& d) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d);
    const uint32_t g = gen_.load(std::memory_order_acquire);
    lk.unlock();
    const bool notified = pollUntil(g, deadline);
    lk.lock();
    return notified;
  }
  template <typename Lock, typename Rep, typename Period, typename Pred>
  bool wait_for(Lock& lk, const std::chrono::duration<Rep, Period>& d, Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d);
    while (!pred()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return pred();
      wait_for(lk, deadline - now);
    }
    return true;
  }

 private:
  bool pollUntil(uint32_t g, std::chrono::steady_clock::time_point deadline) {
    // Same pause -> yield -> sleep schedule as before; LL_SPIN_STATS counts it (JD t40u, instrumentation).
    const bool st = spinstats::enabled();
    const uint64_t t0 = st ? spinstats::nowNs() : 0;
    uint64_t p = 0, y = 0, s = 0;
    bool notified;
    for (unsigned i = 0;; ++i) {
      if (gen_.load(std::memory_order_acquire) != g) { notified = true; break; }
      if ((i & 63u) == 63u && std::chrono::steady_clock::now() >= deadline) { notified = false; break; }
      if (i < 256) { LL_ATOMIC_COND_PAUSE(); ++p; }
      else if (i < 1024) { std::this_thread::yield(); ++y; }
      else { std::this_thread::sleep_for(std::chrono::microseconds(200)); ++s; }
    }
    if (st) spinstats::record(spinstats::kCondWait, spinstats::nowNs() - t0, p, y, s);
    return notified;
  }
  std::atomic<uint32_t> gen_{0};
};

}  // namespace litholattice
