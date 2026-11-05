// 2026-10-26 (JD F3 lag trace): LL_TRACE_F3=1 turns on a per-frame trace of the F3 XYZ line,
// from the movement step to the text the renderer drew. Off by default; when off the only
// cost is one cached bool test per hook. This header carries the renderer -> game handoff
// (the line the HUD rasterizer actually drew), which may be written on a render thread.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include "litholattice/core/atomic_gate.h"

namespace litholattice::f3trace {

inline bool on() {
  static const bool b = [] { const char* e = std::getenv("LL_TRACE_F3"); return e && e[0] == '1'; }();
  return b;
}

struct Drawn {
  litholattice::cAtomicGate m;
  std::string text;    // the HUD text item containing "XYZ", as handed to the HUD rasterizer
  uint64_t seq = 0;    // renderer frames that drew it
  size_t verts = 0;    // HUD vertices built that frame (built fresh every frame: there is no HUD mesh cache)
};
inline Drawn& drawn() { static Drawn d; return d; }
inline void noteDrawn(std::string_view t, size_t verts) {
  Drawn& d = drawn();
  litholattice::cGateGuard l(d.m);
  d.text.assign(t);
  ++d.seq;
  d.verts = verts;
}

}  // namespace litholattice::f3trace
