// Lock-free per-thread readout (JD t3572u, 2026-10-31; switch LL_THREAD_READOUT=1, default off).
// Relaxed atomic counters per pool worker, written only by that worker; read-only for the sim,
// so no hash can change. dump() logs busy %, idle, jobs, worst job, steals and max queue depth.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "litholattice/core/log.h"

namespace litholattice {

class cThreadReadout {
 public:
  static bool enabled() {
    static const bool on = [] { const char* e = std::getenv("LL_THREAD_READOUT"); return e && e[0] == '1'; }();
    return on;
  }
  static uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  explicit cThreadReadout(unsigned n) : n_(n), s_(std::make_unique<Slot[]>(n)) { t0_ = nowNs(); }

  void job(unsigned i, uint64_t ns) {
    Slot& s = s_[i];
    s.busy.fetch_add(ns, std::memory_order_relaxed);
    s.jobs.fetch_add(1, std::memory_order_relaxed);
    if (ns > s.worst.load(std::memory_order_relaxed)) s.worst.store(ns, std::memory_order_relaxed);
  }
  void idle(unsigned i, uint64_t ns) { s_[i].idle.fetch_add(ns, std::memory_order_relaxed); }
  void steal(unsigned i) { s_[i].steals.fetch_add(1, std::memory_order_relaxed); }
  void depth(int d) {
    int m = maxDepth_.load(std::memory_order_relaxed);
    while (d > m && !maxDepth_.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
  }
  /// Worker 0 calls this each loop; logs at most every 5 s (one CAS winner).
  void maybeDump(const char* who) {
    const uint64_t now = nowNs(), last = lastDump_.load(std::memory_order_relaxed);
    if (now - last < 5000000000ull) return;
    uint64_t exp = last;
    if (!lastDump_.compare_exchange_strong(exp, now, std::memory_order_relaxed)) return;
    dump(who, now);
  }
  void dump(const char* who, uint64_t now) {
    const double wall = double(now - t0_) / 1e9;
    std::string line = std::string("readout ") + who + ": wall " + std::to_string(int(wall)) + "s maxQueue " +
                       std::to_string(maxDepth_.load(std::memory_order_relaxed)) + " |";
    for (unsigned i = 0; i < n_; ++i) {
      const Slot& s = s_[i];
      char b[160];
      std::snprintf(b, sizeof b, " w%u busy %.1f%% idle %.1fs jobs %llu worst %.2fms steals %llu |", i,
                    100.0 * double(s.busy.load(std::memory_order_relaxed)) / 1e9 / std::max(wall, 1e-9),
                    double(s.idle.load(std::memory_order_relaxed)) / 1e9, (unsigned long long)s.jobs.load(std::memory_order_relaxed),
                    double(s.worst.load(std::memory_order_relaxed)) / 1e6, (unsigned long long)s.steals.load(std::memory_order_relaxed));
      line += b;
    }
    logInfo("%s", line.c_str());
  }

 private:
  struct alignas(64) Slot {
    std::atomic<uint64_t> busy{0}, idle{0}, jobs{0}, worst{0}, steals{0};
  };
  unsigned n_;
  std::unique_ptr<Slot[]> s_;
  uint64_t t0_ = 0;
  std::atomic<int> maxDepth_{0};
  std::atomic<uint64_t> lastDump_{0};
};

}  // namespace litholattice