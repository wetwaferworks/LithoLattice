// Tick ledger (Thread Handshake plan P4, Archie's design §1 and ruling R7).
//
// Every job a Timing Handshake tick runs gets a deterministic record:
//   * SYNC jobs (the due components of one runPhase) get seq 0..K-1 in registration
//     order. Their slot is ring[(T_offset + seq) & MASK]: T_offset is the running count
//     of sync records issued before this tick, so each tick's slots are contiguous.
//     Workers fill their own slots in any order; at the barrier the authority sweeps
//     them linearly in (tick, seq) order. No locks and no heap use after warm-up: the
//     ring is preallocated from a cTickArena owned by the ledger.
//   * ASYNC jobs are spawned from inside a sync job with asyncBegin(latencyTicks). They
//     may finish on any thread at any time; they are COMMITTED at the barrier of tick
//     spawnTick + latencyTicks (WAIT policy: that barrier waits for them, the watchdog
//     ceiling still applies).
//
// Hashing rule (plan §1 fairness rule):
//   sync   hash(tick, seq, componentId, returnTick, payloadHash)
//   async  hash(spawnTick, dispatchSeq, deterministicCommitEpoch, payloadHash)
// observedReturnTick (the tick open when an async job really finished) and all
// wall-clock stamps are telemetry/audit only and never reach a hash. Async records are
// folded with a commutative sum, so the fold does not depend on which thread grabbed
// which ring slot; dispatchSeq = (parent seq << 32) | spawn index within the parent,
// which is deterministic because one sync job runs on one thread.
//
// commit_epoch (Archie R7): monotonic uint64, advanced only at the sim-tick barrier when
// the phase reaches COMMITTED (after the sweep and the commit hook). A frozen phase never
// commits and never advances it.
//
// Threads: openTick/sweep/commit/abandonOpenTick run on the authority thread only.
// Slot writes come from the worker running that job (handed over by the barrier mutex).
// asyncBegin runs inside a job; asyncComplete may be called from any thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

#include "litholattice/core/log.h"
#include "litholattice/core/tick_arena.h"
#include "litholattice/core/tick_profile.h"

namespace litholattice {

class cTickLedger {
 public:
  static constexpr uint64_t kNoHandle = UINT64_MAX;

  struct SyncSlot {
    uint64_t tick = 0, returnTick = 0, payloadHash = 0;
    uint32_t seq = 0, componentId = 0, asyncSpawned = 0, filled = 0;
    uint64_t telStartNs = 0, telEndNs = 0;  // telemetry only, never hashed
  };
  struct AsyncRec {
    std::atomic<uint32_t> state;  // kFree / kPending / kDone / kCancelled
    uint32_t pad = 0;
    uint64_t spawnTick = 0, dispatchSeq = 0, dueTick = 0, payloadHash = 0;
    uint64_t observedReturnTick = 0;  // audit only, never hashed
  };
  enum : uint32_t { kFree = 0, kPending = 1, kDone = 2, kCancelled = 3 };

  struct Stats {
    uint64_t ticks = 0, syncRecords = 0, asyncCommitted = 0, asyncLate = 0, asyncCancelled = 0;
    uint64_t overheadNsTotal = 0, overheadNsMax = 0, asyncWaitNsTotal = 0;
  };

  explicit cTickLedger(uint32_t asyncCapacity = 4096) : arena_(64 * 1024) {
    uint32_t a = 64;
    while (a < asyncCapacity) a <<= 1;
    asyncMask_ = a - 1;
    async_ = arena_.alloc<AsyncRec>(a);
    for (uint32_t i = 0; i < a; ++i) {
      ::new (&async_[i].state) std::atomic<uint32_t>(kFree);
      async_[i].spawnTick = async_[i].dispatchSeq = async_[i].dueTick = async_[i].payloadHash = 0;
    }
  }
  cTickLedger(const cTickLedger&) = delete;
  cTickLedger& operator=(const cTickLedger&) = delete;

  /// Size the sync ring for `components` (pre-start; growing later re-reserves from the
  /// arena, never frees). Capacity is a power of two >= 2 x components, minimum 64.
  void reserve(size_t components) {
    uint32_t want = 64;
    while (want < components * 2) want <<= 1;
    if (want <= mask_ + 1 && ring_) return;
    ring_ = arena_.alloc<SyncSlot>(want);
    for (uint32_t i = 0; i < want; ++i) ::new (&ring_[i]) SyncSlot();
    mask_ = want - 1;
  }

