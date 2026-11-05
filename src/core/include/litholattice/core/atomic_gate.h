// Archie Phase 3: non-reentrant atomic gate (BasicLockable). No std::mutex.
// PlayL JD: never block-wait (no atomic::wait / notify); contended lock spins.
#pragma once
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
    for (;;) {
      bool expected = false;
      if (busy_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
        return;
      LL_ATOMIC_GATE_PAUSE();  // spin; never busy_.wait
    }
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
