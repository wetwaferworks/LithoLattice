// lk_simd_wasm.h ... the SSE2/SSE4.1 intrinsics the software renderer's SIMD kernels use
// (software_simd_sse2.cpp, software_simd_sse41.cpp, software_simd_span4.inl), written in-house on
// clang's <wasm_simd128.h> for the LithoRoot wasm32 build (S1, 2026-11-16; JD 11:54 ET: no
// Emscripten, no library imports). Only these 45 intrinsics and _MM_TRANSPOSE4_PS exist here; a new
// one fails to compile until it is added with its exact SSE semantics.
//
// Exactness, intrinsic by intrinsic, is the point. Where wasm and SSE differ, the SSE result is
// emulated:
//   _mm_min_ps / _mm_max_ps   SSE returns the SECOND operand when either is NaN or both are zeros
//                             (a < b ? a : b); wasm f32x4.min propagates NaN. Uses pmin/pmax with
//                             swapped operands, which is exactly the SSE rule.
//   _mm_cvttps_epi32          SSE gives 0x80000000 for NaN and out-of-range; wasm trunc_sat
//                             saturates. Emulated with a range mask.
//   _mm_cvtps_epi32           round-to-nearest-even (the default MXCSR mode) via f32x4.nearest, then
//                             the same 0x80000000 rule.
//   _mm_sll/slli/srli_epi32   SSE gives 0 for counts > 31; wasm shifts use count mod 32.
//   _mm_blendv_ps             SSE selects on the mask's sign bit only.
//   _mm_andnot_ps             SSE is (~a) & b; wasm andnot is a & ~b, so the operands swap.
// Checked against real SSE on x86-64 by tests/lk_simd_wasm_check (box; same vectors, bit compare).
#pragma once
#if !defined(__wasm_simd128__)
#error "lk_simd_wasm.h is for the wasm32 build with -msimd128 only"
#endif
#include <stdint.h>
#include <wasm_simd128.h>

typedef float __m128 __attribute__((__vector_size__(16), __aligned__(16)));
typedef long long __m128i __attribute__((__vector_size__(16), __aligned__(16)));

#define LK_SIMD_INLINE static inline __attribute__((__always_inline__, __nodebug__))
#define LK_V(x) ((v128_t)(x))
#define LK_PS(x) ((__m128)(x))
#define LK_SI(x) ((__m128i)(x))

/* ---- set / load / store ---- */
LK_SIMD_INLINE __m128 _mm_setzero_ps(void) { return LK_PS(wasm_f32x4_splat(0.0f)); }
LK_SIMD_INLINE __m128i _mm_setzero_si128(void) { return LK_SI(wasm_i32x4_splat(0)); }
LK_SIMD_INLINE __m128 _mm_set1_ps(float v) { return LK_PS(wasm_f32x4_splat(v)); }
LK_SIMD_INLINE __m128i _mm_set1_epi32(int v) { return LK_SI(wasm_i32x4_splat(v)); }
LK_SIMD_INLINE __m128 _mm_set_ps(float e3, float e2, float e1, float e0) { return LK_PS(wasm_f32x4_make(e0, e1, e2, e3)); }
LK_SIMD_INLINE __m128i _mm_set_epi32(int e3, int e2, int e1, int e0) { return LK_SI(wasm_i32x4_make(e0, e1, e2, e3)); }
LK_SIMD_INLINE __m128i _mm_cvtsi32_si128(int a) { return LK_SI(wasm_i32x4_make(a, 0, 0, 0)); }
LK_SIMD_INLINE int _mm_cvtsi128_si32(__m128i a) { return wasm_i32x4_extract_lane(LK_V(a), 0); }
LK_SIMD_INLINE __m128 _mm_load_ps(const float* p) { return LK_PS(wasm_v128_load(p)); }
LK_SIMD_INLINE __m128i _mm_loadl_epi64(const __m128i* p) { return LK_SI(wasm_v128_load64_zero(p)); }
LK_SIMD_INLINE void _mm_store_ps(float* p, __m128 a) { wasm_v128_store(p, LK_V(a)); }
LK_SIMD_INLINE void _mm_storeu_ps(float* p, __m128 a) { wasm_v128_store(p, LK_V(a)); }
LK_SIMD_INLINE void _mm_store_si128(__m128i* p, __m128i a) { wasm_v128_store(p, LK_V(a)); }
LK_SIMD_INLINE void _mm_storeu_si128(__m128i* p, __m128i a) { wasm_v128_store(p, LK_V(a)); }
LK_SIMD_INLINE __m128i _mm_castps_si128(__m128 a) { return LK_SI(a); }
LK_SIMD_INLINE __m128 _mm_castsi128_ps(__m128i a) { return LK_PS(a); }

/* ---- float arithmetic (IEEE, identical to SSE for every non-NaN input) ---- */
LK_SIMD_INLINE __m128 _mm_add_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_add(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_sub_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_sub(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_mul_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_mul(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_div_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_div(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_sqrt_ps(__m128 a) { return LK_PS(wasm_f32x4_sqrt(LK_V(a))); }
/* SSE: min = (a < b) ? a : b, max = (a > b) ? a : b. pmin(x, y) = (y < x) ? y : x. */
LK_SIMD_INLINE __m128 _mm_min_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_pmin(LK_V(b), LK_V(a))); }
LK_SIMD_INLINE __m128 _mm_max_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_pmax(LK_V(b), LK_V(a))); }

