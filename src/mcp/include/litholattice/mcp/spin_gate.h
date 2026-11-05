// PlayL: mcp-local spin gate (BasicLockable). Keeps litholattice_mcp free of a
// core include/link dependency. Same contract as litholattice::cAtomicGate.
#pragma once
#include <atomic>
#if defined(_MSC_VER)
#  include <intrin.h>
#  define LL_MCP_GATE_PAUSE() _mm_pause()
#elif defined(__i386__) || defined(__x86_64__)
#  define LL_MCP_GATE_PAUSE() __builtin_ia32_pause()
#else
#  define LL_MCP_GATE_PAUSE() ((void)0)
#endif

namespace litholattice::mcp {

class cSpinGate {
 public:
  cSpinGate() = default;
  cSpinGate(const cSpinGate&) = delete;
  cSpinGate& operator=(const cSpinGate&) = delete;

  void lock() {
    for (;;) {
      bool expected = false;
      if (busy_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
        return;
      LL_MCP_GATE_PAUSE();
    }
  }

  bool try_lock() noexcept {
    bool expected = false;
    return busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept { busy_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> busy_{false};
};

template <typename Gate>
class cSpinGuard {
 public:
  explicit cSpinGuard(Gate& g) : g_(g) { g_.lock(); }
  ~cSpinGuard() { g_.unlock(); }
  cSpinGuard(const cSpinGuard&) = delete;
  cSpinGuard& operator=(const cSpinGuard&) = delete;
 private:
  Gate& g_;
};

}  // namespace litholattice::mcp
