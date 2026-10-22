// Ramp (slope) block shape rules, shared by worldgen (Silicon Cities) and the game's placement
// hook (Cortana 2026-10-16, JD: smooth deck undersides and ramps). Pure constexpr integer code.
//
// State (5 bits, in the cell state's low "value" bits so it also fits procgen's 8-bit state):
//   bits 0-1 facing (0 N -Z, 1 E +X, 2 S +Z, 3 W -X): the HIGH (full-height) side
//   bits 2-3 shape  (0 straight, 1 inner corner, 2 outer corner; 3 unused)
//   bit  4   hanging (upside down: flat face up, slope underneath, for deck undersides)
// Corners are stored canonically as Minecraft's "left" variants: a "right" corner is the same
// solid as a "left" corner of the next facing clockwise, so 2 shape bits are enough.
#pragma once
#include <cstdint>

namespace litholattice::procgen::ramp {

enum Shape : int { kStraight = 0, kInner = 1, kOuter = 2 };

constexpr uint8_t encode(int facing, int shape, bool hanging) {
  return uint8_t((facing & 3) | ((shape & 3) << 2) | (hanging ? 16 : 0));
}
constexpr int facingOf(uint32_t s) { return int(s & 3u); }
constexpr int shapeOf(uint32_t s) { return int((s >> 2) & 3u); }
constexpr bool hangingOf(uint32_t s) { return ((s >> 4) & 1u) != 0; }

/// Is top corner (xi, zi) (0/1 each) on side d of the cell?
constexpr bool onSide(int d, int xi, int zi) {
  return d == 0 ? zi == 0 : d == 1 ? xi == 1 : d == 2 ? zi == 1 : xi == 0;
}
/// Is top corner (xi, zi) at full height for this state (before the hanging flip)?
constexpr bool cornerHigh(uint32_t s, int xi, int zi) {
  const int f = facingOf(s), l = (f + 3) & 3;
  switch (shapeOf(s)) {
    case kInner: return onSide(f, xi, zi) || onSide(l, xi, zi);
    case kOuter: return onSide(f, xi, zi) && onSide(l, xi, zi);
    default: return onSide(f, xi, zi);
  }
}

/// Minecraft-stair corner snapping. `at(dx, dz)` returns the facing of a ramp with the same
/// hanging flag in the horizontal neighbour (dx, dz), or -1. Returns the canonical state.
template <class At>
constexpr uint8_t snap(int f, bool hanging, At at) {
  constexpr int dx[4] = {0, 1, 0, -1}, dz[4] = {-1, 0, 1, 0};
  const int left = (f + 3) & 3, right = (f + 1) & 3;
  const int f1 = at(dx[f], dz[f]);  // neighbour on the high side
  if (f1 >= 0 && (f1 & 1) != (f & 1)) {
    const int o = (f1 + 2) & 3;
    if (at(dx[o], dz[o]) != f) return encode(f1 == left ? f : right, kOuter, hanging);
  }
  const int b = (f + 2) & 3;
  const int f2 = at(dx[b], dz[b]);  // neighbour on the low side
  if (f2 >= 0 && (f2 & 1) != (f & 1)) {
    if (at(dx[f2], dz[f2]) != f) return encode(f2 == left ? f : right, kInner, hanging);
  }
  return encode(f, kStraight, hanging);
}

/// Highway Ramp (Proposed by JD, Built by Cortana 2026-10-16): a 1-in-2 slope spread over two
/// cells placed as a pair. Straight only (no corners, no hanging). State bits 0-1 facing (the HIGH
/// side, same convention as above), bit 2 half: 0 = low half (rises 0 -> 8/16), 1 = high half
/// (8/16 -> 16). The partner of the low half is one cell toward the facing; the high half's partner
/// is one cell away from it.
namespace highway {
constexpr uint8_t encode(int facing, int half) { return uint8_t((facing & 3) | ((half & 1) << 2)); }
constexpr int facingOf(uint32_t s) { return int(s & 3u); }
constexpr int halfOf(uint32_t s) { return int((s >> 2) & 1u); }
constexpr int kDx[4] = {0, 1, 0, -1}, kDz[4] = {-1, 0, 1, 0};
/// Offset from this half to its partner.
constexpr int partnerDx(uint32_t s) { return halfOf(s) ? -kDx[facingOf(s)] : kDx[facingOf(s)]; }
constexpr int partnerDz(uint32_t s) { return halfOf(s) ? -kDz[facingOf(s)] : kDz[facingOf(s)]; }
/// Surface height in 1/16 block at (px, pz) in 1/16 units (0..16) inside the cell.
constexpr int height16(uint32_t s, int px, int pz) {
  const int f = facingOf(s);
  const int t = f == 0 ? 16 - pz : f == 1 ? px : f == 2 ? pz : 16 - px;
  return (halfOf(s) * 16 + t) / 2;
}
/// Same, continuous (fx, fz in [0, 1]); returns blocks (0..1). Used by player collision.
inline double heightAt(uint32_t s, double fx, double fz) {
  const int f = facingOf(s);
  const double t = f == 0 ? 1.0 - fz : f == 1 ? fx : f == 2 ? fz : 1.0 - fx;
  return (double(halfOf(s)) + t) * 0.5;
}
}  // namespace highway

}  // namespace litholattice::procgen::ramp