/* ---- compares (ordered: false on NaN, like SSE's lt/le/gt/ge) and masks ---- */
LK_SIMD_INLINE __m128 _mm_cmplt_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_lt(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_cmpgt_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_gt(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_cmpge_ps(__m128 a, __m128 b) { return LK_PS(wasm_f32x4_ge(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE int _mm_movemask_ps(__m128 a) { return (int)wasm_i32x4_bitmask(LK_V(a)); }
LK_SIMD_INLINE __m128 _mm_and_ps(__m128 a, __m128 b) { return LK_PS(wasm_v128_and(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_or_ps(__m128 a, __m128 b) { return LK_PS(wasm_v128_or(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128 _mm_andnot_ps(__m128 a, __m128 b) { return LK_PS(wasm_v128_andnot(LK_V(b), LK_V(a))); }
LK_SIMD_INLINE __m128 _mm_blendv_ps(__m128 a, __m128 b, __m128 m) {
  return LK_PS(wasm_v128_bitselect(LK_V(b), LK_V(a), wasm_i32x4_shr(LK_V(m), 31)));
}

/* ---- conversions ---- */
LK_SIMD_INLINE __m128 _mm_cvtepi32_ps(__m128i a) { return LK_PS(wasm_f32x4_convert_i32x4(LK_V(a))); }
/* In range means -2^31 <= x < 2^31 (both bounds exact in float); NaN fails both compares. */
LK_SIMD_INLINE v128_t lk_simd__cvt_fix(v128_t x, v128_t r) {
  const v128_t ok = wasm_v128_and(wasm_f32x4_ge(x, wasm_f32x4_splat(-2147483648.0f)), wasm_f32x4_lt(x, wasm_f32x4_splat(2147483648.0f)));
  return wasm_v128_bitselect(r, wasm_i32x4_splat((int32_t)0x80000000u), ok);
}
LK_SIMD_INLINE __m128i _mm_cvttps_epi32(__m128 a) { return LK_SI(lk_simd__cvt_fix(LK_V(a), wasm_i32x4_trunc_sat_f32x4(LK_V(a)))); }
LK_SIMD_INLINE __m128i _mm_cvtps_epi32(__m128 a) {
  const v128_t n = wasm_f32x4_nearest(LK_V(a));  /* round half to even = MXCSR default */
  return LK_SI(lk_simd__cvt_fix(n, wasm_i32x4_trunc_sat_f32x4(n)));
}

/* ---- integer ---- */
LK_SIMD_INLINE __m128i _mm_add_epi32(__m128i a, __m128i b) { return LK_SI(wasm_i32x4_add(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_sub_epi32(__m128i a, __m128i b) { return LK_SI(wasm_i32x4_sub(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_and_si128(__m128i a, __m128i b) { return LK_SI(wasm_v128_and(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_or_si128(__m128i a, __m128i b) { return LK_SI(wasm_v128_or(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_cmpgt_epi32(__m128i a, __m128i b) { return LK_SI(wasm_i32x4_gt(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_slli_epi32(__m128i a, int n) {
  return (unsigned)n > 31u ? _mm_setzero_si128() : LK_SI(wasm_i32x4_shl(LK_V(a), (uint32_t)n));
}
LK_SIMD_INLINE __m128i _mm_srli_epi32(__m128i a, int n) {
  return (unsigned)n > 31u ? _mm_setzero_si128() : LK_SI(wasm_u32x4_shr(LK_V(a), (uint32_t)n));
}
/* The count is the low 64 bits of `count`, unsigned. */
LK_SIMD_INLINE __m128i _mm_sll_epi32(__m128i a, __m128i count) {
  const uint64_t c = (uint64_t)wasm_i64x2_extract_lane(LK_V(count), 0);
  return c > 31u ? _mm_setzero_si128() : LK_SI(wasm_i32x4_shl(LK_V(a), (uint32_t)c));
}
/* SSE4.1 packus: signed int32 -> unsigned int16 with saturation, a's lanes then b's. */
LK_SIMD_INLINE __m128i _mm_packus_epi32(__m128i a, __m128i b) { return LK_SI(wasm_u16x8_narrow_i32x4(LK_V(a), LK_V(b))); }
LK_SIMD_INLINE __m128i _mm_unpacklo_epi16(__m128i a, __m128i b) {
  return LK_SI(wasm_i16x8_shuffle(LK_V(a), LK_V(b), 0, 8, 1, 9, 2, 10, 3, 11));
}
#define _mm_shuffle_epi32(a, imm)                                                                \
  ((__m128i)wasm_i32x4_shuffle((v128_t)(a), (v128_t)(a), ((imm) >> 0) & 3, ((imm) >> 2) & 3, \
                               ((imm) >> 4) & 3, ((imm) >> 6) & 3))

/* xmmintrin.h's 4x4 transpose, rows in place. */
#define _MM_TRANSPOSE4_PS(r0, r1, r2, r3)                                             \
  do {                                                                                \
    const v128_t lk_t0 = wasm_i32x4_shuffle((v128_t)(r0), (v128_t)(r1), 0, 4, 1, 5); \
    const v128_t lk_t1 = wasm_i32x4_shuffle((v128_t)(r2), (v128_t)(r3), 0, 4, 1, 5); \
    const v128_t lk_t2 = wasm_i32x4_shuffle((v128_t)(r0), (v128_t)(r1), 2, 6, 3, 7); \
    const v128_t lk_t3 = wasm_i32x4_shuffle((v128_t)(r2), (v128_t)(r3), 2, 6, 3, 7); \
    (r0) = (__m128)wasm_i64x2_shuffle(lk_t0, lk_t1, 0, 2);                           \
    (r1) = (__m128)wasm_i64x2_shuffle(lk_t0, lk_t1, 1, 3);                           \
    (r2) = (__m128)wasm_i64x2_shuffle(lk_t2, lk_t3, 0, 2);                           \
    (r3) = (__m128)wasm_i64x2_shuffle(lk_t2, lk_t3, 1, 3);                           \
  } while (0)