  /// Opens the tick with K sync jobs (seq 0..K-1). Authority thread.
  void openTick(uint32_t k) {
    const uint64_t t0 = lnow();
    if (!ring_ || k > mask_ + 1) reserve(k);
    open_ = true;
    k_ = k;
    openTick_.store(epoch_, std::memory_order_release);  // the tick being built commits as epoch_ + 1
    for (uint32_t s = 0; s < k; ++s) ring_[(tOffset_ + s) & mask_].filled = 0;
    openNs_ = lnow() - t0;
  }

  /// The slot of sync job `seq` of the open tick; the worker running it fills it.
  SyncSlot& slot(uint32_t seq) { return ring_[(tOffset_ + seq) & mask_]; }
  uint64_t currentTick() const { return openTick_.load(std::memory_order_acquire); }

  /// Called by the authority's runItem around one job.
  struct JobScope {
    SyncSlot* prev;
    cTickLedger* prevLedger;
    JobScope(cTickLedger& l, SyncSlot& s, uint32_t seq, uint32_t componentId, uint64_t telNs) : prev(tCur()), prevLedger(tLedger()) {
      s.tick = l.currentTick(); s.seq = seq; s.componentId = componentId;
      s.payloadHash = 0; s.asyncSpawned = 0; s.telStartNs = telNs;
      tCur() = &s; tLedger() = &l;
    }
    ~JobScope() { tCur() = prev; tLedger() = prevLedger; }
    JobScope(const JobScope&) = delete;
    JobScope& operator=(const JobScope&) = delete;
  };
  /// Sync rule: a sync job returns in its own tick, so returnTick = tick.
  static void finishJob(SyncSlot& s, uint64_t telNs) { s.returnTick = s.tick; s.telEndNs = telNs; s.filled = 1; }

  /// From inside a job: fold a deterministic payload hash into this job's record.
  static void notePayload(uint64_t h) {
    if (SyncSlot* s = tCur()) s->payloadHash = mix(s->payloadHash ^ 0x9e3779b97f4a7c15ULL, h);
  }

  /// From inside a job: register an async job committed latencyTicks (>= 1) ticks later.
  /// Returns a handle for asyncComplete(), or kNoHandle when called outside a job.
  static uint64_t asyncBegin(uint32_t latencyTicks) {
    SyncSlot* s = tCur();
    cTickLedger* l = tLedger();
    if (!s || !l) { logError("tick ledger: asyncBegin outside a handshake job (ignored)"); return kNoHandle; }
    const uint64_t h = l->asyncHead_.fetch_add(1, std::memory_order_acq_rel);
    AsyncRec& r = l->async_[h & l->asyncMask_];
    if (r.state.load(std::memory_order_acquire) != kFree) {
      logError("tick ledger: async ring full (%u records pending); raise the ledger capacity", l->asyncMask_ + 1);
      std::abort();
    }
    r.spawnTick = s->tick;
    r.dispatchSeq = (uint64_t(s->seq) << 32) | s->asyncSpawned++;
    r.dueTick = s->tick + (latencyTicks < 1 ? 1 : latencyTicks);
    r.payloadHash = 0;
    r.observedReturnTick = 0;
    r.state.store(kPending, std::memory_order_release);
    return h;
  }
  /// Any thread: the async job finished with this payload hash.
  void asyncComplete(uint64_t handle, uint64_t payloadHash) {
    if (handle == kNoHandle) return;
    AsyncRec& r = async_[handle & asyncMask_];
    r.observedReturnTick = currentTick();  // audit only
    r.payloadHash = payloadHash;
    uint32_t st = kPending;
    if (!r.state.compare_exchange_strong(st, kDone, std::memory_order_acq_rel) && st == kCancelled)
      r.state.store(kFree, std::memory_order_release);  // its tick never committed
  }

