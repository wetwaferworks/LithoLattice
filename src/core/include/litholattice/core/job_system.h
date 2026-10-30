// Work-stealing job system: the threading backbone for LithoLattice.
//
// Thread Handshake P4 (Archie t3353u, via JD): cJobSystem is now a thin SUBMISSION
// FACADE over cJobPool::shared(). It owns no worker threads: jobs wait in three
// per-priority FIFOs here and are run by up to `workerThreads` "pumps", each a normal
// job on the shared pool that drains this system's queues (high priority first) and
// hands its worker back whenever Timing Handshake shard jobs are waiting
// (cJobPool::urgentPending). Every worker job here is short and frame-bounded, so no
// caller needs the background lane; long work goes to cJobPool background views
// (lend / reclaim, threading.h). Dedicated I/O threads (class System, never budgeted
// as compute) still run blocking jobs so pool workers NEVER block.
// A JobFence is the per-stage barrier the deterministic scheduler uses.
//
// The work-stealing fleet (per-worker Chase-Lev deques, MPSC inboxes) is retired with
// the private threads; chase_lev.h / mpsc_queue.h stay as standalone primitives.
//
// ---- lock order (strict; read before adding any lock) --------------------------
// L0  lock-free: Chase-Lev deques, cSpscQueue, atomics (stop_, workGen_,
//     inFlight_, ioInFlight_, submitCursor_). No lock, no order.
// L1  overflowM_ / ioM_ / fence m_: leaf mutexes. Held only to push/drain a short
//     queue or to flip a completion counter; NEVER held while running a job and
//     never held while acquiring L2.
// L2  cJobSystem control mutex m_: only for the sleep/wake condition-variable
//     handshake and the shutdown sequence. Never held while running a job.
// L3  debug-log mutex (thread_debug.cpp): absolute leaf. Taken to emit one log
//     line, released immediately; no other lock may be acquired while holding it.
// RULES: no lock is ever held across a job boundary (jobs run with zero locks
// held); a job must never call waitIdle()/JobFence::wait() (thread-local assert —
// that would deadlock the pool); blocking work goes to submitIo(), never to a
// worker job (debug builds log a warning for any job running longer than
// kSlowJobWarnUs).
//
// ---- shutdown -------------------------------------------------------------------
// Disciplined, no detached threads, no races: the destructor sets stop_, wakes
// everyone, drains already-submitted work (workers AND I/O threads finish what is
// queued), then joins every thread. Submitting during/after shutdown is a bug
// (assert in debug builds). Because the destructor drains, jobs must not reference
// objects that die before the cJobSystem does.
//
// ---- determinism ----------------------------------------------------------------
// The system provides WHERE work runs; the cDeterministicScheduler (sim/det_sched.h)
// declares the ORDER. A stage's systems are submitted behind a JobFence; the fence
// is the stage barrier, so stages still run strictly in order and command buffers
// still drain between stages exactly as on the cJobPool path. Stealing only moves a
// job to another thread — within one stage the systems provably write disjoint
// resources, so placement cannot change the result. Threaded runs hash
// bit-identical to single-threaded runs (see the det-sched self-tests, 1/2/4/8
// workers, cJobPool vs cJobSystem backends).
//
// Naming: litholattice:: namespace; I-prefix only for real interfaces (none here —
// the -OR suffix is for game-style components, not engine infrastructure, and is
// deliberately not used).
#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "litholattice/core/chase_lev.h"
#include "litholattice/core/mpsc_queue.h"
#include "litholattice/core/log.h"
#include "litholattice/core/thread_debug.h"
#include "litholattice/core/thread_census.h"  // Thread Handshake P3
#include "litholattice/core/threading.h"      // P4: cJobPool::shared() backs every cJobSystem

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#endif

