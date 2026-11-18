// Force 16 (Mode 416): the render-side frame gate.
// Mode 416 brief: JD and Argon (WWW-LLE-416-20261117). The sim/render handshake it rides on
// (mutex-free published snapshots, picked at a phase boundary): JD and Elfie.
//
// The gate shows one frame per 2^shift world ticks: frame_mask = (1u << shift) - 1, and a
// tick T is a frame tick when (uint32_t(T) & frame_mask) == 0. Mode 416 is shift 5 (mask
// 0x1F): 512 TPS / 32 = 16 FPS. The gate never presents from inside a sim tick and never
// writes into the sim. The render side reads the sim's completed-tick count at a phase
// boundary (the published SimStats / flush front) and the gate only decides whether that
// count has reached a new frame tick since the last frame it showed. Because the sim
// publishes per pass (several ticks may complete in one pass), "reached" means the latest
// frame tick at or below the completed count is newer than the last one shown.
#pragma once
#include <cstdint>

namespace litholattice {

enum class RenderShiftMode : uint8_t { Sync512 = 0, Sync256, Sync128, Sync64, Sync32, Mode416 = 5 };
inline constexpr int kRenderShiftModeCount = 6;

/// Display names, indexed by mode (settings selector).
inline const char* renderShiftModeName(RenderShiftMode m) {
  switch (m) {
    case RenderShiftMode::Sync512: return "Sync512 (1:1)";
    case RenderShiftMode::Sync256: return "Sync256 (2:1)";
    case RenderShiftMode::Sync128: return "Sync128 (4:1)";
    case RenderShiftMode::Sync64:  return "Sync64 (8:1)";
    case RenderShiftMode::Sync32:  return "Sync32 (16:1)";
    case RenderShiftMode::Mode416: return "Mode 416 (32:1)";
  }
  return "?";
}

inline constexpr RenderShiftMode renderShiftModeFromInt(int v) {
  return v <= 0 ? RenderShiftMode::Sync512 : v >= 5 ? RenderShiftMode::Mode416 : RenderShiftMode(uint8_t(v));
}

class FrameGate {
 public:
  explicit FrameGate(RenderShiftMode m = RenderShiftMode::Mode416) { setMode(m); }

  void setMode(RenderShiftMode m) {
    shift_ = uint32_t(m);
    frame_mask = (1u << shift_) - 1u;
    reset();
  }
  RenderShiftMode mode() const { return RenderShiftMode(uint8_t(shift_)); }
  uint32_t shift() const { return shift_; }
  uint32_t ratio() const { return 1u << shift_; }  ///< world ticks per shown frame
  /// FPS the gate shows at a given TPS (512 TPS in Mode 416 -> 16).
  uint32_t fpsAt(uint32_t tps) const { return tps >> shift_; }

  /// The brief's rule: tick T is a frame tick when (uint32_t(T) & frame_mask) == 0.
  bool isFrameTick(uint64_t tick) const { return (uint32_t(tick) & frame_mask) == 0; }
  /// The newest frame tick at or below `completed` (a completed-tick count read at a phase boundary).
  uint64_t frameTickAtOrBelow(uint64_t completed) const { return completed & ~uint64_t(frame_mask); }

  /// Render side, once per frame-loop pass: `completed` is the sim's completed-tick count read
  /// from its published snapshot. True when a new frame tick has been reached since the last
  /// admitted frame (the caller then renders the published snapshot). Never touches the sim.
  bool admit(uint64_t completed) {
    const uint64_t ft = frameTickAtOrBelow(completed);
    if (hasShown_ && ft <= lastShown_) return false;
    hasShown_ = true;
    lastShown_ = ft;
    ++admitted_;
    return true;
  }
  void reset() { hasShown_ = false; lastShown_ = 0; admitted_ = 0; }
  uint64_t lastShownTick() const { return lastShown_; }
  uint64_t admitted() const { return admitted_; }

  uint32_t frame_mask = 31;  ///< (1u << shift) - 1

 private:
  uint32_t shift_ = 5;
  bool hasShown_ = false;
  uint64_t lastShown_ = 0;
  uint64_t admitted_ = 0;
};

}  // namespace litholattice
