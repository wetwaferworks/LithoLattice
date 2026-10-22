// Bit-manipulation primitives — the single, tested home for the tree's
// hot bit math (build 59 sweep; JD's ruling 2026-10-20: bit shifting,
// packing, OR/XOR/AND done directly, "so nothing gets mangled by the
// compiler", defined once in the build environment and used everywhere).
//
// Contract for every function here:
// - BIT-EXACT with the expression it replaces, on every platform. These
//   are value-preserving spellings of the tree's existing ops, adopted
//   only where a differential test against the original expression
//   passes (tools/bitops_diff_test.cpp). Anything whose optimization
//   would change a value — e.g. re-associating the FNV-1a byte loop in
//   DeterministicScheduler::hashBytes — is NOT adopted, by the hash law.
// - Header-only, constexpr where the original was, always-inline: the
//   call must never cost more than the expression it spells.
// - Portable C++ is the canonical form. Compiler builtins are used only
//   where they spell the identical operation; inline assembly was
//   evaluated for the x86-64 hot ops and is included only where it
//   provably lowers better than the portable form (see the build-59 run
//   summary for the lowering comparison) — hand asm that merely matches
//   the compiler is maintenance debt, not speed.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
#define LL_BITOPS_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define LL_BITOPS_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define LL_BITOPS_ALWAYS_INLINE inline
#endif

namespace litholattice::bitops {

/// Low-`w`-bits mask. Domain: w in [0, 64) — the same domain every
/// original expression in the tree assumes (palette widths, field
/// widths); w == 64 would shift by the full width, exactly as the
/// originals would.
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t mask64(int w) { return (uint64_t(1) << w) - 1; }
constexpr LL_BITOPS_ALWAYS_INLINE uint32_t mask32(int w) { return (uint32_t(1) << w) - 1; }

/// Rotations. Every compiler in the tree's matrix (MSVC, GCC, Clang,
/// Clang-WASM — which has a native i64.rotl) recognizes this idiom and
/// lowers it to a single rotate; k == 0 is defined here (returns x),
/// where the bare idiom would shift by 64.
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t rotl64(uint64_t x, unsigned k) {
  k &= 63u;
  return (x << k) | (x >> ((64u - k) & 63u));
}
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t rotr64(uint64_t x, unsigned k) {
  k &= 63u;
  return (x >> k) | (x << ((64u - k) & 63u));
}

/// The SplitMix64 mixer (Stafford's finalizer), WITHOUT the golden-
/// gamma increment: procgen::mix64 adds the increment itself, so
/// mix64(z) == splitmixMix64(z + 0x9E3779B97F4A7C15) bit-for-bit.
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t splitmixMix64(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// One FNV-1a step (multiply after XOR — order is the format; the byte
/// loop built from this step is never re-associated).
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t fnv1aStep(uint64_t h, uint8_t b) {
  return (h ^ uint64_t(b)) * 0x100000001B3ull;
}

/// Extract the `w`-bit field at bit offset `sh`: (v >> sh) & mask.
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t extractBits(uint64_t v, unsigned sh, int w) {
  return (v >> sh) & mask64(w);
}
constexpr LL_BITOPS_ALWAYS_INLINE uint32_t extractBits32(uint32_t v, unsigned sh, int w) {
  return (v >> sh) & mask32(w);
}

/// Insert the low `w` bits of `field` at bit offset `sh`, clearing the
/// field's bits in `dst` first: (dst & ~(mask << sh)) | ((field & mask) << sh).
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t insertBits(uint64_t dst, uint64_t field, unsigned sh, int w) {
  const uint64_t m = mask64(w);
  return (dst & ~(m << sh)) | ((field & m) << sh);
}
constexpr LL_BITOPS_ALWAYS_INLINE uint32_t insertBits32(uint32_t dst, uint32_t field, unsigned sh, int w) {
  const uint32_t m = mask32(w);
  return (dst & ~(m << sh)) | ((field & m) << sh);
}

/// Two-word spanning field read (the voxel palette's packed layout): the
/// field starts at bit offset `sh` of `lo` and may continue into `hi`.
/// Verbatim the original readPacked arithmetic.
constexpr LL_BITOPS_ALWAYS_INLINE uint64_t extractBits2(uint64_t lo, uint64_t hi, unsigned sh, int w) {
  uint64_t v = lo >> sh;
  if (sh + unsigned(w) > 64) v |= hi << (64 - sh);
  return v & mask64(w);
}

/// Two-word spanning field insert, in place on (lo, hi). Verbatim the
/// original writePacked arithmetic — including the spill term
/// (idx >> (64 - sh)), which is deliberately NOT masked beyond the field
/// width in the high word, exactly as the original store computed it.
constexpr LL_BITOPS_ALWAYS_INLINE void insertBits2(uint64_t& lo, uint64_t& hi, unsigned sh, int w, uint64_t idx) {
  const uint64_t m = mask64(w);
  lo = (lo & ~(m << sh)) | ((idx & m) << sh);
  if (sh + unsigned(w) > 64) {
    const uint64_t hm = (uint64_t(1) << (sh + unsigned(w) - 64)) - 1;
    hi = (hi & ~hm) | (idx >> (64 - sh));
  }
}

}  // namespace litholattice::bitops