namespace litholattice {

// ---- tunables -----------------------------------------------------------------
inline constexpr size_t kJobDequeCapacity = 1024;  ///< per-thread, per-priority Chase-Lev slots
inline constexpr unsigned kMaxJobThreads = 64;     ///< sanity clamp on worker/io thread counts
/// Debug tripwire: a worker job running longer than this is logged as a bug
/// ("worker jobs must never block"). Active only with LITHOLATTICE_THREAD_DEBUG.
inline constexpr uint64_t kSlowJobWarnUs = 50000;

enum class JobPriority : uint8_t { Low = 0, Normal = 1, High = 2 };
inline constexpr unsigned kJobPriorityCount = 3;

using Job = std::function<void()>;

// splitmix32: cheap, high-quality mixer for seeding per-worker PRNG states.
inline uint32_t splitmix32(uint32_t x) {
  x += 0x9E3779B9u;
  x = (x ^ (x >> 15)) * 0x2C1B3C6Du;
  x = (x ^ (x >> 12)) * 0x297A2D39u;
  return x ^ (x >> 15);
}

#ifdef LITHOLATTICE_THREAD_DEBUG
inline constexpr bool kThreadDebug = true;
#else
inline constexpr bool kThreadDebug = false;
#endif

// ---- thread utilities -----------------------------------------------------------
/// Platform-guarded thread naming. Linux: pthread_setname_np; elsewhere a no-op.
/// Names are truncated to 15 chars (Linux limit). JD t3349u item 9 / Archie ruling 6:
/// the engine never pins, so the affinity helpers are gone.
struct ThreadUtil {
  static void setName(const char* name) {
#if defined(__linux__)
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s", name);
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
    LITHO_THREAD_LOG("thread", "named '%s'", name);
  }

  static unsigned coreCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
  }
};

/// RAII named thread: names itself, joins on destruction (never detached). Never
/// pinned (ruling 6; the affinity mask parameter was removed, JD t3349u item 9).
class cNamedThread {
 public:
  template <typename Fn>
  cNamedThread(const char* name, Fn&& fn) {
    thread_ = std::thread([n = std::string(name), f = std::forward<Fn>(fn)]() mutable {
      ThreadUtil::setName(n.c_str());
      f();
    });
  }
  ~cNamedThread() {
    if (thread_.joinable()) thread_.join();  // disciplined: never detached
  }
  cNamedThread(const cNamedThread&) = delete;
  cNamedThread& operator=(const cNamedThread&) = delete;

 private:
  std::thread thread_;
};

// ---- the job system ---------------------------------------------------------------
class cJobSystem {
 public:
  struct Config {
    /// Max jobs of THIS system running at once (its pumps on the shared pool). 0 = the
    /// census pool ceiling clamp(cores-2, 1, 64), i.e. cJobPool::shared().size().
    /// P4: no threads are created for workers; this is a concurrency cap only.
    unsigned workerThreads = 0;
    unsigned ioThreads = 1;      ///< dedicated blocking-job threads (clamped to >= 1), class System
    const char* namePrefix = "litho";  ///< io thread names: "<prefix>-io0" ... (<=15 chars total)
  };

  // Note: no `= Config{}` default argument here — GCC rejects a nested struct with
  // default member initializers as a default argument inside the enclosing class.
  cJobSystem() { init(Config{}); }
  explicit cJobSystem(const Config& cfg) { init(cfg); }
  ~cJobSystem() { shutdown(); }

  cJobSystem(const cJobSystem&) = delete;
  cJobSystem& operator=(const cJobSystem&) = delete;

  /// Submit a non-blocking job (priority-ordered). Worker jobs must NEVER block: no I/O,
  /// no waiting on game locks, no waitIdle().
  void submit(Job job, JobPriority prio = JobPriority::Normal) {
    assert(!stop_.load(std::memory_order_relaxed) && "submit during/after shutdown");
    inFlight_.fetch_add(1, std::memory_order_relaxed);
    bool launch = false;
    {
      std::lock_guard<std::mutex> l(qM_);  // L1 leaf
      queue_[unsigned(prio)].push_back(std::move(job));
      if (pumps_ < cap_) { ++pumps_; launch = true; }
    }
    LITHO_THREAD_LOG("job", "submit prio=%u%s", unsigned(prio), launch ? " +pump" : "");
    if (launch) launchPump();
  }

