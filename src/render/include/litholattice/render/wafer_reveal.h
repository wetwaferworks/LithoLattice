#pragma once
// Thread Handshake P7 - the wafer reveal (render-only; never touches sim state).
//
// The world appears ring by ring around the spawn column: a pedestal (the
// innermost rings) at t = 0, then each Chebyshev ring k = max(|dx|, |dz|) in
// spiral order. The pacer gates a ring on BOTH its slot (rings at v_etch per
// second) and its columns being meshed (or a timeout), so the etch never
// races ahead of streaming. Each column's reveal time ("spawn timestamp")
// goes to the GPU in a storage buffer; the chunk shader hides a column until
// its time and raises it into place over etchSec. When the last ring has
// etched, SKYBOX_ONLINE: the reveal turns off and the sky comes up.
// v_etch <= 0 or infinite (--bench) reveals everything at once.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace litholattice {

constexpr int kRevealHalf = 32;                  ///< SSBO window: columns within Chebyshev 32 of the origin
constexpr int kRevealSide = 2 * kRevealHalf + 1;  ///< 65
constexpr float kRevealHidden = 1.0e30f;          ///< spawn timestamp of a column not yet scheduled
constexpr float kRevealInstant = -1.0e9f;         ///< spawn timestamp of a column revealed at once

/// Per-frame reveal input for the renderer (Vulkan: set 0, binding 7).
struct WaferRevealFrame {
  bool active = false;   ///< false = draw everything normally
  float nowSec = 0.f;    ///< reveal clock (frozen while the render pipeline is paused)
  float etchSec = 0.6f;  ///< rise time of one column
  float riseBlocks = 24.f;
  int originCx = 0, originCz = 0;
  std::vector<float> spawnTs;  ///< kRevealSide^2, row-major [dz + half][dx + half]
};

inline int revealRing(int dx, int dz) { return std::max(std::abs(dx), std::abs(dz)); }

/// Position of (dx, dz) along its ring's perimeter, 0 .. 8k-1 (ring 0: 0). Starts just
/// above (k, -k) and runs counter-clockwise: +x side, +z side, -x side, -z side.
inline int revealSpiralIndex(int dx, int dz) {
  const int k = revealRing(dx, dz);
  if (k == 0) return 0;
  if (dx == k && dz > -k) return dz + k - 1;
  if (dz == k && dx < k) return 2 * k + (k - 1 - dx);
  if (dx == -k && dz < k) return 4 * k + (k - 1 - dz);
  return 6 * k + (dx + k - 1);  // dz == -k && dx > -k
}

class cWaferRevealPacer {
 public:
  struct Config {
    float vEtch = 6.f;  ///< rings per second; <= 0 or infinite = instant
    float etchSec = 0.6f;
    float riseBlocks = 24.f;
    int pedestalRings = 1;  ///< rings 0..pedestalRings appear at t = 0
    int maxRing = 16;       ///< the last ring the pacer waits for (clamped to kRevealHalf)
    float ringTimeoutSec = 2.f;  ///< a ring whose columns never arrive etches anyway after this
  };
  static bool instant(float vEtch) { return !(vEtch > 0.f) || std::isinf(vEtch); }

  void begin(int originCx, int originCz, const Config& c) {
    cfg_ = c;
    cfg_.maxRing = std::clamp(cfg_.maxRing, 0, kRevealHalf);
    cfg_.pedestalRings = std::clamp(cfg_.pedestalRings, 0, cfg_.maxRing);
    ocx_ = originCx; ocz_ = originCz;
    now_ = 0.f; lastRingT_ = 0.f; waitStart_ = -1.f; lastTs_ = 0.f;
    active_ = true; online_ = false;
    ts_.assign(size_t(kRevealSide) * kRevealSide, kRevealHidden);
    if (instant(cfg_.vEtch)) {
      std::fill(ts_.begin(), ts_.end(), kRevealInstant);
      nextRing_ = cfg_.maxRing + 1;
      lastTs_ = kRevealInstant;
      online_ = true;
      return;
    }
    for (int k = 0; k <= cfg_.pedestalRings; ++k) scheduleRing(k, 0.f);
    nextRing_ = cfg_.pedestalRings + 1;
    lastRingT_ = 0.f;
  }
  void stop() { active_ = false; }

  /// Advance the reveal clock by dt (pass 0 while paused). ready(cx, cz) says whether a
  /// column (absolute chunk coordinates) is meshed and resident.
  template <class Ready>
  void advance(float dtSec, Ready&& ready) {
    if (!active_ || online_) return;
    now_ += std::max(0.f, dtSec);
    const float slot = 1.f / cfg_.vEtch;
    while (nextRing_ <= cfg_.maxRing) {
      const int k = nextRing_;
      const float earliest = lastRingT_ + slot;
      if (now_ < earliest) break;
      if (waitStart_ < 0.f) waitStart_ = now_;
      bool all = true;
      for (int dz = -k; dz <= k && all; ++dz)
        for (int dx = -k; dx <= k; ++dx)
          if (revealRing(dx, dz) == k && !ready(ocx_ + dx, ocz_ + dz)) { all = false; break; }
      if (!all && now_ - waitStart_ < cfg_.ringTimeoutSec) break;
      const float t = std::max(earliest, waitStart_);
      scheduleRing(k, t);
      lastRingT_ = t;
      waitStart_ = -1.f;
      ++nextRing_;
    }
    if (nextRing_ > cfg_.maxRing && now_ >= lastTs_ + cfg_.etchSec) online_ = true;
  }

  bool active() const { return active_; }
  bool skyboxOnline() const { return online_; }
  float now() const { return now_; }
  int ringsScheduled() const { return nextRing_; }
  float spawnTs(int dx, int dz) const {
    if (revealRing(dx, dz) > kRevealHalf) return kRevealHidden;
    return ts_[size_t(dz + kRevealHalf) * kRevealSide + size_t(dx + kRevealHalf)];
  }
  /// Renderer input; inactive once SKYBOX_ONLINE (everything draws normally).
  void fill(WaferRevealFrame& f) const {
    f.active = active_ && !online_;
    f.nowSec = now_; f.etchSec = cfg_.etchSec; f.riseBlocks = cfg_.riseBlocks;
    f.originCx = ocx_; f.originCz = ocz_;
    if (f.active) f.spawnTs = ts_;
  }

 private:
  void scheduleRing(int k, float t) {
    const int n = k == 0 ? 1 : 8 * k;
    const float span = k <= cfg_.pedestalRings ? 0.f : 1.f / cfg_.vEtch;  // the spiral sweeps the ring's slot
    for (int dz = -k; dz <= k; ++dz)
      for (int dx = -k; dx <= k; ++dx)
        if (revealRing(dx, dz) == k) {
          const float ts = t + span * float(revealSpiralIndex(dx, dz)) / float(n);
          ts_[size_t(dz + kRevealHalf) * kRevealSide + size_t(dx + kRevealHalf)] = ts;
          lastTs_ = std::max(lastTs_, ts);
        }
  }
  Config cfg_{};
  int ocx_ = 0, ocz_ = 0, nextRing_ = 0;
  float now_ = 0.f, lastRingT_ = 0.f, waitStart_ = -1.f, lastTs_ = 0.f;
  bool active_ = false, online_ = false;
  std::vector<float> ts_;
};

}  // namespace litholattice
