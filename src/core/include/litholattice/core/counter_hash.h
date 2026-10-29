#pragma once
// Counter-based hashing (Litho Strike P4 -> engine, 2026-10-29): stateless,
// order-free random values keyed by counters (seed, tick, player, shot,
// stream), so every peer and every thread count derives the same value.
// It shares the random-tick mixer: counterMix64 IS detMix64
// (litholattice/sim/det_sched.h), both spelled through bitops::splitmixMix64.
#include <cstdint>

#include "litholattice/core/bitops.h"

namespace litholattice {

inline constexpr uint64_t kCounterGolden = 0x9E3779B97F4A7C15ULL;
inline constexpr uint64_t kCounterOdd = 0x2545F4914F6CDD1DULL;

/// splitmix64 step: splitmixMix64(x + golden). Bit-exact with detMix64.
constexpr uint64_t counterMix64(uint64_t x) { return bitops::splitmixMix64(x + kCounterGolden); }

/// The counter hash. Bit-exact with Litho Strike's shotHash (strike_random.h);
/// fixed vectors are pinned in engine_tests (core).
constexpr uint64_t counterHash(uint64_t seed, uint64_t tick, uint32_t player, uint32_t index, uint32_t stream) {
  uint64_t h = seed + kCounterGolden;
  h ^= bitops::splitmixMix64(tick + kCounterGolden);
  h ^= bitops::splitmixMix64(uint64_t(player) * kCounterOdd + stream);
  h ^= bitops::splitmixMix64(uint64_t(index) * kCounterGolden + uint64_t(stream) * kCounterOdd);
  return bitops::splitmixMix64(h);
}

/// Top 53 bits as a double in [0, 1) (exact conversion).
constexpr double counterUnit(uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }

}  // namespace litholattice