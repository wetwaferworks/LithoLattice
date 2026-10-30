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
#include <string>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include "litholattice/core/log.h"
#include "litholattice/core/thread_census.h"     // Thread Handshake P3: the shared backing pool
#include "litholattice/core/timer_resolution.h"  // raiseCurrentThreadForSim (shard work, F57-1)

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
///
/// Thread Handshake P4 (Archie t3353u): three lanes on one worker set, picked in order
///   urgent      submitUrgent(): Timing Handshake shard jobs, never throttled;
///   normal      submit(): the pool's own FIFO (BgThrottle cap / yield-render as before);
///   background  submitBackground(): LENT capacity. At most lendCap() background jobs run
///               at once, so reserve() workers are always left for shard jobs, and while a
///               Timing Handshake epoch is open (beginEpoch .. endEpoch) the lane is
///               RECLAIMED: no background job starts while shard jobs wait or while it
///               would dip into the reserve. Jobs are never preempted, so background work
///               must be stage-bounded: one job = one slice (a column, a mesh, a load);
///               a lent worker returns to the pool at every job boundary.
/// A VIEW (cJobPool(backing, Lane, maxConcurrent)) owns no threads: it queues its own jobs
/// and feeds them one at a time to the backing pool's lane (one job per lane turn), with
/// its own waitIdle()/failedJobs()/maxActive. LithoKraft's game pool is a background view
/// over cJobPool::shared() (P4 step 4).
class cJobPool {
 public:
  enum class Lane : uint8_t { Normal = 0, Background = 1 };

