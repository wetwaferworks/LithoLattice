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
// Idle workers pause-spin, then sleep on an atomic generation word (std::atomic::wait);
// every submit bumps it and notifies only when a worker sleeps. waitIdle() waits on the
// in-flight atomic. Jobs are heap-allocated std::function (Job*), one owner at a time.
// cJobPool (threading.h) forwards to this when LL_POOL_LOCKFREE=1.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
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
    static const bool on = [] { const char* e = std::getenv("LL_POOL_LOCKFREE"); return e && e[0] == '1'; }();
    return on;
  }

  cLockFreePool(unsigned n, void (*threadInit)()) {
    n = std::max(1u, n);
    lendCap_.store(n, std::memory_order_relaxed);
    if (cThreadReadout::enabled()) readout_ = std::make_unique<cThreadReadout>(n);  // LL_THREAD_READOUT
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) workers_.push_back(std::make_unique<Worker>());
    for (unsigned i = 0; i < n; ++i)
      workers_[i]->thread = std::thread([this, i, threadInit] { if (threadInit) threadInit(); run(i); });
  }
  ~cLockFreePool() {
    stop_.store(true, std::memory_order_release);
    wake(true);
    for (auto& w : workers_) if (w->thread.joinable()) w->thread.join();
  }
  cLockFreePool(const cLockFreePool&) = delete;
  cLockFreePool& operator=(const cLockFreePool&) = delete;

  unsigned size() const { return unsigned(workers_.size()); }

  void submit(std::function<void()> f) {
    Job j = new std::function<void()>(std::move(f));
    { const int d = inFlight_.fetch_add(1, std::memory_order_acq_rel) + 1; if (readout_) readout_->depth(d); }
    pushNormal(j);
    wake(false);
  }
  void submitUrgent(std::function<void()> f) {
    Job j = new std::function<void()>(std::move(f));
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    if (!urgent_.tryPush(j)) pushNormal(j);  // ring full: spill (never drop, never block)
    wake(false);
  }
  void submitBackground(std::function<void()> f) {
    Job j = new std::function<void()>(std::move(f));
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    bgCount_.fetch_add(1, std::memory_order_acq_rel);
    bg_.push(std::move(j));
    wake(false);
  }
  bool urgentPending() const { return urgent_.pending() > 0; }
  void setLendCap(unsigned c) { lendCap_.store(std::max(1u, c), std::memory_order_release); wake(true); }
  unsigned lendCap() const { return lendCap_.load(std::memory_order_acquire); }
  void waitIdle() const {
    for (int v = inFlight_.load(std::memory_order_acquire); v != 0; v = inFlight_.load(std::memory_order_acquire))
      inFlight_.wait(v, std::memory_order_acquire);
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
    gen_.fetch_add(1, std::memory_order_acq_rel);
    if (all) gen_.notify_all();
    else if (sleepers_.load(std::memory_order_acquire) > 0) gen_.notify_one();
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
      if (gen_.load(std::memory_order_acquire) == g && !stop_.load(std::memory_order_acquire)) gen_.wait(g, std::memory_order_acquire);
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
    if (inFlight_.fetch_sub(1, std::memory_order_acq_rel) == 1) inFlight_.notify_all();
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  cUrgentRing<Job, 256> urgent_;
  cMpscQueue<Job> bg_;
  alignas(64) std::atomic<uint32_t> gen_{0};
  alignas(64) std::atomic<int> sleepers_{0};
  alignas(64) mutable std::atomic<int> inFlight_{0};
  std::atomic<int> bgActive_{0}, bgCount_{0};
  std::atomic<unsigned> lendCap_{1};
  std::atomic<size_t> rr_{0};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> failed_{0}, steals_{0};
  std::unique_ptr<cThreadReadout> readout_;  // LL_THREAD_READOUT=1 only; null otherwise
};

}  // namespace litholattice