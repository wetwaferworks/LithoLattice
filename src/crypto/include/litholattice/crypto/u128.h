// Cortana 2026-10-15 0438: PORT new ll_u128 type
// Portable unsigned 128-bit integer for the crypto code.
// GCC/Clang: the native unsigned __int128 (unchanged behaviour).
// MSVC (no native 128-bit type): a minimal hi/lo struct with exactly the
// operations sha512.cpp / ed25519.cpp use; results are bit-identical
// (mod 2^128 wraparound semantics, same as the native type).
#pragma once

#include <cstdint>
#include <type_traits>

namespace litholattice::crypto {

// wasm32 (S1, 2026-11-16): the struct too; unsigned __int128 lowers to compiler-rt
// libcalls (__multi3, __udivti3, ...) that the LithoRoot sysroot does not provide.
#if (defined(_MSC_VER) && !defined(__clang__)) || defined(__wasm__)
struct ll_u128 {
  uint64_t lo = 0, hi = 0;
  constexpr ll_u128() = default;
  constexpr ll_u128(uint64_t v) : lo(v), hi(0) {}  // NOLINT(implicit)
  constexpr ll_u128(uint64_t h, uint64_t l) : lo(l), hi(h) {}
  template <class T, class = std::enable_if_t<std::is_integral_v<T>>>
  constexpr explicit operator T() const { return static_cast<T>(lo); }

  friend constexpr ll_u128 operator+(ll_u128 a, ll_u128 b) {
    const uint64_t l = a.lo + b.lo;
    return ll_u128(a.hi + b.hi + (l < a.lo ? 1u : 0u), l);
  }
  friend constexpr ll_u128 operator-(ll_u128 a, ll_u128 b) {
    const uint64_t l = a.lo - b.lo;
    return ll_u128(a.hi - b.hi - (a.lo < b.lo ? 1u : 0u), l);
  }
  static constexpr ll_u128 mul64(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xffffffffu, a1 = a >> 32;
    const uint64_t b0 = b & 0xffffffffu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    return ll_u128(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32),
                   (mid << 32) | (p00 & 0xffffffffu));
  }
  friend constexpr ll_u128 operator*(ll_u128 a, ll_u128 b) {
    ll_u128 r = mul64(a.lo, b.lo);
    r.hi += a.lo * b.hi + a.hi * b.lo;  // mod 2^128
    return r;
  }
  friend constexpr ll_u128 operator>>(ll_u128 a, int s) {
    if (s <= 0) return a;
    if (s >= 128) return ll_u128();
    if (s >= 64) return ll_u128(0, a.hi >> (s - 64));
    return ll_u128(a.hi >> s, (a.lo >> s) | (a.hi << (64 - s)));
  }
  friend constexpr ll_u128 operator<<(ll_u128 a, int s) {
    if (s <= 0) return a;
    if (s >= 128) return ll_u128();
    if (s >= 64) return ll_u128(a.lo << (s - 64), 0);
    return ll_u128((a.hi << s) | (a.lo >> (64 - s)), a.lo << s);
  }
  ll_u128& operator+=(ll_u128 b) { return *this = *this + b; }
};
#else
using ll_u128 = unsigned __int128;
#endif

}  // namespace litholattice::crypto