  explicit cJobPool(unsigned n, void (*threadInit)() = nullptr) {
    lendCap_ = n;  // a plain pool lends everything (no reserve) until configureLending()
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this, threadInit] { if (threadInit) threadInit(); run(); });
  }
  /// P4: a thread-less view over `backing` (which must outlive it). maxConcurrent caps
  /// this view's jobs running at once (further capped by setDefaultMaxActive and, on the
  /// background lane, by the backing pool's lendCap()).
  cJobPool(cJobPool& backing, Lane lane, unsigned maxConcurrent)
      : backing_(&backing), lane_(lane), viewCap_(std::max(1u, maxConcurrent)) {}
  ~cJobPool() {
    if (backing_) {  // view: drain like a pool (queued jobs still run), then wait out the pumps
      std::unique_lock l(m_);
      idle_.wait(l, [this] { return inFlight_ == 0 && pumps_ == 0; });
      return;
    }
    { std::lock_guard l(m_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  cJobPool(const cJobPool&) = delete;
  cJobPool& operator=(const cJobPool&) = delete;

  void submit(std::function<void()> job) {
    if (backing_) { viewSubmit(std::move(job)); return; }
    { locktiming::TimedGuard l(m_, locktiming::kJobPool); ++inFlight_; jobs_.push_back(std::move(job)); }  // 0.0.1.60 timing (diagnostic)
    cv_.notify_one();
  }
  /// Thread Handshake P3: a front-of-line job that bypasses the BgThrottle cap and
  /// yield-render. Timing Handshake shard work must never queue behind, or be throttled
  /// like, background work. Exceptions and waitIdle() behave as for submit().
  /// (On a view this goes straight to the backing pool, outside the view's counters.)
  void submitUrgent(std::function<void()> job) {
    if (backing_) { backing_->submitUrgent(std::move(job)); return; }
    {
      locktiming::TimedGuard l(m_, locktiming::kJobPool);
      ++inFlight_;
      urgent_.push_back(std::move(job));
      urgentCount_.fetch_add(1, std::memory_order_release);  // H3b: under m_, before a worker can pop it
    }
    cv_.notify_one();
  }
  /// P4: one stage-bounded slice of background work on lent capacity (see the class note).
  void submitBackground(std::function<void()> job) {
    if (backing_) { backing_->submitBackground(std::move(job)); return; }
    { locktiming::TimedGuard l(m_, locktiming::kJobPool); ++inFlight_; bg_.push_back(std::move(job)); }
    cv_.notify_one();
  }
  /// True while urgent (shard) jobs wait for a worker. Cooperative work (the cJobSystem
  /// facade's pumps) checks it at every job boundary and hands its worker back.
  /// Test only: raw urgent counter (must never go negative).
  int urgentCountForTest() const { return urgentCount_.load(std::memory_order_acquire); }
  bool urgentPending() const {
    return backing_ ? backing_->urgentPending() : urgentCount_.load(std::memory_order_acquire) > 0;
  }

  /// P4 lend / reclaim. reserve = workers never lent to the background lane (the shard
  /// jobs' guaranteed capacity); clamped so at least one worker can always be lent.
  void configureLending(unsigned reserve) {
    { std::lock_guard l(m_);
      const unsigned n = unsigned(workers_.size());
      reserve_ = n > 1 ? std::min(reserve, n - 1) : 0;
      lendCap_ = n - reserve_; }
    cv_.notify_all();
  }
  /// A Timing Handshake epoch (one runPhase) opens: background capacity is reclaimed
  /// until endEpoch(). Lent workers finish their current slice and come back.
  void beginEpoch() {
    std::lock_guard l(m_);
    ++epochs_;
    ++stats_.epochs;
    if (bgActive_ > 0) ++stats_.reclaimsWithLent;
  }
  void endEpoch() {
    { std::lock_guard l(m_); if (epochs_ > 0) --epochs_; }
    cv_.notify_all();  // the lane reopens: lend idle workers again
  }
  struct LendStats {
    uint64_t epochs = 0;            ///< beginEpoch() calls
    uint64_t reclaimsWithLent = 0;  ///< epochs that opened while background slices ran
    uint64_t bgStarted = 0;         ///< background slices started
    uint64_t bgStartedInEpoch = 0;  ///< ... of which started inside an epoch (spare workers only)
    unsigned bgPeak = 0;            ///< most background slices running at once
    unsigned lendCap = 0, reserve = 0;
  };
  LendStats lendStats() const {
    std::lock_guard l(m_);
    LendStats s = stats_;
    s.lendCap = lendCap_;
    s.reserve = reserve_;
    return s;
  }
  unsigned lendCap() const { if (backing_) return backing_->lendCap(); std::lock_guard l(m_); return lendCap_; }
  unsigned reserve() const { if (backing_) return backing_->reserve(); std::lock_guard l(m_); return reserve_; }

  /// Thread Handshake P3 (Archie ruling 3): the process-wide backing pool,
  /// clamp(cores - 2, 1, 64) workers from the thread census, recorded as Class B.
  /// Placement-new into static storage and never destroyed (JD t3349u item 5), so owners
  /// torn down in static destruction can still submit/drain. Workers run at sim priority
  /// (raiseCurrentThreadForSim, as the dedicated shard threads did). P4: the normal lane
  /// is unthrottled (LL_BG_MAX_ACTIVE applies to background views) and lending keeps
  /// min(simShards, size-1) workers in reserve.
  static cJobPool& shared();
  /// Worker threads; a view reports the most of its jobs that can run at once.
  unsigned size() const {
    if (!backing_) return unsigned(workers_.size());
    const unsigned b = lane_ == Lane::Background ? backing_->lendCap() : backing_->size();
    return std::min(viewCap_, b);
  }
  /// Cap on concurrently running jobs for THIS pool unless LL_BG_MAX_ACTIVE overrides it.
  void setDefaultMaxActive(int n) {
    if (BgThrottle::get().maxActiveSet) return;
    { std::lock_guard l(m_); maxActive_ = std::max(0, n); }
    if (backing_) { viewKick(); return; }
    cv_.notify_all();
  }
  int maxActive() const { std::lock_guard l(m_); return maxActive_; }
  /// 2026-10-26 (Archie item 4): runtime yield-render (adaptive policy). LL_BG_YIELD_RENDER=1 forces it on.
  /// P4: on a background view this sets the backing pool's background-lane yield.
  void setYieldRender(bool on) {
    if (backing_) {
      { std::lock_guard l(m_); yieldRender_ = on; }
      if (lane_ == Lane::Background) backing_->setBackgroundYieldRender(on);
      return;
    }
    { std::lock_guard l(m_); yieldRender_ = on; }
    cv_.notify_all();
  }
  void setBackgroundYieldRender(bool on) {
    { std::lock_guard l(m_); bgYieldRender_ = on; }
    cv_.notify_all();
  }
  /// Blocks (no polling) until every job submitted so far has finished.
  void waitIdle() const {
    std::unique_lock l(m_);
    idle_.wait(l, [this] { return inFlight_ == 0; });
  }
  /// Jobs that ended with an exception since the pool was created.
  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }

 private:
  // ---- view (P4) ----
  int viewLimit() const {  // m_ held
    int c = int(viewCap_);
    if (maxActive_ > 0) c = std::min(c, maxActive_);
    return c;
  }
  void viewLaunch() {
    auto f = [this] { viewPump(); };
    if (lane_ == Lane::Background) backing_->submitBackground(std::move(f));
    else backing_->submit(std::move(f));
  }
  void viewSubmit(std::function<void()> job) {
    bool launch = false;
    { locktiming::TimedGuard l(m_, locktiming::kJobPool);
      ++inFlight_;
      jobs_.push_back(std::move(job));
      if (pumps_ < viewLimit()) { ++pumps_; launch = true; } }
    if (launch) viewLaunch();
  }
  void viewKick() {  // a raised cap admits more pumps for already-queued jobs
    int n = 0;
    { std::lock_guard l(m_);
      while (pumps_ < viewLimit() && size_t(pumps_) < jobs_.size()) { ++pumps_; ++n; } }
    for (int i = 0; i < n; ++i) viewLaunch();
  }
  // One lane turn = one job: the backing worker is handed back at every job (stage)
  // boundary, and the next turn queues behind whatever the lane admits next.
  void viewPump() {
    std::function<void()> job;
    { std::lock_guard l(m_);
      if (jobs_.empty() || pumps_ > viewLimit()) { --pumps_; idle_.notify_all(); return; }  // notify under the lock
      job = std::move(jobs_.front());
      jobs_.pop_front(); }
    try {
      job();
    } catch (const std::exception& e) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job pool: job threw: %s", e.what());
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job pool: job threw a non-std exception");
    }
    bool again;
    { std::lock_guard l(m_);
      --inFlight_;
      again = !jobs_.empty() && pumps_ <= viewLimit();
      if (!again) --pumps_;
      idle_.notify_all(); }  // under the lock: the waiter may destroy *this right after
    if (again) viewLaunch();
  }

  // ---- worker pool ----
  // Background admission (m_ held). Outside an epoch: up to lendCap_ slices. Inside one
  // (reclaimed): only on a spare worker, never while shard jobs wait or into the reserve.
  bool bgAdmit(bool renderIsBusy) const {
    if ((bgYieldRender_ || BgThrottle::get().yieldRender) && renderIsBusy) return false;
    if (bgActive_ >= int(lendCap_)) return false;
    if (epochs_ > 0) {
      if (!urgent_.empty()) return false;
      if (int(workers_.size()) - active_ <= int(reserve_)) return false;  // this worker is one of the free ones
    }
    return true;
  }
  void run() {
    for (;;) {
      std::function<void()> job;
      bool bg = false;
      {
        std::unique_lock l(m_);
        for (;;) {
          if (!urgent_.empty()) {  // P3: urgent (shard) jobs first, never throttled
            job = std::move(urgent_.front());
            urgent_.pop_front();
            urgentCount_.fetch_sub(1, std::memory_order_release);
            break;
          }
          const BgThrottle& bt = BgThrottle::get();
          const bool rb = renderBusy().load(std::memory_order_relaxed) > 0;
          const bool yieldN = bt.yieldRender || yieldRender_;
          if (!jobs_.empty() && (stop_ || ((maxActive_ <= 0 || active_ < maxActive_) && !(yieldN && rb)))) {
            job = std::move(jobs_.front());
            jobs_.pop_front();
            break;
          }
          if (!bg_.empty() && (stop_ || bgAdmit(rb))) {
            job = std::move(bg_.front());
            bg_.pop_front();
            bg = true;
            ++bgActive_;
            ++stats_.bgStarted;
            if (epochs_ > 0) ++stats_.bgStartedInEpoch;
            stats_.bgPeak = std::max(stats_.bgPeak, unsigned(bgActive_));
            break;
          }
          if (stop_ && jobs_.empty() && bg_.empty()) return;
          // renderBusy() changes outside m_: poll every 1 ms only while a yield-render gate holds work back
          const bool poll = (yieldN && !jobs_.empty()) || ((bgYieldRender_ || bt.yieldRender) && !bg_.empty());
          if (poll) cv_.wait_for(l, std::chrono::milliseconds(1));
          else cv_.wait(l);
        }
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
      bool wake;
      { std::lock_guard l(m_);
        idle = --inFlight_ == 0;
        --active_;
        if (bg) --bgActive_;
        wake = maxActive_ > 0 || bg || !bg_.empty(); }
      if (idle) idle_.notify_all();
      if (wake) cv_.notify_one();  // a capped / lent slot freed
    }
  }
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> jobs_;    // normal lane (a view: its own queue)
  std::deque<std::function<void()>> urgent_;  // P3: submitUrgent(), ahead of jobs_, guarded by m_
  std::deque<std::function<void()>> bg_;      // P4: background lane (lent capacity), guarded by m_
  mutable std::mutex m_;
  std::condition_variable cv_;
  mutable std::condition_variable idle_;
  bool stop_ = false;
  int inFlight_ = 0;  // guarded by m_
  int active_ = 0;    // jobs running now, guarded by m_ (BgThrottle cap)
  int maxActive_ = BgThrottle::get().maxActive;  // guarded by m_; 0 = uncapped
  bool yieldRender_ = false;                     // guarded by m_ (setYieldRender)
  std::atomic<uint64_t> failed_{0};
  std::atomic<int> urgentCount_{0};  // urgent_.size(), readable without m_ (urgentPending)
  // P4 lend / reclaim (guarded by m_)
  int bgActive_ = 0;
  unsigned lendCap_ = 0, reserve_ = 0;
  int epochs_ = 0;
  bool bgYieldRender_ = false;
  LendStats stats_;
  // P4 view (backing_ != nullptr)
  cJobPool* backing_ = nullptr;
  Lane lane_ = Lane::Normal;
  unsigned viewCap_ = 0;
  int pumps_ = 0;  // guarded by m_
};

/// RAII Timing Handshake epoch on a pool (null-safe).
class cJobPoolEpoch {
 public:
  explicit cJobPoolEpoch(cJobPool* p) : p_(p) { if (p_) p_->beginEpoch(); }
  ~cJobPoolEpoch() { if (p_) p_->endEpoch(); }
  cJobPoolEpoch(const cJobPoolEpoch&) = delete;
  cJobPoolEpoch& operator=(const cJobPoolEpoch&) = delete;
 private:
  cJobPool* p_;
};

inline cJobPool& cJobPool::shared() {
  static cJobPool* p = [] {
    cThreadCensus& c = cThreadCensus::instance();
    const unsigned n = c.budget().pool;  // clamp(cores-2, 1, 64)
    alignas(cJobPool) static unsigned char storage[sizeof(cJobPool)];
    auto* pool = new (storage) cJobPool(n, &raiseCurrentThreadForSim);
    { std::lock_guard l(pool->m_); pool->maxActive_ = 0; }  // P4: normal lane unthrottled; views carry the cap
    pool->configureLending(c.budget().simShards);             // P4: shard jobs' reserve
    for (unsigned i = 0; i < n; ++i) c.declare(ThreadClass::Pooled, ThreadRole::PoolWorker, uint16_t(i), "pool");
    logInfo("threads: shared job pool up: %u workers (clamp(cores-2, 1, %u)); lends up to %u to background, "
            "reserves %u for shard jobs", n, kMaxPoolWorkers, pool->lendCap(), pool->reserve());
    const std::string inv = poolInvariantViolation(c.live(ThreadClass::Pooled), c.budget());  // t3353u
    if (!inv.empty()) logWarn("%s", inv.c_str());
    return pool;
  }();
  return *p;
}

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
