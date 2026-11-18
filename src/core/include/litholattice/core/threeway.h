#pragma once
// Three-way split timing handshake toggle (LL_THREEWAY=1, default OFF). Proposed by JD; co-implemented by JD and Cortana.
// Main thread: window, OS events, input, game logic. Render thread: GPU and memory work from a finished
// frame snapshot. Sim thread: unchanged ticks; point reads get a short between-tick handoff of blockMutex_.
#include <cstdlib>
namespace litholattice::threeway {
inline bool enabled() {
  static const bool on = [] { const char* e = std::getenv("LL_THREEWAY"); return e && e[0] == '1'; }();
  return on;
}
// t46u item 2 (JD 2026-11-16): the renderer lock and its guard are gone. While the split runs,
// the render thread owns the renderer and the game reaches it only through the command
// mailbox (litholattice/render/renderer_mailbox.h), applied at the frame seal.
}  // namespace litholattice::threeway
