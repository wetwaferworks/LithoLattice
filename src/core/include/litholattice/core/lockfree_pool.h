// Lock-free on-demand pool (JD t3557u / t3560u, 2026-10-30; switch LL_POOL_LOCKFREE=1).
//
// JD's target layout: three permanent threads (main game loop, tick loop, control+event)
// and ONE on-demand pool on every remaining hardware thread for scripting, render,
// gen/mesh and other per-request engine work. This pool has no mutex and no condition
// variable anywhere:
//   urgent      cUrgentRing (atomic sequence counters), taken first by every worker;
//   normal      per-worker cChaseLevDeque; a worker's own submits push its deque bottom,
//               external submits go to a worker's cMpscQueue injection queue (round
//               robin); idle workers steal deque tops and claim other injection queues;
//   background  one cMpscQueue, admitted while fewer than lendCap() slices run (CAS).
// Idle workers pause-spin, then back off (yield, short sleeps; t46u 5b: no atomic::wait)
// re-reading an atomic generation word that every submit bumps. waitIdle() re-reads the
// in-flight atomic the same way. Jobs are heap-allocated std::function (Job*), one owner at a time.
// cJobPool (threading.h) forwards to this when LL_POOL_LOCKFREE=1.
#pragma once
#include "litholattice/core/thread_name.h"
#include "litholattice/core/idle_backoff.h"  // t46u item 5b

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <system_error>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <immintrin.h>
#endif

#include "litholattice/core/chase_lev.h"
#include "litholattice/core/log.h"
#include "litholattice/core/mpsc_queue.h"
#include "litholattice/core/thread_readout.h"
#include "litholattice/core/urgent_ring.h"

namespace litholattice {

class cLockFreePool {
 public:
  using Job = std::function<void()>*;

  static bool enabled() {
    static const bool on = true;  /* Archie PlayI: always ON (no mutex pool fallback) */
    return on;
  }

  cLockFreePool(unsigned n, void (*threadInit)()) {
    // n==0: no worker threads. submit() runs on the caller so a 0-reserve shared
    // pool under LL_HS_LAYOUT=original cannot hang or do `% 0`. (The old floor of 1
    // would spend a render-side thread we do not have on 4-core machines.)
    if (n == 0) {
      lendCap_.store(0, std::memory_order_relaxed);
      return;
    }
    lendCap_.store(n, std::memory_order_relaxed);
    if (cThreadReadout::enabled()) readout_ = std::make_unique<cThreadReadout>(n);  // LL_THREAD_READOUT
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) workers_.push_back(std::make_unique<Worker>());
    try {
      for (unsigned i = 0; i < n; ++i)
        workers_[i]->thread = std::thread([this, i, threadInit] { char nm[16]; std::snprintf(nm, sizeof nm, "lk-lfpool-%u", unsigned(i)); ::litholattice::setCurrentThreadName(nm); if (threadInit) threadInit(); run(i); });
    } catch (const std::system_error& e) {
      logInfo("job pool: worker spawn failed (%s); running jobs inline on the caller", e.what());
      stop_.store(true, std::memory_order_release);
      wake(true);
      for (auto& w : workers_) if (w && w->thread.joinable()) w->thread.join();
      workers_.clear();
      lendCap_.store(0, std::memory_order_relaxed);
      stop_.store(false, std::memory_order_release);
    }
  }
  ~cLockFreePool() {
    stop_.store(true, std::memory_order_release);
    wake(true);
    for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
  }
  cLockFreePool(const cLockFreePool&) = delete;
  cLockFreePool& operator=(const cLockFreePool&) = delete;

  unsigned size() const { return unsigned(workers_.size()); }

  void runInline(std::function<void()> f) {
    try { f(); } catch (...) { failed_.fetch_add(1, std::memory_order_relaxed); throw; }
  }
  void submit(std::function<void()> f) {
    if (workers_.empty()) { runInline(std::move(f)); return; }
    Job j = new std::function<void()>(std::move(f));
    { const int d = inFlight_.fetch_add(1, std::memory_order_acq_rel) + 1; if (readout_) readout_->depth(d); }
    pushNormal(j);
    wake(false);
  }
  void submitUrgent(std::function<void()> f) {
    if (workers_.empty()) { runInline(std::move(f)); return; }
    Job j = new std::function<void()>(std::move(f));
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    if (readout_) readout_->depth(cThreadReadout::kUrgent, int(urgent_.pending()) + 1);
    if (!urgent_.tryPush(j)) pushNormal(j);  // ring full: spill (never drop, never block)
    wake(false);
  }
  void submitBackground(std::function<void()> f) {
    if (workers_.empty()) { runInline(std::move(f)); return; }
    Job j = new std::function<void()>(std::move(f));
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    { const int d = int(bgCount_.fetch_add(1, std::memory_order_acq_rel)) + 1; if (readout_) readout_->depth(cThreadReadout::kBackground, d); }
    bg_.push(std::move(j));
    wake(false);
  }
  /// Gate 1: true on a worker thread of ANY cLockFreePool (fork-join callers run inline there).
  static bool onAnyWorker() { return tlsOwner() != nullptr && selfIndex() >= 0; }
  bool urgentPending() const { return urgent_.pending() > 0; }
  void setLendCap(unsigned c) {
    if (workers_.empty()) { lendCap_.store(0, std::memory_order_release); return; }
    lendCap_.store(std::max(1u, c), std::memory_order_release);
    wake(true);
  }
  unsigned lendCap() const { return lendCap_.load(std::memory_order_acquire); }
  void waitIdle() const {
    for (int v = inFlight_.load(std::memory_order_acquire); v != 0; v = inFlight_.load(std::memory_order_acquire))
      lkIdleWaitUntil([&] { return inFlight_.load(std::memory_order_acquire) != v; });  // t46u 5b
  }
  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }
  uint64_t steals() const { return steals_.load(std::memory_order_relaxed); }

 private:
  struct Worker {
    cChaseLevDeque<Job, 4096> deque;
    cMpscQueue<Job> inject;
    std::thread thread;
  };
  static int& selfIndex() { static thread_local int i = -1; return i; }

  static void relax() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    _mm_pause();
