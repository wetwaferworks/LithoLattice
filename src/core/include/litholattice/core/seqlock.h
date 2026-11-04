// Single-writer seqlock over a small tuple of 64-bit words (P0.5 review fix 1, 2026-10-29).
//
// Why: the Timing Handshake publication tuple (phase, cycle, group, open stamp, seq)
// was revalidated by re-reading a counter that the writer only bumped AFTER storing
// the fields, so a reader could accept a tuple mixing two publications (external
// review by Cryosis (sirlinhart)). This is the textbook fix:
//   writer: seq = odd (relaxed), release fence, store words (relaxed), seq = even (release)
//   reader: s1 = seq (acquire); odd -> busy; load words (relaxed); acquire fence;
//           s2 = seq (relaxed); accept only if s1 == s2.
// Every word is its own std::atomic, so a concurrent read is never a data race (no UB);
// the sequence check is what makes the tuple consistent.
// Exactly ONE writer at a time (callers serialize writers, e.g. under a mutex).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace litholattice {

template <size_t N>
class cSeqLockTuple {
 public:
  void write(const uint64_t (&in)[N]) {
    const uint64_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);         // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);  // odd seq visible before any word
    for (size_t i = 0; i < N; ++i) words_[i].store(in[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);         // even: words visible before this
  }
  /// One attempt. Returns false if a write was in progress or raced the read.
  bool tryRead(uint64_t (&out)[N]) const {
    const uint64_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1u) return false;
    for (size_t i = 0; i < N; ++i) out[i] = words_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);  // word loads complete before re-check
    return seq_.load(std::memory_order_relaxed) == s1;
  }
  /// Spins until a consistent snapshot is read (readers never block the writer).
  void read(uint64_t (&out)[N]) const {
    while (!tryRead(out)) {
    }
  }
  uint64_t version() const { return seq_.load(std::memory_order_acquire); }

 private:
  std::atomic<uint64_t> seq_{0};
  std::atomic<uint64_t> words_[N]{};
};

}  // namespace litholattice