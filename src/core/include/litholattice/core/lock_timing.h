#pragma once
// Diagnostic-only main-thread lock-wait timing (0.0.1.60, Proposed by JD, Built by Cortana).
// LL_LOCK_TIMING=1 turns it on (read once); default OFF. When off, every timed acquisition is a
// plain lock() behind one cached-bool branch, and holder marks are a branch only: no behavior change.
// When on, the main thread's acquisitions of instrumented locks are timed (try_lock first; only a
// contended acquisition reads the clock), and a "lock timing:" summary is logged every 120 frames.
// Measurement only: lock order, scope and type are unchanged.
#include <atomic>
#include <cstdint>

namespace litholattice::locktiming {

enum Site : uint8_t { kBlock = 0, kHandoff, kLog, kArena, kJobPool, kSiteCount };
enum Holder : uint8_t { kHolderNone = 0, kHolderSimTick, kHolderSimPost, kHolderOther, kHolderCount };

bool readEnabled();
inline bool enabled() { static const bool e = readEnabled(); return e; }
bool onMainThread();
void markMainThread();                      ///< call once on the game/render thread
void record(Site s, int64_t waitNs, Holder h, bool contended);
std::atomic<uint8_t>& blockHolder();        ///< current blockMutex_ holder site (sim side), when enabled
inline void setBlockHolder(Holder h) { if (enabled()) blockHolder().store(h, std::memory_order_relaxed); }
void frameEnd();                            ///< main thread, once per frame (logs every 120 frames)

int64_t nowNs();

template <class M>
inline void lockTimed(M& m, Site s) {
  if (!enabled() || !onMainThread()) { m.lock(); return; }
  if (m.try_lock()) { record(s, 0, kHolderNone, false); return; }
  const Holder h = s == kBlock ? Holder(blockHolder().load(std::memory_order_relaxed)) : kHolderOther;
  const int64_t t0 = nowNs();
  m.lock();
  record(s, nowNs() - t0, h == kHolderNone ? kHolderOther : h, true);
}

/// Drop-in for std::lock_guard on an instrumented lock.
template <class M>
class TimedGuard {
 public:
  TimedGuard(M& m, Site s) : m_(m) { lockTimed(m_, s); }
  ~TimedGuard() { m_.unlock(); }
  TimedGuard(const TimedGuard&) = delete;
  TimedGuard& operator=(const TimedGuard&) = delete;
 private:
  M& m_;
};

}  // namespace litholattice::locktiming