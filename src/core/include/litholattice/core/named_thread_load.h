// Lock-free named-thread load readout (JD t3584u, 2026-10-31; switch LL_THREAD_READOUT=1, default off).
// One fixed slot per named thread (tick, main, control). Each slot has exactly ONE writer, the thread
// it names; readers (the periodic dump) only load relaxed atomics, so no lock and no hash effect.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace litholattice {

class cNamedThreadLoad {
 public:
  enum Slot : unsigned { kTick = 0, kMain = 1, kControl = 2, kSlots = 3 };
  static cNamedThreadLoad& instance() { static cNamedThreadLoad s; return s; }
  /// Owning thread only: one pass = time waiting to enter its window + time working inside it.
  void pass(Slot s, uint64_t waitNs, uint64_t busyNs) {
    Load& l = l_[s];
    l.passes.fetch_add(1, std::memory_order_relaxed);
    l.wait.fetch_add(waitNs, std::memory_order_relaxed);
    l.busy.fetch_add(busyNs, std::memory_order_relaxed);
    if (waitNs > l.worstWait.load(std::memory_order_relaxed)) l.worstWait.store(waitNs, std::memory_order_relaxed);
    if (busyNs > l.worstBusy.load(std::memory_order_relaxed)) l.worstBusy.store(busyNs, std::memory_order_relaxed);
  }
  /// Read-only summary, e.g. " tick busy 31.0% wait 0.2% passes 1234 worstBusy 44.50ms worstWait 0.10ms |".
  std::string summary(double wallSec) const {
    static const char* kName[kSlots] = {"tick", "main", "control"};
    std::string out;
    for (unsigned i = 0; i < kSlots; ++i) {
      const Load& l = l_[i];
      const uint64_t p = l.passes.load(std::memory_order_relaxed);
      if (p == 0) continue;
      char b[200];
      std::snprintf(b, sizeof b, " %s busy %.1f%% wait %.2f%% passes %llu worstBusy %.2fms worstWait %.2fms |", kName[i],
                    100.0 * double(l.busy.load(std::memory_order_relaxed)) / 1e9 / std::max(wallSec, 1e-9),
                    100.0 * double(l.wait.load(std::memory_order_relaxed)) / 1e9 / std::max(wallSec, 1e-9), (unsigned long long)p,
                    double(l.worstBusy.load(std::memory_order_relaxed)) / 1e6, double(l.worstWait.load(std::memory_order_relaxed)) / 1e6);
      out += b;
    }
    return out;
  }

 private:
  struct alignas(64) Load {
    std::atomic<uint64_t> passes{0}, wait{0}, busy{0}, worstWait{0}, worstBusy{0};
  };
  Load l_[kSlots];
};

}  // namespace litholattice