  /// Submit a job that may block (file/network I/O). Runs on a dedicated I/O
  /// thread, never on a pool worker — a blocked worker is a lost sim core.
  void submitIo(Job job) {
    assert(!stop_.load(std::memory_order_relaxed) && "submitIo during/after shutdown");
    ioInFlight_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> l(ioM_);  // L1 leaf
      ioQueue_.push_back(std::move(job));
    }
    ioCv_.notify_one();
  }

  /// Block until every submitted job (workers + I/O) has finished. Must NOT be
  /// called from a worker/I/O job (thread-local assert: that would deadlock).
  void waitIdle() {
    assert(!t_onJobThread && "waitIdle() from a job thread would deadlock the pool");
    std::unique_lock<std::mutex> l(m_);  // L2
    auto idle = [this] {
      return inFlight_.load(std::memory_order_acquire) == 0 &&
             ioInFlight_.load(std::memory_order_acquire) == 0;
    };
    if constexpr (kThreadDebug) {  // B8 watchdog (debug builds only): same wait, dump after 10 s
      if (!idleCv_.wait_for(l, std::chrono::seconds(10), idle)) { debugDump("waitIdle() 10 s without progress"); }
    }
    idleCv_.wait(l, idle);
  }

  uint64_t failedJobs() const { return failed_.load(std::memory_order_relaxed); }
  /// Concurrency cap (P4: pumps on the shared pool, not owned threads).
  unsigned workerCount() const { return cap_; }
  unsigned ioThreadCount() const { return (unsigned)ioThreads_.size(); }

  /// B8 (debug builds only): job-system counters + every thread's trace ring to stderr.
  void debugDump(const char* why) const {
    LITHO_THREAD_LOG("job", "dump: inFlight=%d ioInFlight=%d stop=%d", inFlight_.load(), ioInFlight_.load(),
                     int(stop_.load()));
    LITHO_THREAD_DUMP(why);
    (void)why;
  }

  /// True when called from inside a job of any cJobSystem (pump or I/O thread).
  static bool onJobThread() { return t_onJobThread; }

  /// Owner-rule audit (P0.5 review fix 2). P4: the per-worker deques are gone with the
  /// private threads, so there is no owner end to violate; always 0.
  uint64_t ownerViolations() const { return 0; }

 private:
  static thread_local bool t_onJobThread;

  void init(const Config& cfg) {
    pool_ = &cJobPool::shared();
    unsigned cap = cfg.workerThreads == 0 ? cThreadCensus::instance().budget().pool : cfg.workerThreads;
    if (cap < 1) cap = 1;
    if (cap > kMaxJobThreads) cap = kMaxJobThreads;
    cap_ = cap;
    unsigned ios = cfg.ioThreads < 1 ? 1 : cfg.ioThreads;
    if (ios > kMaxJobThreads) ios = kMaxJobThreads;
    ioThreads_.reserve(ios);
    for (unsigned i = 0; i < ios; ++i) {
      std::string name = std::string(cfg.namePrefix) + "-io" + std::to_string(i);
      ioThreads_.emplace_back([this, i, name] {
        t_onJobThread = true;
        ThreadUtil::setName(name.c_str());
        LITHO_THREAD_LOG("thread", "io thread %u online", i);
        ioLoop(i);
        LITHO_THREAD_LOG("thread", "io thread %u offline", i);
      });
    }
    for (unsigned i = 0; i < ios; ++i) records_.add(ThreadClass::System, ThreadRole::Storage, uint16_t(i), "jobsys-io");
    LITHO_THREAD_LOG("thread", "cJobSystem up: facade cap %u on the %u-worker shared pool + %u io threads", cap_,
                     pool_->size(), ios);
  }

  void shutdown() {
    LITHO_THREAD_LOG("thread", "shutdown: draining %d worker + %d io jobs in flight",
                     inFlight_.load(std::memory_order_relaxed),
                     ioInFlight_.load(std::memory_order_relaxed));
    // Drain first (everything already submitted still runs, as before), then stop.
    {
      std::unique_lock<std::mutex> l(m_);  // L2
      idleCv_.wait(l, [this] {
        return inFlight_.load(std::memory_order_acquire) == 0 && ioInFlight_.load(std::memory_order_acquire) == 0;
      });
    }
    {  // a pump may still be between its last job and its exit: it touches `this` until then
      std::unique_lock<std::mutex> l(qM_);
      pumpCv_.wait(l, [this] { return pumps_ == 0; });
    }
    stop_.store(true, std::memory_order_release);
    // B8: the empty lock before the notify is REQUIRED (io threads test stop_ under ioM_).
    { std::lock_guard<std::mutex> l(ioM_); }
    ioCv_.notify_all();
    for (auto& t : ioThreads_) t.join();
    records_.retireAll();  // joined -> RETURNED
    LITHO_THREAD_LOG("thread", "shutdown: io threads joined, failed=%llu",
                     (unsigned long long)failed_.load(std::memory_order_relaxed));
  }

  void launchPump() { pool_->submit([this] { pump(); }); }

  // One pump: drains this system's queues, high priority first, on a shared-pool worker.
  // Hands the worker back (re-queues itself) at a job boundary when shard jobs wait.
  void pump() {
    const bool prev = t_onJobThread;
    t_onJobThread = true;
    for (;;) {
      Job job;
      {
        std::lock_guard<std::mutex> l(qM_);
        int p = int(JobPriority::High);
        while (p >= int(JobPriority::Low) && queue_[p].empty()) --p;
        if (p < int(JobPriority::Low)) {
          --pumps_;
          pumpCv_.notify_all();  // under the lock: shutdown may destroy *this right after
          break;
        }
        job = std::move(queue_[p].front());
        queue_[p].pop_front();
      }
      executeJob(std::move(job), inFlight_, "worker", 0);
      if (pool_->urgentPending()) {  // shard jobs first: give the worker back, keep the pump slot
        launchPump();
        break;
      }
    }
    t_onJobThread = prev;
  }

  void ioLoop(unsigned self) {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> l(ioM_);  // L1
        ioCv_.wait(l, [this] {
          return stop_.load(std::memory_order_acquire) || !ioQueue_.empty();
        });
        if (ioQueue_.empty()) return;  // stop_ is set (see predicate)
        job = std::move(ioQueue_.front());
        ioQueue_.pop_front();
      }
      executeJob(std::move(job), ioInFlight_, "io", self);
    }
  }

  // Runs one job with ZERO locks held (the lock-order rules). Exceptions are
  // caught and counted; the in-flight counter is always released so waitIdle()
  // and JobFence::wait() can never hang on a failed job.
  void executeJob(Job&& job, std::atomic<int>& counter, const char* kind, unsigned idx) {
    (void)kind;  // only used by the debug logging below
    (void)idx;
    [[maybe_unused]] uint64_t id = 0, t0 = 0;
    if constexpr (kThreadDebug) {
      id = detail::threadDebugNextJobId();
      t0 = detail::threadDebugNowUs();
      LITHO_THREAD_LOG("job", "start id=%llu %s=%u", (unsigned long long)id, kind, idx);
    }
    try {
      job();
    } catch (const std::exception& e) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job system: job threw: %s", e.what());
    } catch (...) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      logError("job system: job threw a non-std exception");
    }
    if constexpr (kThreadDebug) {
      const uint64_t dt = detail::threadDebugNowUs() - t0;
      LITHO_THREAD_LOG("job", "finish id=%llu %llu us", (unsigned long long)id,
                       (unsigned long long)dt);
      if (dt > kSlowJobWarnUs)
        LITHO_THREAD_LOG("job", "SLOW JOB id=%llu ran %llu us: worker jobs must never block",
                         (unsigned long long)id, (unsigned long long)dt);
    }
    if (counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> l(m_);  // L2, only to pair with idleCv_
      idleCv_.notify_all();
    }
  }

  cJobPool* pool_ = nullptr;   // cJobPool::shared(), never destroyed
  unsigned cap_ = 1;
  std::deque<Job> queue_[kJobPriorityCount];  // guarded by qM_
  unsigned pumps_ = 0;                         // guarded by qM_
  std::mutex qM_;                              // L1 leaf
  std::condition_variable pumpCv_;             // shutdown waits for pumps_ == 0
  std::vector<std::thread> ioThreads_;
  cThreadRecordSet records_;  // io threads, class System

  std::deque<Job> ioQueue_;
  std::mutex ioM_;                 // L1
  std::condition_variable ioCv_;

  std::mutex m_;                   // L2: idleCv_ pairing
  std::condition_variable idleCv_;  // waitIdle() sleeps here
  std::atomic<bool> stop_{false};
  std::atomic<int> inFlight_{0};
  std::atomic<int> ioInFlight_{0};
  std::atomic<uint64_t> failed_{0};
};

