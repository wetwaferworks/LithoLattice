// Archie Phase 3: non-reentrant atomic gate (BasicLockable). No std::mutex.
// PlayL JD: never block-wait (no atomic::wait / notify); contended lock spins.
#pragma once
#include "litholattice/core/spin_stats.h"
#include <atomic>
#if defined(_MSC_VER)
#  include <intrin.h>
#  define LL_ATOMIC_GATE_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#  define LL_ATOMIC_GATE_PAUSE() __builtin_ia32_pause()
#else
#  define LL_ATOMIC_GATE_PAUSE() ((void)0)
#endif

namespace litholattice {

class cAtomicGate {
 public:
  cAtomicGate() = default;
  cAtomicGate(const cAtomicGate&) = delete;
  cAtomicGate& operator=(const cAtomicGate&) = delete;

  void lock() {
    bool expected = false;
    if (busy_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
      return;
    lockContended();
  }

  bool try_lock() noexcept {
    bool expected = false;
    return busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    busy_.store(false, std::memory_order_release);
    // no notify: waiters spin
  }

 private:
  // Same CAS + pause sequence as before; LL_SPIN_STATS only counts it (JD t40u, instrumentation).
  void lockContended() {
    const bool st = spinstats::enabled();
    const uint64_t t0 = st ? spinstats::nowNs() : 0;
    uint64_t spins = 0;
    for (;;) {
      LL_ATOMIC_GATE_PAUSE();  // spin; never busy_.wait
      ++spins;
      bool expected = false;
      if (busy_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) break;
    }
    if (st) spinstats::record(spinstats::kAtomicGate, spinstats::nowNs() - t0, spins);
  }
  std::atomic<bool> busy_{false};
};

template <typename Gate>
class cGateGuard {
 public:
  explicit cGateGuard(Gate& g) : g_(g) { g_.lock(); }
  ~cGateGuard() { g_.unlock(); }
  cGateGuard(const cGateGuard&) = delete;
  cGateGuard& operator=(const cGateGuard&) = delete;
 private:
  Gate& g_;
};

}  // namespace litholattice
