// Ginny (Argon) 2026-10-16 Pass 2 step 5 (Cortana): SSE2 kernels (ENGINE). SSE2 is part of
// x86-64 itself, so this TU needs no flags and runs on every 64-bit x86 CPU (2003+).
// Reached through software_simd.cpp's runtime dispatch only.
#include <cstddef>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__) || \
    defined(__wasm_simd128__)
#include <emmintrin.h>
#include "litholattice/render/software_simd.h"
namespace litholattice::swsimd {
void fill32Sse2(uint32_t* p, size_t n, uint32_t v) {
  const __m128i vv = _mm_set1_epi32(int(v));
  size_t i = 0;
  for (; i + 4 <= n; i += 4) _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i), vv);
  for (; i < n; ++i) p[i] = v;
}
void fillFSse2(float* p, size_t n, float v) {
  const __m128 vv = _mm_set1_ps(v);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) _mm_storeu_ps(p + i, vv);
  for (; i < n; ++i) p[i] = v;
}
// SSE2 has no blendv: (m & b) | (~m & a)
static inline __m128 sse2Blend(__m128 a, __m128 b, __m128 m) { return _mm_or_ps(_mm_and_ps(m, b), _mm_andnot_ps(m, a)); }
#define LL_SPAN4_FN shadeSpanBasicSse2
#define LL_QUAD4_FN transformQuadSse2
#define LL_SPAN4_TARGET
#define LL_SPAN4_LAMBDA_TARGET
#define LL_SPAN4_BLEND_PS(a, b, m) sse2Blend((a), (b), (m))
#include "software_simd_span4.inl"
}  // namespace litholattice::swsimd
#endif
