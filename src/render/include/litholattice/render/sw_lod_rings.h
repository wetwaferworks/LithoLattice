// PlayB (JD 2026-11-04, Elfie far-LOD brief): the one source of the software far-LOD ring ladder,
// shared by the game (classification / meshing) and the software renderer (ring bins).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace litholattice {
/// Far-LOD ring edges (Chebyshev chunk distance from the player), clamped to the view radius.
/// Designed ladder: a 32-chunk full-fidelity core, then rings to 64 / 96 (JD 2026-10-28).
/// The mesher is hard-bounded to the view: no ring edge lies beyond it (see setView).
/// Lock-free: the edges are relaxed atomics, written by the game thread, read by mesh workers.
class cSwLodRings final {
 public:
  static constexpr int kDefault[3] = {32, 64, 96};
  /// Sets the designed edges (LL_SW_LOD_RINGS) and the view radius; returns whether LOD is live.
  bool configure(const int design[3], int view) {
    for (int i = 0; i < 3; ++i) design_[i] = design[i];
    return setView(view);
  }
  /// Archie Phase 1: the user's far-LOD detail (settings slider): 0 off, 1 high (edges x1.5),
  /// 2 balanced (the designed ladder, default), 3 fast (edges x0.5). Call setView afterwards.
  void setDetail(int d) { detail_.store(std::clamp(d, 0, 3), std::memory_order_relaxed); }
  int detail() const { return detail_.load(std::memory_order_relaxed); }
  /// Re-clamps for a new view radius. Returns whether LOD is live (view > ring 0).
  bool setView(int view) {
    view_.store(view, std::memory_order_relaxed);
    // PlayB (Elfie LOD report, 2026-11-04): inside the full-fidelity core (view <= ring 0) the ladder
    // scales with the view so the triangle diet still engages on the near resident world:
    // edge_i = design_i * view / design_2 ({32,64,96} at view 6 -> {2,4,6}, the October ladder).
    // view > ring 0: the designed edges clamped to the view (view 48+: {32,64,96} as designed).
    // LL_SW_LOD_SCALE=0: no scaling (LOD off when view <= ring 0, JD's strict hard-bound reading).
    static const bool kScale = [] { const char* e = std::getenv("LL_SW_LOD_SCALE"); return !(e && e[0] == '0'); }();
    const bool core = view <= design_[0];
    const int det = detail_.load(std::memory_order_relaxed);
    const int num = det == 1 ? 3 : 1, den = det == 3 ? 2 : det == 1 ? 2 : 1;
    const bool live = det != 0 && view > 0 && (!core || kScale);
    for (int i = 0; i < 3; ++i) {
      int e = view;
      if (live) e = core ? std::max(1, design_[i] * view / std::max(1, design_[2])) : std::min(design_[i], view);
      if (live) e = std::min(view, std::max(1, e * num / den));  // detail 2: unchanged
      edge_[i].store(e, std::memory_order_relaxed);
    }
    live_.store(live, std::memory_order_relaxed);
    return live;
  }
  bool live() const { return live_.load(std::memory_order_relaxed); }
  int view() const { return view_.load(std::memory_order_relaxed); }
  int edge(int i) const { return edge_[i].load(std::memory_order_relaxed); }
  /// LOD level 0..3 at Chebyshev distance d (0 when LOD is not live; nothing beyond the view steps).
  int level(int d) const {
    if (!live()) return 0;
    return d > edge(2) ? 3 : d > edge(1) ? 2 : d > edge(0) ? 1 : 0;
  }
  /// Process-wide instance the software renderer's ring bins read (the game configures it).
  static cSwLodRings& shared() { static cSwLodRings r; return r; }

 private:
  int design_[3] = {kDefault[0], kDefault[1], kDefault[2]};
  std::atomic<int> edge_[3]{kDefault[0], kDefault[1], kDefault[2]};
  std::atomic<int> view_{1 << 20};
  std::atomic<bool> live_{true};
  std::atomic<int> detail_{2};
};
}  // namespace litholattice
