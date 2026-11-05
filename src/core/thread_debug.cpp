// Debug sink for LITHOLATTICE_THREAD_DEBUG (see thread_debug.h). Compiles to almost
// nothing when the flag is off.
#include "litholattice/core/thread_debug.h"

#ifdef LITHOLATTICE_THREAD_DEBUG

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <algorithm>

#if defined(__linux__)
#include <pthread.h>
#include "litholattice/core/atomic_gate.h"
#endif

namespace litholattice {
namespace detail {

namespace {
// The debug-log lock: L3 in the lock order, the absolute leaf. Taken to emit one
// line and released immediately; no other lock is ever acquired while holding it.
litholattice::cAtomicGate g_threadLogMutex;

// B8: per-thread lock-free trace rings. Each thread owns one ring (single writer); the
// registry is a fixed array claimed with fetch_add, so logging never takes a lock. Rings are
// never freed (a dump may read a ring of a thread that already exited).
struct TraceRing {
  static constexpr uint32_t kLines = 256, kLen = 200;
  std::atomic<uint32_t> head{0};
  char who[32]{};
  char lines[kLines][kLen]{};
};
constexpr uint32_t kMaxRings = 256;
std::atomic<TraceRing*> g_rings[kMaxRings];
std::atomic<uint32_t> g_ringCount{0};
TraceRing* myRing(const char* who) {
  thread_local TraceRing* r = nullptr;
  if (!r) {
    r = new TraceRing;
    std::snprintf(r->who, sizeof(r->who), "%s", who);
    const uint32_t i = g_ringCount.fetch_add(1, std::memory_order_acq_rel);
    if (i < kMaxRings) g_rings[i].store(r, std::memory_order_release);
  }
  return r;
}
bool echoStderr() {
  static const bool on = [] { const char* e = std::getenv("LITHO_THREAD_LOG_STDERR"); return e && *e == '1'; }();
  return on;
}
}  // namespace

uint64_t threadDebugNowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t threadDebugNextJobId() {
  static std::atomic<uint64_t> nextId{1};
  return nextId.fetch_add(1, std::memory_order_relaxed);
}

void threadLog(const char* tag, const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char who[32];
#if defined(__linux__)
  if (pthread_getname_np(pthread_self(), who, sizeof(who)) != 0)
    std::snprintf(who, sizeof(who), "tid-%lu", (unsigned long)pthread_self());
#else
  std::snprintf(who, sizeof(who), "tid-%zu",
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif

  TraceRing* r = myRing(who);
  const uint32_t h = r->head.load(std::memory_order_relaxed);
  std::snprintf(r->lines[h % TraceRing::kLines], TraceRing::kLen, "%llu [%s] %s", (unsigned long long)threadDebugNowUs(), tag, msg);
  r->head.store(h + 1, std::memory_order_release);
  if (!echoStderr()) return;
  litholattice::cGateGuard<litholattice::cAtomicGate> l(g_threadLogMutex);  // L3 leaf
  std::fprintf(stderr, "[litho-thread][%s][%s] %s\n", tag, who, msg);
  std::fflush(stderr);
}

void threadDebugDump(const char* why) {
  litholattice::cGateGuard<litholattice::cAtomicGate> l(g_threadLogMutex);  // L3 leaf
  std::fprintf(stderr, "[litho-thread] ===== DUMP: %s =====\n", why);
  const uint32_t n = std::min<uint32_t>(g_ringCount.load(std::memory_order_acquire), kMaxRings);
  for (uint32_t i = 0; i < n; ++i) {
    const TraceRing* r = g_rings[i].load(std::memory_order_acquire);
    if (!r) continue;
    const uint32_t h = r->head.load(std::memory_order_acquire), from = h > 12 ? h - 12 : 0;  // last 12 lines per thread
    std::fprintf(stderr, "[litho-thread] --- %s (%u lines) ---\n", r->who, h);
    for (uint32_t k = from; k < h; ++k) std::fprintf(stderr, "[litho-thread]   %s\n", r->lines[k % TraceRing::kLines]);
  }
  std::fprintf(stderr, "[litho-thread] ===== END DUMP =====\n");
  std::fflush(stderr);
}

}  // namespace detail
}  // namespace litholattice

#endif  // LITHOLATTICE_THREAD_DEBUG