  /// Barrier sweep (authority thread, after every shard arrived, before the commit hook).
  /// Linear over the tick's sync slots in seq order, then the async records due now
  /// (waiting for unfinished ones until `deadlineNs`). Returns false on a watchdog trip.
  bool sweep(uint64_t deadlineNs) {
    const uint64_t t0 = lnow();
    const uint64_t tick = epoch_, commitEpoch = epoch_ + 1;
    uint64_t h = mix(chain_, tick);
    h = mix(h, k_);
    for (uint32_t s = 0; s < k_; ++s) {
      const SyncSlot& r = ring_[(tOffset_ + s) & mask_];
      h = mix(h, r.filled ? 1 : 0xdead);
      h = mix(h, r.tick); h = mix(h, r.seq); h = mix(h, r.componentId);
      h = mix(h, r.returnTick); h = mix(h, r.payloadHash);
    }
    uint64_t asyncSum = 0, asyncN = 0, waitNs = 0;
    const uint64_t head = asyncHead_.load(std::memory_order_acquire);
    while (asyncLow_ < head && async_[asyncLow_ & asyncMask_].state.load(std::memory_order_acquire) == kFree) ++asyncLow_;
    for (uint64_t i = asyncLow_; i < head; ++i) {
      AsyncRec& r = async_[i & asyncMask_];
      uint32_t st = r.state.load(std::memory_order_acquire);
      if (st == kFree || st == kCancelled || r.dueTick != tick) continue;
      if (st == kPending) {
        const uint64_t w0 = lnow();
        ++stats_.asyncLate;
        while ((st = r.state.load(std::memory_order_acquire)) == kPending) {
          if (lnow() > deadlineNs) return false;
          std::this_thread::yield();
        }
        waitNs += lnow() - w0;
      }
      uint64_t a = mix(0xa5a5a5a5a5a5a5a5ULL, r.spawnTick);
      a = mix(a, r.dispatchSeq); a = mix(a, commitEpoch); a = mix(a, r.payloadHash);
      asyncSum += a;
      ++asyncN;
      r.state.store(kFree, std::memory_order_release);
    }
    h = mix(h, asyncN);
    h = mix(h, asyncSum);
    pendingHash_ = h;
    stats_.asyncCommitted += asyncN;
    stats_.asyncWaitNsTotal += waitNs;
    const uint64_t ns = lnow() - t0 - waitNs + openNs_;
    stats_.overheadNsTotal += ns;
    if (ns > stats_.overheadNsMax) stats_.overheadNsMax = ns;
    if (tickprof::enabled()) tickprof::ledger().add(ns, k_, asyncN);
    return true;
  }

  /// COMMITTED: advance commit_epoch, chain the tick hash, move T_offset past this tick.
  void commit() {
    chain_ = pendingHash_;
    lastTickHash_ = pendingHash_;
    tOffset_ += k_;
    stats_.syncRecords += k_;
    ++stats_.ticks;
    open_ = false;
    epoch_++;
    epochAtomic_.store(epoch_, std::memory_order_release);
    openTick_.store(epoch_, std::memory_order_release);
  }

  /// Ledger disabled (Config::ledger = false, overhead A/B only): COMMITTED still
  /// advances commit_epoch.
  void commitEpochOnly() {
    epoch_++;
    epochAtomic_.store(epoch_, std::memory_order_release);
    openTick_.store(epoch_, std::memory_order_release);
  }

  /// A frozen (never committed) tick: its sync slots are rewritten when the tick reopens;
  /// async jobs it spawned are cancelled (their completion just frees the record).
  void abandonOpenTick() {
    if (!open_) return;
    const uint64_t tick = epoch_;
    const uint64_t head = asyncHead_.load(std::memory_order_acquire);
    for (uint64_t i = asyncLow_; i < head; ++i) {
      AsyncRec& r = async_[i & asyncMask_];
      if (r.spawnTick != tick) continue;
      uint32_t st = kPending;
      if (r.state.compare_exchange_strong(st, kCancelled, std::memory_order_acq_rel)) { ++stats_.asyncCancelled; continue; }
      if (st == kDone) { r.state.store(kFree, std::memory_order_release); ++stats_.asyncCancelled; }
    }
    open_ = false;
  }

  uint64_t commitEpoch() const { return epochAtomic_.load(std::memory_order_acquire); }
  uint64_t chainHash() const { return chain_; }
  uint64_t lastTickHash() const { return lastTickHash_; }
  const Stats& stats() const { return stats_; }  // authority thread (or stopped)
  uint32_t ringCapacity() const { return mask_ + 1; }
  uint32_t asyncCapacity() const { return asyncMask_ + 1; }

  static uint64_t mix(uint64_t h, uint64_t v) {
    uint64_t z = h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

 private:
  static uint64_t lnow() { return uint64_t(tickprof::nowNs()); }
  static SyncSlot*& tCur() { thread_local SyncSlot* s = nullptr; return s; }
  static cTickLedger*& tLedger() { thread_local cTickLedger* l = nullptr; return l; }

  cTickArena arena_;
  SyncSlot* ring_ = nullptr;
  uint32_t mask_ = 0;
  AsyncRec* async_ = nullptr;
  uint32_t asyncMask_ = 0;
  std::atomic<uint64_t> asyncHead_{0};
  uint64_t asyncLow_ = 0;
  uint64_t tOffset_ = 0;
  uint32_t k_ = 0;
  bool open_ = false;
  uint64_t epoch_ = 0;
  std::atomic<uint64_t> epochAtomic_{0};
  std::atomic<uint64_t> openTick_{0};
  uint64_t chain_ = 0x6c656467657231ULL, lastTickHash_ = 0, pendingHash_ = 0, openNs_ = 0;
  Stats stats_;
};

}  // namespace litholattice
