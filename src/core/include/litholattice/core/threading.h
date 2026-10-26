// Threading primitives: lock-free triple buffer for snapshots, and a small job pool.
#pragma once
#include "litholattice/core/lock_timing.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include "litholattice/core/log.h"

namespace litholattice {

// Single-producer / single-consumer "latest value" mailbox. The writer never blocks the
// reader and vice versa (double buffering plus a spare slot so neither side ever waits).
//
// READ-REFERENCE LIFETIME (review A-L5): read() returns a reference into one of the
// three slots, and that reference is borrowed, not owned. It stays valid only until
// the writer's NEXT publish+write cycle reaches the same slot: the writer rotates
// through the slots, so after at most two further publish() calls the writer may be
// writing into the very slot a slow reader still holds, and the reader will observe
// the value changing under it (or torn, for non-trivially-copyable T). Callers must
// therefore COPY the value out of read() immediately and must not retain the
// reference across frames/ticks. There is exactly ONE reader thread: read() mutates
// the plain (non-atomic) read_ index, so two concurrent readers race with each
// other even though each individual exchange with the writer is atomic.
template <class T>
class TripleBuffer {
 public:
  T& writeSlot() { return slots_[write_]; }
  void publish() {  // writer: swap back slot into the shared middle, flag it fresh
    unsigned prev = middle_.exchange(write_ | kFresh, std::memory_order_acq_rel);
    write_ = prev & ~kFresh;
  }
  const T& read() {  // reader: grab the freshest slot if one was published
    if (middle_.load(std::memory_order_relaxed) & kFresh) {
      unsigned prev = middle_.exchange(read_, std::memory_order_acq_rel);
      read_ = prev & ~kFresh;
    }
    return slots_[read_];
  }

 private:
  static constexpr unsigned kFresh = 4;
  T slots_[3]{};
  unsigned write_ = 0, read_ = 1;
  std::atomic<unsigned> middle_{2};
};

/// 2026-10-26 (Archie triage, streaming hitches): opt-in background-worker throttle, read once
/// from the environment. LL_BG_MAX_ACTIVE=N caps concurrently running pool jobs per pool (0 =
/// no cap); LL_BG_YIELD_RENDER=1 makes idle workers hold off starting a NEW job while the
/// software rasterizer's band passes are in flight (renderBusy() > 0). Running jobs are never
/// interrupted. Both default off for every pool; the game's main pool opts in to a cap of 2
/// via cJobPool::setDefaultMaxActive (LL_BG_MAX_ACTIVE, when set, wins; 0 = uncapped).
struct BgThrottle {
  int maxActive = 0;
  bool maxActiveSet = false;  // LL_BG_MAX_ACTIVE present in the environment
  bool yieldRender = false;
  bool on() const { return maxActive > 0 || yieldRender; }
  static const BgThrottle& get() {
    static const BgThrottle t = [] {
      BgThrottle b;
      if (const char* e = std::getenv("LL_BG_MAX_ACTIVE")) { b.maxActive = std::max(0, std::atoi(e)); b.maxActiveSet = true; }
      if (const char* e = std::getenv("LL_BG_YIELD_RENDER")) b.yieldRender = e[0] == '1';
      return b;
    }();
    return t;
  }
};
inline std::atomic<int>& renderBusy() { static std::atomic<int> n{0}; return n; }
/// RAII mark around a render-critical parallel section (see BgThrottle).
struct RenderBusyScope {
  RenderBusyScope() { renderBusy().fetch_add(1, std::memory_order_relaxed); }
  ~RenderBusyScope() { renderBusy().fetch_sub(1, std::memory_order_relaxed); }
};

/// Fixed set of worker threads draining one FIFO of jobs. Exception-safe: a job that throws is
/// logged and counted (failedJobs()) instead of terminating the process, and the in-flight count
/// is always released, so waitIdle() and JobBatch::wait() can never hang on a failed job.
class cJobPool {
 public:
  explicit cJobPool(unsigned n) {
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { run(); });
  }
  ~cJobPool() {
    { std::lock_guard l(m_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  cJobPool(const cJobPool&) = delete;
  cJobPool& operator=(const cJobPool&) = delete;

  void submit(std::function<void()> job) {
    { locktiming::TimedGuard l(m_, locktiming::kJobPool); ++inFlight_; jobs_.push_back(std::move(job)); }  // 0.0.1.60 timing (diagnostic)
    cv_.notify_one();
  }
  unsigned size() const { return unsigned(workers_.size()); }
  /// Cap on concurrently running jobs for THIS pool unless LL_BG_MAX_ACTIVE overrides it.
  void setDefaultMaxActive(int n) {
    if (BgThrottle::get().maxActiveSet) return;
    { std::lock_guard l(m_); maxActive_ = std::max(0, n); }
    cv_.notify_all();
  }
  int maxActive() const { std::lock_guard l(m_); return maxActive_; }
  /// Blocks (no polling) until every job submitted so far has finished.
  void waitIdle() const {
    std::unique_lock l(m_);
    idle_.wait(l, [this] { return inFlight_ == 0; });
  }
  /// Jobs that ended with an exception since the pool was created.
  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }

 private:
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock l(m_);
        const BgThrottle& bt = BgThrottle::get();
        if (maxActive_ <= 0 && !bt.yieldRender) {
          cv_.wait(l, [this] { return stop_ || !jobs_.empty() || maxActive_ > 0; });
          if (!stop_ && maxActive_ > 0) continue;  // a cap was set while waiting: honour it
        } else {  // opt-in throttle: renderBusy() changes outside m_, so re-check every 1 ms
          auto ready = [&] {
            return stop_ || (!jobs_.empty() && (maxActive_ <= 0 || active_ < maxActive_) &&
                             !(bt.yieldRender && renderBusy().load(std::memory_order_relaxed) > 0));
          };
          while (!ready()) cv_.wait_for(l, std::chrono::milliseconds(1));
        }
        if (stop_ && jobs_.empty()) return;
        if (jobs_.empty()) continue;
        job = std::move(jobs_.front());
        jobs_.pop_front();
        ++active_;
      }
      try {
        job();
      } catch (const std::exception& e) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job pool: job threw: %s", e.what());
      } catch (...) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        logError("job pool: job threw a non-std exception");
      }
      bool idle;
      bool capped;
      { std::lock_guard l(m_); idle = --inFlight_ == 0; --active_; capped = maxActive_ > 0; }
      if (idle) idle_.notify_all();
      if (capped) cv_.notify_one();  // a capped slot freed
    }
  }
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> jobs_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  mutable std::condition_variable idle_;
  bool stop_ = false;
  int inFlight_ = 0;  // guarded by m_
  int active_ = 0;    // jobs running now, guarded by m_ (BgThrottle cap)
  int maxActive_ = BgThrottle::get().maxActive;  // guarded by m_; 0 = uncapped
  std::atomic<uint64_t> failed_{0};
};

