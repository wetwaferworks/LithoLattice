// single_owner.h (mutexfree pass 2b, 2026-11-11): thread-safety CHECK for state that a phase design gives to ONE
// owner thread (e.g. the MCP owner thread: the stdio serve loop is the only MCP dispatch thread in LithoKraft).
// Not a lock: nothing waits. The first thread to call check() becomes the owner; any later call from a different
// thread is a design violation, counted and reported once on stderr. Cost: one relaxed load per call.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace litholattice {

class cSingleOwner {
 public:
  explicit cSingleOwner(const char* what) : what_(what) {}
  cSingleOwner(const cSingleOwner&) = delete;
  cSingleOwner& operator=(const cSingleOwner&) = delete;

  /// Claim on first use; afterwards verify the caller is the owner.
  void check() const {
    const std::thread::id me = std::this_thread::get_id();
    std::thread::id cur = owner_.load(std::memory_order_relaxed);
    if (cur == me) return;
    if (cur == std::thread::id{} && owner_.compare_exchange_strong(cur, me, std::memory_order_acq_rel)) return;
    if (cur == me) return;
    if (violations_.fetch_add(1, std::memory_order_relaxed) == 0)
      std::fprintf(stderr, "[single-owner] VIOLATION: %s used from a second thread (phase design broken)\n", what_);
  }
  /// Hand ownership back (the next caller claims it), e.g. when the owner thread is joined.
  void release() const { owner_.store(std::thread::id{}, std::memory_order_release); }
  uint32_t violations() const { return violations_.load(std::memory_order_relaxed); }

 private:
  const char* what_;
  mutable std::atomic<std::thread::id> owner_{};
  mutable std::atomic<uint32_t> violations_{0};
};

}  // namespace litholattice
