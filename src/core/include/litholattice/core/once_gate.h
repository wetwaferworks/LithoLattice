// PlayN (JD Next #1): std::call_once replacement (no <mutex>). The first caller
// runs fn; concurrent first callers spin (pause/yield) until it is done.
// Re-entry from inside fn on the same gate is a caller bug (as with call_once).
#pragma once
#include <atomic>
#include <thread>

namespace litholattice {

class cOnceGate {
 public:
  template <typename Fn>
  void call(Fn&& fn) {
    if (state_.load(std::memory_order_acquire) == 2) return;
    int expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      fn();
      state_.store(2, std::memory_order_release);
      return;
    }
    while (state_.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  }

 private:
  std::atomic<int> state_{0};
};

}  // namespace litholattice
