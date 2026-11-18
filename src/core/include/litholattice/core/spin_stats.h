#pragma once
// Instrumentation only (JD t40u, 2026-11-16): per-thread spin / yield / sleep / wait
// counters for the software render handoff and the spin gates, so a profile can say which
// thread waits where. LL_SPIN_STATS=1 (or LL_LOCK_TIMING=1) turns it on, read once. Off,
// every hook is one cached-bool branch, and the gate hooks sit only on the contended path.
// Each thread writes only its own 64-byte slot with relaxed load+store (no read-modify-write,
// no shared hot counter); the main thread reads every slot for the summary line. No mutex,
// no gate, no behavior change: lock order, scope and wait strategy are untouched.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace litholattice::spinstats {

enum Site : uint8_t {
  kAtomicGate = 0,  // cAtomicGate::lock, contended path
  kBlockGate,       // cBlockGate::lock, contended path
  kCondWait,        // cAtomicCondition poll (pause -> yield -> sleep)
  kSwSubmitWait,    // main: SwRenderWorker::submit waiting for the previous frame
  kSwExecIdle,      // executor: waiting for the next detached frame
  kSwFrameSlot,     // caller: waiting for the band pool's frame slot
  kSwBandIdle,      // band worker: waiting for the next band job
  kSiteCount
};

inline const char* siteName(int s) {
  static const char* const k[kSiteCount] = {"gate", "block-gate", "cond", "sw-submit-wait",
                                            "sw-exec-idle", "sw-frame-slot", "sw-band-idle"};
  return (s >= 0 && s < kSiteCount) ? k[s] : "?";
}

inline bool enabled() {
  static const bool e = [] {
    auto on = [](const char* k) { const char* v = std::getenv(k); return v && v[0] == '1'; };
    return on("LL_SPIN_STATS") || on("LL_LOCK_TIMING");
  }();
  return e;
}

inline uint64_t nowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline constexpr int kMaxSlots = 64;
inline constexpr int kNameWords = 4;  // 32 bytes of thread name

struct alignas(64) Slot {
  std::atomic<uint32_t> used{0};
  std::atomic<uint32_t> osTid{0};
  std::atomic<uint64_t> name[kNameWords] = {};
  std::atomic<uint64_t> waits[kSiteCount] = {};
  std::atomic<uint64_t> waitNs[kSiteCount] = {};
  std::atomic<uint64_t> spins[kSiteCount] = {};
  std::atomic<uint64_t> yields[kSiteCount] = {};
  std::atomic<uint64_t> sleeps[kSiteCount] = {};
};

inline Slot g_slots[kMaxSlots];
inline std::atomic<int> g_next{0};

/// The calling thread's slot (claimed once per thread with one fetch_add), or nullptr when full.
inline Slot* mine() {
  thread_local Slot* s = [] {
    const int i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSlots) return static_cast<Slot*>(nullptr);
    g_slots[i].used.store(1, std::memory_order_release);
    return &g_slots[i];
  }();
  return s;
}

inline void bump(std::atomic<uint64_t>& a, uint64_t d = 1) {
  a.store(a.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);  // owner-only writer
}

/// Remember the thread's name (and OS id) for the summary. Called by setCurrentThreadName.
inline void setThreadName(const char* n, uint32_t osTid) {
  if (!enabled() || !n) return;
  Slot* s = mine();
  if (!s) return;
  char buf[kNameWords * 8] = {};
  for (int i = 0; i < kNameWords * 8 - 1 && n[i]; ++i) buf[i] = n[i];
  for (int w = 0; w < kNameWords; ++w) {
    uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v |= uint64_t(uint8_t(buf[w * 8 + b])) << (8 * b);
    s->name[w].store(v, std::memory_order_relaxed);
  }
  s->osTid.store(osTid, std::memory_order_release);
}

/// One finished wait at `site`: how long, and how many pauses / yields / sleeps it took.
inline void record(Site site, uint64_t ns, uint64_t spins, uint64_t yields = 0, uint64_t sleeps = 0) {
  Slot* s = mine();
  if (!s) return;
  bump(s->waits[site]);
  bump(s->waitNs[site], ns);
  if (spins) bump(s->spins[site], spins);
  if (yields) bump(s->yields[site], yields);
  if (sleeps) bump(s->sleeps[site], sleeps);
}

/// RAII wall-time wait counter for a blocking call (atomic::wait, condition wait).
struct WaitScope {
  explicit WaitScope(Site s) : site(s), on(enabled()), t0(on ? nowNs() : 0) {}
  ~WaitScope() { if (on) record(site, nowNs() - t0, 0); }
  WaitScope(const WaitScope&) = delete;
  WaitScope& operator=(const WaitScope&) = delete;
  Site site;
  bool on;
  uint64_t t0;
};

/// Main thread, once per frame: every 120 frames logs per-thread deltas ("spin stats:").
/// Defined in lock_timing.cpp (core). No-op when off.
void frameEnd();

}  // namespace litholattice::spinstats
