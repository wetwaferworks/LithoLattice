// The view radius in chunks (Chebyshev, from the player's column), shared by the game and
// the software renderer's draw-side view bound. 2026-11-17 (JD): this replaces the far-LOD
// ring ladder; with LOD gone, the view radius is the only distance the renderer needs.
// Lock-free: one relaxed atomic, written by the game thread, read by the render thread.
#pragma once
#include <atomic>

namespace litholattice {
class cViewRadius final {
 public:
  static constexpr int kUnbounded = 1 << 20;
  static void set(int chunks) { value().store(chunks, std::memory_order_relaxed); }
  static int get() { return value().load(std::memory_order_relaxed); }
  /// True when `chunks` (a Chebyshev chunk distance) lies outside a bounded view.
  static bool beyond(int chunks) {
    const int v = get();
    return v > 0 && v < kUnbounded && chunks > v;
  }

 private:
  static std::atomic<int>& value() { static std::atomic<int> v{kUnbounded}; return v; }
};
}  // namespace litholattice
