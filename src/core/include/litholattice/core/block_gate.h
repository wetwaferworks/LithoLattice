// Archie Phase 3 PlayI: reentrant block gate (atomics). Drop-in BasicLockable
// for blockMutex_: same reentrancy as recursive_mutex (sim holds across fn that may getBlock).
// No std::mutex. PlayL JD: never block-wait (no atomic::wait / notify); contended
// lock spins with pause. Threads never park on each other ("cruise").
// Owner token is a non-zero uint32_t thread id (MSVC wait/notify on thread::id
// was not reliably integral; failed re-enter while sim held the gate deadlocked).
#pragma once
#include "litholattice/core/spin_stats.h"
#include <atomic>
#include <cstdint>
#if defined(__wasm__)
#include <lithoweave.h>  // lw_current_tid (web build only)
#endif
#if defined(_MSC_VER)
#  include <intrin.h>
#  define LL_BLOCK_GATE_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#  define LL_BLOCK_GATE_PAUSE() __builtin_ia32_pause()
#else
#  define LL_BLOCK_GATE_PAUSE() ((void)0)
#endif
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#else
#  include <pthread.h>
#endif

namespace litholattice {

class cBlockGate {
 public:
  cBlockGate() = default;
  cBlockGate(const cBlockGate&) = delete;
  cBlockGate& operator=(const cBlockGate&) = delete;

  void lock() {
    const uint32_t self = selfToken();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ++depth_;
      return;
    }
    uint32_t expected = 0;
    if (owner_.compare_exchange_weak(expected, self, std::memory_order_acquire, std::memory_order_relaxed)) {
      depth_ = 1;
      return;
    }
    // Same CAS + pause sequence as before; LL_SPIN_STATS only counts it (JD t40u, instrumentation).
    const bool st = spinstats::enabled();
    const uint64_t t0 = st ? spinstats::nowNs() : 0;
    uint64_t spins = 0;
    for (;;) {
      LL_BLOCK_GATE_PAUSE();  // spin; never owner_.wait
      ++spins;
      expected = 0;
      if (owner_.compare_exchange_weak(expected, self, std::memory_order_acquire, std::memory_order_relaxed)) break;
    }
    depth_ = 1;
    if (st) spinstats::record(spinstats::kBlockGate, spinstats::nowNs() - t0, spins);
  }

  bool try_lock() noexcept {
    const uint32_t self = selfToken();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ++depth_;
      return true;
    }
    uint32_t expected = 0;
    if (!owner_.compare_exchange_strong(expected, self, std::memory_order_acquire, std::memory_order_relaxed))
      return false;
    depth_ = 1;
    return true;
  }

  /// t46u item 6a: true when the calling thread holds the gate (any depth). Exact for the caller:
  /// only this thread can store its own token, so a stale read can never claim a false yes.
  bool heldByMe() const noexcept { return owner_.load(std::memory_order_relaxed) == selfToken(); }

  void unlock() noexcept {
    if (--depth_ != 0) return;
    owner_.store(0, std::memory_order_release);
    // no notify: waiters spin
  }

 private:
  static uint32_t selfToken() noexcept {
#ifdef _WIN32
    const unsigned long tid = ::GetCurrentThreadId();
    return tid ? uint32_t(tid) : 1u;  // never 0 (0 = unlocked)
#elif defined(__wasm__)
    // 2026-11-16 (S5 web): pthread_t is LithoWeave's handle struct there, not an integer or
    // pointer, so use the weave's own thread id (positive; 0 only if attaching failed).
    const int32_t tid = lw_current_tid();
    return tid > 0 ? uint32_t(tid) : 1u;
#else
    const auto p = reinterpret_cast<uintptr_t>(pthread_self());
    uint32_t t = uint32_t(p ^ (p >> 32));
    return t ? t : 1u;
#endif
  }

  std::atomic<uint32_t> owner_{0};  // 0 = free; else owning thread token
  int depth_ = 0;                   // only touched by the owning thread
};

}  // namespace litholattice
