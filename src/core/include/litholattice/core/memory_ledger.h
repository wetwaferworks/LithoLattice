// Gate 0 (Archie Q0-Q17 rulings, 2026-10-31): lock-free memory readout counters.
// Process-wide relaxed atomic byte/count totals per category with a CAS-max peak. Writers are any
// thread (allocation sites); readers only log. No lock, no allocation, no effect on sim state, so
// no hash can change. Readout: LL_MEM_READOUT=1 logs a "mem ledger:" line each perf second; the
// bench JSON always carries "memory_ledger".
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
namespace litholattice {
class cMemoryLedger {
 public:
  enum Category : unsigned { kVkDeviceBytes = 0, kGlTboBytes, kChunkColumns, kCount };
  static const char* name(Category c) {
    static const char* const n[kCount] = {"vk_device_bytes", "gl_tbo_bytes", "chunk_columns"};
    return c < kCount ? n[c] : "?";
  }
  static bool enabled() {
    static const bool on = [] { const char* e = std::getenv("LL_MEM_READOUT"); return e && e[0] == '1'; }();
    return on;
  }
  static void add(Category c, int64_t n) {
    Slot& s = slots()[c];
    const int64_t now = s.cur.fetch_add(n, std::memory_order_relaxed) + n;
    int64_t p = s.peak.load(std::memory_order_relaxed);
    while (now > p && !s.peak.compare_exchange_weak(p, now, std::memory_order_relaxed)) {}
  }
  static void sub(Category c, int64_t n) { slots()[c].cur.fetch_sub(n, std::memory_order_relaxed); }
  static int64_t current(Category c) { return slots()[c].cur.load(std::memory_order_relaxed); }
  static int64_t peak(Category c) { return slots()[c].peak.load(std::memory_order_relaxed); }
  /// One-line text readout (MiB for byte categories, plain count otherwise).
  static std::string line() {
    std::string out; char b[96];
    for (unsigned i = 0; i < kCount; ++i) {
      const Category c = Category(i);
      if (c == kChunkColumns) std::snprintf(b, sizeof b, "%s%s=%lld (peak %lld)", i ? " " : "", name(c), (long long)current(c), (long long)peak(c));
      else std::snprintf(b, sizeof b, "%s%s=%.1fMiB (peak %.1f)", i ? " " : "", name(c), current(c) / 1048576.0, peak(c) / 1048576.0);
      out += b;
    }
    return out;
  }
  /// JSON object {"name": {"cur": n, "peak": n}, ...} (raw units).
  static std::string json() {
    std::string out = "{"; char b[128];
    for (unsigned i = 0; i < kCount; ++i) {
      std::snprintf(b, sizeof b, "%s\"%s\": {\"cur\": %lld, \"peak\": %lld}", i ? ", " : "", name(Category(i)),
                    (long long)current(Category(i)), (long long)peak(Category(i)));
      out += b;
    }
    return out + "}";
  }

 private:
  struct alignas(64) Slot { std::atomic<int64_t> cur{0}, peak{0}; };
  static Slot* slots() { static Slot s[kCount]; return s; }
};
}  // namespace litholattice
