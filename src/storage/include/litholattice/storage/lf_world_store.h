// cLfWorldStore (JD 2026-11-01, LL_LF_SAVE / LL_LF_LOAD): the lock-free, asynchronous twin
// of cWorldStore. Same region files, same records, same codec (on-disk format unchanged).
//   - One I/O thread per dimension (as before). Jobs arrive on two lock-free MPSC handoffs:
//     the FIFO lane (saves, flushes, ordinary loads; strict order) and the load lane.
//   - Per-region atomic state: a pending-save counter per region (cLfKeyTable). A load whose
//     region has no queued save jumps the FIFO (it cannot miss a write: every earlier save to
//     that region is already in the region's pending records, which read() consults first).
//     A load into a region with a queued save waits its turn in the FIFO.
//   - Waits are per request (atomic flag + spin/yield/sleep), never "until the whole queue is
//     idle" (cWorldStore::load waited behind every queued save and region flush).
//   - Encode/decode context rides a thread_local pointer set around each encode/decode on the
//     I/O thread (replaces cWorldSave's mutex-guarded exporting_ / signalSink_ handoff).
// No mutex, no condition variable.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "litholattice/core/lf_handoff.h"
#include "litholattice/core/lf_key_table.h"
#include "litholattice/core/log.h"
#include "litholattice/storage/storage.h"

namespace litholattice::storage {

class cLfWorldStore {
 public:
  struct LoadReq {
    std::atomic<int> done{0};
    std::unique_ptr<ChunkColumn> column;
    std::string err;
    std::vector<uint8_t> sink;  ///< decode context (the game's SGNL bytes for this column)
  };

  cLfWorldStore(std::filesystem::path dir, const cPayloadReg& payloads) : dir_(std::move(dir)), payloads_(payloads) {
    std::filesystem::create_directories(dir_);
    thread_ = std::thread([this] { run(); });
  }
  ~cLfWorldStore() {
    flush();
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }
  cLfWorldStore(const cLfWorldStore&) = delete;
  cLfWorldStore& operator=(const cLfWorldStore&) = delete;

  /// thread_local contexts read by payload callbacks during encode / decode on the I/O thread.
  static void*& tlsEncodeContext() { static thread_local void* p = nullptr; return p; }
  static void*& tlsDecodeContext() { static thread_local void* p = nullptr; return p; }

  /// Takes the copy; encodes and writes on the I/O thread with `ctx` as the encode context.
  void saveAsync(std::unique_ptr<ChunkColumn> c, std::shared_ptr<void> ctx) {
    const uint64_t rk = regionKey(floorDiv(c->cx, cRegionFile::kSpan), floorDiv(c->cz, cRegionFile::kSpan));
    if (!pending_.add(rk, 1)) pendingSpill_.fetch_add(1, std::memory_order_acq_rel);
    post(q_, [this, rk, c = std::shared_ptr<ChunkColumn>(std::move(c)), ctx] {
      const int rx = floorDiv(c->cx, cRegionFile::kSpan), rz = floorDiv(c->cz, cRegionFile::kSpan);
      tlsEncodeContext() = ctx.get();
      Bytes rec = payloads_.encode(*c);
      tlsEncodeContext() = nullptr;
      region(rx, rz).put(c->cx - rx * cRegionFile::kSpan, c->cz - rz * cRegionFile::kSpan, std::move(rec));
      if (!pending_.add(rk, -1)) pendingSpill_.fetch_sub(1, std::memory_order_acq_rel);
    });
  }
  /// Queues a load; `r->done` turns 1 (release) when column/err/sink are filled.
  void loadAsync(const std::shared_ptr<LoadReq>& r, int cx, int cz) {
    const int rx = floorDiv(cx, cRegionFile::kSpan), rz = floorDiv(cz, cRegionFile::kSpan);
    const bool jump = pendingSpill_.load(std::memory_order_acquire) == 0 && pending_.get(regionKey(rx, rz)) == 0;
    if (jump) jumped_.fetch_add(1, std::memory_order_relaxed);
    post(jump ? lq_ : q_, [this, r, cx, cz, rx, rz] {
      std::string err;
      auto rec = region(rx, rz).read(cx - rx * cRegionFile::kSpan, cz - rz * cRegionFile::kSpan, &err);
      tlsDecodeContext() = &r->sink;
      std::unique_ptr<ChunkColumn> c = rec ? payloads_.decode(*rec, &err) : nullptr;
      tlsDecodeContext() = nullptr;
      r->column = std::move(c);
      r->err = std::move(err);
      r->done.store(1, std::memory_order_release);
    });
  }
  /// Synchronous load: waits for THIS request only. `sink` receives the decode-context bytes.
  std::unique_ptr<ChunkColumn> load(int cx, int cz, std::string* err, std::vector<uint8_t>* sink) {
    auto r = std::make_shared<LoadReq>();
    loadAsync(r, cx, cz);
    waitFlag(r->done);
    if (err) *err = std::move(r->err);
    if (sink) *sink = std::move(r->sink);
    return std::move(r->column);
  }
  void flushAsync(std::shared_ptr<std::atomic<int>> done = nullptr) {
    post(q_, [this, done] {
      FileWriteOptions opts;
      opts.deferDirFsync = true;
      int failures = 0;
      for (auto& [k, r] : regions_) {
        std::string err;
        if (!r->flush(&err, opts)) {
          ++failures;
          std::fprintf(stderr, "storage: %s\n", err.c_str());
          logError("storage (lock-free store): region flush failed (kept pending for retry): %s", err.c_str());
        }
      }
      lastFlushFailures_.store(failures, std::memory_order_relaxed);
      lastFlushOk_.store(failures == 0, std::memory_order_relaxed);
      syncDirContaining(dir_);
      if (done) done->store(1, std::memory_order_release);
    });
  }
  /// Waits for its own flush job (every earlier save is on disk). False when a region failed.
  bool flush() {
    auto d = std::make_shared<std::atomic<int>>(0);
    flushAsync(d);
    waitFlag(*d);
    return lastFlushOk();
  }
  bool lastFlushOk() const { return lastFlushOk_.load(std::memory_order_relaxed); }
  int lastFlushFailureCount() const { return lastFlushFailures_.load(std::memory_order_relaxed); }
  size_t quarantinedCount() const { return quarantined_.load(std::memory_order_relaxed); }
  uint64_t loadsJumped() const { return jumped_.load(std::memory_order_relaxed); }

