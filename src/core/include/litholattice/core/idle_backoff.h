// t46u item 5b (JD 2026-11-16): idle without parking on a peer.
// Replaces every std::atomic::wait / notify pair in the engine. A thread that
// has nothing to do re-checks its condition with a tiered backoff: a short
// pause-spin (a fork-join partner is usually microseconds away), then
// yields for up to kYieldWindow, then short sleeps (a whole idle frame or
// an idle I/O thread). Nobody blocks inside the OS on another thread's
// address, nobody has to remember to notify, and a missed wake cannot hang:
// the condition is simply read again.
#pragma once
#include <chrono>
#include <thread>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define LK_IDLE_RELAX() _mm_pause()
#else
#define LK_IDLE_RELAX() ((void)0)
#endif

namespace litholattice {

class cIdleBackoff {
 public:
  static constexpr unsigned kSpins = 64;
  static constexpr std::chrono::microseconds kYieldWindow{500};
  static constexpr std::chrono::microseconds kSleep{100};  // ~1 ms on Windows at timeBeginPeriod(1)
  void pause() {
    if (n_ < kSpins) { ++n_; LK_IDLE_RELAX(); return; }
    const auto now = std::chrono::steady_clock::now();
    if (n_ == kSpins) { t0_ = now; ++n_; }
    if (now - t0_ < kYieldWindow) { std::this_thread::yield(); return; }
    std::this_thread::sleep_for(kSleep);
  }
  void reset() { n_ = 0; }

 private:
  unsigned n_ = 0;
  std::chrono::steady_clock::time_point t0_{};
};

/// Re-check `done()` with cIdleBackoff until it returns true.
template <class Pred>
inline void lkIdleWaitUntil(Pred&& done) {
  cIdleBackoff b;
  while (!done()) b.pause();
}

}  // namespace litholattice