// Duplicate-work trace (Direction JD, 2026-10-30). See work_trace.h.
#include "litholattice/core/work_trace.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <vector>
namespace litholattice {
namespace {
struct Ev { const char* stage; int cx, sy, cz; uint64_t seq; int64_t us; };
struct Buf { unsigned idx; size_t tid; std::vector<Ev> ev; };
std::mutex g_mx;
std::vector<std::unique_ptr<Buf>> g_bufs;  // never shrinks; flushed once at exit
std::atomic<unsigned> g_next{0};
const char* path() { static const char* p = [] { const char* e = std::getenv("LL_WORK_TRACE"); return (e && *e) ? e : nullptr; }(); return p; }
const auto g_t0 = std::chrono::steady_clock::now();
Buf* mine() {
  thread_local Buf* b = nullptr;
  if (!b) {
    auto nb = std::make_unique<Buf>();
    nb->idx = g_next.fetch_add(1);
    nb->tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    nb->ev.reserve(1 << 14);
    std::lock_guard<std::mutex> l(g_mx);
    b = nb.get();
    g_bufs.push_back(std::move(nb));
  }
  return b;
}
}  // namespace
bool workTraceOn() { return path() != nullptr; }
void workTrace(const char* stage, int cx, int sy, int cz, uint64_t seq) {
  if (!path()) return;
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
  mine()->ev.push_back({stage, cx, sy, cz, seq, us});
}
void workTraceFlush() {
  const char* p = path();
  if (!p) return;
  std::lock_guard<std::mutex> l(g_mx);
  FILE* f = std::fopen(p, "w");
  if (!f) return;
  std::fprintf(f, "stage,thread,os_tid,cx,sy,cz,seq,t_us\n");
  for (const auto& b : g_bufs)
    for (const Ev& e : b->ev)
      std::fprintf(f, "%s,%u,%zx,%d,%d,%d,%llu,%lld\n", e.stage, b->idx, b->tid, e.cx, e.sy, e.cz, (unsigned long long)e.seq, (long long)e.us);
  std::fclose(f);
}
}  // namespace litholattice