  static void waitFlag(const std::atomic<int>& f) {
    unsigned spins = 0;
    while (f.load(std::memory_order_acquire) == 0) {
      if (++spins < 64) continue;
      if (spins < 512) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

 private:
  using Job = std::function<void()>*;
  static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
  static uint64_t regionKey(int rx, int rz) { return (uint64_t(uint32_t(rx)) << 32) | uint32_t(rz); }

  void post(cLfHandoff<Job>& lane, std::function<void()> fn) {
    lane.push(new std::function<void()>(std::move(fn)));
    gen_.fetch_add(1, std::memory_order_release);
  }
  void exec(Job j) {
    try { (*j)(); }
    catch (const std::exception& e) { logError("storage (lock-free store): job threw: %s", e.what()); }
    catch (...) { logError("storage (lock-free store): job threw a non-std exception"); }
    delete j;
  }
  void run() {
    std::vector<Job> loads, fifo;
    std::deque<Job> local;
    unsigned spins = 0;
    for (;;) {
      bool did = false;
      loads.clear();
      lq_.drainInto(loads);
      for (Job j : loads) { exec(j); did = true; }
      fifo.clear();
      q_.drainInto(fifo);
      for (Job j : fifo) local.push_back(j);
      if (!local.empty()) { Job j = local.front(); local.pop_front(); exec(j); did = true; }
      if (did) { spins = 0; continue; }
      if (stop_.load(std::memory_order_acquire) && local.empty() && lq_.sizeApprox() == 0 && q_.sizeApprox() == 0) return;
      if (++spins < 64) continue;
      if (spins < 512) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  cRegionFile& region(int rx, int rz) {  // I/O thread only
    auto& r = regions_[{rx, rz}];
    if (!r) {
      r = std::make_unique<cRegionFile>(cWorldStore::regionPath(dir_, rx, rz));
      std::string err;
      if (!r->open(&err)) {
        quarantined_.fetch_add(1, std::memory_order_relaxed);
        logError("storage: quarantined region r.%d.%d.llr (%s); continuing with an empty region", rx, rz, err.c_str());
      }
    }
    return *r;
  }

  std::filesystem::path dir_;
  const cPayloadReg& payloads_;
  std::map<std::pair<int, int>, std::unique_ptr<cRegionFile>> regions_;  // I/O thread only
  cLfHandoff<Job> q_, lq_;
  cLfKeyTable pending_{4096};
  std::atomic<int64_t> pendingSpill_{0};
  std::atomic<uint64_t> gen_{0}, jumped_{0};
  std::atomic<size_t> quarantined_{0};
  std::atomic<bool> lastFlushOk_{true}, stop_{false};
  std::atomic<int> lastFlushFailures_{0};
  std::thread thread_;
};

}  // namespace litholattice::storage
