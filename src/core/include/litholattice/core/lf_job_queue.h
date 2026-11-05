// cLfJobQueue (JD 2026-11-01, switch LL_LF_JOBS=1, default off): a job queue on a bounded
// MPMC ring (cLfJobRing) with its own workers. Atomics only: no mutex, no condition
// variable, no atomic wait. Idle workers back off pause-spin -> yield -> 100 us sleep.
// cJobPool stays the default. A caller routes here only when LL_LF_JOBS=1 AND its own
// switch is on: LL_LF_WORLDBUILD (dimension build), LL_LF_GEN (fresh chunk generation; a
// saved world only with the lock-free I/O path LL_LF_SAVE / LL_LF_LOAD), LL_LF_UPLOAD (mesh
// upload jobs) and LL_LF_INGRESS.
//
// Thread count: never more than the hardware threads. shared() reads the census budget
// (immutable, no census mutex) and claims workers = min(pool budget, hw - (pool + main +
// sim-clock threads)) from a process-wide atomic counter (CAS), floor 0. With 0 workers
// every job runs inline on the submitting thread (still correct, just serial).
// A full ring never blocks and never drops: the submitter runs the job inline.
// wait helpers: callers wait on their own atomic counter and help by running queued jobs,
// so a wait can never deadlock even with 0 workers.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#include "litholattice/core/lf_job_ring.h"
#include "litholattice/core/log.h"
#include "litholattice/core/thread_census.h"

namespace litholattice {

class cLfJobQueue {
 public:
  using Job = std::function<void()>*;

  static bool enabled() {
    static const bool on = true;  /* Archie PlayI: always ON (no legacy job_system mutex path) */
    return on;
  }

  /// `workers` is clamped to hardware_concurrency() (and to >= 0).
  explicit cLfJobQueue(unsigned workers, size_t capacity = 4096) : ring_(capacity) {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    workers = std::min(workers, hw);
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
  }
  ~cLfJobQueue() { stop(0); }

  /// PlayM (JD 10:30) stop step: close intake (later submits run inline on the caller), drain
  /// the ring by helping until idle or `drainMs` passes (0 = until idle), then stop and join
  /// the workers and run any job still queued inline: nothing is dropped. Idempotent, atomics
  /// only. Returns true when the drain finished inside the deadline.
  bool stop(long drainMs) {
    if (closed_.exchange(true, std::memory_order_acq_rel)) return true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(drainMs);
    unsigned spins = 0;
    bool inTime = true;
    while (inFlight_.load(std::memory_order_acquire) != 0) {
      if (helpOne()) { spins = 0; continue; }
      if (drainMs > 0 && std::chrono::steady_clock::now() >= end) { inTime = false; break; }
      backoff(spins);
    }
    stop_.store(true, std::memory_order_release);
    for (auto& t : threads_) if (t.joinable()) t.join();
    Job j = nullptr;
    while (ring_.tryPop(j)) exec(j);
    if (!inTime) logWarn("LL_LF_JOBS stop: drain passed %ld ms; workers joined, rest ran inline", drainMs);
    return inTime;
  }
  /// Stop step for the process queue at final exit (no-op if it was never created).
  static void stopShared(long drainMs) { if (sharedPtr()) sharedPtr()->stop(drainMs); }
  bool stopped() const { return closed_.load(std::memory_order_acquire); }
  cLfJobQueue(const cLfJobQueue&) = delete;
  cLfJobQueue& operator=(const cLfJobQueue&) = delete;

  /// The process queue, created on first use. Sized so the process never runs more threads
  /// than the hardware has (see header comment).
  static cLfJobQueue& shared() {
    static cLfJobQueue* q = [] {
      auto* p = makeShared();
      sharedPtr() = p;
      return p;
    }();
    return *q;
  }

 private:
  static cLfJobQueue*& sharedPtr() { static cLfJobQueue* p = nullptr; return p; }
  static cLfJobQueue* makeShared() {
    {
      // 2026-11-01: no census mutex on this path. budget() is an immutable read; the room left
      // after cJobPool's workers and the main + sim-clock threads is claimed from a plain
      // atomic counter (CAS), so the process never exceeds the hardware threads.
      const ThreadBudget& bd = cThreadCensus::instance().budget();
      const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
      const unsigned used = bd.pool + bd.mainThreads + bd.simClock;
      const unsigned room = used >= hw ? 0u : std::min(hw - used, bd.pool);
      unsigned n = 0;
      unsigned cur = lfWorkersClaimed().load(std::memory_order_relaxed);
      do { n = cur >= room ? 0u : room - cur; } while (n && !lfWorkersClaimed().compare_exchange_weak(cur, cur + n, std::memory_order_acq_rel));
      auto* p = new cLfJobQueue(n);  // never destroyed (like cJobPool::shared)
      logInfo("threads: LL_LF_JOBS queue up: %u workers (hw %u, %u reserved for pool/main/clock); MPMC ring, atomics only", n, hw, used);
      return p;
    }
  }

 public:

  unsigned workers() const { return unsigned(threads_.size()); }
  /// LF workers claimed process-wide by shared() (plain atomic counter, no census lock).
  static std::atomic<unsigned>& lfWorkersClaimed() { static std::atomic<unsigned> n{0}; return n; }

  void submit(std::function<void()> f) {
    Job j = new std::function<void()>(std::move(f));
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    if (threads_.empty() || closed_.load(std::memory_order_acquire) || !ring_.tryPush(j)) exec(j);  // stopped too: inline  // no workers / ring full: run inline
  }
  /// Run one queued job on this thread. False when the ring was empty.
  bool helpOne() {
    Job j = nullptr;
    if (!ring_.tryPop(j)) return false;
    exec(j);
    return true;
  }
  /// Wait until `pending` reaches 0, running queued jobs meanwhile.
  void helpUntilZero(const std::atomic<int>& pending) {
    unsigned spins = 0;
    while (pending.load(std::memory_order_acquire) != 0) {
      if (helpOne()) { spins = 0; continue; }
      backoff(spins);
    }
  }
  /// Wait until every submitted job has finished, helping meanwhile.
  void waitIdle() { helpUntilZero(inFlight_); }

  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }
  uint64_t executed() const { return executed_.load(std::memory_order_relaxed); }

  static void backoff(unsigned& spins) {
    ++spins;
    if (spins < 64) { relax(); return; }
    if (spins < 256) { std::this_thread::yield(); return; }
    std::this_thread::sleep_for(std::chrono::microseconds(100));  // long idle: stop burning a core
  }

 private:
  static void relax() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#endif
  }
  void run() {
    unsigned spins = 0;
    for (;;) {
      if (helpOne()) { spins = 0; continue; }
      if (stop_.load(std::memory_order_acquire)) return;
      backoff(spins);
    }
  }
  void exec(Job j) {
    try {
      (*j)();
    } catch (const std::exception& e) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("LL_LF_JOBS: job threw: %s", e.what());
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("LL_LF_JOBS: job threw a non-std exception");
    }
    delete j;
    executed_.fetch_add(1, std::memory_order_relaxed);
    inFlight_.fetch_sub(1, std::memory_order_acq_rel);
  }

  cLfJobRing<Job> ring_;
  std::vector<std::thread> threads_;
  alignas(64) std::atomic<int> inFlight_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> closed_{false};  // PlayM: intake closed by stop()
  std::atomic<uint64_t> failed_{0}, executed_{0};
};

}  // namespace litholattice
