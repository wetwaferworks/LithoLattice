// t46u (JD 2026-11-16, lock-free doctrine): the one-writer / one-reader publish primitive.
//
// Three slots: the writer owns `back`, the reader owns `front`, and the third sits in the
// shared `middle` index. publish() swaps back <-> middle (marking it fresh); take() swaps
// middle <-> front when middle is fresh. One atomic exchange per side, wait-free, never
// torn, never blocks either side; the newest published value always wins. The only atomic
// is the middle index (own cache line). This is the phase-boundary handoff for frame jobs,
// finished frames and render-side read state (renderer_mailbox.h, main.cpp).
#pragma once
#include <atomic>
#include <cstdint>

namespace litholattice {

template <class T>
class cTripleBuffer {
 public:
  // ---- writer (one thread) ----
  T& back() { return buf_[back_]; }
  /// Hand `back` to the reader side; the writer gets the previous middle as its new back.
  void publish() { back_ = mid_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIdx; }
  /// True while the last published value has not been taken yet (writer-side pacing hint).
  bool pending() const { return (mid_.load(std::memory_order_acquire) & kFresh) != 0; }

  // ---- reader (one thread) ----
  /// Take the newest published value into `front`. False (front unchanged) when nothing new.
  bool take() {
    if ((mid_.load(std::memory_order_acquire) & kFresh) == 0) return false;
    front_ = mid_.exchange(front_, std::memory_order_acq_rel) & kIdx;
    return true;
  }
  T& front() { return buf_[front_]; }
  const T& front() const { return buf_[front_]; }

 private:
  static constexpr unsigned kIdx = 3u, kFresh = 4u;
  T buf_[3]{};
  unsigned back_ = 0;                          // writer-owned
  alignas(64) std::atomic<unsigned> mid_{1u};  // the only shared word
  alignas(64) unsigned front_ = 2;             // reader-owned
};

}  // namespace litholattice