#endif
  }
  bool onWorker() const { return tlsOwner() == this && selfIndex() >= 0; }
  static const cLockFreePool*& tlsOwner() { static thread_local const cLockFreePool* p = nullptr; return p; }

  void pushNormal(Job j) {
    if (onWorker()) {
      Job tmp = j;
      if (workers_[size_t(selfIndex())]->deque.pushBottom(std::move(tmp))) return;
    }
    const size_t k = rr_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
    workers_[k]->inject.push(std::move(j));
  }
  void wake(bool all) {
    gen_.fetch_add(1, std::memory_order_acq_rel);  // idle workers re-read it (t46u 5b: no notify)
    (void)all;
  }
  bool takeInject(Worker& w, Job& out) {
    if (!w.inject.maybeNonEmpty() || !w.inject.tryClaim()) return false;
    const bool got = w.inject.pop(out);
    w.inject.release();
    return got;
  }
  bool takeBackground(Job& out) {
    if (bgCount_.load(std::memory_order_acquire) <= 0) return false;
    int a = bgActive_.load(std::memory_order_acquire);
    do { if (a >= int(lendCap_.load(std::memory_order_acquire))) return false; }
    while (!bgActive_.compare_exchange_weak(a, a + 1, std::memory_order_acq_rel));
    bool got = false;
    if (bg_.tryClaim()) { got = bg_.pop(out); bg_.release(); }
    if (got) bgCount_.fetch_sub(1, std::memory_order_acq_rel);
    else bgActive_.fetch_sub(1, std::memory_order_acq_rel);
    return got;
  }
  bool find(unsigned self, Job& out, bool& bg) {
    bg = false;
    if (urgent_.tryPop(out)) return true;
    Worker& me = *workers_[self];
    if (me.deque.popBottom(out)) return true;
    if (takeInject(me, out)) return true;
    const size_t n = workers_.size();
    for (size_t d = 1; d < n; ++d) {
      Worker& v = *workers_[(self + d) % n];
      if (v.deque.steal(out)) { steals_.fetch_add(1, std::memory_order_relaxed); if (readout_) readout_->steal(self); return true; }
      if (takeInject(v, out)) return true;
    }
    if (takeBackground(out)) { bg = true; return true; }
    return false;
  }
  void run(unsigned self) {
    selfIndex() = int(self);
    tlsOwner() = this;
    workers_[self]->deque.bindOwner();
    for (;;) {
      const uint32_t g = gen_.load(std::memory_order_acquire);
      Job j = nullptr;
      bool bg = false;
      if (readout_ && self == 0) readout_->maybeDump("pool");
      if (find(self, j, bg)) {
        if (readout_) { const uint64_t a = cThreadReadout::nowNs(); exec(j, bg); readout_->job(self, cThreadReadout::nowNs() - a); }
        else exec(j, bg);
        continue;
      }
      if (stop_.load(std::memory_order_acquire)) return;
      bool changed = false;
      for (int i = 0; i < 2048 && !changed; ++i) { relax(); changed = gen_.load(std::memory_order_acquire) != g; }
      if (changed) continue;
      sleepers_.fetch_add(1, std::memory_order_acq_rel);
      const uint64_t idle0 = readout_ ? cThreadReadout::nowNs() : 0;
      lkIdleWaitUntil([&] { return gen_.load(std::memory_order_acquire) != g || stop_.load(std::memory_order_acquire); });  // t46u 5b
      if (readout_) readout_->idle(self, cThreadReadout::nowNs() - idle0);
      sleepers_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  void exec(Job j, bool bg) {
    try {
      (*j)();
    } catch (const std::exception& e) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job pool (lock-free): job threw: %s", e.what());
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job pool (lock-free): job threw a non-std exception");
    }
    delete j;
    if (bg) { bgActive_.fetch_sub(1, std::memory_order_acq_rel); if (bgCount_.load(std::memory_order_acquire) > 0) wake(false); }
    inFlight_.fetch_sub(1, std::memory_order_acq_rel);  // waitIdle re-reads it (t46u 5b)
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  cUrgentRing<Job, 256> urgent_;
  cMpscQueue<Job> bg_;
  alignas(64) std::atomic<uint32_t> gen_{0};
  alignas(64) std::atomic<int> sleepers_{0};
  alignas(64) mutable std::atomic<int> inFlight_{0};
  std::atomic<int> bgActive_{0}, bgCount_{0};
  std::atomic<unsigned> lendCap_{1};
  alignas(64) std::atomic<size_t> rr_{0};  // t46u item 5: every submitter bumps it; keep it off the bg counters' line
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> failed_{0}, steals_{0};
  std::unique_ptr<cThreadReadout> readout_;  // LL_THREAD_READOUT=1 only; null otherwise
};

}  // namespace litholattice