/// A group of jobs on a cJobPool that can be waited on by itself (other pool work keeps running).
/// Each job's exception is caught here; wait() reports how many jobs of this batch failed.
class JobBatch {
 public:
  explicit JobBatch(cJobPool& pool) : pool_(pool) {}
  ~JobBatch() { wait(); }  // jobs reference this object: never let it die before they finish
  JobBatch(const JobBatch&) = delete;
  JobBatch& operator=(const JobBatch&) = delete;

  void submit(std::function<void()> job) {
    { std::lock_guard l(m_); ++pending_; }
    pool_.submit([this, job = std::move(job)] {
      bool ok = true;
      try { job(); } catch (const std::exception& e) { ok = false; logError("job batch: job threw: %s", e.what()); }
      catch (...) { ok = false; logError("job batch: job threw a non-std exception"); }
      std::lock_guard l(m_);
      if (!ok) ++failed_;
      if (--pending_ == 0) cv_.notify_all();  // notify under the lock: the waiter may destroy *this right after
    });
  }
  /// Blocks until every job submitted to this batch has finished. Returns the number that threw.
  int wait() {
    std::unique_lock l(m_);
    cv_.wait(l, [this] { return pending_ == 0; });
    return failed_;
  }

 private:
  cJobPool& pool_;
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0, failed_ = 0;
};

// MPSC hand-off: workers push, render thread swaps the whole vector out in O(1).
template <class T>
class cHandoffQueue {
 public:
  void push(T v) { locktiming::TimedGuard l(m_, locktiming::kHandoff); items_.push_back(std::move(v)); }  // 0.0.1.60 timing (diagnostic)
  void drainInto(std::vector<T>& out) {
    // Moves straight out of items_ and clears (keeping its buffer): the old
    // swap-into-a-local version freed the queue's heap buffer on every drain,
    // so the next push reallocated - one alloc/free per drain cycle.
    locktiming::TimedGuard l(m_, locktiming::kHandoff);  // 0.0.1.60 timing (diagnostic)
    out.insert(out.end(), std::make_move_iterator(items_.begin()),
               std::make_move_iterator(items_.end()));
    items_.clear();
  }
 private:
  std::mutex m_;
  std::vector<T> items_;
};

}  // namespace litholattice