inline thread_local bool cJobSystem::t_onJobThread = false;

/// A group of jobs on a cJobSystem that can be waited on by itself (other pool work
/// keeps running) — the stage barrier for the deterministic scheduler. Each job's
/// exception is caught here; wait() reports how many jobs of this fence failed.
/// The destructor waits, so a fence must not outlive work it references... rather:
/// jobs may reference the fence's owner, so never let the owner die first.
class JobFence {
 public:
  explicit JobFence(cJobSystem& sys) : sys_(sys) {}
  ~JobFence() { wait(); }  // jobs reference the caller: never let it die before they finish
  JobFence(const JobFence&) = delete;
  JobFence& operator=(const JobFence&) = delete;

  void submit(Job job, JobPriority prio = JobPriority::Normal) {
    {
      std::lock_guard<std::mutex> l(m_);  // L1 leaf
      ++pending_;
    }
    sys_.submit(
        [this, j = std::move(job)] {
          bool ok = true;
          try {
            j();
          } catch (const std::exception& e) {
            ok = false;
            logError("job fence: job threw: %s", e.what());
          } catch (...) {
            ok = false;
            logError("job fence: job threw a non-std exception");
          }
          std::lock_guard<std::mutex> l(m_);  // L1 leaf; never nested with system locks
          if (!ok) ++failed_;
          if (--pending_ == 0) cv_.notify_all();  // notify under the lock: the waiter may destroy *this right after
        },
        prio);
  }

  /// Blocks until every job submitted to this fence has finished. Returns the
  /// number that threw. Must not be called from a job thread (assert).
  int wait() {
    assert(!cJobSystem::onJobThread() && "JobFence::wait() from a job thread would deadlock");
    std::unique_lock<std::mutex> l(m_);  // L1 leaf
    if constexpr (kThreadDebug) {  // B8 watchdog (debug builds only): same wait, dump after 10 s
      if (!cv_.wait_for(l, std::chrono::seconds(10), [this] { return pending_ == 0; })) {
        LITHO_THREAD_LOG("job", "fence stuck: pending=%d", pending_);
        sys_.debugDump("JobFence::wait() 10 s without progress");
      }
    }
    cv_.wait(l, [this] { return pending_ == 0; });
    return failed_;
  }

 private:
  cJobSystem& sys_;
  std::mutex m_;
  std::condition_variable cv_;
  int pending_ = 0, failed_ = 0;
};

}  // namespace litholattice
