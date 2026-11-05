#pragma once
// Three-way split timing handshake toggle (LL_THREEWAY=1, default OFF). Proposed by JD; co-implemented by JD and Cortana.
// Main thread: window, OS events, input, game logic. Render thread: GPU and memory work from a finished
// frame snapshot. Sim thread: unchanged ticks; point reads get a short between-tick handoff of blockMutex_.
#include <cstdlib>
#include "litholattice/core/block_gate.h"
namespace litholattice::threeway {
inline bool enabled() {
  static const bool on = [] { const char* e = std::getenv("LL_THREEWAY"); return e && e[0] == '1'; }();
  return on;
}
/// Serialises the game's renderer calls (mesh/entity uploads, settings, grabs) with the render thread's renderFrame.
inline cBlockGate& rendererMutex() { static cBlockGate m; return m; }
/// No-op unless LL_THREEWAY=1, so the default path is unchanged.
struct RendererGuard {
  bool held = false;
  RendererGuard() { if (enabled()) { rendererMutex().lock(); held = true; } }
  ~RendererGuard() { release(); }
  void release() { if (held) { rendererMutex().unlock(); held = false; } }
  RendererGuard(const RendererGuard&) = delete;
  RendererGuard& operator=(const RendererGuard&) = delete;
};
}  // namespace litholattice::threeway
