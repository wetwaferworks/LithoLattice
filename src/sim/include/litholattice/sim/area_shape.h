// Area shapes for the sim tick area and the stream (residency) footprint.
// Fix run 3 (2026-10-23, JD t2262u; Built by Cortana): one parser, one
// containment rule, shared by the engine, the game cvars, the CLI, env vars,
// the world save (level.json v3) and the demo record preamble.
//   Square  : max(|dx|,|dz|) <= r
//   Round   : dx^2 + dz^2 <= r^2 + r        (F56-7 disc)
//   Diamond : |dx| + |dz| <= r              (Manhattan; the DEFAULT)
// Parsing is case-insensitive. An unknown value NEVER falls back to square:
// it falls back to Diamond and reports ok=false so the caller logs a warning.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace litholattice {

enum class AreaShape : uint8_t { Square = 0, Round = 1, Diamond = 2 };
inline constexpr AreaShape kDefaultAreaShape = AreaShape::Diamond;

inline const char* areaShapeName(AreaShape s) {
  switch (s) {
    case AreaShape::Square: return "square";
    case AreaShape::Round: return "round";
    case AreaShape::Diamond: return "diamond";
  }
  return "diamond";
}

/// Case-insensitive parse of "square" | "round" | "diamond" (surrounding
/// spaces ignored). allowRound=false (the stream footprint has no disc mode)
/// treats "round" as unknown. Unknown -> Diamond, *ok = false.
inline AreaShape parseAreaShape(std::string_view v, bool* ok = nullptr, bool allowRound = true) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
  std::string low(v);
  for (char& c : low) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  bool good = true;
  AreaShape s = kDefaultAreaShape;
  if (low == "square") s = AreaShape::Square;
  else if (low == "diamond") s = AreaShape::Diamond;
  else if (low == "round" && allowRound) s = AreaShape::Round;
  else good = false;
  if (ok) *ok = good;
  return s;
}

/// Is chunk offset (dx, dz) inside a footprint of shape s and radius r?
inline bool areaContains(AreaShape s, int dx, int dz, int r) {
  const int ax = std::abs(dx), az = std::abs(dz);
  switch (s) {
    case AreaShape::Square: return ax <= r && az <= r;
    case AreaShape::Round: return int64_t(dx) * dx + int64_t(dz) * dz <= int64_t(r) * r + r;
    case AreaShape::Diamond: return ax + az <= r;
  }
  return ax + az <= r;
}

/// Smallest stream radius R such that a stream footprint (streamShape, R)
/// contains every chunk of the sim area (simShape, simR): the sim area must
/// always sit inside the stream area, so no ticking chunk is ever unloaded.
inline int minStreamRadiusCovering(AreaShape simShape, int simR, AreaShape streamShape) {
  if (simR <= 0) return 0;
  int need = 0;
  for (int dz = -simR; dz <= simR; ++dz)
    for (int dx = -simR; dx <= simR; ++dx) {
      if (!areaContains(simShape, dx, dz, simR)) continue;
      const int ax = std::abs(dx), az = std::abs(dz);
      int m = ax > az ? ax : az;  // Square metric
      if (streamShape == AreaShape::Diamond) m = ax + az;
      else if (streamShape == AreaShape::Round) {
        while (!areaContains(AreaShape::Round, dx, dz, m)) ++m;
      }
      if (m > need) need = m;
    }
  return need;
}

}  // namespace litholattice
