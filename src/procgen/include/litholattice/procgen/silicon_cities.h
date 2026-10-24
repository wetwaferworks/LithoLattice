// Silicon Cities (design and math: Argon; implementation: Cortana, 2026-10-16).
// Logic-based procedural interchanges on the Wafer: one hub per 256x256 die sector, linked to
// its neighbours by elevated bus decks (NS M1 at +8, EW M2 at +16) on Manhattan routes.
//
// Determinism rules (hash e015e51cf1559089 with cities off; the cities-on baseline is logged in
// CHANGES.md): integer math only (no floats, no <cmath>), C++20 shift semantics, every decision a
// pure function of (dimension seed, world x, z). Seeds: deriveSeed(dimSeed, nameHash("city")) and
// latticeHash. Each column computes its own share of every pad, building and deck, so pieces line
// up across chunk borders with no overlap replay.
#pragma once
#include <array>
#include <cstdint>
#include "litholattice/procgen/pipeline.h"
#include "litholattice/procgen/ramp_shape.h"

namespace litholattice::procgen {

enum class CityDistrict : uint8_t { Quarry, Sram, Clock, Alu, LithoHub };
/// Argon's 8-entry table: Quarry 3/8, SRAM 2/8, Clock / ALU / LithoHub 1/8 each.
inline constexpr std::array<CityDistrict, 8> kCityDistrictTable{
    CityDistrict::Quarry, CityDistrict::Quarry, CityDistrict::Quarry, CityDistrict::Sram,
    CityDistrict::Sram,   CityDistrict::Clock,  CityDistrict::Alu,    CityDistrict::LithoHub};
const char* cityDistrictName(CityDistrict d);

/// Argon's fixed-point Q16 quintic smoothstep 6t^5 - 15t^4 + 10t^3, t in Q6 [0, 64], clamped.
/// Returns 0..65536 exactly. Integer only.
constexpr int32_t quinticQ16(int32_t t) {
  const int64_t c = t < 0 ? 0 : (t > 64 ? 64 : t);
  const int64_t poly = 6 * c * c - 15 * 64 * c + 10 * 4096;  // Q12
  return int32_t((c * c * c * poly) >> 14);                   // Q18 * Q12 >> 14 = Q16
}
/// Bus bit-width: 4, 8, 16 or 32 from 2 hash bits.
constexpr uint32_t cityBusWidth(uint64_t h) { return 4u << ((h >> 24) & 3u); }
/// 1:2 ramp: height gained `s` blocks along the ramp, capped at `rise`.
constexpr int cityRampRise(int s, int rise) { return s <= 0 ? 0 : ((s >> 1) < rise ? (s >> 1) : rise); }

// Live sectors: 3/16 (hash bits 28..31). Unconnected bus links: 1/8 (separate link hash).
struct SiliconCitySettings {
  int baseY = 264;            ///< pad top (multiple of 4); decks at +8 (M1) and +16 (M2)
  int skirt = 8;              ///< terraced blend width around pads
  int clearAbove = 24;        ///< air cleared above pads (trees, hills)
  MaterialId pad = 0, foundation = 0, deck = 0, traceLive = 0, traceDormant = 0, guardrail = 0,
             pillar = 0, alu = 0, ramp = 0 /* optional: 0 = no ramps */, ramp2 = 0 /* optional Highway Ramp on step tops (JD; 0 = off, default) */, clock = 0, clockTop = 0, sram = 0, sramDormant = 0;
};

/// One hub, a pure function of (citySeed, sector).
struct CityHub {
  int32_t sx = 0, sz = 0;     ///< sector (x >> 8, z >> 8)
  int32_t x = 0, z = 0;       ///< hub centre in world blocks (sector origin + 64..191)
  uint32_t hash = 0;
  CityDistrict district = CityDistrict::Quarry;
  uint32_t busWidth = 4;
  int padRadius = 14;         ///< Chebyshev radius of the flat pad
  bool live = false;
};

uint64_t citySeed(uint64_t dimensionSeed);
CityHub cityHubAt(uint64_t citySeed, int32_t sx, int32_t sz);
/// Bus link from sector (sx, sz) to its east (eastWest) or south neighbour; false = unconnected.
bool cityLinkConnected(uint64_t citySeed, int32_t sx, int32_t sz, bool eastWest);

class cSiliconCitiesFeature final : public IFeature {
 public:
  explicit cSiliconCitiesFeature(SiliconCitySettings s) : s_(s) {}
  void place(StageContext& ctx, Rng& rng) const override;
  const SiliconCitySettings& settings() const { return s_; }

 private:
  SiliconCitySettings s_;
};

/// Process-wide --cities override: -1 = use the JSON "enabled" flag, 0 = off, 1 = on.
/// Set once at startup, before any generator loads.
void setSiliconCitiesOverride(int v);
int siliconCitiesOverride();

/// Selftest (silicon_cities_selftest.cpp): prints PASS/FAIL lines, returns failures.
int runSiliconCitiesSelfTest();

}  // namespace litholattice::procgen
