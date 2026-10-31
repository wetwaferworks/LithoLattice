// LL_HITCH_CAPTURE=1 (JD 2026-10-31): lock-free per-frame stage timer. Mesh workers add their
// gather / mesh / vertex nanoseconds with relaxed atomics; the main thread takes and clears them
// once per frame and, when the frame exceeds 2x the running median of the last 256 frames, logs a
// breakdown (main-thread stages plus worker stage totals that landed in that frame).
// Default off: enabled() is a one-time getenv; when off nothing is timed or logged.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace litholattice {

class cHitchCapture {
 public:
  enum Stage : int { kGather = 0, kMesh = 1, kVertex = 2, kStages = 3 };
  struct Taken { double ms[kStages]; uint64_t jobs; double maxJobMs; };

  static bool enabled() {
    static const bool on = [] { const char* e = std::getenv("LL_HITCH_CAPTURE"); return e && e[0] == '1'; }();
    return on;
  }
  static uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  /// Worker side (any thread).
  static void add(Stage s, uint64_t ns) { acc(s).fetch_add(ns, std::memory_order_relaxed); }
  static void addJob(uint64_t totalNs) {
    jobs().fetch_add(1, std::memory_order_relaxed);
    uint64_t m = maxJob().load(std::memory_order_relaxed);
    while (totalNs > m && !maxJob().compare_exchange_weak(m, totalNs, std::memory_order_relaxed)) {}
  }
  /// Main thread, once per frame: take and clear the worker totals.
  static Taken take() {
    Taken t{};
    for (int i = 0; i < kStages; ++i) t.ms[i] = double(acc(Stage(i)).exchange(0, std::memory_order_relaxed)) * 1e-6;
    t.jobs = jobs().exchange(0, std::memory_order_relaxed);
    t.maxJobMs = double(maxJob().exchange(0, std::memory_order_relaxed)) * 1e-6;
    return t;
  }
  /// Main thread: push this frame's time; returns the median of the previous window (0 until 32 frames).
  double noteFrame(double ms) {
    const double med = n_ >= 32 ? median_ : 0.0;
    ring_[pos_] = float(ms);
    pos_ = (pos_ + 1) & 255;
    if (n_ < 256) ++n_;
    if (++sinceMedian_ >= 16 && n_ >= 32) {  // refresh every 16 frames (nth_element over <= 256)
      sinceMedian_ = 0;
      std::array<float, 256> tmp = ring_;
      std::nth_element(tmp.begin(), tmp.begin() + n_ / 2, tmp.begin() + n_);
      median_ = tmp[n_ / 2];
    }
    return med;
  }

 private:
  static std::atomic<uint64_t>& acc(Stage s) { static std::atomic<uint64_t> a[kStages]; return a[s]; }
  static std::atomic<uint64_t>& jobs() { static std::atomic<uint64_t> j{0}; return j; }
  static std::atomic<uint64_t>& maxJob() { static std::atomic<uint64_t> m{0}; return m; }
  std::array<float, 256> ring_{};
  size_t pos_ = 0, n_ = 0;
  int sinceMedian_ = 0;
  double median_ = 0.0;
};

}  // namespace litholattice
